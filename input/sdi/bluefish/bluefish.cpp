#if HAVE_BLUEDRIVER_P_H

/*****************************************************************************
 * bluefish.cpp: BlueFish444 Far frame grabber input module
 *****************************************************************************
 * Copyright (C) 2019 LTN
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

//#define __STDC_FORMAT_MACROS   1
//#define __STDC_CONSTANT_MACROS 1

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include <signal.h>
#include "BlueVelvetC.h"
#include "BlueVelvetCUtils.h"
#include "BlueVelvetCHancUtils.h"
//#include "BlueHancUtils.h"

using namespace std;

// Other
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>

#define MODULE_PREFIX "[bluefish]: "
#define BLUEFISH_HANC_BUFFER_SIZE (256 * 1024)

extern "C"
{
#include "common/common.h"
#include "common/lavc.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include "input/sdi/ancillary.h"
#include "input/sdi/vbi.h"
#include "input/sdi/x86/sdi.h"
#include <libavresample/avresample.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/bswap.h>
#include <libyuv/convert.h>
#include <alsa/asoundlib.h>
}

struct obe_to_v4l2
{
    int obe_name;
    uint32_t bmd_name;
};

struct obe_to_bluefish_video
{
    int obe_name;
    int width, height;
    uint32_t reserved;
    int timebase_num;
    int timebase_den;
};

const static struct obe_to_bluefish_video video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_720P_5994, 1280, 720, 0 /* bmdModeHD720p60 */, 1001, 60000 },
    { INPUT_VIDEO_FORMAT_720P_60,   1280, 720, 0 /* bmdModeHD720p60 */, 1000, 60000 },
};

static int lookupOBEName(int w, int h, int den, int num)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_bluefish_video)); i++) {
		const struct obe_to_bluefish_video *fmt = &video_format_tab[i];
		if ((fmt->width == w) && (fmt->height == h) &&
			(fmt->timebase_den == den) && (fmt->timebase_num == num))
		{
			return fmt->obe_name;
		}
	}

	return video_format_tab[0].obe_name;
}

typedef struct
{
	/* Bluefish SDK related */
	BLUEVELVETC_HANDLE hBvc;
	EVideoMode VideoMode;
	int iCardType;
	uint64_t v_counter;
	unsigned long CurrentFieldCount;
	unsigned long LastFieldCount;
	unsigned int frameSizeBytesVideo;

	/* VANC */
	unsigned int frameSizeBytesVanc;
	unsigned int vancLineCount;

	/* HANC */
	unsigned int frameSizeBytesHanc;
	struct hanc_decode_struct HancInfo;

	/* End: Bluefish SDK related */

	/* AVCodec for V210 conversion. */
	AVCodec         *dec;
	AVCodecContext  *codec;
	/* End: AVCodec for V210 conversion. */

	AVAudioResampleContext *avr;

	pthread_t vthreadId;
	int vthreadTerminate, vthreadRunning, vthreadComplete;

	obe_device_t *device;
	obe_t *h;

	AVRational   v_timebase;
} bluefish_ctx_t;

typedef struct
{
    bluefish_ctx_t ctx;

    /* Input */
    int card_idx;

    int video_format;
    int num_channels;

    /* True if we're problem, else false during normal streaming. */
    int probe;

    /* Output */
    int probe_success;

    int width;
    int height;
    int timebase_num;
    int timebase_den;

    int interlaced;
    int tff;
} bluefish_opts_t;

static void BailOut(BLUEVELVETC_HANDLE hBvc)
{
        bfcDetach(hBvc);
        bfcDestroy(hBvc);
}

