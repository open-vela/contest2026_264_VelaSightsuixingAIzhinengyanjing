/****************************************************************************
 * apps/camera_preview/preview_face.h
 *
 * Procedurally drawn faces for the two GC9D01 round panels.
 *
 * The renderer is deliberately free of NuttX, V4L2 and framebuffer
 * dependencies: it only writes RGB565 pixels into memory the caller owns.
 * That is what lets the whole expression table be rendered and checked on
 * the host (board/beken/chips/bk7258/sim_tests/test_face.c) instead of
 * spending a four-minute flash cycle per typo in a coordinate table.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APPS_CAMERA_PREVIEW_PREVIEW_FACE_H
#define __APPS_CAMERA_PREVIEW_PREVIEW_FACE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Number of expressions in the table. */

int preview_face_count(void);

/* Name of expression idx, or NULL when idx is out of range. */

const char *preview_face_name(int idx);

/* Index of the named expression, or -1 when there is no such name. */

int preview_face_lookup(const char *name);

/* Total animation length in ms, or 0 for a static expression (one
 * keyframe).  Callers use this to decide whether to loop.
 */

uint32_t preview_face_duration_ms(int idx);

/* Render expression idx at animation phase phase_ms into buf.
 *
 * buf must have room for h rows of stride bytes; pixels are RGB565 in the
 * host's byte order (the panel driver does the on-wire halfword swap).  The
 * whole rectangle is overwritten, so the caller never has to clear it.
 *
 * phase_ms is taken modulo the animation length, so a caller can pass a
 * monotonically increasing millisecond counter forever.
 */

void preview_face_render(uint8_t *buf, size_t stride, int w, int h,
                         int idx, uint32_t phase_ms);

#endif /* __APPS_CAMERA_PREVIEW_PREVIEW_FACE_H */
