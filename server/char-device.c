/* spice-server char device flow control code

   Copyright (C) 2012-2015 Red Hat, Inc.

   Red Hat Authors:
   Yonit Halperin <yhalperi@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http:www.gnu.org/licenses/>.
*/

#include <config.h>
#include "char-device.h"
#include "red-client.h"
#include "reds.h"
#include <inttypes.h>
#include "glib-compat.h"

#define CHAR_DEVICE_WRITE_TO_TIMEOUT 100
#define RED_CHAR_DEVICE_WAIT_TOKENS_TIMEOUT 30000
#define MAX_POOL_SIZE (10 * 64 * 1024)

typedef enum {
    WRITE_BUFFER_ORIGIN_NONE,
    WRITE_BUFFER_ORIGIN_CLIENT,
    WRITE_BUFFER_ORIGIN_SERVER,
    WRITE_BUFFER_ORIGIN_SERVER_NO_TOKEN,
} WriteBufferOrigin;

struct RedCharDeviceWriteBufferPrivate
{
    RedClient *client; /* The client that sent the message to the device.
                          NULL if the server created the message */
    WriteBufferOrigin origin;
    uint32_t          token_price;
    uint32_t          refs;
};

typedef struct RedCharDeviceClient RedCharDeviceClient;
struct RedCharDeviceClient
{
    RedCharDevice *dev;
    RedClient *    client;
    int            do_flow_control;
    uint64_t       num_client_tokens;
    uint64_t       num_client_tokens_free; /* client messages that were consumed by the
                                              device */
    uint64_t    num_send_tokens;           /* send to client */
    SpiceTimer *wait_for_tokens_timer;
    int         wait_for_tokens_started;
    GQueue *    send_queue;
    uint32_t    max_send_queue_size;
};

struct RedCharDevicePrivate
{
    int running;
    int active; /* has read/write been performed since the device was started */
    int wait_for_migrate_data;

    GQueue                    write_queue;
    GQueue                    write_bufs_pool;
    uint64_t                  cur_pool_size;
    RedCharDeviceWriteBuffer *cur_write_buf;
    uint8_t *                 cur_write_buf_pos;
    SpiceTimer *              write_to_dev_timer;
    uint64_t                  num_self_tokens;

    GList *clients; /* list of RedCharDeviceClient */

    uint64_t client_tokens_interval; /* frequency of returning tokens to the client */
    SpiceCharDeviceInstance *sin;

    int during_read_from_device;
    int during_write_to_device;

    SpiceServer *reds;
};

G_DEFINE_TYPE(RedCharDevice, red_char_device, G_TYPE_OBJECT)

#define RED_CHAR_DEVICE_PRIVATE(o)                                                                 \
    (G_TYPE_INSTANCE_GET_PRIVATE((o), RED_TYPE_CHAR_DEVICE, RedCharDevicePrivate))

enum
{
    PROP_0,
    PROP_CHAR_DEV_INSTANCE,
    PROP_SPICE_SERVER,
    PROP_CLIENT_TOKENS_INTERVAL,
    PROP_SELF_TOKENS,
};

static void red_char_device_write_buffer_unref(RedCharDeviceWriteBuffer *write_buf);
static void red_char_device_write_retry(void *opaque);

static RedPipeItem *red_char_device_read_one_msg_from_device(RedCharDevice *dev)
{
    RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(dev);

    return klass->read_one_msg_from_device(dev, dev->priv->sin);
}

static void
red_char_device_send_msg_to_client(RedCharDevice *dev, RedPipeItem *msg, RedClient *client)
{
    RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(dev);

    klass->send_msg_to_client(dev, msg, client);
}

static void
red_char_device_send_tokens_to_client(RedCharDevice *dev, RedClient *client, uint32_t tokens)
{
    RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(dev);

    klass->send_tokens_to_client(dev, client, tokens);
}

static void red_char_device_on_free_self_token(RedCharDevice *dev)
{
    RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(dev);

    if (klass->on_free_self_token != NULL) {
        klass->on_free_self_token(dev);
    }
}

static void red_char_device_remove_client(RedCharDevice *dev, RedClient *client)
{
    RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(dev);

    klass->remove_client(dev, client);
}

static void red_char_device_write_buffer_free(RedCharDeviceWriteBuffer *buf)
{
    if (buf == NULL)
        return;

    g_free(buf->buf);
    g_free(buf);
}

static void write_buffers_queue_free(GQueue *write_queue)
{
    RedCharDeviceWriteBuffer *buf;
    while ((buf = g_queue_pop_tail(write_queue)))
        red_char_device_write_buffer_free(buf);
}

static void red_char_device_write_buffer_pool_add(RedCharDevice *dev, RedCharDeviceWriteBuffer *buf)
{
    if (buf->priv->refs == 1 && dev->priv->cur_pool_size < MAX_POOL_SIZE) {
        buf->buf_used     = 0;
        buf->priv->origin = WRITE_BUFFER_ORIGIN_NONE;
        buf->priv->client = NULL;
        dev->priv->cur_pool_size += buf->buf_size;
        g_queue_push_head(&dev->priv->write_bufs_pool, buf);
        return;
    }

    /* Buffer still being used - just unref for the caller */
    red_char_device_write_buffer_unref(buf);
}