static void InitInputChannel(BLUEVELVETC_HANDLE hBvc, unsigned int DefaultInputChannel, unsigned int UpdateFormat,
        unsigned int MemoryFormat, unsigned int VideoEngine, unsigned int nInputConnector)
{
        /* MOST IMPORTANT: as the first step set the channel that we want to work with */
        bfcSetCardProperty32(hBvc, DEFAULT_VIDEO_INPUT_CHANNEL, DefaultInputChannel);

        /* make sure the FIFO hasn't been left running (e.g. application crash before), otherwise we can't change card properties */
        bfcVideoCaptureStop(hBvc);

        unsigned int nMR2InputChannel = EPOCH_DEST_INPUT_MEM_INTERFACE_CHA;
        if(DefaultInputChannel == BLUE_VIDEO_INPUT_CHANNEL_D)
                nMR2InputChannel = EPOCH_DEST_INPUT_MEM_INTERFACE_CHD;
        else if(DefaultInputChannel == BLUE_VIDEO_INPUT_CHANNEL_C)
                nMR2InputChannel = EPOCH_DEST_INPUT_MEM_INTERFACE_CHC;
        else if(DefaultInputChannel == BLUE_VIDEO_INPUT_CHANNEL_B)
                nMR2InputChannel = EPOCH_DEST_INPUT_MEM_INTERFACE_CHB;
        else //if(DefaultInputChannel == BLUE_VIDEO_INPUT_CHANNEL_A)
                nMR2InputChannel = EPOCH_DEST_INPUT_MEM_INTERFACE_CHA;

        bfcUtilsSetMR2Routing(hBvc, nInputConnector, nMR2InputChannel, BLUE_CONNECTOR_PROP_SINGLE_LINK);

        bfcSetCardProperty32(hBvc, VIDEO_INPUT_UPDATE_TYPE, UpdateFormat);
        bfcSetCardProperty32(hBvc, VIDEO_INPUT_MEMORY_FORMAT, MemoryFormat);

        /* Only set the Video Engine after setting up the required update type and memory format
         * and make sure that there is a valid input signal
         */
        bfcSetCardProperty32(hBvc, VIDEO_INPUT_ENGINE, VideoEngine);

	int ret = bfcSetCardProperty32(hBvc, AUDIO_INPUT_PROP, BLUE_AUDIO_EMBEDDED);
	if (BLUE_FAIL(ret)) {
		fprintf(stderr, MODULE_PREFIX "Failed to set audio input to embedded\n");
	}
}

