/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2009,2010 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <inttypes.h>
#include <pthread.h>
#include <glib.h>

#include "memslot.h"
#include "red-common.h"
#include "red-parse-qxl.h"
#include "red-record-qxl.h"
#include "zlib-encoder.h"

struct RedRecord
{
    FILE *          fd;
    pthread_mutex_t lock;
    unsigned int    counter;
    gint            refs;
};

#if 0
static void hexdump_qxl(RedMemSlotInfo *slots, int group_id,
                        QXLPHYSICAL addr, uint8_t bytes)
{
    uint8_t *hex;
    int i;
    int error;

    hex = (uint8_t*)memslot_get_virt(slots, addr, bytes, group_id,
                                     &error);
    for (i = 0; i < bytes; i++) {
        if (0 == i % 16) {
            fprintf(stderr, "%lx: ", addr+i);
        }
        if (0 == i % 4) {
            fprintf(stderr, " ");
        }
        fprintf(stderr, " %02x", hex[i]);
        if (15 == i % 16) {
            fprintf(stderr, "\n");
        }
    }
}
#endif

#define WITH_ZLIB 0

/* TODO: make this thread safe (required for two qxl devices) */

#if WITH_ZLIB
typedef struct RecordEncoderData
{
    ZlibEncoderUsrContext base;
    uint8_t *             buf;
    int                   size;
} RecordEncoderData;

static int record_zlib_more_space(ZlibEncoderUsrContext *usr, uint8_t **io_ptr) { return 0; }

static int record_zlib_more_input(ZlibEncoderUsrContext *usr, uint8_t **input)
{
    RecordEncoderData *data = SPICE_UPCAST(RecordEncoderData, usr);

    if (data->buf == NULL) {
        fprintf(stderr, "%s: error: no more data\n", __FUNCTION__);
        exit(1);
    }
    *input    = data->buf;
    data->buf = 0;
    return data->size;
}

RecordEncoderData record_encoder_data = {
    .base =
        {
            record_zlib_more_space, record_zlib_more_input,
        },
    .buf  = NULL,
    .size = 0,
};
#define RECORD_ZLIB_DEFAULT_COMPRESSION_LEVEL 3
#endif

#if WITH_ZLIB
static uint8_t output[1024 * 1024 * 4]; // static buffer for encoding, 4MB
#endif

static void write_binary(FILE *fd, const char *prefix, size_t size, const uint8_t *buf)
{
    int n;

#if WITH_ZLIB
    ZlibEncoder *enc;
    int          zlib_size;

    record_encoder_data.buf  = buf;
    record_encoder_data.size = size;
    enc = zlib_encoder_create(&record_encoder_data.base, RECORD_ZLIB_DEFAULT_COMPRESSION_LEVEL);
    if (!enc) {
        fprintf(stderr, "%s: zlib encoder creation failed\n", __FUNCTION__);
        exit(1);
    }
#endif

    fprintf(fd, "binary %d %s %zu:", WITH_ZLIB, prefix, size);
#if WITH_ZLIB
    zlib_size =
        zlib_encode(enc, RECORD_ZLIB_DEFAULT_COMPRESSION_LEVEL, size, output, sizeof(output));
    fprintf(fd, "%d:", zlib_size);
    n = fwrite(output, zlib_size, 1, fd);
    zlib_encoder_destroy(enc);
#else
    n = fwrite(buf, size, 1, fd);
#endif
    (void)n;
    fprintf(fd, "\n");
}

static size_t red_record_data_chunks_ptr(FILE *          fd,
                                         const char *    prefix,
                                         RedMemSlotInfo *slots,
                                         int             group_id,
                                         int             memslot_id,
                                         QXLDataChunk *  qxl)
{
    size_t        data_size    = qxl->data_size;
    int           count_chunks = 0;
    QXLDataChunk *cur          = qxl;
    int           error;

    while (cur->next_chunk) {
        cur = (QXLDataChunk *)memslot_get_virt(slots, cur->next_chunk, sizeof(*cur), group_id,
                                               &error);
        data_size += cur->data_size;
        count_chunks++;
    }
    fprintf(fd, "data_chunks %d %zu\n", count_chunks, data_size);
    memslot_validate_virt(slots, (intptr_t)qxl->data, memslot_id, qxl->data_size, group_id);
    write_binary(fd, prefix, qxl->data_size, qxl->data);

    while (qxl->next_chunk) {
        memslot_id = memslot_get_id(slots, qxl->next_chunk);
        qxl = (QXLDataChunk *)memslot_get_virt(slots, qxl->next_chunk, sizeof(*qxl), group_id,
                                               &error);

        memslot_validate_virt(slots, (intptr_t)qxl->data, memslot_id, qxl->data_size, group_id);
        write_binary(fd, prefix, qxl->data_size, qxl->data);
    }

    return data_size;
}

