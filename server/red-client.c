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
#endif /* HAVE_CONFIG_H */

#include "red-channel.h"
#include "red-client.h"
#include "reds.h"

#define FOREACH_CHANNEL_CLIENT(_client, _data)                                                     \
    GLIST_FOREACH((_client ? (_client)->channels : NULL), RedChannelClient, _data)

struct RedClient
{
    GObject            parent;
    RedsState *        reds;
    GList *            channels;
    MainChannelClient *mcc;
    pthread_mutex_t    lock; // different channels can be in different threads

    pthread_t thread_id;

    int disconnecting;
    /* Note that while semi-seamless migration is conducted by the main thread,
     * seamless migration
     * involves all channels, and thus the related variables can be accessed from
     * different
     * threads */
    /* if seamless=TRUE, migration_target is turned off when all
     * the clients received their migration data. Otherwise (semi-seamless),
     * it is turned off, when red_client_semi_seamless_migrate_complete
     * is called */
    int during_target_migrate;
    int seamless_migrate;
    int num_migrated_channels; /* for seamless - number of channels that wait for
                                  migrate data*/
};

struct RedClientClass
{
    GObjectClass parent_class;
};

G_DEFINE_TYPE(RedClient, red_client, G_TYPE_OBJECT)

enum
{
    PROP0,
    PROP_SPICE_SERVER,
    PROP_MIGRATED
};