static void red_char_device_client_free(RedCharDevice *dev, RedCharDeviceClient *dev_client)
{
    GList *l, *next;

    reds_core_timer_remove(dev->priv->reds, dev_client->wait_for_tokens_timer);
    dev_client->wait_for_tokens_timer = NULL;

    g_queue_free_full(dev_client->send_queue, (GDestroyNotify)red_pipe_item_unref);

    /* remove write buffers that are associated with the client */
    spice_debug("write_queue_is_empty %d",
                g_queue_is_empty(&dev->priv->write_queue) && !dev->priv->cur_write_buf);
    l = g_queue_peek_head_link(&dev->priv->write_queue);
    while (l) {
        RedCharDeviceWriteBuffer *write_buf = l->data;
        next                                = l->next;

        if (write_buf->priv->origin == WRITE_BUFFER_ORIGIN_CLIENT &&
            write_buf->priv->client == dev_client->client) {
            g_queue_delete_link(&dev->priv->write_queue, l);
            red_char_device_write_buffer_pool_add(dev, write_buf);
        }
        l = next;
    }

    if (dev->priv->cur_write_buf &&
        dev->priv->cur_write_buf->priv->origin == WRITE_BUFFER_ORIGIN_CLIENT &&
        dev->priv->cur_write_buf->priv->client == dev_client->client) {
        dev->priv->cur_write_buf->priv->origin = WRITE_BUFFER_ORIGIN_NONE;
        dev->priv->cur_write_buf->priv->client = NULL;
    }

    dev->priv->clients = g_list_remove(dev->priv->clients, dev_client);
    g_free(dev_client);
}

static void red_char_device_handle_client_overflow(RedCharDeviceClient *dev_client)
{
    RedCharDevice *dev = dev_client->dev;
    spice_printerr("dev %p client %p ", dev, dev_client);
    red_char_device_remove_client(dev, dev_client->client);
}

static RedCharDeviceClient *red_char_device_client_find(RedCharDevice *dev, RedClient *client)
{
    GList *item;

    for (item = dev->priv->clients; item != NULL; item = item->next) {
        RedCharDeviceClient *dev_client = item->data;

        if (dev_client->client == client) {
            return dev_client;
        }
    }
    return NULL;
}

/***************************
 * Reading from the device *
 **************************/

static void device_client_wait_for_tokens_timeout(void *opaque)
{
    RedCharDeviceClient *dev_client = opaque;

    red_char_device_handle_client_overflow(dev_client);
}

static int red_char_device_can_send_to_client(RedCharDeviceClient *dev_client)
{
    return !dev_client->do_flow_control || dev_client->num_send_tokens;
}

static uint64_t red_char_device_max_send_tokens(RedCharDevice *dev)
{
    GList *  item;
    uint64_t max = 0;

    for (item = dev->priv->clients; item != NULL; item = item->next) {
        RedCharDeviceClient *dev_client = item->data;

        if (!dev_client->do_flow_control) {
            max = ~0;
            break;
        }

        if (dev_client->num_send_tokens > max) {
            max = dev_client->num_send_tokens;
        }
    }
    return max;
}

static void red_char_device_add_msg_to_client_queue(RedCharDeviceClient *dev_client,
                                                    RedPipeItem *        msg)
{
    RedCharDevice *dev = dev_client->dev;

    if (g_queue_get_length(dev_client->send_queue) >= dev_client->max_send_queue_size) {
        red_char_device_handle_client_overflow(dev_client);
        return;
    }

    red_pipe_item_ref(msg);
    g_queue_push_head(dev_client->send_queue, msg);
    if (!dev_client->wait_for_tokens_started) {
        reds_core_timer_start(dev->priv->reds, dev_client->wait_for_tokens_timer,
                              RED_CHAR_DEVICE_WAIT_TOKENS_TIMEOUT);
        dev_client->wait_for_tokens_started = TRUE;
    }
}

static void red_char_device_send_msg_to_clients(RedCharDevice *dev, RedPipeItem *msg)
{
    GList *l;

    l = dev->priv->clients;
    while (l) {
        GList *              next       = l->next;
        RedCharDeviceClient *dev_client = l->data;

        if (red_char_device_can_send_to_client(dev_client)) {
            dev_client->num_send_tokens--;
            spice_assert(g_queue_is_empty(dev_client->send_queue));
            red_char_device_send_msg_to_client(dev, msg, dev_client->client);

            /* don't refer to dev_client anymore, it may have been released */
        } else {
            red_char_device_add_msg_to_client_queue(dev_client, msg);
        }
        l = next;
    }
}