static size_t red_record_data_chunks(
    FILE *fd, const char *prefix, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr)
{
    QXLDataChunk *qxl;
    int           memslot_id = memslot_get_id(slots, addr);
    int           error;

    qxl = (QXLDataChunk *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);
    return red_record_data_chunks_ptr(fd, prefix, slots, group_id, memslot_id, qxl);
}

static void red_record_point_ptr(FILE *fd, QXLPoint *qxl)
{
    fprintf(fd, "point %d %d\n", qxl->x, qxl->y);
}

static void red_record_point16_ptr(FILE *fd, QXLPoint16 *qxl)
{
    fprintf(fd, "point16 %d %d\n", qxl->x, qxl->y);
}

static void red_record_rect_ptr(FILE *fd, const char *prefix, QXLRect *qxl)
{
    fprintf(fd, "rect %s %d %d %d %d\n", prefix, qxl->top, qxl->left, qxl->bottom, qxl->right);
}

static void red_record_path(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr)
{
    QXLPath *qxl;
    int      error;

    qxl = (QXLPath *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);
    red_record_data_chunks_ptr(fd, "path", slots, group_id, memslot_get_id(slots, addr),
                               &qxl->chunk);
}

static void red_record_clip_rects(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr)
{
    QXLClipRects *qxl;
    int           error;

    qxl = (QXLClipRects *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);
    fprintf(fd, "num_rects %d\n", qxl->num_rects);
    red_record_data_chunks_ptr(fd, "clip_rects", slots, group_id, memslot_get_id(slots, addr),
                               &qxl->chunk);
}

static void red_record_virt_data_flat(FILE *          fd,
                                      const char *    prefix,
                                      RedMemSlotInfo *slots,
                                      int             group_id,
                                      QXLPHYSICAL     addr,
                                      size_t          size)
{
    int error;

    write_binary(fd, prefix, size,
                 (uint8_t *)memslot_get_virt(slots, addr, size, group_id, &error));
}

static void red_record_image_data_flat(
    FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr, size_t size)
{
    red_record_virt_data_flat(fd, "image_data_flat", slots, group_id, addr, size);
}

static void red_record_transform(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr)
{
    red_record_virt_data_flat(fd, "transform", slots, group_id, addr, sizeof(SpiceTransform));
}

