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
#include "histogram.h"
extern "C"
{
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>
}

#define LOCAL_DEBUG 0

#define MODULE_PREFIX "[vega-audio]: "

/* Take PCM, convrt it into frames and push it into the downstream workflow. */
static void deliver_audio_frame(vega_opts_t *opts, unsigned char *plane, int sizeBytes, int sampleCount, int64_t frametime, int channelCount)
{
	vega_ctx_t *ctx = &opts->ctx;

#if 0
	int offset = 0;
	for (int i = offset; i < (offset + 64); i++)
		printf("%02x", plane[i]);
	printf("\n");
#endif

	//printf("%s() %d bytes numsamples %d\n", __func__, sizeBytes, sampleCount);
	obe_raw_frame_t *rf = new_raw_frame();
	if (!rf)
		return;

	rf->audio_frame.num_samples  = sampleCount;
	rf->audio_frame.num_channels = MAX_VEGA_AUDIO_CHANNELS;
	rf->audio_frame.sample_fmt   = AV_SAMPLE_FMT_S32P; /* Destination format */
	rf->audio_frame.linesize     = rf->audio_frame.num_channels * sizeof(uint16_t);

	if (av_samples_alloc(rf->audio_frame.audio_data, &rf->audio_frame.linesize, rf->audio_frame.num_channels,
                              rf->audio_frame.num_samples, (AVSampleFormat)rf->audio_frame.sample_fmt, 0) < 0)
	{
		fprintf(stderr, MODULE_PREFIX "av_samples_alloc failed\n");
                free(rf);
		return;
	}

        /* Convert input samples from S16 interleaved (native from the h/w) into S32P planer. */
        if (swr_convert(ctx->avr,
                rf->audio_frame.audio_data,
                rf->audio_frame.num_samples,
                (const uint8_t**)&plane,
                rf->audio_frame.num_samples) < 0)
        {
                fprintf(stderr, MODULE_PREFIX "Sample format conversion failed\n");
                /* TODO: Free the audio plane? */
                free(rf);
                return;
        }

	avfm_init(&rf->avfm, AVFM_AUDIO_PCM);

	avfm_set_hw_status_mask(&rf->avfm, AVFM_HW_STATUS__MASK_VEGA); // probably not required.

	/* Hardware clock of the dektec starts up non-zero, increments regardless,
	 * out stack assumes the clock starts at zero. Compensate by subtracting
	 * the base start value.
	 */
	static int64_t frametime_base = 0;
	if (frametime_base == 0)
		frametime_base = frametime;

	avfm_set_pts_audio(&rf->avfm, frametime - frametime_base);
	avfm_set_hw_received_time(&rf->avfm);
	//avfm_set_video_interval_clk(&rf->avfm, decklink_ctx->vframe_duration);

	rf->release_data = obe_release_audio_data;
	rf->release_frame = obe_release_frame;
	rf->input_stream_id = 1; // TODO: Is this correct?

#if LOCAL_DEBUG
        static int64_t last_audio_pts = 0;
        printf(MODULE_PREFIX "Pushing audio downstream PTS %012" PRIi64 ", delta %012" PRIi64 "\n",
                rf->avfm.audio_pts,
                rf->avfm.audio_pts - last_audio_pts);
        last_audio_pts = rf->avfm.audio_pts;
#endif
	if (add_to_filter_queue(ctx->h, rf) < 0 ) {
		fprintf(stderr, MODULE_PREFIX "Unable to add audio frame to the filter q.\n");
	}
}

