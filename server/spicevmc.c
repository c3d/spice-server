/* spice-server spicevmc passthrough channel code

   Copyright (C) 2011 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

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

#include <assert.h>
#include <string.h>
#ifdef USE_LZ4
#include <lz4.h>
#endif

#include <common/generated_server_marshallers.h>

#include "char-device.h"
#include "migration-protocol.h"
#include "red-channel-client.h"
#include "red-channel.h"
#include "reds.h"

/* todo: add flow control. i.e.,
 * (a) limit the tokens available for the client
 * (b) limit the tokens available for the server
 */
/* 64K should be enough for all but the largest writes + 32 bytes hdr */
#define BUF_SIZE (64 * 1024 + 32)
#define COMPRESS_THRESHOLD 1000

SPICE_DECLARE_TYPE(RedCharDeviceSpiceVmc, red_char_device_spicevmc, CHAR_DEVICE_SPICEVMC);
#define RED_TYPE_CHAR_DEVICE_SPICEVMC red_char_device_spicevmc_get_type()

SPICE_DECLARE_TYPE(RedVmcChannel, red_vmc_channel, VMC_CHANNEL);
#define RED_TYPE_VMC_CHANNEL red_vmc_channel_get_type()

SPICE_DECLARE_TYPE(RedVmcChannelPort, red_vmc_channel_port, VMC_CHANNEL_PORT);
#define RED_TYPE_VMC_CHANNEL_PORT red_vmc_channel_port_get_type()

SPICE_DECLARE_TYPE(VmcChannelClient, vmc_channel_client, VMC_CHANNEL_CLIENT);
#define TYPE_VMC_CHANNEL_CLIENT vmc_channel_client_get_type()

SPICE_DECLARE_TYPE(RedVmcChannelUsbredir, red_vmc_channel_usbredir, VMC_CHANNEL_USBREDIR);
#define RED_TYPE_VMC_CHANNEL_USBREDIR red_vmc_channel_usbredir_get_type()

SPICE_DECLARE_TYPE(RedVmcChannelWebdav, red_vmc_channel_webdav, VMC_CHANNEL_WEBDAV);
#define RED_TYPE_VMC_CHANNEL_WEBDAV red_vmc_channel_webdav_get_type()

typedef struct RedVmcPipeItem
{
    RedPipeItem base;

    SpiceDataCompressionType type;
    uint32_t                 uncompressed_data_size;
    /* writes which don't fit this will get split, this is not a problem */
    uint8_t  buf[BUF_SIZE];
    uint32_t buf_used;
} RedVmcPipeItem;

struct RedCharDeviceSpiceVmc
{
    RedCharDevice  parent;
    RedVmcChannel *channel;
};

struct RedCharDeviceSpiceVmcClass
{
    RedCharDeviceClass parent_class;
};

static RedCharDevice *
red_char_device_spicevmc_new(SpiceCharDeviceInstance *sin, RedsState *reds, RedVmcChannel *channel);

G_DEFINE_TYPE(RedCharDeviceSpiceVmc, red_char_device_spicevmc, RED_TYPE_CHAR_DEVICE)

struct RedVmcChannel
{
    RedChannel parent;

    RedChannelClient *        rcc;
    RedCharDevice *           chardev; /* weak */
    SpiceCharDeviceInstance * chardev_sin;
    RedVmcPipeItem *          pipe_item;
    RedCharDeviceWriteBuffer *recv_from_client_buf;
    uint8_t                   port_opened;
    RedStatCounter            in_data;
    RedStatCounter            in_compressed;
    RedStatCounter            in_decompressed;
    RedStatCounter            out_data;
    RedStatCounter            out_compressed;
    RedStatCounter            out_uncompressed;
};

struct RedVmcChannelClass
{
    RedChannelClass parent_class;
};

G_DEFINE_TYPE(RedVmcChannel, red_vmc_channel, RED_TYPE_CHANNEL)

struct RedVmcChannelUsbredir
{
    RedVmcChannel parent;
};

struct RedVmcChannelUsbredirClass
{
    RedVmcChannelClass parent_class;
};

static void red_vmc_channel_usbredir_init(RedVmcChannelUsbredir *self) {}
G_DEFINE_TYPE(RedVmcChannelUsbredir, red_vmc_channel_usbredir, RED_TYPE_VMC_CHANNEL)