static void
red_record_image(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr, uint32_t flags)
{
    QXLImage *qxl;
    size_t    bitmap_size, size;
    uint8_t   qxl_flags;
    int       error;

    fprintf(fd, "image %d\n", addr ? 1 : 0);
    if (addr == 0) {
        return;
    }

    qxl = (QXLImage *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);
    fprintf(fd, "descriptor.id %" PRIu64 "\n", qxl->descriptor.id);
    fprintf(fd, "descriptor.type %d\n", qxl->descriptor.type);
    fprintf(fd, "descriptor.flags %d\n", qxl->descriptor.flags);
    fprintf(fd, "descriptor.width %d\n", qxl->descriptor.width);
    fprintf(fd, "descriptor.height %d\n", qxl->descriptor.height);

    switch (qxl->descriptor.type) {
    case SPICE_IMAGE_TYPE_BITMAP:
        fprintf(fd, "bitmap.format %d\n", qxl->bitmap.format);
        fprintf(fd, "bitmap.flags %d\n", qxl->bitmap.flags);
        fprintf(fd, "bitmap.x %d\n", qxl->bitmap.x);
        fprintf(fd, "bitmap.y %d\n", qxl->bitmap.y);
        fprintf(fd, "bitmap.stride %d\n", qxl->bitmap.stride);
        qxl_flags = qxl->bitmap.flags;
        fprintf(fd, "has_palette %d\n", qxl->bitmap.palette ? 1 : 0);
        if (qxl->bitmap.palette) {
            QXLPalette *qp;
            int         i, num_ents;
            qp = (QXLPalette *)memslot_get_virt(slots, qxl->bitmap.palette, sizeof(*qp), group_id,
                                                &error);
            num_ents = qp->num_ents;
            fprintf(fd, "qp.num_ents %d\n", qp->num_ents);
            memslot_validate_virt(slots, (intptr_t)qp->ents,
                                  memslot_get_id(slots, qxl->bitmap.palette),
                                  num_ents * sizeof(qp->ents[0]), group_id);
            fprintf(fd, "unique %" PRIu64 "\n", qp->unique);
            for (i = 0; i < num_ents; i++) {
                fprintf(fd, "ents %d\n", qp->ents[i]);
            }
        }
        bitmap_size = qxl->bitmap.y * qxl->bitmap.stride;
        if (qxl_flags & QXL_BITMAP_DIRECT) {
            red_record_image_data_flat(fd, slots, group_id, qxl->bitmap.data, bitmap_size);
        } else {
            size = red_record_data_chunks(fd, "bitmap.data", slots, group_id, qxl->bitmap.data);
            spice_assert(size == bitmap_size);
        }
        break;
    case SPICE_IMAGE_TYPE_SURFACE:
        fprintf(fd, "surface_image.surface_id %d\n", qxl->surface_image.surface_id);
        break;
    case SPICE_IMAGE_TYPE_QUIC:
        fprintf(fd, "quic.data_size %d\n", qxl->quic.data_size);
        size =
            red_record_data_chunks_ptr(fd, "quic.data", slots, group_id,
                                       memslot_get_id(slots, addr), (QXLDataChunk *)qxl->quic.data);
        spice_assert(size == qxl->quic.data_size);
        break;
    default:
        spice_error("unknown type %d", qxl->descriptor.type);
    }
}

static void
red_record_brush_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLBrush *qxl, uint32_t flags)
{
    fprintf(fd, "type %d\n", qxl->type);
    switch (qxl->type) {
    case SPICE_BRUSH_TYPE_SOLID:
        fprintf(fd, "u.color %d\n", qxl->u.color);
        break;
    case SPICE_BRUSH_TYPE_PATTERN:
        red_record_image(fd, slots, group_id, qxl->u.pattern.pat, flags);
        red_record_point_ptr(fd, &qxl->u.pattern.pos);
        break;
    }
}

static void
red_record_qmask_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLQMask *qxl, uint32_t flags)
{
    fprintf(fd, "flags %d\n", qxl->flags);
    red_record_point_ptr(fd, &qxl->pos);
    red_record_image(fd, slots, group_id, qxl->bitmap, flags);
}

static void
red_record_fill_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLFill *qxl, uint32_t flags)
{
    red_record_brush_ptr(fd, slots, group_id, &qxl->brush, flags);
    fprintf(fd, "rop_descriptor %d\n", qxl->rop_descriptor);
    red_record_qmask_ptr(fd, slots, group_id, &qxl->mask, flags);
}

static void
red_record_opaque_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLOpaque *qxl, uint32_t flags)
{
    red_record_image(fd, slots, group_id, qxl->src_bitmap, flags);
    red_record_rect_ptr(fd, "src_area", &qxl->src_area);
    red_record_brush_ptr(fd, slots, group_id, &qxl->brush, flags);
    fprintf(fd, "rop_descriptor %d\n", qxl->rop_descriptor);
    fprintf(fd, "scale_mode %d\n", qxl->scale_mode);
    red_record_qmask_ptr(fd, slots, group_id, &qxl->mask, flags);
}

static void
red_record_copy_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLCopy *qxl, uint32_t flags)
{
    red_record_image(fd, slots, group_id, qxl->src_bitmap, flags);
    red_record_rect_ptr(fd, "src_area", &qxl->src_area);
    fprintf(fd, "rop_descriptor %d\n", qxl->rop_descriptor);
    fprintf(fd, "scale_mode %d\n", qxl->scale_mode);
    red_record_qmask_ptr(fd, slots, group_id, &qxl->mask, flags);
}

