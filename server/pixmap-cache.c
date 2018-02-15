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

#include "pixmap-cache.h"

int pixmap_cache_unlocked_set_lossy(PixmapCache *cache, uint64_t id, int lossy)
{
    NewCacheItem *item;

    item = cache->hash_table[BITS_CACHE_HASH_KEY(id)];

    while (item) {
        if (item->id == id) {
            item->lossy = lossy;
            break;
        }
        item = item->next;
    }
    return !!item;
}

void pixmap_cache_clear(PixmapCache *cache)
{
    NewCacheItem *item;

    if (cache->frozen) {
        cache->lru.next = cache->frozen_head;
        cache->lru.prev = cache->frozen_tail;
        cache->frozen   = FALSE;
    }

    SPICE_VERIFY(SPICE_OFFSETOF(NewCacheItem, lru_link) == 0);
    while ((item = (NewCacheItem *)ring_get_head(&cache->lru))) {
        ring_remove(&item->lru_link);
        g_free(item);
    }
    memset(cache->hash_table, 0, sizeof(*cache->hash_table) * BITS_CACHE_HASH_SIZE);

    cache->available = cache->size;
    cache->items     = 0;
}

bool pixmap_cache_freeze(PixmapCache *cache)
{
    pthread_mutex_lock(&cache->lock);

    if (cache->frozen) {
        pthread_mutex_unlock(&cache->lock);
        return FALSE;
    }

    cache->frozen_head = cache->lru.next;
    cache->frozen_tail = cache->lru.prev;
    ring_init(&cache->lru);
    memset(cache->hash_table, 0, sizeof(*cache->hash_table) * BITS_CACHE_HASH_SIZE);
    cache->available = -1;
    cache->frozen    = TRUE;

    pthread_mutex_unlock(&cache->lock);
    return TRUE;
}

static void pixmap_cache_destroy(PixmapCache *cache)
{
    spice_assert(cache);

    pthread_mutex_lock(&cache->lock);
    pixmap_cache_clear(cache);
    pthread_mutex_unlock(&cache->lock);
}

static pthread_mutex_t cache_lock        = PTHREAD_MUTEX_INITIALIZER;
static Ring            pixmap_cache_list = {&pixmap_cache_list, &pixmap_cache_list};

static PixmapCache *pixmap_cache_new(RedClient *client, uint8_t id, int64_t size)
{
    PixmapCache *cache = g_new0(PixmapCache, 1);

    ring_item_init(&cache->base);
    pthread_mutex_init(&cache->lock, NULL);
    cache->id   = id;
    cache->refs = 1;
    ring_init(&cache->lru);
    cache->available = size;
    cache->size      = size;
    cache->client    = client;

    return cache;
}

PixmapCache *pixmap_cache_get(RedClient *client, uint8_t id, int64_t size)
{
    PixmapCache *ret = NULL;
    RingItem *   now;
    pthread_mutex_lock(&cache_lock);

    now = &pixmap_cache_list;
    while ((now = ring_next(&pixmap_cache_list, now))) {
        PixmapCache *cache = SPICE_UPCAST(PixmapCache, now);
        if ((cache->client == client) && (cache->id == id)) {
            ret = cache;
            ret->refs++;
            break;
        }
    }
    if (!ret) {
        ret = pixmap_cache_new(client, id, size);
        ring_add(&pixmap_cache_list, &ret->base);
    }
    pthread_mutex_unlock(&cache_lock);
    return ret;
}

void pixmap_cache_unref(PixmapCache *cache)
{
    if (!cache)
        return;

    pthread_mutex_lock(&cache_lock);
    if (--cache->refs) {
        pthread_mutex_unlock(&cache_lock);
        return;
    }
    ring_remove(&cache->base);
    pthread_mutex_unlock(&cache_lock);
    pixmap_cache_destroy(cache);
    g_free(cache);
}