static bool red_char_device_read_from_device(RedCharDevice *dev)
{
    uint64_t max_send_tokens;
    int      did_read = FALSE;

    if (!dev->priv->running || dev->priv->wait_for_migrate_data || !dev->priv->sin) {
        return FALSE;
    }

    /* There are 2 scenarios where we can get called recursively:
     * 1) spice-vmc vmc_read triggering flush of throttled data, recalling wakeup
     * (virtio)
     * 2) in case of sending messages to the client, and unreferencing the
     * msg, we trigger another read.
     */
    if (dev->priv->during_read_from_device++ > 0) {
        return FALSE;
    }

    max_send_tokens = red_char_device_max_send_tokens(dev);
    g_object_ref(dev);
    /*
     * Reading from the device only in case at least one of the clients have a
     * free token.
     * All messages will be discarded if no client is attached to the device
     */
    while ((max_send_tokens || (dev->priv->clients == NULL)) && dev->priv->running) {
        RedPipeItem *msg;

        msg = red_char_device_read_one_msg_from_device(dev);
        if (!msg) {
            if (dev->priv->during_read_from_device > 1) {
                dev->priv->during_read_from_device = 1;
                continue; /* a wakeup might have been called during the read -
                             make sure it doesn't get lost */
            }
            break;
        }
        did_read = TRUE;
        red_char_device_send_msg_to_clients(dev, msg);
        red_pipe_item_unref(msg);
        max_send_tokens--;
    }
    dev->priv->during_read_from_device = 0;
    if (dev->priv->running) {
        dev->priv->active = dev->priv->active || did_read;
    }
    g_object_unref(dev);
    return did_read;
}

static void red_char_device_client_send_queue_push(RedCharDeviceClient *dev_client)
{
    while (!g_queue_is_empty(dev_client->send_queue) &&
           red_char_device_can_send_to_client(dev_client)) {
        RedPipeItem *msg = g_queue_pop_tail(dev_client->send_queue);
        g_assert(msg != NULL);
        dev_client->num_send_tokens--;
        red_char_device_send_msg_to_client(dev_client->dev, msg, dev_client->client);
        red_pipe_item_unref(msg);
    }
}

static void red_char_device_send_to_client_tokens_absorb(RedCharDeviceClient *dev_client,
                                                         uint32_t             tokens)
{
    RedCharDevice *dev = dev_client->dev;
    dev_client->num_send_tokens += tokens;

    if (g_queue_get_length(dev_client->send_queue)) {
        spice_assert(dev_client->num_send_tokens == tokens);
        red_char_device_client_send_queue_push(dev_client);
    }

    if (red_char_device_can_send_to_client(dev_client)) {
        reds_core_timer_cancel(dev->priv->reds, dev_client->wait_for_tokens_timer);
        dev_client->wait_for_tokens_started = FALSE;
        red_char_device_read_from_device(dev_client->dev);
    } else if (!g_queue_is_empty(dev_client->send_queue)) {
        reds_core_timer_start(dev->priv->reds, dev_client->wait_for_tokens_timer,
                              RED_CHAR_DEVICE_WAIT_TOKENS_TIMEOUT);
        dev_client->wait_for_tokens_started = TRUE;
    }
}

void red_char_device_send_to_client_tokens_add(RedCharDevice *dev,
                                               RedClient *    client,
                                               uint32_t       tokens)
{
    RedCharDeviceClient *dev_client;

    dev_client = red_char_device_client_find(dev, client);

    if (!dev_client) {
        spice_error("client wasn't found dev %p client %p", dev, client);
        return;
    }
    red_char_device_send_to_client_tokens_absorb(dev_client, tokens);
}

void red_char_device_send_to_client_tokens_set(RedCharDevice *dev,
                                               RedClient *    client,
                                               uint32_t       tokens)
{
    RedCharDeviceClient *dev_client;

    dev_client = red_char_device_client_find(dev, client);

    if (!dev_client) {
        spice_error("client wasn't found dev %p client %p", dev, client);
        return;
    }

    dev_client->num_send_tokens = 0;
    red_char_device_send_to_client_tokens_absorb(dev_client, tokens);
}

/**************************
 * Writing to the device  *
***************************/

static void red_char_device_client_tokens_add(RedCharDevice *      dev,
                                              RedCharDeviceClient *dev_client,
                                              uint32_t             num_tokens)
{
    if (!dev_client->do_flow_control) {
        return;
    }
    if (num_tokens > 1) {
        spice_debug("#tokens > 1 (=%u)", num_tokens);
    }
    dev_client->num_client_tokens_free += num_tokens;
    if (dev_client->num_client_tokens_free >= dev->priv->client_tokens_interval) {
        uint32_t tokens = dev_client->num_client_tokens_free;

        dev_client->num_client_tokens += dev_client->num_client_tokens_free;
        dev_client->num_client_tokens_free = 0;
        red_char_device_send_tokens_to_client(dev, dev_client->client, tokens);
    }
}