static void
red_record_blend_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLBlend *qxl, uint32_t flags)
{
    red_record_image(fd, slots, group_id, qxl->src_bitmap, flags);
    red_record_rect_ptr(fd, "src_area", &qxl->src_area);
    fprintf(fd, "rop_descriptor %d\n", qxl->rop_descriptor);
    fprintf(fd, "scale_mode %d\n", qxl->scale_mode);
    red_record_qmask_ptr(fd, slots, group_id, &qxl->mask, flags);
}

static void red_record_transparent_ptr(
    FILE *fd, RedMemSlotInfo *slots, int group_id, QXLTransparent *qxl, uint32_t flags)
{
    red_record_image(fd, slots, group_id, qxl->src_bitmap, flags);
    red_record_rect_ptr(fd, "src_area", &qxl->src_area);
    fprintf(fd, "src_color %d\n", qxl->src_color);
    fprintf(fd, "true_color %d\n", qxl->true_color);
}

static void red_record_alpha_blend_ptr(
    FILE *fd, RedMemSlotInfo *slots, int group_id, QXLAlphaBlend *qxl, uint32_t flags)
{
    fprintf(fd, "alpha_flags %d\n", qxl->alpha_flags);
    fprintf(fd, "alpha %d\n", qxl->alpha);
    red_record_image(fd, slots, group_id, qxl->src_bitmap, flags);
    red_record_rect_ptr(fd, "src_area", &qxl->src_area);
}

static void red_record_alpha_blend_ptr_compat(
    FILE *fd, RedMemSlotInfo *slots, int group_id, QXLCompatAlphaBlend *qxl, uint32_t flags)
{
    fprintf(fd, "alpha %d\n", qxl->alpha);
    red_record_image(fd, slots, group_id, qxl->src_bitmap, flags);
    red_record_rect_ptr(fd, "src_area", &qxl->src_area);
}

static void
red_record_rop3_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLRop3 *qxl, uint32_t flags)
{
    red_record_image(fd, slots, group_id, qxl->src_bitmap, flags);
    red_record_rect_ptr(fd, "src_area", &qxl->src_area);
    red_record_brush_ptr(fd, slots, group_id, &qxl->brush, flags);
    fprintf(fd, "rop3 %d\n", qxl->rop3);
    fprintf(fd, "scale_mode %d\n", qxl->scale_mode);
    red_record_qmask_ptr(fd, slots, group_id, &qxl->mask, flags);
}

static void
red_record_stroke_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLStroke *qxl, uint32_t flags)
{
    int error;

    red_record_path(fd, slots, group_id, qxl->path);
    fprintf(fd, "attr.flags %d\n", qxl->attr.flags);
    if (qxl->attr.flags & SPICE_LINE_FLAGS_STYLED) {
        int      style_nseg = qxl->attr.style_nseg;
        uint8_t *buf;

        fprintf(fd, "attr.style_nseg %d\n", qxl->attr.style_nseg);
        spice_assert(qxl->attr.style);
        buf = (uint8_t *)memslot_get_virt(slots, qxl->attr.style, style_nseg * sizeof(QXLFIXED),
                                          group_id, &error);
        write_binary(fd, "style", style_nseg * sizeof(QXLFIXED), buf);
    }
    red_record_brush_ptr(fd, slots, group_id, &qxl->brush, flags);
    fprintf(fd, "fore_mode %d\n", qxl->fore_mode);
    fprintf(fd, "back_mode %d\n", qxl->back_mode);
}

static void red_record_string(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr)
{
    QXLString *qxl;
    size_t     chunk_size;
    int        error;

    qxl = (QXLString *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);
    fprintf(fd, "data_size %d\n", qxl->data_size);
    fprintf(fd, "length %d\n", qxl->length);
    fprintf(fd, "flags %d\n", qxl->flags);
    chunk_size = red_record_data_chunks_ptr(fd, "string", slots, group_id,
                                            memslot_get_id(slots, addr), &qxl->chunk);
    spice_assert(chunk_size == qxl->data_size);
}

static void
red_record_text_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLText *qxl, uint32_t flags)
{
    red_record_string(fd, slots, group_id, qxl->str);
    red_record_rect_ptr(fd, "back_area", &qxl->back_area);
    red_record_brush_ptr(fd, slots, group_id, &qxl->fore_brush, flags);
    red_record_brush_ptr(fd, slots, group_id, &qxl->back_brush, flags);
    fprintf(fd, "fore_mode %d\n", qxl->fore_mode);
    fprintf(fd, "back_mode %d\n", qxl->back_mode);
}

