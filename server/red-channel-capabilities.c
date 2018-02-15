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

#include <common/macros.h>
#include <string.h>

#include "red-channel-capabilities.h"

GType red_channel_capabilities_type;

void red_channel_capabilities_init(RedChannelCapabilities *dest, const RedChannelCapabilities *caps)
{
    *dest = *caps;
    if (caps->common_caps) {
        dest->common_caps = g_memdup(caps->common_caps, caps->num_common_caps * sizeof(uint32_t));
    }
    if (caps->num_caps) {
        dest->caps = g_memdup(caps->caps, caps->num_caps * sizeof(uint32_t));
    }
}

void red_channel_capabilities_reset(RedChannelCapabilities *caps)
{
    g_free(caps->common_caps);
    g_free(caps->caps);
    memset(caps, 0, sizeof(*caps));
}

static RedChannelCapabilities *red_channel_capabilities_dup(const RedChannelCapabilities *caps)
{
    RedChannelCapabilities *res = g_new(RedChannelCapabilities, 1);
    red_channel_capabilities_init(res, caps);
    return res;
}

static void red_channel_capabilities_free(RedChannelCapabilities *caps)
{
    red_channel_capabilities_reset(caps);
    g_free(caps);
}

SPICE_CONSTRUCTOR_FUNC(red_channel_capabilities_construct)
{
#if !GLIB_CHECK_VERSION(2, 36, 0)
    g_type_init();
#endif

    red_channel_capabilities_type = g_boxed_type_register_static(
        "RedChannelCapabilities", (GBoxedCopyFunc)red_channel_capabilities_dup,
        (GBoxedFreeFunc)red_channel_capabilities_free);
}