static void *bluefish_videoThreadFunc(void *p)
{
	bluefish_opts_t *opts = (bluefish_opts_t *)p;
	bluefish_ctx_t *ctx = &opts->ctx;
	int finished = 0;
	int ret;

	printf(MODULE_PREFIX "Video thread starts\n");

	ctx->v_counter = 0;
	ctx->vthreadRunning = 1;
	ctx->vthreadComplete = 0;
	ctx->vthreadTerminate = 0;

	unsigned int ScheduleID = 0;
	unsigned int CapturingID = 0;
	unsigned int DoneID = 0;

	/* start the capture sequence
         * this call just synchronises us to the card
         */
	bfcWaitVideoInputSync(ctx->hBvc, UPD_FMT_FRAME, ctx->CurrentFieldCount);
	bfcRenderBufferCapture(ctx->hBvc, BlueBuffer_Image(ScheduleID));
	//bfcRenderBufferCapture(ctx->hBvc, BlueBuffer_Image_VBI(ScheduleID));
	CapturingID = ScheduleID;
	ScheduleID = (ScheduleID+1)%4;
	ctx->LastFieldCount = ctx->CurrentFieldCount;

	/* the first buffer starts to be captured now; this is it's field count */
	bfcWaitVideoInputSync(ctx->hBvc, UPD_FMT_FRAME, ctx->CurrentFieldCount);
	bfcRenderBufferCapture(ctx->hBvc, BlueBuffer_Image(ScheduleID));
	//bfcRenderBufferCapture(ctx->hBvc, BlueBuffer_Image_VBI(ScheduleID));
	DoneID = CapturingID;
	CapturingID = ScheduleID;
	ScheduleID = (ScheduleID + 1) % 4;
	ctx->LastFieldCount = ctx->CurrentFieldCount;

	/* Get VID_FMT_INVALID flag; this enum has changed over time and might be
	 * different depending on which driver this application runs on.
	 */
	unsigned int nValue = 0;
	bfcQueryCardProperty32(ctx->hBvc, INVALID_VIDEO_MODE_FLAG, nValue);
	unsigned int InvalidVideoModeFlag = nValue;

	printf(MODULE_PREFIX "frameSizeBytesVideo = %d\n", ctx->frameSizeBytesVideo);
	printf(MODULE_PREFIX "frameSizeBytesVanc = %d, line count = %d\n", ctx->frameSizeBytesVanc, ctx->vancLineCount);
	printf(MODULE_PREFIX "frameSizeBytesHanc = %d\n", ctx->frameSizeBytesHanc);
	unsigned char *pVideoBuffer = (unsigned char *)valloc(ctx->frameSizeBytesVideo);
	unsigned char *pVancBuffer = (unsigned char *)valloc(ctx->frameSizeBytesVanc);
	unsigned char *pHancBuffer = (unsigned char *)valloc(ctx->frameSizeBytesHanc);
	memset(pHancBuffer, 0, ctx->frameSizeBytesHanc);
	memset(pVancBuffer, 0, ctx->frameSizeBytesVanc);

	/* Initialize the HANC parser, decode all audio channels. 1-16 */
	unsigned char* pAudioSamples = new unsigned char[2002*16*4];

	memset(&ctx->HancInfo, 0, sizeof(ctx->HancInfo));
	unsigned int nAudioChannels = 16;
	ctx->HancInfo.audio_ch_required_mask =
		MONO_CHANNEL_1 | MONO_CHANNEL_2 |
		MONO_CHANNEL_3 | MONO_CHANNEL_4 |
		MONO_CHANNEL_5 | MONO_CHANNEL_6 |
		MONO_CHANNEL_7 | MONO_CHANNEL_8 |
		MONO_CHANNEL_11 | MONO_CHANNEL_12 |
		MONO_CHANNEL_13 | MONO_CHANNEL_14 |
		MONO_CHANNEL_15 | MONO_CHANNEL_16 |
		MONO_CHANNEL_17 | MONO_CHANNEL_18;

	ctx->HancInfo.audio_pcm_data_ptr = pAudioSamples;
	ctx->HancInfo.type_of_sample_required = AUDIO_CHANNEL_16BIT;
	ctx->HancInfo.max_expected_audio_sample_count = 2002;

	ctx->v_counter = 0;
	while (!ctx->vthreadTerminate && opts->probe == 0) {

		bfcWaitVideoInputSync(ctx->hBvc, UPD_FMT_FRAME, ctx->CurrentFieldCount);

		if (ctx->LastFieldCount + 2 < ctx->CurrentFieldCount) {
			cout << "Error: dropped " << ((ctx->CurrentFieldCount - ctx->LastFieldCount + 2)/2)
				<< " frames" << endl;
		}
		ctx->LastFieldCount = ctx->CurrentFieldCount;

		/* tell the card to capture another frame at the next interrupt */
		bfcRenderBufferCapture(ctx->hBvc, BlueBuffer_Image_HANC(ScheduleID));
		//bfcRenderBufferCapture(ctx->hBvc, BlueBuffer_Image_VBI(ScheduleID));

		/* check if the video signal was valid for the last frame (until wait_input_video_synch() returned) */
		unsigned int nValue = 0;
		bfcQueryCardProperty32(ctx->hBvc, VIDEO_INPUT_SIGNAL_VIDEO_MODE, nValue);
		if (nValue < InvalidVideoModeFlag) {
			
			/* DoneID is now what ScheduleID was at the last iteration when we called
			 * render_buffer_capture(ScheduleID). We just checked if the video signal
			 * for the buffer DoneID was valid while it was capturing so we can DMA the buffer
			 * DMA the frame from the card to our buffer
			 */
#if 1
                        ret = bfcSystemBufferRead(ctx->hBvc, pVideoBuffer, ctx->frameSizeBytesVideo,
				BlueImage_DMABuffer(DoneID, BLUE_DATA_IMAGE));
#else
                        ret = bfcSystemBufferRead(ctx->hBvc, pVideoBuffer, ctx->frameSizeBytesVideo,
				BlueImage_VBI_DMABuffer(DoneID, BLUE_DATA_IMAGE));
#endif
			if (BLUE_FAIL(ret)) {
				fprintf(stderr, MODULE_PREFIX "Unable to read frame image\n");
			}

                        ret = bfcSystemBufferRead(ctx->hBvc, pVancBuffer, ctx->frameSizeBytesVanc,
				BlueImage_VBI_DMABuffer(DoneID, BLUE_DATA_VBI));
			if (BLUE_FAIL(ret)) {
				fprintf(stderr, MODULE_PREFIX "Unable to read frame vanc\n");
			}

#if 1
			ret = bfcSystemBufferRead(ctx->hBvc, pHancBuffer, ctx->frameSizeBytesHanc,
				BlueImage_DMABuffer(DoneID, BLUE_DATA_HANC));
#else
			ret = bfcSystemBufferRead(ctx->hBvc, pHancBuffer, ctx->frameSizeBytesHanc,
				BlueImage_VBI_DMABuffer(DoneID, BLUE_DATA_HANC));
#endif
			if (BLUE_FAIL(ret)) {
				fprintf(stderr, MODULE_PREFIX "Unable to read frame hanc\n");
			} else {
			}

			/* Process HANC and extract audio samples. */
			ctx->HancInfo.raw_custom_anc_pkt_data_ptr = NULL;
			ctx->HancInfo.audio_input_source = AUDIO_INPUT_SOURCE_EMB;
			hanc_decoder_ex(ctx->iCardType, (unsigned int*)pHancBuffer, &ctx->HancInfo);

			nValue = 0;
			ret = bfcQueryCardProperty32(ctx->hBvc, EMBEDDED_AUDIO_INPUT_INFO, nValue);
			if (BLUE_FAIL(ret)) {
				fprintf(stderr, MODULE_PREFIX "Unable to query Audio input info\n");
			}

#if 0
			printf("[%6d] 0x%16llx 0x%16llx 0x%16llx 0x%16llx\n",
				ctx->HancInfo.no_audio_samples,
				ctx->HancInfo.timecodes[0],
				ctx->HancInfo.timecodes[1],
				ctx->HancInfo.timecodes[2],
				ctx->HancInfo.timecodes[3]);
#endif

#if 0
			printf("Audio samples decoded: %4d (%8.2f), available 0x%04x, payload 0x%04x\n",
				ctx->HancInfo.no_audio_samples,
				ctx->HancInfo.no_audio_samples/(float)nAudioChannels,
				nValue & 0xffff,
				(nValue >> 16) & 0xffff);
#endif

#if 0
			char fn[64];
			snprintf(fn, sizeof(fn), "vbi%06d.raw", ctx->v_counter);
			FILE *fh = fopen(fn, "wb");
			if (fh) {
				fwrite(pVancBuffer, 1, ctx->frameSizeBytesVanc, fh);
				fclose(fh);
			}
#endif

			DoneID = CapturingID;
			CapturingID = ScheduleID;
			ScheduleID = (ScheduleID +1 ) % 4;

			/* Ship the video payload into the OBE pipeline. */
			obe_raw_frame_t *raw_frame = new_raw_frame();
			if (!raw_frame) {
				fprintf(stderr, MODULE_PREFIX "Could not allocate raw video frame\n");
				break;
			}

			AVFrame *frame = avcodec_alloc_frame();
			ctx->codec->width = opts->width;
			ctx->codec->height = opts->height;

			AVPacket pkt;
			av_init_packet(&pkt);

			pkt.data = (uint8_t *)pVideoBuffer;
			pkt.size = ctx->frameSizeBytesVideo;

			int ret = avcodec_decode_video2(ctx->codec, frame, &finished, &pkt);
			if (ret < 0 || !finished) {
				fprintf(stderr, MODULE_PREFIX "Could not decode video frame\n");
				break;
			}

			raw_frame->release_data = obe_release_video_data;
			raw_frame->release_frame = obe_release_frame;

			memcpy(raw_frame->alloc_img.stride, frame->linesize, sizeof(raw_frame->alloc_img.stride));
			memcpy(raw_frame->alloc_img.plane, frame->data, sizeof(raw_frame->alloc_img.plane));
			avcodec_free_frame(&frame);
			raw_frame->alloc_img.csp = (int)ctx->codec->pix_fmt;
			raw_frame->alloc_img.planes = av_pix_fmt_descriptors[raw_frame->alloc_img.csp].nb_components;
			raw_frame->alloc_img.width = opts->width;
			raw_frame->alloc_img.height = opts->height;
			raw_frame->alloc_img.format = opts->video_format;
			raw_frame->timebase_num = opts->timebase_num;
			raw_frame->timebase_den = opts->timebase_den;
			memcpy(&raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->alloc_img));

			int64_t pts = av_rescale_q(ctx->v_counter++, ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
			obe_clock_tick(ctx->h, pts);
			raw_frame->pts = pts;

			/* AVFM */
			avfm_init(&raw_frame->avfm, AVFM_VIDEO);
			avfm_set_hw_status_mask(&raw_frame->avfm, AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_FULL);

			/* Remember that we drive everything in the pipeline from the audio clock. */
			avfm_set_pts_video(&raw_frame->avfm, pts);
			avfm_set_pts_audio(&raw_frame->avfm, pts);

			avfm_set_hw_received_time(&raw_frame->avfm);
			double dur = 27000000 / ((double)opts->timebase_den / (double)opts->timebase_num);
			avfm_set_video_interval_clk(&raw_frame->avfm, dur);
			//raw_frame->avfm.hw_audio_correction_clk = clock_offset;
			//avfm_dump(&raw_frame->avfm);

			if (add_to_filter_queue(ctx->h, raw_frame) < 0 ) {
			}

			/* Handle all of the Audio..... */
			/* HANC parser produced S16 interleaved sames C1L | C1R | C2L | C2R
			 *                            we need planer  C1L | C1R | C1L | C1R .... | C2L | C2R .... etc
			 */
			{
				obe_raw_frame_t *aud_frame = new_raw_frame();
				if (!raw_frame) {
					fprintf(stderr, MODULE_PREFIX "Could not allocate raw audio frame\n" );
					break;
				}

				aud_frame->release_data = obe_release_audio_data;
				aud_frame->release_frame = obe_release_frame;
				aud_frame->audio_frame.num_samples = ctx->HancInfo.no_audio_samples / nAudioChannels;
				aud_frame->audio_frame.num_channels = nAudioChannels;
				aud_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;
				aud_frame->audio_frame.linesize = nAudioChannels * (16 /*bits */ / 8);
				aud_frame->input_stream_id = 1;

				/* Allocate a new sample buffer ready to hold S32P */
				if (av_samples_alloc(aud_frame->audio_frame.audio_data,
					&aud_frame->audio_frame.linesize,
					opts->num_channels,
					aud_frame->audio_frame.num_samples,
					(AVSampleFormat)aud_frame->audio_frame.sample_fmt, 0) < 0)
				{
					fprintf(stderr, MODULE_PREFIX "avsample alloc failed\n");
				}

				/* Convert input samples from S16 interleaved into S32P planer. */
				if (avresample_convert(ctx->avr,
					aud_frame->audio_frame.audio_data,
					aud_frame->audio_frame.linesize,
					aud_frame->audio_frame.num_samples,
					(uint8_t**)&pAudioSamples,
					0,
					aud_frame->audio_frame.num_samples) < 0)
				{
					fprintf(stderr, MODULE_PREFIX "sample format conversion failed\n");
				}

// MMM
//		int64_t pts = av_rescale_q(v4l2_ctx->a_counter++, v4l2_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
		//obe_clock_tick(v4l2_ctx->h, pts);
				pts = 0;
				aud_frame->pts = pts;

				/* AVFM */
				avfm_init(&aud_frame->avfm, AVFM_AUDIO_PCM);
				avfm_set_hw_status_mask(&aud_frame->avfm, 0);

				/* Remember that we drive everything in the pipeline from the audio clock. */
				avfm_set_pts_video(&aud_frame->avfm, pts);
				avfm_set_pts_audio(&aud_frame->avfm, pts);

				avfm_set_hw_received_time(&aud_frame->avfm);
				double dur = 27000000 / ((double)opts->timebase_den / (double)opts->timebase_num);
				avfm_set_video_interval_clk(&aud_frame->avfm, dur);
				//raw_frame->avfm.hw_audio_correction_clk = clock_offset;
				//avfm_dump(&raw_frame->avfm);

				if (add_to_filter_queue(ctx->h, aud_frame) < 0 ) {
					printf("Failed to add\n");
				}
			}

			if (frame)
				avcodec_free_frame(&frame);

			av_free_packet(&pkt);
		}
	}
	printf(MODULE_PREFIX "Video thread complete\n");

	free(pVideoBuffer);
	free(pVancBuffer);
	free(pHancBuffer);
	free(pAudioSamples);

	ctx->vthreadComplete = 1;
	pthread_exit(0);
	return 0;
}