struct RedVmcChannelWebdav
{
    RedVmcChannel parent;
};

struct RedVmcChannelWebdavClass
{
    RedVmcChannelClass parent_class;
};

static void red_vmc_channel_webdav_init(RedVmcChannelWebdav *self) {}
G_DEFINE_TYPE(RedVmcChannelWebdav, red_vmc_channel_webdav, RED_TYPE_VMC_CHANNEL)

struct RedVmcChannelPort
{
    RedVmcChannel parent;
};

struct RedVmcChannelPortClass
{
    RedVmcChannelClass parent_class;
};

static void red_vmc_channel_port_init(RedVmcChannelPort *self) {}
G_DEFINE_TYPE(RedVmcChannelPort, red_vmc_channel_port, RED_TYPE_VMC_CHANNEL)

struct VmcChannelClient
{
    RedChannelClient parent;
};

struct VmcChannelClientClass
{
    RedChannelClientClass parent_class;
};

G_DEFINE_TYPE(VmcChannelClient, vmc_channel_client, RED_TYPE_CHANNEL_CLIENT)

static RedChannelClient *vmc_channel_client_create(RedChannel *            channel,
                                                   RedClient *             client,
                                                   RedStream *             stream,
                                                   RedChannelCapabilities *caps);

static void spicevmc_connect(RedChannel *            channel,
                             RedClient *             client,
                             RedStream *             stream,
                             int                     migration,
                             RedChannelCapabilities *caps);

static void red_vmc_channel_constructed(GObject *object)
{
    RedVmcChannel *self       = RED_VMC_CHANNEL(object);
    ClientCbs      client_cbs = {
        NULL,
    };
    RedsState *reds = red_channel_get_server(RED_CHANNEL(self));

    G_OBJECT_CLASS(red_vmc_channel_parent_class)->constructed(object);

    client_cbs.connect = spicevmc_connect;
    red_channel_register_client_cbs(RED_CHANNEL(self), &client_cbs, NULL);

    red_channel_init_stat_node(RED_CHANNEL(self), NULL, "spicevmc");
    const RedStatNode *stat = red_channel_get_stat_node(RED_CHANNEL(self));
    stat_init_counter(&self->in_data, reds, stat, "in_data", TRUE);
    stat_init_counter(&self->in_compressed, reds, stat, "in_compressed", TRUE);
    stat_init_counter(&self->in_decompressed, reds, stat, "in_decompressed", TRUE);
    stat_init_counter(&self->out_data, reds, stat, "out_data", TRUE);
    stat_init_counter(&self->out_compressed, reds, stat, "out_compressed", TRUE);
    stat_init_counter(&self->out_uncompressed, reds, stat, "out_uncompressed", TRUE);

#ifdef USE_LZ4
    red_channel_set_cap(RED_CHANNEL(self), SPICE_SPICEVMC_CAP_DATA_COMPRESS_LZ4);
#endif

    reds_register_channel(reds, RED_CHANNEL(self));
}

static void red_vmc_channel_init(RedVmcChannel *self) {}

static void red_vmc_channel_finalize(GObject *object)
{
    RedVmcChannel *self = RED_VMC_CHANNEL(object);

    red_char_device_write_buffer_release(self->chardev, &self->recv_from_client_buf);
    if (self->pipe_item) {
        red_pipe_item_unref(&self->pipe_item->base);
    }

    G_OBJECT_CLASS(red_vmc_channel_parent_class)->finalize(object);
}

static RedVmcChannel *red_vmc_channel_new(RedsState *reds, uint8_t channel_type)
{
    GType gtype = G_TYPE_NONE;

    switch (channel_type) {
    case SPICE_CHANNEL_USBREDIR:
        gtype = RED_TYPE_VMC_CHANNEL_USBREDIR;
        break;
    case SPICE_CHANNEL_WEBDAV:
        gtype = RED_TYPE_VMC_CHANNEL_WEBDAV;
        break;
    case SPICE_CHANNEL_PORT:
        gtype = RED_TYPE_VMC_CHANNEL_PORT;
        break;
    default:
        g_error("Unsupported channel_type for red_vmc_channel_new(): %u", channel_type);
        return NULL;
    }

    int id = reds_get_free_channel_id(reds, channel_type);
    if (id < 0) {
        g_warning("Free ID not found creating new VMC channel");
        return NULL;
    }

    return g_object_new(gtype, "spice-server", reds, "core-interface",
                        reds_get_core_interface(reds), "channel-type", channel_type, "id", id,
                        "handle-acks", FALSE, "migration-flags",
                        (SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER), NULL);
}

