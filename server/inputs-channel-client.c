/*
   Copyright (C) 2009-2015 Red Hat, Inc.

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

#include "inputs-channel-client.h"
#include "migration-protocol.h"
#include "red-channel-client.h"

G_DEFINE_TYPE(InputsChannelClient, inputs_channel_client, RED_TYPE_CHANNEL_CLIENT)

#define INPUTS_CHANNEL_CLIENT_PRIVATE(o)                                                           \
    (G_TYPE_INSTANCE_GET_PRIVATE((o), TYPE_INPUTS_CHANNEL_CLIENT, InputsChannelClientPrivate))

// TODO: RECEIVE_BUF_SIZE used to be the same for inputs_channel and
// main_channel
// since it was defined once in reds.c which contained both.
// Now that they are split we can give a more fitting value for inputs - what
// should it be?
#define REDS_AGENT_WINDOW_SIZE 10
#define REDS_NUM_INTERNAL_AGENT_MESSAGES 1

// approximate max receive message size
#define RECEIVE_BUF_SIZE                                                                           \
    (4096 + (REDS_AGENT_WINDOW_SIZE + REDS_NUM_INTERNAL_AGENT_MESSAGES) * SPICE_AGENT_MAX_DATA_SIZE)

struct InputsChannelClientPrivate
{
    uint16_t motion_count;
    uint8_t  recv_buf[RECEIVE_BUF_SIZE];
};

static uint8_t *
inputs_channel_client_alloc_msg_rcv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size)
{
    if (size > RECEIVE_BUF_SIZE) {
        spice_printerr("error: too large incoming message");
        return NULL;
    }

    InputsChannelClient *icc = INPUTS_CHANNEL_CLIENT(rcc);
    return icc->priv->recv_buf;
}

static void inputs_channel_client_release_msg_rcv_buf(RedChannelClient *rcc,
                                                      uint16_t          type,
                                                      uint32_t          size,
                                                      uint8_t *         msg)
{
}

static void inputs_channel_client_on_disconnect(RedChannelClient *rcc)
{
    if (!rcc) {
        return;
    }
    inputs_release_keys(INPUTS_CHANNEL(red_channel_client_get_channel(rcc)));
}

static void inputs_channel_client_class_init(InputsChannelClientClass *klass)
{
    RedChannelClientClass *client_class = RED_CHANNEL_CLIENT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(InputsChannelClientPrivate));

    client_class->alloc_recv_buf   = inputs_channel_client_alloc_msg_rcv_buf;
    client_class->release_recv_buf = inputs_channel_client_release_msg_rcv_buf;
    client_class->on_disconnect    = inputs_channel_client_on_disconnect;
}

static void inputs_channel_client_init(InputsChannelClient *self)
{
    self->priv = INPUTS_CHANNEL_CLIENT_PRIVATE(self);
}

RedChannelClient *inputs_channel_client_create(RedChannel *            channel,
                                               RedClient *             client,
                                               RedStream *             stream,
                                               RedChannelCapabilities *caps)
{
    RedChannelClient *rcc;

    rcc = g_initable_new(TYPE_INPUTS_CHANNEL_CLIENT, NULL, NULL, "channel", channel, "client",
                         client, "stream", stream, "caps", caps, NULL);

    return rcc;
}

void inputs_channel_client_send_migrate_data(RedChannelClient *rcc,
                                             SpiceMarshaller * m,
                                             RedPipeItem *     item)
{
    InputsChannelClient *icc = INPUTS_CHANNEL_CLIENT(rcc);

    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE_DATA);

    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_INPUTS_MAGIC);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_INPUTS_VERSION);
    spice_marshaller_add_uint16(m, icc->priv->motion_count);
}

void inputs_channel_client_handle_migrate_data(InputsChannelClient *icc, uint16_t motion_count)
{
    icc->priv->motion_count = motion_count;

    for (; icc->priv->motion_count >= SPICE_INPUT_MOTION_ACK_BUNCH;
         icc->priv->motion_count -= SPICE_INPUT_MOTION_ACK_BUNCH) {
        red_channel_client_pipe_add_type(RED_CHANNEL_CLIENT(icc), RED_PIPE_ITEM_MOUSE_MOTION_ACK);
    }
}

void inputs_channel_client_on_mouse_motion(InputsChannelClient *icc)
{
    InputsChannel *inputs_channel =
        INPUTS_CHANNEL(red_channel_client_get_channel(RED_CHANNEL_CLIENT(icc)));

    if (++icc->priv->motion_count % SPICE_INPUT_MOTION_ACK_BUNCH == 0 &&
        !inputs_channel_is_src_during_migrate(inputs_channel)) {
        red_channel_client_pipe_add_type(RED_CHANNEL_CLIENT(icc), RED_PIPE_ITEM_MOUSE_MOTION_ACK);
        icc->priv->motion_count = 0;
    }
}
