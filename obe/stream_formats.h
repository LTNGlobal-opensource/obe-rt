/*****************************************************************************
 * stream_formats.h : Stream format type definitions and utility functions
 *****************************************************************************
 * Copyright (C) 2019 LiveTimeNet Inc. All Rights Reserved.
 *
 * Authors: Steven Toth <stoth@ltnglobal.com>
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
 ******************************************************************************/

#ifndef STREAM_FORMATS_H
#define STREAM_FORMATS_H

/* Don't add anything new to this unless it goes ad the end,
 * else various internal lists and structs will break their
 * hard index values.
 */
enum stream_formats_e
{
    /* Separate Streams */
    VIDEO_UNCOMPRESSED,
    VIDEO_AVC,
    VIDEO_MPEG2,

    AUDIO_PCM,
    AUDIO_MP2,    /* MPEG-1 Layer II */
    AUDIO_AC_3,   /* ATSC A/52B / AC-3 */
    AUDIO_E_AC_3, /* ATSC A/52B Annex E / Enhanced AC-3 */
//    AUDIO_E_DIST, /* E Distribution Audio */
    AUDIO_AAC,
    AUDIO_AC_3_BITSTREAM,   /* ATSC A/52B / AC-3 Bitstream passthorugh */

    SUBTITLES_DVB,
    MISC_TELETEXT,
    MISC_TELETEXT_INVERTED,
    MISC_WSS,
    MISC_VPS,

    /* Per-frame Streams/Data */
    CAPTIONS_CEA_608,
    CAPTIONS_CEA_708,
    MISC_AFD,
    MISC_BAR_DATA,
    MISC_PAN_SCAN,

    /* VBI data services */
    VBI_RAW,         /* location flag */
    VBI_AMOL_48,
    VBI_AMOL_96,
    VBI_NABTS,
    VBI_TVG2X,
    VBI_CP,
    VBI_VITC,
    VBI_VIDEO_INDEX, /* location flag */

    /* Vertical Ancillary (location flags) */
    VANC_GENERIC,
    VANC_DVB_SCTE_VBI,
    VANC_OP47_SDP,
    VANC_OP47_MULTI_PACKET,
    VANC_ATC,
    VANC_DTV_PROGRAM_DESCRIPTION,
    VANC_DTV_DATA_BROADCAST,
    VANC_SMPTE_VBI,
    VANC_SCTE_104,

    /* Kernel Labs, a generic handler */
    DVB_TABLE_SECTION,
    SMPTE2038,
    VIDEO_HEVC_X265,
    VIDEO_AVC_VAAPI,
    VIDEO_HEVC_VAAPI,
    VIDEO_AVC_GPU_VAAPI_AVCODEC,
    VIDEO_HEVC_GPU_VAAPI_AVCODEC,
    VIDEO_AVC_CPU_AVCODEC,
    VIDEO_HEVC_CPU_AVCODEC,
    VIDEO_HEVC_GPU_NVENC_AVCODEC,
    VIDEO_HEVC_VEGA3301,
    VIDEO_HEVC_VEGA3311,
    SMPTE2031, /* RDD8 WST teletext subtitles */
    VIDEO_AVC_VEGA3311,
    VIDEO_AVC_MACOS_VIDEOTOOLBOX,
};

const char *stream_format_name(enum stream_formats_e id);

#endif /* STREAM_FORMATS_H */
