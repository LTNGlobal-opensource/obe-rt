#if HAVE_VEGA3311_CAP_TYPES_H

/*****************************************************************************
 * vega-misc.cpp: Audio/Video from the VEGA encoder cards
 *****************************************************************************
 * Copyright (C) 2021 LTN
 *
 * Authors: Steven Toth <stoth@ltnglobal.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <iostream>
#include <limits.h>
#include <fcntl.h>
#include <semaphore.h>
#include <time.h>

#include "vega-3311.h"

extern "C"
{
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>
}

const char *lookupVegaeTimeBase(API_VEGA_BQB_TIMEBASE_E tb)
{
    switch(tb) {
    case API_VEGA_BQB_TIMEBASE_SECOND:       return "API_VEGA_BQB_TIMEBASE_SECOND";
    case API_VEGA_BQB_TIMEBASE_MILLI_SECOND: return "API_VEGA_BQB_TIMEBASE_MILLI_SECOND";
    case API_VEGA_BQB_TIMEBASE_90KHZ:        return "API_VEGA_BQB_TIMEBASE_90KHZ";
    default:                                 return "UNDEFINED";
    }
}

const char *lookupVegaAudioPacketSizeName(API_VEGA3311_CAP_AUDIO_PACKET_SIZE_E v)
{
        switch (v) {
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_240AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_240AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_288AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_288AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_336AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_336AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_384AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_384AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_432AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_432AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_480AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_480AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_528AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_528AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_576AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_576AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_624AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_624AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_672AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_672AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_720AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_720AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_768AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_768AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_816AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_816AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_864AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_864AS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_912AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_912AAS";
        case API_VEGA3311_CAP_AUDIO_PACKET_SIZE_960AS:                return "API_VEGA3311_CAP_AUDIO_PACKET_SIZE_960AS";
        default:                                                      return "UNDEFINED";
        }
}

const char *lookupVegaAudioLayoutName(API_VEGA3311_CAP_AUDIO_LAYOUT_E v)
{
        switch (v) {
        case API_VEGA3311_CAP_AUDIO_LAYOUT_MONO:                return "Audio layouts: mono";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_STEREO:              return "Audio layouts: stereo";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_2P1:                 return "Audio layouts: 2.1 channel";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_3P0:                 return "Audio layouts: 3.0 channel";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_QUAD:                return "Audio layouts: quadi";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_QUAD_SIDE:           return "Audio layouts: quad side";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_3P1:                 return "Audio layouts: 3.1 channel";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_5P0:                 return "Audio layouts: 5.0 channel";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_5P0_SIDE:            return "Audio layouts: 5.0 channel side";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_5P1:                 return "Audio layouts: 5.1 channel";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_5P1_SIDE:            return "Audio layouts: 5.1 channel side";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_7P0:                 return "Audio layouts: 7.0 channel";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_7P1:                 return "Audio layouts: 7.1 channel";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_16P0:                return "Audio layouts: 16.0 channel";
        case API_VEGA3311_CAP_AUDIO_LAYOUT_PASS_THROUGH:        return "Audio layouts: fpga format"; 
        default:                                                return "UNDEFINED"; 
        }
}

const char *lookupVegaEncodingResolutionName(int v)
{
        switch (v) {
        case API_VENC_RESOLUTION_720x480:           return "API_VENC_RESOLUTION_720x480";
        case API_VENC_RESOLUTION_720x576:           return "API_VENC_RESOLUTION_720x576";
        case API_VENC_RESOLUTION_1280x720:          return "API_VENC_RESOLUTION_1280x720";
        case API_VENC_RESOLUTION_1920x1080:         return "API_VENC_RESOLUTION_1920x1080";
        case API_VENC_RESOLUTION_3840x2160:         return "API_VENC_RESOLUTION_3840x2160";
        default:                                    return "UNDEFINED";
        }
}

const char *lookupVegaEncodingImageFormatName(int v)
{
        switch (v) {
        case API_VEGA_BQB_IMAGE_FORMAT_NV12:        return "API_VEGA_BQB_IMAGE_FORMAT_NV12";
        case API_VEGA_BQB_IMAGE_FORMAT_NV16:        return "API_VEGA_BQB_IMAGE_FORMAT_NV16";
        //case API_VEGA_BQB_IMAGE_FORMAT_I420:        return "API_VEGA_BQB_IMAGE_FORMAT_I420";
        //case API_VEGA_BQB_IMAGE_FORMAT_PP01:        return "API_VEGA_BQB_IMAGE_FORMAT_PP01";
        //case API_VEGA_BQB_IMAGE_FORMAT_PP21:        return "API_VEGA_BQB_IMAGE_FORMAT_PP21";
        //case API_VEGA_BQB_IMAGE_FORMAT_I0AL:        return "API_VEGA_BQB_IMAGE_FORMAT_I0AL";
        //case API_VEGA_BQB_IMAGE_FORMAT_I2AL:        return "API_VEGA_BQB_IMAGE_FORMAT_I2AL";
        case API_VEGA_BQB_IMAGE_FORMAT_XV20:        return "API_VEGA_BQB_IMAGE_FORMAT_XV20";
        case API_VEGA_BQB_IMAGE_FORMAT_YUV420:      return "API_VEGA_BQB_IMAGE_FORMAT_YUV420";
        case API_VEGA_BQB_IMAGE_FORMAT_YUV420P010:  return "API_VEGA_BQB_IMAGE_FORMAT_YUV420P010";
        case API_VEGA_BQB_IMAGE_FORMAT_YUV422P010:  return "API_VEGA_BQB_IMAGE_FORMAT_YUV422P010";
        case API_VEGA_BQB_IMAGE_FORMAT_YUV420P10LE: return "API_VEGA_BQB_IMAGE_FORMAT_YUV420P10LE";
        case API_VEGA_BQB_IMAGE_FORMAT_YUV422P10LE: return "API_VEGA_BQB_IMAGE_FORMAT_YUV422P10LE";
        default:                                    return "UNDEFINED";
        }
}

const char *lookupVegaEncodingChromaName(API_VEGA_BQB_CHROMA_FORMAT_E v)
{
        switch (v) {
        case API_VEGA_BQB_CHROMA_FORMAT_MONO:           return "API_VEGA_BQB_CHROMA_FORMAT_MONO";
        case API_VEGA_BQB_CHROMA_FORMAT_420:            return "API_VEGA_BQB_CHROMA_FORMAT_420";
        case API_VEGA_BQB_CHROMA_FORMAT_422:            return "API_VEGA_BQB_CHROMA_FORMAT_422";
        case API_VEGA_BQB_CHROMA_FORMAT_422_TO_420:     return "API_VEGA_BQB_CHROMA_FORMAT_422_TO_420";
        default:                                        return "UNDEFINED";
        }
}

const char *lookupVegaBitDepthName(int v)
{
        switch (v) {
        case API_VENC_BIT_DEPTH_8:                  return "API_VENC_BIT_DEPTH_8";
        case API_VENC_BIT_DEPTH_10:                 return "API_VENC_BIT_DEPTH_10";
        default:                                    return "UNDEFINED";
        }
}

const char *lookupVegaInputModeName(int v)
{
        switch (v) {
        case API_VEGA3311_CAP_INPUT_MODE_1CHN_QUAD:  return "API_VEGA3311_CAP_INPUT_MODE_1CHN_QUAD";
        case API_VEGA3311_CAP_INPUT_MODE_1CHN_2SI:   return "API_VEGA3311_CAP_INPUT_MODE_1CHN_2SI";
        case API_VEGA3311_CAP_INPUT_MODE_4CHN:       return "API_VEGA3311_CAP_INPUT_MODE_4CHN";
        default:                                     return "UNDEFINED";
        }
}

const char *lookupVegaInputSourceName(int v)
{
        switch (v) {
        case API_VEGA3311_CAP_INPUT_SOURCE_SDI:          return "API_VEGA3311_CAP_INPUT_SOURCE_SDI";
        case API_VEGA3311_CAP_INPUT_SOURCE_HDMI:         return "API_VEGA3311_CAP_INPUT_SOURCE_HDMI";
        case API_VEGA3311_CAP_INPUT_SOURCE_10G_ETH_2022: return "API_VEGA3311_CAP_INPUT_SOURCE_10G_ETH_2022";
        case API_VEGA3311_CAP_INPUT_SOURCE_12G_SDI:      return "API_VEGA3311_CAP_INPUT_SOURCE_12G_SDI";
        case API_VEGA3311_CAP_INPUT_SOURCE_10G_ETH_2110: return "API_VEGA3311_CAP_INPUT_SOURCE_10G_ETH_2110";
        default:                                         return "UNDEFINED";
        }
}

const char *lookupVegaSDILevelName(int v)
{
        switch (v) {
        case API_VEGA3311_CAP_SDI_LEVEL_A:              return "API_VEGA3311_CAP_SDI_LEVEL_A";
        case API_VEGA3311_CAP_SDI_LEVEL_B:              return "API_VEGA3311_CAP_SDI_LEVEL_B";
        default:                                        return "UNDEFINED";
        }
}

const char *lookupVegaPixelFormatName(int v)
{
        switch (v) {
        case API_VEGA3311_CAP_IMAGE_FORMAT_P210:        return "API_VEGA3311_CAP_IMAGE_FORMAT_P210";
        case API_VEGA3311_CAP_IMAGE_FORMAT_P010:        return "API_VEGA3311_CAP_IMAGE_FORMAT_P010";
        case API_VEGA3311_CAP_IMAGE_FORMAT_NV12:        return "API_VEGA3311_CAP_IMAGE_FORMAT_NV12";
        case API_VEGA3311_CAP_IMAGE_FORMAT_NV16:        return "API_VEGA3311_CAP_IMAGE_FORMAT_NV16";
        case API_VEGA3311_CAP_IMAGE_FORMAT_YV12:        return "API_VEGA3311_CAP_IMAGE_FORMAT_YV12";
        case API_VEGA3311_CAP_IMAGE_FORMAT_YV16:        return "API_VEGA3311_CAP_IMAGE_FORMAT_YV16";
        case API_VEGA3311_CAP_IMAGE_FORMAT_YUY2:        return "API_VEGA3311_CAP_IMAGE_FORMAT_YUY2";
        case API_VEGA3311_CAP_IMAGE_FORMAT_V210:        return "API_VEGA3311_CAP_IMAGE_FORMAT_V210";
        case API_VEGA3311_CAP_IMAGE_FORMAT_Y210:        return "API_VEGA3311_CAP_IMAGE_FORMAT_Y210";
        default:                                        return "UNDEFINED";
        }
}

const char *vega_lookupFrameType(API_VEGA_BQB_FRAME_TYPE_E type)
{
        switch (type) {
        case API_VEGA_BQB_FRAME_TYPE_I: return "I";
        case API_VEGA_BQB_FRAME_TYPE_P: return "P";
        case API_VEGA_BQB_FRAME_TYPE_B: return "B";
        default:
                return "U";
        }
}

const static struct obe_to_vega_video video_format_tab[] =
{
    { 0, INPUT_VIDEO_FORMAT_PAL,          720,  576,  API_VEGA3311_CAP_RESOLUTION_720x576,   API_VENC_RESOLUTION_720x576,      1,    25, API_VEGA_BQB_FPS_25 },
    { 0, INPUT_VIDEO_FORMAT_NTSC,         720,  480,  API_VEGA3311_CAP_RESOLUTION_720x480,   API_VENC_RESOLUTION_720x480,   1001, 30000, API_VEGA_BQB_FPS_29_97 },
    //
    { 1, INPUT_VIDEO_FORMAT_720P_50,     1280,  720,  API_VEGA3311_CAP_RESOLUTION_1280x720,  API_VENC_RESOLUTION_1280x720,  1000, 50000, API_VEGA_BQB_FPS_50 },
    { 1, INPUT_VIDEO_FORMAT_720P_5994,   1280,  720,  API_VEGA3311_CAP_RESOLUTION_1280x720,  API_VENC_RESOLUTION_1280x720,  1001, 60000, API_VEGA_BQB_FPS_59_94 },
    { 1, INPUT_VIDEO_FORMAT_720P_60,     1280,  720,  API_VEGA3311_CAP_RESOLUTION_1280x720,  API_VENC_RESOLUTION_1280x720,  1000, 60000, API_VEGA_BQB_FPS_60 },
    //
    { 0, INPUT_VIDEO_FORMAT_1080I_50,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 50000, API_VEGA_BQB_FPS_50 },
    { 0, INPUT_VIDEO_FORMAT_1080I_5994,  1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1001, 60000, API_VEGA_BQB_FPS_59_94 },
    //
    { 1, INPUT_VIDEO_FORMAT_1080P_24,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 24000, API_VEGA_BQB_FPS_24 },
    { 1, INPUT_VIDEO_FORMAT_1080P_25,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 25000, API_VEGA_BQB_FPS_25 },
    { 1, INPUT_VIDEO_FORMAT_1080P_2997,  1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 30000, API_VEGA_BQB_FPS_29_97 },
    { 1, INPUT_VIDEO_FORMAT_1080P_30,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 30000, API_VEGA_BQB_FPS_30 },
    { 1, INPUT_VIDEO_FORMAT_1080P_50,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 50000, API_VEGA_BQB_FPS_50 },
    { 1, INPUT_VIDEO_FORMAT_1080P_5994,  1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1001, 60000, API_VEGA_BQB_FPS_59_94 },
    { 1, INPUT_VIDEO_FORMAT_1080P_60,    1920, 1080,  API_VEGA3311_CAP_RESOLUTION_1920x1080, API_VENC_RESOLUTION_1920x1080, 1000, 60000, API_VEGA_BQB_FPS_60 },
    //
    { 1, INPUT_VIDEO_FORMAT_2160P_25,    3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_3840x2160, 1000, 25000, API_VEGA_BQB_FPS_25 },
    { 1, INPUT_VIDEO_FORMAT_2160P_2997,  3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_3840x2160, 1001, 30000, API_VEGA_BQB_FPS_29_97 },
    { 1, INPUT_VIDEO_FORMAT_2160P_30,    3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_3840x2160, 1000, 30000, API_VEGA_BQB_FPS_30 },
    { 1, INPUT_VIDEO_FORMAT_2160P_50,    3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_3840x2160, 1000, 50000, API_VEGA_BQB_FPS_50 },
    { 1, INPUT_VIDEO_FORMAT_2160P_5994,  3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_3840x2160, 1001, 60000, API_VEGA_BQB_FPS_59_94 },
    { 1, INPUT_VIDEO_FORMAT_2160P_60,    3840, 2160,  API_VEGA3311_CAP_RESOLUTION_3840x2160, API_VENC_RESOLUTION_3840x2160, 1000, 60000, API_VEGA_BQB_FPS_60 },
};

/* For a given vegas format and framerate, return a record with translations into OBE speak. */
const struct obe_to_vega_video *lookupVegaCaptureResolution(int std, int framerate, int interlaced)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_vega_video)); i++) {
		const struct obe_to_vega_video *fmt = &video_format_tab[i];
		if (fmt->vegaCaptureResolution == std && fmt->vegaFramerate == framerate && fmt->progressive == !interlaced) {
			return fmt;
		}
	}

	return NULL;
}

