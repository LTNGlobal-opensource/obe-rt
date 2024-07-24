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
#include <input/sdi/yuv422p10le.h>
#include <sys/stat.h>
#include "vega-3311.h"
#include "histogram.h"
extern "C"
{
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>
}

#define LOCAL_DEBUG 0

#define MODULE_PREFIX "[vega-video]: "

/* Take a single NAL, convert to a raw frame and push it into the downstream workflow.
 * TODO: Coalesce these at a higher level and same some memory allocs?
 * PTS is a 64bit signed value, than doesn't wrap at 33 bites, it constantly accumlates.
 */
static void deliver_video_nal(vega_opts_t *opts, unsigned char *buf, int lengthBytes, int64_t pts, int64_t dts)
{
	vega_ctx_t *ctx = &opts->ctx;
#if LOCAL_DEBUG
        printf(MODULE_PREFIX "%s() delivering nal\n", __func__);
#endif

        obe_raw_frame_t *rf = new_raw_frame();
        if (!rf) {
                fprintf(stderr, MODULE_PREFIX "Could not allocate raw video frame\n");
                return;
        }

        rf->alloc_img.planes = 1;
        rf->alloc_img.width = lengthBytes;
        rf->alloc_img.height = 1;
        rf->alloc_img.format = opts->video_format;
        rf->alloc_img.csp = AV_PIX_FMT_QSV; /* Magic type to indicate to the downstream filters that this isn't raw video */
        rf->timebase_num = opts->timebase_num;
        rf->timebase_den = opts->timebase_den;
        rf->pts = pts * 300LL; /* Converts 90KHz to 27MHz */

        //printf("%s() format = %d, timebase_num %d, timebase_den %d\n", __func__, rf->alloc_img.format, rf->timebase_num, rf->timebase_den);  // TODO: FIxme IE INPUT_VIDEO_FORMAT_2160P_25

        rf->alloc_img.stride[0] = lengthBytes;
        rf->alloc_img.plane[0] = (uint8_t *)malloc(((lengthBytes / 4096) + 1) * 4096);

        if (rf->alloc_img.plane[0] == NULL) {
                fprintf(stderr, MODULE_PREFIX "Could not allocate nal video plane\n");
                free(rf);
                return;
        }
        memcpy(rf->alloc_img.plane[0], buf, lengthBytes);

        /* AVFM */
        avfm_init(&rf->avfm, AVFM_VIDEO);
        avfm_set_hw_status_mask(&rf->avfm, AVFM_HW_STATUS__MASK_VEGA); // TODO: We probably don't need this

        /* Remember that we drive everything in the pipeline from the audio clock. */
        avfm_set_pts_video(&rf->avfm, rf->pts); /* TODO: This should be the hardware time */
        avfm_set_dts_video(&rf->avfm, rf->pts); /* TODO: This should be the hardware time */
        avfm_set_pts_audio(&rf->avfm, rf->pts); /* TODO: This should be the hardware time */

        avfm_set_hw_received_time(&rf->avfm);

        /* Safety div by zero */
        double dur = 0;
        if (rf->timebase_num && rf->timebase_den) {
                dur = 27000000.0 / ((double)rf->timebase_den / (double)rf->timebase_num);
        }

        avfm_set_video_interval_clk(&rf->avfm, dur);
        //avfm_dump(&rf->avfm);

        if (add_to_filter_queue(ctx->h, rf) < 0 ) {
                fprintf(stderr, MODULE_PREFIX "Could not allocate raw video frame\n");
                free(rf->alloc_img.plane[0]);
                free(rf);
                return;
        }
}

/* Called by the vega sdk when a newly compressed video frame is ready. */
void vega3311_video_avc_compressed_callback(API_VEGA_BQB_AVC_CODED_PICT_T *p_pict, void *args)
{
        /* It's probably safe to colapse both HEVC and AVC callback functions into a single thing
         * and cast the p_pict into the right type based on codec from args context.
         * Until we know for sure the timing is exactly the same, I want to avoid significant unification.
         */
        vega_opts_t *opts = (vega_opts_t *)args;
	vega_ctx_t *ctx = &opts->ctx;

#if LOCAL_DEBUG
        printf(MODULE_PREFIX "%s() recd AVC es video frames\n", __func__);
#endif

        if (ctx->bLastFrame) {
                /* Encoder wants to shut down */
                return;
        }
#if 0
        FILE *fh = fopen("/storage/ltn/vega/avc-pic-data.nals", "a+");
        if (fh) {
                fwrite(p_pict, 1, 16 * sizeof(int64_t), fh);
                fclose(fh);
                printf(MODULE_PREFIX "Wrote %d bytes\n", sizeof(*p_pict));
        }
#endif

        /* When the value wraps, make sure we disgregard iffy bits 63-33.
         * The result is a pure 33bit guaranteed positive number.
         */
        int64_t adjustedPicPTS = p_pict->pts & 0x1ffffffffLL;
        int64_t adjustedPicDTS = p_pict->dts & 0x1ffffffffLL;

        if (adjustedPicDTS < ctx->lastAdjustedPicDTS) {
                /* Detect the wrap, adjust the constant accumulator */
                ctx->videoDTSOffset += 0x1ffffffffLL;
                printf(MODULE_PREFIX "Bumping DTS base\n");
        }
        if (adjustedPicPTS < ctx->lastAdjustedPicPTS) {
                /* Detect the wrap, adjust the constant accumulator */
                ctx->videoPTSOffset += 0x1ffffffffLL;
                printf(MODULE_PREFIX "Bumping PTS base\n");
        }

        /* Compute a new PTS/DTS based on a constant accumulator and guaranteed positive PTS/DTS value */
        int64_t correctedPTS = ctx->videoDTSOffset + adjustedPicPTS;
        int64_t correctedDTS = ctx->videoPTSOffset + adjustedPicDTS;

        /* The upshot is that the correctPTS and correctDTS should always increase over time regardless of
         * wrapping and can be used to pass downstream (same as the decklink).
         */
        for (unsigned int i = 0; i < p_pict->u32NalNum; i++) {

                if (g_decklink_monitor_hw_clocks) {
                        printf(MODULE_PREFIX "corrected  pts: %15" PRIi64 "  dts: %15" PRIi64
                                " adjustedPicPTS %15" PRIi64 " adjustedPicDTS %15" PRIi64
                                " ctx->videoPTSOffset %15" PRIi64 " ctx->videoDTSOffset %15" PRIi64 "\n",
                                correctedPTS, correctedDTS,
                                adjustedPicPTS, adjustedPicDTS,
                                ctx->videoPTSOffset, ctx->videoDTSOffset
                        );
                        printf(MODULE_PREFIX "pic        pts: %15" PRIi64 "  dts: %15" PRIi64 " base %012" PRIu64 " ext %012d frametype: %s  addr: %p length %7d -- ",
                                p_pict->pts,
                                p_pict->dts,
                                p_pict->u64ItcBase,
                                p_pict->u32ItcExt,
                                vega_lookupFrameType(p_pict->eFrameType),
                                p_pict->tNalInfo[i].pu8Addr, p_pict->tNalInfo[i].u32Length);
                        int dlen = p_pict->tNalInfo[i].u32Length;
                        if (dlen > 32)
                                dlen = 32;

                        for (int j = 0; j < dlen; j++)
                                printf("%02x ", p_pict->tNalInfo[i].pu8Addr[j]);
                        printf("\n");
                }

                if (g_sei_timestamping) {
                        /* Find the latency SEI and update it. */
                        if (p_pict->tNalInfo[i].u32Length == (7 + SEI_TIMESTAMP_PAYLOAD_LENGTH)) {
                                int offset = ltn_uuid_find(p_pict->tNalInfo[i].pu8Addr, p_pict->tNalInfo[i].u32Length);
                                if (offset >= 0) {
                                        struct timeval tv;
                                        gettimeofday(&tv, NULL);

                                        /* Add the time exit from compressor seconds/useconds. */
                                        sei_timestamp_field_set(&p_pict->tNalInfo[i].pu8Addr[offset], p_pict->tNalInfo[i].u32Length - offset, 6, tv.tv_sec);
                                        sei_timestamp_field_set(&p_pict->tNalInfo[i].pu8Addr[offset], p_pict->tNalInfo[i].u32Length - offset, 7, tv.tv_usec);
                                }
                        }
                }
                /* TODO: Decision here. Coalesce all nals into a single allocation - better performance, one raw frame downstream. */

                deliver_video_nal(opts, p_pict->tNalInfo[i].pu8Addr, p_pict->tNalInfo[i].u32Length, correctedPTS, correctedDTS);
        }

        ctx->videoLastDTS = p_pict->dts;
        ctx->videoLastPTS = p_pict->pts;
        ctx->lastAdjustedPicPTS = adjustedPicPTS;
        ctx->lastAdjustedPicDTS = adjustedPicDTS;
        ctx->lastcorrectedPicPTS = correctedPTS;
        ctx->lastcorrectedPicDTS = correctedDTS;
}