typedef struct RedPortInitPipeItem
{
    RedPipeItem base;
    char *      name;
    uint8_t     opened;
} RedPortInitPipeItem;

typedef struct RedPortEventPipeItem
{
    RedPipeItem base;
    uint8_t     event;
} RedPortEventPipeItem;

enum
{
    RED_PIPE_ITEM_TYPE_SPICEVMC_DATA = RED_PIPE_ITEM_TYPE_CHANNEL_BASE,
    RED_PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA,
    RED_PIPE_ITEM_TYPE_PORT_INIT,
    RED_PIPE_ITEM_TYPE_PORT_EVENT,
};

static void spicevmc_red_channel_release_msg_rcv_buf(RedChannelClient *rcc,
                                                     uint16_t          type,
                                                     uint32_t          size,
                                                     uint8_t *         msg);
/* n is the data size (uncompressed)
 * msg_item -- the current pipe item with the uncompressed data
 * This function returns:
 *  - NULL upon failure.
 *  - a new pipe item with the compressed data in it upon success
 */
#ifdef USE_LZ4
static RedVmcPipeItem *try_compress_lz4(RedVmcChannel *channel, int n, RedVmcPipeItem *msg_item)
{
    RedVmcPipeItem *msg_item_compressed;
    int             compressed_data_count;

    if (red_stream_get_family(red_channel_client_get_stream(channel->rcc)) == AF_UNIX) {
        /* AF_LOCAL - data will not be compressed */
        return NULL;
    }
    if (n <= COMPRESS_THRESHOLD) {
        /* n <= threshold - data will not be compressed */
        return NULL;
    }
    if (!red_channel_test_remote_cap(RED_CHANNEL(channel), SPICE_SPICEVMC_CAP_DATA_COMPRESS_LZ4)) {
        /* Client doesn't have compression cap - data will not be compressed */
        return NULL;
    }
    msg_item_compressed = g_new0(RedVmcPipeItem, 1);
    red_pipe_item_init(&msg_item_compressed->base, RED_PIPE_ITEM_TYPE_SPICEVMC_DATA);
    compressed_data_count = LZ4_compress_default((char *)&msg_item->buf,
                                                 (char *)&msg_item_compressed->buf, n, BUF_SIZE);

    if (compressed_data_count > 0 && compressed_data_count < n) {
        stat_inc_counter(channel->out_uncompressed, n);
        stat_inc_counter(channel->out_compressed, compressed_data_count);
        msg_item_compressed->type                   = SPICE_DATA_COMPRESSION_TYPE_LZ4;
        msg_item_compressed->uncompressed_data_size = n;
        msg_item_compressed->buf_used               = compressed_data_count;
        g_free(msg_item);
        return msg_item_compressed;
    }

    /* LZ4 compression failed or did non compress, fallback a non-compressed data
     * is to be sent */
    g_free(msg_item_compressed);
    return NULL;
}
#endif