const struct obe_to_vega_video *lookupVegaStandardByResolution(int width, int height, int framerate)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_vega_video)); i++) {
		const struct obe_to_vega_video *fmt = &video_format_tab[i];
		if (fmt->width == width && fmt->height == height && fmt->vegaFramerate == framerate) {
			return fmt;
		}
	}

	return NULL;
}

int lookupVegaFramerate(int num, int den, API_VEGA_BQB_FPS_E *fps)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_vega_video)); i++) {
		const struct obe_to_vega_video *fmt = &video_format_tab[i];
		if (fmt->timebase_num == num && fmt->timebase_den == den) {
			*fps = fmt->vegaFramerate;
                        return 0; /* Success */
		}
	}

	return -1; /* Error */
}

void klvanc_packet_header_dump_console(struct klvanc_packet_header_s *pkt)
{
        printf("pkt %p\n", pkt);
        printf("\ttype -> %d\n", pkt->type);
        printf("\tdid  -> %04x\n", pkt->did);
        printf("\tsdid -> %04x\n", pkt->dbnsdid);
        printf("\tline -> %d\n", pkt->lineNr);
        printf("\tlen  -> %d\n", pkt->payloadLengthWords);
}

const char *lookupVegaCapState(API_VEGA3311_CAP_STATE_E s)
{
        if (s == API_VEGA3311_CAP_STATE_STANDBY)        return "STANDBY";
        if (s == API_VEGA3311_CAP_STATE_CAPTURING)      return "CAPTURING";
        if (s == API_VEGA3311_CAP_STATE_WAITING)        return "WAITING";

        return "UNKNOWN";
}