/* Called by the vega sdk when a newly compressed video frame is ready. */
void vega3311_video_hevc_compressed_callback(API_VEGA_BQB_HEVC_CODED_PICT_T *p_pict, void *args)
{
        vega_opts_t *opts = (vega_opts_t *)args;
	vega_ctx_t *ctx = &opts->ctx;

#if LOCAL_DEBUG
        printf(MODULE_PREFIX "%s() recd HEVC es video frames\n", __func__);
#endif

        if (ctx->bLastFrame) {
                /* Encoder wants to shut down */
                return;
        }
#if 0
        FILE *fh = fopen("/storage/ltn/vega/hevc-pic-data.bin", "a+");
        if (fh) {
                fwrite(p_pict, 1, 16 * sizeof(int64_t), fh);
                fclose(fh);
                printf(MODULE_PREFIX "Wrote %d bytes\n", sizeof(*p_pict));
        }
#endif

        /* When the value wraps, make sure we disgregard iffy bits 63-33.
         * The result is a pure 33bit guaranteed positive number.
         */
        int64_t adjustedPicPTS = p_pict->pts & 0x1ffffffffLL;
        int64_t adjustedPicDTS = p_pict->dts & 0x1ffffffffLL;

        if (adjustedPicDTS < ctx->lastAdjustedPicDTS) {
                /* Detect the wrap, adjust the constant accumulator */
                ctx->videoDTSOffset += 0x1ffffffffLL;
                printf(MODULE_PREFIX "Bumping DTS base\n");
        }
        if (adjustedPicPTS < ctx->lastAdjustedPicPTS) {
                /* Detect the wrap, adjust the constant accumulator */
                ctx->videoPTSOffset += 0x1ffffffffLL;
                printf(MODULE_PREFIX "Bumping PTS base\n");
        }

        /* Compute a new PTS/DTS based on a constant accumulator and guaranteed positive PTS/DTS value */
        int64_t correctedPTS = ctx->videoDTSOffset + adjustedPicPTS;
        int64_t correctedDTS = ctx->videoPTSOffset + adjustedPicDTS;

        /* The upshot is that the correctPTS and correctDTS should always increase over time regardless of
         * wrapping and can be used to pass downstream (same as the decklink).
         */

        for (unsigned int i = 0; i < p_pict->u32NalNum; i++) {

                if (g_decklink_monitor_hw_clocks) {
                        printf(MODULE_PREFIX "corrected  pts: %15" PRIi64 "  dts: %15" PRIi64
                                " adjustedPicPTS %15" PRIi64 " adjustedPicDTS %15" PRIi64
                                " ctx->videoPTSOffset %15" PRIi64 " ctx->videoDTSOffset %15" PRIi64 "\n",
                                correctedPTS, correctedDTS,
                                adjustedPicPTS, adjustedPicDTS,
                                ctx->videoPTSOffset, ctx->videoDTSOffset
                        );
                        printf(MODULE_PREFIX "pic        pts: %15" PRIi64 "  dts: %15" PRIi64 " base %012" PRIu64 " ext %012d frametype: %s  addr: %p length %7d -- ",
                                p_pict->pts,
                                p_pict->dts,
                                p_pict->u64ItcBase,
                                p_pict->u32ItcExt,
                                vega_lookupFrameType(p_pict->eFrameType),
                                p_pict->tNalInfo[i].pu8Addr, p_pict->tNalInfo[i].u32Length);
                        int dlen = p_pict->tNalInfo[i].u32Length;
                        if (dlen > 32)
                                dlen = 32;

                        for (int j = 0; j < dlen; j++)
                                printf("%02x ", p_pict->tNalInfo[i].pu8Addr[j]);
                        printf("\n");
                }

                if (g_sei_timestamping) {
                        /* Find the latency SEI and update it. */
                        if (p_pict->tNalInfo[i].u32Length == (7 + SEI_TIMESTAMP_PAYLOAD_LENGTH)) {
                                int offset = ltn_uuid_find(p_pict->tNalInfo[i].pu8Addr, p_pict->tNalInfo[i].u32Length);
                                if (offset >= 0) {
                                        struct timeval tv;
                                        gettimeofday(&tv, NULL);

                                        /* Add the time exit from compressor seconds/useconds. */
                                        sei_timestamp_field_set(&p_pict->tNalInfo[i].pu8Addr[offset], p_pict->tNalInfo[i].u32Length - offset, 6, tv.tv_sec);
                                        sei_timestamp_field_set(&p_pict->tNalInfo[i].pu8Addr[offset], p_pict->tNalInfo[i].u32Length - offset, 7, tv.tv_usec);
                                }
                        }
                }
                /* TODO: Decision here. Coalesce all nals into a single allocation - better performance, one raw frame downstream. */

                deliver_video_nal(opts, p_pict->tNalInfo[i].pu8Addr, p_pict->tNalInfo[i].u32Length, correctedPTS, correctedDTS);
        }

        ctx->videoLastDTS = p_pict->dts;
        ctx->videoLastPTS = p_pict->pts;
        ctx->lastAdjustedPicPTS = adjustedPicPTS;
        ctx->lastAdjustedPicDTS = adjustedPicDTS;
        ctx->lastcorrectedPicPTS = correctedPTS;
        ctx->lastcorrectedPicDTS = correctedDTS;

}

