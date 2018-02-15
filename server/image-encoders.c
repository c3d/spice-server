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
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <glib.h>

#include "spice-bitmap-utils.h"
#include "image-encoders.h"
#include "pixmap-cache.h"  // MAX_CACHE_CLIENTS
#include "red-parse-qxl.h" // red_drawable_unref

#define ZLIB_DEFAULT_COMPRESSION_LEVEL 3

#define ENCODER_MESSAGE_SIZE 512

#define MAX_GLZ_DRAWABLE_INSTANCES 2

typedef struct RedGlzDrawable          RedGlzDrawable;
typedef struct GlzDrawableInstanceItem GlzDrawableInstanceItem;

struct GlzSharedDictionary
{
    GlzEncDictContext *dict;
    uint32_t           refs;
    uint8_t            id;
    pthread_rwlock_t   encode_lock;
    int                migrate_freeze;
    RedClient *        client; // channel clients of the same client share the dict
};

/* for each qxl drawable, there may be several instances of lz drawables */
/* TODO - reuse this stuff for the top level. I just added a second level of
 * multiplicity
 * at the Drawable by keeping a ring, so:
 * Drawable -> (ring of) RedGlzDrawable -> (up to 2) GlzDrawableInstanceItem
 * and it should probably (but need to be sure...) be
 * Drawable -> ring of GlzDrawableInstanceItem.
 */
struct GlzDrawableInstanceItem
{
    RingItem                glz_link;
    RingItem                free_link;
    GlzEncDictImageContext *context;
    RedGlzDrawable *        glz_drawable;
};

struct RedGlzDrawable
{
    RingItem                link; // ordered by the time it was encoded
    RingItem                drawable_link;
    RedDrawable *           red_drawable;
    GlzDrawableInstanceItem instances_pool[MAX_GLZ_DRAWABLE_INSTANCES];
    Ring                    instances;
    uint8_t                 instances_count;
    gboolean                has_drawable;
    ImageEncoders *         encoders;
};

#define LINK_TO_GLZ(ptr) SPICE_CONTAINEROF((ptr), RedGlzDrawable, drawable_link)
#define DRAWABLE_FOREACH_GLZ_SAFE(drawable, link, next, glz)                                       \
    SAFE_FOREACH(link, next, drawable, &(drawable)->glz_retention.ring, glz, LINK_TO_GLZ(link))

static void glz_drawable_instance_item_free(GlzDrawableInstanceItem *instance);
static void encoder_data_init(EncoderData *data);
static void encoder_data_reset(EncoderData *data);
static void image_encoders_release_glz(ImageEncoders *enc);

static SPICE_GNUC_NORETURN
    SPICE_GNUC_PRINTF(2, 3) void quic_usr_error(QuicUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    va_list      ap;
    char         message_buf[ENCODER_MESSAGE_SIZE];

    va_start(ap, fmt);
    vsnprintf(message_buf, sizeof(message_buf), fmt, ap);
    va_end(ap);
    spice_critical("%s", message_buf);

    longjmp(usr_data->jmp_env, 1);
}

static SPICE_GNUC_NORETURN
    SPICE_GNUC_PRINTF(2, 3) void lz_usr_error(LzUsrContext *usr, const char *fmt, ...)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    va_list      ap;
    char         message_buf[ENCODER_MESSAGE_SIZE];

    va_start(ap, fmt);
    vsnprintf(message_buf, sizeof(message_buf), fmt, ap);
    va_end(ap);
    spice_critical("%s", message_buf);

    longjmp(usr_data->jmp_env, 1);
}

static SPICE_GNUC_PRINTF(2, 3) void glz_usr_error(GlzEncoderUsrContext *usr, const char *fmt, ...)
{
    va_list ap;
    char    message_buf[ENCODER_MESSAGE_SIZE];

    va_start(ap, fmt);
    vsnprintf(message_buf, sizeof(message_buf), fmt, ap);
    va_end(ap);

    spice_critical("%s", message_buf); // if global lz fails in the middle
                                       // the consequences are not predictable since the window
                                       // can turn to be unsynchronized between the server and
                                       // and the client
}

static SPICE_GNUC_PRINTF(2, 3) void quic_usr_warn(QuicUsrContext *usr, const char *fmt, ...)
{
    va_list ap;
    char    message_buf[ENCODER_MESSAGE_SIZE];

    va_start(ap, fmt);
    vsnprintf(message_buf, sizeof(message_buf), fmt, ap);
    va_end(ap);
    spice_warning("%s", message_buf);
}

static SPICE_GNUC_PRINTF(2, 3) void lz_usr_warn(LzUsrContext *usr, const char *fmt, ...)
{
    va_list ap;
    char    message_buf[ENCODER_MESSAGE_SIZE];

    va_start(ap, fmt);
    vsnprintf(message_buf, sizeof(message_buf), fmt, ap);
    va_end(ap);
    spice_warning("%s", message_buf);
}

static SPICE_GNUC_PRINTF(2, 3) void glz_usr_warn(GlzEncoderUsrContext *usr, const char *fmt, ...)
{
    va_list ap;
    char    message_buf[ENCODER_MESSAGE_SIZE];

    va_start(ap, fmt);
    vsnprintf(message_buf, sizeof(message_buf), fmt, ap);
    va_end(ap);
    spice_warning("%s", message_buf);
}

static void *quic_usr_malloc(QuicUsrContext *usr, int size) { return g_malloc(size); }

static void *lz_usr_malloc(LzUsrContext *usr, int size) { return g_malloc(size); }

static void *glz_usr_malloc(GlzEncoderUsrContext *usr, int size) { return g_malloc(size); }

static void quic_usr_free(QuicUsrContext *usr, void *ptr) { g_free(ptr); }

static void lz_usr_free(LzUsrContext *usr, void *ptr) { g_free(ptr); }

static void glz_usr_free(GlzEncoderUsrContext *usr, void *ptr) { g_free(ptr); }

static void encoder_data_init(EncoderData *data)
{
    data->bufs_tail            = g_new(RedCompressBuf, 1);
    data->bufs_head            = data->bufs_tail;
    data->bufs_head->send_next = NULL;
}

static void encoder_data_reset(EncoderData *data)
{
    RedCompressBuf *buf = data->bufs_head;
    while (buf) {
        RedCompressBuf *next = buf->send_next;
        g_free(buf);
        buf = next;
    }
    data->bufs_head = data->bufs_tail = NULL;
}

