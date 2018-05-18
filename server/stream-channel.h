/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2017 Red Hat, Inc.

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

#ifndef STREAM_CHANNEL_H_
#define STREAM_CHANNEL_H_

#include <spice/stream-device.h>

#include "red-channel.h"

G_BEGIN_DECLS

/**
 * This type it's a RedChannel class which implement display
 * channel with input only by stream.
 * A pointer to StreamChannel can be converted to a RedChannel.
 */
typedef struct StreamChannel StreamChannel;
typedef struct StreamChannelClass StreamChannelClass;

#define TYPE_STREAM_CHANNEL stream_channel_get_type()

#define STREAM_CHANNEL(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_STREAM_CHANNEL, StreamChannel))
#define STREAM_CHANNEL_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), TYPE_STREAM_CHANNEL, StreamChannelClass))
#define IS_STREAM_CHANNEL(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), TYPE_STREAM_CHANNEL))
#define IS_STREAM_CHANNEL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), TYPE_STREAM_CHANNEL))
#define STREAM_CHANNEL_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), TYPE_STREAM_CHANNEL, StreamChannelClass))

GType stream_channel_get_type(void) G_GNUC_CONST;

/**
 * Create StreamChannel.
 */
StreamChannel* stream_channel_new(RedsState *server, uint32_t id);

/**
 * Reset channel at initial state
 */
void stream_channel_reset(StreamChannel *channel);

struct StreamMsgStreamFormat;
struct StreamMsgStartStop;

void stream_channel_change_format(StreamChannel *channel,
                                  const struct StreamMsgFormat *fmt);
void stream_channel_send_data(StreamChannel *channel,
                              const void *data, size_t size,
                              uint32_t mm_time);

typedef void (*stream_channel_start_proc)(void *opaque, struct StreamMsgStartStop *start,
                                          StreamChannel *channel);
void stream_channel_register_start_cb(StreamChannel *channel,
                                      stream_channel_start_proc cb, void *opaque);

typedef struct StreamQueueStat {
    uint32_t num_items;
    uint32_t size;
} StreamQueueStat;

typedef void (*stream_channel_queue_stat_proc)(void *opaque, const StreamQueueStat *stats,
                                               StreamChannel *channel);
void stream_channel_register_queue_stat_cb(StreamChannel *channel,
                                           stream_channel_queue_stat_proc cb, void *opaque);

typedef void (*stream_channel_adjust_proc)(void *opaque,
                                           uint32_t parameter_id, uint32_t parameter_value,
                                           StreamChannel *channel);
void stream_channel_register_adjust_cb(StreamChannel *channel,
                                       stream_channel_adjust_proc cb, void *opaque);

G_END_DECLS

#endif /* STREAM_CHANNEL_H_ */
