/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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

#include <common/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spice/stats.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "stat-file.h"

#define STAT_SHM_SIZE(max_nodes) (sizeof(SpiceStat) + (max_nodes) * sizeof(SpiceStatNode))

struct RedStatFile
{
    char *          shm_name;
    SpiceStat *     stat;
    pthread_mutex_t lock;
    unsigned int    max_nodes;
};

RedStatFile *stat_file_new(unsigned int max_nodes)
{
    int          fd;
    size_t       shm_size  = STAT_SHM_SIZE(max_nodes);
    RedStatFile *stat_file = g_new0(RedStatFile, 1);

    stat_file->max_nodes = max_nodes;
    stat_file->shm_name  = g_strdup_printf(SPICE_STAT_SHM_NAME, getpid());
    shm_unlink(stat_file->shm_name);
    if ((fd = shm_open(stat_file->shm_name, O_CREAT | O_RDWR, 0444)) == -1) {
        spice_error("statistics shm_open failed, %s", strerror(errno));
        goto cleanup;
    }
    if (ftruncate(fd, shm_size) == -1) {
        close(fd);
        spice_error("statistics ftruncate failed, %s", strerror(errno));
        goto cleanup;
    }
    stat_file->stat = (SpiceStat *)mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (stat_file->stat == (SpiceStat *)MAP_FAILED) {
        spice_error("statistics mmap failed, %s", strerror(errno));
        goto cleanup;
    }
    memset(stat_file->stat, 0, shm_size);
    stat_file->stat->magic      = SPICE_STAT_MAGIC;
    stat_file->stat->version    = SPICE_STAT_VERSION;
    stat_file->stat->root_index = INVALID_STAT_REF;
    if (pthread_mutex_init(&stat_file->lock, NULL)) {
        spice_error("mutex init failed");
        goto cleanup;
    }
    return stat_file;

cleanup:
    g_free(stat_file);
    return NULL;
}

void stat_file_free(RedStatFile *stat_file)
{
    if (!stat_file) {
        return;
    }

    stat_file_unlink(stat_file);
/* TODO other part of the code is not ready for this! */
#if 0
    size_t shm_size = STAT_SHM_SIZE(stat_file->max_nodes);
    munmap(stat_file->stat, shm_size);
#endif

    pthread_mutex_destroy(&stat_file->lock);
    g_free(stat_file);
}

const char *stat_file_get_shm_name(RedStatFile *stat_file) { return stat_file->shm_name; }

void stat_file_unlink(RedStatFile *stat_file)
{
    if (stat_file->shm_name) {
        shm_unlink(stat_file->shm_name);
        g_free(stat_file->shm_name);
        stat_file->shm_name = NULL;
    }
}

static void reds_insert_stat_node(RedStatFile *stat_file, StatNodeRef parent, StatNodeRef ref)
{
    SpiceStatNode *node = &stat_file->stat->nodes[ref];
    uint32_t       pos  = INVALID_STAT_REF;
    uint32_t       node_index;
    uint32_t *     head;
    SpiceStatNode *n;

    node->first_child_index = INVALID_STAT_REF;
    head                    = (parent == INVALID_STAT_REF ? &stat_file->stat->root_index
                                       : &stat_file->stat->nodes[parent].first_child_index);
    node_index = *head;
    while (node_index != INVALID_STAT_REF && (n = &stat_file->stat->nodes[node_index]) &&
           strcmp(node->name, n->name) > 0) {
        pos        = node_index;
        node_index = n->next_sibling_index;
    }
    if (pos == INVALID_STAT_REF) {
        node->next_sibling_index = *head;
        *head                    = ref;
    } else {
        n                        = &stat_file->stat->nodes[pos];
        node->next_sibling_index = n->next_sibling_index;
        n->next_sibling_index    = ref;
    }
}

StatNodeRef
stat_file_add_node(RedStatFile *stat_file, StatNodeRef parent, const char *name, int visible)
{
    StatNodeRef    ref;
    SpiceStatNode *node;

    spice_assert(name && strlen(name) > 0);
    if (strlen(name) >= sizeof(node->name)) {
        return INVALID_STAT_REF;
    }
    pthread_mutex_lock(&stat_file->lock);
    ref = (parent == INVALID_STAT_REF ? stat_file->stat->root_index
                                      : stat_file->stat->nodes[parent].first_child_index);
    while (ref != INVALID_STAT_REF) {
        node = &stat_file->stat->nodes[ref];
        if (strcmp(name, node->name)) {
            ref = node->next_sibling_index;
        } else {
            pthread_mutex_unlock(&stat_file->lock);
            return ref;
        }
    }
    for (ref = 0; ref < stat_file->max_nodes; ref++) {
        node = &stat_file->stat->nodes[ref];
        if (!!(node->flags & SPICE_STAT_NODE_FLAG_ENABLED)) {
            continue;
        }
        stat_file->stat->generation++;
        stat_file->stat->num_of_nodes++;
        node->value = 0;
        node->flags = SPICE_STAT_NODE_FLAG_ENABLED | (visible ? SPICE_STAT_NODE_FLAG_VISIBLE : 0);
        g_strlcpy(node->name, name, sizeof(node->name));
        reds_insert_stat_node(stat_file, parent, ref);
        pthread_mutex_unlock(&stat_file->lock);
        return ref;
    }
    pthread_mutex_unlock(&stat_file->lock);
    return INVALID_STAT_REF;
}

uint64_t *
stat_file_add_counter(RedStatFile *stat_file, StatNodeRef parent, const char *name, int visible)
{
    StatNodeRef    ref = stat_file_add_node(stat_file, parent, name, visible);
    SpiceStatNode *node;

    if (ref == INVALID_STAT_REF) {
        return NULL;
    }
    node = &stat_file->stat->nodes[ref];
    __sync_or_and_fetch(&node->flags, SPICE_STAT_NODE_FLAG_VALUE);
    return &node->value;
}

static void stat_file_remove(RedStatFile *stat_file, SpiceStatNode *node)
{
    const StatNodeRef node_ref  = node - stat_file->stat->nodes;
    const StatNodeRef node_next = node->next_sibling_index;
    StatNodeRef       ref;

    pthread_mutex_lock(&stat_file->lock);
    node->flags &= ~SPICE_STAT_NODE_FLAG_ENABLED;
    stat_file->stat->generation++;
    stat_file->stat->num_of_nodes--;
    /* remove links from parent or siblings */
    /* children will be orphans */
    if (stat_file->stat->root_index == node_ref) {
        stat_file->stat->root_index = node_next;
    } else
        for (ref = 0; ref < stat_file->max_nodes; ref++) {
            node = &stat_file->stat->nodes[ref];
            if (!(node->flags & SPICE_STAT_NODE_FLAG_ENABLED)) {
                continue;
            }
            /* in a tree there is only a link from a parent or
             * previous sibling so we can exit the loop as
             * soon as we found on link */
            if (node->first_child_index == node_ref) {
                node->first_child_index = node_next;
                break;
            }
            if (node->next_sibling_index == node_ref) {
                node->next_sibling_index = node_next;
                break;
            }
        }
    pthread_mutex_unlock(&stat_file->lock);
}

void stat_file_remove_node(RedStatFile *stat_file, StatNodeRef ref)
{
    stat_file_remove(stat_file, &stat_file->stat->nodes[ref]);
}

void stat_file_remove_counter(RedStatFile *stat_file, uint64_t *counter)
{
    stat_file_remove(stat_file, (SpiceStatNode *)(counter - SPICE_OFFSETOF(SpiceStatNode, value)));
}