static void red_record_whiteness_ptr(
    FILE *fd, RedMemSlotInfo *slots, int group_id, QXLWhiteness *qxl, uint32_t flags)
{
    red_record_qmask_ptr(fd, slots, group_id, &qxl->mask, flags);
}

static void red_record_blackness_ptr(
    FILE *fd, RedMemSlotInfo *slots, int group_id, QXLBlackness *qxl, uint32_t flags)
{
    red_record_qmask_ptr(fd, slots, group_id, &qxl->mask, flags);
}

static void
red_record_invers_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLInvers *qxl, uint32_t flags)
{
    red_record_qmask_ptr(fd, slots, group_id, &qxl->mask, flags);
}

static void red_record_clip_ptr(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLClip *qxl)
{
    fprintf(fd, "type %d\n", qxl->type);
    switch (qxl->type) {
    case SPICE_CLIP_TYPE_RECTS:
        red_record_clip_rects(fd, slots, group_id, qxl->data);
        break;
    }
}

static void red_record_composite_ptr(
    FILE *fd, RedMemSlotInfo *slots, int group_id, QXLComposite *qxl, uint32_t flags)
{
    fprintf(fd, "flags %d\n", qxl->flags);

    red_record_image(fd, slots, group_id, qxl->src, flags);
    fprintf(fd, "src_transform %d\n", !!qxl->src_transform);
    if (qxl->src_transform)
        red_record_transform(fd, slots, group_id, qxl->src_transform);
    fprintf(fd, "mask %d\n", !!qxl->mask);
    if (qxl->mask)
        red_record_image(fd, slots, group_id, qxl->mask, flags);
    fprintf(fd, "mask_transform %d\n", !!qxl->mask_transform);
    if (qxl->mask_transform)
        red_record_transform(fd, slots, group_id, qxl->mask_transform);

    fprintf(fd, "src_origin %d %d\n", qxl->src_origin.x, qxl->src_origin.y);
    fprintf(fd, "mask_origin %d %d\n", qxl->mask_origin.x, qxl->mask_origin.y);
}

static void red_record_native_drawable(
    FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr, uint32_t flags)
{
    QXLDrawable *qxl;
    int          i;
    int          error;

    qxl = (QXLDrawable *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);

    red_record_rect_ptr(fd, "bbox", &qxl->bbox);
    red_record_clip_ptr(fd, slots, group_id, &qxl->clip);
    fprintf(fd, "effect %d\n", qxl->effect);
    fprintf(fd, "mm_time %d\n", qxl->mm_time);
    fprintf(fd, "self_bitmap %d\n", qxl->self_bitmap);
    red_record_rect_ptr(fd, "self_bitmap_area", &qxl->self_bitmap_area);
    fprintf(fd, "surface_id %d\n", qxl->surface_id);

    for (i = 0; i < 3; i++) {
        fprintf(fd, "surfaces_dest %d\n", qxl->surfaces_dest[i]);
        red_record_rect_ptr(fd, "surfaces_rects", &qxl->surfaces_rects[i]);
    }

    fprintf(fd, "type %d\n", qxl->type);
    switch (qxl->type) {
    case QXL_DRAW_ALPHA_BLEND:
        red_record_alpha_blend_ptr(fd, slots, group_id, &qxl->u.alpha_blend, flags);
        break;
    case QXL_DRAW_BLACKNESS:
        red_record_blackness_ptr(fd, slots, group_id, &qxl->u.blackness, flags);
        break;
    case QXL_DRAW_BLEND:
        red_record_blend_ptr(fd, slots, group_id, &qxl->u.blend, flags);
        break;
    case QXL_DRAW_COPY:
        red_record_copy_ptr(fd, slots, group_id, &qxl->u.copy, flags);
        break;
    case QXL_COPY_BITS:
        red_record_point_ptr(fd, &qxl->u.copy_bits.src_pos);
        break;
    case QXL_DRAW_FILL:
        red_record_fill_ptr(fd, slots, group_id, &qxl->u.fill, flags);
        break;
    case QXL_DRAW_OPAQUE:
        red_record_opaque_ptr(fd, slots, group_id, &qxl->u.opaque, flags);
        break;
    case QXL_DRAW_INVERS:
        red_record_invers_ptr(fd, slots, group_id, &qxl->u.invers, flags);
        break;
    case QXL_DRAW_NOP:
        break;
    case QXL_DRAW_ROP3:
        red_record_rop3_ptr(fd, slots, group_id, &qxl->u.rop3, flags);
        break;
    case QXL_DRAW_STROKE:
        red_record_stroke_ptr(fd, slots, group_id, &qxl->u.stroke, flags);
        break;
    case QXL_DRAW_TEXT:
        red_record_text_ptr(fd, slots, group_id, &qxl->u.text, flags);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_record_transparent_ptr(fd, slots, group_id, &qxl->u.transparent, flags);
        break;
    case QXL_DRAW_WHITENESS:
        red_record_whiteness_ptr(fd, slots, group_id, &qxl->u.whiteness, flags);
        break;
    case QXL_DRAW_COMPOSITE:
        red_record_composite_ptr(fd, slots, group_id, &qxl->u.composite, flags);
        break;
    default:
        spice_error("unknown type %d", qxl->type);
        break;
    };
}