static int red_char_device_write_to_device(RedCharDevice *dev)
{
    SpiceCharDeviceInterface *sif;
    int                       total = 0;
    int                       n;

    if (!dev->priv->running || dev->priv->wait_for_migrate_data || !dev->priv->sin) {
        return 0;
    }

    /* protect against recursion with red_char_device_wakeup */
    if (dev->priv->during_write_to_device++ > 0) {
        return 0;
    }

    g_object_ref(dev);

    if (dev->priv->write_to_dev_timer) {
        reds_core_timer_cancel(dev->priv->reds, dev->priv->write_to_dev_timer);
    }

    sif = spice_char_device_get_interface(dev->priv->sin);
    while (dev->priv->running) {
        uint32_t write_len;

        if (!dev->priv->cur_write_buf) {
            dev->priv->cur_write_buf = g_queue_pop_tail(&dev->priv->write_queue);
            if (!dev->priv->cur_write_buf)
                break;
            dev->priv->cur_write_buf_pos = dev->priv->cur_write_buf->buf;
        }

        write_len = dev->priv->cur_write_buf->buf + dev->priv->cur_write_buf->buf_used -
                    dev->priv->cur_write_buf_pos;
        n = sif->write(dev->priv->sin, dev->priv->cur_write_buf_pos, write_len);
        if (n <= 0) {
            if (dev->priv->during_write_to_device > 1) {
                dev->priv->during_write_to_device = 1;
                continue; /* a wakeup might have been called during the write -
                             make sure it doesn't get lost */
            }
            break;
        }
        total += n;
        write_len -= n;
        if (!write_len) {
            red_char_device_write_buffer_release(dev, &dev->priv->cur_write_buf);
            continue;
        }
        dev->priv->cur_write_buf_pos += n;
    }
    /* retry writing as long as the write queue is not empty */
    if (dev->priv->running) {
        if (dev->priv->cur_write_buf) {
            if (dev->priv->write_to_dev_timer) {
                reds_core_timer_start(dev->priv->reds, dev->priv->write_to_dev_timer,
                                      CHAR_DEVICE_WRITE_TO_TIMEOUT);
            }
        } else {
            spice_assert(g_queue_is_empty(&dev->priv->write_queue));
        }
        dev->priv->active = dev->priv->active || total;
    }
    dev->priv->during_write_to_device = 0;
    g_object_unref(dev);
    return total;
}

static void red_char_device_write_retry(void *opaque)
{
    RedCharDevice *dev = opaque;

    if (dev->priv->write_to_dev_timer) {
        reds_core_timer_cancel(dev->priv->reds, dev->priv->write_to_dev_timer);
    }
    red_char_device_write_to_device(dev);
}

static RedCharDeviceWriteBuffer *__red_char_device_write_buffer_get(RedCharDevice *   dev,
                                                                    RedClient *       client,
                                                                    int               size,
                                                                    WriteBufferOrigin origin,
                                                                    int migrated_data_tokens)
{
    RedCharDeviceWriteBuffer *ret;

    if (origin == WRITE_BUFFER_ORIGIN_SERVER && !dev->priv->num_self_tokens) {
        return NULL;
    }

    ret = g_queue_pop_tail(&dev->priv->write_bufs_pool);
    if (ret) {
        dev->priv->cur_pool_size -= ret->buf_size;
    } else {
        struct RedCharDeviceWriteBufferFull
        {
            RedCharDeviceWriteBuffer        buffer;
            RedCharDeviceWriteBufferPrivate priv;
        } * write_buf;
        write_buf = g_new0(struct RedCharDeviceWriteBufferFull, 1);
        ret       = &write_buf->buffer;
        ret->priv = &write_buf->priv;
    }

    spice_assert(!ret->buf_used);

    if (ret->buf_size < size) {
        ret->buf      = g_realloc(ret->buf, size);
        ret->buf_size = size;
    }
    ret->priv->origin = origin;

    if (origin == WRITE_BUFFER_ORIGIN_CLIENT) {
        spice_assert(client);
        RedCharDeviceClient *dev_client = red_char_device_client_find(dev, client);
        if (dev_client) {
            if (!migrated_data_tokens && dev_client->do_flow_control &&
                !dev_client->num_client_tokens) {
                spice_printerr("token violation: dev %p client %p", dev, client);
                red_char_device_handle_client_overflow(dev_client);
                goto error;
            }
            ret->priv->client = client;
            if (!migrated_data_tokens && dev_client->do_flow_control) {
                dev_client->num_client_tokens--;
            }
        } else {
            /* it is possible that the client was removed due to send tokens
             * underflow, but
             * the caller still receive messages from the client */
            spice_printerr("client not found: dev %p client %p", dev, client);
            goto error;
        }
    } else if (origin == WRITE_BUFFER_ORIGIN_SERVER) {
        dev->priv->num_self_tokens--;
    }

    ret->priv->token_price = migrated_data_tokens ? migrated_data_tokens : 1;
    ret->priv->refs        = 1;
    return ret;
error:
    dev->priv->cur_pool_size += ret->buf_size;
    g_queue_push_head(&dev->priv->write_bufs_pool, ret);
    return NULL;
}

RedCharDeviceWriteBuffer *
red_char_device_write_buffer_get(RedCharDevice *dev, RedClient *client, int size)
{
    return __red_char_device_write_buffer_get(
        dev, client, size, client ? WRITE_BUFFER_ORIGIN_CLIENT : WRITE_BUFFER_ORIGIN_SERVER, 0);
}

RedCharDeviceWriteBuffer *red_char_device_write_buffer_get_server_no_token(RedCharDevice *dev,
                                                                           int            size)
{
    return __red_char_device_write_buffer_get(dev, NULL, size, WRITE_BUFFER_ORIGIN_SERVER_NO_TOKEN,
                                              0);
}