static RedPipeItem *spicevmc_chardev_read_msg_from_dev(RedCharDevice *          self,
                                                       SpiceCharDeviceInstance *sin)
{
    RedCharDeviceSpiceVmc *   vmc     = RED_CHAR_DEVICE_SPICEVMC(self);
    RedVmcChannel *           channel = RED_VMC_CHANNEL(vmc->channel);
    SpiceCharDeviceInterface *sif;
    RedVmcPipeItem *          msg_item;
    int                       n;

    sif = spice_char_device_get_interface(sin);

    if (!channel->rcc) {
        return NULL;
    }

    if (!channel->pipe_item) {
        msg_item       = g_new0(RedVmcPipeItem, 1);
        msg_item->type = SPICE_DATA_COMPRESSION_TYPE_NONE;
        red_pipe_item_init(&msg_item->base, RED_PIPE_ITEM_TYPE_SPICEVMC_DATA);
    } else {
        spice_assert(channel->pipe_item->buf_used == 0);
        msg_item           = channel->pipe_item;
        channel->pipe_item = NULL;
    }

    n = sif->read(sin, msg_item->buf, sizeof(msg_item->buf));
    if (n > 0) {
        spice_debug("read from dev %d", n);
#ifdef USE_LZ4
        RedVmcPipeItem *msg_item_compressed;

        msg_item_compressed = try_compress_lz4(channel, n, msg_item);
        if (msg_item_compressed != NULL) {
            return &msg_item_compressed->base;
        }
#endif
        stat_inc_counter(channel->out_data, n);
        msg_item->uncompressed_data_size = n;
        msg_item->buf_used               = n;
        return &msg_item->base;
    } else {
        channel->pipe_item = msg_item;
        return NULL;
    }
}

static void
spicevmc_chardev_send_msg_to_client(RedCharDevice *self, RedPipeItem *msg, RedClient *client)
{
    RedCharDeviceSpiceVmc *vmc     = RED_CHAR_DEVICE_SPICEVMC(self);
    RedVmcChannel *        channel = RED_VMC_CHANNEL(vmc->channel);

    spice_assert(red_channel_client_get_client(channel->rcc) == client);
    red_pipe_item_ref(msg);
    red_channel_client_pipe_add_push(channel->rcc, msg);
}

static void red_port_init_item_free(struct RedPipeItem *base)
{
    RedPortInitPipeItem *item = SPICE_UPCAST(RedPortInitPipeItem, base);

    g_free(item->name);
    g_free(item);
}

static void spicevmc_port_send_init(RedChannelClient *rcc)
{
    RedVmcChannel *          channel = RED_VMC_CHANNEL(red_channel_client_get_channel(rcc));
    SpiceCharDeviceInstance *sin     = channel->chardev_sin;
    RedPortInitPipeItem *    item    = g_new(RedPortInitPipeItem, 1);

    red_pipe_item_init_full(&item->base, RED_PIPE_ITEM_TYPE_PORT_INIT, red_port_init_item_free);
    item->name   = g_strdup(sin->portname);
    item->opened = channel->port_opened;
    red_channel_client_pipe_add_push(rcc, &item->base);
}

static void spicevmc_port_send_event(RedChannelClient *rcc, uint8_t event)
{
    RedPortEventPipeItem *item = g_new(RedPortEventPipeItem, 1);

    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_PORT_EVENT);
    item->event = event;
    red_channel_client_pipe_add_push(rcc, &item->base);
}

static void
spicevmc_char_dev_send_tokens_to_client(RedCharDevice *self, RedClient *client, uint32_t tokens)
{
    spice_printerr("Not implemented!");
}

static void spicevmc_char_dev_remove_client(RedCharDevice *self, RedClient *client)
{
    RedCharDeviceSpiceVmc *vmc     = RED_CHAR_DEVICE_SPICEVMC(self);
    RedVmcChannel *        channel = RED_VMC_CHANNEL(vmc->channel);

    spice_printerr("vmc channel %p, client %p", channel, client);
    spice_assert(channel->rcc && red_channel_client_get_client(channel->rcc) == client);

    red_channel_client_shutdown(channel->rcc);
}

static void spicevmc_red_channel_client_on_disconnect(RedChannelClient *rcc)
{
    RedVmcChannel *           channel;
    SpiceCharDeviceInterface *sif;
    RedClient *               client = red_channel_client_get_client(rcc);

    channel = RED_VMC_CHANNEL(red_channel_client_get_channel(rcc));

    /* partial message which wasn't pushed to device */
    red_char_device_write_buffer_release(channel->chardev, &channel->recv_from_client_buf);

    if (channel->chardev) {
        if (red_char_device_client_exists(channel->chardev, client)) {
            red_char_device_client_remove(channel->chardev, client);
        } else {
            spice_printerr("client %p have already been removed from char dev %p", client,
                           channel->chardev);
        }
    }

    /* Don't destroy the rcc if it is already being destroyed, as then
       red_client_destroy/red_channel_client_destroy will already do this! */
    if (!red_channel_client_is_destroying(rcc))
        red_channel_client_destroy(rcc);

    channel->rcc = NULL;
    sif          = spice_char_device_get_interface(channel->chardev_sin);
    if (sif->state) {
        sif->state(channel->chardev_sin, 0);
    }
}

