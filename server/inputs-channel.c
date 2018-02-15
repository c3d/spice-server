/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <spice/macros.h>
#include <spice/protocol.h>
#include <spice/vd_agent.h>
#include <stddef.h> // NULL

#include <common/generated_server_marshallers.h>
#include <common/marshaller.h>
#include <common/messages.h>

#include "spice.h"
#include "demarshallers.h"
#include "inputs-channel-client.h"
#include "inputs-channel.h"
#include "main-channel-client.h"
#include "migration-protocol.h"
#include "red-channel-client.h"
#include "red-channel.h"
#include "red-client.h"
#include "red-common.h"
#include "red-stream.h"
#include "reds.h"
#include "utils.h"

struct InputsChannel
{
    RedChannel parent;

    VDAgentMouseState mouse_state;
    int               src_during_migrate;
    SpiceTimer *      key_modifiers_timer;

    SpiceKbdInstance *   keyboard;
    SpiceMouseInstance * mouse;
    SpiceTabletInstance *tablet;
};

struct InputsChannelClass
{
    RedChannelClass parent_class;
};

G_DEFINE_TYPE(InputsChannel, inputs_channel, RED_TYPE_CHANNEL)

struct SpiceKbdState
{
    uint8_t push_ext_type;

    /* track key press state */
    bool           key[0x80];
    bool           key_ext[0x80];
    InputsChannel *inputs;
};

static SpiceKbdInstance *inputs_channel_get_keyboard(InputsChannel *inputs);
static SpiceMouseInstance *inputs_channel_get_mouse(InputsChannel *inputs);
static SpiceTabletInstance *inputs_channel_get_tablet(InputsChannel *inputs);

static SpiceKbdState *spice_kbd_state_new(InputsChannel *inputs)
{
    SpiceKbdState *st = g_new0(SpiceKbdState, 1);
    st->inputs        = inputs;
    return st;
}

struct SpiceMouseState
{
    int dummy;
};

static SpiceMouseState *spice_mouse_state_new(void) { return g_new0(SpiceMouseState, 1); }

struct SpiceTabletState
{
    RedsState *reds;
};

static SpiceTabletState *spice_tablet_state_new(RedsState *reds)
{
    SpiceTabletState *st = g_new0(SpiceTabletState, 1);
    st->reds             = reds;
    return st;
}

static void spice_tablet_state_free(SpiceTabletState *st) { g_free(st); }

RedsState *spice_tablet_state_get_server(SpiceTabletState *st) { return st->reds; }

typedef struct RedKeyModifiersPipeItem
{
    RedPipeItem base;
    uint8_t     modifiers;
} RedKeyModifiersPipeItem;

typedef struct RedInputsInitPipeItem
{
    RedPipeItem base;
    uint8_t     modifiers;
} RedInputsInitPipeItem;

#define KEY_MODIFIERS_TTL (MSEC_PER_SEC * 2)

#define SCAN_CODE_RELEASE 0x80
#define SCROLL_LOCK_SCAN_CODE 0x46
#define NUM_LOCK_SCAN_CODE 0x45
#define CAPS_LOCK_SCAN_CODE 0x3a

void inputs_channel_set_tablet_logical_size(InputsChannel *inputs, int x_res, int y_res)
{
    SpiceTabletInterface *sif;

    sif = SPICE_UPCAST(SpiceTabletInterface, inputs->tablet->base.sif);
    sif->set_logical_size(inputs->tablet, x_res, y_res);
}

const VDAgentMouseState *inputs_channel_get_mouse_state(InputsChannel *inputs)
{
    return &inputs->mouse_state;
}

#define OUTGOING_OK 0
#define OUTGOING_FAILED -1
#define OUTGOING_BLOCKED 1

#define RED_MOUSE_STATE_TO_LOCAL(state)                                                            \
    ((state & SPICE_MOUSE_BUTTON_MASK_LEFT) | ((state & SPICE_MOUSE_BUTTON_MASK_MIDDLE) << 1) |    \
     ((state & SPICE_MOUSE_BUTTON_MASK_RIGHT) >> 1))