static RedCharDeviceWriteBuffer *
red_char_device_write_buffer_ref(RedCharDeviceWriteBuffer *write_buf)
{
    spice_assert(write_buf);

    write_buf->priv->refs++;
    return write_buf;
}

static void red_char_device_write_buffer_unref(RedCharDeviceWriteBuffer *write_buf)
{
    spice_assert(write_buf);

    write_buf->priv->refs--;
    if (write_buf->priv->refs == 0)
        red_char_device_write_buffer_free(write_buf);
}

void red_char_device_write_buffer_add(RedCharDevice *dev, RedCharDeviceWriteBuffer *write_buf)
{
    spice_assert(dev);
    /* caller shouldn't add buffers for client that was removed */
    if (write_buf->priv->origin == WRITE_BUFFER_ORIGIN_CLIENT &&
        !red_char_device_client_find(dev, write_buf->priv->client)) {
        spice_printerr("client not found: dev %p client %p", dev, write_buf->priv->client);
        red_char_device_write_buffer_pool_add(dev, write_buf);
        return;
    }

    g_queue_push_head(&dev->priv->write_queue, write_buf);
    red_char_device_write_to_device(dev);
}

void red_char_device_write_buffer_release(RedCharDevice *            dev,
                                          RedCharDeviceWriteBuffer **p_write_buf)
{
    RedCharDeviceWriteBuffer *write_buf = *p_write_buf;
    if (!write_buf) {
        return;
    }
    *p_write_buf = NULL;

    WriteBufferOrigin buf_origin      = write_buf->priv->origin;
    uint32_t          buf_token_price = write_buf->priv->token_price;
    RedClient *       client          = write_buf->priv->client;

    if (!dev) {
        spice_printerr("no device. write buffer is freed");
        red_char_device_write_buffer_free(write_buf);
        return;
    }

    spice_assert(dev->priv->cur_write_buf != write_buf);

    red_char_device_write_buffer_pool_add(dev, write_buf);
    if (buf_origin == WRITE_BUFFER_ORIGIN_CLIENT) {
        RedCharDeviceClient *dev_client;

        spice_assert(client);
        dev_client = red_char_device_client_find(dev, client);
        /* when a client is removed, we remove all the buffers that are associated
         * with it */
        spice_assert(dev_client);
        red_char_device_client_tokens_add(dev, dev_client, buf_token_price);
    } else if (buf_origin == WRITE_BUFFER_ORIGIN_SERVER) {
        dev->priv->num_self_tokens++;
        red_char_device_on_free_self_token(dev);
    }
}

/********************************
 * char_device_state management *
 ********************************/

void red_char_device_reset_dev_instance(RedCharDevice *dev, SpiceCharDeviceInstance *sin)
{
    spice_debug("sin %p, char device %p", sin, dev);
    dev->priv->sin = sin;
    if (sin)
        sin->st = dev;
    g_object_notify(G_OBJECT(dev), "sin");
}

void red_char_device_destroy(RedCharDevice *char_dev)
{
    g_return_if_fail(RED_IS_CHAR_DEVICE(char_dev));
    g_object_unref(char_dev);
}

static RedCharDeviceClient *red_char_device_client_new(RedClient *client,
                                                       int        do_flow_control,
                                                       uint32_t   max_send_queue_size,
                                                       uint32_t   num_client_tokens,
                                                       uint32_t   num_send_tokens)
{
    RedCharDeviceClient *dev_client;

    dev_client                      = g_new0(RedCharDeviceClient, 1);
    dev_client->client              = client;
    dev_client->send_queue          = g_queue_new();
    dev_client->max_send_queue_size = max_send_queue_size;
    dev_client->do_flow_control     = do_flow_control;
    if (do_flow_control) {
        RedsState *reds = red_client_get_server(client);

        dev_client->wait_for_tokens_timer =
            reds_core_timer_add(reds, device_client_wait_for_tokens_timeout, dev_client);
        if (!dev_client->wait_for_tokens_timer) {
            spice_error("failed to create wait for tokens timer");
        }
        dev_client->num_client_tokens = num_client_tokens;
        dev_client->num_send_tokens   = num_send_tokens;
    } else {
        dev_client->num_client_tokens = ~0;
        dev_client->num_send_tokens   = ~0;
    }

    return dev_client;
}

bool red_char_device_client_add(RedCharDevice *dev,
                                RedClient *    client,
                                int            do_flow_control,
                                uint32_t       max_send_queue_size,
                                uint32_t       num_client_tokens,
                                uint32_t       num_send_tokens,
                                int            wait_for_migrate_data)
{
    RedCharDeviceClient *dev_client;

    spice_assert(dev);
    spice_assert(client);

    if (wait_for_migrate_data && (dev->priv->clients != NULL || dev->priv->active)) {
        spice_warning("can't restore device %p from migration data. The device "
                      "has already been active",
                      dev);
        return FALSE;
    }

    dev->priv->wait_for_migrate_data = wait_for_migrate_data;

    spice_debug("char device %p, client %p", dev, client);
    dev_client = red_char_device_client_new(client, do_flow_control, max_send_queue_size,
                                            num_client_tokens, num_send_tokens);
    dev_client->dev    = dev;
    dev->priv->clients = g_list_prepend(dev->priv->clients, dev_client);
    /* Now that we have a client, forward any pending device data */
    red_char_device_wakeup(dev);
    return TRUE;
}