static bool spicevmc_channel_client_handle_migrate_flush_mark(RedChannelClient *rcc)
{
    red_channel_client_pipe_add_type(rcc, RED_PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA);
    return TRUE;
}

static bool
spicevmc_channel_client_handle_migrate_data(RedChannelClient *rcc, uint32_t size, void *message)
{
    SpiceMigrateDataHeader *  header;
    SpiceMigrateDataSpiceVmc *mig_data;
    RedVmcChannel *           channel;

    channel = RED_VMC_CHANNEL(red_channel_client_get_channel(rcc));

    header   = (SpiceMigrateDataHeader *)message;
    mig_data = (SpiceMigrateDataSpiceVmc *)(header + 1);
    spice_assert(size >= sizeof(SpiceMigrateDataHeader) + sizeof(SpiceMigrateDataSpiceVmc));

    if (!migration_protocol_validate_header(header, SPICE_MIGRATE_DATA_SPICEVMC_MAGIC,
                                            SPICE_MIGRATE_DATA_SPICEVMC_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }
    return red_char_device_restore(channel->chardev, &mig_data->base);
}

static bool handle_compressed_msg(RedVmcChannel *         channel,
                                  RedChannelClient *      rcc,
                                  SpiceMsgCompressedData *compressed_data_msg)
{
    /* NOTE: *decompressed is free by the char-device */
    int                       decompressed_size;
    RedCharDeviceWriteBuffer *write_buf;

    write_buf =
        red_char_device_write_buffer_get(channel->chardev, red_channel_client_get_client(rcc),
                                         compressed_data_msg->uncompressed_size);
    if (!write_buf) {
        return FALSE;
    }

    switch (compressed_data_msg->type) {
#ifdef USE_LZ4
    case SPICE_DATA_COMPRESSION_TYPE_LZ4: {
        uint8_t *decompressed = write_buf->buf;
        decompressed_size     = LZ4_decompress_safe(
            (char *)compressed_data_msg->compressed_data, (char *)decompressed,
            compressed_data_msg->compressed_size, compressed_data_msg->uncompressed_size);
        stat_inc_counter(channel->in_compressed, compressed_data_msg->compressed_size);
        stat_inc_counter(channel->in_decompressed, decompressed_size);
        break;
    }
#endif
    default:
        spice_warning("Invalid Compression Type");
        red_char_device_write_buffer_release(channel->chardev, &write_buf);
        return FALSE;
    }
    if (decompressed_size != compressed_data_msg->uncompressed_size) {
        spice_warning("Decompression Error");
        red_char_device_write_buffer_release(channel->chardev, &write_buf);
        return FALSE;
    }
    write_buf->buf_used = decompressed_size;
    red_char_device_write_buffer_add(channel->chardev, write_buf);
    return TRUE;
}

static bool spicevmc_red_channel_client_handle_message(RedChannelClient *rcc,
                                                       uint16_t          type,
                                                       uint32_t          size,
                                                       void *            msg)
{
    /* NOTE: *msg free by g_free() (when cb to
     * spicevmc_red_channel_release_msg_rcv_buf
     * with the compressed msg type) */
    RedVmcChannel *           channel;
    SpiceCharDeviceInterface *sif;

    channel = RED_VMC_CHANNEL(red_channel_client_get_channel(rcc));
    sif     = spice_char_device_get_interface(channel->chardev_sin);

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA:
        spice_assert(channel->recv_from_client_buf->buf == msg);
        stat_inc_counter(channel->in_data, size);
        channel->recv_from_client_buf->buf_used = size;
        red_char_device_write_buffer_add(channel->chardev, channel->recv_from_client_buf);
        channel->recv_from_client_buf = NULL;
        break;
    case SPICE_MSGC_SPICEVMC_COMPRESSED_DATA:
        return handle_compressed_msg(channel, rcc, (SpiceMsgCompressedData *)msg);
        break;
    case SPICE_MSGC_PORT_EVENT:
        if (size != sizeof(uint8_t)) {
            spice_warning("bad port event message size");
            return FALSE;
        }
        if (sif->base.minor_version >= 2 && sif->event != NULL)
            sif->event(channel->chardev_sin, *(uint8_t *)msg);
        break;
    default:
        return red_channel_client_handle_message(rcc, type, size, msg);
    }

    return TRUE;
}

