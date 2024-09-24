#if HAVE_VEGA3311_CAP_TYPES_H

/* Comments / Questions:
 *
 *                      3301  3311 
 *     Audio channels      8    16
 *   608/708 Captions      ?     Y
 *                AFD      ?     Y
 *             Decode      N     Y
 *            Latency    2-5   2-5  frames.
 *          gstreamer      N     N
 * 
 * Move this entire file into a vega3311 specific section and restore the vega 3301 implementation.
 * 
 * Duration / runtime issue with clocks going negative when the PCR
 * wraps. Fix this.
 * 
 * 2022-06-10 Testign3311 with:
 * vega3311_0_89_20210910_03_0000_3GSD_wt.bin
 * 4x 1080p60 HEVC encodes, 4xmp2, latency is 110ms, cpuload 20% per encoder instance.
 * 
 * What does LOS look like? (especially, blue video but no audio?).
 * What does bitstream audio look like?
 * 
 * Does the PCR keep rolling when the signal is disconnected?
 * 
 * Is the audio from SDI bitstream capable?
 * - They think they're just collecting and handing us HANC.
 * - so in principle yes.
 * 
 * Encode some actual 10bit video, check we're not losing 10-to-8 in the encode because
 * of the defined colorspace.
 * 
 * Test interlaced formats.
 *
 * How do I test HDR encoding? What does the workflow look like?
 * 
 * Can I write into the NV12 pixels pre-encode? (video injection).
 * Enable V210 burnwriter codes pre-encode.
 * 
 * Increase/test 8 additional audio channels on the 3311.
 * 
 * Mediainfo not working on ts files
 */

/*****************************************************************************
 * vega.cpp: Audio/Video from the VEGA encoder cards
 *****************************************************************************
 * Copyright (C) 2021-2022 LTN
 *
 * Authors: Steven Toth <steven.toth@ltnglobal.com>
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
#include "histogram.h"
#include "vega-3311.h"

#if VEGA_IS_SOURCING_NDI
#pragma message "VEGA: NDI sourcing is enabled"
#endif

extern "C"
{
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>
}

#define MODULE_PREFIX "[vega]: "

#define LOCAL_DEBUG 0

#define FORCE_10BIT 1

#define CAP_DBG_LEVEL API_VEGA3311_CAP_DBG_LEVEL_0

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define VEGA_VERSION STR(VEGA331X_KERNEL_VERSION) "." STR(VEGA331X_MAJOR_VERSION) "." STR(VEGA331X_MINOR_VERSION)

using namespace std;

const char *vega3311_sdk_version = VEGA_VERSION;

static int configureCodec(vega_opts_t *opts)
{
#if LOCAL_DEBUG
        printf("%s()\n", __func__);
#endif
        vega_ctx_t *ctx = &opts->ctx;
        obe_output_stream_t *os = obe_core_get_output_stream_by_index(ctx->h, 0);
        if (!os) {
                return -1;
        }
        x264_param_t *p = &os->avc_param;

        printf(MODULE_PREFIX "Configuring codec based on os_stream_format type 0x%x\n", os->stream_format);

        switch(os->stream_format) {
        case VIDEO_AVC_VEGA3311:
        case VIDEO_HEVC_VEGA3311:
                break;
        default:
                fprintf(stderr, MODULE_PREFIX "Invalid os->stream_format, for type 0x%x\n", os->stream_format);
                return -1;
        }

        if (OPTION_ENABLED(4k_2si)) {
                printf(MODULE_PREFIX "Enabling '4K' 12G-2SI mode\n");
                opts->codec.inputMode    = API_VEGA3311_CAP_INPUT_MODE_1CHN_2SI;
                opts->codec.inputSource  = API_VEGA3311_CAP_INPUT_SOURCE_12G_SDI;
        } else
        if (OPTION_ENABLED(4k_quad)) {
                printf(MODULE_PREFIX "Enabling '4K' QUAD mode\n");
                opts->codec.inputMode    = API_VEGA3311_CAP_INPUT_MODE_1CHN_QUAD;
                opts->codec.inputSource  = API_VEGA3311_CAP_INPUT_SOURCE_SDI;

        } else { 
                printf(MODULE_PREFIX "Enabling '4 individual channels' mode\n");
                opts->codec.inputMode    = API_VEGA3311_CAP_INPUT_MODE_4CHN;
                opts->codec.inputSource  = API_VEGA3311_CAP_INPUT_SOURCE_SDI;
        }
        opts->codec.sdiLevel     = API_VEGA3311_CAP_SDI_LEVEL_A;
        opts->codec.audioLayout  = API_VEGA3311_CAP_AUDIO_LAYOUT_16P0;
        opts->codec.pixelFormat  = API_VEGA3311_CAP_IMAGE_FORMAT_NV16;
        opts->codec.bitrate_kbps = p->rc.i_bitrate;
        opts->codec.gop_size     = (API_VEGA_BQB_GOP_SIZE_E)p->i_keyint_max;
        opts->codec.bframes      = (API_VEGA_BQB_B_FRAME_NUM_E)p->i_bframe;
        opts->codec.width        = p->i_width;
        opts->codec.height       = p->i_height;

        const struct obe_to_vega_video *fmt = lookupVegaStandardByResolution(opts->codec.width, opts->codec.height, API_VEGA3311_CAP_FPS_60);
        if (!fmt) {
		fprintf(stderr, MODULE_PREFIX "unable to query encoder parameters for specific width, height and framerate\n");
		return -1;
        }
        opts->codec.encodingResolution = (API_VEGA_BQB_RESOLUTION_E)fmt->vegaEncodingResolution;
        opts->codec.interlaced = p->b_interlaced;

        if (lookupVegaFramerate(p->i_fps_den, p->i_fps_num, &opts->codec.fps) < 0) {
		fprintf(stderr, MODULE_PREFIX "unable to query encoder framerate %d, %d\n", p->i_fps_num, p->i_fps_den);
		return -1;
        }

        if (opts->codec.bitDepth == API_VEGA_BQB_BIT_DEPTH_10) {
                p->i_csp |= X264_CSP_HIGH_DEPTH;
        }

        if ((p->i_csp & X264_CSP_I420) && ((p->i_csp & X264_CSP_HIGH_DEPTH) == 0)) {
                /* 4:2:0 8bit via NV12 */
                printf(MODULE_PREFIX "Selecting 4:2:0 8bit via NV12\n");
                opts->codec.chromaFormat = API_VEGA_BQB_CHROMA_FORMAT_420;