void red_char_device_client_remove(RedCharDevice *dev, RedClient *client)
{
    RedCharDeviceClient *dev_client;

    spice_debug("char device %p, client %p", dev, client);
    dev_client = red_char_device_client_find(dev, client);

    if (!dev_client) {
        spice_error("client wasn't found");
        return;
    }
    red_char_device_client_free(dev, dev_client);
    if (dev->priv->wait_for_migrate_data) {
        spice_assert(dev->priv->clients == NULL);
        dev->priv->wait_for_migrate_data = FALSE;
        red_char_device_read_from_device(dev);
    }

    if (dev->priv->clients == NULL) {
        spice_debug("client removed, memory pool will be freed (%" PRIu64 " bytes)",
                    dev->priv->cur_pool_size);
        write_buffers_queue_free(&dev->priv->write_bufs_pool);
        dev->priv->cur_pool_size = 0;
    }
}

int red_char_device_client_exists(RedCharDevice *dev, RedClient *client)
{
    return (red_char_device_client_find(dev, client) != NULL);
}

void red_char_device_start(RedCharDevice *dev)
{
    spice_debug("char device %p", dev);
    dev->priv->running = TRUE;
    g_object_ref(dev);
    while (red_char_device_write_to_device(dev) || red_char_device_read_from_device(dev))
        ;
    g_object_unref(dev);
}

void red_char_device_stop(RedCharDevice *dev)
{
    spice_debug("char device %p", dev);
    dev->priv->running = FALSE;
    dev->priv->active  = FALSE;
    if (dev->priv->write_to_dev_timer) {
        reds_core_timer_cancel(dev->priv->reds, dev->priv->write_to_dev_timer);
    }
}

void red_char_device_reset(RedCharDevice *dev)
{
    GList *                   client_item;
    RedCharDeviceWriteBuffer *buf;

    dev->priv->wait_for_migrate_data = FALSE;
    spice_debug("char device %p", dev);
    while ((buf = g_queue_pop_tail(&dev->priv->write_queue))) {
        red_char_device_write_buffer_release(dev, &buf);
    }
    red_char_device_write_buffer_release(dev, &dev->priv->cur_write_buf);

    for (client_item = dev->priv->clients; client_item != NULL; client_item = client_item->next) {
        RedCharDeviceClient *dev_client = client_item->data;

        spice_debug("send_queue_empty %d", g_queue_is_empty(dev_client->send_queue));
        dev_client->num_send_tokens += g_queue_get_length(dev_client->send_queue);
        g_queue_foreach(dev_client->send_queue, (GFunc)red_pipe_item_unref, NULL);
        g_queue_clear(dev_client->send_queue);

        /* If device is reset, we must reset the tokens counters as well as we
         * don't hold any data from client and upon agent's reconnection we send
         * SPICE_MSG_MAIN_AGENT_CONNECTED_TOKENS with all free tokens we have */
        dev_client->num_client_tokens += dev_client->num_client_tokens_free;
        dev_client->num_client_tokens_free = 0;
    }
}

void red_char_device_wakeup(RedCharDevice *dev)
{
    red_char_device_write_to_device(dev);
    red_char_device_read_from_device(dev);
}

/*************
 * Migration *
 * **********/

void red_char_device_migrate_data_marshall_empty(SpiceMarshaller *m)
{
    SpiceMigrateDataCharDevice *mig_data;

    spice_debug("trace");
    mig_data = (SpiceMigrateDataCharDevice *)spice_marshaller_reserve_space(m, sizeof(*mig_data));
    memset(mig_data, 0, sizeof(*mig_data));
    mig_data->version   = SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION;
    mig_data->connected = FALSE;
}

static void migrate_data_marshaller_write_buffer_free(uint8_t *data, void *opaque)
{
    RedCharDeviceWriteBuffer *write_buf = (RedCharDeviceWriteBuffer *)opaque;

    red_char_device_write_buffer_unref(write_buf);
}