static uint8_t *
spicevmc_red_channel_alloc_msg_rcv_buf(RedChannelClient *rcc, uint16_t type, uint32_t size)
{

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA: {
        RedClient *    client  = red_channel_client_get_client(rcc);
        RedVmcChannel *channel = RED_VMC_CHANNEL(red_channel_client_get_channel(rcc));

        assert(!channel->recv_from_client_buf);

        channel->recv_from_client_buf =
            red_char_device_write_buffer_get(channel->chardev, client, size);
        if (!channel->recv_from_client_buf) {
            spice_error("failed to allocate write buffer");
            return NULL;
        }
        return channel->recv_from_client_buf->buf;
    }

    default:
        return g_malloc(size);
    }
}

static void spicevmc_red_channel_release_msg_rcv_buf(RedChannelClient *rcc,
                                                     uint16_t          type,
                                                     uint32_t          size,
                                                     uint8_t *         msg)
{

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA: {
        RedVmcChannel *channel = RED_VMC_CHANNEL(red_channel_client_get_channel(rcc));
        /* buffer wasn't pushed to device */
        red_char_device_write_buffer_release(channel->chardev, &channel->recv_from_client_buf);
        break;
    }
    default:
        g_free(msg);
    }
}

static void
spicevmc_red_channel_send_data(RedChannelClient *rcc, SpiceMarshaller *m, RedPipeItem *item)
{
    RedVmcPipeItem *i = SPICE_UPCAST(RedVmcPipeItem, item);

    /* for compatibility send using not compressed data message */
    if (i->type == SPICE_DATA_COMPRESSION_TYPE_NONE) {
        red_channel_client_init_send_data(rcc, SPICE_MSG_SPICEVMC_DATA);
    } else {
        /* send as compressed */
        red_channel_client_init_send_data(rcc, SPICE_MSG_SPICEVMC_COMPRESSED_DATA);
        SpiceMsgCompressedData compressed_msg = {.type              = i->type,
                                                 .uncompressed_size = i->uncompressed_data_size};
        spice_marshall_SpiceMsgCompressedData(m, &compressed_msg);
    }
    red_pipe_item_ref(item);
    spice_marshaller_add_by_ref_full(m, i->buf, i->buf_used, marshaller_unref_pipe_item, item);
}

static void
spicevmc_red_channel_send_migrate_data(RedChannelClient *rcc, SpiceMarshaller *m, RedPipeItem *item)
{
    RedVmcChannel *channel;

    channel = RED_VMC_CHANNEL(red_channel_client_get_channel(rcc));
    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE_DATA);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_SPICEVMC_MAGIC);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_SPICEVMC_VERSION);

    red_char_device_migrate_data_marshall(channel->chardev, m);
}

static void
spicevmc_red_channel_send_port_init(RedChannelClient *rcc, SpiceMarshaller *m, RedPipeItem *item)
{
    RedPortInitPipeItem *i = SPICE_UPCAST(RedPortInitPipeItem, item);
    SpiceMsgPortInit     init;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PORT_INIT);
    init.name      = (uint8_t *)i->name;
    init.name_size = strlen(i->name) + 1;
    init.opened    = i->opened;
    spice_marshall_msg_port_init(m, &init);
}

static void
spicevmc_red_channel_send_port_event(RedChannelClient *rcc, SpiceMarshaller *m, RedPipeItem *item)
{
    RedPortEventPipeItem *i = SPICE_UPCAST(RedPortEventPipeItem, item);
    SpiceMsgPortEvent     event;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PORT_EVENT);
    event.event = i->event;
    spice_marshall_msg_port_event(m, &event);
}

