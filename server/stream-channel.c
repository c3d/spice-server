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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <common/generated_server_marshallers.h>
#include <spice/stream-device.h>

#include "red-channel-client.h"
#include "stream-channel.h"
#include "reds.h"
#include "common-graphics-channel.h"
#include "display-limits.h"
#include "video-stream.h" // TODO remove, put common stuff

RECORDER_DEFINE(stream_channel, 32, "Events related to stream channels");
RECORDER_DEFINE(stream_stat, 64, "Statistics about a stream");
RECORDER_DEFINE(stream_send, 64, "Sending data to a stream");

#define TYPE_STREAM_CHANNEL_CLIENT stream_channel_client_get_type()

#define STREAM_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_CAST((obj), TYPE_STREAM_CHANNEL_CLIENT, StreamChannelClient))
#define STREAM_CHANNEL_CLIENT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_CAST((klass), TYPE_STREAM_CHANNEL_CLIENT, StreamChannelClientClass))
#define IS_STREAM_CHANNEL_CLIENT(obj) \
    (G_TYPE_CHECK_INSTANCE_TYPE((obj), TYPE_STREAM_CHANNEL_CLIENT))
#define IS_STREAM_CHANNEL_CLIENT_CLASS(klass) \
    (G_TYPE_CHECK_CLASS_TYPE((klass), TYPE_STREAM_CHANNEL_CLIENT))
#define STREAM_CHANNEL_CLIENT_GET_CLASS(obj) \
    (G_TYPE_INSTANCE_GET_CLASS((obj), TYPE_STREAM_CHANNEL_CLIENT, StreamChannelClientClass))

typedef struct StreamChannelClient StreamChannelClient;
typedef struct StreamChannelClientClass StreamChannelClientClass;

/* we need to inherit from CommonGraphicsChannelClient
 * to get buffer handling */
struct StreamChannelClient {
    CommonGraphicsChannelClient parent;

    /* current video stream id, <0 if not initialized or
     * we are not sending a stream */
    int stream_id;
};

struct StreamChannelClientClass {
    CommonGraphicsChannelClientClass parent_class;
};

GType stream_channel_client_get_type(void) G_GNUC_CONST;

G_DEFINE_TYPE(StreamChannelClient, stream_channel_client, TYPE_COMMON_GRAPHICS_CHANNEL_CLIENT)

typedef struct StreamEncoderParameters {
    uint32_t frames_per_second;
    uint32_t max_bytes_per_second;
    uint32_t average_bytes_per_second;
    uint32_t gop_length;
    uint32_t quality;
} StreamEncoderParameters;

typedef struct StreamMetrics {
    uint32_t value[SPICE_MSGC_METRIC_LAST];
    uint32_t average[SPICE_MSGC_METRIC_LAST];
    uint32_t weight[SPICE_MSGC_METRIC_LAST];
} StreamMetrics;

struct StreamChannel {
    RedChannel parent;

    /* current video stream id, <0 if not initialized or
     * we are not sending a stream */
    int stream_id;
    /* size of the current video stream */
    unsigned width, height;

    StreamQueueStat queue_stat;

    /* callback to notify when a stream should be started or stopped */
    stream_channel_start_proc start_cb;
    void *start_opaque;

    /* callback to notify when queue statistics changes */
    stream_channel_queue_stat_proc queue_cb;
    void *queue_opaque;

    /* callback to notify encoder of a parameter change */
    stream_channel_adjust_proc adjust_cb;
    void *adjust_opaque;
    StreamEncoderParameters encoder_parameters;
    StreamMetrics metrics;
};

struct StreamChannelClass {
    RedChannelClass parent_class;
};

G_DEFINE_TYPE(StreamChannel, stream_channel, RED_TYPE_CHANNEL)

enum {
    RED_PIPE_ITEM_TYPE_SURFACE_CREATE = RED_PIPE_ITEM_TYPE_COMMON_LAST,
    RED_PIPE_ITEM_TYPE_SURFACE_DESTROY,
    RED_PIPE_ITEM_TYPE_FILL_SURFACE,
    RED_PIPE_ITEM_TYPE_STREAM_CREATE,
    RED_PIPE_ITEM_TYPE_STREAM_DATA,
    RED_PIPE_ITEM_TYPE_STREAM_DESTROY,
    RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT,
    RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_METRICS,
    RED_PIPE_ITEM_TYPE_MONITORS_CONFIG,
};

typedef struct StreamCreateItem {
    RedPipeItem base;
    SpiceMsgDisplayStreamCreate stream_create;
} StreamCreateItem;

typedef struct StreamDataItem {
    RedPipeItem base;
    StreamChannel *channel;
    // NOTE: this must be the last field in the structure
    SpiceMsgDisplayStreamData data;
} StreamDataItem;

#define PRIMARY_SURFACE_ID 0

static void stream_channel_client_on_disconnect(RedChannelClient *rcc);

static void
stream_channel_client_class_init(StreamChannelClientClass *klass)
{
    RedChannelClientClass *channel_class = RED_CHANNEL_CLIENT_CLASS(klass);

    channel_class->on_disconnect = stream_channel_client_on_disconnect;
}

static void
stream_channel_client_init(StreamChannelClient *client)
{
    client->stream_id = -1;
}

static void
request_new_stream(StreamChannel *channel, StreamMsgStartStop *start)
{
    if (channel->start_cb) {
        channel->start_cb(channel->start_opaque, start, channel);
    }
}

static void
stream_channel_client_on_disconnect(RedChannelClient *rcc)
{
    RedChannel *red_channel = red_channel_client_get_channel(rcc);

    // if there are still some client connected keep streaming
    // TODO, maybe would be worth sending new codecs if they are better
    if (red_channel_is_connected(red_channel)) {
        return;
    }

    StreamChannel *channel = STREAM_CHANNEL(red_channel);
    channel->stream_id = -1;
    channel->width = 0;
    channel->height = 0;

    // send stream stop to device
    StreamMsgStartStop stop = { 0, };
    request_new_stream(channel, &stop);
}