static void red_record_compat_drawable(
    FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr, uint32_t flags)
{
    QXLCompatDrawable *qxl;
    int                error;

    qxl = (QXLCompatDrawable *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);

    red_record_rect_ptr(fd, "bbox", &qxl->bbox);
    red_record_clip_ptr(fd, slots, group_id, &qxl->clip);
    fprintf(fd, "effect %d\n", qxl->effect);
    fprintf(fd, "mm_time %d\n", qxl->mm_time);

    fprintf(fd, "bitmap_offset %d\n", qxl->bitmap_offset);
    red_record_rect_ptr(fd, "bitmap_area", &qxl->bitmap_area);

    fprintf(fd, "type %d\n", qxl->type);
    switch (qxl->type) {
    case QXL_DRAW_ALPHA_BLEND:
        red_record_alpha_blend_ptr_compat(fd, slots, group_id, &qxl->u.alpha_blend, flags);
        break;
    case QXL_DRAW_BLACKNESS:
        red_record_blackness_ptr(fd, slots, group_id, &qxl->u.blackness, flags);
        break;
    case QXL_DRAW_BLEND:
        red_record_blend_ptr(fd, slots, group_id, &qxl->u.blend, flags);
        break;
    case QXL_DRAW_COPY:
        red_record_copy_ptr(fd, slots, group_id, &qxl->u.copy, flags);
        break;
    case QXL_COPY_BITS:
        red_record_point_ptr(fd, &qxl->u.copy_bits.src_pos);
        break;
    case QXL_DRAW_FILL:
        red_record_fill_ptr(fd, slots, group_id, &qxl->u.fill, flags);
        break;
    case QXL_DRAW_OPAQUE:
        red_record_opaque_ptr(fd, slots, group_id, &qxl->u.opaque, flags);
        break;
    case QXL_DRAW_INVERS:
        red_record_invers_ptr(fd, slots, group_id, &qxl->u.invers, flags);
        break;
    case QXL_DRAW_NOP:
        break;
    case QXL_DRAW_ROP3:
        red_record_rop3_ptr(fd, slots, group_id, &qxl->u.rop3, flags);
        break;
    case QXL_DRAW_STROKE:
        red_record_stroke_ptr(fd, slots, group_id, &qxl->u.stroke, flags);
        break;
    case QXL_DRAW_TEXT:
        red_record_text_ptr(fd, slots, group_id, &qxl->u.text, flags);
        break;
    case QXL_DRAW_TRANSPARENT:
        red_record_transparent_ptr(fd, slots, group_id, &qxl->u.transparent, flags);
        break;
    case QXL_DRAW_WHITENESS:
        red_record_whiteness_ptr(fd, slots, group_id, &qxl->u.whiteness, flags);
        break;
    default:
        spice_error("unknown type %d", qxl->type);
        break;
    };
}

static void
red_record_drawable(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr, uint32_t flags)
{
    fprintf(fd, "drawable\n");
    if (flags & QXL_COMMAND_FLAG_COMPAT) {
        red_record_compat_drawable(fd, slots, group_id, addr, flags);
    } else {
        red_record_native_drawable(fd, slots, group_id, addr, flags);
    }
}