static void close_device(bluefish_opts_t *opts)
{
	bluefish_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Closing card idx #%d\n", opts->card_idx);

	if (ctx->vthreadRunning) {
		ctx->vthreadTerminate = 1;
		while (!ctx->vthreadComplete)
			usleep(50 * 1000);
	}

	BailOut(ctx->hBvc);

	if (ctx->codec) {
		avcodec_close(ctx->codec);
		av_free(ctx->codec);
	}

	if (ctx->avr)
		avresample_free(&ctx->avr);

	printf(MODULE_PREFIX "Closed card idx #%d\n", opts->card_idx);
}

static int open_device(bluefish_opts_t *opts)
{
	printf(MODULE_PREFIX "%s()\n", __func__);

	bluefish_ctx_t *ctx = &opts->ctx;
	cout << MODULE_PREFIX << "BlueVelvetC version: " << bfcGetVersion() << endl;

	ctx->hBvc = bfcFactory();

	opts->card_idx++;
	printf(MODULE_PREFIX "Searching for card idx #%d\n", opts->card_idx);

	int deviceCount = 0;
	bfcEnumerate(ctx->hBvc, deviceCount);
	if (deviceCount <= 0) {
		fprintf(stderr, MODULE_PREFIX "No bluefish devices detected.\n");
		bfcDestroy(ctx->hBvc);
		ctx->hBvc = NULL;
		return -1;
	}

	if (BLUE_FAIL(bfcAttach(ctx->hBvc, opts->card_idx))) {
		fprintf(stderr, MODULE_PREFIX "Unable to attached to bluefish card #%d\n", opts->card_idx);
		bfcDestroy(ctx->hBvc);
		ctx->hBvc = NULL;
		return -1;
	}

	/* Query card and firmware type */
	ctx->iCardType = CRD_INVALID;
	bfcQueryCardType(ctx->hBvc, ctx->iCardType);

	/* Get the firmware type */
	unsigned int nValue = 0;
	bfcQueryCardProperty32(ctx->hBvc, EPOCH_GET_PRODUCT_ID, nValue);
	printf(MODULE_PREFIX "Product ID 0x%x / card type 0x%x\n", nValue, ctx->iCardType);

	InitInputChannel(ctx->hBvc, BLUE_VIDEO_INPUT_CHANNEL_A, UPD_FMT_FRAME,
		MEM_FMT_V210, VIDEO_ENGINE_FRAMESTORE, EPOCH_SRC_SDI_INPUT_B);

	/* Detect signal properties */
	int l = 0;
	while (l++ < 16) {
		usleep(100 * 1000);

		/* Get VID_FMT_INVALID flag; this enum has changed over time and might be
		 * different depending on which driver this application runs on.
		 */
		nValue = 0;
		bfcQueryCardProperty32(ctx->hBvc, INVALID_VIDEO_MODE_FLAG, nValue);
		unsigned int InvalidVideoModeFlag = nValue;

		/* Check if we have a valid input signal
		 * synchronise with the card before querying VIDEO_INPUT_SIGNAL_VIDEO_MODE
		 */
		bfcWaitVideoInputSync(ctx->hBvc, UPD_FMT_FRAME, ctx->LastFieldCount);

		nValue = 0;
		bfcQueryCardProperty32(ctx->hBvc, VIDEO_INPUT_SIGNAL_VIDEO_MODE, nValue);
		if (nValue >= InvalidVideoModeFlag) {
			printf(MODULE_PREFIX "No valid input signal on channel A\n");
			BailOut(ctx->hBvc);
			return 0;
		}
		ctx->VideoMode = (EVideoMode)nValue;
		break;
	}

	unsigned int nVideoWidth = 0;
	bfcGetPixelsPerLine(ctx->VideoMode, nVideoWidth);
	unsigned int nVideoHeight = 0;
	bfcGetLinesPerFrame(ctx->VideoMode, UPD_FMT_FRAME, nVideoHeight);

	/* Determine the overall size of the video frame. */
	bfcGetGoldenValue(ctx->VideoMode, MEM_FMT_V210, UPD_FMT_FRAME, ctx->frameSizeBytesVideo);

	/* We need to understand how much VANC we're going to be receiving. */
	bfcGetVANCGoldenValue(ctx->iCardType, ctx->VideoMode, MEM_FMT_V210, BLUE_DATA_FRAME, ctx->frameSizeBytesVanc);
	bfcGetVANCLineCount(ctx->iCardType, ctx->VideoMode, BLUE_DATA_FRAME, ctx->vancLineCount);
	ctx->frameSizeBytesHanc = BLUEFISH_HANC_BUFFER_SIZE;

	opts->width = nVideoWidth;
	opts->height = nVideoHeight;
	opts->interlaced = 0;
	opts->timebase_den = bfcUtilsGetFpsForVideoMode(ctx->VideoMode) * 1000;
	if (bfcUtilsIsVideoMode1001Framerate(ctx->VideoMode)) {
		opts->timebase_num = 1001;
	} else {
		opts->timebase_num = 1000;
	}
	opts->video_format = lookupOBEName(opts->width, opts->height, opts->timebase_den, opts->timebase_num);

	ctx->v_timebase.den = opts->timebase_den;
	ctx->v_timebase.num = opts->timebase_num;

	fprintf(stderr, MODULE_PREFIX "Detected resolution %dx%d @ %d/%d\n",
		opts->width, opts->height,
		opts->timebase_den, opts->timebase_num);

	avcodec_register_all();

	ctx->dec = avcodec_find_decoder(AV_CODEC_ID_V210);
	if (!ctx->dec) {
		fprintf(stderr, MODULE_PREFIX "Could not find v210 decoder\n");
	}

	ctx->codec = avcodec_alloc_context3(ctx->dec);
	if (!ctx->codec) {
		fprintf(stderr, MODULE_PREFIX "Could not allocate a codec context\n");
		return -1;
	}

	ctx->avr = avresample_alloc_context();
        if (!ctx->avr) {
            fprintf(stderr, MODULE_PREFIX "Unable to alloc avresample context\n");
            return -1;
        }

	/* Give libavresample our custom audio channel map */
	av_opt_set_int(ctx->avr, "in_channel_layout",   (1 << opts->num_channels) - 1, 0 );
	av_opt_set_int(ctx->avr, "in_sample_fmt",       AV_SAMPLE_FMT_S16, 0 );
	av_opt_set_int(ctx->avr, "in_sample_rate",      48000, 0 );
	av_opt_set_int(ctx->avr, "out_channel_layout",  (1 << opts->num_channels) - 1, 0 );
	av_opt_set_int(ctx->avr, "out_sample_fmt",      AV_SAMPLE_FMT_S32P, 0 );

	if (avresample_open(ctx->avr) < 0) {
		fprintf(stderr, MODULE_PREFIX "Could not configure avresample\n");
	}

	ctx->codec->get_buffer = obe_get_buffer;
	ctx->codec->release_buffer = obe_release_buffer;
	ctx->codec->reget_buffer = obe_reget_buffer;
	ctx->codec->flags |= CODEC_FLAG_EMU_EDGE;

	if (avcodec_open2(ctx->codec, ctx->dec, NULL) < 0) {
		fprintf(stderr, MODULE_PREFIX "Could not open libavcodec\n");
	}


	return 0; /* Success */
}

