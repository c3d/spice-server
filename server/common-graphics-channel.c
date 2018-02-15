/*
   Copyright (C) 2009-2016 Red Hat, Inc.

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

#include <fcntl.h>

#include "common-graphics-channel.h"
#include "dcc.h"
#include "red-client.h"

#define CHANNEL_RECEIVE_BUF_SIZE 1024

G_DEFINE_ABSTRACT_TYPE(CommonGraphicsChannel, common_graphics_channel, RED_TYPE_CHANNEL)

G_DEFINE_TYPE(CommonGraphicsChannelClient, common_graphics_channel_client, RED_TYPE_CHANNEL_CLIENT)

#define GRAPHICS_CHANNEL_PRIVATE(o)                                                                \
    (G_TYPE_INSTANCE_GET_PRIVATE((o), TYPE_COMMON_GRAPHICS_CHANNEL, CommonGraphicsChannelPrivate))
#define GRAPHICS_CHANNEL_CLIENT_PRIVATE(o)                                                         \
    (G_TYPE_INSTANCE_GET_PRIVATE((o), TYPE_COMMON_GRAPHICS_CHANNEL_CLIENT,                         \
                                 CommonGraphicsChannelClientPrivate))

struct CommonGraphicsChannelPrivate
{
    int during_target_migrate; /* TRUE when the client that is associated with the
                                  channel
                                  is during migration. Turned off when the vm is
                                  started.
                                  The flag is used to avoid sending messages that
                                  are artifacts
                                  of the transition from stopped vm to loaded vm
                                  (e.g., recreation
                                  of the primary surface) */
};

struct CommonGraphicsChannelClientPrivate
{
    uint8_t recv_buf[CHANNEL_RECEIVE_BUF_SIZE];
};

static uint8_t *common_alloc_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size)
{
    CommonGraphicsChannelClient *common = COMMON_GRAPHICS_CHANNEL_CLIENT(rcc);

    /* SPICE_MSGC_MIGRATE_DATA is the only client message whose size is dynamic */
    if (type == SPICE_MSGC_MIGRATE_DATA) {
        return g_malloc(size);
    }

    if (size > CHANNEL_RECEIVE_BUF_SIZE) {
        spice_critical("unexpected message size %u (max is %d)", size, CHANNEL_RECEIVE_BUF_SIZE);
        return NULL;
    }
    return common->priv->recv_buf;
}

static void
common_release_recv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size, uint8_t *msg)
{
    if (type == SPICE_MSGC_MIGRATE_DATA) {
        g_free(msg);
    }
}

bool common_channel_client_config_socket(RedChannelClient *rcc)
{
    RedClient *        client = red_channel_client_get_client(rcc);
    MainChannelClient *mcc    = red_client_get_main(client);
    RedStream *        stream = red_channel_client_get_stream(rcc);
    gboolean           is_low_bandwidth;

    // TODO - this should be dynamic, not one time at channel creation
    is_low_bandwidth = main_channel_client_is_low_bandwidth(mcc);
    /* FIXME: Using Nagle's Algorithm can lead to apparent delays, depending
     * on the delayed ack timeout on the other side.
     * Instead of using Nagle's, we need to implement message buffering on
     * the application level.
     * see: http://www.stuartcheshire.org/papers/NagleDelayedAck/
     */
    red_stream_set_no_delay(stream, !is_low_bandwidth);

    // TODO: move wide/narrow ack setting to red_channel.
    red_channel_client_ack_set_client_window(rcc, is_low_bandwidth ? WIDE_CLIENT_ACK_WINDOW
                                                                   : NARROW_CLIENT_ACK_WINDOW);
    return TRUE;
}

static void common_graphics_channel_class_init(CommonGraphicsChannelClass *klass)
{
    g_type_class_add_private(klass, sizeof(CommonGraphicsChannelPrivate));
}

static void common_graphics_channel_init(CommonGraphicsChannel *self)
{
    self->priv = GRAPHICS_CHANNEL_PRIVATE(self);
}

void common_graphics_channel_set_during_target_migrate(CommonGraphicsChannel *self, gboolean value)
{
    self->priv->during_target_migrate = value;
}

gboolean common_graphics_channel_get_during_target_migrate(CommonGraphicsChannel *self)
{
    return self->priv->during_target_migrate;
}

static void common_graphics_channel_client_init(CommonGraphicsChannelClient *self)
{
    self->priv = GRAPHICS_CHANNEL_CLIENT_PRIVATE(self);
}

static void common_graphics_channel_client_class_init(CommonGraphicsChannelClientClass *klass)
{
    RedChannelClientClass *client_class = RED_CHANNEL_CLIENT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(CommonGraphicsChannelClientPrivate));

    client_class->config_socket    = common_channel_client_config_socket;
    client_class->alloc_recv_buf   = common_alloc_recv_buf;
    client_class->release_recv_buf = common_release_recv_buf;
}