static void red_record_update_cmd(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr)
{
    QXLUpdateCmd *qxl;
    int           error;

    qxl = (QXLUpdateCmd *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);

    fprintf(fd, "update\n");
    red_record_rect_ptr(fd, "area", &qxl->area);
    fprintf(fd, "update_id %d\n", qxl->update_id);
    fprintf(fd, "surface_id %d\n", qxl->surface_id);
}

static void red_record_message(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr)
{
    QXLMessage *qxl;
    int         error;

    /*
     * security alert:
     *   qxl->data[0] size isn't specified anywhere -> can't verify
     *   luckily this is for debug logging only,
     *   so we can just ignore it by default.
     */
    qxl = (QXLMessage *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);
    write_binary(fd, "message", strlen((char *)qxl->data), (uint8_t *)qxl->data);
}

static void red_record_surface_cmd(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr)
{
    QXLSurfaceCmd *qxl;
    size_t         size;
    int            error;

    qxl = (QXLSurfaceCmd *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);

    fprintf(fd, "surface_cmd\n");
    fprintf(fd, "surface_id %d\n", qxl->surface_id);
    fprintf(fd, "type %d\n", qxl->type);
    fprintf(fd, "flags %d\n", qxl->flags);

    switch (qxl->type) {
    case QXL_SURFACE_CMD_CREATE:
        fprintf(fd, "u.surface_create.format %d\n", qxl->u.surface_create.format);
        fprintf(fd, "u.surface_create.width %d\n", qxl->u.surface_create.width);
        fprintf(fd, "u.surface_create.height %d\n", qxl->u.surface_create.height);
        fprintf(fd, "u.surface_create.stride %d\n", qxl->u.surface_create.stride);
        size = qxl->u.surface_create.height * abs(qxl->u.surface_create.stride);
        if ((qxl->flags & QXL_SURF_FLAG_KEEP_DATA) != 0) {
            write_binary(fd, "data", size,
                         (uint8_t *)memslot_get_virt(slots, qxl->u.surface_create.data, size,
                                                     group_id, &error));
        }
        break;
    }
}

static void red_record_cursor(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr)
{
    QXLCursor *qxl;
    int        error;

    qxl = (QXLCursor *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);

    fprintf(fd, "header.unique %" PRIu64 "\n", qxl->header.unique);
    fprintf(fd, "header.type %d\n", qxl->header.type);
    fprintf(fd, "header.width %d\n", qxl->header.width);
    fprintf(fd, "header.height %d\n", qxl->header.height);
    fprintf(fd, "header.hot_spot_x %d\n", qxl->header.hot_spot_x);
    fprintf(fd, "header.hot_spot_y %d\n", qxl->header.hot_spot_y);

    fprintf(fd, "data_size %d\n", qxl->data_size);
    red_record_data_chunks_ptr(fd, "cursor", slots, group_id, memslot_get_id(slots, addr),
                               &qxl->chunk);
}

static void red_record_cursor_cmd(FILE *fd, RedMemSlotInfo *slots, int group_id, QXLPHYSICAL addr)
{
    QXLCursorCmd *qxl;
    int           error;

    qxl = (QXLCursorCmd *)memslot_get_virt(slots, addr, sizeof(*qxl), group_id, &error);

    fprintf(fd, "cursor_cmd\n");
    fprintf(fd, "type %d\n", qxl->type);
    switch (qxl->type) {
    case QXL_CURSOR_SET:
        red_record_point16_ptr(fd, &qxl->u.set.position);
        fprintf(fd, "u.set.visible %d\n", qxl->u.set.visible);
        red_record_cursor(fd, slots, group_id, qxl->u.set.shape);
        break;
    case QXL_CURSOR_MOVE:
        red_record_point16_ptr(fd, &qxl->u.position);
        break;
    case QXL_CURSOR_TRAIL:
        fprintf(fd, "u.trail.length %d\n", qxl->u.trail.length);
        fprintf(fd, "u.trail.frequency %d\n", qxl->u.trail.frequency);
        break;
    }
}

void red_record_primary_surface_create(RedRecord *          record,
                                       QXLDevSurfaceCreate *surface,
                                       uint8_t *            line_0)
{
    FILE *fd = record->fd;

    pthread_mutex_lock(&record->lock);
    fprintf(fd, "%d %d %d %d\n", surface->width, surface->height, surface->stride, surface->format);
    fprintf(fd, "%d %d %d %d\n", surface->position, surface->mouse_mode, surface->flags,
            surface->type);
    write_binary(fd, "data", line_0 ? abs(surface->stride) * surface->height : 0, line_0);
    pthread_mutex_unlock(&record->lock);
}