void red_char_device_migrate_data_marshall(RedCharDevice *dev, SpiceMarshaller *m)
{
    RedCharDeviceClient *dev_client;
    GList *              item;
    uint8_t *            write_to_dev_sizes_ptr;
    uint32_t             write_to_dev_size;
    uint32_t             write_to_dev_tokens;
    SpiceMarshaller *    m2;

    /* multi-clients are not supported */
    spice_assert(g_list_length(dev->priv->clients) == 1);
    dev_client = g_list_last(dev->priv->clients)->data;
    /* FIXME: if there were more than one client before the marshalling,
     * it is possible that the send_queue length > 0, and the send data
     * should be migrated as well */
    spice_assert(g_queue_is_empty(dev_client->send_queue));
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION);
    spice_marshaller_add_uint8(m, 1); /* connected */
    spice_marshaller_add_uint32(m, dev_client->num_client_tokens);
    spice_marshaller_add_uint32(m, dev_client->num_send_tokens);
    write_to_dev_sizes_ptr = spice_marshaller_reserve_space(m, sizeof(uint32_t) * 2);
    write_to_dev_size      = 0;
    write_to_dev_tokens    = 0;

    m2 = spice_marshaller_get_ptr_submarshaller(m, 0);
    if (dev->priv->cur_write_buf) {
        uint32_t buf_remaining = dev->priv->cur_write_buf->buf +
                                 dev->priv->cur_write_buf->buf_used - dev->priv->cur_write_buf_pos;
        spice_marshaller_add_by_ref_full(
            m2, dev->priv->cur_write_buf_pos, buf_remaining,
            migrate_data_marshaller_write_buffer_free,
            red_char_device_write_buffer_ref(dev->priv->cur_write_buf));
        write_to_dev_size += buf_remaining;
        if (dev->priv->cur_write_buf->priv->origin == WRITE_BUFFER_ORIGIN_CLIENT) {
            spice_assert(dev->priv->cur_write_buf->priv->client == dev_client->client);
            write_to_dev_tokens += dev->priv->cur_write_buf->priv->token_price;
        }
    }

    for (item = g_queue_peek_tail_link(&dev->priv->write_queue); item != NULL; item = item->prev) {
        RedCharDeviceWriteBuffer *write_buf = item->data;

        spice_marshaller_add_by_ref_full(m2, write_buf->buf, write_buf->buf_used,
                                         migrate_data_marshaller_write_buffer_free,
                                         red_char_device_write_buffer_ref(write_buf));
        write_to_dev_size += write_buf->buf_used;
        if (write_buf->priv->origin == WRITE_BUFFER_ORIGIN_CLIENT) {
            spice_assert(write_buf->priv->client == dev_client->client);
            write_to_dev_tokens += write_buf->priv->token_price;
        }
    }
    spice_debug("migration data dev %p: write_queue size %u tokens %u", dev, write_to_dev_size,
                write_to_dev_tokens);
    spice_marshaller_set_uint32(m, write_to_dev_sizes_ptr, write_to_dev_size);
    spice_marshaller_set_uint32(m, write_to_dev_sizes_ptr + sizeof(uint32_t), write_to_dev_tokens);
}

bool red_char_device_restore(RedCharDevice *dev, SpiceMigrateDataCharDevice *mig_data)
{
    RedCharDeviceClient *dev_client;
    uint32_t             client_tokens_window;

    spice_assert(g_list_length(dev->priv->clients) == 1 && dev->priv->wait_for_migrate_data);

    dev_client = g_list_last(dev->priv->clients)->data;
    if (mig_data->version > SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION) {
        spice_error("dev %p error: migration data version %u is bigger than self %u", dev,
                    mig_data->version, SPICE_MIGRATE_DATA_CHAR_DEVICE_VERSION);
        return FALSE;
    }
    spice_assert(!dev->priv->cur_write_buf && g_queue_is_empty(&dev->priv->write_queue));
    spice_assert(mig_data->connected);

    client_tokens_window          = dev_client->num_client_tokens; /* initial state of tokens */
    dev_client->num_client_tokens = mig_data->num_client_tokens;
    /* assumption: client_tokens_window stays the same across severs */
    dev_client->num_client_tokens_free =
        client_tokens_window - mig_data->num_client_tokens - mig_data->write_num_client_tokens;
    dev_client->num_send_tokens = mig_data->num_send_tokens;

    if (mig_data->write_size > 0) {
        if (mig_data->write_num_client_tokens) {
            dev->priv->cur_write_buf = __red_char_device_write_buffer_get(
                dev, dev_client->client, mig_data->write_size, WRITE_BUFFER_ORIGIN_CLIENT,
                mig_data->write_num_client_tokens);
        } else {
            dev->priv->cur_write_buf = __red_char_device_write_buffer_get(
                dev, NULL, mig_data->write_size, WRITE_BUFFER_ORIGIN_SERVER, 0);
        }
        /* the first write buffer contains all the data that was saved for migration
         */
        memcpy(dev->priv->cur_write_buf->buf,
               ((uint8_t *)mig_data) + mig_data->write_data_ptr - sizeof(SpiceMigrateDataHeader),
               mig_data->write_size);
        dev->priv->cur_write_buf->buf_used = mig_data->write_size;
        dev->priv->cur_write_buf_pos       = dev->priv->cur_write_buf->buf;
    }
    dev->priv->wait_for_migrate_data = FALSE;
    red_char_device_write_to_device(dev);
    red_char_device_read_from_device(dev);
    return TRUE;
}

SpiceServer *red_char_device_get_server(RedCharDevice *dev) { return dev->priv->reds; }

SpiceCharDeviceInterface *spice_char_device_get_interface(SpiceCharDeviceInstance *instance)
{
    return SPICE_UPCAST(SpiceCharDeviceInterface, instance->base.sif);
}

static void red_char_device_init_device_instance(RedCharDevice *self)
{
    SpiceCharDeviceInterface *sif;

    g_return_if_fail(self->priv->reds);

    reds_core_timer_remove(self->priv->reds, self->priv->write_to_dev_timer);
    self->priv->write_to_dev_timer = NULL;

    if (self->priv->sin == NULL) {
        return;
    }

    sif = spice_char_device_get_interface(self->priv->sin);
    if (sif->base.minor_version <= 2 || !(sif->flags & SPICE_CHAR_DEVICE_NOTIFY_WRITABLE)) {
        self->priv->write_to_dev_timer =
            reds_core_timer_add(self->priv->reds, red_char_device_write_retry, self);
        if (!self->priv->write_to_dev_timer) {
            spice_error("failed creating char dev write timer");
        }
    }

    self->priv->sin->st = self;
}