/* Called from open_input() */
static void close_thread(void *handle)
{
	printf(MODULE_PREFIX "%s()\n", __func__);

	if (!handle)
		return;

	bluefish_opts_t *opts = (bluefish_opts_t*)handle;
	close_device(opts);
	free(opts);
}

static void *bluefish_probe_stream(void *ptr)
{
	obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
	obe_t *h = probe_ctx->h;
	obe_input_t *user_opts = &probe_ctx->user_opts;
	obe_device_t *device;
	obe_int_input_stream_t *streams[MAX_STREAMS];
	int num_streams = 9;

	printf(MODULE_PREFIX "%s()\n", __func__);

	bluefish_ctx_t *ctx;

	bluefish_opts_t *opts = (bluefish_opts_t*)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to malloc opts\n");
		goto finish;
	}

	/* TODO: support multi-channel */
	opts->num_channels = 16;
	opts->card_idx = user_opts->card_idx;
	opts->video_format = user_opts->video_format;
	opts->probe = 1;

	ctx = &opts->ctx;
	ctx->h = h;

	/* Open device */
	if (open_device(opts) < 0) {
		fprintf(stderr, MODULE_PREFIX "Unable to open the device\n");
		goto finish;
	}

	sleep(1);

	close_device(opts);

	opts->probe_success = 1;
	fprintf(stderr, MODULE_PREFIX "Probe success\n" );

	if (!opts->probe_success) {
		fprintf(stderr, MODULE_PREFIX "No valid frames received - check connection and input format\n");
		goto finish;
	}


	/* TODO: probe for SMPTE 337M */
	/* TODO: factor some of the code below out */

	/* Create the video output stream and eight output audio pairs. */
	for (int i = 0; i < 9; i++) {

		streams[i] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[i]));
		if (!streams[i])
			goto finish;

		/* TODO: make it take a continuous set of stream-ids */
		pthread_mutex_lock(&h->device_list_mutex);
		streams[i]->input_stream_id = h->cur_input_stream_id++;
		pthread_mutex_unlock(&h->device_list_mutex);

		if (i == 0) {
			streams[i]->stream_type = STREAM_TYPE_VIDEO;
			streams[i]->stream_format = VIDEO_UNCOMPRESSED;
			streams[i]->width  = opts->width;
			streams[i]->height = opts->height;
			streams[i]->timebase_num = opts->timebase_num;
			streams[i]->timebase_den = opts->timebase_den;
			streams[i]->csp    = PIX_FMT_YUV422P10;
			streams[i]->interlaced = opts->interlaced;
			streams[i]->tff = 1; /* NTSC is bff in baseband but coded as tff */
			streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */
		}
		else if (i > 0) {
			streams[i]->stream_type = STREAM_TYPE_AUDIO;
			streams[i]->stream_format = AUDIO_PCM;
			streams[i]->num_channels  = 2;
			streams[i]->sample_format = AV_SAMPLE_FMT_S16;
			streams[i]->sample_rate = 48000;
			streams[i]->sdi_audio_pair = i;
		}
	}

	device = new_device();
	if (!device)
		goto finish;

	device->num_input_streams = num_streams;
	memcpy(device->input_streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**));
	device->device_type = INPUT_DEVICE_BLUEFISH;
	memcpy(&device->user_opts, user_opts, sizeof(*user_opts));

	/* add device */
	add_device(h, device);

finish:
	if (opts) {
		opts->probe = 0;
		free(opts);
	}

	free(probe_ctx);

	return NULL;
}

static void *bluefish_open_input(void *ptr)
{
	obe_input_params_t *input = (obe_input_params_t*)ptr;
	obe_t *h = input->h;
	obe_device_t *device = input->device;
	obe_input_t *user_opts = &device->user_opts;
	bluefish_ctx_t *ctx;

	bluefish_opts_t *opts = (bluefish_opts_t *)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to alloc context\n");
		return NULL;
	}

	pthread_cleanup_push(close_thread, (void *)opts);

	opts->num_channels = 16;
	opts->card_idx = user_opts->card_idx;
	opts->video_format = user_opts->video_format;

	ctx = &opts->ctx;

	ctx->device = device;
	ctx->h = h;
	ctx->v_counter = 0;

	if (open_device(opts) < 0)
		return NULL;

	pthread_create(&ctx->vthreadId, 0, bluefish_videoThreadFunc, opts);

	sleep(INT_MAX);

	pthread_cleanup_pop(1);

	return NULL;
}

const obe_input_func_t bluefish_input = { bluefish_probe_stream, bluefish_open_input };

#endif /* HAVE_BLUEDRIVER_P_H */