/* Callback from the video capture device, we're handed a raw frame */
void vega3311_video_capture_callback(uint32_t u32DevId,
        API_VEGA3311_CAP_CHN_E eCh,
        API_VEGA3311_CAPTURE_FRAME_INFO_T *st_frame_info,
        API_VEGA3311_CAPTURE_FORMAT_T *st_input_info,
        void *pv_user_arg)
{
        vega_opts_t *opts = (vega_opts_t *)pv_user_arg;
	vega_ctx_t *ctx = &opts->ctx;

#if LOCAL_DEBUG
        printf(MODULE_PREFIX "%s() recd raw video frame\n", __func__);
#endif

        if (ctx->bLastFrame) {
                /* Encoder wants to shut down */
                return;
        }

        API_VEGA_BQB_IMG_T img;
        int64_t pcr = st_input_info->tCurrentPCR.u64Dword;

        if (st_frame_info->u32BufSize == 0) {
                if (st_input_info->eVideoState == API_VEGA3311_CAP_STATE_CAPTURING) {
                        //printf(MODULE_PREFIX "[DEV%d:CH%d] video state change to capturing, source signal recovery\n", u32DevId, eCh);
                        return;
                }
                else if (st_input_info->eVideoState == API_VEGA3311_CAP_STATE_WAITING) {
                        //printf(MODULE_PREFIX "[DEV%d:CH%d] video state change to waiting, source signal loss\n", u32DevId, eCh);
                        return;
                }
                else if (st_input_info->eVideoState == API_VEGA3311_CAP_STATE_STANDBY) {
                        //printf(MODULE_PREFIX "[DEV%d:CH%d] video state change to standby, callback function is terminated\n", u32DevId, eCh);
                }
        }

        if (vega_has_source_signal_changed(&ctx->detectedFormat, st_input_info)) {
                /* We need to terminate the encoder, it will forced a restart and a new format */
                static time_t lastMsg = 0;
                time_t now = time(NULL);

                if (lastMsg != now) {
                        lastMsg = now;
                        char ts[32];
                        obe_getTimestamp(&ts[0], NULL);
                        fprintf(stderr, MODULE_PREFIX "%s: input signal format changed. Dropping video, waiting for the controller to restart process\n", ts);
                }
                return;
        }

	ltn_histogram_interval_update(ctx->hg_callback_video);
	if (g_decklink_histogram_print_secs > 0) {
		ltn_histogram_interval_print(STDOUT_FILENO, ctx->hg_callback_video, g_decklink_histogram_print_secs);
        }

        //if (ops->cb_param_p->last_frame == 1)
        {
         //       sem_post(&cb_param_p->capture_sem);
        //        printf("vega processed last frame\n");
        }

        ctx->framecount++;

#if 0
	static time_t last_report = 0;
	time_t now;
	now = time(NULL);
	if (now != last_report) {
		printf(MODULE_PREFIX "[DEV%d:CH%d] Video drops: %" PRIu64 " Audio drops: %" PRIu64 "\n",
		       u32DevId, eCh, st_input_info->u64VideoFrameDropped, st_input_info->u64AudioFrameDropped);
		last_report = now;
	}
#endif

        /* Capture an output frame to disk, for debug */
        {
                struct stat s;
                const char *tf = "/tmp/vega-frame-capture-input.touch";
                int r = stat(tf, &s);
                if (r == 0) {
                        char fn[64];
                        sprintf(fn, "/tmp/%08" PRIu64 "-vega-capture-input-port%d.bin", ctx->framecount, opts->card_idx);
                        FILE *fh = fopen(fn, "wb");
                        if (fh) {
                                fwrite(st_frame_info->u8pDataBuf, 1, st_frame_info->u32BufSize, fh);
                                fclose(fh);
                        }
                        remove(tf);
                }
        }

        //int64_t pts = convertSCR_to_PCR(st_input_info->u64CurrentPCR) / 300;
        memset(&img, 0 , sizeof(img));
        uint8_t *dst[3] = { 0, 0, 0 };

#if 0
	/* Burnreader to validate frames on capture */
	if (ctx->ch_init_param.eFormat == API_VEGA3311_CAP_IMAGE_FORMAT_Y210) {
		/* These are the default constants in the burnwriter */
		int startline = 1;
		int bitwidth = 30;
		int bitheight = 30;

		/* Check the line at the vertical center of the boxes */
		int checkline = startline + (bitheight / 2);
		uint16_t *pic = (uint16_t *)&st_frame_info->u8pDataBuf[0];
		uint16_t *x;
		uint32_t bits = 0;
		int bitcount = 0;
		static int64_t framecnt = 0;

		/* Check to ensure counters are actually present */
		x = pic + (checkline * opts->width) + 1;
		for (int c = 30; c >= 0; c--) {
			x += (bitwidth / 2 * 2);
			if ((*x >> 6) > 0x195 && (*x >> 6) < 0x205)
				bitcount++;
			x += (bitwidth / 2 * 2);
		}

		/* Now extract the count */
		x = pic + (checkline * opts->width);
		for (int c = 31; c >= 0; c--) {
                        x += (bitwidth / 2 * 2);
                        if ((*x >> 6) > 0x200)
				bits |= (1 << c);
                        x += (bitwidth / 2 * 2);
		}

		if ((bitcount == 31) && (framecnt && framecnt + 1 != bits)) {
                        char ts[64];
                        obe_getTimestamp(ts, NULL);
                        printf("%s: OSD counter discontinuity, expected %08" PRIx64 " got %08" PRIx32 "\n",
                               ts, framecnt + 1, bits);
		}
		framecnt = bits;
	}
#endif

        if (opts->codec.eFormat == API_VEGA_BQB_IMAGE_FORMAT_YUV422P10LE) {
                /* If 10bit 422 .... Colorspace convert Y210 into yuv422p10le */
#if LOCAL_DEBUG
        printf(MODULE_PREFIX "%s() dora raw video frame\n", __func__);
        return;
#endif

                /*  len as expressed in bytes. No stride */
                int ylen = (opts->width * opts->height) * 2;
                int ulen = ylen / 2;
                int vlen = ylen / 2;
                int dstlen = ylen + ulen + vlen;

                dst[0] = (uint8_t *)malloc(dstlen); /* Y - Primary plane - we submit this single address the the GPU */
                dst[1] = dst[0] + ylen; /* U plane */
                dst[2] = dst[1] + ulen; /* V plane */

#if 0
                printf("dstlen %d   ylen %d  ulen %d vlen %d\n", dstlen, ylen, ulen, vlen);
                for (int i = 0; i < 3; i++)
                        printf("dst[%d] %p\n", i, dst[i]);
#endif

                /* Unpack all of the 16 bit Y/U/Y/V words, shift to correct for padding and write new planes */
                uint16_t *wy = (uint16_t *)dst[0];
                uint16_t *wu = (uint16_t *)dst[1];
                uint16_t *wv = (uint16_t *)dst[2];
                uint16_t *p = (uint16_t *)&st_frame_info->u8pDataBuf[0];

                /* For all 4xUnsignedShort sized blocks in the source frame....*/
                for (int i = 0 ; i < opts->width * opts->height * 4; i += 8) {
                        *(wy++) = *(p++) >> 6;
                        *(wu++) = *(p++) >> 6;
                        *(wy++) = *(p++) >> 6;
                        *(wv++) = *(p++) >> 6;
                }

                img.pu8Addr     = dst[0];
                img.u32Size     = dstlen;
                img.eFormat     = opts->codec.eFormat;

                if (g_decklink_render_walltime) {
                        struct YUV422P10LE_painter_s pctx;
                        YUV422P10LE_painter_reset(&pctx, dst[0], opts->width, opts->height, opts->width);

                        char ts[64];
                        obe_getTimestamp(ts, NULL);
                        YUV422P10LE_painter_draw_ascii_at(&pctx, 2, 2, ts);
                }
        } else if ((opts->codec.eFormat == API_VEGA_BQB_IMAGE_FORMAT_YUV422P010) ||
                (opts->codec.eFormat == API_VEGA_BQB_IMAGE_FORMAT_YUV420P010))
        {
#if LOCAL_DEBUG
        printf(MODULE_PREFIX "%s() dorb raw video frame, known working path\n", __func__);
#endif
/*
  8x 16bit words:
   y0 bits 15-6
  cb0 bits 15-6
   y1 bits 15-6
  cb1 bits 15-6
   y2 bits 15-6
  cb2 bits 15-6
   y3 bits 15-6
  cb3 bits 15-6


  YYYY YYYY YY          CCCC CCCC CC          YYYY YYYY YY          CCCC CCCC CC          YYYY YYYY YY          CCCC CCCC CC          YYYY YYYY YY          CCCC CCCC CC       
  0000|0000 0000|0000   0000|0000 0000|0000   0000|0000 0000|0000   0000|0000 0000|0000   0000|0000 0000|0000   0000|0000 0000|0000   0000|0000 0000|0000   0000|0000 0000|0000

  YYYY|YYYY YYYY|YYYY   YYYY|YYYY YYYY|YYYY   YYYY|YYYY

*/

                /* If 10bit 422 .... Colorspace convert Y210 into NV20 (mislabeled by Advantech as P210).
                 * If we're encoding in 422_to_420 mode, we also come through this path.
                 */

                /*  len as expressed in bytes. No stride */
                int ylen = (opts->width * opts->height) * 5 / 4;
                int uvlen = ylen;
                int dstlen = ylen + uvlen;

                dst[0] = (uint8_t *)malloc(dstlen); /* Y - Primary plane - we submit this single address the the GPU */
                dst[1] = dst[0] + ylen; /* UV plane */

                /* Unpack all of the 16 bit Y/U/Y/V words, shift to correct for padding and write new planes */
                uint8_t *wy = dst[0];
		uint8_t *wuv = dst[1];
                uint16_t *p = (uint16_t *)&st_frame_info->u8pDataBuf[0];

                for (int i = 0 ; i < opts->width * opts->height; i += 4) {
			uint16_t y0, y1, y2, y3;
			uint16_t uv0, uv1, uv2, uv3;
			y0 = *(p++) >> 6;
			uv0 = *(p++) >> 6;
			y1 = *(p++) >> 6;
			uv1 = *(p++) >> 6;
			y2 = *(p++) >> 6;
			uv2 = *(p++) >> 6;
			y3 = *(p++) >> 6;
			uv3 = *(p++) >> 6;

			*(wy++) = y0;
                        *(wy++) = (y0 >> 8) | (y1 << 2);
                        *(wy++) = (y1 >> 6) | (y2 << 4);
                        *(wy++) = (y2 >> 4) | (y3 << 6);
                        *(wy++) = (y3 >> 2);

			*(wuv++) = uv0;
                        *(wuv++) = (uv0 >> 8) | (uv1 << 2);
                        *(wuv++) = (uv1 >> 6) | (uv2 << 4);
                        *(wuv++) = (uv2 >> 4) | (uv3 << 6);
                        *(wuv++) = (uv3 >> 2);
                }

                img.pu8Addr     = dst[0];
                img.u32Size     = dstlen;
                img.eFormat     = opts->codec.eFormat;
        } else if (opts->codec.eFormat == API_VEGA_BQB_IMAGE_FORMAT_NV12) {
#if LOCAL_DEBUG
        printf(MODULE_PREFIX "%s() dord raw video frame, NV12 8bit 4:2:0\n", __func__);
#endif
                img.pu8Addr     = st_frame_info->u8pDataBuf;
                img.u32Size     = st_frame_info->u32BufSize;
                //img.eFormat     = API_VEGA_BQB_IMAGE_FORMAT_NV12; /* 4:2:0 8bit */
        } else {

//#if LOCAL_DEBUG
        printf(MODULE_PREFIX "%s() dorc raw video frame, unsupported, opts->codec.eFormat = 0x%x\n", __func__, opts->codec.eFormat);
//        return;
//#endif
                img.pu8Addr     = st_frame_info->u8pDataBuf;
                img.u32Size     = st_frame_info->u32BufSize;
                img.eFormat     = API_VEGA_BQB_IMAGE_FORMAT_YUV420P010; /* 4:2:0 10bit */
        }

        img.eFormat     = opts->codec.eFormat;
        img.pts         = pcr / 300LL;
        img.eTimeBase   = API_VEGA_BQB_TIMEBASE_90KHZ;
        img.bLastFrame  = ctx->bLastFrame;
        img.u32SeiNum   = 0;

        if (g_sei_timestamping) {
                vega_sei_append_ltn_timing(ctx);
        }

        /* Append any queued SEI (captions typically), from sei index 1 onwards */
        {
                vega_sei_lock(ctx);
                if (ctx->seiCount) {
                        for (int i = 0; i < ctx->seiCount; i++) {
                                memcpy(&img.tSeiParam[i], &ctx->sei[i], sizeof(API_VEGA_BQB_SEI_PARAM_T));

                        }
                        img.u32SeiNum += ctx->seiCount;
                        ctx->seiCount = 0;
                }
                vega_sei_unlock(ctx);
        }

        /* Capture an output frame to disk, for debug, when you touch this file. */
        {
                struct stat s;
                const char *tf = "/tmp/vega-frame-capture-output.touch";
                int r = stat(tf, &s);
                if (r == 0) {
                        char fn[64];
                        sprintf(fn, "/tmp/%08" PRIu64 "-vega-capture-output-port%d.bin", ctx->framecount, opts->card_idx);
                        FILE *fh = fopen(fn, "wb");
                        if (fh) {
                                fwrite(img.pu8Addr, 1, img.u32Size, fh);
                                fclose(fh);
                        }
                        remove(tf);
                }
        }

        /* Submit the picture to the codec */
        if (VEGA_BQB_ENC_PushImage((API_VEGA_BQB_DEVICE_E)u32DevId, (API_VEGA_BQB_CHN_E)eCh, &img)) {
                fprintf(stderr, MODULE_PREFIX "Error: PushImage failed\n");
        }

        if (dst[0])
                free(dst[0]); /* Free the CSCd frame */

#if LOCAL_DEBUG
        printf(MODULE_PREFIX "pushed raw video frame to encoder, PCR %016" PRIi64 ", PTS %012" PRIi64 ", sei msgs = %d\n",
                pcr, img.pts, img.u32SeiNum);
#endif

#if 0
        printf("Video last pcr minus current pcr %12" PRIi64 " (are we dropping frames?)\n",
                pcr - ctx->videoLastPCR
        );
#endif
        ctx->videoLastPCR = pcr;

        /* Check the audio PCR vs the video PCR is never more than 200 ms apart */
        int64_t avdeltams = (ctx->videoLastPCR - ctx->audioLastPCR) / 300LL / 90LL;
        int64_t avdelta_warning = 150;
        if (abs(avdeltams) >= avdelta_warning) {
                printf(MODULE_PREFIX "warning: video PCR vs audio PCR delta %12" PRIi64 " (ms), exceeds %" PRIi64 ".\n",
                        (ctx->videoLastPCR - ctx->audioLastPCR) / 300 / 90,
                        avdelta_warning);
        }

        if (g_decklink_inject_scte104_preroll6000 > 0) {
            g_decklink_inject_scte104_preroll6000 = 0;
            printf("Injecting a scte104 preroll 6000 message (VEGA NOT SUPPORTED)\n");
#if 0
            /* Insert a static SCTE104 splice messages to occur 6 seconds from now. */
            /* Pass this into the framework just like any other message we'd find
             * on line 10, for DID 41 and SDID 07.
             */
            unsigned char msg[] = {
                0x00, 0x00, 0xff, 0x03, 0xff, 0x03, 0x41, 0x02, 0x07, 0x01, 0x1f, 0x01, 0x08, 0x01, 0xff, 0x02,
                0xff, 0x02, 0x00, 0x02, 0x1e, 0x02, 0x00, 0x02, 0x01, 0x01, 0x3b, 0x01, 0x00, 0x02, 0x01, 0x01,
                0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x02, 0x0e, 0x01, 0x01, 0x01,
                0x00, 0x02, 0x00, 0x02, 0xb1, 0x02, 0xdd, 0x02, 0x00, 0x02, 0x00, 0x02, 0x17, 0x02, 0x70, 0x01,
                0x0b, 0x01, 0xb8, 0x02, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0xb3, 0x01, 0x40, 0x00, 0x40, 0x00,
                0x40, 0x00
            };
            endian_flip_array(msg, sizeof(msg));
            ret = _vancparse(decklink_ctx->vanchdl, &msg[0], sizeof(msg), 10);
            if (ret < 0) {
                fprintf(stderr, "%s() Unable to parse scte104 preroll 6000 packet\n", __func__);
            }

            /* Paint a message in the V210, so we know per frame when an event was received. */
            videoframe->GetBytes(&frame_bytes);
            struct V210_painter_s painter;
            V210_painter_reset(&painter, (unsigned char *)frame_bytes, width, height, stride, 0);
            V210_painter_draw_ascii_at(&painter, 0, 3, "SCTE104 - 6000ms Preroll injected");
#endif
        }

        if (g_decklink_inject_scte104_fragmented > 0) {
            g_decklink_inject_scte104_fragmented = 0;
            printf("Injecting a scte104 fragmented frame (VEGA NOT SUPPORTED)\n");
#if 0
            /* Very large multi-fragment SCTE104 message. */
            unsigned char msg[] = {
                0x00, 0x00, 0xff, 0x03, 0xff, 0x03, 0x41, 0x02, 0x07, 0x01, 0xff, 0x02, 0x0c, 0x02, 0xff, 0x02,
                0xff, 0x02, 0x01, 0x01, 0x0e, 0x01, 0x00, 0x02, 0x00, 0x02, 0x09, 0x02, 0x00, 0x02, 0x00, 0x02,
                0x00, 0x02, 0x00, 0x02, 0x05, 0x02, 0x01, 0x01, 0x04, 0x01, 0x00, 0x02, 0x02, 0x01, 0x0f, 0x02,
                0xa0, 0x02, 0x01, 0x01, 0x0b, 0x01, 0x00, 0x02, 0x23, 0x01, 0x07, 0x01, 0xb8, 0x02, 0x42, 0x02,
                0xb9, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x11, 0x02, 0x55, 0x02, 0x4b, 0x02,
                0x52, 0x01, 0x41, 0x02, 0x49, 0x01, 0x4e, 0x02, 0x45, 0x01, 0x48, 0x02, 0x45, 0x01, 0x4c, 0x01,
                0x50, 0x02, 0x30, 0x02, 0x34, 0x01, 0x57, 0x01, 0x50, 0x02, 0x56, 0x02, 0x49, 0x01, 0x35, 0x02,
                0x01, 0x01, 0x01, 0x01, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02,
                0x01, 0x01, 0x0b, 0x01, 0x00, 0x02, 0x43, 0x01, 0x07, 0x01, 0x82, 0x02, 0xab, 0x01, 0x69, 0x02,
                0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x31, 0x01, 0x50, 0x02, 0x43, 0x01, 0x52, 0x01,
                0x31, 0x01, 0x5f, 0x02, 0x30, 0x02, 0x33, 0x02, 0x30, 0x02, 0x35, 0x02, 0x32, 0x01, 0x32, 0x01,
                0x30, 0x02, 0x35, 0x02, 0x35, 0x02, 0x38, 0x01, 0x57, 0x01, 0x50, 0x02, 0x56, 0x02, 0x49, 0x01,
                0x41, 0x02, 0x43, 0x01, 0x54, 0x01, 0x49, 0x01, 0x4f, 0x01, 0x4e, 0x02, 0x4e, 0x02, 0x45, 0x01,
                0x57, 0x01, 0x53, 0x02, 0x53, 0x02, 0x41, 0x02, 0x54, 0x01, 0x55, 0x02, 0x52, 0x01, 0x44, 0x02,
                0x41, 0x02, 0x59, 0x02, 0x4d, 0x02, 0x4f, 0x01, 0x52, 0x01, 0x4e, 0x02, 0x49, 0x01, 0x4e, 0x02,
                0x47, 0x02, 0x41, 0x02, 0x54, 0x01, 0x36, 0x02, 0x41, 0x02, 0x4d, 0x02, 0x11, 0x02, 0x01, 0x01,
                0x01, 0x01, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02, 0x01, 0x01,
                0x0b, 0x01, 0x00, 0x02, 0x43, 0x01, 0x07, 0x01, 0xb9, 0x01, 0xb2, 0x02, 0x16, 0x01, 0x00, 0x02,
                0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x31, 0x01, 0x50, 0x02, 0x43, 0x01, 0x52, 0x01, 0x31, 0x01,
                0x5f, 0x02, 0x30, 0x02, 0x33, 0x02, 0x30, 0x02, 0x35, 0x02, 0x32, 0x01, 0x32, 0x01, 0x30, 0x02,
                0x36, 0x02, 0x35, 0x02, 0x38, 0x01, 0x57, 0x01, 0x50, 0x02, 0x56, 0x02, 0x49, 0x01, 0x41, 0x02,
                0x43, 0x01, 0x54, 0x01, 0x49, 0x01, 0x4f, 0x01, 0x4e, 0x02, 0x4e, 0x02, 0x45, 0x01, 0x57, 0x01,
                0x53, 0x02, 0x53, 0x02, 0x41, 0x02, 0x54, 0x01, 0x55, 0x02, 0x52, 0x01, 0x44, 0x02, 0x41, 0x02,
                0x59, 0x02, 0x4d, 0x02, 0x4f, 0x01, 0x52, 0x01, 0x4e, 0x02, 0x49, 0x01, 0x4e, 0x02, 0x47, 0x02,
                0x41, 0x02, 0x54, 0x01, 0x37, 0x01, 0x41, 0x02, 0x4d, 0x02, 0x10, 0x01, 0x01, 0x01, 0x01, 0x01,
                0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02, 0x01, 0x01, 0x0b, 0x01,
                0x00, 0x02, 0x43, 0x01, 0x07, 0x01, 0xb9, 0x01, 0xb2, 0x02, 0x17, 0x02, 0x00, 0x02, 0x02, 0x01,
                0x62, 0x01, 0x01, 0x01, 0x31, 0x01, 0x50, 0x02, 0x43, 0x01, 0x52, 0x01, 0x31, 0x01, 0x5f, 0x02,
                0x30, 0x02, 0x33, 0x02, 0x30, 0x02, 0x35, 0x02, 0x32, 0x01, 0x32, 0x01, 0x30, 0x02, 0x36, 0x02,
                0x35, 0x02, 0x38, 0x01, 0x57, 0x01, 0x50, 0x02, 0x56, 0x02, 0x49, 0x01, 0x41, 0x02, 0x43, 0x01,
                0x54, 0x01, 0x49, 0x01, 0x4f, 0x01, 0x4e, 0x02, 0x4e, 0x02, 0x45, 0x01, 0x57, 0x01, 0x53, 0x02,
                0x53, 0x02, 0x41, 0x02, 0x54, 0x01, 0x55, 0x02, 0x52, 0x01, 0x44, 0x02, 0x41, 0x02, 0x59, 0x02,
                0x4d, 0x02, 0x4f, 0x01, 0x52, 0x01, 0x4e, 0x02, 0x49, 0x01, 0xd5, 0x02,
                0x00, 0x00, 0xff, 0x03, 0xff, 0x03, 0x41, 0x02, 0x07, 0x01, 0x11, 0x02, 0x0a, 0x02, 0x4e, 0x02,
                0x47, 0x02, 0x41, 0x02, 0x54, 0x01, 0x37, 0x01, 0x41, 0x02, 0x4d, 0x02, 0x20, 0x01, 0x01, 0x01,
                0x01, 0x01, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02, 0x7a, 0x01,
            };
            endian_flip_array(msg, sizeof(msg));
            ret = _vancparse(decklink_ctx->vanchdl, &msg[0], sizeof(msg), 10);
            if (ret < 0) {
                fprintf(stderr, "%s() Unable to parse scte104 fragment\n", __func__);
            }

            /* Paint a message in the V210, so we know per frame when an event was received. */
            videoframe->GetBytes(&frame_bytes);
            struct V210_painter_s painter;
            V210_painter_reset(&painter, (unsigned char *)frame_bytes, width, height, stride, 0);
            V210_painter_draw_ascii_at(&painter, 0, 3, "SCTE104 - fragments injected");
#endif
        }

}