static void spicevmc_red_channel_send_item(RedChannelClient *rcc, RedPipeItem *item)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);

    switch (item->type) {
    case RED_PIPE_ITEM_TYPE_SPICEVMC_DATA:
        spicevmc_red_channel_send_data(rcc, m, item);
        break;
    case RED_PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA:
        spicevmc_red_channel_send_migrate_data(rcc, m, item);
        break;
    case RED_PIPE_ITEM_TYPE_PORT_INIT:
        spicevmc_red_channel_send_port_init(rcc, m, item);
        break;
    case RED_PIPE_ITEM_TYPE_PORT_EVENT:
        spicevmc_red_channel_send_port_event(rcc, m, item);
        break;
    default:
        spice_error("bad pipe item %d", item->type);
        return;
    }
    red_channel_client_begin_send_message(rcc);
}

static void red_vmc_channel_class_init(RedVmcChannelClass *klass)
{
    GObjectClass *   object_class  = G_OBJECT_CLASS(klass);
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    object_class->constructed = red_vmc_channel_constructed;
    object_class->finalize    = red_vmc_channel_finalize;

    channel_class->handle_message = spicevmc_red_channel_client_handle_message;

    channel_class->send_item                 = spicevmc_red_channel_send_item;
    channel_class->handle_migrate_flush_mark = spicevmc_channel_client_handle_migrate_flush_mark;
    channel_class->handle_migrate_data       = spicevmc_channel_client_handle_migrate_data;
}

static void red_vmc_channel_usbredir_class_init(RedVmcChannelUsbredirClass *klass)
{
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_USBREDIR, NULL);
}

static void red_vmc_channel_webdav_class_init(RedVmcChannelWebdavClass *klass)
{
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_WEBDAV, NULL);
}

static void red_vmc_channel_port_class_init(RedVmcChannelPortClass *klass)
{
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_PORT, NULL);
}

static void spicevmc_connect(RedChannel *            channel,
                             RedClient *             client,
                             RedStream *             stream,
                             int                     migration,
                             RedChannelCapabilities *caps)
{
    RedChannelClient *        rcc;
    RedVmcChannel *           vmc_channel;
    SpiceCharDeviceInstance * sin;
    SpiceCharDeviceInterface *sif;

    vmc_channel = RED_VMC_CHANNEL(channel);
    sin         = vmc_channel->chardev_sin;

    if (vmc_channel->rcc) {
        red_channel_printerr(channel,
                             "channel client (%p) already connected, refusing second connection",
                             vmc_channel->rcc);
        // TODO: notify client in advance about the in use channel using
        // SPICE_MSG_MAIN_CHANNEL_IN_USE (for example)
        red_stream_free(stream);
        return;
    }

    rcc = vmc_channel_client_create(channel, client, stream, caps);
    if (!rcc) {
        return;
    }
    vmc_channel->rcc = rcc;
    red_channel_client_ack_zero_messages_window(rcc);

    if (strcmp(sin->subtype, "port") == 0) {
        spicevmc_port_send_init(rcc);
    }

    if (!red_char_device_client_add(vmc_channel->chardev, client, FALSE, 0, ~0, ~0,
                                    red_channel_client_is_waiting_for_migrate_data(rcc))) {
        spice_warning("failed to add client to spicevmc");
        red_channel_client_disconnect(rcc);
        return;
    }

    sif = spice_char_device_get_interface(vmc_channel->chardev_sin);
    if (sif->state) {
        sif->state(sin, 1);
    }
}

RedCharDevice *
spicevmc_device_connect(RedsState *reds, SpiceCharDeviceInstance *sin, uint8_t channel_type)
{
    RedCharDevice *dev;
    RedVmcChannel *channel = red_vmc_channel_new(reds, channel_type);
    if (!channel) {
        return NULL;
    }

    /* char device takes ownership of channel */
    dev = red_char_device_spicevmc_new(sin, reds, channel);

    channel->chardev_sin = sin;
    g_object_unref(channel);

    return dev;
}

/* Must be called from RedClient handling thread. */
void spicevmc_device_disconnect(RedsState *reds, SpiceCharDeviceInstance *sin)
{
    g_object_unref(RED_CHAR_DEVICE(sin->st));
    sin->st = NULL;
}