#if VEGA_IS_SOURCING_NDI
                opts->codec.chromaFormat = API_VEGA_BQB_CHROMA_FORMAT_422_TO_420;
#endif
                opts->codec.bitDepth     = API_VEGA_BQB_BIT_DEPTH_8;
                opts->codec.pixelFormat  = API_VEGA3311_CAP_IMAGE_FORMAT_NV12;
                opts->codec.eFormat      = API_VEGA_BQB_IMAGE_FORMAT_NV12;
                // OK
        } else
        if ((p->i_csp & X264_CSP_I420) && (p->i_csp & X264_CSP_HIGH_DEPTH)) {
                /* 4:2:0 10bit via PP01 */
                printf(MODULE_PREFIX "Selecting 4:2:0 10bit via PP01\n");
		fprintf(stderr, MODULE_PREFIX "using colorspace 4:2:0 10bit (not supported)\n");
                opts->codec.chromaFormat = API_VEGA_BQB_CHROMA_FORMAT_422_TO_420;
                opts->codec.bitDepth     = API_VEGA_BQB_BIT_DEPTH_10;
                opts->codec.pixelFormat  = API_VEGA3311_CAP_IMAGE_FORMAT_Y210; /* technically 4:2:2, SDK converts to 4:2:0 */
                opts->codec.eFormat      = API_VEGA_BQB_IMAGE_FORMAT_YUV420P010;
        } else
        if ((p->i_csp & X264_CSP_I422) && ((p->i_csp & X264_CSP_HIGH_DEPTH) == 0)) {
                /* 4:2:2 8bit via NV16 */
                printf(MODULE_PREFIX "Selecting 4:2:2 8bit via NV16\n");
                opts->codec.chromaFormat = API_VEGA_BQB_CHROMA_FORMAT_422;
                opts->codec.bitDepth     = API_VEGA_BQB_BIT_DEPTH_8;
                opts->codec.pixelFormat  = API_VEGA3311_CAP_IMAGE_FORMAT_NV16;
                opts->codec.eFormat      = API_VEGA_BQB_IMAGE_FORMAT_NV16;
                // NOT ok according to VLC
        } else
        if ((p->i_csp & X264_CSP_I422) && (p->i_csp & X264_CSP_HIGH_DEPTH)) {
                /* 4:2:2 10bit via Y210 and colorspace conversion  */
		fprintf(stderr, MODULE_PREFIX "Selecting 4:2:2 10bit via Y210\n");
                opts->codec.chromaFormat = API_VEGA_BQB_CHROMA_FORMAT_422;
                opts->codec.bitDepth     = API_VEGA_BQB_BIT_DEPTH_10;
                opts->codec.pixelFormat  = API_VEGA3311_CAP_IMAGE_FORMAT_Y210;
                opts->codec.eFormat      = API_VEGA_BQB_IMAGE_FORMAT_YUV422P010;
        } else {
		fprintf(stderr, MODULE_PREFIX "unable to determine colorspace, i_csp = 0x%x\n", p->i_csp);
		return -1;
        }

        printf(MODULE_PREFIX "encoder.device       = %d\n", opts->brd_idx);
        printf(MODULE_PREFIX "encoder.eFormat      = %d '%s'\n", opts->codec.eFormat, lookupVegaEncodingImageFormatName(opts->codec.eFormat));
        printf(MODULE_PREFIX "encoder.inputsource  = %d '%s'\n", opts->codec.inputSource, lookupVegaInputSourceName(opts->codec.inputSource));
        printf(MODULE_PREFIX "encoder.inputport    = %d\n", opts->card_idx);
        printf(MODULE_PREFIX "encoder.sdilevel     = %d '%s'\n", opts->codec.sdiLevel, lookupVegaSDILevelName(opts->codec.sdiLevel));
        printf(MODULE_PREFIX "encoder.inputmode    = %d '%s'\n", opts->codec.inputMode, lookupVegaInputModeName(opts->codec.inputMode));
        printf(MODULE_PREFIX "encoder.bitrate_kbps = %d\n", opts->codec.bitrate_kbps);
        printf(MODULE_PREFIX "encoder.gop_size     = %d\n", opts->codec.gop_size);
        printf(MODULE_PREFIX "encoder.bframes      = %d\n", opts->codec.bframes);
        printf(MODULE_PREFIX "encoder.width        = %d\n", opts->codec.width);
        printf(MODULE_PREFIX "encoder.height       = %d\n", opts->codec.height);
        printf(MODULE_PREFIX "encoder.resolution   = %d '%s'\n", opts->codec.encodingResolution, lookupVegaEncodingResolutionName(opts->codec.encodingResolution));
        printf(MODULE_PREFIX "encoder.fps          = %d\n", opts->codec.fps);
        printf(MODULE_PREFIX "encoder.chroma       = %d '%s'\n", opts->codec.chromaFormat, lookupVegaEncodingChromaName(opts->codec.chromaFormat));
        printf(MODULE_PREFIX "encoder.bitdepth     = %d '%s'\n", opts->codec.bitDepth, lookupVegaBitDepthName(opts->codec.bitDepth));
        printf(MODULE_PREFIX "encoder.pixelformat  = %d '%s'\n", opts->codec.pixelFormat, lookupVegaPixelFormatName(opts->codec.pixelFormat));
        printf(MODULE_PREFIX "encoder.interlaced   = %d\n", opts->codec.interlaced);

        return 0; /* Success */
}