#define RED_MOUSE_BUTTON_STATE_TO_AGENT(state)                                                     \
    (((state & SPICE_MOUSE_BUTTON_MASK_LEFT) ? VD_AGENT_LBUTTON_MASK : 0) |                        \
     ((state & SPICE_MOUSE_BUTTON_MASK_MIDDLE) ? VD_AGENT_MBUTTON_MASK : 0) |                      \
     ((state & SPICE_MOUSE_BUTTON_MASK_RIGHT) ? VD_AGENT_RBUTTON_MASK : 0))

static void activate_modifiers_watch(InputsChannel *inputs)
{
    SpiceCoreInterfaceInternal *core = red_channel_get_core_interface(RED_CHANNEL(inputs));
    core->timer_start(core, inputs->key_modifiers_timer, KEY_MODIFIERS_TTL);
}

static void kbd_push_scan(SpiceKbdInstance *sin, uint8_t scan)
{
    SpiceKbdInterface *sif;

    if (!sin) {
        return;
    }
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceKbdInterface, base);

    /* track XT scan code set 1 key state */
    if (scan >= 0xe0 && scan <= 0xe2) {
        sin->st->push_ext_type = scan;
    } else {
        if (sin->st->push_ext_type == 0 || sin->st->push_ext_type == 0xe0) {
            bool *state        = sin->st->push_ext_type ? sin->st->key_ext : sin->st->key;
            state[scan & 0x7f] = !(scan & SCAN_CODE_RELEASE);
        }
        sin->st->push_ext_type = 0;
    }

    sif->push_scan_freg(sin, scan);
}

static uint8_t kbd_get_leds(SpiceKbdInstance *sin)
{
    SpiceKbdInterface *sif;

    if (!sin) {
        return 0;
    }
    sif = SPICE_CONTAINEROF(sin->base.sif, SpiceKbdInterface, base);
    return sif->get_leds(sin);
}

static RedPipeItem *red_inputs_key_modifiers_item_new(uint8_t modifiers)
{
    RedKeyModifiersPipeItem *item = g_new(RedKeyModifiersPipeItem, 1);

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_KEY_MODIFIERS);
    item->modifiers = modifiers;
    return &item->base;
}

static void inputs_channel_send_item(RedChannelClient *rcc, RedPipeItem *base)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);

    switch (base->type) {
    case RED_PIPE_ITEM_KEY_MODIFIERS: {
        SpiceMsgInputsKeyModifiers key_modifiers;

        red_channel_client_init_send_data(rcc, SPICE_MSG_INPUTS_KEY_MODIFIERS);
        key_modifiers.modifiers = SPICE_UPCAST(RedKeyModifiersPipeItem, base)->modifiers;
        spice_marshall_msg_inputs_key_modifiers(m, &key_modifiers);
        break;
    }
    case RED_PIPE_ITEM_INPUTS_INIT: {
        SpiceMsgInputsInit inputs_init;

        red_channel_client_init_send_data(rcc, SPICE_MSG_INPUTS_INIT);
        inputs_init.keyboard_modifiers = SPICE_UPCAST(RedInputsInitPipeItem, base)->modifiers;
        spice_marshall_msg_inputs_init(m, &inputs_init);
        break;
    }
    case RED_PIPE_ITEM_MOUSE_MOTION_ACK:
        red_channel_client_init_send_data(rcc, SPICE_MSG_INPUTS_MOUSE_MOTION_ACK);
        break;
    case RED_PIPE_ITEM_MIGRATE_DATA:
        INPUTS_CHANNEL(red_channel_client_get_channel(rcc))->src_during_migrate = FALSE;
        inputs_channel_client_send_migrate_data(rcc, m, base);
        break;
    default:
        spice_warning("invalid pipe iten %d", base->type);
        break;
    }
    red_channel_client_begin_send_message(rcc);
}

