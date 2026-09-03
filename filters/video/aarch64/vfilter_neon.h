/*****************************************************************************
 * vfilter_neon.h: video filter NEON intrinsics prototypes
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 *****************************************************************************/

/* aarch64/arm64 (e.g. Apple Silicon) equivalents of the x86 SIMD row
 * functions in filters/video/x86/vfilter.asm, implemented with NEON
 * intrinsics rather than hand assembly since NEON is baseline on all
 * AArch64 hosts (no runtime CPU-feature dispatch needed, unlike x86 SSE/AVX).
 */

#ifndef OBE_AARCH64_VFILTER_NEON
#define OBE_AARCH64_VFILTER_NEON

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void obe_downsample_chroma_row_top_neon( uint16_t *src, uint16_t *dst, int width, int stride );
void obe_downsample_chroma_row_bottom_neon( uint16_t *src, uint16_t *dst, int width, int stride );

void obe_dither_row_10_to_8_neon( uint16_t *src, uint8_t *dst, const uint16_t *dither, int width, int stride );

#ifdef __cplusplus
};
#endif

#endif