static StreamChannelClient*
stream_channel_client_new(StreamChannel *channel, RedClient *client, RedStream *stream,
                          int mig_target, RedChannelCapabilities *caps)
{
    StreamChannelClient *rcc;

    rcc = g_initable_new(TYPE_STREAM_CHANNEL_CLIENT,
                         NULL, NULL,
                         "channel", channel,
                         "client", client,
                         "stream", stream,
                         "monitor-latency", FALSE,
                         "caps", caps,
                         NULL);

    return rcc;
}

static void
fill_base(SpiceMarshaller *m, const StreamChannel *channel)
{
    SpiceMsgDisplayBase base;

    base.surface_id = PRIMARY_SURFACE_ID;
    base.box = (SpiceRect) { 0, 0, channel->width, channel->height };
    base.clip = (SpiceClip) { SPICE_CLIP_TYPE_NONE, NULL };

    spice_marshall_DisplayBase(m, &base);
}

static void
marshall_monitors_config(RedChannelClient *rcc, StreamChannel *channel, SpiceMarshaller *m)
{
    struct {
        SpiceMsgDisplayMonitorsConfig config;
        SpiceHead head;
    } msg = {
        { 1, 1, },
        {
            // monitor ID. These IDs are allocated per channel starting from 0
            0,
            PRIMARY_SURFACE_ID,
            channel->width, channel->height,
            0, 0,
            0 // flags
        }
    };

    red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_MONITORS_CONFIG);
    spice_marshall_msg_display_monitors_config(m, &msg.config);
}

RECORDER_DEFINE(stream_channel_activate, 16, "Activation of stream channels");

static void
stream_channel_send_item(RedChannelClient *rcc, RedPipeItem *pipe_item)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);
    StreamChannelClient *client = STREAM_CHANNEL_CLIENT(rcc);
    StreamChannel *channel = STREAM_CHANNEL(red_channel_client_get_channel(rcc));

    RECORD(stream_channel, "Send item %p type %u to rcc %p", pipe_item, pipe_item->type, rcc);

    switch (pipe_item->type) {
    case RED_PIPE_ITEM_TYPE_SURFACE_CREATE: {
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_SURFACE_CREATE);
        SpiceMsgSurfaceCreate surface_create = {
            PRIMARY_SURFACE_ID,
            channel->width, channel->height,
            SPICE_SURFACE_FMT_32_xRGB, SPICE_SURFACE_FLAGS_PRIMARY
        };

        // give an hint to client that we are sending just streaming
        // see spice.proto for capability check here
        if (red_channel_client_test_remote_cap(rcc, SPICE_DISPLAY_CAP_MULTI_CODEC)) {
            surface_create.flags |= SPICE_SURFACE_FLAGS_STREAMING_MODE;
        }

        spice_marshall_msg_display_surface_create(m, &surface_create);
        break;
    }
    case RED_PIPE_ITEM_TYPE_MONITORS_CONFIG:
        if (!red_channel_client_test_remote_cap(rcc, SPICE_DISPLAY_CAP_MONITORS_CONFIG)) {
            return;
        }
        marshall_monitors_config(rcc, channel, m);
        break;
    case RED_PIPE_ITEM_TYPE_SURFACE_DESTROY: {
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_SURFACE_DESTROY);
        SpiceMsgSurfaceDestroy surface_destroy = { PRIMARY_SURFACE_ID };
        spice_marshall_msg_display_surface_destroy(m, &surface_destroy);
        break;
    }
    case RED_PIPE_ITEM_TYPE_FILL_SURFACE: {
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_DRAW_FILL);

        fill_base(m, channel);

        SpiceFill fill;
        fill.brush = (SpiceBrush) { SPICE_BRUSH_TYPE_SOLID, { .color = 0 } };
        fill.rop_descriptor = SPICE_ROPD_OP_PUT;
        fill.mask = (SpiceQMask) { 0, { 0, 0 }, NULL };
        SpiceMarshaller *brush_pat_out, *mask_bitmap_out;
        spice_marshall_Fill(m, &fill, &brush_pat_out, &mask_bitmap_out);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_CREATE: {
        StreamCreateItem *item = SPICE_UPCAST(StreamCreateItem, pipe_item);
        client->stream_id = item->stream_create.id;
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_CREATE);
        spice_marshall_msg_display_stream_create(m, &item->stream_create);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_REPORT: {
        RECORD(stream_channel_activate, "Activating reports on stream %d", client->stream_id);
        if (client->stream_id < 0
            || !red_channel_client_test_remote_cap(rcc, SPICE_DISPLAY_CAP_STREAM_REPORT)) {
            return;
        }
        SpiceMsgDisplayStreamActivateReport msg;
        msg.stream_id = client->stream_id;
        msg.unique_id = 1; // TODO useful ?
        msg.max_window_size = RED_STREAM_CLIENT_REPORT_WINDOW;
        msg.timeout_ms = RED_STREAM_CLIENT_REPORT_TIMEOUT;
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_ACTIVATE_REPORT);
        spice_marshall_msg_display_stream_activate_report(m, &msg);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_METRICS: {
        RECORD(stream_channel_activate, "Activating metrics on stream %d", client->stream_id);
        if (client->stream_id < 0
            || !red_channel_client_test_remote_cap(rcc, SPICE_DISPLAY_CAP_METRICS)) {
            return;
        }
        SpiceMsgDisplayStreamActivateMetrics msg;
        msg.stream_id = client->stream_id;
        msg.unique_id = 1; // TODO useful ?
        msg.max_window_size = RED_STREAM_CLIENT_REPORT_WINDOW;
        msg.timeout_ms = RED_STREAM_CLIENT_REPORT_TIMEOUT;
        msg.last_known_metric = SPICE_MSGC_METRIC_LAST;
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_ACTIVATE_METRICS);
        spice_marshall_msg_display_stream_activate_metrics(m, &msg);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_DATA: {
        StreamDataItem *item = SPICE_UPCAST(StreamDataItem, pipe_item);
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DATA);
        spice_marshall_msg_display_stream_data(m, &item->data);
        red_pipe_item_ref(pipe_item);
        spice_marshaller_add_by_ref_full(m, item->data.data, item->data.data_size,
                                         marshaller_unref_pipe_item, pipe_item);
        break;
    }
    case RED_PIPE_ITEM_TYPE_STREAM_DESTROY: {
        if (client->stream_id < 0) {
            return;
        }
        SpiceMsgDisplayStreamDestroy stream_destroy = { client->stream_id };
        red_channel_client_init_send_data(rcc, SPICE_MSG_DISPLAY_STREAM_DESTROY);
        spice_marshall_msg_display_stream_destroy(m, &stream_destroy);
        client->stream_id = -1;
        break;
    }
    default:
        spice_error("invalid pipe item type");
    }

    red_channel_client_begin_send_message(rcc);
}