static void close_device(vega_opts_t *opts)
{
	vega_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Closing device#%d port#%d\n", opts->brd_idx, opts->card_idx);

        /* Stop all of the hardware */
        pthread_mutex_lock(&ctx->bDoLastFrame_lock);
        ctx->bDoLastFrame = true;
        pthread_mutex_unlock(&ctx->bDoLastFrame_lock);

#if VEGA_IS_SOURCING_NDI
        vega_ndi_stop(opts);
#else
	VEGA3311_CAP_Stop(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, API_VEGA3311_CAP_MEDIA_TYPE_VIDEO);
	VEGA3311_CAP_Stop(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, API_VEGA3311_CAP_MEDIA_TYPE_AUDIO);
        VEGA3311_CAP_Stop(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, API_VEGA3311_CAP_MEDIA_TYPE_ANC_DATA);
#endif

        for (int i = 0; i < 30; i++) {
                int ret = VEGA_BQB_ENC_Stop((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx);
                printf("ret %d, bDoLastFrame %d bLastFramePushed %d\n", ret, ctx->bDoLastFrame, ctx->bLastFramePushed);
                if (ret == API_VEGA_BQB_RET_FAIL) {
                        printf("API fail, probably ok\n");
                        break;
                }
                if (ret == API_VEGA_BQB_RET_NO_LAST_FRAME) {
                        printf("LAST FRAME not yet being pushed!\n");
                }
                if (ret == API_VEGA_BQB_RET_NO_LAST_ES) {
                        printf("LAST ES not yet being popped!\n");
                }
                if (ret == API_VEGA_BQB_RET_SUCCESS) {
                        printf("Stop was a success\n");
                        break;
                }
                usleep(100 * 1000);
        }
//        VEGA_BQB_ENC_Stop((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx);
//        sleep(2);

	VEGA_BQB_ENC_Exit((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx);
	VEGA3311_CAP_Exit(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx);

        if (ctx->avr)
                swr_free(&ctx->avr);

        if (ctx->vanchdl) {
                klvanc_context_destroy(ctx->vanchdl);
                ctx->vanchdl = 0;
        }

        if (ctx->smpte2038_ctx) {
                klvanc_smpte2038_packetizer_free(&ctx->smpte2038_ctx);
                ctx->smpte2038_ctx = 0;
        }

        if (ctx->fc_ctx) {
                filter_compress_free(ctx->fc_ctx);
        }

	printf(MODULE_PREFIX "Closed card idx #%d\n", opts->card_idx);
}

static int open_device(vega_opts_t *opts, int probe)
{
	printf(MODULE_PREFIX "%s() probe = %s\n", __func__, probe == 1 ? "true" : "false");
	API_VEGA3311_CAP_RET_E capret;
        API_VEGA_BQB_RET encret;

	vega_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Searching for device#0 port#%d\n", opts->card_idx);
        
        if (klvanc_context_create(&ctx->vanchdl) < 0) {
                fprintf(stderr, MODULE_PREFIX "Error initializing VANC library context\n");
        } else {
                ctx->vanchdl->verbose = 0;
                ctx->vanchdl->callbacks = &vega3311_vanc_callbacks;
                ctx->vanchdl->callback_context = opts;
                ctx->vanchdl->allow_bad_checksums = 1;
                //ctx->last_vanc_cache_dump = 0;

#if 0
                if (OPTION_ENABLED(vanc_cache)) {
                        /* Turn on the vanc cache, we'll want to query it later. */
                        decklink_ctx->last_vanc_cache_dump = 1;
                        fprintf(stdout, "Enabling option VANC CACHE, interval %d seconds\n", VANC_CACHE_DUMP_INTERVAL);
                        klvanc_context_enable_cache(decklink_ctx->vanchdl);
                }
#endif
        }

        if (ctx->h->enable_scte35) {
                klsyslog_and_stdout(LOG_INFO, MODULE_PREFIX "Enabling option SCTE35");
        } else {
                /* Disable SCTE104 parsing callbacks, configuration optimization. */
                vega3311_vanc_callbacks.scte_104 = NULL;
        }

        if (OPTION_ENABLED(smpte2038)) {
                klsyslog_and_stdout(LOG_INFO, MODULE_PREFIX "Enabling option SMPTE2038");
                if (klvanc_smpte2038_packetizer_alloc(&ctx->smpte2038_ctx) < 0) {
                        fprintf(stderr, MODULE_PREFIX "Unable to allocate a SMPTE2038 context.\n");
                }
        }

        if (OPTION_ENABLED(4k_quad)) {
                klsyslog_and_stdout(LOG_INFO, MODULE_PREFIX "Enabling option 4K QUAD Input");
        }
        if (OPTION_ENABLED(4k_2si)) {
                klsyslog_and_stdout(LOG_INFO, MODULE_PREFIX "Enabling option 4K 2SI Input");
        }

        if (OPTION_ENABLED(hdr)) {
                klsyslog_and_stdout(LOG_INFO, MODULE_PREFIX "Enabling option HDR");
        } else {
                /* Disable parsing callbacks, configuration optimization. */
                vega3311_vanc_callbacks.smpte_2108_1 = NULL;
        }


        API_VEGA3311_CAPTURE_DEVICE_INFO_T st_dev_info;

        //VEGA3311_CAP_ResetChannel(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx);
        //sleep(3);
        capret = VEGA3311_CAP_Init(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, CAP_DBG_LEVEL);
        if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "failed to initialize the capture input\n");
                return -1;
        }
        printf(MODULE_PREFIX "Found SDI hardware device#0 port#%d\n", opts->card_idx);
		
        capret = VEGA3311_CAP_GetProperty(opts->brd_idx,
                (API_VEGA3311_CAP_CHN_E)opts->card_idx, &st_dev_info);

        if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "failed to get hardware properties\n");
                return -1;
        }

        capret = VEGA3311_CAP_QueryStatus(opts->brd_idx,
                (API_VEGA3311_CAP_CHN_E)opts->card_idx, &ctx->detectedFormat);

        if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "failed to get signal properties\n");
                return -1;
        }

	if (probe == 1) {
                vega_dump_signals_to_console(st_dev_info, ctx->detectedFormat);
	}

	if (ctx->detectedFormat.eSourceSdiLocked != API_VEGA3311_CAP_SRC_STATUS_LOCKED) {
		fprintf(stderr, MODULE_PREFIX "No signal found\n");
		return -1;
	}

	/* We need to understand how much VANC we're going to be receiving. */
	const struct obe_to_vega_video *std = lookupVegaCaptureResolution(
                ctx->detectedFormat.eSourceSdiResolution,
                ctx->detectedFormat.eSourceSdiFrameRate,
                ctx->detectedFormat.bSourceSdiInterlace);
	if (std == NULL) {
		fprintf(stderr, MODULE_PREFIX "No detected standard for vega aborting\n");
		exit(0);
	}

	opts->brd_idx = (API_VEGA_BQB_DEVICE_E)0;
	opts->width = std->width;
	opts->height = std->height;
	opts->interlaced = std->progressive ? 0 : 1;
	opts->timebase_den = std->timebase_den;
	opts->timebase_num = std->timebase_num;
	opts->video_format = std->obe_name;

	fprintf(stderr, MODULE_PREFIX "Detected resolution %dx%d%c @ %d/%d\n",
		opts->width, opts->height,
                opts->interlaced ? 'i' : 'p',
		opts->timebase_den, opts->timebase_num);

        if (probe) {
                return 0; /* Success */
        }

        /* We're opening the device for streaming, configure the unit as needed. */
        ctx->avr = swr_alloc();
        if (!ctx->avr) {
                fprintf(stderr, MODULE_PREFIX "Could not alloc libswresample context\n");
                return -1;
        }

        ltn_histogram_alloc_video_defaults(&ctx->hg_callback_audio, "audio arrival latency");
        ltn_histogram_alloc_video_defaults(&ctx->hg_callback_video, "video arrival latency");

        /* Give libswresample a made up channel map.
         * Convert S16 interleaved to S32P planar.
         */
        av_opt_set_int(ctx->avr, "in_channel_layout",   (1 << opts->num_audio_channels) - 1, 0 );
        av_opt_set_int(ctx->avr, "in_sample_fmt",       AV_SAMPLE_FMT_S16, 0 );
        av_opt_set_int(ctx->avr, "in_sample_rate",      48000, 0 );
        av_opt_set_int(ctx->avr, "out_channel_layout",  (1 << opts->num_audio_channels) - 1, 0 );
        av_opt_set_int(ctx->avr, "out_sample_fmt",      AV_SAMPLE_FMT_S32P, 0 );
        av_opt_set_int(ctx->avr, "out_sample_rate",     48000, 0 );

        if (swr_init(ctx->avr) < 0) {
                fprintf(stderr, MODULE_PREFIX "couldn't setup sample rate conversion\n");
                return -1;
        }

        obe_output_stream_t *os = obe_core_get_output_stream_by_index(ctx->h, 0);

        printf(MODULE_PREFIX "Configuring advantec codec for stream_format type 0x%x\n", os->stream_format);

        switch (os->stream_format) {
        case VIDEO_HEVC_VEGA3311:
                if (vega3311_video_configure_hevc(opts) < 0) {
                        fprintf(stderr, MODULE_PREFIX "failed to configure codec HEVC, aborting - unstable\n");
                        return -1;
                }
                break;
        case VIDEO_AVC_VEGA3311:
                if (vega3311_video_configure_avc(opts) < 0) {
                        fprintf(stderr, MODULE_PREFIX "failed to configure codec AVC, aborting - unstable\n");
                        return -1;
                }
                break;
        default:
                fprintf(stderr, MODULE_PREFIX "unsupported stream_format type 0x%x, aborting\n", os->stream_format);
                return -1;
        }

        API_VEGA_BQB_DESC_T desc = { { 0 } };
        VEGA_BQB_ENC_InitParamToString(ctx->init_params, &desc);
        printf(MODULE_PREFIX "initial param to string:\n%s\n", desc.content);

        /* Open the capture hardware here */
        if (VEGA_BQB_ENC_Init((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx, &ctx->init_params)) {
                fprintf(stderr, MODULE_PREFIX "VEGA_BQB_ENC_Init: failed to initialize the encoder\n");
                return -1;
        }

        if (os->stream_format == VIDEO_HEVC_VEGA3311) {
                encret = VEGA_BQB_ENC_RegisterCallback((API_VEGA_BQB_DEVICE_E)opts->brd_idx,
                        (API_VEGA_BQB_CHN_E)opts->card_idx, vega3311_video_hevc_compressed_callback, opts);
                fprintf(stderr, MODULE_PREFIX "Registered HEVC Codec callback\n");
        } else
        if (os->stream_format == VIDEO_AVC_VEGA3311) {
                encret = VEGA_BQB_ENC_RegisterAvcCallback((API_VEGA_BQB_DEVICE_E)opts->brd_idx,
                        (API_VEGA_BQB_CHN_E)opts->card_idx, vega3311_video_avc_compressed_callback, opts);
                fprintf(stderr, MODULE_PREFIX "Registered AVC Codec callback\n");
        }
        else {
                encret = API_VEGA_BQB_RET_FAIL;
        }

        if (encret != API_VEGA_BQB_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "ERROR: failed to register encode callback function\n");
                return -1;
        }

        printf(MODULE_PREFIX "Registering Capture Video callback\n");

        capret = VEGA3311_CAP_RegisterVideoCallback(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx,
                vega3311_video_capture_callback, opts);
        if (capret != API_VEGA3311_CAP_RET_SUCCESS)
        {
                fprintf(stderr, MODULE_PREFIX "ERROR: failed to register video capture callback function\n");
                return -1;
        }
        printf(MODULE_PREFIX "Registering Capture Audio callback\n");

        capret = VEGA3311_CAP_RegisterAudioCallback(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx,
                vega3311_audio_callback, opts);
        if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "ERROR: failed to register audio capture callback function\n");
                return -1;
        }

        printf(MODULE_PREFIX "Starting hardware encoder\n");

        encret = VEGA_BQB_ENC_Start((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx);
        if (encret != API_VEGA_BQB_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "ERROR: failed to enc start\n");
                return -1;
        }
        
        ctx->ch_init_param.eFormat       = opts->codec.pixelFormat;