/* Allocate more space for compressed buffer.
 * The pointer returned in io_ptr is garanteed to be aligned to 4 bytes.
 */
static int encoder_usr_more_space(EncoderData *enc_data, uint8_t **io_ptr)
{
    RedCompressBuf *buf;

    buf                            = g_new(RedCompressBuf, 1);
    enc_data->bufs_tail->send_next = buf;
    enc_data->bufs_tail            = buf;
    buf->send_next                 = NULL;
    *io_ptr                        = buf->buf.bytes;
    return sizeof(buf->buf);
}

static int quic_usr_more_space(QuicUsrContext *usr, uint32_t **io_ptr, int rows_completed)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    return encoder_usr_more_space(usr_data, (uint8_t **)io_ptr) / sizeof(uint32_t);
}

static int lz_usr_more_space(LzUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}

static int glz_usr_more_space(GlzEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}

static int jpeg_usr_more_space(JpegEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((JpegData *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}

#ifdef USE_LZ4
static int lz4_usr_more_space(Lz4EncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((Lz4Data *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}
#endif

static int zlib_usr_more_space(ZlibEncoderUsrContext *usr, uint8_t **io_ptr)
{
    EncoderData *usr_data = &(((ZlibData *)usr)->data);
    return encoder_usr_more_space(usr_data, io_ptr);
}

static inline int encoder_usr_more_lines(EncoderData *enc_data, uint8_t **lines)
{
    struct SpiceChunk *chunk;

    if (enc_data->u.lines_data.reverse) {
        if (!(enc_data->u.lines_data.next >= 0)) {
            return 0;
        }
    } else {
        if (!(enc_data->u.lines_data.next < enc_data->u.lines_data.chunks->num_chunks)) {
            return 0;
        }
    }

    chunk = &enc_data->u.lines_data.chunks->chunk[enc_data->u.lines_data.next];
    if (chunk->len % enc_data->u.lines_data.stride) {
        return 0;
    }

    if (enc_data->u.lines_data.reverse) {
        enc_data->u.lines_data.next--;
        *lines = chunk->data + chunk->len - enc_data->u.lines_data.stride;
    } else {
        enc_data->u.lines_data.next++;
        *lines = chunk->data;
    }

    return chunk->len / enc_data->u.lines_data.stride;
}

static int quic_usr_more_lines(QuicUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((QuicData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int lz_usr_more_lines(LzUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((LzData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int glz_usr_more_lines(GlzEncoderUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((GlzData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

static int jpeg_usr_more_lines(JpegEncoderUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((JpegData *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}

#ifdef USE_LZ4
static int lz4_usr_more_lines(Lz4EncoderUsrContext *usr, uint8_t **lines)
{
    EncoderData *usr_data = &(((Lz4Data *)usr)->data);
    return encoder_usr_more_lines(usr_data, lines);
}
#endif

static int zlib_usr_more_input(ZlibEncoderUsrContext *usr, uint8_t **input)
{
    EncoderData *usr_data = &(((ZlibData *)usr)->data);
    int          buf_size;

    if (!usr_data->u.compressed_data.next) {
        spice_assert(usr_data->u.compressed_data.size_left == 0);
        return 0;
    }

    *input = usr_data->u.compressed_data.next->buf.bytes;
    buf_size =
        MIN(sizeof(usr_data->u.compressed_data.next->buf), usr_data->u.compressed_data.size_left);

    usr_data->u.compressed_data.next = usr_data->u.compressed_data.next->send_next;
    usr_data->u.compressed_data.size_left -= buf_size;
    return buf_size;
}

static void image_encoders_init_quic(ImageEncoders *enc)
{
    enc->quic_data.usr.error      = quic_usr_error;
    enc->quic_data.usr.warn       = quic_usr_warn;
    enc->quic_data.usr.info       = quic_usr_warn;
    enc->quic_data.usr.malloc     = quic_usr_malloc;
    enc->quic_data.usr.free       = quic_usr_free;
    enc->quic_data.usr.more_space = quic_usr_more_space;
    enc->quic_data.usr.more_lines = quic_usr_more_lines;

    enc->quic = quic_create(&enc->quic_data.usr);

    if (!enc->quic) {
        spice_critical("create quic failed");
    }
}

static void image_encoders_init_lz(ImageEncoders *enc)
{
    enc->lz_data.usr.error      = lz_usr_error;
    enc->lz_data.usr.warn       = lz_usr_warn;
    enc->lz_data.usr.info       = lz_usr_warn;
    enc->lz_data.usr.malloc     = lz_usr_malloc;
    enc->lz_data.usr.free       = lz_usr_free;
    enc->lz_data.usr.more_space = lz_usr_more_space;
    enc->lz_data.usr.more_lines = lz_usr_more_lines;

    enc->lz = lz_create(&enc->lz_data.usr);

    if (!enc->lz) {
        spice_critical("create lz failed");
    }
}

static void glz_usr_free_image(GlzEncoderUsrContext *usr, GlzUsrImageContext *image)
{
    GlzData *                lz_data               = (GlzData *)usr;
    GlzDrawableInstanceItem *glz_drawable_instance = (GlzDrawableInstanceItem *)image;
    ImageEncoders *          drawable_enc          = glz_drawable_instance->glz_drawable->encoders;
    ImageEncoders *          this_enc = SPICE_CONTAINEROF(lz_data, ImageEncoders, glz_data);
    if (this_enc == drawable_enc) {
        glz_drawable_instance_item_free(glz_drawable_instance);
    } else {
        /* The glz dictionary is shared between all DisplayChannelClient
         * instances that belong to the same client, and glz_usr_free_image
         * can be called by the dictionary code
         * (glz_dictionary_window_remove_head). Thus this function can be
         * called from any DisplayChannelClient thread, hence the need for
         * this check.
         */
        pthread_mutex_lock(&drawable_enc->glz_drawables_inst_to_free_lock);
        ring_add_before(&glz_drawable_instance->free_link,
                        &drawable_enc->glz_drawables_inst_to_free);
        pthread_mutex_unlock(&drawable_enc->glz_drawables_inst_to_free_lock);
    }
}

static void image_encoders_init_glz_data(ImageEncoders *enc)
{
    enc->glz_data.usr.error      = glz_usr_error;
    enc->glz_data.usr.warn       = glz_usr_warn;
    enc->glz_data.usr.info       = glz_usr_warn;
    enc->glz_data.usr.malloc     = glz_usr_malloc;
    enc->glz_data.usr.free       = glz_usr_free;
    enc->glz_data.usr.more_space = glz_usr_more_space;
    enc->glz_data.usr.more_lines = glz_usr_more_lines;
    enc->glz_data.usr.free_image = glz_usr_free_image;
}

static void image_encoders_init_jpeg(ImageEncoders *enc)
{
    enc->jpeg_data.usr.more_space = jpeg_usr_more_space;
    enc->jpeg_data.usr.more_lines = jpeg_usr_more_lines;

    enc->jpeg = jpeg_encoder_create(&enc->jpeg_data.usr);

    if (!enc->jpeg) {
        spice_critical("create jpeg encoder failed");
    }
}

#ifdef USE_LZ4
static inline void image_encoders_init_lz4(ImageEncoders *enc)
{
    enc->lz4_data.usr.more_space = lz4_usr_more_space;
    enc->lz4_data.usr.more_lines = lz4_usr_more_lines;

    enc->lz4 = lz4_encoder_create(&enc->lz4_data.usr);

    if (!enc->lz4) {
        spice_critical("create lz4 encoder failed");
    }
}
#endif

static void image_encoders_init_zlib(ImageEncoders *enc)
{
    enc->zlib_data.usr.more_space = zlib_usr_more_space;
    enc->zlib_data.usr.more_input = zlib_usr_more_input;

    enc->zlib = zlib_encoder_create(&enc->zlib_data.usr, ZLIB_DEFAULT_COMPRESSION_LEVEL);

    if (!enc->zlib) {
        spice_critical("create zlib encoder failed");
    }
}

void image_encoders_init(ImageEncoders *enc, ImageEncoderSharedData *shared_data)
{
    spice_assert(shared_data);
    enc->shared_data = shared_data;

    ring_init(&enc->glz_drawables);
    ring_init(&enc->glz_drawables_inst_to_free);
    pthread_mutex_init(&enc->glz_drawables_inst_to_free_lock, NULL);

    image_encoders_init_glz_data(enc);
    image_encoders_init_quic(enc);
    image_encoders_init_lz(enc);
    image_encoders_init_jpeg(enc);
#ifdef USE_LZ4
    image_encoders_init_lz4(enc);
#endif
    image_encoders_init_zlib(enc);

    // todo: tune level according to bandwidth
    enc->zlib_level = ZLIB_DEFAULT_COMPRESSION_LEVEL;
}

void image_encoders_free(ImageEncoders *enc)
{
    image_encoders_release_glz(enc);
    quic_destroy(enc->quic);
    enc->quic = NULL;
    lz_destroy(enc->lz);
    enc->lz = NULL;
    jpeg_encoder_destroy(enc->jpeg);
    enc->jpeg = NULL;
#ifdef USE_LZ4
    lz4_encoder_destroy(enc->lz4);
    enc->lz4 = NULL;
#endif
    zlib_encoder_destroy(enc->zlib);
    enc->zlib = NULL;
    pthread_mutex_destroy(&enc->glz_drawables_inst_to_free_lock);
}

/* Remove from the to_free list and the instances_list.
   When no instance is left - the RedGlzDrawable is released too. (and the qxl
   drawable too, if
   it is not used by Drawable).
   NOTE - 1) can be called only by the display channel that created the drawable
          2) it is assumed that the instance was already removed from the
   dictionary*/
static void glz_drawable_instance_item_free(GlzDrawableInstanceItem *instance)
{
    RedGlzDrawable *glz_drawable;

    spice_assert(instance);
    spice_assert(instance->glz_drawable);

    glz_drawable = instance->glz_drawable;

    spice_assert(glz_drawable->instances_count > 0);

    ring_remove(&instance->glz_link);
    glz_drawable->instances_count--;

    // when the remove callback is performed from the channel that the
    // drawable belongs to, the instance is not added to the 'to_free' list
    if (ring_item_is_linked(&instance->free_link)) {
        ring_remove(&instance->free_link);
    }

    if (ring_is_empty(&glz_drawable->instances)) {
        spice_assert(glz_drawable->instances_count == 0);

        if (glz_drawable->has_drawable) {
            ring_remove(&glz_drawable->drawable_link);
        }
        red_drawable_unref(glz_drawable->red_drawable);
        glz_drawable->encoders->shared_data->glz_drawable_count--;
        if (ring_item_is_linked(&glz_drawable->link)) {
            ring_remove(&glz_drawable->link);
        }
        g_free(glz_drawable);
    }
}

/*
 * Releases all the instances of the drawable from the dictionary and the
 * display channel client.
 * The release of the last instance will also release the drawable itself and
 * the qxl drawable
 * if possible.
 * NOTE - the caller should prevent encoding using the dictionary during this
 * operation
 */
static void red_glz_drawable_free(RedGlzDrawable *glz_drawable)
{
    ImageEncoders *enc           = glz_drawable->encoders;
    RingItem *     head_instance = ring_get_head(&glz_drawable->instances);
    int            cont          = (head_instance != NULL);

    while (cont) {
        if (glz_drawable->instances_count == 1) {
            /* Last instance: glz_drawable_instance_item_free will free the
             * glz_drawable */
            cont = FALSE;
        }
        GlzDrawableInstanceItem *instance =
            SPICE_CONTAINEROF(head_instance, GlzDrawableInstanceItem, glz_link);
        if (!ring_item_is_linked(&instance->free_link)) {
            // the instance didn't get out from window yet
            glz_enc_dictionary_remove_image(enc->glz_dict->dict, instance->context,
                                            &enc->glz_data.usr);
        }
        glz_drawable_instance_item_free(instance);

        if (cont) {
            head_instance = ring_get_head(&glz_drawable->instances);
        }
    }
}

gboolean image_encoders_glz_encode_lock(ImageEncoders *enc)
{
    if (enc->glz_dict) {
        pthread_rwlock_wrlock(&enc->glz_dict->encode_lock);
        return TRUE;
    }
    return FALSE;
}

void image_encoders_glz_encode_unlock(ImageEncoders *enc)
{
    if (enc->glz_dict) {
        pthread_rwlock_unlock(&enc->glz_dict->encode_lock);
    }
}

/*
 * Remove from the global lz dictionary some glz_drawables that have no
 * reference to
 * Drawable (their qxl drawables are released too).
 * NOTE - the caller should prevent encoding using the dictionary during the
 * operation
 */
int image_encoders_free_some_independent_glz_drawables(ImageEncoders *enc)
{
    RingItem *ring_link;
    int       n = 0;

    if (!enc) {
        return 0;
    }
    ring_link = ring_get_head(&enc->glz_drawables);
    while ((n < RED_RELEASE_BUNCH_SIZE) && (ring_link != NULL)) {
        RedGlzDrawable *glz_drawable = SPICE_CONTAINEROF(ring_link, RedGlzDrawable, link);
        ring_link                    = ring_next(&enc->glz_drawables, ring_link);
        if (!glz_drawable->has_drawable) {
            red_glz_drawable_free(glz_drawable);
            n++;
        }
    }
    return n;
}

void image_encoders_free_glz_drawables_to_free(ImageEncoders *enc)
{
    RingItem *ring_link;

    if (!enc->glz_dict) {
        return;
    }
    pthread_mutex_lock(&enc->glz_drawables_inst_to_free_lock);
    while ((ring_link = ring_get_head(&enc->glz_drawables_inst_to_free))) {
        GlzDrawableInstanceItem *drawable_instance =
            SPICE_CONTAINEROF(ring_link, GlzDrawableInstanceItem, free_link);
        glz_drawable_instance_item_free(drawable_instance);
    }
    pthread_mutex_unlock(&enc->glz_drawables_inst_to_free_lock);
}

/* Clear all lz drawables - enforce their removal from the global dictionary.
   NOTE - prevents encoding using the dictionary during the operation*/
void image_encoders_free_glz_drawables(ImageEncoders *enc)
{
    RingItem *           ring_link;
    GlzSharedDictionary *glz_dict = enc ? enc->glz_dict : NULL;

    if (!glz_dict) {
        return;
    }

    // assure no display channel is during global lz encoding
    pthread_rwlock_wrlock(&glz_dict->encode_lock);
    while ((ring_link = ring_get_head(&enc->glz_drawables))) {
        RedGlzDrawable *drawable = SPICE_CONTAINEROF(ring_link, RedGlzDrawable, link);
        // no need to lock the to_free list, since we assured no other thread is
        // encoding and
        // thus not other thread access the to_free list of the channel
        red_glz_drawable_free(drawable);
    }
    pthread_rwlock_unlock(&glz_dict->encode_lock);
}

void glz_retention_free_drawables(GlzImageRetention *ret)
{
    RingItem *      glz_item, *next_item;
    RedGlzDrawable *glz;
    SAFE_FOREACH(glz_item, next_item, TRUE, &ret->ring, glz, LINK_TO_GLZ(glz_item))
    {
        red_glz_drawable_free(glz);
    }
}

void glz_retention_detach_drawables(GlzImageRetention *ret)
{
    RingItem *item, *next;

    RING_FOREACH_SAFE(item, next, &ret->ring)
    {
        SPICE_CONTAINEROF(item, RedGlzDrawable, drawable_link)->has_drawable = FALSE;
        ring_remove(item);
    }
}

static void image_encoders_freeze_glz(ImageEncoders *enc)
{
    pthread_rwlock_wrlock(&enc->glz_dict->encode_lock);
    enc->glz_dict->migrate_freeze = TRUE;
    pthread_rwlock_unlock(&enc->glz_dict->encode_lock);
}

void image_encoders_glz_get_restore_data(ImageEncoders *        enc,
                                         uint8_t *              out_id,
                                         GlzEncDictRestoreData *out_data)
{
    spice_assert(enc->glz_dict);
    image_encoders_freeze_glz(enc);
    *out_id = enc->glz_dict->id;
    glz_enc_dictionary_get_restore_data(enc->glz_dict->dict, out_data, &enc->glz_data.usr);
}

static GlzSharedDictionary *
glz_shared_dictionary_new(RedClient *client, uint8_t id, GlzEncDictContext *dict)
{
    spice_return_val_if_fail(dict != NULL, NULL);

    GlzSharedDictionary *shared_dict = g_new0(GlzSharedDictionary, 1);

    shared_dict->dict           = dict;
    shared_dict->id             = id;
    shared_dict->refs           = 1;
    shared_dict->migrate_freeze = FALSE;
    shared_dict->client         = client;
    pthread_rwlock_init(&shared_dict->encode_lock, NULL);

    return shared_dict;
}

static pthread_mutex_t glz_dictionary_list_lock = PTHREAD_MUTEX_INITIALIZER;
static GList *         glz_dictionary_list;

static GlzSharedDictionary *find_glz_dictionary(RedClient *client, uint8_t dict_id)
{
    GList *              l;
    GlzSharedDictionary *ret = NULL;

    for (l = glz_dictionary_list; l != NULL; l = l->next) {
        GlzSharedDictionary *dict = l->data;
        if ((dict->client == client) && (dict->id == dict_id)) {
            ret = dict;
            break;
        }
    }

    return ret;
}

#define MAX_LZ_ENCODERS MAX_CACHE_CLIENTS

static GlzSharedDictionary *
create_glz_dictionary(ImageEncoders *enc, RedClient *client, uint8_t id, int window_size)
{
    spice_debug("Lz Window %d Size=%d", id, window_size);

    GlzEncDictContext *glz_dict =
        glz_enc_dictionary_create(window_size, MAX_LZ_ENCODERS, &enc->glz_data.usr);

    return glz_shared_dictionary_new(client, id, glz_dict);
}

gboolean image_encoders_get_glz_dictionary(ImageEncoders *enc,
                                           RedClient *    client,
                                           uint8_t        id,
                                           int            window_size)
{
    GlzSharedDictionary *shared_dict;

    spice_return_val_if_fail(!enc->glz_dict, FALSE);

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = find_glz_dictionary(client, id);
    if (shared_dict) {
        shared_dict->refs++;
    } else {
        shared_dict = create_glz_dictionary(enc, client, id, window_size);
        if (shared_dict != NULL) {
            glz_dictionary_list = g_list_prepend(glz_dictionary_list, shared_dict);
        }
    }

    pthread_mutex_unlock(&glz_dictionary_list_lock);
    enc->glz_dict = shared_dict;
    return shared_dict != NULL;
}

static GlzSharedDictionary *restore_glz_dictionary(ImageEncoders *        enc,
                                                   RedClient *            client,
                                                   uint8_t                id,
                                                   GlzEncDictRestoreData *restore_data)
{
    GlzEncDictContext *glz_dict = glz_enc_dictionary_restore(restore_data, &enc->glz_data.usr);

    return glz_shared_dictionary_new(client, id, glz_dict);
}

gboolean image_encoders_restore_glz_dictionary(ImageEncoders *        enc,
                                               RedClient *            client,
                                               uint8_t                id,
                                               GlzEncDictRestoreData *restore_data)
{
    GlzSharedDictionary *shared_dict = NULL;

    spice_return_val_if_fail(!enc->glz_dict, FALSE);

    pthread_mutex_lock(&glz_dictionary_list_lock);

    shared_dict = find_glz_dictionary(client, id);

    if (shared_dict) {
        shared_dict->refs++;
    } else {
        shared_dict = restore_glz_dictionary(enc, client, id, restore_data);
        if (shared_dict != NULL) {
            glz_dictionary_list = g_list_prepend(glz_dictionary_list, shared_dict);
        }
    }

    pthread_mutex_unlock(&glz_dictionary_list_lock);
    enc->glz_dict = shared_dict;
    return shared_dict != NULL;
}

gboolean image_encoders_glz_create(ImageEncoders *enc, uint8_t id)
{
    enc->glz = glz_encoder_create(id, enc->glz_dict->dict, &enc->glz_data.usr);
    return enc->glz != NULL;
}

/* destroy encoder, and dictionary if no one uses it*/
static void image_encoders_release_glz(ImageEncoders *enc)
{
    GlzSharedDictionary *shared_dict;

    image_encoders_free_glz_drawables(enc);

    glz_encoder_destroy(enc->glz);
    enc->glz = NULL;

    if (!(shared_dict = enc->glz_dict)) {
        return;
    }

    enc->glz_dict = NULL;
    pthread_mutex_lock(&glz_dictionary_list_lock);
    if (--shared_dict->refs != 0) {
        pthread_mutex_unlock(&glz_dictionary_list_lock);
        return;
    }
    glz_dictionary_list = g_list_remove(glz_dictionary_list, shared_dict);
    pthread_mutex_unlock(&glz_dictionary_list_lock);
    glz_enc_dictionary_destroy(shared_dict->dict, &enc->glz_data.usr);
    pthread_rwlock_destroy(&shared_dict->encode_lock);
    g_free(shared_dict);
}

bool image_encoders_compress_quic(ImageEncoders *       enc,
                                  SpiceImage *          dest,
                                  SpiceBitmap *         src,
                                  compress_send_data_t *o_comp_data)
{
    QuicData *             quic_data = &enc->quic_data;
    QuicContext *          quic      = enc->quic;
    volatile QuicImageType type;
    int                    size, stride;
    stat_start_time_t      start_time;
    stat_start_time_init(&start_time, &enc->shared_data->quic_stat);

#ifdef COMPRESS_DEBUG
    spice_debug("QUIC compress");
#endif

    switch (src->format) {
    case SPICE_BITMAP_FMT_32BIT:
        type = QUIC_IMAGE_TYPE_RGB32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        type = QUIC_IMAGE_TYPE_RGBA;
        break;
    case SPICE_BITMAP_FMT_16BIT:
        type = QUIC_IMAGE_TYPE_RGB16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        type = QUIC_IMAGE_TYPE_RGB24;
        break;
    default:
        return FALSE;
    }

    encoder_data_init(&quic_data->data);

    if (setjmp(quic_data->data.jmp_env)) {
        encoder_data_reset(&quic_data->data);
        return FALSE;
    }

    if (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE) {
        spice_chunks_linearize(src->data);
    }

    quic_data->data.u.lines_data.chunks = src->data;
    quic_data->data.u.lines_data.stride = src->stride;
    if ((src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        quic_data->data.u.lines_data.next    = 0;
        quic_data->data.u.lines_data.reverse = 0;
        stride                               = src->stride;
    } else {
        quic_data->data.u.lines_data.next    = src->data->num_chunks - 1;
        quic_data->data.u.lines_data.reverse = 1;
        stride                               = -src->stride;
    }
    size = quic_encode(quic, type, src->x, src->y, NULL, 0, stride,
                       quic_data->data.bufs_head->buf.words,
                       G_N_ELEMENTS(quic_data->data.bufs_head->buf.words));

    // the compressed buffer is bigger than the original data
    if ((size << 2) > (src->y * src->stride)) {
        longjmp(quic_data->data.jmp_env, 1);
    }

    dest->descriptor.type  = SPICE_IMAGE_TYPE_QUIC;
    dest->u.quic.data_size = size << 2;

    o_comp_data->comp_buf      = quic_data->data.bufs_head;
    o_comp_data->comp_buf_size = size << 2;

    stat_compress_add(&enc->shared_data->quic_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

static const LzImageType bitmap_fmt_to_lz_image_type[] = {
    LZ_IMAGE_TYPE_INVALID, LZ_IMAGE_TYPE_PLT1_LE, LZ_IMAGE_TYPE_PLT1_BE, LZ_IMAGE_TYPE_PLT4_LE,
    LZ_IMAGE_TYPE_PLT4_BE, LZ_IMAGE_TYPE_PLT8,    LZ_IMAGE_TYPE_RGB16,   LZ_IMAGE_TYPE_RGB24,
    LZ_IMAGE_TYPE_RGB32,   LZ_IMAGE_TYPE_RGBA,    LZ_IMAGE_TYPE_A8};

bool image_encoders_compress_lz(ImageEncoders *       enc,
                                SpiceImage *          dest,
                                SpiceBitmap *         src,
                                compress_send_data_t *o_comp_data)
{
    LzData *    lz_data = &enc->lz_data;
    LzContext * lz      = enc->lz;
    LzImageType type    = bitmap_fmt_to_lz_image_type[src->format];
    int         size; // size of the compressed data

    stat_start_time_t start_time;
    stat_start_time_init(&start_time, &enc->shared_data->lz_stat);

#ifdef COMPRESS_DEBUG
    spice_debug("LZ LOCAL compress");
#endif

    encoder_data_init(&lz_data->data);

    if (setjmp(lz_data->data.jmp_env)) {
        encoder_data_reset(&lz_data->data);
        return FALSE;
    }

    lz_data->data.u.lines_data.chunks  = src->data;
    lz_data->data.u.lines_data.stride  = src->stride;
    lz_data->data.u.lines_data.next    = 0;
    lz_data->data.u.lines_data.reverse = 0;

    size = lz_encode(lz, type, src->x, src->y, !!(src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN), NULL,
                     0, src->stride, lz_data->data.bufs_head->buf.bytes,
                     sizeof(lz_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (size > (src->y * src->stride)) {
        longjmp(lz_data->data.jmp_env, 1);
    }

    if (bitmap_fmt_is_rgb(src->format)) {
        dest->descriptor.type    = SPICE_IMAGE_TYPE_LZ_RGB;
        dest->u.lz_rgb.data_size = size;

        o_comp_data->comp_buf      = lz_data->data.bufs_head;
        o_comp_data->comp_buf_size = size;
    } else {
        /* masks are 1BIT bitmaps without palettes, but they are not compressed
         * (see fill_mask) */
        spice_assert(src->palette);
        dest->descriptor.type      = SPICE_IMAGE_TYPE_LZ_PLT;
        dest->u.lz_plt.data_size   = size;
        dest->u.lz_plt.flags       = src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN;
        dest->u.lz_plt.palette     = src->palette;
        dest->u.lz_plt.palette_id  = src->palette->unique;
        o_comp_data->comp_buf      = lz_data->data.bufs_head;
        o_comp_data->comp_buf_size = size;

        o_comp_data->lzplt_palette = dest->u.lz_plt.palette;
    }

    stat_compress_add(&enc->shared_data->lz_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

bool image_encoders_compress_jpeg(ImageEncoders *       enc,
                                  SpiceImage *          dest,
                                  SpiceBitmap *         src,
                                  compress_send_data_t *o_comp_data)
{
    JpegData *                    jpeg_data = &enc->jpeg_data;
    LzData *                      lz_data   = &enc->lz_data;
    JpegEncoderContext *          jpeg      = enc->jpeg;
    LzContext *                   lz        = enc->lz;
    volatile JpegEncoderImageType jpeg_in_type;
    int                           jpeg_size     = 0;
    volatile int                  has_alpha     = FALSE;
    int                           alpha_lz_size = 0;
    int                           comp_head_filled;
    int                           comp_head_left;
    int                           stride;
    uint8_t *                     lz_out_start_byte;
    stat_start_time_t             start_time;
    stat_start_time_init(&start_time, &enc->shared_data->jpeg_alpha_stat);

#ifdef COMPRESS_DEBUG
    spice_debug("JPEG compress");
#endif

    switch (src->format) {
    case SPICE_BITMAP_FMT_16BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_RGB16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGR24;
        break;
    case SPICE_BITMAP_FMT_32BIT:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGRX32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        jpeg_in_type = JPEG_IMAGE_TYPE_BGRX32;
        has_alpha    = TRUE;
        break;
    default:
        return FALSE;
    }

    encoder_data_init(&jpeg_data->data);

    if (setjmp(jpeg_data->data.jmp_env)) {
        encoder_data_reset(&jpeg_data->data);
        return FALSE;
    }

    if (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE) {
        spice_chunks_linearize(src->data);
    }

    jpeg_data->data.u.lines_data.chunks = src->data;
    jpeg_data->data.u.lines_data.stride = src->stride;
    if ((src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN)) {
        jpeg_data->data.u.lines_data.next    = 0;
        jpeg_data->data.u.lines_data.reverse = 0;
        stride                               = src->stride;
    } else {
        jpeg_data->data.u.lines_data.next    = src->data->num_chunks - 1;
        jpeg_data->data.u.lines_data.reverse = 1;
        stride                               = -src->stride;
    }
    jpeg_size =
        jpeg_encode(jpeg, enc->jpeg_quality, jpeg_in_type, src->x, src->y, NULL, 0, stride,
                    jpeg_data->data.bufs_head->buf.bytes, sizeof(jpeg_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (jpeg_size > (src->y * src->stride)) {
        longjmp(jpeg_data->data.jmp_env, 1);
    }

    if (!has_alpha) {
        dest->descriptor.type  = SPICE_IMAGE_TYPE_JPEG;
        dest->u.jpeg.data_size = jpeg_size;

        o_comp_data->comp_buf      = jpeg_data->data.bufs_head;
        o_comp_data->comp_buf_size = jpeg_size;
        o_comp_data->is_lossy      = TRUE;

        stat_compress_add(&enc->shared_data->jpeg_stat, start_time, src->stride * src->y,
                          o_comp_data->comp_buf_size);
        return TRUE;
    }

    lz_data->data.bufs_head = jpeg_data->data.bufs_tail;
    lz_data->data.bufs_tail = lz_data->data.bufs_head;

    comp_head_filled  = jpeg_size % sizeof(lz_data->data.bufs_head->buf);
    comp_head_left    = sizeof(lz_data->data.bufs_head->buf) - comp_head_filled;
    lz_out_start_byte = lz_data->data.bufs_head->buf.bytes + comp_head_filled;

    lz_data->data.u.lines_data.chunks  = src->data;
    lz_data->data.u.lines_data.stride  = src->stride;
    lz_data->data.u.lines_data.next    = 0;
    lz_data->data.u.lines_data.reverse = 0;

    alpha_lz_size = lz_encode(lz, LZ_IMAGE_TYPE_XXXA, src->x, src->y,
                              !!(src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN), NULL, 0, src->stride,
                              lz_out_start_byte, comp_head_left);

    // the compressed buffer is bigger than the original data
    if ((jpeg_size + alpha_lz_size) > (src->y * src->stride)) {
        longjmp(jpeg_data->data.jmp_env, 1);
    }

    dest->descriptor.type    = SPICE_IMAGE_TYPE_JPEG_ALPHA;
    dest->u.jpeg_alpha.flags = 0;
    if (src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN) {
        dest->u.jpeg_alpha.flags |= SPICE_JPEG_ALPHA_FLAGS_TOP_DOWN;
    }

    dest->u.jpeg_alpha.jpeg_size = jpeg_size;
    dest->u.jpeg_alpha.data_size = jpeg_size + alpha_lz_size;

    o_comp_data->comp_buf      = jpeg_data->data.bufs_head;
    o_comp_data->comp_buf_size = jpeg_size + alpha_lz_size;
    o_comp_data->is_lossy      = TRUE;
    stat_compress_add(&enc->shared_data->jpeg_alpha_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}

#ifdef USE_LZ4
bool image_encoders_compress_lz4(ImageEncoders *       enc,
                                 SpiceImage *          dest,
                                 SpiceBitmap *         src,
                                 compress_send_data_t *o_comp_data)
{
    Lz4Data *          lz4_data = &enc->lz4_data;
    Lz4EncoderContext *lz4      = enc->lz4;
    int                lz4_size = 0;
    stat_start_time_t  start_time;
    stat_start_time_init(&start_time, &enc->shared_data->lz4_stat);

#ifdef COMPRESS_DEBUG
    spice_debug("LZ4 compress");
#endif

    encoder_data_init(&lz4_data->data);

    if (setjmp(lz4_data->data.jmp_env)) {
        encoder_data_reset(&lz4_data->data);
        return FALSE;
    }

    if (src->data->flags & SPICE_CHUNKS_FLAGS_UNSTABLE) {
        spice_chunks_linearize(src->data);
    }

    lz4_data->data.u.lines_data.chunks  = src->data;
    lz4_data->data.u.lines_data.stride  = src->stride;
    lz4_data->data.u.lines_data.next    = 0;
    lz4_data->data.u.lines_data.reverse = 0;

    lz4_size = lz4_encode(lz4, src->y, src->stride, lz4_data->data.bufs_head->buf.bytes,
                          sizeof(lz4_data->data.bufs_head->buf),
                          src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN, src->format);

    // the compressed buffer is bigger than the original data
    if (lz4_size > (src->y * src->stride)) {
        longjmp(lz4_data->data.jmp_env, 1);
    }

    dest->descriptor.type = SPICE_IMAGE_TYPE_LZ4;
    dest->u.lz4.data_size = lz4_size;

    o_comp_data->comp_buf      = lz4_data->data.bufs_head;
    o_comp_data->comp_buf_size = lz4_size;

    stat_compress_add(&enc->shared_data->lz4_stat, start_time, src->stride * src->y,
                      o_comp_data->comp_buf_size);
    return TRUE;
}
#endif

/* if already exists, returns it. Otherwise allocates and adds it (1) to the
   ring tail
   in the channel (2) to the Drawable*/
static RedGlzDrawable *
get_glz_drawable(ImageEncoders *enc, RedDrawable *red_drawable, GlzImageRetention *glz_retention)
{
    RedGlzDrawable *ret;
    RingItem *      item, *next;

    // TODO - I don't really understand what's going on here, so doing the
    // technical equivalent
    // now that we have multiple glz_dicts, so the only way to go from dcc to
    // drawable glz is to go
    // over the glz_ring (unless adding some better data structure then a ring)
    SAFE_FOREACH(item, next, TRUE, &glz_retention->ring, ret, LINK_TO_GLZ(item))
    {
        if (ret->encoders == enc) {
            return ret;
        }
    }

    ret = g_new(RedGlzDrawable, 1);

    ret->encoders        = enc;
    ret->red_drawable    = red_drawable_ref(red_drawable);
    ret->has_drawable    = TRUE;
    ret->instances_count = 0;
    ring_init(&ret->instances);

    ring_item_init(&ret->link);
    ring_item_init(&ret->drawable_link);
    ring_add_before(&ret->link, &enc->glz_drawables);
    ring_add(&glz_retention->ring, &ret->drawable_link);
    enc->shared_data->glz_drawable_count++;
    return ret;
}

/* allocates new instance and adds it to instances in the given drawable.
   NOTE - the caller should set the glz_instance returned by the encoder by
   itself.*/
static GlzDrawableInstanceItem *add_glz_drawable_instance(RedGlzDrawable *glz_drawable)
{
    spice_assert(glz_drawable->instances_count < MAX_GLZ_DRAWABLE_INSTANCES);
    // NOTE: We assume the additions are performed consecutively, without removals
    // in the middle
    GlzDrawableInstanceItem *ret = glz_drawable->instances_pool + glz_drawable->instances_count;
    glz_drawable->instances_count++;

    ring_item_init(&ret->free_link);
    ring_item_init(&ret->glz_link);
    ring_add(&glz_drawable->instances, &ret->glz_link);
    ret->context      = NULL;
    ret->glz_drawable = glz_drawable;

    return ret;
}

#define MIN_GLZ_SIZE_FOR_ZLIB 100

bool image_encoders_compress_glz(ImageEncoders *       enc,
                                 SpiceImage *          dest,
                                 SpiceBitmap *         src,
                                 RedDrawable *         red_drawable,
                                 GlzImageRetention *   glz_retention,
                                 compress_send_data_t *o_comp_data,
                                 gboolean              enable_zlib_glz_wrap)
{
    stat_start_time_t start_time;
    stat_start_time_init(&start_time, &enc->shared_data->zlib_glz_stat);
    spice_assert(bitmap_fmt_is_rgb(src->format));
    GlzData *                glz_data = &enc->glz_data;
    ZlibData *               zlib_data;
    LzImageType              type = bitmap_fmt_to_lz_image_type[src->format];
    RedGlzDrawable *         glz_drawable;
    GlzDrawableInstanceItem *glz_drawable_instance;
    int                      glz_size;
    int                      zlib_size;

#ifdef COMPRESS_DEBUG
    spice_debug("LZ global compress fmt=%d", src->format);
#endif

    if ((src->x * src->y) >= glz_enc_dictionary_get_size(enc->glz_dict->dict)) {
        return FALSE;
    }

    pthread_rwlock_rdlock(&enc->glz_dict->encode_lock);
    /* using the global dictionary only if it is not frozen */
    if (enc->glz_dict->migrate_freeze) {
        pthread_rwlock_unlock(&enc->glz_dict->encode_lock);
        return FALSE;
    }

    encoder_data_init(&glz_data->data);

    glz_drawable          = get_glz_drawable(enc, red_drawable, glz_retention);
    glz_drawable_instance = add_glz_drawable_instance(glz_drawable);

    glz_data->data.u.lines_data.chunks  = src->data;
    glz_data->data.u.lines_data.stride  = src->stride;
    glz_data->data.u.lines_data.next    = 0;
    glz_data->data.u.lines_data.reverse = 0;

    glz_size = glz_encode(
        enc->glz, type, src->x, src->y, (src->flags & SPICE_BITMAP_FLAGS_TOP_DOWN), NULL, 0,
        src->stride, glz_data->data.bufs_head->buf.bytes, sizeof(glz_data->data.bufs_head->buf),
        glz_drawable_instance, &glz_drawable_instance->context);

    stat_compress_add(&enc->shared_data->glz_stat, start_time, src->stride * src->y, glz_size);

    if (!enable_zlib_glz_wrap || (glz_size < MIN_GLZ_SIZE_FOR_ZLIB)) {
        goto glz;
    }
    stat_start_time_init(&start_time, &enc->shared_data->zlib_glz_stat);
    zlib_data = &enc->zlib_data;

    encoder_data_init(&zlib_data->data);

    zlib_data->data.u.compressed_data.next      = glz_data->data.bufs_head;
    zlib_data->data.u.compressed_data.size_left = glz_size;

    zlib_size =
        zlib_encode(enc->zlib, enc->zlib_level, glz_size, zlib_data->data.bufs_head->buf.bytes,
                    sizeof(zlib_data->data.bufs_head->buf));

    // the compressed buffer is bigger than the original data
    if (zlib_size >= glz_size) {
        encoder_data_reset(&zlib_data->data);
        goto glz;
    } else {
        encoder_data_reset(&glz_data->data);
    }

    dest->descriptor.type          = SPICE_IMAGE_TYPE_ZLIB_GLZ_RGB;
    dest->u.zlib_glz.glz_data_size = glz_size;
    dest->u.zlib_glz.data_size     = zlib_size;

    o_comp_data->comp_buf      = zlib_data->data.bufs_head;
    o_comp_data->comp_buf_size = zlib_size;

    stat_compress_add(&enc->shared_data->zlib_glz_stat, start_time, glz_size, zlib_size);
    pthread_rwlock_unlock(&enc->glz_dict->encode_lock);
    return TRUE;

glz:
    pthread_rwlock_unlock(&enc->glz_dict->encode_lock);

    dest->descriptor.type    = SPICE_IMAGE_TYPE_GLZ_RGB;
    dest->u.lz_rgb.data_size = glz_size;

    o_comp_data->comp_buf      = glz_data->data.bufs_head;
    o_comp_data->comp_buf_size = glz_size;

    return TRUE;
}

void image_encoder_shared_init(ImageEncoderSharedData *shared_data)
{
    clockid_t stat_clock = CLOCK_THREAD_CPUTIME_ID;

    stat_compress_init(&shared_data->off_stat, "off", stat_clock);
    stat_compress_init(&shared_data->lz_stat, "lz", stat_clock);
    stat_compress_init(&shared_data->glz_stat, "glz", stat_clock);
    stat_compress_init(&shared_data->quic_stat, "quic", stat_clock);
    stat_compress_init(&shared_data->jpeg_stat, "jpeg", stat_clock);
    stat_compress_init(&shared_data->zlib_glz_stat, "zlib", stat_clock);
    stat_compress_init(&shared_data->jpeg_alpha_stat, "jpeg_alpha", stat_clock);
    stat_compress_init(&shared_data->lz4_stat, "lz4", stat_clock);
}

void image_encoder_shared_stat_reset(ImageEncoderSharedData *shared_data)
{
    stat_reset(&shared_data->off_stat);
    stat_reset(&shared_data->quic_stat);
    stat_reset(&shared_data->lz_stat);
    stat_reset(&shared_data->glz_stat);
    stat_reset(&shared_data->jpeg_stat);
    stat_reset(&shared_data->zlib_glz_stat);
    stat_reset(&shared_data->jpeg_alpha_stat);
    stat_reset(&shared_data->lz4_stat);
}

#define STAT_FMT "%s\t%8u\t%13.8g\t%12.8g\t%12.8g"

#ifdef COMPRESS_STAT
static void stat_print_one(const char *name, const stat_info_t *stat)
{
    spice_info(STAT_FMT, name, stat->count, stat_byte_to_mega(stat->orig_size),
               stat_byte_to_mega(stat->comp_size), stat_cpu_time_to_sec(stat->total));
}

static void stat_sum(stat_info_t *total, const stat_info_t *stat)
{
    total->count += stat->count;
    total->orig_size += stat->orig_size;
    total->comp_size += stat->comp_size;
    total->total += stat->total;
}
#endif

void image_encoder_shared_stat_print(const ImageEncoderSharedData *shared_data)
{
#ifdef COMPRESS_STAT
    /* sum all statistics */
    stat_info_t total = {.count = 0, .orig_size = 0, .comp_size = 0, .total = 0};
    stat_sum(&total, &shared_data->off_stat);
    stat_sum(&total, &shared_data->quic_stat);
    stat_sum(&total, &shared_data->glz_stat);
    stat_sum(&total, &shared_data->lz_stat);
    stat_sum(&total, &shared_data->jpeg_stat);
    stat_sum(&total, &shared_data->jpeg_alpha_stat);
    stat_sum(&total, &shared_data->lz4_stat);

    /* fix for zlib glz */
    total.total += shared_data->zlib_glz_stat.total;
    if (shared_data->zlib_glz_stat.count) {
        total.comp_size = total.comp_size - shared_data->glz_stat.comp_size +
                          shared_data->zlib_glz_stat.comp_size;
    }

    spice_info("Method   \t  count  \torig_size(MB)\tenc_size(MB)\tenc_time(s)");
    stat_print_one("OFF      ", &shared_data->off_stat);
    stat_print_one("QUIC     ", &shared_data->quic_stat);
    stat_print_one("GLZ      ", &shared_data->glz_stat);
    stat_print_one("ZLIB GLZ ", &shared_data->zlib_glz_stat);
    stat_print_one("LZ       ", &shared_data->lz_stat);
    stat_print_one("JPEG     ", &shared_data->jpeg_stat);
    stat_print_one("JPEG-RGBA", &shared_data->jpeg_alpha_stat);
    stat_print_one("LZ4      ", &shared_data->lz4_stat);
    spice_info("-------------------------------------------------------------------");
    stat_print_one("Total    ", &total);
#endif
}
