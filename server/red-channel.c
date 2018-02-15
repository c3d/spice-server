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


    Author:
        yhalperi@redhat.com
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <common/ring.h>

#include "main-dispatcher.h"
#include "red-channel-client.h"
#include "red-channel.h"
#include "red-stream.h"
#include "reds.h"
#include "utils.h"

/*
 * Lifetime of RedChannel, RedChannelClient and RedClient:
 * RedChannel is created and destroyed by the calls to
 * red_channel_create.* and red_channel_destroy. The RedChannel resources
 * are deallocated only after red_channel_destroy is called and no
 * RedChannelClient
 * refers to the channel.
 * RedChannelClient is created and destroyed by the calls to
 * xxx_channel_client_new
 * and red_channel_client_destroy. RedChannelClient resources are deallocated
 * only when
 * its refs == 0. The reference count of RedChannelClient can be increased by
 * routines
 * that include calls that might destroy the red_channel_client. For example,
 * red_peer_handle_incoming calls the handle_message proc of the channel, which
 * might lead to destroying the client. However, after the call to
 * handle_message,
 * there is a call to the channel's release_msg_buf proc.
 *
 * Once red_channel_client_destroy is called, the RedChannelClient is
 * disconnected and
 * removed from the RedChannel clients list, but if rcc->refs != 0, it will
 * still hold
 * a reference to the Channel. The reason for this is that on the one hand
 * RedChannel holds
 * callbacks that may be still in use by RedChannel, and on the other hand,
 * when an operation is performed on the list of clients that belongs to the
 * channel,
 * we don't want to execute it on the "to be destroyed" channel client.
 *
 * RedClient is created and destroyed by the calls to red_client_new and
 * red_client_destroy.
 * When it is destroyed, it also disconnects and destroys all the
 * RedChannelClients that
 * are associated with it. However, since part of these channel clients may
 * still have
 * other references, they will not be completely released, until they are
 * dereferenced.
 *
 * Note: red_channel_client_destroy is not thread safe, and still it is called
 * from
 * red_client_destroy (from the client's thread). However, since before this
 * call,
 * red_client_destroy calls rcc->channel->client_cbs.disconnect(rcc), which is
 * synchronous,
 * we assume that if the channel is in another thread, it does no longer have
 * references to
 * this channel client.
 * If a call to red_channel_client_destroy is made from another location, it
 * must be called
 * from the channel's thread.
*/

G_DEFINE_ABSTRACT_TYPE(RedChannel, red_channel, G_TYPE_OBJECT)

#define CHANNEL_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE((o), RED_TYPE_CHANNEL, RedChannelPrivate))

struct RedChannelPrivate
{
    uint32_t type;
    uint32_t id;

    SpiceCoreInterfaceInternal *core;
    gboolean                    handle_acks;

    // RedChannel will hold only connected channel clients
    // (logic - when pushing pipe item to all channel clients, there
    // is no need to go over disconnect clients)
    // . While client will hold the channel clients till it is destroyed
    // and then it will destroy them as well.
    // However RCC still holds a reference to the Channel.
    // Maybe replace these logic with ref count?
    // TODO: rename to 'connected_clients'?
    GList *clients;

    RedChannelCapabilities local_caps;
    uint32_t               migration_flags;

    void *data;

    ClientCbs client_cbs;
    // TODO: when different channel_clients are in different threads
    // from Channel -> need to protect!
    pthread_t   thread_id;
    RedsState * reds;
    RedStatNode stat;
};

enum
{
    PROP0,
    PROP_SPICE_SERVER,
    PROP_CORE_INTERFACE,
    PROP_TYPE,
    PROP_ID,
    PROP_HANDLE_ACKS,
    PROP_MIGRATION_FLAGS
};

