/*****************************************************************************
 * v210.cpp: V210 data from disk input module
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include <signal.h>

using namespace std;

// Other
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>

#define MODULE_PREFIX "[v210]: "

extern "C"
{
#include "common/common.h"
#include "common/lavc.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include "input/sdi/ancillary.h"
#include "input/sdi/vbi.h"
#include "input/sdi/x86/sdi.h"
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/bswap.h>
#include <libavutil/opt.h>
}

struct obe_to_v210_video
{
    int obe_name;
    int width, height;
    uint32_t reserved;
    int timebase_num;
    int timebase_den;
};

const static struct obe_to_v210_video video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_720P_5994, 1280, 720, 0 /* bmdModeHD720p60 */, 1001, 60000 },
    { INPUT_VIDEO_FORMAT_720P_60,   1280, 720, 0 /* bmdModeHD720p60 */, 1000, 60000 },
};

static int lookupOBEName(int w, int h, int den, int num)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_v210_video)); i++) {
		const struct obe_to_v210_video *fmt = &video_format_tab[i];
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
	/* V210 input related */
	int v210_fd;
	uint8_t *v210_addr;
	int64_t v210_file_size;
	unsigned int frameSizeBytesVideo;
	unsigned int totalInputFrames;
	unsigned int currentFrame;
	uint64_t v_counter;

	/* AVCodec for V210 conversion. */
	AVCodec         *dec;
	AVCodecContext  *codec;
	/* End: AVCodec for V210 conversion. */

	pthread_t vthreadId;
	int vthreadTerminate, vthreadRunning, vthreadComplete;

	obe_device_t *device;
	obe_t *h;

	AVRational   v_timebase;
} v210_ctx_t;

typedef struct
{
    v210_ctx_t ctx;

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
} v210_opts_t;

static uint8_t *getNextFrameAddress(v210_opts_t *opts)
{
	v210_ctx_t *ctx = &opts->ctx;

	if (ctx->currentFrame + 1 >= ctx->totalInputFrames) {
		ctx->currentFrame = 0;
	} else
		ctx->currentFrame++;

	return ctx->v210_addr + (ctx->currentFrame * ctx->frameSizeBytesVideo);
}