int vega3311_video_configure_hevc(vega_opts_t *opts)
{
	printf(MODULE_PREFIX "%s()\n", __func__);

	vega_ctx_t *ctx = &opts->ctx;

// API_VENC_INIT_PARAM_T
        ctx->init_params.eCodecType             = API_VEGA_BQB_CODEC_TYPE_HEVC;
        ctx->init_params.eOutputFmt             = API_VEGA_BQB_STREAM_OUTPUT_FORMAT_ES;

        /* HEVC */
        ctx->init_params.tHevcParam.eInputMode       = API_VEGA_BQB_INPUT_MODE_DATA;  /* Source data from Host */
        //ctx->init_params.tHevcParam.eInputMode       = API_VEGA_BQB_INPUT_MODE_VIF_SQUARE;
        ctx->init_params.tHevcParam.eInputPort       = (API_VEGA_BQB_VIF_MODE_INPUT_PORT_E)opts->card_idx; // API_VEGA_BQB_VIF_MODE_INPUT_PORT_A;
        ctx->init_params.tHevcParam.eRobustMode      = API_VEGA_BQB_VIF_ROBUST_MODE_BLUE_SCREEN;
        ctx->init_params.tHevcParam.eProfile         = API_VEGA_BQB_HEVC_MAIN422_10_PROFILE;
        ctx->init_params.tHevcParam.eLevel           = API_VEGA_BQB_HEVC_LEVEL_41;
        ctx->init_params.tHevcParam.eTier            = API_VEGA_BQB_HEVC_MAIN_TIER;
        ctx->init_params.tHevcParam.eResolution      = opts->codec.encodingResolution;
        ctx->init_params.tHevcParam.eAspectRatioIdc  = API_VEGA_BQB_HEVC_ASPECT_RATIO_IDC_1;

        ctx->init_params.tHevcParam.u32SarWidth     = opts->width;
        ctx->init_params.tHevcParam.u32SarHeight    = opts->height;
        ctx->init_params.tHevcParam.bDisableVpsTimingInfoPresent = false;
        ctx->init_params.tHevcParam.eFormat          = opts->codec.eFormat;
        ctx->init_params.tHevcParam.eChromaFmt       = opts->codec.chromaFormat;
        ctx->init_params.tHevcParam.eBitDepth        = opts->codec.bitDepth;
        ctx->init_params.tHevcParam.bInterlace       = opts->codec.interlaced;
        ctx->init_params.tHevcParam.bDisableSceneChangeDetect       = false;

        /* Prevent 1920x1080 encodes coming out as 1920x1088 */
        ctx->init_params.tHevcParam.tCrop.u32CropLeft   = 0;
        ctx->init_params.tHevcParam.tCrop.u32CropRight  = 0;
        ctx->init_params.tHevcParam.tCrop.u32CropTop    = 0;
        ctx->init_params.tHevcParam.tCrop.u32CropBottom = ctx->init_params.tHevcParam.u32SarHeight % 16;

        ctx->init_params.tHevcParam.eTargetFrameRate = opts->codec.fps;
// tcustomedframerateinfo
        ctx->init_params.tHevcParam.ePtsMode         = API_VEGA_BQB_PTS_MODE_AUTO;
        ctx->init_params.tHevcParam.eIDRFrameNum     = API_VEGA_BQB_IDR_FRAME_ALL;
        //ctx->init_params.tHevcParam.eIDRType         = API_VEGA_BQB_HEVC_IDR_TYPE_W_RADL;
        ctx->init_params.tHevcParam.eIDRType         = API_VEGA_BQB_HEVC_IDR_TYPE_N_LP;
        if (opts->codec.bframes) {
                ctx->init_params.tHevcParam.eGopType = API_VEGA_BQB_GOP_IPB;
        } else {
                ctx->init_params.tHevcParam.eGopType = API_VEGA_BQB_GOP_IP;
        }
// eGopHeirarchy
        ctx->init_params.tHevcParam.eGopSize         = opts->codec.gop_size;
        ctx->init_params.tHevcParam.eBFrameNum       = opts->codec.bframes;
        ctx->init_params.tHevcParam.bDisableTemporalId = false;
        ctx->init_params.tHevcParam.eRateCtrlAlgo    = API_VEGA_BQB_RATE_CTRL_ALGO_CBR;
// u32FillerTriggerLevel
        ctx->init_params.tHevcParam.u32Bitrate       = opts->codec.bitrate_kbps; /* TODO: We need to drive this from external to the module. */
// u32MaxVBR
// u32AveVBR
// u32MinVBR
// u32CpbDelay
        ctx->init_params.tHevcParam.tCoding.bDisableDeblocking  = false;
// tHdrConfig
        ctx->init_params.tHevcParam.bEnableUltraLowLatency = false;
        //ctx->init_params.bDisableVpsTimingInfoPresent = false;
        //ctx->init_params.tVideoSignalType.bPresentFlag = true;

        //VEGA_BQB_ENC_SetDbgMsgLevel((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx, API_VEGA_BQB_DBG_LEVEL_3);

#if 0
/**
@brief Creates a API_VEGA_BQB_INIT_PARAM_X_T structure for HEVC encode.

The \b VEGA_BQB_ENC_MakeInitParamX function helps user to create a VEGA-331X init parameter, which can be used to initialize the 1-N encoder.

@param pApiInitParam    pointer to init parameter to be set; provided by caller.
@param eInputResolution input resolution size
@param eInputChromaFmt  input chroma format
@param eInputBitDepth   input bit depth
@param eInputFrameRate  input frame rate
@param u32NumOfOutputs  output numbers
@param u32OutputId[]    array of output ID
@param eProfile[]       output profile values.
@param eLevel[]         output level values
@param eTier[]          output tier values
@param eResolution[]    output resolution values.
@param eChromaFmt[]     output chroma formats.
@param eBitDepth[]      output bit depth.
@param eTargetFrameRate[] output frame rate values.
@param u32Bitrate[]     output bitrate value in kbps. 0 < u32Bitrate <= API_MAX_BITRATE.
@param eGopHierarchy[]  output GOP hierarchy
@param eGopSize[]       output GOP sizes
@param eGopType[]       output GOP types
@param eBFrameNum[]     number of B frame used in a group
@param u32CpbDelay[]    output CPB delay value.
@return
- API_VEGA_BQB_RET_SUCCESS: Successful
- API_VEGA_BQB_RET_FAIL: Failed
*/
LIBVEGA_BQB_API API_VEGA_BQB_RET
VEGA_BQB_ENC_MakeInitParamX
(
API_VEGA_BQB_INIT_PARAM_X_T    *pApiInitParam,
const API_VEGA_BQB_RESOLUTION_E       eInputResolution,
const API_VEGA_BQB_CHROMA_FORMAT_E    eInputChromaFmt,
const API_VEGA_BQB_BIT_DEPTH_E        eInputBitDepth,
const API_VEGA_BQB_FPS_E              eInputFrameRate,
const uint32_t                        u32NumOfOutputs,
const uint32_t                        u32OutputId[],
const API_VEGA_BQB_HEVC_PROFILE_E     eProfile[],
const API_VEGA_BQB_HEVC_LEVEL_E       eLevel[],
const API_VEGA_BQB_HEVC_TIER_E        eTier[],
const API_VEGA_BQB_RESOLUTION_E       eResolution[],
const API_VEGA_BQB_CHROMA_FORMAT_E    eChromaFmt[],
const API_VEGA_BQB_BIT_DEPTH_E        eBitDepth[],
const API_VEGA_BQB_FPS_E              eTargetFrameRate[],
const uint32_t                        u32Bitrate[],
const API_VEGA_BQB_GOP_HIERARCHY_E    eGopHierarchy[],
const API_VEGA_BQB_GOP_SIZE_E         eGopSize[],
const API_VEGA_BQB_GOP_TYPE_E         eGopType[],
const API_VEGA_BQB_B_FRAME_NUM_E      eBFrameNum[],
const uint32_t                        u32CpbDelay[]
);
/**
@brief Creates a API_VEGA_BQB_INIT_PARAM_T structure for HEVC encode.

The \b VEGA_BQB_ENC_MakeInitParam function helps user to create a VEGA331X init parameter, which can be used to initialize the VEGA331X encoder.

@param pApiInitParam    pointer to init parameter to be set; provided by caller.
@param eProfile         profile value.
@param eLevel           level value
@param eTier            tier value.
@param eResolution      resolution value.
@param eChromaFmt       chroma format.
@param eBitDepth        bit depth.
@param eTargetFrameRate frame rate value.
@param u32Bitrate       bitrate value in kbps. 0 < u32Bitrate <= API_MAX_BITRATE.
@param u32CpbDelay      CPB delay value.
@return
- API_VEGA_BQB_RET_SUCCESS: Successful
- API_VEGA_BQB_RET_FAIL: Failed
*/
LIBVEGA_BQB_API API_VEGA_BQB_RET
VEGA_BQB_ENC_MakeInitParam
(
API_VEGA_BQB_INIT_PARAM_T      *pApiInitParam,
const API_VEGA_BQB_HEVC_PROFILE_E     eProfile,
const API_VEGA_BQB_HEVC_LEVEL_E       eLevel,
const API_VEGA_BQB_HEVC_TIER_E        eTier,
const API_VEGA_BQB_RESOLUTION_E       eResolution,
const API_VEGA_BQB_CHROMA_FORMAT_E    eChromaFmt,
const API_VEGA_BQB_BIT_DEPTH_E        eBitDepth,
const API_VEGA_BQB_FPS_E              eTargetFrameRate,
const uint32_t                        u32Bitrate,
const uint32_t                        u32CpbDelay
);

VEGA_BQB_ENC_MakeULLInitParam
(
pInitParam,
API_VEGA_BQB_HEVC_MAIN_PROFILE,
API_VEGA_BQB_HEVC_LEVEL_AUTO,
API_VEGA_BQB_HEVC_HIGH_TIER,
API_VEGA_BQB_RESOLUTION_3840x2160,
API_VEGA_BQB_CHROMA_FORMAT_420,
API_VEGA_BQB_BIT_DEPTH_8,
API_VEGA_BQB_FPS_60,
60000
);
#endif

        VEGA_BQB_ENC_MakeInitParam
        (
                &ctx->init_paramsMACRO,
                API_VEGA_BQB_HEVC_MAIN422_10_PROFILE,
                API_VEGA_BQB_HEVC_LEVEL_41,
                API_VEGA_BQB_HEVC_MAIN_TIER,
                opts->codec.encodingResolution,
                opts->codec.chromaFormat,
                opts->codec.bitDepth,
                opts->codec.fps,
                opts->codec.bitrate_kbps,
                270000
        );
        ctx->init_paramsMACRO.eOutputFmt = API_VEGA_BQB_STREAM_OUTPUT_FORMAT_ES;

        VEGA_BQB_ENC_MakeULLInitParam
        (
                &ctx->init_paramsMACROULL,
                API_VEGA_BQB_HEVC_MAIN422_10_PROFILE,
                API_VEGA_BQB_HEVC_LEVEL_AUTO,
                API_VEGA_BQB_HEVC_MAIN_TIER,
                opts->codec.encodingResolution,
                opts->codec.chromaFormat,
                opts->codec.bitDepth,
                opts->codec.fps,
                opts->codec.bitrate_kbps
        );
        ctx->init_paramsMACRO.eOutputFmt = API_VEGA_BQB_STREAM_OUTPUT_FORMAT_ES;

        /* Configure HDR */
        if (OPTION_ENABLED(hdr)) {
                ctx->init_params.tHevcParam.tVideoSignalType.bPresentFlag = true;
                ctx->init_params.tHevcParam.tVideoSignalType.bVideoFullRange = false;
                ctx->init_params.tHevcParam.tVideoSignalType.eVideoFormat = API_VEGA_BQB_VIDEO_FORMAT_UNSPECIFIED;
                ctx->init_params.tHevcParam.tVideoSignalType.tColorDesc.bPresentFlag = true;
                ctx->init_params.tHevcParam.tVideoSignalType.tColorDesc.eColorPrimaries = API_VEGA_BQB_COLOR_PRIMARY_BT2020;
                ctx->init_params.tHevcParam.tVideoSignalType.tColorDesc.eTransferCharacteristics = API_VEGA_BQB_TRANSFER_CHAR_SMPTE_ST_2084;
                ctx->init_params.tHevcParam.tVideoSignalType.tColorDesc.eMatrixCoeff = API_VEGA_BQB_MATRIX_COEFFS_BT2020NC;

#if 1
                /* We assume this to mean, don't automatically put the HDR SEI in the GOP automatically,
                 * we'll do this later by hand as a result of the VANC callbacks.
                 */
                ctx->init_params.tHevcParam.tHdrConfig.bEnable = false;
#else
                /* DO we need to configure the HDR metadata, if we're attaching it to each GOP with vanc callbacks? */
                ctx->init_params.tHevcParam.tHdrConfig.bEnable = true;
                ctx->init_params.tHevcParam.tHdrConfig.u8LightContentLocation = 0;
                ctx->init_params.tHevcParam.tHdrConfig.u16MaxContentLightLevel = 10000;
                ctx->init_params.tHevcParam.tHdrConfig.u16MaxPictureAvgLightLevel = 4000;

                ctx->init_params.tHevcParam.tHdrConfig.u8AlternativeTransferCharacteristicsLocation = 1;
                ctx->init_params.tHevcParam.tHdrConfig.eAlternativeTransferCharacteristics = API_VEGA_BQB_TRANSFER_CHAR_BT2020_10;

                ctx->init_params.tHevcParam.tHdrConfig.u8MasteringDisplayColourVolumeLocation = 2;
                ctx->init_params.tHevcParam.tHdrConfig.u16DisplayPrimariesX0 = 13250;
                ctx->init_params.tHevcParam.tHdrConfig.u16DisplayPrimariesY0 = 34500;
                ctx->init_params.tHevcParam.tHdrConfig.u16DisplayPrimariesX1 = 7500;
                ctx->init_params.tHevcParam.tHdrConfig.u16DisplayPrimariesY1 = 3000;
                ctx->init_params.tHevcParam.tHdrConfig.u16DisplayPrimariesX2 = 34000;
                ctx->init_params.tHevcParam.tHdrConfig.u16DisplayPrimariesY2 = 16000;
                ctx->init_params.tHevcParam.tHdrConfig.u16WhitePointX = 15635;
                ctx->init_params.tHevcParam.tHdrConfig.u16WhitePointY = 16450;
                ctx->init_params.tHevcParam.tHdrConfig.u32MaxDisplayMasteringLuminance = 10000000;
                ctx->init_params.tHevcParam.tHdrConfig.u32MinDisplayMasteringLuminance = 500;
#endif
        }

        if (VEGA_BQB_ENC_IsDeviceModeConfigurable((API_VEGA_BQB_DEVICE_E)opts->brd_idx)) {
                fprintf(stderr, "DEVICE MODE IS CONFIGURABLE (HEVC)\n");
                API_VEGA_BQB_ENCODE_CONFIG_T encode_config;
                memset(&encode_config, 0, sizeof(API_VEGA_BQB_ENCODE_CONFIG_T));
                switch (ctx->init_params.tHevcParam.eResolution) {
                case API_VEGA_BQB_RESOLUTION_4096x2160:
                case API_VEGA_BQB_RESOLUTION_3840x2160:
                        encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_1CH_4K2K;
                        break;
                case API_VEGA_BQB_RESOLUTION_2048x1080:
                case API_VEGA_BQB_RESOLUTION_1920x1080:
                        encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_4CH_1080P;
                        break;
                case API_VEGA_BQB_RESOLUTION_1280x720:
                        encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_8CH_720P;
                        break;
                case API_VEGA_BQB_RESOLUTION_720x576:
                case API_VEGA_BQB_RESOLUTION_720x480:
                case API_VEGA_BQB_RESOLUTION_416x240:
                case API_VEGA_BQB_RESOLUTION_352x288:
                        encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_16CH_SD;
                        break;
                default:
                        encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_1CH_4K2K;
                        break;
                }

                VEGA_BQB_ENC_ConfigDeviceMode((API_VEGA_BQB_DEVICE_E)opts->brd_idx, &encode_config);
        }

        return 0; /* success */
}

