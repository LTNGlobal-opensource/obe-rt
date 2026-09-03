/*****************************************************************************
 * stream_formats.c : Stream format type definitions and utility functions
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

#include <stdio.h>
#include "stream_formats.h"

static char lbl[64]; /* in non-typical use cases. */
const char *stream_format_name(enum stream_formats_e id)
{
	switch(id) {
	case AUDIO_MP2: return "AUDIO_MP2";
	case AUDIO_AAC: return "AUDIO_AAC";
	case AUDIO_AC_3: return "AUDIO_AC_3";
	case AUDIO_E_AC_3: return "AUDIO_E_AC_3";
	case AUDIO_AC_3_BITSTREAM: return "AUDIO_AC_3_BITSTREAM";
	case VIDEO_AVC: return "VIDEO_AVC";
	case VIDEO_UNCOMPRESSED: return "VIDEO_UNCOMPRESSED";
	case VIDEO_HEVC_X265: return "VIDEO_HEVC_X265";
	case VIDEO_HEVC_VEGA3301: return "VIDEO_HEVC_VEGA3301";
	case VIDEO_HEVC_VEGA3311: return "VIDEO_HEVC_VEGA3311";
	case VIDEO_AVC_VEGA3311: return "VIDEO_AVC_VEGA3311";
	case VIDEO_AVC_CPU_AVCODEC: return "VIDEO_AVC_CPU_AVCODEC";
	case VIDEO_AVC_GPU_VAAPI_AVCODEC: return "VIDEO_AVC_GPU_VAAPI_AVCODEC";
	case VIDEO_HEVC_GPU_VAAPI_AVCODEC: return "VIDEO_HEVC_GPU_VAAPI_AVCODEC";
	case VIDEO_HEVC_CPU_AVCODEC: return "VIDEO_HEVC_CPU_AVCODEC";
	case VIDEO_HEVC_GPU_NVENC_AVCODEC: return "VIDEO_HEVC_GPU_NVENC_AVCODEC";
	case VIDEO_AVC_MACOS_VIDEOTOOLBOX: return "VIDEO_AVC_MACOS_VIDEOTOOLBOX";
	case DVB_TABLE_SECTION: return "DVB_TABLE_SECTION";
	case SMPTE2038: return "SMPTE2038";
	case SMPTE2031: return "SMPTE2031";
	default:
		{
		sprintf(lbl,  "UNDEFINED id %d", id);
		return lbl;
		}
	}
}