RECORDER_TWEAK_DEFINE(fps_tgt,          60, "Ideal frames per second for smart streaming");
RECORDER_TWEAK_DEFINE(fps_min,           2, "  Minimum number of FPS");
RECORDER_TWEAK_DEFINE(fps_max,          60, "  Maximum number of FPS");
RECORDER_TWEAK_DEFINE(fps_adjpct,       10, "  Percentage of rate adjustment for FPS");

RECORDER_TWEAK_DEFINE(bps_tgt,       80000, "Ideal bytes per second for smart streaming");
RECORDER_TWEAK_DEFINE(bps_min,        2000, "  Minimum number of BPS");
RECORDER_TWEAK_DEFINE(bps_max,    10000000, "  Maximum number of BPS");
RECORDER_TWEAK_DEFINE(bps_adjpct,       10, "  Percentage of rate adjustment for BPS");

RECORDER_DEFINE(smstr_fps,              16, "Smart streaming frames per second")
RECORDER_DEFINE(smstr_bps,              16, "Smart streaming average bytes per second")
RECORDER_DEFINE(smstr_max_bps,          16, "Smart streaming max bytes per second")
RECORDER_DEFINE(smstr_gop_length,       16, "Smart streaming GOP length")
RECORDER_DEFINE(smstr_quality,          16, "Smart streaming quality")