static void
red_client_get_property(GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
    RedClient *self = RED_CLIENT(object);

    switch (property_id) {
    case PROP_SPICE_SERVER:
        g_value_set_pointer(value, self->reds);
        break;
    case PROP_MIGRATED:
        g_value_set_boolean(value, self->during_target_migrate);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void
red_client_set_property(GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
    RedClient *self = RED_CLIENT(object);

    switch (property_id) {
    case PROP_SPICE_SERVER:
        self->reds = g_value_get_pointer(value);
        break;
    case PROP_MIGRATED:
        self->during_target_migrate = g_value_get_boolean(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
    }
}

static void red_client_finalize(GObject *object)
{
    RedClient *self = RED_CLIENT(object);

    spice_debug("release client=%p", self);
    pthread_mutex_destroy(&self->lock);

    G_OBJECT_CLASS(red_client_parent_class)->finalize(object);
}

static void red_client_class_init(RedClientClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->get_property = red_client_get_property;
    object_class->set_property = red_client_set_property;
    object_class->finalize     = red_client_finalize;

    g_object_class_install_property(
        object_class, PROP_SPICE_SERVER,
        g_param_spec_pointer("spice-server", "Spice server", "The Spice Server",
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
    g_object_class_install_property(
        object_class, PROP_MIGRATED,
        g_param_spec_boolean("migrated", "migrated", "Whether this client was migrated", FALSE,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
}

static void red_client_init(RedClient *self)
{
    pthread_mutex_init(&self->lock, NULL);
    self->thread_id = pthread_self();
}

RedClient *red_client_new(RedsState *reds, int migrated)
{
    return g_object_new(RED_TYPE_CLIENT, "spice-server", reds, "migrated", migrated, NULL);
}

void red_client_set_migration_seamless(RedClient *client) // dest
{
    RedChannelClient *rcc;

    spice_assert(client->during_target_migrate);
    pthread_mutex_lock(&client->lock);
    client->seamless_migrate = TRUE;
    /* update channel clients that got connected before the migration
     * type was set. red_client_add_channel will handle newer channel clients */
    FOREACH_CHANNEL_CLIENT(client, rcc)
    {
        if (red_channel_client_set_migration_seamless(rcc)) {
            client->num_migrated_channels++;
        }
    }
    pthread_mutex_unlock(&client->lock);
}

void red_client_migrate(RedClient *client)
{
    RedChannelClient *rcc;
    RedChannel *      channel;

    spice_printerr("migrate client with #channels %d", g_list_length(client->channels));
    if (!pthread_equal(pthread_self(), client->thread_id)) {
        spice_warning("client->thread_id (0x%lx) != pthread_self (0x%lx)."
                      "If one of the threads is != io-thread && != vcpu-thread,"
                      " this might be a BUG",
                      client->thread_id, pthread_self());
    }
    FOREACH_CHANNEL_CLIENT(client, rcc)
    {
        if (red_channel_client_is_connected(rcc)) {
            channel = red_channel_client_get_channel(rcc);
            red_channel_migrate_client(channel, rcc);
        }
    }
}

void red_client_destroy(RedClient *client)
{
    RedChannelClient *rcc;

    spice_printerr("destroy client %p with #channels=%d", client, g_list_length(client->channels));
    if (!pthread_equal(pthread_self(), client->thread_id)) {
        spice_warning("client->thread_id (0x%lx) != pthread_self (0x%lx)."
                      "If one of the threads is != io-thread && != vcpu-thread,"
                      " this might be a BUG",
                      client->thread_id, pthread_self());
    }
    red_client_set_disconnecting(client);
    FOREACH_CHANNEL_CLIENT(client, rcc)
    {
        RedChannel *channel;
        // some channels may be in other threads, so disconnection
        // is not synchronous.
        channel = red_channel_client_get_channel(rcc);
        red_channel_client_set_destroying(rcc);
        // some channels may be in other threads. However we currently
        // assume disconnect is synchronous (we changed the dispatcher
        // to wait for disconnection)
        // TODO: should we go back to async. For this we need to use
        // ref count for channel clients.
        red_channel_disconnect_client(channel, rcc);
        spice_assert(red_channel_client_pipe_is_empty(rcc));
        spice_assert(red_channel_client_no_item_being_sent(rcc));
        red_channel_client_destroy(rcc);
    }
    g_object_unref(client);
}

/* client->lock should be locked */
static RedChannelClient *red_client_get_channel(RedClient *client, int type, int id)
{
    RedChannelClient *rcc;
    RedChannelClient *ret = NULL;

    FOREACH_CHANNEL_CLIENT(client, rcc)
    {
        int         channel_type, channel_id;
        RedChannel *channel;

        channel = red_channel_client_get_channel(rcc);
        g_object_get(channel, "channel-type", &channel_type, "id", &channel_id, NULL);
        if (channel_type == type && channel_id == id) {
            ret = rcc;
            break;
        }
    }
    return ret;
}

gboolean red_client_add_channel(RedClient *client, RedChannelClient *rcc, GError **error)
{
    uint32_t    type, id;
    RedChannel *channel;
    gboolean    result = TRUE;

    spice_assert(rcc && client);
    channel = red_channel_client_get_channel(rcc);

    pthread_mutex_lock(&client->lock);

    g_object_get(channel, "channel-type", &type, "id", &id, NULL);
    if (client->disconnecting) {
        g_set_error(error, SPICE_SERVER_ERROR, SPICE_SERVER_ERROR_FAILED,
                    "Client %p got disconnected while connecting channel type %d id %d", client,
                    type, id);
        result = FALSE;
        goto cleanup;
    }

    if (red_client_get_channel(client, type, id)) {
        g_set_error(error, SPICE_SERVER_ERROR, SPICE_SERVER_ERROR_FAILED,
                    "Client %p: duplicate channel type %d id %d", client, type, id);
        result = FALSE;
        goto cleanup;
    }

    client->channels = g_list_prepend(client->channels, rcc);
    if (client->during_target_migrate && client->seamless_migrate) {
        if (red_channel_client_set_migration_seamless(rcc)) {
            client->num_migrated_channels++;
        }
    }

cleanup:
    pthread_mutex_unlock(&client->lock);
    return result;
}

MainChannelClient *red_client_get_main(RedClient *client) { return client->mcc; }

void red_client_set_main(RedClient *client, MainChannelClient *mcc) { client->mcc = mcc; }

void red_client_semi_seamless_migrate_complete(RedClient *client)
{
    RedChannelClient *rcc;

    pthread_mutex_lock(&client->lock);
    if (!client->during_target_migrate || client->seamless_migrate) {
        spice_error("unexpected");
        pthread_mutex_unlock(&client->lock);
        return;
    }
    client->during_target_migrate = FALSE;
    FOREACH_CHANNEL_CLIENT(client, rcc)
    {
        red_channel_client_semi_seamless_migration_complete(rcc);
    }
    pthread_mutex_unlock(&client->lock);
    reds_on_client_semi_seamless_migrate_complete(client->reds, client);
}

/* should be called only from the main thread */
int red_client_during_migrate_at_target(RedClient *client)
{
    int ret;
    pthread_mutex_lock(&client->lock);
    ret = client->during_target_migrate;
    pthread_mutex_unlock(&client->lock);
    return ret;
}

void red_client_remove_channel(RedChannelClient *rcc)
{
    RedClient *client = red_channel_client_get_client(rcc);
    pthread_mutex_lock(&client->lock);
    client->channels = g_list_remove(client->channels, rcc);
    pthread_mutex_unlock(&client->lock);
}

/* returns TRUE If all channels are finished migrating, FALSE otherwise */
gboolean red_client_seamless_migration_done_for_channel(RedClient *client)
{
    gboolean ret = FALSE;

    pthread_mutex_lock(&client->lock);
    client->num_migrated_channels--;
    /* we assume we always have at least one channel who has migration data
     * transfer,
     * otherwise, this flag will never be set back to FALSE*/
    if (!client->num_migrated_channels) {
        client->during_target_migrate = FALSE;
        client->seamless_migrate      = FALSE;
        /* migration completion might have been triggered from a different thread
         * than the main thread */
        main_dispatcher_seamless_migrate_dst_complete(reds_get_main_dispatcher(client->reds),
                                                      client);
        ret = TRUE;
    }
    pthread_mutex_unlock(&client->lock);

    return ret;
}

gboolean red_client_is_disconnecting(RedClient *client)
{
    gboolean ret;
    pthread_mutex_lock(&client->lock);
    ret = client->disconnecting;
    pthread_mutex_unlock(&client->lock);
    return ret;
}

void red_client_set_disconnecting(RedClient *client)
{
    pthread_mutex_lock(&client->lock);
    client->disconnecting = TRUE;
    pthread_mutex_unlock(&client->lock);
}

RedsState *red_client_get_server(RedClient *client) { return client->reds; }