static void *v210_videoThreadFunc(void *p)
{
	v210_opts_t *opts = (v210_opts_t *)p;
	v210_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Video thread starts\n");

	ctx->vthreadRunning = 1;
	ctx->vthreadComplete = 0;
	ctx->vthreadTerminate = 0;

	while (!ctx->vthreadTerminate && opts->probe == 0) {

		/* Ship the payload into the OBE pipeline. */
		obe_raw_frame_t *raw_frame = new_raw_frame();
		if (!raw_frame) {
			fprintf(stderr, MODULE_PREFIX "Could not allocate raw video frame\n");
			break;
		}

		AVFrame *frame = av_frame_alloc();
		ctx->codec->width = opts->width;
		ctx->codec->height = opts->height;

		usleep(166 * 100);

		AVPacket pkt;
		av_init_packet(&pkt);

		pkt.data = getNextFrameAddress(opts);
		pkt.size = ctx->frameSizeBytesVideo;

		int ret = avcodec_send_packet(ctx->codec, &pkt);
		while (ret >= 0) {
			ret = avcodec_receive_frame(ctx->codec, frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				return NULL;
			else if (ret < 0) {
			}
			break;
		}

		raw_frame->release_data = obe_release_video_data;
		raw_frame->release_frame = obe_release_frame;

		memcpy(raw_frame->alloc_img.stride, frame->linesize, sizeof(raw_frame->alloc_img.stride));
		memcpy(raw_frame->alloc_img.plane, frame->data, sizeof(raw_frame->alloc_img.plane));
		av_frame_free(&frame);
		raw_frame->alloc_img.csp = ctx->codec->pix_fmt;
                const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(raw_frame->alloc_img.csp);
		raw_frame->alloc_img.planes = d->nb_components;
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
		double dur = 27000000 / (opts->timebase_den / opts->timebase_num);
		avfm_set_video_interval_clk(&raw_frame->avfm, dur);
		//raw_frame->avfm.hw_audio_correction_clk = clock_offset;
			//avfm_dump(&raw_frame->avfm);

		if (add_to_filter_queue(ctx->h, raw_frame) < 0 ) {
		}

		if (frame)
			av_frame_free(&frame);

		av_packet_unref(&pkt);
	}
	printf(MODULE_PREFIX "Video thread complete\n");

	ctx->vthreadComplete = 1;
	pthread_exit(0);
	return 0;
}

static void close_device(v210_opts_t *opts)
{
	v210_ctx_t *ctx = &opts->ctx;

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

	munmap(ctx->v210_addr, ctx->v210_file_size);
	close(ctx->v210_fd);
	ctx->v210_fd = -1;

	printf(MODULE_PREFIX "Closed card idx #%d\n", opts->card_idx);
}

static int open_device(v210_opts_t *opts)
{
	printf(MODULE_PREFIX "%s()\n", __func__);

	v210_ctx_t *ctx = &opts->ctx;

	char fn[64];
	snprintf(fn, sizeof(fn), "../../raw-input%d.v210", opts->card_idx);
	printf(MODULE_PREFIX "Searching for V210 filename '%s'\n", fn);

	ctx->v210_fd = open(fn,
#if defined(__linux__)
		O_LARGEFILE,
#else
		0,
#endif
		0600); //6 = read+write for me!
	if (ctx->v210_fd < 0) {
		fprintf(stderr, MODULE_PREFIX "No input filename '%s' detected.\n", fn);
		snprintf(fn, sizeof(fn), "raw-input%d.v210", opts->card_idx);
		printf(MODULE_PREFIX "Searching for V210 filename '%s'\n", fn);
		ctx->v210_fd = open(fn,
#if defined(__linux__)
			O_LARGEFILE,
#else
			0,
#endif
			0600); //6 = read+write for me!
		if (ctx->v210_fd < 0) {
			fprintf(stderr, MODULE_PREFIX "No input filename '%s' detected.\n", fn);
			return -1;
		}
	}

	struct stat buf;
	if (fstat(ctx->v210_fd, &buf) < 0) {
		close(ctx->v210_fd);
		ctx->v210_fd = -1;
		fprintf(stderr, MODULE_PREFIX "Unable to obtain file size.\n");
		return -1;
	}

	ctx->v210_file_size = buf.st_size;

	/* Determine the overall size of the video frame. */
	ctx->frameSizeBytesVideo = 3456 * 720;
	ctx->totalInputFrames = ctx->v210_file_size / ctx->frameSizeBytesVideo;

	ctx->v210_addr = (uint8_t *)mmap(NULL, ctx->v210_file_size, PROT_READ, MAP_SHARED, ctx->v210_fd, 0);
	printf("Mapped %" PRIi64 " bytes at %p, %" PRIu64" frames.\n",
		ctx->v210_file_size, ctx->v210_addr,
		ctx->v210_file_size / ctx->frameSizeBytesVideo);

	if (ctx->v210_addr == (void*)-1) {
		fprintf(stderr, MODULE_PREFIX "Unable to MMAP file.\n");
		perror("mmap");
		close(ctx->v210_fd);
		ctx->v210_fd = -1;
		return -1;
	}

	/* We need to understand how much VANC we're going to be receiving. */
	opts->width = 1280;
	opts->height = 720;
	opts->interlaced = 0;
	opts->timebase_den = 60000;
	opts->timebase_num = 1001;
	opts->video_format = lookupOBEName(opts->width, opts->height, opts->timebase_den, opts->timebase_num);

	ctx->v_timebase.den = opts->timebase_den;
	ctx->v_timebase.num = opts->timebase_num;

	fprintf(stderr, MODULE_PREFIX "Detected resolution %dx%d @ %d/%d\n",
		opts->width, opts->height,
		opts->timebase_den, opts->timebase_num);

	ctx->dec = avcodec_find_decoder(AV_CODEC_ID_V210);
	if (!ctx->dec) {
		fprintf(stderr, MODULE_PREFIX "Could not find v210 decoder\n");
	}

	ctx->codec = avcodec_alloc_context3(ctx->dec);
	if (!ctx->codec) {
		fprintf(stderr, MODULE_PREFIX "Could not allocate a codec context\n");
	}

#if 0
	ctx->codec->get_buffer = obe_get_buffer;
	ctx->codec->release_buffer = obe_release_buffer;
	ctx->codec->reget_buffer = obe_reget_buffer;
	ctx->codec->flags |= CODEC_FLAG_EMU_EDGE;
#endif

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

	v210_opts_t *opts = (v210_opts_t*)handle;
	close_device(opts);
	free(opts);
}

static void *v210_probe_stream(void *ptr)
{
	obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
	obe_t *h = probe_ctx->h;
	obe_input_t *user_opts = &probe_ctx->user_opts;
	obe_device_t *device;
	obe_int_input_stream_t *streams[MAX_STREAMS];
	int num_streams = 2;

	printf(MODULE_PREFIX "%s()\n", __func__);

	v210_ctx_t *ctx;

	v210_opts_t *opts = (v210_opts_t*)calloc(1, sizeof(*opts));
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
		fprintf(stderr, MODULE_PREFIX "Unable to open the V210 input file.\n");
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

	for( int i = 0; i < 2; i++ ) {

		streams[i] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[i]) );
		if (!streams[i])
			goto finish;

		/* TODO: make it take a continuous set of stream-ids */
		pthread_mutex_lock( &h->device_list_mutex );
		streams[i]->input_stream_id = h->cur_input_stream_id++;
		pthread_mutex_unlock( &h->device_list_mutex );

		if (i == 0) {
			streams[i]->stream_type = STREAM_TYPE_VIDEO;
			streams[i]->stream_format = VIDEO_UNCOMPRESSED;
			streams[i]->width  = opts->width;
			streams[i]->height = opts->height;
			streams[i]->timebase_num = opts->timebase_num;
			streams[i]->timebase_den = opts->timebase_den;
			streams[i]->csp    = AV_PIX_FMT_YUV422P10;
			streams[i]->interlaced = opts->interlaced;
			streams[i]->tff = 1; /* NTSC is bff in baseband but coded as tff */
			streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */
		}
		else if( i == 1 ) {
			/* TODO: various v4l2 assumptions about audio being 48KHz need resolved.
         		 * Some sources could be 44.1 and this module will fall down badly.
			 */
			streams[i]->stream_type = STREAM_TYPE_AUDIO;
			streams[i]->stream_format = AUDIO_PCM;
			streams[i]->num_channels  = 2;
			streams[i]->sample_format = AV_SAMPLE_FMT_S16;
			streams[i]->sample_rate = 48000;
		}
	}

	device = new_device();
	if (!device)
		goto finish;

	device->num_input_streams = num_streams;
	memcpy(device->input_streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**));
	device->device_type = INPUT_DEVICE_V210;
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

static void *v210_open_input(void *ptr)
{
	obe_input_params_t *input = (obe_input_params_t*)ptr;
	obe_t *h = input->h;
	obe_device_t *device = input->device;
	obe_input_t *user_opts = &device->user_opts;
	v210_ctx_t *ctx;

	v210_opts_t *opts = (v210_opts_t *)calloc(1, sizeof(*opts));
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

	pthread_create(&ctx->vthreadId, 0, v210_videoThreadFunc, opts);

	sleep(INT_MAX);

	pthread_cleanup_pop(1);

	return NULL;
}

const obe_input_func_t v210_input = { v210_probe_stream, v210_open_input };