static bool
inputs_channel_handle_message(RedChannelClient *rcc, uint16_t type, uint32_t size, void *message)
{
    InputsChannel *      inputs_channel = INPUTS_CHANNEL(red_channel_client_get_channel(rcc));
    InputsChannelClient *icc            = INPUTS_CHANNEL_CLIENT(rcc);
    uint32_t             i;
    RedsState *          reds = red_channel_get_server(RED_CHANNEL(inputs_channel));

    switch (type) {
    case SPICE_MSGC_INPUTS_KEY_DOWN: {
        SpiceMsgcKeyDown *key_down = message;
        if (key_down->code == CAPS_LOCK_SCAN_CODE || key_down->code == NUM_LOCK_SCAN_CODE ||
            key_down->code == SCROLL_LOCK_SCAN_CODE) {
            activate_modifiers_watch(inputs_channel);
        }
    }
    /* fallthrough */
    case SPICE_MSGC_INPUTS_KEY_UP: {
        SpiceMsgcKeyUp *key_up = message;
        for (i = 0; i < 4; i++) {
            uint8_t code = (key_up->code >> (i * 8)) & 0xff;
            if (code == 0) {
                break;
            }
            kbd_push_scan(inputs_channel_get_keyboard(inputs_channel), code);
        }
        break;
    }
    case SPICE_MSGC_INPUTS_KEY_SCANCODE: {
        uint8_t *code = message;
        for (i = 0; i < size; i++) {
            kbd_push_scan(inputs_channel_get_keyboard(inputs_channel), code[i]);
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_MOTION: {
        SpiceMouseInstance *  mouse        = inputs_channel_get_mouse(inputs_channel);
        SpiceMsgcMouseMotion *mouse_motion = message;

        inputs_channel_client_on_mouse_motion(icc);
        if (mouse && reds_get_mouse_mode(reds) == SPICE_MOUSE_MODE_SERVER) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(mouse->base.sif, SpiceMouseInterface, base);
            sif->motion(mouse, mouse_motion->dx, mouse_motion->dy, 0,
                        RED_MOUSE_STATE_TO_LOCAL(mouse_motion->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_POSITION: {
        SpiceMsgcMousePosition *pos    = message;
        SpiceTabletInstance *   tablet = inputs_channel_get_tablet(inputs_channel);

        inputs_channel_client_on_mouse_motion(icc);
        if (reds_get_mouse_mode(reds) != SPICE_MOUSE_MODE_CLIENT) {
            break;
        }
        spice_assert((reds_config_get_agent_mouse(reds) && reds_has_vdagent(reds)) || tablet);
        if (!reds_config_get_agent_mouse(reds) || !reds_has_vdagent(reds)) {
            SpiceTabletInterface *sif;
            sif = SPICE_CONTAINEROF(tablet->base.sif, SpiceTabletInterface, base);
            sif->position(tablet, pos->x, pos->y, RED_MOUSE_STATE_TO_LOCAL(pos->buttons_state));
            break;
        }
        VDAgentMouseState *mouse_state = &inputs_channel->mouse_state;
        mouse_state->x                 = pos->x;
        mouse_state->y                 = pos->y;
        mouse_state->buttons           = RED_MOUSE_BUTTON_STATE_TO_AGENT(pos->buttons_state);
        mouse_state->display_id        = pos->display_id;
        reds_handle_agent_mouse_event(reds, mouse_state);
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_PRESS: {
        SpiceMsgcMousePress *mouse_press = message;
        int                  dz          = 0;
        if (mouse_press->button == SPICE_MOUSE_BUTTON_UP) {
            dz = -1;
        } else if (mouse_press->button == SPICE_MOUSE_BUTTON_DOWN) {
            dz = 1;
        }
        if (reds_get_mouse_mode(reds) == SPICE_MOUSE_MODE_CLIENT) {
            if (reds_config_get_agent_mouse(reds) && reds_has_vdagent(reds)) {
                inputs_channel->mouse_state.buttons =
                    RED_MOUSE_BUTTON_STATE_TO_AGENT(mouse_press->buttons_state) |
                    (dz == -1 ? VD_AGENT_UBUTTON_MASK : 0) | (dz == 1 ? VD_AGENT_DBUTTON_MASK : 0);
                reds_handle_agent_mouse_event(reds, &inputs_channel->mouse_state);
            } else if (inputs_channel_get_tablet(inputs_channel)) {
                SpiceTabletInterface *sif;
                sif = SPICE_CONTAINEROF(inputs_channel_get_tablet(inputs_channel)->base.sif,
                                        SpiceTabletInterface, base);
                sif->wheel(inputs_channel_get_tablet(inputs_channel), dz,
                           RED_MOUSE_STATE_TO_LOCAL(mouse_press->buttons_state));
            }
        } else if (inputs_channel_get_mouse(inputs_channel)) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(inputs_channel_get_mouse(inputs_channel)->base.sif,
                                    SpiceMouseInterface, base);
            sif->motion(inputs_channel_get_mouse(inputs_channel), 0, 0, dz,
                        RED_MOUSE_STATE_TO_LOCAL(mouse_press->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_MOUSE_RELEASE: {
        SpiceMsgcMouseRelease *mouse_release = message;
        if (reds_get_mouse_mode(reds) == SPICE_MOUSE_MODE_CLIENT) {
            if (reds_config_get_agent_mouse(reds) && reds_has_vdagent(reds)) {
                inputs_channel->mouse_state.buttons =
                    RED_MOUSE_BUTTON_STATE_TO_AGENT(mouse_release->buttons_state);
                reds_handle_agent_mouse_event(reds, &inputs_channel->mouse_state);
            } else if (inputs_channel_get_tablet(inputs_channel)) {
                SpiceTabletInterface *sif;
                sif = SPICE_CONTAINEROF(inputs_channel_get_tablet(inputs_channel)->base.sif,
                                        SpiceTabletInterface, base);
                sif->buttons(inputs_channel_get_tablet(inputs_channel),
                             RED_MOUSE_STATE_TO_LOCAL(mouse_release->buttons_state));
            }
        } else if (inputs_channel_get_mouse(inputs_channel)) {
            SpiceMouseInterface *sif;
            sif = SPICE_CONTAINEROF(inputs_channel_get_mouse(inputs_channel)->base.sif,
                                    SpiceMouseInterface, base);
            sif->buttons(inputs_channel_get_mouse(inputs_channel),
                         RED_MOUSE_STATE_TO_LOCAL(mouse_release->buttons_state));
        }
        break;
    }
    case SPICE_MSGC_INPUTS_KEY_MODIFIERS: {
        SpiceMsgcKeyModifiers *modifiers = message;
        uint8_t                leds;
        SpiceKbdInstance *     keyboard = inputs_channel_get_keyboard(inputs_channel);

        if (!keyboard) {
            break;
        }
        leds = kbd_get_leds(keyboard);
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_SCROLL_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_SCROLL_LOCK)) {
            kbd_push_scan(keyboard, SCROLL_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, SCROLL_LOCK_SCAN_CODE | SCAN_CODE_RELEASE);
        }
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_NUM_LOCK)) {
            kbd_push_scan(keyboard, NUM_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, NUM_LOCK_SCAN_CODE | SCAN_CODE_RELEASE);
        }
        if ((modifiers->modifiers & SPICE_KEYBOARD_MODIFIER_FLAGS_CAPS_LOCK) !=
            (leds & SPICE_KEYBOARD_MODIFIER_FLAGS_CAPS_LOCK)) {
            kbd_push_scan(keyboard, CAPS_LOCK_SCAN_CODE);
            kbd_push_scan(keyboard, CAPS_LOCK_SCAN_CODE | SCAN_CODE_RELEASE);
        }
        activate_modifiers_watch(inputs_channel);
        break;
    }
    default:
        return red_channel_client_handle_message(rcc, type, size, message);
    }
    return TRUE;
}

void inputs_release_keys(InputsChannel *inputs)
{
    int               i;
    SpiceKbdState *   st;
    SpiceKbdInstance *keyboard = inputs_channel_get_keyboard(inputs);

    if (!keyboard) {
        return;
    }
    st = keyboard->st;

    for (i = 0; i < SPICE_N_ELEMENTS(st->key); i++) {
        if (!st->key[i])
            continue;

        st->key[i] = FALSE;
        kbd_push_scan(keyboard, i | SCAN_CODE_RELEASE);
    }

    for (i = 0; i < SPICE_N_ELEMENTS(st->key_ext); i++) {
        if (!st->key_ext[i])
            continue;

        st->key_ext[i] = FALSE;
        kbd_push_scan(keyboard, 0xe0);
        kbd_push_scan(keyboard, i | SCAN_CODE_RELEASE);
    }
}

static void inputs_pipe_add_init(RedChannelClient *rcc)
{
    RedInputsInitPipeItem *item   = g_new(RedInputsInitPipeItem, 1);
    InputsChannel *        inputs = INPUTS_CHANNEL(red_channel_client_get_channel(rcc));

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_INPUTS_INIT);
    item->modifiers = kbd_get_leds(inputs_channel_get_keyboard(inputs));
    red_channel_client_pipe_add_push(rcc, &item->base);
}

static void inputs_connect(RedChannel *            channel,
                           RedClient *             client,
                           RedStream *             stream,
                           int                     migration,
                           RedChannelCapabilities *caps)
{
    RedChannelClient *rcc;

    if (!red_stream_is_ssl(stream) && !red_client_during_migrate_at_target(client)) {
        main_channel_client_push_notify(red_client_get_main(client),
                                        "keyboard channel is insecure");
    }

    spice_printerr("inputs channel client create");
    rcc = inputs_channel_client_create(channel, client, stream, caps);
    if (!rcc) {
        return;
    }
    inputs_pipe_add_init(rcc);
}

static void inputs_migrate(RedChannelClient *rcc)
{
    InputsChannel *inputs      = INPUTS_CHANNEL(red_channel_client_get_channel(rcc));
    inputs->src_during_migrate = TRUE;
    red_channel_client_default_migrate(rcc);
}

static void inputs_channel_push_keyboard_modifiers(InputsChannel *inputs, uint8_t modifiers)
{
    if (!inputs || !red_channel_is_connected(RED_CHANNEL(inputs)) || inputs->src_during_migrate) {
        return;
    }
    red_channel_pipes_add(RED_CHANNEL(inputs), red_inputs_key_modifiers_item_new(modifiers));
}

SPICE_GNUC_VISIBLE int spice_server_kbd_leds(SpiceKbdInstance *sin, int leds)
{
    inputs_channel_push_keyboard_modifiers(sin->st->inputs, leds);
    return 0;
}

static void key_modifiers_sender(void *opaque)
{
    InputsChannel *inputs = opaque;
    inputs_channel_push_keyboard_modifiers(inputs,
                                           kbd_get_leds(inputs_channel_get_keyboard(inputs)));
}

static bool inputs_channel_handle_migrate_flush_mark(RedChannelClient *rcc)
{
    red_channel_client_pipe_add_type(rcc, RED_PIPE_ITEM_MIGRATE_DATA);
    return TRUE;
}

static bool inputs_channel_handle_migrate_data(RedChannelClient *rcc, uint32_t size, void *message)
{
    InputsChannelClient *   icc    = INPUTS_CHANNEL_CLIENT(rcc);
    InputsChannel *         inputs = INPUTS_CHANNEL(red_channel_client_get_channel(rcc));
    SpiceMigrateDataHeader *header;
    SpiceMigrateDataInputs *mig_data;

    if (size < sizeof(SpiceMigrateDataHeader) + sizeof(SpiceMigrateDataInputs)) {
        spice_warning("bad message size %u", size);
        return FALSE;
    }

    header   = (SpiceMigrateDataHeader *)message;
    mig_data = (SpiceMigrateDataInputs *)(header + 1);

    if (!migration_protocol_validate_header(header, SPICE_MIGRATE_DATA_INPUTS_MAGIC,
                                            SPICE_MIGRATE_DATA_INPUTS_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }
    key_modifiers_sender(inputs);
    inputs_channel_client_handle_migrate_data(icc, mig_data->motion_count);
    return TRUE;
}

InputsChannel *inputs_channel_new(RedsState *reds)
{
    return g_object_new(TYPE_INPUTS_CHANNEL, "spice-server", reds, "core-interface",
                        reds_get_core_interface(reds), "channel-type", (int)SPICE_CHANNEL_INPUTS,
                        "id", 0, "handle-acks", FALSE, "migration-flags",
                        (guint)(SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER), NULL);
}

static void inputs_channel_constructed(GObject *object)
{
    ClientCbs client_cbs = {
        NULL,
    };
    InputsChannel *             self = INPUTS_CHANNEL(object);
    RedsState *                 reds = red_channel_get_server(RED_CHANNEL(self));
    SpiceCoreInterfaceInternal *core = red_channel_get_core_interface(RED_CHANNEL(self));

    G_OBJECT_CLASS(inputs_channel_parent_class)->constructed(object);

    client_cbs.connect = inputs_connect;
    client_cbs.migrate = inputs_migrate;
    red_channel_register_client_cbs(RED_CHANNEL(self), &client_cbs, NULL);

    red_channel_set_cap(RED_CHANNEL(self), SPICE_INPUTS_CAP_KEY_SCANCODE);
    reds_register_channel(reds, RED_CHANNEL(self));

    self->key_modifiers_timer = core->timer_add(core, key_modifiers_sender, self);
    if (!self->key_modifiers_timer) {
        spice_error("key modifiers timer create failed");
    }
}

static void inputs_channel_finalize(GObject *object)
{
    InputsChannel *             self = INPUTS_CHANNEL(object);
    SpiceCoreInterfaceInternal *core = red_channel_get_core_interface(RED_CHANNEL(self));

    inputs_channel_detach_tablet(self, self->tablet);
    core->timer_remove(core, self->key_modifiers_timer);

    G_OBJECT_CLASS(inputs_channel_parent_class)->finalize(object);
}

static void inputs_channel_init(InputsChannel *self) {}

static void inputs_channel_class_init(InputsChannelClass *klass)
{
    GObjectClass *   object_class  = G_OBJECT_CLASS(klass);
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    object_class->constructed = inputs_channel_constructed;
    object_class->finalize    = inputs_channel_finalize;

    channel_class->parser         = spice_get_client_channel_parser(SPICE_CHANNEL_INPUTS, NULL);
    channel_class->handle_message = inputs_channel_handle_message;

    /* channel callbacks */
    channel_class->send_item                 = inputs_channel_send_item;
    channel_class->handle_migrate_data       = inputs_channel_handle_migrate_data;
    channel_class->handle_migrate_flush_mark = inputs_channel_handle_migrate_flush_mark;
}

static SpiceKbdInstance *inputs_channel_get_keyboard(InputsChannel *inputs)
{
    return inputs->keyboard;
}

int inputs_channel_set_keyboard(InputsChannel *inputs, SpiceKbdInstance *keyboard)
{
    if (inputs->keyboard) {
        spice_printerr("already have keyboard");
        return -1;
    }
    inputs->keyboard     = keyboard;
    inputs->keyboard->st = spice_kbd_state_new(inputs);
    return 0;
}

static SpiceMouseInstance *inputs_channel_get_mouse(InputsChannel *inputs) { return inputs->mouse; }

int inputs_channel_set_mouse(InputsChannel *inputs, SpiceMouseInstance *mouse)
{
    if (inputs->mouse) {
        spice_printerr("already have mouse");
        return -1;
    }
    inputs->mouse     = mouse;
    inputs->mouse->st = spice_mouse_state_new();
    return 0;
}

static SpiceTabletInstance *inputs_channel_get_tablet(InputsChannel *inputs)
{
    return inputs->tablet;
}

int inputs_channel_set_tablet(InputsChannel *inputs, SpiceTabletInstance *tablet)
{
    if (inputs->tablet) {
        spice_printerr("already have tablet");
        return -1;
    }
    inputs->tablet     = tablet;
    inputs->tablet->st = spice_tablet_state_new(red_channel_get_server(RED_CHANNEL(inputs)));
    return 0;
}

int inputs_channel_has_tablet(InputsChannel *inputs)
{
    return inputs != NULL && inputs->tablet != NULL;
}

void inputs_channel_detach_tablet(InputsChannel *inputs, SpiceTabletInstance *tablet)
{
    spice_printerr("");
    if (tablet != NULL && tablet == inputs->tablet) {
        spice_tablet_state_free(tablet->st);
        tablet->st = NULL;
    }
    inputs->tablet = NULL;
}

gboolean inputs_channel_is_src_during_migrate(InputsChannel *inputs)
{
    return inputs->src_during_migrate;
}