static bool stream_smarts(uint32_t metric_id, uint32_t value, uint32_t average,
                          StreamMetrics *metrics, StreamEncoderParameters *parms)
{
    uint32_t tgt_fps = RECORDER_TWEAK(fps_tgt);
    uint32_t tgt_bps = RECORDER_TWEAK(bps_tgt);
    uint32_t *fps_ptr = &parms->frames_per_second;
    uint32_t *bps_ptr = &parms->average_bytes_per_second;
    uint32_t fps = *fps_ptr;
    uint32_t bps = *bps_ptr;

    // Average values
    uint32_t rec_fps = metrics->average[SPICE_MSGC_METRIC_FRAMES_RECEIVED_PER_SECOND];
    uint32_t dec_fps = metrics->average[SPICE_MSGC_METRIC_FRAMES_DECODED_PER_SECOND];
    uint32_t dis_fps = metrics->average[SPICE_MSGC_METRIC_FRAMES_DISPLAYED_PER_SECOND];
    //uint32_t drp_fps = metrics->average[SPICE_MSGC_METRIC_FRAMES_DROPPED_PER_SECOND];
    //uint32_t rec_bps = metrics->average[SPICE_MSGC_METRIC_BYTES_RECEIVED_PER_SECOND];
    //uint32_t dec_bps = metrics->average[SPICE_MSGC_METRIC_BYTES_DECODED_PER_SECOND];
    //uint32_t dis_bps = metrics->average[SPICE_MSGC_METRIC_BYTES_DISPLAYED_PER_SECOND];
    //uint32_t drp_bps = metrics->average[SPICE_MSGC_METRIC_BYTES_DROPPED_PER_SECOND];
    //uint32_t que_len = metrics->average[SPICE_MSGC_METRIC_DECODER_QUEUE_LENGTH];

    // Latest values
    //uint32_t cur_rec_fps = metrics->value[SPICE_MSGC_METRIC_FRAMES_RECEIVED_PER_SECOND];
    //uint32_t cur_dec_fps = metrics->value[SPICE_MSGC_METRIC_FRAMES_DECODED_PER_SECOND];
    uint32_t cur_dis_fps = metrics->value[SPICE_MSGC_METRIC_FRAMES_DISPLAYED_PER_SECOND];
    //uint32_t cur_drp_fps = metrics->value[SPICE_MSGC_METRIC_FRAMES_DROPPED_PER_SECOND];
    //uint32_t cur_rec_bps = metrics->value[SPICE_MSGC_METRIC_BYTES_RECEIVED_PER_SECOND];
    //uint32_t cur_dec_bps = metrics->value[SPICE_MSGC_METRIC_BYTES_DECODED_PER_SECOND];
    //uint32_t cur_dis_bps = metrics->value[SPICE_MSGC_METRIC_BYTES_DISPLAYED_PER_SECOND];
    //uint32_t cur_drp_bps = metrics->value[SPICE_MSGC_METRIC_BYTES_DROPPED_PER_SECOND];
    //uint32_t cur_que_len = metrics->value[SPICE_MSGC_METRIC_DECODER_QUEUE_LENGTH];

    // Check if data was received
    uint32_t has_rec_fps = metrics->weight[SPICE_MSGC_METRIC_FRAMES_RECEIVED_PER_SECOND];
    uint32_t has_dec_fps = metrics->weight[SPICE_MSGC_METRIC_FRAMES_DECODED_PER_SECOND];
    uint32_t has_dis_fps = metrics->weight[SPICE_MSGC_METRIC_FRAMES_DISPLAYED_PER_SECOND];
    //uint32_t has_drp_fps = metrics->weight[SPICE_MSGC_METRIC_FRAMES_DROPPED_PER_SECOND];
    //uint32_t has_rec_bps = metrics->weight[SPICE_MSGC_METRIC_BYTES_RECEIVED_PER_SECOND];
    //uint32_t has_dec_bps = metrics->weight[SPICE_MSGC_METRIC_BYTES_DECODED_PER_SECOND];
    //uint32_t has_dis_bps = metrics->weight[SPICE_MSGC_METRIC_BYTES_DISPLAYED_PER_SECOND];
    //uint32_t has_drp_bps = metrics->weight[SPICE_MSGC_METRIC_BYTES_DROPPED_PER_SECOND];
    //uint32_t has_que_len = metrics->weight[SPICE_MSGC_METRIC_DECODER_QUEUE_LENGTH];

#define ADJUST(parm, reason, condition, target)                         \
    if (condition) {                                                    \
        uint32_t cur = parm;                                            \
        uint32_t tgt = (target);                                        \
        uint32_t pct = RECORDER_TWEAK(parm##_adjpct);                   \
        uint32_t min = RECORDER_TWEAK(parm##_min);                      \
        uint32_t max = RECORDER_TWEAK(parm##_max);                      \
        uint32_t val = (pct * tgt + (100 - pct) * cur) / 100;           \
        uint32_t set = val < min ? min : val > max ? max : val;         \
        if (set != cur) {                                               \
            record(smstr_##parm,                                        \
                   "Adjusting " #parm " from %u to %u"                  \
                   " towards %u want %u  (" reason ")",                 \
                   cur, set, tgt, RECORDER_TWEAK(parm##_tgt));          \
            *parm##_ptr = set;                                          \
            return TRUE;                                                \
        }                                                               \
    }
#define ADJUST_FPS(r,c,t) ADJUST(fps, r,c,t)
#define ADJUST_BPS(r,c,t) ADJUST(bps, r,c,t)

    switch(metric_id) {
    case SPICE_MSGC_METRIC_FRAMES_RECEIVED_PER_SECOND:
    case SPICE_MSGC_METRIC_FRAMES_DECODED_PER_SECOND:
    case SPICE_MSGC_METRIC_FRAMES_DISPLAYED_PER_SECOND:
        // We have a client-side problem is one of the following is true:
        // - Any incoming FPS metric is below our target (meaning client is already behind us)
        ADJUST_FPS("Low metric",
                   average < fps,
                   average);

        // - We display less than we can decode, i.e. display is the bottleneck
        //   (this case normally only occurs when no fully-accelerated pipeline is available)
        ADJUST_FPS("Display lag",
                   has_dec_fps && has_dis_fps && dis_fps < dec_fps,
                   dis_fps);

        // - We decode less than we can receive
        //   (this case occurs e.g. if software decoder does not get sufficient CPU)
        ADJUST_FPS("Decoder lag",
                   has_rec_fps && has_dec_fps && dec_fps < rec_fps,
                   dec_fps);

        // - We display less than we receive
        //   (if not handled above, it's that we didn't receive a decoder metric)
        ADJUST_FPS("Pipleine lag",
                   has_dis_fps && has_rec_fps && dis_fps < rec_fps,
                   dis_fps);

        // If not in the cases above and we displayed more than current settings,
        // we can safely attempt to increase FPS to whatever we last displayed
        ADJUST_FPS("Return to last FPS",
                   has_dis_fps && (fps < cur_dis_fps || dis_fps < cur_dis_fps),
                   cur_dis_fps);

        // If the average FPS is higher than the last one, try to treat that as a glitch
        ADJUST_FPS("Return to average FPS if higher than last",
                   has_dis_fps && fps < dis_fps,
                   dis_fps);

        // Otherwise, try to return to the ideal FPS (recovery after a problem)
        ADJUST_FPS("Return to target FPS", fps < tgt_fps, tgt_fps);
        break;

    case SPICE_MSGC_METRIC_BYTES_RECEIVED_PER_SECOND:
    case SPICE_MSGC_METRIC_BYTES_DECODED_PER_SECOND:
    case SPICE_MSGC_METRIC_BYTES_DISPLAYED_PER_SECOND:
        // There is a network issue if:
        // - The client receives much less than what we send
        // REVISIT: Implement that step

        // REVISIT: Should we recompute max bytes per second?
        // Smart streaming currently does not adjust it

        // Try to get closer to the average that is measured
        ADJUST_BPS("Network usage is going down", average < bps, average);
        ADJUST_BPS("Network usage is going up", average > bps, average);
        ADJUST_BPS("Return to target bandwidth", true, tgt_bps);
        break;

    case SPICE_MSGC_METRIC_FRAMES_DROPPED_PER_SECOND:
        // When we drop frames, reduce FPS by that amount
        ADJUST_FPS("Dropped frames", average != 0 && fps > average, fps - average);
        break;

    case SPICE_MSGC_METRIC_BYTES_DROPPED_PER_SECOND:
        // When we drop bytes, reduce BPS by how much we dropped
        ADJUST_BPS("Dropped frames", average != 0 && bps > average, bps - average);
        break;

    case SPICE_MSGC_METRIC_DECODER_QUEUE_LENGTH:
        // If the decoder queue length starts diverging, we may want to reduce FPS
        ADJUST_FPS("Queue length is diverging", value > average, fps - 1);
        break;
    }

    return FALSE;
}

RECORDER_DEFINE(server_stream_metrics, 32, "Metrics received by server from client")
RECORDER_TWEAK_DEFINE(metric_percent, 10, "Percentage of change caused by input metric");

RECORDER_TWEAK_DEFINE(dropped_fps_min, 0, "Minimum number of dropped frames per second considered for smart streaming");
RECORDER_TWEAK_DEFINE(dropped_fps_max, 120, "Maximum number of dropped frames per second considered for smart streaming");

RECORDER_TWEAK_DEFINE(queue_length_min, 0, "Minimum queue length considered by smart streaming");
RECORDER_TWEAK_DEFINE(queue_length_max, 32000000, "Maximum queue length considered by smart streaming");

RECORDER_TWEAK_DEFINE(smstr_clear, 0, "Clear all smart streaming stats for testing");

RECORDER_DEFINE(metric_received_fps,    16, "Incoming metric for received frames per second")
RECORDER_DEFINE(metric_decoded_fps,     16, "Incoming metric for decoded frames per second")
RECORDER_DEFINE(metric_displayed_fps,   16, "Incoming metric for displayed frames per second")
RECORDER_DEFINE(metric_dropped_fps,     16, "Incoming metric for dropped frames per second")
RECORDER_DEFINE(metric_received_bps,    16, "Incoming metric for received bytes per second")
RECORDER_DEFINE(metric_decoded_bps,     16, "Incoming metric for decoded bytes per second")
RECORDER_DEFINE(metric_displayed_bps,   16, "Incoming metric for displayed bytes per second")
RECORDER_DEFINE(metric_dropped_bps,     16, "Incoming metric for dropped bytes per second")
RECORDER_DEFINE(metric_queue_length,    16, "Incoming metric for client-side decoder queue length")

static bool handle_stream_metric(RedChannelClient *rcc,
                                 SpiceMsgcDisplayStreamMetric *metric)
{
    RedChannel *red_channel = red_channel_client_get_channel(rcc);
    StreamChannel *channel = STREAM_CHANNEL(red_channel);
    StreamMetrics *metrics = &channel->metrics;
    StreamEncoderParameters saved = channel->encoder_parameters;
    StreamEncoderParameters adjusted = saved;

    if (RECORDER_TWEAK(smstr_clear)) {
        RECORDER_TWEAK(smstr_clear) = 0;
        memset(&channel->metrics, 0, sizeof(channel->metrics));
        memset(&channel->encoder_parameters, 0, sizeof(channel->encoder_parameters));
        memset(&saved, 0, sizeof(saved));
        memset(&adjusted, 0, sizeof(adjusted));
    }

    if (metric->stream_id != channel->stream_id) {
        spice_warning("stream_metrics: invalid stream id %u-%u",
                      metric->stream_id, channel->stream_id);
        return FALSE;
    }
    if (!channel->adjust_cb) {
        spice_warning("stream_metrics: no adjust callback for stream %u", metric->stream_id);
        return FALSE;
    }

    uint32_t metric_id = metric->metric_id;
    uint32_t value = metric->metric_value * metric->metric_duration / 1000;
    uint32_t average = metrics->average[metric_id];
    uint32_t percent = RECORDER_TWEAK(metric_percent);

    static char *metric_name[SPICE_MSGC_METRIC_LAST] = {
        [SPICE_MSGC_METRIC_INVALID]                     = "METRIC_INVALID",

        [SPICE_MSGC_METRIC_FRAMES_RECEIVED_PER_SECOND]  = "METRIC_FRAMES_RECEIVED_PER_SECOND",
        [SPICE_MSGC_METRIC_FRAMES_DECODED_PER_SECOND]   = "METRIC_FRAMES_DECODED_PER_SECOND",
        [SPICE_MSGC_METRIC_FRAMES_DISPLAYED_PER_SECOND] = "METRIC_FRAMES_DISPLAYED_PER_SECOND",
        [SPICE_MSGC_METRIC_FRAMES_DROPPED_PER_SECOND]   = "METRIC_FRAMES_DROPPED_PER_SECOND",
        [SPICE_MSGC_METRIC_BYTES_RECEIVED_PER_SECOND]   = "METRIC_BYTES_RECEIVED_PER_SECOND",
        [SPICE_MSGC_METRIC_BYTES_DECODED_PER_SECOND]    = "METRIC_BYTES_DECODED_PER_SECOND",
        [SPICE_MSGC_METRIC_BYTES_DISPLAYED_PER_SECOND]  = "METRIC_BYTES_DISPLAYED_PER_SECOND",
        [SPICE_MSGC_METRIC_BYTES_DROPPED_PER_SECOND]    = "METRIC_BYTES_DROPPED_PER_SECOND",
        [SPICE_MSGC_METRIC_DECODER_QUEUE_LENGTH]        = "METRIC_DECODER_QUEUE_LENGTH",
    };
    record(server_stream_metrics, "Received %+s (%u) value %u",
           metric_id < SPICE_MSGC_METRIC_LAST ? metric_name[metric_id] : "Unknown",
           metric_id, value);

    switch(metric_id) {
    case SPICE_MSGC_METRIC_FRAMES_RECEIVED_PER_SECOND:
        record(metric_received_fps, "Received FPS=%u average %u (raw %u in %u ms)",
               value, average, metric->metric_value, metric->metric_duration);
        break;
    case SPICE_MSGC_METRIC_FRAMES_DECODED_PER_SECOND:
        record(metric_decoded_fps, "Decoded FPS=%u average %u (raw %u in %u ms)",
               value, average, metric->metric_value, metric->metric_duration);
        break;
    case SPICE_MSGC_METRIC_FRAMES_DISPLAYED_PER_SECOND:
        record(metric_displayed_fps, "Displayed FPS=%u average %u (raw %u in %u ms)",
               value, average, metric->metric_value, metric->metric_duration);
        break;
    case SPICE_MSGC_METRIC_FRAMES_DROPPED_PER_SECOND:
        record(metric_dropped_fps, "Dropped FPS=%u average %u (raw %u in %u ms)",
               value, average, metric->metric_value, metric->metric_duration);
        break;
    case SPICE_MSGC_METRIC_BYTES_RECEIVED_PER_SECOND:
        record(metric_received_bps, "Received BPS=%u average %u (raw %u in %u ms)",
               value, average, metric->metric_value, metric->metric_duration);
        break;
    case SPICE_MSGC_METRIC_BYTES_DECODED_PER_SECOND:
        record(metric_decoded_bps, "Decoded BPS=%u average %u (raw %u in %u ms)",
               value, average, metric->metric_value, metric->metric_duration);
        break;
    case SPICE_MSGC_METRIC_BYTES_DISPLAYED_PER_SECOND:
        record(metric_displayed_bps, "Displayed BPS=%u average %u (raw %u in %u ms)",
               value, average, metric->metric_value, metric->metric_duration);
        break;
    case SPICE_MSGC_METRIC_BYTES_DROPPED_PER_SECOND:
        record(metric_dropped_bps, "Dropped BPS=%u average %u (raw %u in %u ms)",
               value, average, metric->metric_value, metric->metric_duration);
        break;
    case SPICE_MSGC_METRIC_DECODER_QUEUE_LENGTH:
        record(metric_queue_length, "Client queue length=%u average %u (raw %u in %u ms)",
               value, average, metric->metric_value, metric->metric_duration);
        break;
    }

    // Check which bounds apply for the incoming metric
    // REVISIT: Do the same thing without recorder tweaks
    uint32_t min = 0;
    uint32_t max = ~0U;
    switch(metric_id) {
    case SPICE_MSGC_METRIC_FRAMES_RECEIVED_PER_SECOND:
    case SPICE_MSGC_METRIC_FRAMES_DECODED_PER_SECOND:
    case SPICE_MSGC_METRIC_FRAMES_DISPLAYED_PER_SECOND:
        min = RECORDER_TWEAK(fps_min);
        max = RECORDER_TWEAK(fps_max);
        break;
    case SPICE_MSGC_METRIC_FRAMES_DROPPED_PER_SECOND:
        min = RECORDER_TWEAK(dropped_fps_min);
        max = RECORDER_TWEAK(dropped_fps_max);
        break;
    case SPICE_MSGC_METRIC_BYTES_RECEIVED_PER_SECOND:
    case SPICE_MSGC_METRIC_BYTES_DECODED_PER_SECOND:
    case SPICE_MSGC_METRIC_BYTES_DISPLAYED_PER_SECOND:
        min = RECORDER_TWEAK(bps_min);
        max = RECORDER_TWEAK(bps_max);
        break;
    case SPICE_MSGC_METRIC_BYTES_DROPPED_PER_SECOND:
        min = 0;
        max = RECORDER_TWEAK(bps_max);
        break;
    case SPICE_MSGC_METRIC_DECODER_QUEUE_LENGTH:
        min = RECORDER_TWEAK(queue_length_min);
        max = RECORDER_TWEAK(queue_length_max);
        break;
    default:
        spice_warning("stream_metrics: received unknown metric %u", metric_id);
        return FALSE;
    }

    // Adjust the metric, smoothing over time using 'percent'
    if (value >= min && value <= max) {
        if (!metrics->weight[metric_id]) {
            percent = 100;
            metrics->weight[metric_id] = 1;
        }
        uint32_t previous = metrics->average[metric_id];
        average = (percent * value + (100-percent) * previous) / 100;
        metrics->average[metric_id] = average;
        metrics->value[metric_id] = value;
    }

    // Now perform comparisons to try to detect bad scenarios
    if (stream_smarts(metric_id, value, average, metrics, &adjusted)) {
#define SEND_ADJUSTMENT(parm, msg)                                      \
        if (adjusted.parm != saved.parm) {                              \
            channel->adjust_cb(channel->adjust_opaque,                  \
                               msg, adjusted.parm, channel);            \
        }
        SEND_ADJUSTMENT(frames_per_second, STREAM_MSG_PARAMETER_FRAMES_PER_SECOND);
        SEND_ADJUSTMENT(max_bytes_per_second, STREAM_MSG_PARAMETER_MAX_BYTES_PER_SECOND);
        SEND_ADJUSTMENT(average_bytes_per_second, STREAM_MSG_PARAMETER_AVERAGE_BYTES_PER_SECOND);
        SEND_ADJUSTMENT(gop_length, STREAM_MSG_PARAMETER_GROUP_OF_PICTURE_SIZE);
        SEND_ADJUSTMENT(quality, STREAM_MSG_PARAMETER_QUALITY);
#undef SEND_ADJUSTMENT
        channel->encoder_parameters = adjusted;
    }

    return TRUE;

}

static bool
handle_message(RedChannelClient *rcc, uint16_t type, uint32_t size, void *msg)
{
    switch (type) {
    case SPICE_MSGC_DISPLAY_INIT:
    case SPICE_MSGC_DISPLAY_PREFERRED_COMPRESSION:
        return true;
    case SPICE_MSGC_DISPLAY_STREAM_REPORT:
        /* TODO these will help tune the streaming reducing/increasing quality */
        return true;
    case SPICE_MSGC_DISPLAY_STREAM_METRIC:
        /* TODO these will help tune the streaming reducing/increasing quality */
        return handle_stream_metric(rcc, (SpiceMsgcDisplayStreamMetric *)msg);
    case SPICE_MSGC_DISPLAY_GL_DRAW_DONE:
        /* client should not send this message */
        return false;
    default:
        return red_channel_client_handle_message(rcc, type, size, msg);
    }
}


StreamChannel*
stream_channel_new(RedsState *server, uint32_t id)
{
    return g_object_new(TYPE_STREAM_CHANNEL,
                        "spice-server", server,
                        "core-interface", reds_get_core_interface(server),
                        "channel-type", SPICE_CHANNEL_DISPLAY,
                        // TODO this id should be after all qxl devices
                        "id", id,
                        "migration-flags", 0,
                        "handle-acks", TRUE, // TODO sure ??
                        NULL);
}

#define MAX_SUPPORTED_CODECS SPICE_VIDEO_CODEC_TYPE_ENUM_END

// find common codecs supported by all clients
static uint8_t
stream_channel_get_supported_codecs(StreamChannel *channel, uint8_t *out_codecs)
{
    RedChannelClient *rcc;
    int codec;

    static const uint16_t codec2cap[] = {
        0, // invalid
        SPICE_DISPLAY_CAP_CODEC_MJPEG,
        SPICE_DISPLAY_CAP_CODEC_VP8,
        SPICE_DISPLAY_CAP_CODEC_H264,
        SPICE_DISPLAY_CAP_CODEC_VP9,
        SPICE_DISPLAY_CAP_CODEC_H265,
    };

    bool supported[SPICE_N_ELEMENTS(codec2cap)];

    for (codec = 0; codec < SPICE_N_ELEMENTS(codec2cap); ++codec) {
        supported[codec] = true;
    }

    FOREACH_CLIENT(channel, rcc) {
        for (codec = 1; codec < SPICE_N_ELEMENTS(codec2cap); ++codec) {
            // if do not support codec delete from list
            if (!red_channel_client_test_remote_cap(rcc, codec2cap[codec])) {
                supported[codec] = false;
            }
        }
    }

    // surely mjpeg is supported
    supported[SPICE_VIDEO_CODEC_TYPE_MJPEG] = true;

    int num = 0;
    for (codec = 1; codec < SPICE_N_ELEMENTS(codec2cap); ++codec) {
        if (supported[codec]) {
            out_codecs[num++] = codec;
        }
    }

    return num;
}

static void
stream_channel_connect(RedChannel *red_channel, RedClient *red_client, RedStream *stream,
                       int migration, RedChannelCapabilities *caps)
{
    StreamChannel *channel = STREAM_CHANNEL(red_channel);
    StreamChannelClient *client;
    struct {
        StreamMsgStartStop base;
        uint8_t codecs_buffer[MAX_SUPPORTED_CODECS];
    } start_msg;
    StreamMsgStartStop *const start = &start_msg.base;

    spice_return_if_fail(stream != NULL);

    client = stream_channel_client_new(channel, red_client, stream, migration, caps);
    spice_return_if_fail(client != NULL);

    // request new stream
    start->num_codecs = stream_channel_get_supported_codecs(channel, start->codecs);
    // send in any case, even if list is not changed
    // notify device about changes
    request_new_stream(channel, start);


    // see guest_set_client_capabilities
    RedChannelClient *rcc = RED_CHANNEL_CLIENT(client);
    red_channel_client_push_set_ack(rcc);

    // TODO what should happen on migration, dcc return if on migration wait ??
    red_channel_client_ack_zero_messages_window(rcc);

    // "emulate" dcc_start
    red_channel_client_pipe_add_empty_msg(rcc, SPICE_MSG_DISPLAY_INVAL_ALL_PALETTES);

    // only if "surface"
    if (channel->width == 0 || channel->height == 0) {
        return;
    }

    // pass proper data
    red_channel_client_pipe_add_type(rcc, RED_PIPE_ITEM_TYPE_SURFACE_CREATE);
    red_channel_client_pipe_add_type(rcc, RED_PIPE_ITEM_TYPE_MONITORS_CONFIG);
    // surface data
    red_channel_client_pipe_add_type(rcc, RED_PIPE_ITEM_TYPE_FILL_SURFACE);
    // TODO monitor configs ??
    red_channel_client_pipe_add_empty_msg(rcc, SPICE_MSG_DISPLAY_MARK);
}

static void
stream_channel_constructed(GObject *object)
{
    ClientCbs client_cbs = { NULL, };
    RedChannel *red_channel = RED_CHANNEL(object);
    RedsState *reds = red_channel_get_server(red_channel);

    G_OBJECT_CLASS(stream_channel_parent_class)->constructed(object);

    client_cbs.connect = stream_channel_connect;
    red_channel_register_client_cbs(red_channel, &client_cbs);

    red_channel_set_cap(red_channel, SPICE_DISPLAY_CAP_MONITORS_CONFIG);
    red_channel_set_cap(red_channel, SPICE_DISPLAY_CAP_STREAM_REPORT);

    reds_register_channel(reds, red_channel);
}

static void
stream_channel_class_init(StreamChannelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    RedChannelClass *channel_class = RED_CHANNEL_CLASS(klass);

    object_class->constructed = stream_channel_constructed;

    channel_class->parser = spice_get_client_channel_parser(SPICE_CHANNEL_DISPLAY, NULL);
    channel_class->handle_message = handle_message;

    channel_class->send_item = stream_channel_send_item;
}

static void
stream_channel_init(StreamChannel *channel)
{
    channel->stream_id = -1;
    channel->width = 0;
    channel->height = 0;
}

void
stream_channel_change_format(StreamChannel *channel, const StreamMsgFormat *fmt)
{
    RedChannel *red_channel = RED_CHANNEL(channel);

    // send destroy old stream
    red_channel_pipes_add_type(red_channel, RED_PIPE_ITEM_TYPE_STREAM_DESTROY);

    // send new create surface if required
    if (channel->width != fmt->width || channel->height != fmt->height) {
        if (channel->width != 0 && channel->height != 0) {
            red_channel_pipes_add_type(red_channel, RED_PIPE_ITEM_TYPE_SURFACE_DESTROY);
        }
        channel->width = fmt->width;
        channel->height = fmt->height;
        red_channel_pipes_add_type(red_channel, RED_PIPE_ITEM_TYPE_SURFACE_CREATE);
        red_channel_pipes_add_type(red_channel, RED_PIPE_ITEM_TYPE_MONITORS_CONFIG);
        // TODO monitors config ??
        red_channel_pipes_add_empty_msg(red_channel, SPICE_MSG_DISPLAY_MARK);
    }

    // allocate a new stream id
    channel->stream_id = (channel->stream_id + 1) % NUM_STREAMS;

    // send create stream
    StreamCreateItem *item = g_new0(StreamCreateItem, 1);
    red_pipe_item_init(&item->base, RED_PIPE_ITEM_TYPE_STREAM_CREATE);
    item->stream_create.id = channel->stream_id;
    item->stream_create.flags = SPICE_STREAM_FLAGS_TOP_DOWN;
    item->stream_create.codec_type = fmt->codec;
    item->stream_create.stream_width = fmt->width;
    item->stream_create.stream_height = fmt->height;
    item->stream_create.src_width = fmt->width;
    item->stream_create.src_height = fmt->height;
    item->stream_create.dest = (SpiceRect) { 0, 0, fmt->width, fmt->height };
    item->stream_create.clip = (SpiceClip) { SPICE_CLIP_TYPE_NONE, NULL };
    red_channel_pipes_add(red_channel, &item->base);

    // activate stream report if possible
    red_channel_pipes_add_type(red_channel, RED_PIPE_ITEM_TYPE_STREAM_ACTIVATE_METRICS);
}

static inline void
stream_channel_update_queue_stat(StreamChannel *channel,
                                 int32_t num_diff, int32_t size_diff)
{
    channel->queue_stat.num_items += num_diff;
    channel->queue_stat.size += size_diff;
    record(stream_stat, "Queue stat: add %d items (total %d), size %d (total %d)",
           num_diff, channel->queue_stat.num_items,
           size_diff, channel->queue_stat.size);
    if (channel->queue_cb) {
        channel->queue_cb(channel->queue_opaque, &channel->queue_stat, channel);
    }
}

static void
data_item_free(RedPipeItem *base)
{
    StreamDataItem *pipe_item = SPICE_UPCAST(StreamDataItem, base);

    stream_channel_update_queue_stat(pipe_item->channel, -1, -pipe_item->data.data_size);

    g_free(pipe_item);
}

void
stream_channel_send_data(StreamChannel *channel, const void *data, size_t size, uint32_t mm_time)
{
    record(stream_send, "Sending %u items mm_time %u buffer %p to channel %d",
           size, mm_time, data, channel->stream_id);
    if (channel->stream_id < 0) {
        // this condition can happen if the guest didn't handle
        // the format stop that we send so think the stream is still
        // started
        return;
    }

    RedChannel *red_channel = RED_CHANNEL(channel);

    StreamDataItem *item = g_malloc(sizeof(*item) + size);
    red_pipe_item_init_full(&item->base, RED_PIPE_ITEM_TYPE_STREAM_DATA,
                            data_item_free);
    item->data.base.id = channel->stream_id;
    item->data.base.multi_media_time = mm_time;
    item->data.data_size = size;
    item->channel = channel;
    stream_channel_update_queue_stat(channel, 1, size);
    // TODO try to optimize avoiding the copy
    memcpy(item->data.data, data, size);
    red_channel_pipes_add(red_channel, &item->base);
}

void
stream_channel_register_start_cb(StreamChannel *channel,
                                 stream_channel_start_proc cb, void *opaque)
{
    channel->start_cb = cb;
    channel->start_opaque = opaque;
}

void
stream_channel_register_queue_stat_cb(StreamChannel *channel,
                                      stream_channel_queue_stat_proc cb, void *opaque)
{
    channel->queue_cb = cb;
    channel->queue_opaque = opaque;
}

void
stream_channel_register_adjust_cb(StreamChannel *channel,
                                  stream_channel_adjust_proc cb, void *opaque)
{
    channel->adjust_cb = cb;
    channel->adjust_opaque = opaque;
}

void
stream_channel_reset(StreamChannel *channel)
{
    struct {
        StreamMsgStartStop base;
        uint8_t codecs_buffer[MAX_SUPPORTED_CODECS];
    } start_msg;
    StreamMsgStartStop *const start = &start_msg.base;
    RedChannel *red_channel = RED_CHANNEL(channel);

    // send destroy old stream
    red_channel_pipes_add_type(red_channel, RED_PIPE_ITEM_TYPE_STREAM_DESTROY);

    // destroy display surface
    if (channel->width != 0 && channel->height != 0) {
        red_channel_pipes_add_type(red_channel, RED_PIPE_ITEM_TYPE_SURFACE_DESTROY);
    }

    channel->stream_id = -1;
    channel->width = 0;
    channel->height = 0;

    if (!red_channel_is_connected(red_channel)) {
        return;
    }

    // try to request a new stream, this should start a new stream
    // if the guest is connected to the device and a client is already connected
    start->num_codecs = stream_channel_get_supported_codecs(channel, start->codecs);
    // send in any case, even if list is not changed
    // notify device about changes
    request_new_stream(channel, start);
}