static void red_record_event_unlocked(RedRecord *record, int what, uint32_t type)
{
    red_time_t ts = spice_get_monotonic_time_ns();
    // TODO: record the size of the packet in the header. This would make
    // navigating it much faster (well, I can add an index while I'm at it..)
    // and make it trivial to get a histogram from a file.
    // But to implement that I would need some temporary buffer for each event.
    // (that can be up to VGA_FRAMEBUFFER large)
    fprintf(record->fd, "event %u %d %u %" PRIu64 "\n", record->counter++, what, type, ts);
}

void red_record_event(RedRecord *record, int what, uint32_t type)
{
    pthread_mutex_lock(&record->lock);
    red_record_event_unlocked(record, what, type);
    pthread_mutex_unlock(&record->lock);
}

void red_record_qxl_command(RedRecord *record, RedMemSlotInfo *slots, QXLCommandExt ext_cmd)
{
    FILE *fd = record->fd;

    pthread_mutex_lock(&record->lock);
    red_record_event_unlocked(record, 0, ext_cmd.cmd.type);

    switch (ext_cmd.cmd.type) {
    case QXL_CMD_DRAW:
        red_record_drawable(fd, slots, ext_cmd.group_id, ext_cmd.cmd.data, ext_cmd.flags);
        break;
    case QXL_CMD_UPDATE:
        red_record_update_cmd(fd, slots, ext_cmd.group_id, ext_cmd.cmd.data);
        break;
    case QXL_CMD_MESSAGE:
        red_record_message(fd, slots, ext_cmd.group_id, ext_cmd.cmd.data);
        break;
    case QXL_CMD_SURFACE:
        red_record_surface_cmd(fd, slots, ext_cmd.group_id, ext_cmd.cmd.data);
        break;
    case QXL_CMD_CURSOR:
        red_record_cursor_cmd(fd, slots, ext_cmd.group_id, ext_cmd.cmd.data);
        break;
    }
    pthread_mutex_unlock(&record->lock);
}

/**
 * Redirects child output to the file specified
 */
static void child_output_setup(gpointer user_data)
{
    int fd = GPOINTER_TO_INT(user_data);

    while (dup2(fd, STDOUT_FILENO) < 0 && errno == EINTR) {
        continue;
    }
    close(fd);
}

RedRecord *red_record_new(const char *filename)
{
    static const char header[] = "SPICE_REPLAY 1\n";

    const char *filter;
    FILE *      f;
    RedRecord * record;

    f = fopen(filename, "w+");
    if (!f) {
        spice_error("failed to open recording file %s\n", filename);
    }

    filter = getenv("SPICE_WORKER_RECORD_FILTER");
    if (filter) {
        gint     argc;
        gchar ** argv  = NULL;
        GError * error = NULL;
        GPid     child_pid;
        gboolean ret;
        gint     fd_in;

        ret = g_shell_parse_argv(filter, &argc, &argv, &error);

        if (ret)
            ret = g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                           child_output_setup, GINT_TO_POINTER(fileno(f)),
                                           &child_pid, &fd_in, NULL, NULL, &error);

        g_strfreev(argv);
        if (!ret) {
            g_error_free(error);
            fclose(f);
            spice_error("failed to setup filter for replay");
        }
        while (dup2(fd_in, fileno(f)) < 0 && errno == EINTR) {
            continue;
        }
        close(fd_in);
    }

    if (fwrite(header, sizeof(header) - 1, 1, f) != 1) {
        spice_error("failed to write replay header");
    }

    record          = g_new(RedRecord, 1);
    record->refs    = 1;
    record->fd      = f;
    record->counter = 0;
    pthread_mutex_init(&record->lock, NULL);
    return record;
}

RedRecord *red_record_ref(RedRecord *record)
{
    g_atomic_int_inc(&record->refs);
    return record;
}

void red_record_unref(RedRecord *record)
{
    if (!record || !g_atomic_int_dec_and_test(&record->refs)) {
        return;
    }
    fclose(record->fd);
    pthread_mutex_destroy(&record->lock);
    g_free(record);
}