/* Called by the vega sdk when a raw audio frame is ready. */
void vega3311_audio_callback(uint32_t u32DevId,
        API_VEGA3311_CAP_CHN_E eCh,
        API_VEGA3311_CAPTURE_FRAME_INFO_T *st_frame_info,
        API_VEGA3311_CAPTURE_FORMAT_T *st_input_info,
        void* pv_user_arg)
{
        vega_opts_t *opts = (vega_opts_t *)pv_user_arg;
	vega_ctx_t *ctx = &opts->ctx;

        if (ctx->bDoLastFrame) {
                /* Encoder wants to shut down */
                return;
        }
        
        if (st_frame_info->u32BufSize == 0) {
                if (st_input_info->eAudioState == API_VEGA3311_CAP_STATE_CAPTURING) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] audio state change to capturing, source signal recovery\n", u32DevId, eCh);
                } else if (st_input_info->eAudioState == API_VEGA3311_CAP_STATE_WAITING) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] audio state change to waiting, source signal loss\n", u32DevId, eCh);
                } else if (st_input_info->eAudioState == API_VEGA3311_CAP_STATE_STANDBY) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] audio state change to standby, callback function is terminated\n", u32DevId, eCh);
                }

                return;
        }

        if (vega_has_source_signal_changed(&ctx->detectedFormat, st_input_info)) {
                /* We need to terminate the encoder, it will forced a restart and a new format */
                static time_t lastMsg = 0;
                time_t now = time(NULL);

                if (lastMsg != now) {
                        lastMsg = now;
                        char ts[32];
                        obe_getTimestamp(&ts[0], NULL);
                        fprintf(stderr, MODULE_PREFIX "%s: input signal format changed. Dropping audio, waiting for the controller to restart process\n", ts);
                }
                return;
        }

        if (g_decklink_record_audio_buffers) {
            g_decklink_record_audio_buffers--;
            static int aidx = 0;
            char fn[256];
            int channels = MAX_VEGA_AUDIO_CHANNELS;
            int samplesPerChannel = st_frame_info->u32BufSize / channels / sizeof(uint16_t);
#if AUDIO_DEBUG_ENABLE
            sprintf(fn, "/storage/ltn/stoth/cardindex%d-audio%03d-srf%d.raw", opts->card_idx, aidx++, samplesPerChannel);
#else
            sprintf(fn, "/tmp/cardindex%d-audio%03d-srf%d.raw", opts->card_idx, aidx++, samplesPerChannel);
#endif
            FILE *fh = fopen(fn, "wb");
            if (fh) {
                fwrite(st_frame_info->u8pDataBuf, 1, st_frame_info->u32BufSize, fh);
                fclose(fh);
                printf("Creating %s\n", fn);
            }
        }

	ltn_histogram_interval_update(ctx->hg_callback_audio);

	if (g_decklink_histogram_print_secs > 0) {
		ltn_histogram_interval_print(STDOUT_FILENO, ctx->hg_callback_audio, g_decklink_histogram_print_secs);
        }

	if (g_decklink_histogram_reset) {
		g_decklink_histogram_reset = 0;
		ltn_histogram_reset(ctx->hg_callback_audio);
		ltn_histogram_reset(ctx->hg_callback_video);
	}

        /* Vega deliveres the samples in s16P planar format already. We don't need to
         * convert from INterleaved to planar. Yay.
         */

        //int64_t pcr = convertSCR_to_PCR(st_input_info->tCurrentPCR.u64Dword);
        int64_t pcr = st_input_info->tCurrentPCR.u64Dword;
//        printf("%" PRIi64 "\n", st_input_info->tCurrentPCR.u64Dword);

#if LOCAL_DEBUG
        int64_t pts = pcr / 300LL;
        printf(MODULE_PREFIX "pushed raw audio frame to encoder, PCR %016" PRIi64 ", PTS %012" PRIi64 " %p length %d delta: %016" PRIi64 "\n",
                pcr, pts,
                st_frame_info->u8pDataBuf, st_frame_info->u32BufSize,
                pcr - ctx->audioLastPCR);
#endif
        int channels = MAX_VEGA_AUDIO_CHANNELS;
        int samplesPerChannel = st_frame_info->u32BufSize / channels / sizeof(uint16_t);

        deliver_audio_frame(opts, st_frame_info->u8pDataBuf, st_frame_info->u32BufSize, samplesPerChannel, pcr, channels);

        ctx->audioLastPCR = pcr;
}

#endif /* #if HAVE_VEGA3311_CAP_TYPES_H */