#if 1
        //ctx->ch_init_param.eFormat       = API_VEGA3311_CAP_IMAGE_FORMAT_P010;
        //ctx->ch_init_param.eFormat       = API_VEGA3311_CAP_IMAGE_FORMAT_P210;
        //ctx->ch_init_param.eFormat       = API_VEGA3311_CAP_IMAGE_FORMAT_V210;
        ctx->ch_init_param.eFormat       = API_VEGA3311_CAP_IMAGE_FORMAT_Y210; // hevc
        ctx->ch_init_param.eFormat       = opts->codec.pixelFormat; // avc 4:2:0
#endif
        ctx->ch_init_param.eInputMode    = opts->codec.inputMode;
        ctx->ch_init_param.eInputSource  = opts->codec.inputSource;
        ctx->ch_init_param.eAudioLayouts = opts->codec.audioLayout;

        /* Configure HANd process. We don't want HANC audio packets but we'll take everything else */
        ctx->ch_init_param.tAncWindowSetting.bHanc = false;     /* Enable HANC ancillary data processing (Audio packet) */
        ctx->ch_init_param.tAncWindowSetting.bPreVanc = true;
        ctx->ch_init_param.tAncWindowSetting.bPostVanc = true;

//	API_VEGA3311_CAP_ANC_WINDOW_T          tAncWindowSetting;
//	API_VEGA3311_CAP_ANC_REMAPPING_T       tAncRemapSetting;

        printf("--- Capture Configuration VEGA3311_CAP_Config(%d, %d, %%p);\n", opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx);
        printf("capture.eFormat             = %2d %s\n", ctx->ch_init_param.eFormat, lookupVegaPixelFormatName(ctx->ch_init_param.eFormat));
        printf("capture.eInputMode          = %2d %s\n", ctx->ch_init_param.eInputMode, lookupVegaInputModeName(ctx->ch_init_param.eInputMode));
        printf("capture.eInputSource        = %2d %s\n", ctx->ch_init_param.eInputSource, lookupVegaInputSourceName(ctx->ch_init_param.eInputSource));
        printf("capture.eAudioPacketSize    = %2d %s\n", ctx->ch_init_param.eAudioPacketSize, lookupVegaAudioPacketSizeName(ctx->ch_init_param.eAudioPacketSize));
        printf("capture.eRobustModeEn       = %2d\n", ctx->ch_init_param.eRobustMode);
        printf("capture.bAudioAutoRestartEn = %2d\n", ctx->ch_init_param.bAudioAutoRestartEn);
        printf("capture.eAudioLayouts       = %2d %s\n", ctx->ch_init_param.eAudioLayouts, lookupVegaAudioLayoutName(ctx->ch_init_param.eAudioLayouts));
        printf("---\n");

        printf(MODULE_PREFIX "Configuring Capture Interface\n");

        capret = VEGA3311_CAP_Config(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, &ctx->ch_init_param);
        if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "ERROR: failed to cap config\n");
                return -1;
        }

        printf(MODULE_PREFIX "Configuring ANC Interface\n");

        capret = VEGA3311_CAP_RegisterAncdCallback(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx, vega3311_vanc_callback, opts);
        if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "ERROR: failed to anc register callback\n");
                return -1;
        }