static void
red_channel_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
    RedChannel *self = RED_CHANNEL(object);

    switch (property_id) {
    case PROP_SPICE_SERVER:
        g_value_set_pointer(value, self->priv->reds);
        break;
    case PROP_CORE_INTERFACE:
        g_value_set_pointer(value, self->priv->core);
        break;
    case PROP_TYPE:
        g_value_set_int(value, self->priv->type);
        break;
    case PROP_ID:
        g_value_set_uint(value, self->priv->id);
        break;
    case PROP_HANDLE_ACKS:
        g_value_set_boolean(value, self->priv->handle_acks);
        break;
    case PROP_MIGRATION_FLAGS:
        g_value_set_uint(value, self->priv->migration_flags);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
red_channel_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
    RedChannel *self = RED_CHANNEL(object);

    switch (property_id) {
    case PROP_SPICE_SERVER:
        self->priv->reds = g_value_get_pointer(value);
        break;
    case PROP_CORE_INTERFACE:
        self->priv->core = g_value_get_pointer(value);
        break;
    case PROP_TYPE:
        self->priv->type = g_value_get_int(value);
        break;
    case PROP_ID:
        self->priv->id = g_value_get_uint(value);
        break;
    case PROP_HANDLE_ACKS:
        self->priv->handle_acks = g_value_get_boolean(value);
        break;
    case PROP_MIGRATION_FLAGS:
        self->priv->migration_flags = g_value_get_uint(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void red_channel_finalize(GObject *object)
{
    RedChannel *self = RED_CHANNEL(object);

    red_channel_capabilities_reset(&self->priv->local_caps);

    G_OBJECT_CLASS(red_channel_parent_class)->finalize(object);
}

static void red_channel_constructed(GObject *object)
{
    RedChannel *self = RED_CHANNEL(object);

    red_channel_debug(self, "thread_id 0x%lx", self->priv->thread_id);

    RedChannelClass *klass = RED_CHANNEL_GET_CLASS(self);

    G_OBJECT_CLASS(red_channel_parent_class)->constructed(object);

    spice_assert(klass->handle_migrate_data ||
                 !(self->priv->migration_flags & SPICE_MIGRATE_NEED_DATA_TRANSFER));
}

static void red_channel_client_default_connect(RedChannel *            channel,
                                               RedClient *             client,
                                               RedStream *             stream,
                                               int                     migration,
                                               RedChannelCapabilities *caps)
{
    spice_error("not implemented");
}

static void red_channel_client_default_disconnect(RedChannelClient *base)
{
    red_channel_client_disconnect(base);
}

static void red_channel_class_init(RedChannelClass *klass)
{
    GParamSpec *  spec;
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    g_type_class_add_private(klass, sizeof(RedChannelPrivate));

    object_class->get_property = red_channel_get_property;
    object_class->set_property = red_channel_set_property;
    object_class->finalize     = red_channel_finalize;
    object_class->constructed  = red_channel_constructed;

    spec = g_param_spec_pointer(
        "spice-server", "spice-server", "The spice server associated with this channel",
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_SPICE_SERVER, spec);

    spec =
        g_param_spec_pointer("core-interface", "core-interface",
                             "The SpiceCoreInterface server associated with this channel",
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_CORE_INTERFACE, spec);

    /* FIXME: generate enums for this in spice-common? */
    spec = g_param_spec_int("channel-type", "channel type", "Type of this channel", 0,
                            SPICE_END_CHANNEL, 0,
                            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_TYPE, spec);

    spec = g_param_spec_uint("id", "id", "ID of this channel", 0, G_MAXUINT, 0,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_ID, spec);

    spec = g_param_spec_boolean("handle-acks", "Handle ACKs", "Whether this channel handles ACKs",
                                FALSE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                                           G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_HANDLE_ACKS, spec);

    spec = g_param_spec_uint("migration-flags", "migration flags",
                             "Migration flags for this channel", 0, G_MAXUINT, 0,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property(object_class, PROP_MIGRATION_FLAGS, spec);
}

static void red_channel_init(RedChannel *self)
{
    self->priv = CHANNEL_PRIVATE(self);

    red_channel_set_common_cap(self, SPICE_COMMON_CAP_MINI_HEADER);
    self->priv->thread_id = pthread_self();

    self->priv->client_cbs.connect    = red_channel_client_default_connect;
    self->priv->client_cbs.disconnect = red_channel_client_default_disconnect;
    self->priv->client_cbs.migrate    = red_channel_client_default_migrate;
}

void red_channel_receive(RedChannel *channel)
{
    g_list_foreach(channel->priv->clients, (GFunc)red_channel_client_receive, NULL);
}

void red_channel_add_client(RedChannel *channel, RedChannelClient *rcc)
{
    spice_assert(rcc);
    channel->priv->clients = g_list_prepend(channel->priv->clients, rcc);
}

bool red_channel_test_remote_cap(RedChannel *channel, uint32_t cap)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(channel, rcc)
    {
        if (!red_channel_client_test_remote_cap(rcc, cap)) {
            return FALSE;
        }
    }
    return TRUE;
}

bool red_channel_is_waiting_for_migrate_data(RedChannel *channel)
{
    RedChannelClient *rcc;
    guint             n_clients = g_list_length(channel->priv->clients);

    if (!red_channel_is_connected(channel)) {
        return FALSE;
    }

    if (n_clients > 1) {
        return FALSE;
    }
    spice_assert(n_clients == 1);
    rcc = g_list_nth_data(channel->priv->clients, 0);
    return red_channel_client_is_waiting_for_migrate_data(rcc);
}

void red_channel_init_stat_node(RedChannel *channel, const RedStatNode *parent, const char *name)
{
    spice_return_if_fail(channel != NULL);

    // TODO check not already initialized
    stat_init_node(&channel->priv->stat, channel->priv->reds, parent, name, TRUE);
}

const RedStatNode *red_channel_get_stat_node(RedChannel *channel) { return &channel->priv->stat; }

void red_channel_register_client_cbs(RedChannel *     channel,
                                     const ClientCbs *client_cbs,
                                     gpointer         cbs_data)
{
    spice_assert(client_cbs->connect || channel->priv->type == SPICE_CHANNEL_MAIN);
    channel->priv->client_cbs.connect = client_cbs->connect;

    if (client_cbs->disconnect) {
        channel->priv->client_cbs.disconnect = client_cbs->disconnect;
    }

    if (client_cbs->migrate) {
        channel->priv->client_cbs.migrate = client_cbs->migrate;
    }
    channel->priv->data = cbs_data;
}

static void add_capability(uint32_t **caps, int *num_caps, uint32_t cap)
{
    int nbefore, n;

    nbefore   = *num_caps;
    n         = cap / 32;
    *num_caps = MAX(*num_caps, n + 1);
    *caps     = spice_renew(uint32_t, *caps, *num_caps);
    memset(*caps + nbefore, 0, (*num_caps - nbefore) * sizeof(uint32_t));
    (*caps)[n] |= (1 << (cap % 32));
}

void red_channel_set_common_cap(RedChannel *channel, uint32_t cap)
{
    add_capability(&channel->priv->local_caps.common_caps,
                   &channel->priv->local_caps.num_common_caps, cap);
}

void red_channel_set_cap(RedChannel *channel, uint32_t cap)
{
    add_capability(&channel->priv->local_caps.caps, &channel->priv->local_caps.num_caps, cap);
}

void red_channel_destroy(RedChannel *channel)
{
    if (!channel) {
        return;
    }

    // prevent future connection
    reds_unregister_channel(channel->priv->reds, channel);

    g_list_foreach(channel->priv->clients, (GFunc)red_channel_client_destroy, NULL);
    g_object_unref(channel);
}

void red_channel_send(RedChannel *channel)
{
    g_list_foreach(channel->priv->clients, (GFunc)red_channel_client_send, NULL);
}

void red_channel_push(RedChannel *channel)
{
    if (!channel) {
        return;
    }

    g_list_foreach(channel->priv->clients, (GFunc)red_channel_client_push, NULL);
}

void red_channel_pipes_add(RedChannel *channel, RedPipeItem *item)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(channel, rcc)
    {
        red_pipe_item_ref(item);
        red_channel_client_pipe_add(rcc, item);
    }

    red_pipe_item_unref(item);
}

void red_channel_pipes_add_type(RedChannel *channel, int pipe_item_type)
{
    RedPipeItem *item = g_new(RedPipeItem, 1);

    red_pipe_item_init(item, pipe_item_type);

    red_channel_pipes_add(channel, item);
}

void red_channel_pipes_add_empty_msg(RedChannel *channel, int msg_type)
{
    red_channel_pipes_add(channel, red_channel_client_new_empty_msg(msg_type));
}

int red_channel_is_connected(RedChannel *channel) { return channel && channel->priv->clients; }

const char *red_channel_get_name(RedChannel *channel)
{
    return red_channel_type_to_str(channel->priv->type);
}

void red_channel_remove_client(RedChannel *channel, RedChannelClient *rcc)
{
    GList *link;
    g_return_if_fail(channel == red_channel_client_get_channel(rcc));

    if (!pthread_equal(pthread_self(), channel->priv->thread_id)) {
        red_channel_warning(channel, "channel->thread_id (0x%lx) != pthread_self (0x%lx)."
                                     "If one of the threads is != io-thread && != vcpu-thread, "
                                     "this might be a BUG",
                            channel->priv->thread_id, pthread_self());
    }
    spice_return_if_fail(channel);
    link = g_list_find(channel->priv->clients, rcc);
    spice_return_if_fail(link != NULL);

    channel->priv->clients = g_list_delete_link(channel->priv->clients, link);
    // TODO: should we set rcc->channel to NULL???
}

void red_channel_disconnect(RedChannel *channel)
{
    g_list_foreach(channel->priv->clients, (GFunc)red_channel_client_disconnect, NULL);
}

void red_channel_connect(RedChannel *            channel,
                         RedClient *             client,
                         RedStream *             stream,
                         int                     migration,
                         RedChannelCapabilities *caps)
{
    channel->priv->client_cbs.connect(channel, client, stream, migration, caps);
}

GList *red_channel_get_clients(RedChannel *channel) { return channel->priv->clients; }
guint red_channel_get_n_clients(RedChannel *channel)
{
    return g_list_length(channel->priv->clients);
}

bool red_channel_all_blocked(RedChannel *channel)
{
    RedChannelClient *rcc;

    if (!channel || !channel->priv->clients) {
        return FALSE;
    }
    FOREACH_CLIENT(channel, rcc)
    {
        if (!red_channel_client_is_blocked(rcc)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* return TRUE if any of the connected clients to this channel are blocked */
static bool red_channel_any_blocked(RedChannel *channel)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(channel, rcc)
    {
        if (red_channel_client_is_blocked(rcc)) {
            return TRUE;
        }
    }
    return FALSE;
}

static bool red_channel_no_item_being_sent(RedChannel *channel)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(channel, rcc)
    {
        if (!red_channel_client_no_item_being_sent(rcc)) {
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * Functions to push the same item to multiple pipes.
 */

/*
 * TODO: after convinced of correctness, add paths for single client
 * that avoid the whole loop. perhaps even have a function pointer table
 * later.
 * TODO - inline? macro? right now this is the simplest from code amount
 */

typedef void (*rcc_item_t)(RedChannelClient *rcc, RedPipeItem *item);

/**
 * red_channel_pipes_create_batch:
 * @channel: a channel
 * @creator: a callback to create pipe item (not null)
 * @data: the data to pass to the creator
 * @pipe_add: a callback to add non-null pipe items (not null)
 *
 * Returns: the number of added items
 **/
static int red_channel_pipes_create_batch(RedChannel *    channel,
                                          new_pipe_item_t creator,
                                          void *          data,
                                          rcc_item_t      pipe_add)
{
    RedChannelClient *rcc;
    RedPipeItem *     item;
    int               num = 0, n = 0;

    spice_assert(creator != NULL);
    spice_assert(pipe_add != NULL);

    FOREACH_CLIENT(channel, rcc)
    {
        item = (*creator)(rcc, data, num++);
        if (item) {
            (*pipe_add)(rcc, item);
            n++;
        }
    }

    return n;
}

int red_channel_pipes_new_add(RedChannel *channel, new_pipe_item_t creator, void *data)
{
    return red_channel_pipes_create_batch(channel, creator, data, red_channel_client_pipe_add);
}

uint32_t red_channel_max_pipe_size(RedChannel *channel)
{
    RedChannelClient *rcc;
    uint32_t          pipe_size = 0;

    FOREACH_CLIENT(channel, rcc)
    {
        uint32_t new_size;
        new_size  = red_channel_client_get_pipe_size(rcc);
        pipe_size = MAX(pipe_size, new_size);
    }
    return pipe_size;
}

uint32_t red_channel_sum_pipes_size(RedChannel *channel)
{
    RedChannelClient *rcc;
    uint32_t          sum = 0;

    FOREACH_CLIENT(channel, rcc) { sum += red_channel_client_get_pipe_size(rcc); }
    return sum;
}

static void red_channel_disconnect_if_pending_send(RedChannel *channel)
{
    RedChannelClient *rcc;

    FOREACH_CLIENT(channel, rcc)
    {
        if (red_channel_client_is_blocked(rcc) || !red_channel_client_pipe_is_empty(rcc)) {
            red_channel_client_disconnect(rcc);
        } else {
            spice_assert(red_channel_client_no_item_being_sent(rcc));
        }
    }
}

bool red_channel_wait_all_sent(RedChannel *channel, int64_t timeout)
{
    uint64_t end_time;
    uint32_t max_pipe_size;
    int      blocked = FALSE;

    if (timeout != -1) {
        end_time = spice_get_monotonic_time_ns() + timeout;
    } else {
        end_time = UINT64_MAX;
    }

    red_channel_push(channel);
    while (((max_pipe_size = red_channel_max_pipe_size(channel)) ||
            (blocked = red_channel_any_blocked(channel))) &&
           (timeout == -1 || spice_get_monotonic_time_ns() < end_time)) {
        spice_debug("pipe-size %u blocked %d", max_pipe_size, blocked);
        usleep(CHANNEL_BLOCKED_SLEEP_DURATION);
        red_channel_receive(channel);
        red_channel_send(channel);
        red_channel_push(channel);
    }

    if (max_pipe_size || blocked) {
        spice_warning("timeout: pending out messages exist (pipe-size %u, blocked %d)",
                      max_pipe_size, blocked);
        red_channel_disconnect_if_pending_send(channel);
        return FALSE;
    } else {
        spice_assert(red_channel_no_item_being_sent(channel));
        return TRUE;
    }
}

RedsState *red_channel_get_server(RedChannel *channel) { return channel->priv->reds; }

SpiceCoreInterfaceInternal *red_channel_get_core_interface(RedChannel *channel)
{
    return channel->priv->core;
}

void red_channel_send_item(RedChannel *self, RedChannelClient *rcc, RedPipeItem *item)
{
    RedChannelClass *klass = RED_CHANNEL_GET_CLASS(self);
    g_return_if_fail(klass->send_item);

    klass->send_item(rcc, item);
}

void red_channel_reset_thread_id(RedChannel *self) { self->priv->thread_id = pthread_self(); }

const RedChannelCapabilities *red_channel_get_local_capabilities(RedChannel *self)
{
    return &self->priv->local_caps;
}

void red_channel_migrate_client(RedChannel *channel, RedChannelClient *rcc)
{
    channel->priv->client_cbs.migrate(rcc);
}

void red_channel_disconnect_client(RedChannel *channel, RedChannelClient *rcc)
{
    channel->priv->client_cbs.disconnect(rcc);
}