static void
red_char_device_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
    RedCharDevice *self = RED_CHAR_DEVICE(object);

    switch (property_id) {
    case PROP_CHAR_DEV_INSTANCE:
        g_value_set_pointer(value, self->priv->sin);
        break;
    case PROP_SPICE_SERVER:
        g_value_set_pointer(value, self->priv->reds);
        break;
    case PROP_CLIENT_TOKENS_INTERVAL:
        g_value_set_uint64(value, self->priv->client_tokens_interval);
        break;
    case PROP_SELF_TOKENS:
        g_value_set_uint64(value, self->priv->num_self_tokens);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void red_char_device_set_property(GObject *     object,
                                         guint         property_id,
                                         const GValue *value,
                                         GParamSpec *  pspec)
{
    RedCharDevice *self = RED_CHAR_DEVICE(object);

    switch (property_id) {
    case PROP_CHAR_DEV_INSTANCE:
        red_char_device_reset_dev_instance(self, g_value_get_pointer(value));
        break;
    case PROP_SPICE_SERVER:
        self->priv->reds = g_value_get_pointer(value);
        break;
    case PROP_CLIENT_TOKENS_INTERVAL:
        self->priv->client_tokens_interval = g_value_get_uint64(value);
        break;
    case PROP_SELF_TOKENS:
        self->priv->num_self_tokens = g_value_get_uint64(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void red_char_device_on_sin_changed(GObject *   object,
                                           GParamSpec *pspec G_GNUC_UNUSED,
                                           gpointer user_data G_GNUC_UNUSED)
{
    RedCharDevice *self = RED_CHAR_DEVICE(object);

    red_char_device_init_device_instance(self);
}

static void red_char_device_finalize(GObject *object)
{
    RedCharDevice *self = RED_CHAR_DEVICE(object);

    reds_core_timer_remove(self->priv->reds, self->priv->write_to_dev_timer);
    self->priv->write_to_dev_timer = NULL;

    write_buffers_queue_free(&self->priv->write_queue);
    write_buffers_queue_free(&self->priv->write_bufs_pool);
    self->priv->cur_pool_size = 0;
    red_char_device_write_buffer_free(self->priv->cur_write_buf);
    self->priv->cur_write_buf = NULL;

    while (self->priv->clients != NULL) {
        RedCharDeviceClient *dev_client = self->priv->clients->data;
        red_char_device_client_free(self, dev_client);
    }
    self->priv->running = FALSE;

    G_OBJECT_CLASS(red_char_device_parent_class)->finalize(object);
}

static void port_event_none(RedCharDevice *self G_GNUC_UNUSED, uint8_t event G_GNUC_UNUSED) {}

static void red_char_device_class_init(RedCharDeviceClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(RedCharDevicePrivate));

    object_class->get_property = red_char_device_get_property;
    object_class->set_property = red_char_device_set_property;
    object_class->finalize     = red_char_device_finalize;

    g_object_class_install_property(
        object_class, PROP_CHAR_DEV_INSTANCE,
        g_param_spec_pointer("sin", "sin", "Char device instance",
                             G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
    g_object_class_install_property(
        object_class, PROP_SPICE_SERVER,
        g_param_spec_pointer("spice-server", "spice-server", "RedsState instance",
                             G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
    g_object_class_install_property(
        object_class, PROP_CLIENT_TOKENS_INTERVAL,
        g_param_spec_uint64("client-tokens-interval", "client-tokens-interval",
                            "Client token interval", 0, G_MAXUINT64, 0,
                            G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));
    g_object_class_install_property(
        object_class, PROP_SELF_TOKENS,
        g_param_spec_uint64("self-tokens", "self-tokens", "Self tokens", 0, G_MAXUINT64, 0,
                            G_PARAM_STATIC_STRINGS | G_PARAM_READWRITE));

    klass->port_event = port_event_none;
}

SPICE_GNUC_VISIBLE void spice_server_port_event(SpiceCharDeviceInstance *sin, uint8_t event)
{
    if (sin->st == NULL) {
        spice_warning("no SpiceCharDeviceState attached to instance %p", sin);
        return;
    }

    RedCharDeviceClass *klass = RED_CHAR_DEVICE_GET_CLASS(sin->st);
    if (!klass) {
        // wrong object, a warning is already produced by RED_CHAR_DEVICE_GET_CLASS
        return;
    }

    return klass->port_event(sin->st, event);
}

static void red_char_device_init(RedCharDevice *self)
{
    self->priv = RED_CHAR_DEVICE_PRIVATE(self);

    g_queue_init(&self->priv->write_queue);
    g_queue_init(&self->priv->write_bufs_pool);

    g_signal_connect(self, "notify::sin", G_CALLBACK(red_char_device_on_sin_changed), NULL);
}