int vega3311_video_configure_avc(vega_opts_t *opts)
{

/**
@brief Creates a API_VEGA_BQB_INIT_PARAM_T structure for H.264 encode.

The \b VEGA_BQB_ENC_MakeAVCInitParam function helps user to create a VEGA331X init parameter, which can be used to initialize the 1-1 AVC encoder.

@param pApiInitParam    pointer to init parameter to be set; provided by caller.
@param eProfile         profile value.
@param eLevel           level value
@param eResolution      resolution value.
@param eChromaFmt       chroma format.
@param eBitDepth        bit depth.
@param eTargetFrameRate frame rate value.
@param u32Bitrate       bitrate value in kbps. 0 < u32Bitrate <= API_MAX_BITRATE.
@param u32CpbDelay      CPB delay value.
@return
- API_VEGA_BQB_RET_SUCCESS: Successful
- API_VEGA_BQB_RET_FAIL: Failed
*/
LIBVEGA_BQB_API API_VEGA_BQB_RET
VEGA_BQB_ENC_MakeAVCInitParam
(
        API_VEGA_BQB_INIT_PARAM_T      *pApiInitParam,
  const API_VEGA_BQB_AVC_PROFILE_E      eProfile,
  const API_VEGA_BQB_AVC_LEVEL_E        eLevel,
  const API_VEGA_BQB_RESOLUTION_E       eResolution,
  const API_VEGA_BQB_CHROMA_FORMAT_E    eChromaFmt,
  const API_VEGA_BQB_BIT_DEPTH_E        eBitDepth,
  const API_VEGA_BQB_FPS_E              eTargetFrameRate,
  const uint32_t                        u32Bitrate,
  const uint32_t                        u32CpbDelay
);

	printf(MODULE_PREFIX "%s()\n", __func__);

	vega_ctx_t *ctx = &opts->ctx;

        if (VEGA_BQB_ENC_MakeAVCInitParam(
                &ctx->init_params,
                API_VEGA_BQB_AVC_HIGH_PROFILE, // API_VEGA_BQB_AVC_HIGH_PROFILE,
                API_VEGA_BQB_AVC_LEVEL_52,
                opts->codec.encodingResolution,
                opts->codec.chromaFormat,
                opts->codec.bitDepth,
                opts->codec.fps,
                opts->codec.bitrate_kbps,
                270000) != 0)
        {
                fprintf(stderr, MODULE_PREFIX "FAILED TO CALL MACRO TO CONFIGURE\n");
        }
        ctx->init_params.eOutputFmt = API_VEGA_BQB_STREAM_OUTPUT_FORMAT_ES;
        ctx->init_params.tAvcParam.eGopType = API_VEGA_BQB_GOP_IP;
        fprintf(stderr, MODULE_PREFIX "CALLED MACRO TO CONFIGURE AVC\n");

        ctx->init_params.tAvcParam.eInputMode       = API_VEGA_BQB_INPUT_MODE_DATA;  /* Source data from Host */
        ctx->init_params.tAvcParam.eInputPort       = (API_VEGA_BQB_VIF_MODE_INPUT_PORT_E)opts->card_idx; // API_VEGA_BQB_VIF_MODE_INPUT_PORT_A;

        VEGA_BQB_ENC_SetDbgMsgLevel((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx, API_VEGA_BQB_DBG_LEVEL_0);

        /* Configure HDR */
        if (OPTION_ENABLED(hdr)) {
                /* No HDR support in H.264 */
        }

        if (VEGA_BQB_ENC_IsDeviceModeConfigurable((API_VEGA_BQB_DEVICE_E)opts->brd_idx)) {
                fprintf(stderr, "DEVICE MODE IS CONFIGURABLE (AVC)\n");
                API_VEGA_BQB_ENCODE_CONFIG_T encode_config;
                memset(&encode_config, 0, sizeof(API_VEGA_BQB_ENCODE_CONFIG_T));
                switch (ctx->init_params.tAvcParam.eResolution) {
                case API_VEGA_BQB_RESOLUTION_4096x2160:
                case API_VEGA_BQB_RESOLUTION_3840x2160:
                        encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_1CH_4K2K;
                        break;
                case API_VEGA_BQB_RESOLUTION_2048x1080:
                case API_VEGA_BQB_RESOLUTION_1920x1080:
                        encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_4CH_1080P;
                        break;
                case API_VEGA_BQB_RESOLUTION_1280x720:
                        encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_8CH_720P;
                        break;
                case API_VEGA_BQB_RESOLUTION_720x576:
                case API_VEGA_BQB_RESOLUTION_720x480:
                case API_VEGA_BQB_RESOLUTION_416x240:
                case API_VEGA_BQB_RESOLUTION_352x288:
                        encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_16CH_SD;
                        break;
                default:
                        encode_config.eMode = API_VEGA_BQB_DEVICE_ENC_MODE_1CH_4K2K;
                        break;
                }

                VEGA_BQB_ENC_ConfigDeviceMode((API_VEGA_BQB_DEVICE_E)opts->brd_idx, &encode_config);
        }

        return 0; /* success */
}

int vega3311_video_configure_avc2(vega_opts_t *opts)
{
	printf(MODULE_PREFIX "%s()\n", __func__);
return -1;

	vega_ctx_t *ctx = &opts->ctx;

// API_VENC_INIT_PARAM_T
        ctx->init_params.eCodecType             = API_VEGA_BQB_CODEC_TYPE_AVC;
        ctx->init_params.eOutputFmt             = API_VEGA_BQB_STREAM_OUTPUT_FORMAT_ES;

        /* HEVC */
        ctx->init_params.tAvcParam.eInputMode       = API_VEGA_BQB_INPUT_MODE_DATA;  /* Source data from Host */
        //ctx->init_params.tHevcParam.eInputMode       = API_VEGA_BQB_INPUT_MODE_VIF_SQUARE;
        ctx->init_params.tAvcParam.eInputPort       = (API_VEGA_BQB_VIF_MODE_INPUT_PORT_E)opts->card_idx; // API_VEGA_BQB_VIF_MODE_INPUT_PORT_A;
        ctx->init_params.tAvcParam.eRobustMode      = API_VEGA_BQB_VIF_ROBUST_MODE_BLUE_SCREEN;
        ctx->init_params.tAvcParam.eProfile         = API_VEGA_BQB_AVC_HIGH_PROFILE;
        ctx->init_params.tAvcParam.eLevel           = API_VEGA_BQB_AVC_LEVEL_42;
        ctx->init_params.tAvcParam.eEntropyCoding   = API_VEGA_BQB_AVC_ENTROPY_CODING_CABAC;
        ctx->init_params.tAvcParam.eResolution      = opts->codec.encodingResolution;
        ctx->init_params.tAvcParam.bAspectRatioInfoPresent  = true;
        ctx->init_params.tAvcParam.eAspectRatioIdc  = API_VEGA_BQB_AVC_ASPECT_RATIO_IDC_1;

        ctx->init_params.tAvcParam.u32SarWidth     = opts->width;
        ctx->init_params.tAvcParam.u32SarHeight    = opts->height;
        ctx->init_params.tAvcParam.bDisableTimingInfoPresent = false;
        //ctx->init_params.tAvcParam.eFormat          = opts->codec.eFormat;
        ctx->init_params.tAvcParam.eChromaFmt       = opts->codec.chromaFormat;
        //ctx->init_params.tAvcParam.tChromaConvertInfo       = 
        ctx->init_params.tAvcParam.eOverScan       = API_VEGA_BQB_OVERSCAN_INFO_INAPPROPRIATE;
        ctx->init_params.tAvcParam.tVideoSignalType.bPresentFlag = false;
        //ctx->init_params.tAvcParam.tVideoSignalType.eVideoFormat =
        //ctx->init_params.tAvcParam.tVideoSignalType.bVideoFullRange =
        //ctx->init_params.tAvcParam.tVideoSignalType.tColorDesc =
        ctx->init_params.tAvcParam.tChromaLocation.bChromaLoc = false;
        
        ctx->init_params.tAvcParam.eBitDepth        = opts->codec.bitDepth;
        ctx->init_params.tAvcParam.bInterlace       = opts->codec.interlaced;
        ctx->init_params.tAvcParam.bDisableSceneChangeDetect       = false;
        ctx->init_params.tAvcParam.eScPicType       = API_VEGA_BQB_SC_PICTYPE_I;
 
        /* Prevent 1920x1080 encodes coming out as 1920x1088 */
        ctx->init_params.tAvcParam.tCrop.u32CropLeft   = 0;
        ctx->init_params.tAvcParam.tCrop.u32CropRight  = 0;
        ctx->init_params.tAvcParam.tCrop.u32CropTop    = 0;
        ctx->init_params.tAvcParam.tCrop.u32CropBottom = ctx->init_params.tAvcParam.u32SarHeight % 16;

        ctx->init_params.tAvcParam.eTargetFrameRate = opts->codec.fps;

// tcustomedframerateinfo
        ctx->init_params.tAvcParam.ePtsMode         = API_VEGA_BQB_PTS_MODE_AUTO;
        ctx->init_params.tAvcParam.eIDRFrameNum     = API_VEGA_BQB_IDR_FRAME_ALL;
        ctx->init_params.tAvcParam.bIDRDisplayOrderFirst     = true;
        if (opts->codec.bframes) {
                ctx->init_params.tAvcParam.eGopType = API_VEGA_BQB_GOP_IPB;
        } else {
                ctx->init_params.tAvcParam.eGopType = API_VEGA_BQB_GOP_IP;
        }
        ctx->init_params.tAvcParam.eGopHierarchy = API_VEGA_BQB_GOP_HIERARCHY;
 
        ctx->init_params.tAvcParam.eGopSize         = opts->codec.gop_size;
        ctx->init_params.tAvcParam.eBFrameNum       = opts->codec.bframes;
        ctx->init_params.tAvcParam.bDisableTemporalId = false;
        ctx->init_params.tAvcParam.eRateCtrlAlgo    = API_VEGA_BQB_RATE_CTRL_ALGO_CBR;
        //ctx->init_params.tAvcParam.u32FillerTriggerLevel    = 
        ctx->init_params.tAvcParam.u32Bitrate       = opts->codec.bitrate_kbps;
// u32MaxVBR
// u32AveVBR
// u32MinVBR
// u32CpbDelay
        ctx->init_params.tAvcParam.tCoding.bDisableDeblocking  = false;
// tHdrConfig

        VEGA_BQB_ENC_SetDbgMsgLevel((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx, API_VEGA_BQB_DBG_LEVEL_3);

        /* Configure HDR */
        if (OPTION_ENABLED(hdr)) {
                /* No HDR support in H.264 */
        }

        return 0; /* success */
}

#endif /* #if HAVE_VEGA3311_CAP_TYPES_H */