static void spicevmc_port_event(RedCharDevice *char_dev, uint8_t event)
{
    RedVmcChannel *        channel;
    RedCharDeviceSpiceVmc *device = RED_CHAR_DEVICE_SPICEVMC(char_dev);

    channel = RED_VMC_CHANNEL(device->channel);

    if (event == SPICE_PORT_EVENT_OPENED) {
        channel->port_opened = TRUE;
    } else if (event == SPICE_PORT_EVENT_CLOSED) {
        channel->port_opened = FALSE;
    }

    if (channel->rcc == NULL) {
        return;
    }

    spicevmc_port_send_event(channel->rcc, event);
}

static void red_char_device_spicevmc_dispose(GObject *object)
{
    RedCharDeviceSpiceVmc *self = RED_CHAR_DEVICE_SPICEVMC(object);

    if (self->channel) {
        // prevent possible recursive calls
        self->channel->chardev = NULL;

        // close all current connections and drop the reference
        red_channel_destroy(RED_CHANNEL(self->channel));
        self->channel = NULL;
    }
    G_OBJECT_CLASS(red_char_device_spicevmc_parent_class)->dispose(object);
}

enum
{
    PROP0,
    PROP_CHANNEL
};

static void red_char_device_spicevmc_set_property(GObject *     object,
                                                  guint         property_id,
                                                  const GValue *value,
                                                  GParamSpec *  pspec)
{
    RedCharDeviceSpiceVmc *self = RED_CHAR_DEVICE_SPICEVMC(object);

    switch (property_id) {
    case PROP_CHANNEL:
        spice_assert(self->channel == NULL);
        self->channel = g_value_dup_object(value);
        spice_assert(self->channel != NULL);
        self->channel->chardev = RED_CHAR_DEVICE(self);

        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void red_char_device_spicevmc_class_init(RedCharDeviceSpiceVmcClass *klass)
{
    GObjectClass *      object_class   = G_OBJECT_CLASS(klass);
    RedCharDeviceClass *char_dev_class = RED_CHAR_DEVICE_CLASS(klass);

    object_class->set_property = red_char_device_spicevmc_set_property;
    object_class->dispose      = red_char_device_spicevmc_dispose;

    char_dev_class->read_one_msg_from_device = spicevmc_chardev_read_msg_from_dev;
    char_dev_class->send_msg_to_client       = spicevmc_chardev_send_msg_to_client;
    char_dev_class->send_tokens_to_client    = spicevmc_char_dev_send_tokens_to_client;
    char_dev_class->remove_client            = spicevmc_char_dev_remove_client;
    char_dev_class->port_event               = spicevmc_port_event;

    g_object_class_install_property(
        object_class, PROP_CHANNEL,
        g_param_spec_object("channel", "Channel", "Channel associated with this device",
                            RED_TYPE_VMC_CHANNEL,
                            G_PARAM_STATIC_STRINGS | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));
}

static void red_char_device_spicevmc_init(RedCharDeviceSpiceVmc *self) {}

static RedCharDevice *
red_char_device_spicevmc_new(SpiceCharDeviceInstance *sin, RedsState *reds, RedVmcChannel *channel)
{
    return g_object_new(RED_TYPE_CHAR_DEVICE_SPICEVMC, "sin", sin, "spice-server", reds,
                        "client-tokens-interval", 0ULL, "self-tokens", ~0ULL, "channel", channel,
                        NULL);
}

static void vmc_channel_client_init(VmcChannelClient *self) {}

static void vmc_channel_client_class_init(VmcChannelClientClass *klass)
{
    RedChannelClientClass *client_class = RED_CHANNEL_CLIENT_CLASS(klass);

    client_class->alloc_recv_buf   = spicevmc_red_channel_alloc_msg_rcv_buf;
    client_class->release_recv_buf = spicevmc_red_channel_release_msg_rcv_buf;
    client_class->on_disconnect    = spicevmc_red_channel_client_on_disconnect;
}

static RedChannelClient *vmc_channel_client_create(RedChannel *            channel,
                                                   RedClient *             client,
                                                   RedStream *             stream,
                                                   RedChannelCapabilities *caps)
{
    RedChannelClient *rcc;

    rcc = g_initable_new(TYPE_VMC_CHANNEL_CLIENT, NULL, NULL, "channel", channel, "client", client,
                         "stream", stream, "caps", caps, NULL);

    return rcc;
}
