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

#include <sys/stat.h>

#include "spice-bitmap-utils.h"

#define RED_BITMAP_UTILS_RGB16
#include "spice-bitmap-utils.tmpl.c"
#define RED_BITMAP_UTILS_RGB24
#include "spice-bitmap-utils.tmpl.c"
#define RED_BITMAP_UTILS_RGB32
#include "spice-bitmap-utils.tmpl.c"

#define GRADUAL_HIGH_RGB24_TH -0.03
#define GRADUAL_HIGH_RGB16_TH 0

// setting a more permissive threshold for stream identification in order
// not to miss streams that were artificially scaled on the guest (e.g., full
// screen view
// in window media player 12). see red_stream_add_frame
#define GRADUAL_MEDIUM_SCORE_TH 0.002

// assumes that stride doesn't overflow
BitmapGradualType bitmap_get_graduality_level(SpiceBitmap *bitmap)
{
    double      score       = 0.0;
    int         num_samples = 0;
    int         num_lines;
    double      chunk_score       = 0.0;
    int         chunk_num_samples = 0;
    uint32_t    x, i;
    SpiceChunk *chunk;

    chunk = bitmap->data->chunk;
    for (i = 0; i < bitmap->data->num_chunks; i++) {
        num_lines = chunk[i].len / bitmap->stride;
        x         = bitmap->x;
        switch (bitmap->format) {
        case SPICE_BITMAP_FMT_16BIT:
            compute_lines_gradual_score_rgb16((rgb16_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        case SPICE_BITMAP_FMT_24BIT:
            compute_lines_gradual_score_rgb24((rgb24_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        case SPICE_BITMAP_FMT_32BIT:
        case SPICE_BITMAP_FMT_RGBA:
            compute_lines_gradual_score_rgb32((rgb32_pixel_t *)chunk[i].data, x, num_lines,
                                              &chunk_score, &chunk_num_samples);
            break;
        default:
            spice_error("invalid bitmap format (not RGB) %u", bitmap->format);
        }
        score += chunk_score;
        num_samples += chunk_num_samples;
    }

    spice_assert(num_samples);
    score /= num_samples;

    if (bitmap->format == SPICE_BITMAP_FMT_16BIT) {
        if (score < GRADUAL_HIGH_RGB16_TH) {
            return BITMAP_GRADUAL_HIGH;
        }
    } else {
        if (score < GRADUAL_HIGH_RGB24_TH) {
            return BITMAP_GRADUAL_HIGH;
        }
    }

    if (score < GRADUAL_MEDIUM_SCORE_TH) {
        return BITMAP_GRADUAL_MEDIUM;
    } else {
        return BITMAP_GRADUAL_LOW;
    }
}

int bitmap_has_extra_stride(SpiceBitmap *bitmap)
{
    spice_assert(bitmap);
    if (bitmap_fmt_is_rgb(bitmap->format)) {
        return ((bitmap->x * bitmap_fmt_get_bytes_per_pixel(bitmap->format)) < bitmap->stride);
    } else {
        switch (bitmap->format) {
        case SPICE_BITMAP_FMT_8BIT:
            return (bitmap->x < bitmap->stride);
        case SPICE_BITMAP_FMT_4BIT_BE:
        case SPICE_BITMAP_FMT_4BIT_LE: {
            int bytes_width = SPICE_ALIGN(bitmap->x, 2) >> 1;
            return bytes_width < bitmap->stride;
        }
        case SPICE_BITMAP_FMT_1BIT_BE:
        case SPICE_BITMAP_FMT_1BIT_LE: {
            int bytes_width = SPICE_ALIGN(bitmap->x, 8) >> 3;
            return bytes_width < bitmap->stride;
        }
        default:
            spice_error("invalid image type %u", bitmap->format);
            return 0;
        }
    }
    return 0;
}

int spice_bitmap_from_surface_type(uint32_t surface_format)
{
    switch (surface_format) {
    case SPICE_SURFACE_FMT_16_555:
        return SPICE_BITMAP_FMT_16BIT;
    case SPICE_SURFACE_FMT_32_xRGB:
        return SPICE_BITMAP_FMT_32BIT;
    case SPICE_SURFACE_FMT_32_ARGB:
        return SPICE_BITMAP_FMT_RGBA;
    case SPICE_SURFACE_FMT_8_A:
        return SPICE_BITMAP_FMT_8BIT_A;
    default:
        spice_critical("Unsupported surface format");
    }
    return SPICE_BITMAP_FMT_INVALID;
}

#ifdef DUMP_BITMAP
#define RAM_PATH "/tmp/tmpfs"

static void put_16le(uint8_t **ptr, uint16_t val)
{
    **ptr = val & 0xffu;
    (*ptr)++;
    val >>= 8;
    **ptr = val & 0xffu;
    (*ptr)++;
}

static void put_32le(uint8_t **ptr, uint32_t val)
{
    put_16le(ptr, val & 0xffffu);
    val >>= 16;
    put_16le(ptr, val & 0xffffu);
}

#define WRITE(buf, size, f)                                                                        \
    do {                                                                                           \
        if (fwrite(buf, 1, (size), f) != (size))                                                   \
            goto write_err;                                                                        \
    } while (0)

static bool dump_palette(FILE *f, SpicePalette *plt)
{
    int i;
    for (i = 0; i < plt->num_ents; i++) {
        WRITE(plt->ents + i, sizeof(uint32_t), f);
    }
    return true;

write_err:
    return false;
}

static bool dump_line(FILE *f, uint8_t *line, uint16_t n_pixel_bits, int width, int row_size)
{
    static const char zeroes[4]       = {0};
    int               copy_bytes_size = SPICE_ALIGN(n_pixel_bits * width, 8) / 8;

    WRITE(line, copy_bytes_size, f);
    // each line should be 4 bytes aligned
    g_return_val_if_fail(row_size - copy_bytes_size >= 0 && row_size - copy_bytes_size <= 4, false);
    WRITE(zeroes, row_size - copy_bytes_size, f);
    return true;

write_err:
    return false;
}

void dump_bitmap(SpiceBitmap *bitmap)
{
    static uint32_t file_id = 0;

    char          file_str[200];
    int           rgb = TRUE;
    uint16_t      n_pixel_bits;
    SpicePalette *plt = NULL;
    uint32_t      id;
    int           row_size;
    uint32_t      file_size;
    int           alpha       = 0;
    uint32_t      header_size = 14 + 40;
    uint32_t      bitmap_data_offset;
    FILE *        f;
    int           i, j;
    uint8_t       header[128], *ptr;

    switch (bitmap->format) {
    case SPICE_BITMAP_FMT_1BIT_BE:
    case SPICE_BITMAP_FMT_1BIT_LE:
        rgb          = FALSE;
        n_pixel_bits = 1;
        break;
    case SPICE_BITMAP_FMT_4BIT_BE:
    case SPICE_BITMAP_FMT_4BIT_LE:
        rgb          = FALSE;
        n_pixel_bits = 4;
        break;
    case SPICE_BITMAP_FMT_8BIT:
        rgb          = FALSE;
        n_pixel_bits = 8;
        break;
    case SPICE_BITMAP_FMT_16BIT:
        n_pixel_bits = 16;
        break;
    case SPICE_BITMAP_FMT_24BIT:
        n_pixel_bits = 24;
        break;
    case SPICE_BITMAP_FMT_32BIT:
        n_pixel_bits = 32;
        break;
    case SPICE_BITMAP_FMT_RGBA:
        n_pixel_bits = 32;
        alpha        = 1;
        break;
    default:
        spice_error("invalid bitmap format  %u", bitmap->format);
        return;
    }

    if (!rgb) {
        if (!bitmap->palette) {
            return; // dont dump masks.
        }
        plt = bitmap->palette;
    }
    row_size           = (((bitmap->x * n_pixel_bits) + 31) / 32) * 4;
    bitmap_data_offset = header_size;

    if (plt) {
        bitmap_data_offset += plt->num_ents * 4;
    }
    file_size = bitmap_data_offset + (bitmap->y * row_size);

    id = ++file_id;
    mkdir(RAM_PATH, 0755);
    sprintf(file_str, "%s/%u.bmp", RAM_PATH, id);

    f = fopen(file_str, "wb");
    if (!f) {
        spice_error("Error creating bmp");
        return;
    }

    /* writing the bmp v3 header */
    ptr = header;
    put_16le(&ptr, 0x4D42); // "BM"
    put_32le(&ptr, file_size);
    put_16le(&ptr, alpha);
    put_16le(&ptr, 0);
    put_32le(&ptr, bitmap_data_offset);
    put_32le(&ptr, header_size - 14);
    put_32le(&ptr, bitmap->x);
    put_32le(&ptr, bitmap->flags & SPICE_BITMAP_FLAGS_TOP_DOWN ? -bitmap->y : bitmap->y);

    put_16le(&ptr, 1); // plane
    put_16le(&ptr, n_pixel_bits);

    put_32le(&ptr, 0); // compression

    put_32le(&ptr, 0); // file_size - bitmap_data_offset;
    put_32le(&ptr, 0);
    put_32le(&ptr, 0);
    put_32le(&ptr, !plt ? 0 : plt->num_ents); // plt entries
    put_32le(&ptr, 0);

    WRITE(header, ptr - header, f);

    if (plt) {
        if (!dump_palette(f, plt))
            goto write_err;
    }
    /* writing the data */
    for (i = 0; i < bitmap->data->num_chunks; i++) {
        SpiceChunk *chunk     = &bitmap->data->chunk[i];
        int         num_lines = chunk->len / bitmap->stride;
        for (j = 0; j < num_lines; j++) {
            if (!dump_line(f, chunk->data + (j * bitmap->stride), n_pixel_bits, bitmap->x,
                           row_size))
                goto write_err;
        }
    }
    fclose(f);
    return;

write_err:
    fclose(f);
    remove(file_str);
}
#endif
