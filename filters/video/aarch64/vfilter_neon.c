/*****************************************************************************
 * vfilter_neon.c: video filter NEON intrinsics
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

/* NEON intrinsics equivalents of filters/video/x86/vfilter.asm, for
 * aarch64/arm64 hosts (e.g. Apple Silicon) where the x86 yasm/nasm SIMD
 * cannot be built. Bit-exact with the scalar C fallbacks in video.c
 * (dither_row_10_to_8_c / downsample_chroma_row_top_c / _bottom_c). */

#include <arm_neon.h>
#include "vfilter_neon.h"

/* obe_downsample_chroma_row_top_neon/obe_downsample_chroma_row_bottom_neon:
 * field-aware vertical chroma downsample. 'top' weights the current row 3:1
 * against the row one field-line below (src + stride); 'bottom' weights it
 * 1:3. Matches:
 *   top:    dst[i] = (3*src[i] +   srcf[i] + 2) >> 2
 *   bottom: dst[i] = (  src[i] + 3*srcf[i] + 2) >> 2
 */
static inline void downsample_chroma_row_neon( uint16_t *src, uint16_t *dst, int width, int stride, int top )
{
    uint16_t *srcf = src + stride;
    int n = width / 2;
    int i = 0;

    for( ; i <= n - 8; i += 8 )
    {
        uint16x8_t a = vld1q_u16( &src[i] );
        uint16x8_t b = vld1q_u16( &srcf[i] );
        uint16x8_t sum3 = top ? vaddq_u16( vaddq_u16( a, a ), a ) /* 3*a */
                              : vaddq_u16( vaddq_u16( b, b ), b ); /* 3*b */
        uint16x8_t other = top ? b : a;
        uint16x8_t r = vrshrq_n_u16( vaddq_u16( sum3, other ), 2 ); /* (3*x + y + 2) >> 2, rounding shift */
        vst1q_u16( &dst[i], r );
    }

    for( ; i < n; i++ )
        dst[i] = top ? (3*src[i] + srcf[i] + 2) >> 2
                     : (src[i] + 3*srcf[i] + 2) >> 2;
}

void obe_downsample_chroma_row_top_neon( uint16_t *src, uint16_t *dst, int width, int stride )
{
    downsample_chroma_row_neon( src, dst, width, stride, 1 );
}

void obe_downsample_chroma_row_bottom_neon( uint16_t *src, uint16_t *dst, int width, int stride )
{
    downsample_chroma_row_neon( src, dst, width, stride, 0 );
}

/* obe_dither_row_10_to_8_neon: 10-bit -> 8-bit with an 8-wide dither pattern.
 * Matches dst[k] = ((src[k] + dither[k&7]) * 511) >> 11. */
void obe_dither_row_10_to_8_neon( uint16_t *src, uint8_t *dst, const uint16_t *dither, int width, int stride )
{
    (void)stride; /* row is contiguous, stride unused - matches the x86 asm */

    const uint16x8_t dith  = vld1q_u16( dither );
    const uint32x4_t scale = vdupq_n_u32( 511 );

    int k = 0;
    for( ; k <= width - 8; k += 8 )
    {
        uint16x8_t s   = vld1q_u16( &src[k] );
        uint16x8_t sum = vaddq_u16( s, dith );

        uint32x4_t lo = vmovl_u16( vget_low_u16( sum ) );
        uint32x4_t hi = vmovl_u16( vget_high_u16( sum ) );

        lo = vshrq_n_u32( vmulq_u32( lo, scale ), 11 );
        hi = vshrq_n_u32( vmulq_u32( hi, scale ), 11 );

        uint16x8_t r16 = vcombine_u16( vqmovn_u32( lo ), vqmovn_u32( hi ) );
        uint8x8_t  r8  = vqmovn_u16( r16 );

        vst1_u8( &dst[k], r8 );
    }

    for( ; k < width; k++ )
        dst[k] = (src[k] + dither[k & 7]) * 511 >> 11;
}
