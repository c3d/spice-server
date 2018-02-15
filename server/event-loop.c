/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
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

/*
 * This file exports a global variable:
 *
 * const SpiceCoreInterfaceInternal event_loop_core;
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "red-common.h"

struct SpiceTimer
{
    GMainContext * context;
    SpiceTimerFunc func;
    void *         opaque;
    GSource *      source;
};

static SpiceTimer *
timer_add(const SpiceCoreInterfaceInternal *iface, SpiceTimerFunc func, void *opaque)
{
    SpiceTimer *timer = g_new0(SpiceTimer, 1);

    timer->context = iface->main_context;
    timer->func    = func;
    timer->opaque  = opaque;

    return timer;
}

static gboolean timer_func(gpointer user_data)
{
    SpiceTimer *timer = user_data;

    timer->func(timer->opaque);
    /* timer might be free after func(), don't touch */

    return FALSE;
}

static void timer_cancel(const SpiceCoreInterfaceInternal *iface, SpiceTimer *timer)
{
    if (timer->source) {
        g_source_destroy(timer->source);
        g_source_unref(timer->source);
        timer->source = NULL;
    }
}

static void timer_start(const SpiceCoreInterfaceInternal *iface, SpiceTimer *timer, uint32_t ms)
{
    timer_cancel(iface, timer);

    timer->source = g_timeout_source_new(ms);
    spice_assert(timer->source != NULL);

    g_source_set_callback(timer->source, timer_func, timer, NULL);

    g_source_attach(timer->source, timer->context);
}

static void timer_remove(const SpiceCoreInterfaceInternal *iface, SpiceTimer *timer)
{
    timer_cancel(iface, timer);
    spice_assert(timer->source == NULL);
    g_free(timer);
}

struct SpiceWatch
{
    GMainContext * context;
    void *         opaque;
    GSource *      source;
    GIOChannel *   channel;
    SpiceWatchFunc func;
};

static GIOCondition spice_event_to_giocondition(int event_mask)
{
    GIOCondition condition = 0;

    if (event_mask & SPICE_WATCH_EVENT_READ)
        condition |= G_IO_IN;
    if (event_mask & SPICE_WATCH_EVENT_WRITE)
        condition |= G_IO_OUT;

    return condition;
}

static int giocondition_to_spice_event(GIOCondition condition)
{
    int event = 0;

    if (condition & G_IO_IN)
        event |= SPICE_WATCH_EVENT_READ;
    if (condition & G_IO_OUT)
        event |= SPICE_WATCH_EVENT_WRITE;

    return event;
}

static gboolean watch_func(GIOChannel *source, GIOCondition condition, gpointer data)
{
    SpiceWatch *watch = data;
    int         fd    = g_io_channel_unix_get_fd(source);

    watch->func(fd, giocondition_to_spice_event(condition), watch->opaque);

    return TRUE;
}

static void
watch_update_mask(const SpiceCoreInterfaceInternal *iface, SpiceWatch *watch, int event_mask)
{
    if (watch->source) {
        g_source_destroy(watch->source);
        g_source_unref(watch->source);
        watch->source = NULL;
    }

    if (!event_mask)
        return;

    watch->source = g_io_create_watch(watch->channel, spice_event_to_giocondition(event_mask));
    g_source_set_callback(watch->source, (GSourceFunc)watch_func, watch, NULL);
    g_source_attach(watch->source, watch->context);
}

static SpiceWatch *watch_add(const SpiceCoreInterfaceInternal *iface,
                             int                               fd,
                             int                               event_mask,
                             SpiceWatchFunc                    func,
                             void *                            opaque)
{
    SpiceWatch *watch;

    spice_return_val_if_fail(fd != -1, NULL);
    spice_return_val_if_fail(func != NULL, NULL);

    watch          = g_new0(SpiceWatch, 1);
    watch->context = iface->main_context;
    watch->channel = g_io_channel_unix_new(fd);
    watch->func    = func;
    watch->opaque  = opaque;

    watch_update_mask(iface, watch, event_mask);

    return watch;
}

static void watch_remove(const SpiceCoreInterfaceInternal *iface, SpiceWatch *watch)
{
    watch_update_mask(iface, watch, 0);
    spice_assert(watch->source == NULL);

    g_io_channel_unref(watch->channel);
    g_free(watch);
}

const SpiceCoreInterfaceInternal event_loop_core = {
    .timer_add    = timer_add,
    .timer_start  = timer_start,
    .timer_cancel = timer_cancel,
    .timer_remove = timer_remove,

    .watch_add         = watch_add,
    .watch_update_mask = watch_update_mask,
    .watch_remove      = watch_remove,
};
