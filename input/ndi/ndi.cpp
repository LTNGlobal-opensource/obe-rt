#if HAVE_PROCESSING_NDI_LIB_H

/*****************************************************************************
 * ndi.cpp: NDI Far frame grabber input module
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <Processing.NDI.Lib.h>

#pragma comment(lib, "Processing.NDI.Lib.x86.lib")

using namespace std;

// Other
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>

#define MODULE_PREFIX "[ndi-input]: "

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

struct obe_to_ndi_video
{
    int obe_name;
    int width, height;
    uint32_t reserved;
    int timebase_num;
    int timebase_den;
};

const static struct obe_to_ndi_video video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_720P_5994, 1280, 720, 0 /* bmdModeHD720p60 */, 1001, 60000 },
    { INPUT_VIDEO_FORMAT_720P_60,   1280, 720, 0 /* bmdModeHD720p60 */, 1000, 60000 },
};

static int lookupOBEName(int w, int h, int den, int num)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_ndi_video)); i++) {
		const struct obe_to_ndi_video *fmt = &video_format_tab[i];
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
	uint64_t v_counter;
	uint64_t a_counter;
	int	reset_v_pts;
	int	reset_a_pts;
	int64_t	clock_offset;


	/* NDI SDK related */
	NDIlib_recv_instance_t pNDI_recv;
	/* End: NDI SDK related */

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
} ndi_ctx_t;

typedef struct
{
    ndi_ctx_t ctx;

    /* Input */
    int card_idx;

	char *ndi_name;

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
} ndi_opts_t;

static void processFrameAudio(ndi_opts_t *opts, NDIlib_audio_frame_v2_t *frame)
{
	ndi_ctx_t *ctx = &opts->ctx;

	if (ctx->clock_offset == 0) {
	   return;
	}

#if 0
printf("sample_rate = %d\n", frame->sample_rate);
printf("no_channels = %d\n", frame->no_channels);
printf("no_samples = %d\n", frame->no_samples);
printf("channel_stride_in_bytes = %d\n", frame->channel_stride_in_bytes);
#endif

	/* Handle all of the Audio..... */
	/* NDI is planer, convert to S32 planer */
	obe_raw_frame_t *rf = new_raw_frame();
	if (!rf) {
		fprintf(stderr, MODULE_PREFIX "Could not allocate raw audio frame\n" );
		return;
	}

#if 0
// /storage/dev/ffmpeg/root/bin/ffmpeg -y -f f32le -ar 48k -ac 1 -i samples.flt file.wav
static FILE *fh = NULL;
if (fh == NULL)
	fh = fopen("samples.flt", "wb");

if (fh) {
	uint8_t *p = (uint8_t *)frame->p_data;
	fwrite(p, 1, 4800, fh);
}
	return;
#endif
	int depth = 32;

	NDIlib_audio_frame_interleaved_32s_t a_frame;
	a_frame.p_data = new int32_t[frame->no_samples * frame->no_channels];
	NDIlib_util_audio_to_interleaved_32s_v2(frame, &a_frame);

	int linesize = a_frame.no_channels * (depth /8);
	int stride  = a_frame.no_samples * (depth / 4);

	uint8_t *data = (uint8_t *)calloc(16, stride);
	memcpy(data, a_frame.p_data, a_frame.no_channels * stride);

	rf->release_data = obe_release_audio_data;
	rf->release_frame = obe_release_frame;
	rf->audio_frame.num_samples = a_frame.no_samples;
	rf->audio_frame.num_channels = 2;
	rf->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;
	rf->input_stream_id = 1;

	delete[] a_frame.p_data;

	/* Allocate a new sample buffer ready to hold S32P */
	if (av_samples_alloc(rf->audio_frame.audio_data,
		&rf->audio_frame.linesize,
		opts->num_channels,
		rf->audio_frame.num_samples,
		(AVSampleFormat)rf->audio_frame.sample_fmt, 0) < 0)
	{
		fprintf(stderr, MODULE_PREFIX "avsample alloc failed\n");
	}

#if 0
for (int h = 0; h < MAX_CHANNELS; h++)
  printf("data[%d] = %p\n", h, rf->audio_frame.audio_data[h]);

printf("new linesize = %d\n", rf->audio_frame.linesize);
#endif

	/* Convert input samples from S16 interleaved into S32P planer. */
	uint8_t *src[16] = { 0 };

	for (int x = 0; x < opts->num_channels; x++) {
		src[x] = data + (x * stride);
		//printf("src[%d] %p\n", x, src[x]);
	}

	int out_samples = avresample_convert(ctx->avr,
		rf->audio_frame.audio_data,
		rf->audio_frame.linesize,
		rf->audio_frame.num_samples,
		(uint8_t**)&src,
		0,
		rf->audio_frame.num_samples);
//printf("out_samples %d\n", out_samples);
	if (out_samples < 0) {
		fprintf(stderr, MODULE_PREFIX "sample format conversion failed\n");
	}

	free(data);
        if (ctx->reset_a_pts == 1) {
           int64_t timecode_pts = av_rescale_q(frame->timecode, (AVRational){1, 10000000}, (AVRational){1, OBE_CLOCK} );
	   timecode_pts = timecode_pts + ctx->clock_offset;
           int64_t pts_diff = av_rescale_q(1, (AVRational){frame->no_samples, frame->sample_rate}, (AVRational){1, OBE_CLOCK} );
           int q = timecode_pts / pts_diff + (timecode_pts % pts_diff > 0);
           ctx->a_counter = q;
           ctx->reset_a_pts = 0;
        }
// MMM
//		int64_t pts = av_rescale_q(v4l2_ctx->a_counter++, v4l2_ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
		//obe_clock_tick(v4l2_ctx->h, pts);
	int64_t pts = av_rescale_q(ctx->a_counter++, (AVRational){frame->no_samples, frame->sample_rate}, (AVRational){1, OBE_CLOCK} ); 
	rf->pts = pts;

	/* AVFM */
	avfm_init(&rf->avfm, AVFM_AUDIO_PCM);
	avfm_set_hw_status_mask(&rf->avfm, 0);

	/* Remember that we drive everything in the pipeline from the audio clock. */
	avfm_set_pts_video(&rf->avfm, pts);
	avfm_set_pts_audio(&rf->avfm, pts);

	avfm_set_hw_received_time(&rf->avfm);
	double dur = 27000000 / ((double)opts->timebase_den / (double)opts->timebase_num);
	avfm_set_video_interval_clk(&rf->avfm, dur);
	//raw_frame->avfm.hw_audio_correction_clk = clock_offset;
	//avfm_dump(&raw_frame->avfm);

	if (add_to_filter_queue(ctx->h, rf) < 0 ) {
		printf("Failed to add\n");
	}
}