#if VEGA_IS_SOURCING_NDI
        /* Start a NDI interface for capture, instead of the VEGA SDI interface. */
        printf(MODULE_PREFIX "Starting NDI Capture Interface\n");
        vega_ndi_start(opts);
#else
        /* Start the VEGA SDI raw capture interface. */
        printf(MODULE_PREFIX "Starting SDI Capture Interface\n");
        capret = VEGA3311_CAP_Start(opts->brd_idx, (API_VEGA3311_CAP_CHN_E)opts->card_idx,
                API_VEGA3311_CAP_ENABLE_ON, API_VEGA3311_CAP_ENABLE_ON, API_VEGA3311_CAP_ENABLE_ON);
        if (capret != API_VEGA3311_CAP_RET_SUCCESS) {
                fprintf(stderr, MODULE_PREFIX "ERROR: failed to cap start\n");
                return -1;
        }
#endif

        printf(MODULE_PREFIX "The vega encoder device#0 port%d was started\n", opts->card_idx);

	return 0; /* Success */
}

/* Called from open_input() */
static void close_thread(void *handle)
{
	if (!handle)
		return;

	vega_opts_t *opts = (vega_opts_t*)handle;
	close_device(opts);
	free(opts);
}

#define ALLOC_STREAM(nr) \
    streams[cur_stream] = (obe_int_input_stream_t*)calloc(1, sizeof(*streams[cur_stream])); \
    if (!streams[cur_stream]) goto finish;

