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

#ifndef SPICE_REPLAY_H_
#define SPICE_REPLAY_H_

#if !defined(SPICE_H_INSIDE) && !defined(SPICE_SERVER_INTERNAL)
#error "Only spice.h can be included directly."
#endif

#include "spice-core.h"
#include <stdio.h>

typedef struct SpiceReplay SpiceReplay;

/* reads until encountering a cmd, processing any recorded messages (io) on the
 * way */
QXLCommandExt *spice_replay_next_cmd(SpiceReplay *replay, QXLWorker *worker);
void spice_replay_free_cmd(SpiceReplay *replay, QXLCommandExt *cmd);
void spice_replay_free(SpiceReplay *replay);
SpiceReplay *spice_replay_new(FILE *file, int nsurfaces);

#endif /* SPICE_REPLAY_H_ */
