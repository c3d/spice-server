/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2016 Red Hat, Inc.

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
#include <config.h>

#include "red-channel.h"
#include "red-pipe-item.h"

RedPipeItem *red_pipe_item_ref(RedPipeItem *item)
{
    g_return_val_if_fail(item->refcount > 0, NULL);

    g_atomic_int_inc(&item->refcount);

    return item;
}

void red_pipe_item_unref(RedPipeItem *item)
{
    g_return_if_fail(item->refcount > 0);

    if (g_atomic_int_dec_and_test(&item->refcount)) {
        item->free_func(item);
    }
}

void red_pipe_item_init_full(RedPipeItem *item, gint type, red_pipe_item_free_t *free_func)
{
    item->type      = type;
    item->refcount  = 1;
    item->free_func = free_func ? free_func : (red_pipe_item_free_t *)g_free;
}

void marshaller_unref_pipe_item(uint8_t *data G_GNUC_UNUSED, void *opaque)
{
    RedPipeItem *item = opaque;
    red_pipe_item_unref(item);
}