static void *vega_probe_stream(void *ptr)
{
	obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
	obe_t *h = probe_ctx->h;
	obe_input_t *user_opts = &probe_ctx->user_opts;
	obe_device_t *device;
	obe_int_input_stream_t *streams[MAX_STREAMS];
	int num_streams = 1 + MAX_VEGA_AUDIO_PAIRS;
        int cur_stream = 0;

	printf(MODULE_PREFIX "%s()\n", __func__);

	vega_ctx_t *ctx;

        obe_sdi_non_display_data_t *non_display_parser = NULL;

	vega_opts_t *opts = (vega_opts_t*)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to malloc opts\n");
		goto finish;
	}

        non_display_parser = &opts->ctx.non_display_parser;

	/* TODO: support multi-channel */
	opts->num_audio_channels = MAX_VEGA_AUDIO_CHANNELS;
	opts->card_idx = user_opts->card_idx;
	opts->video_format = user_opts->video_format;
        opts->enable_smpte2038 = user_opts->enable_smpte2038;
        opts->enable_hdr = user_opts->enable_hdr;
        opts->enable_4k_quad = user_opts->enable_4k_quad;
        opts->enable_4k_2si = user_opts->enable_4k_2si;
#if 0
        opts->enable_vanc_cache = user_opts->enable_vanc_cache;
        opts->enable_bitstream_audio = user_opts->enable_bitstream_audio;
        opts->enable_patch1 = user_opts->enable_patch1;