const char *lookupVegaEnableState(API_VEGA3311_CAP_ENABLE_E s)
{
        if (s == API_VEGA3311_CAP_ENABLE_ON)        return "ON";
        if (s == API_VEGA3311_CAP_ENABLE_OFF)       return "OFF";

        return "UNKNOWN";
}

void vega_dump_signals_to_console(
        API_VEGA3311_CAPTURE_DEVICE_INFO_T st_dev_info,
        API_VEGA3311_CAPTURE_FORMAT_T input_src_info)
{
        printf("\tu8FpgaDriverVersion: %s\n", &st_dev_info.u8FpgaDriverVersion[0]);
        printf("\tu8FpgaHardwareVersion: %s\n", &st_dev_info.u8FpgaHardwareVersion[0]);
        printf("\tu8FpgaFirmwareVersion: %s\n", &st_dev_info.u8FpgaFirmwareVersion[0]);
        printf("\tu8MBFirmwareVersion: %s\n", &st_dev_info.u8MBFirmwareVersion[0]);
        printf("\teVideoState / enable: %s / %s\n", lookupVegaCapState(input_src_info.eVideoState), lookupVegaEnableState(input_src_info.eVideoEnable));
        printf("\teAudioState / enable: %s / %s\n", lookupVegaCapState(input_src_info.eAudioState), lookupVegaEnableState(input_src_info.eAudioEnable));
        printf("\teAncdState / enable: %s / %s\n", lookupVegaCapState(input_src_info.eAncdState), lookupVegaEnableState(input_src_info.eAncdEnable));
         
        if (st_dev_info.eInputSource == API_VEGA3311_CAP_INPUT_SOURCE_SDI)
                printf("\tinput source: SDI\n");
        else if (st_dev_info.eInputSource == API_VEGA3311_CAP_INPUT_SOURCE_HDMI)
                printf("\tinput source: HDMI\n");
        else if (st_dev_info.eInputSource == API_VEGA3311_CAP_INPUT_SOURCE_10G_ETH_2022)
                printf("\tinput source: 10G ethernet,use ST2022\n");
        else if (st_dev_info.eInputSource == API_VEGA3311_CAP_INPUT_SOURCE_12G_SDI)
                printf("\tinput source: 12G SDI\n");
        else if (st_dev_info.eInputSource == API_VEGA3311_CAP_INPUT_SOURCE_10G_ETH_2110)
                printf("\tinput source: 10G ethernet,use ST2110\n");
        else
                printf("\tinput source: Unknown\n");

        if (st_dev_info.eInputMode == API_VEGA3311_CAP_INPUT_MODE_1CHN_QUAD)
                printf("\tinput mode: 1 Channel UltraHD QUAD\n");
        else if (st_dev_info.eInputMode == API_VEGA3311_CAP_INPUT_MODE_1CHN_2SI)
                printf("\tinput mode: 1 Channel UltraHD 2SI\n");
        else if (st_dev_info.eInputMode == API_VEGA3311_CAP_INPUT_MODE_4CHN)
                printf("\tinput mode: 4 Channel FullHD\n");
        else
                printf("\tinput mode: UNKNOWN\n");

        printf("\tAncillary data window settings:\n");
        printf("\t\tHANC      space: %d\n", st_dev_info.tAncWindowSetting.bHanc);
        printf("\t\tPRE-VANC  space: %d\n", st_dev_info.tAncWindowSetting.bPreVanc);
        printf("\t\tPOST-VANC space: %d\n", st_dev_info.tAncWindowSetting.bPostVanc);

        switch (st_dev_info.eImageFmt) {
        case API_VEGA3311_CAP_IMAGE_FORMAT_NV12: printf("\tImage Fmt: NV12\n"); break;
        case API_VEGA3311_CAP_IMAGE_FORMAT_NV16: printf("\tImage Fmt: NV16\n"); break;
        case API_VEGA3311_CAP_IMAGE_FORMAT_YV12: printf("\tImage Fmt: YV12\n"); break;
        //case API_VEGA3311_CAP_IMAGE_FORMAT_I420: printf("\tImage Fmt: I420\n"); break;
        case API_VEGA3311_CAP_IMAGE_FORMAT_YV16: printf("\tImage Fmt: YV16\n"); break;
        case API_VEGA3311_CAP_IMAGE_FORMAT_YUY2: printf("\tImage Fmt: YUY2\n"); break;
        case API_VEGA3311_CAP_IMAGE_FORMAT_P010: printf("\tImage Fmt: P010\n"); break;
        case API_VEGA3311_CAP_IMAGE_FORMAT_P210: printf("\tImage Fmt: P210\n"); break;
        default:
                printf("\tImage Fmt: NV16\n");
                break;
        }

        switch (st_dev_info.eAudioLayouts) {
        case API_VEGA3311_CAP_AUDIO_LAYOUT_MONO:      printf("\tAudio layouts: mono\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_STEREO:    printf("\tAudio layouts: stereo\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_2P1:       printf("\tAudio layouts: 2.1 channel\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_3P0:       printf("\tAudio layouts: 3.0 channel\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_QUAD:      printf("\tAudio layouts: quad\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_QUAD_SIDE: printf("\tAudio layouts: quad side\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_3P1:       printf("\tAudio layouts: 3.1 channel\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_5P0:       printf("\tAudio layouts: 5.0 channel \n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_5P0_SIDE:  printf("\tAudio layouts: 5.0 channel side\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_5P1:       printf("\tAudio layouts: 5.1 channel\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_5P1_SIDE:  printf("\tAudio layouts: 5.1 channel side\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_7P0:       printf("\tAudio layouts: 7.0 channel\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_7P1:       printf("\tAudio layouts: 7.1 channel\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_16P0:      printf("\tAudio layouts: 16.0 channel\n"); break;
        case API_VEGA3311_CAP_AUDIO_LAYOUT_PASS_THROUGH:      printf("\tAudio layouts: fpga format\n"); break;
        default:
                break;
        }

        if (input_src_info.eSourceSdiLocked == API_VEGA3311_CAP_SRC_STATUS_LOCKED) {
                printf("\tchannel is locked\n");
        } else {
                printf("\tchannel is unlocked\n");
        }

        switch (input_src_info.eSourceSdiResolution) {
        case API_VEGA3311_CAP_RESOLUTION_720x480:   printf("\tResolution: 720x480\n"); break;
        case API_VEGA3311_CAP_RESOLUTION_720x576:   printf("\tResolution: 720x576\n"); break;
        case API_VEGA3311_CAP_RESOLUTION_1280x720:  printf("\tResolution: 1280x720\n"); break;
        case API_VEGA3311_CAP_RESOLUTION_1920x1080: printf("\tResolution: 1920x1080\n"); break;
        case API_VEGA3311_CAP_RESOLUTION_2048x1080: printf("\tResolution: 2048x1080\n"); break;
        case API_VEGA3311_CAP_RESOLUTION_3840x2160: printf("\tResolution: 3840x2160\n"); break;
        case API_VEGA3311_CAP_RESOLUTION_4096x2160: printf("\tResolution: 4096x2160\n"); break;
        default:
                printf("\tResolution: unknown\n");
                break;
        }

        switch (input_src_info.eSourceSdiChromaFmt) {
        case API_VEGA3311_CAP_CHROMA_FORMAT_420: printf("\tChroma Fmt: 420\n"); break;
        case API_VEGA3311_CAP_CHROMA_FORMAT_444: printf("\tChroma Fmt: 444\n"); break;
        case API_VEGA3311_CAP_CHROMA_FORMAT_422: printf("\tChroma Fmt: 422\n"); break;
        default:
                printf("\tChroma Fmt: unknown\n");
                break;
        }

        switch (input_src_info.eSourceSdiSignalLevel) {
        case API_VEGA3311_CAP_SDI_LEVEL_B: printf("\tSDI signal: level B\n"); break;
        default:
        case API_VEGA3311_CAP_SDI_LEVEL_A: printf("\tSDI signal: level A\n"); break;
        }

        if (input_src_info.bSourceSdiInterlace) {
                if (input_src_info.bSourceSdiFieldTop) {
                        printf("\tSDI scan type: interlace, top field\n");
                } else {
                        printf("\tSDI scan type: interlace, bottom field\n");
                }
        } else {
                printf("\tSDI scan type: progressive\n");
        }

        switch (input_src_info.eSourceSdiMode) {
        case API_VEGA3311_CAP_SDI_MODE_HD: printf("\tSDI Mode: HD\n"); break;
        case API_VEGA3311_CAP_SDI_MODE_SD: printf("\tSDI Mode: SD\n"); break;
        case API_VEGA3311_CAP_SDI_MODE_3G: printf("\tSDI Mode: 3G\n"); break;
        case API_VEGA3311_CAP_SDI_MODE_6G: printf("\tSDI Mode: 6G\n"); break;
        case API_VEGA3311_CAP_SDI_MODE_12G: printf("\tSDI Mode: 12G\n"); break;
        default:
                printf("\tSDI Mode: unknown\n");
                break;
        }

        switch (input_src_info.eSourceSdiBitDepth) {
        case API_VEGA3311_CAP_BIT_DEPTH_12: printf("\tBit Depth: 12 bits\n"); break;
        case API_VEGA3311_CAP_BIT_DEPTH_8:  printf("\tBit Depth: 8 bits\n");  break;
        case API_VEGA3311_CAP_BIT_DEPTH_10: printf("\tBit Depth: 10 bits\n"); break;
        default:
                printf("\tBit Depth: unknown\n");
                break;
        }

        switch (input_src_info.eSourceSdiFrameRate) {
        case API_VEGA3311_CAP_FPS_24:    printf("\tFrame per sec: 24\n"); break;
        case API_VEGA3311_CAP_FPS_25:    printf("\tFrame per sec: 25\n"); break;
        case API_VEGA3311_CAP_FPS_29_97: printf("\tFrame per sec: 29.97\n"); break;
        case API_VEGA3311_CAP_FPS_30:    printf("\tFrame per sec: 30\n"); break;
        case API_VEGA3311_CAP_FPS_50:    printf("\tFrame per sec: 50\n"); break;
        case API_VEGA3311_CAP_FPS_59_94: printf("\tFrame per sec: 59.94\n"); break;
        case API_VEGA3311_CAP_FPS_60:    printf("\tFrame per sec: 60\n"); break;
        default:
                printf("\tFrame per sec: unknown\n");
                break;
        }
}

int vega_has_source_signal_changed(API_VEGA3311_CAPTURE_FORMAT_T *src, API_VEGA3311_CAPTURE_FORMAT_T *dst)
{
        if (src->eSourceSdiResolution != dst->eSourceSdiResolution)
                return 1;
        if (src->eSourceSdiFrameRate != dst->eSourceSdiFrameRate)
                return 1;
        if (src->bSourceSdiInterlace != dst->bSourceSdiInterlace)
                return 1;

        return 0; /* False */
}


#endif /* #if HAVE_VEGA3311_CAP_TYPES_H */