static void processFrameVideo(ndi_opts_t *opts, NDIlib_video_frame_v2_t *frame)
{
	ndi_ctx_t *ctx = &opts->ctx;

#if 0
printf("%s()\n", __func__);
printf("line_stride_in_bytes = %d\n", frame->line_stride_in_bytes);
static int64_t lastTimestamp = 0;
printf("timestamp = %" PRIi64 " (%" PRIi64 ")\n",
	frame->timestamp,
	frame->timestamp - lastTimestamp);

lastTimestamp = frame->timestamp;
#endif

	obe_raw_frame_t *rf = new_raw_frame();
	if (!rf) {
		fprintf(stderr, MODULE_PREFIX "Could not allocate raw video frame\n");
		return;
	}

	/* TODO: YUYV only. Drivers will non YUYV colorspaces won't work reliably. */
	rf->alloc_img.csp = (int)AV_PIX_FMT_YUV420P;
	rf->alloc_img.format = opts->video_format;
	rf->alloc_img.width = opts->width;
	rf->alloc_img.height = opts->height;
	rf->alloc_img.first_line = 1;
	rf->alloc_img.planes = av_pix_fmt_descriptors[rf->alloc_img.csp].nb_components;

	rf->alloc_img.stride[0] = opts->width;
	rf->alloc_img.stride[1] = opts->width / 2;
	rf->alloc_img.stride[2] = opts->width / 2;
	rf->alloc_img.stride[3] = 0;

	if (ctx->v_counter == 0) {
	   int64_t timecode_pts = av_rescale_q(frame->timecode, (AVRational){1, 10000000}, (AVRational){1, OBE_CLOCK} );
	   ctx->clock_offset = (timecode_pts * -1);
	   printf("Clock offset established as %" PRIi64 "\n", ctx->clock_offset);
	}
	if (ctx->reset_v_pts == 1) {
	   int64_t timecode_pts = av_rescale_q(frame->timecode, (AVRational){1, 10000000}, (AVRational){1, OBE_CLOCK});
	   timecode_pts = timecode_pts + ctx->clock_offset;
	   int64_t pts_diff = av_rescale_q(1, ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
	   int q = timecode_pts / pts_diff + (timecode_pts % pts_diff > 0);
	   printf("Q is %d\n",q);
	   ctx->v_counter = q;
	   ctx->reset_v_pts = 0;
	}

	int64_t pts = av_rescale_q(ctx->v_counter++, ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
	obe_clock_tick(ctx->h, pts);
	rf->pts = pts;

//printf("pts = %lld\n", raw_frame->pts);
	rf->timebase_num = opts->timebase_num;
	rf->timebase_den = opts->timebase_den;

	rf->alloc_img.plane[0] = (uint8_t *)calloc(1, opts->width * opts->height * 2);
	rf->alloc_img.plane[1] = rf->alloc_img.plane[0] + (opts->width * opts->height);
	rf->alloc_img.plane[2] = rf->alloc_img.plane[1] + ((opts->width * opts->height) / 4);
	rf->alloc_img.plane[3] = 0;
	memcpy(&rf->img, &rf->alloc_img, sizeof(rf->alloc_img));

	rf->release_data = obe_release_video_data;
	rf->release_frame = obe_release_frame;

	/* Convert UYVY to I420. */
	libyuv::UYVYToI420(frame->p_data,
		opts->width * 2,
		rf->alloc_img.plane[0], opts->width,
		rf->alloc_img.plane[1], opts->width / 2,
		rf->alloc_img.plane[2], opts->width / 2,
		opts->width, opts->height);

	/* AVFM */
	avfm_init(&rf->avfm, AVFM_VIDEO);
	avfm_set_hw_status_mask(&rf->avfm, AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_FULL);

	/* Remember that we drive everything in the pipeline from the audio clock. */
	avfm_set_pts_video(&rf->avfm, pts);
	avfm_set_pts_audio(&rf->avfm, pts);

	avfm_set_hw_received_time(&rf->avfm);
	double dur = 27000000 / ((double)opts->timebase_den / (double)opts->timebase_num);
	avfm_set_video_interval_clk(&rf->avfm, dur);
	//raw_frame->avfm.hw_audio_correction_clk = clock_offset;
	//avfm_dump(&raw_frame->avfm);

	if (add_to_filter_queue(ctx->h, rf) < 0 ) {
	}
}

static void *ndi_videoThreadFunc(void *p)
{
	ndi_opts_t *opts = (ndi_opts_t *)p;
	ndi_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Video thread starts\n");

	ctx->v_counter = 0;
	ctx->a_counter = 0;
	ctx->clock_offset = 0;
	ctx->vthreadRunning = 1;
	ctx->vthreadComplete = 0;
	ctx->vthreadTerminate = 0;

	while (!ctx->vthreadTerminate && opts->probe == 0) {

		NDIlib_video_frame_v2_t video_frame;
		NDIlib_audio_frame_v2_t audio_frame;
                NDIlib_metadata_frame_t metadata;
		NDIlib_tally_t tally (true);
		NDIlib_recv_set_tally(ctx->pNDI_recv, &tally);

		int timeout = av_rescale_q(1000, ctx->v_timebase, (AVRational){1, 1});
		timeout = timeout + 40;

		switch (NDIlib_recv_capture_v2(ctx->pNDI_recv, &video_frame, &audio_frame, &metadata, timeout)) {
			case NDIlib_frame_type_video:
				processFrameVideo(opts, &video_frame);
				NDIlib_recv_free_video_v2(ctx->pNDI_recv, &video_frame);
				break;
			case NDIlib_frame_type_audio:
				processFrameAudio(opts, &audio_frame);
				NDIlib_recv_free_audio_v2(ctx->pNDI_recv, &audio_frame);
				break;
                        case NDIlib_frame_type_metadata:
                                printf("Metadata content: %s\n", metadata.p_data);
                                NDIlib_recv_free_metadata(ctx->pNDI_recv, &metadata);
                                break;
			case NDIlib_frame_type_none:
				printf("Frame time exceeded timeout of: %d\n", timeout);
				ctx->reset_v_pts = 1;
				ctx->reset_a_pts = 1;
			default:
				printf("no frame?\n");
		}

#if 0

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
#endif
	}
	printf(MODULE_PREFIX "Video thread complete\n");

	ctx->vthreadComplete = 1;
	pthread_exit(0);
	return 0;
}

static void close_device(ndi_opts_t *opts)
{
	ndi_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Closing card idx #%d\n", opts->card_idx);

	if (ctx->vthreadRunning) {
		ctx->vthreadTerminate = 1;
		while (!ctx->vthreadComplete)
			usleep(50 * 1000);
	}

	if (ctx->codec) {
		avcodec_close(ctx->codec);
		av_free(ctx->codec);
	}

	if (ctx->avr)
		avresample_free(&ctx->avr);

	NDIlib_destroy();

	printf(MODULE_PREFIX "Closed card idx #%d\n", opts->card_idx);
}

static int open_device(ndi_opts_t *opts)
{
	printf(MODULE_PREFIX "%s()\n", __func__);

	ndi_ctx_t *ctx = &opts->ctx;
	printf(MODULE_PREFIX "NDI version: %s\n", NDIlib_version());

	if (!NDIlib_initialize()) {
		fprintf(stderr, MODULE_PREFIX "Unable to initialize NDIlib\n");
		return -1;
	}

	NDIlib_find_instance_t pNDI_find = NDIlib_find_create_v2();
	if (!pNDI_find) {
		fprintf(stderr, MODULE_PREFIX "Unable to initialize NDIlib finder\n");
		return -1;
	}

	// We now have at least one source, so we create a receiver to look at it.
	ctx->pNDI_recv = NDIlib_recv_create_v3();
	if (!ctx->pNDI_recv) {
		fprintf(stderr, MODULE_PREFIX "Unable to create v3 receiver\n");
		return -1;
	}


	if (opts->ndi_name != NULL) {
		printf("NDI Source name: %s\n", opts->ndi_name);
		//We where passed a source name, so we connect to it directly!
		const NDIlib_source_t p_source (opts->ndi_name);
		NDIlib_recv_connect(ctx->pNDI_recv, &p_source);
	} else {
		opts->card_idx++;
		printf(MODULE_PREFIX "Searching for card idx #%d\n", opts->card_idx);
		const NDIlib_source_t *p_sources = NULL;
		uint32_t sourceCount = 0;
		int i = 0;
		while (sourceCount == 0) {
			if (i++ >= 10) {
				fprintf(stderr, MODULE_PREFIX "No NDI sources detected\n");
				return -1;
			}
			NDIlib_find_wait_for_sources(pNDI_find, 500 /* ms */);
			p_sources = NDIlib_find_get_current_sources(pNDI_find, &sourceCount);
		}

		for (uint32_t x = 0; x < sourceCount; x++) {
			printf("Found[%d] %s @ %s\n", x,
				p_sources[x].p_ndi_name,
				p_sources[x].p_url_address);
		}
	        printf("p_sources %p\n", p_sources);
                NDIlib_recv_connect(ctx->pNDI_recv, p_sources + 0);
	}

	/* We don't need this */
	NDIlib_find_destroy(pNDI_find);

	/* Detect signal properties */
	int l = 0;
	while (++l) {

		if (opts->width && l > 32)
			break;

		/* Grab a frame of video and audio, probe it. */
		NDIlib_video_frame_v2_t video_frame;
		NDIlib_audio_frame_v2_t audio_frame;

		switch (NDIlib_recv_capture_v2(ctx->pNDI_recv, &video_frame, &audio_frame, NULL, 5000)) {
			case NDIlib_frame_type_video:
#if 0
printf("line_stride_in_bytes = %d\n", video_frame.line_stride_in_bytes);
static int64_t lastTimestamp = 0;
printf("timestamp = %" PRIi64 " (%" PRIi64 ")\n",
	video_frame.timestamp,
	video_frame.timestamp - lastTimestamp);

lastTimestamp = video_frame.timestamp;
#endif
				opts->width = video_frame.xres;
				opts->height = video_frame.yres;
				opts->timebase_num = video_frame.frame_rate_D;
				opts->timebase_den = video_frame.frame_rate_N;

				if (video_frame.frame_format_type == NDIlib_frame_format_type_progressive)
					opts->interlaced = 0;
				else
					opts->interlaced = 1;
#if 0
				printf("Detected fourCC 0x%x\n", video_frame.FourCC);
				if (video_frame.FourCC == NDIlib_FourCC_type_UYVY) {
					printf("UYVY\n");
				} else {
					printf("CS unknown\n");
				}
#endif

				NDIlib_recv_free_video_v2(ctx->pNDI_recv, &video_frame);
				break;
			case NDIlib_frame_type_audio:
#if 0
printf("Detected audio\n");
printf("sample_rate = %d\n", audio_frame.sample_rate);
printf("no_channels = %d\n", audio_frame.no_channels);
printf("no_samples = %d\n", audio_frame.no_samples);
printf("channel_stride_in_bytes = %d\n", audio_frame.channel_stride_in_bytes);
#endif
				opts->num_channels = audio_frame.no_channels;
				NDIlib_recv_free_audio_v2(ctx->pNDI_recv, &audio_frame);
				break;
			default:
				printf("other\n");
		}
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
	}

	ctx->avr = avresample_alloc_context();
        if (!ctx->avr) {
            fprintf(stderr, MODULE_PREFIX "Unable to alloc avresample context\n");
        }

	/* Give libavresample our custom audio channel map */
	//av_opt_set_int(ctx->avr, "in_channel_layout",   (1 << opts->num_channels) - 1, 0 );
	av_opt_set_int(ctx->avr, "in_channel_layout",   (1 << opts->num_channels) - 1, 0 );
	av_opt_set_int(ctx->avr, "in_sample_fmt",       AV_SAMPLE_FMT_S32, 0 );
	av_opt_set_int(ctx->avr, "in_sample_rate",      48000, 0 );
	av_opt_set_int(ctx->avr, "out_channel_layout",  (1 << opts->num_channels) - 1, 0 );
	av_opt_set_int(ctx->avr, "out_sample_fmt",      AV_SAMPLE_FMT_S32P, 0 );
	av_opt_set_int(ctx->avr, "out_sample_rate",     48000, 0 );

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

	ndi_opts_t *opts = (ndi_opts_t*)handle;
	close_device(opts);
	free(opts);
}

static void *ndi_probe_stream(void *ptr)
{
	obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
	obe_t *h = probe_ctx->h;
	obe_input_t *user_opts = &probe_ctx->user_opts;
	obe_device_t *device;
	obe_int_input_stream_t *streams[MAX_STREAMS];
	int num_streams = 2;

	printf(MODULE_PREFIX "%s()\n", __func__);

	ndi_ctx_t *ctx;

	ndi_opts_t *opts = (ndi_opts_t*)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to malloc opts\n");
		goto finish;
	}

	opts->card_idx = user_opts->card_idx;
	opts->video_format = user_opts->video_format;
	opts->ndi_name = user_opts->ndi_name;
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

	//Split audio in seperate stereo tracks
	//Todo: allow user to specify desired channel count per pair
	num_streams  = opts->num_channels / 2 + 1;


	/* TODO: probe for SMPTE 337M */
	/* TODO: factor some of the code below out */

	/* Create the video output stream and eight output audio pairs. */
	for (int i = 0; i < num_streams; i++) {

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
			streams[i]->csp    = PIX_FMT_UYVY422;
			streams[i]->interlaced = opts->interlaced;
			streams[i]->tff = 1; /* NTSC is bff in baseband but coded as tff */
			streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */
		}
		else if (i > 0) {
			printf("loop count = %d\n",i);
			streams[i]->stream_type = STREAM_TYPE_AUDIO;
			streams[i]->stream_format = AUDIO_PCM;
			streams[i]->num_channels  = 2;
			streams[i]->sample_format = AV_SAMPLE_FMT_S32P;
			streams[i]->sample_rate = 48000;
			streams[i]->sdi_audio_pair = i;
		}
	}

	device = new_device();
	if (!device)
		goto finish;

	device->num_input_streams = num_streams;
	memcpy(device->input_streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**));
	device->device_type = INPUT_DEVICE_NDI;
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

static void *ndi_open_input(void *ptr)
{
	obe_input_params_t *input = (obe_input_params_t*)ptr;
	obe_t *h = input->h;
	obe_device_t *device = input->device;
	obe_input_t *user_opts = &device->user_opts;
	ndi_ctx_t *ctx;

	ndi_opts_t *opts = (ndi_opts_t *)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to alloc context\n");
		return NULL;
	}

	pthread_cleanup_push(close_thread, (void *)opts);

	opts->card_idx = user_opts->card_idx;
	opts->video_format = user_opts->video_format;
	opts->ndi_name = user_opts->ndi_name;

	ctx = &opts->ctx;

	ctx->device = device;
	ctx->h = h;
	ctx->v_counter = 0;
	ctx->a_counter = 0;
        ctx->clock_offset = 0;

	if (open_device(opts) < 0)
		return NULL;

	pthread_create(&ctx->vthreadId, 0, ndi_videoThreadFunc, opts);

	sleep(INT_MAX);

	pthread_cleanup_pop(1);

	return NULL;
}

const obe_input_func_t ndi_input = { ndi_probe_stream, ndi_open_input };

#endif /* HAVE_PROCESSING_NDI_LIB_H */