#endif
	opts->probe = 1;

        non_display_parser->probe = 1;

	ctx = &opts->ctx;
	ctx->h = h;

	/* Open device */
	if (open_device(opts, 1) < 0) {
		fprintf(stderr, MODULE_PREFIX "Unable to open device.\n");
		goto finish;
	}

	sleep(1);

	close_device(opts);

	opts->probe_success = 1;
	fprintf(stderr, MODULE_PREFIX "Probe success\n");

	if (!opts->probe_success) {
		fprintf(stderr, MODULE_PREFIX "No valid frames received - check connection and input format\n");
		goto finish;
	}

	/* TODO: probe for SMPTE 337M */
	/* TODO: factor some of the code below out */

	for( int i = 0; i < num_streams; i++ ) {

                ALLOC_STREAM(i);
		if (!streams[i])
			goto finish;

		/* TODO: make it take a continuous set of stream-ids */
		pthread_mutex_lock( &h->device_list_mutex );
		streams[i]->input_stream_id = h->cur_input_stream_id++;
		pthread_mutex_unlock( &h->device_list_mutex );

		if (i == 0) {
			streams[cur_stream]->stream_type   = STREAM_TYPE_VIDEO;
			streams[cur_stream]->stream_format = VIDEO_HEVC_VEGA3311;
			streams[cur_stream]->width         = opts->width;
			streams[cur_stream]->height        = opts->height;
			streams[cur_stream]->timebase_num  = opts->timebase_num;
			streams[cur_stream]->timebase_den  = opts->timebase_den;
			streams[cur_stream]->csp           = AV_PIX_FMT_QSV; /* Special tag. We're providing NALS not raw video. */
			streams[cur_stream]->interlaced    = opts->interlaced;
			streams[cur_stream]->tff           = 1; /* NTSC is bff in baseband but coded as tff */
			streams[cur_stream]->sar_num       = streams[i]->sar_den = 1; /* The user can choose this when encoding */
                        streams[cur_stream]->is_hdr        = OPTION_ENABLED(hdr); /* User is telling if HDR is enabled or not - we're not detecting */
		}
		else if( i >= 1 ) {
			/* TODO: various assumptions about audio being 48KHz need resolved.
         		 * Some sources could be 44.1 and this module will fall down badly.
			 */
			streams[cur_stream]->stream_type    = STREAM_TYPE_AUDIO;
			streams[cur_stream]->stream_format  = AUDIO_PCM;
			streams[cur_stream]->num_channels   = 2;
			streams[cur_stream]->sample_format  = AV_SAMPLE_FMT_S32P;
			streams[cur_stream]->sample_rate    = 48000;
			streams[cur_stream]->sdi_audio_pair = i;
		}
                cur_stream++;
	}

        /* Add a new output stream type, a TABLE_SECTION mechanism.
         * We use this to pass DVB table sections direct to the muxer,
         * for SCTE35, and other sections in the future.
         */
        if (ctx->h->enable_scte35) {
                ALLOC_STREAM(cur_stream);

                pthread_mutex_lock(&h->device_list_mutex);
                streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
                pthread_mutex_unlock(&h->device_list_mutex);

                streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
                streams[cur_stream]->stream_format = DVB_TABLE_SECTION;
                streams[cur_stream]->pid = 0x123; /* TODO: hardcoded PID not currently used. */

                if(add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM) < 0) {
                        goto finish;
                }
                cur_stream++;
        }

        /* Add a new output stream type, a SCTE2038 mechanism.
         * We use this to pass PES direct to the muxer.
         */
        if (OPTION_ENABLED(smpte2038)) {
                ALLOC_STREAM(cur_stream);

                pthread_mutex_lock(&h->device_list_mutex);
                streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
                pthread_mutex_unlock(&h->device_list_mutex);

                streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
                streams[cur_stream]->stream_format = SMPTE2038;
                streams[cur_stream]->pid = 0x124; /* TODO: hardcoded PID not currently used. */

                if(add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM) < 0) {
                        fprintf(stderr, MODULE_PREFIX "Add non display service, failed\n");
                }
                cur_stream++;
        }

	device = new_device();
	if (!device)
		goto finish;

	device->num_input_streams = cur_stream;
	memcpy(device->input_streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**));
	device->device_type = INPUT_DEVICE_VEGA3311;
	memcpy(&device->user_opts, user_opts, sizeof(*user_opts));

	/* add device */
	add_device(h, device);

finish:
	opts->probe = 0;
	if (opts)
		free(opts);

	free(probe_ctx);

	return NULL;
}

static void *vega_open_input(void *ptr)
{
	obe_input_params_t *input = (obe_input_params_t*)ptr;
	obe_t *h = input->h;
	obe_device_t *device = input->device;
	obe_input_t *user_opts = &device->user_opts;
        obe_sdi_non_display_data_t *non_display_parser;
	vega_ctx_t *ctx;

	vega_opts_t *opts = (vega_opts_t *)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to alloc context\n");
		return NULL;
	}

	pthread_cleanup_push(close_thread, (void *)opts);

	opts->num_audio_channels = MAX_VEGA_AUDIO_CHANNELS;
	opts->card_idx           = user_opts->card_idx;
	opts->video_format       = user_opts->video_format;
        opts->enable_smpte2038   = user_opts->enable_smpte2038;
        opts->enable_hdr         = user_opts->enable_hdr;
        opts->enable_4k_quad     = user_opts->enable_4k_quad;
        opts->enable_4k_2si      = user_opts->enable_4k_2si;

        obe_output_stream_t *os = obe_core_get_output_stream_by_index(h, 0);
        if (!os) {
                return NULL;
        }
        switch (os->video_bit_depth) {
        case 8:
              opts->codec.bitDepth     = API_VEGA_BQB_BIT_DEPTH_8;
              break;  
        case 10:
              opts->codec.bitDepth     = API_VEGA_BQB_BIT_DEPTH_10;
              break;  
        }
        printf(MODULE_PREFIX "compression codec will be configured for %d bit\n", os->video_bit_depth);

#if 0
        opts->enable_vanc_cache = user_opts->enable_vanc_cache;
        opts->enable_bitstream_audio = user_opts->enable_bitstream_audio;
        opts->enable_patch1 = user_opts->enable_patch1;
#endif

	ctx         = &opts->ctx;
	ctx->device = device;
	ctx->h      = h;
        vega_sei_init(ctx);
        pthread_mutex_init(&ctx->bDoLastFrame_lock, NULL);

        non_display_parser = &ctx->non_display_parser;
        non_display_parser->device = device;

        printf("ABOUT TO CONFIGURE CODEC\n");
        if (configureCodec(opts) < 0) {
                fprintf(stderr, MODULE_PREFIX "invalid encoder parameters, aborting.\n");
                return NULL;
        }

	if (open_device(opts, 0) < 0)
		return NULL;

	sleep(INT_MAX);

	pthread_cleanup_pop(1);

	return NULL;
}

const obe_input_func_t vega3311_input = { vega_probe_stream, vega_open_input };

#endif /* #if HAVE_VEGA3311_CAP_TYPES_H */
