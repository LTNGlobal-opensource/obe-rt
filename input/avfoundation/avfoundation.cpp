#if defined(__APPLE__)

/*****************************************************************************
 * avfoundation.cpp: AVFoundation frame grabber input module
 *****************************************************************************
 * Copyright (C) 2020 LTN
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
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace std;

// Other
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>

#define MODULE_PREFIX "[avfoundation-input]: "

#define LOCAL_DEBUG 1

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
#include <libyuv/convert.h>
}

struct obe_to_avfoundation_video
{
    int obe_name;
    int width, height;
    uint32_t reserved;
    int timebase_num;
    int timebase_den;
};

const static struct obe_to_avfoundation_video video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_720P_2997, 1280, 720, 0 /* bmdModeHD720p60 */, 1001, 30000 },
};

static int lookupOBEName(int w, int h, int den, int num)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_avfoundation_video)); i++) {
		const struct obe_to_avfoundation_video *fmt = &video_format_tab[i];
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


	/* AVCodec for V210 conversion. */
	AVCodec         *dec;
	AVCodecContext  *codec;
	/* End: AVCodec for V210 conversion. */

	struct SwrContext *avr;

	pthread_t vthreadId;
	int vthreadTerminate, vthreadRunning, vthreadComplete;

	obe_device_t *device;
	obe_t *h;

	AVRational   v_timebase;
} avfoundation_ctx_t;

typedef struct
{
    avfoundation_ctx_t ctx;

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
} avfoundation_opts_t;

static void *avfoundation_videoThreadFunc(void *p)
{
	avfoundation_opts_t *opts = (avfoundation_opts_t *)p;
	avfoundation_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Video thread starts\n");

	ctx->v_counter = 0;
	ctx->a_counter = 0;
	ctx->clock_offset = 0;
	ctx->vthreadRunning = 1;
	ctx->vthreadComplete = 0;
	ctx->vthreadTerminate = 0;

	while (!ctx->vthreadTerminate && opts->probe == 0) {

		int timeout = av_rescale_q(1000, ctx->v_timebase, (AVRational){1, 1});
		timeout = timeout + 40;

		/* Get a frame and process it */
		sleep(1);

	}
	printf(MODULE_PREFIX "Video thread complete\n");

	ctx->vthreadComplete = 1;
	pthread_exit(0);
	return 0;
}

static void close_device(avfoundation_opts_t *opts)
{
	avfoundation_ctx_t *ctx = &opts->ctx;

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
		swr_free(&ctx->avr);

	printf(MODULE_PREFIX "Closed card idx #%d\n", opts->card_idx);
}

static int open_device(avfoundation_opts_t *opts)
{
	printf(MODULE_PREFIX "%s()\n", __func__);

	avfoundation_ctx_t *ctx = &opts->ctx;

	opts->video_format = lookupOBEName(1280, 720, 30000, 1001);
	opts->width = 1280;
	opts->height = 720;
	opts->interlaced = 0;
	opts->timebase_den = 30000;
	opts->timebase_num = 1001;
	opts->num_channels = 2;

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
		return -1;
	}

	ctx->avr = swr_alloc();
        if (!ctx->avr) {
            fprintf(stderr, MODULE_PREFIX "Unable to alloc libswresample context\n");
            return -1;
        }

	/* Give libavresample our custom audio channel map */
	printf(MODULE_PREFIX "audio num_channels detected = %d\n", opts->num_channels);
	av_opt_set_int(ctx->avr, "in_channel_layout",   (1 << opts->num_channels) - 1, 0 );
	av_opt_set_int(ctx->avr, "in_sample_fmt",       AV_SAMPLE_FMT_S32, 0 );
	av_opt_set_int(ctx->avr, "in_sample_rate",      48000, 0 );
	av_opt_set_int(ctx->avr, "out_channel_layout",  (1 << opts->num_channels) - 1, 0 );
	av_opt_set_int(ctx->avr, "out_sample_fmt",      AV_SAMPLE_FMT_S32P, 0 );
	av_opt_set_int(ctx->avr, "out_sample_rate",     48000, 0 );

	if (swr_init(ctx->avr) < 0) {
		fprintf(stderr, MODULE_PREFIX "Could not configure libswresample\n");
	}

	ctx->codec->get_buffer2 = obe_get_buffer2;
#if 0
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

	avfoundation_opts_t *opts = (avfoundation_opts_t*)handle;
	close_device(opts);
	free(opts);
}

static void *avfoundation_probe_stream(void *ptr)
{
	obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
	obe_t *h = probe_ctx->h;
	obe_input_t *user_opts = &probe_ctx->user_opts;
	obe_device_t *device;
	obe_int_input_stream_t *streams[MAX_STREAMS];
	int num_streams = 2;

	printf(MODULE_PREFIX "%s()\n", __func__);

	avfoundation_ctx_t *ctx;

	avfoundation_opts_t *opts = (avfoundation_opts_t*)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to malloc opts\n");
		goto finish;
	}

	opts->card_idx = user_opts->card_idx;
	opts->video_format = user_opts->video_format;
	opts->ndi_name = user_opts->name;
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
			streams[i]->csp    = AV_PIX_FMT_UYVY422;
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
	device->device_type = INPUT_DEVICE_AVFOUNDATION;
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

static void *avfoundation_open_input(void *ptr)
{
#if LOCAL_DEBUG
	printf("%s()\n", __func__);
#endif
	obe_input_params_t *input = (obe_input_params_t*)ptr;
	obe_t *h = input->h;
	obe_device_t *device = input->device;
	obe_input_t *user_opts = &device->user_opts;
	avfoundation_ctx_t *ctx;

	avfoundation_opts_t *opts = (avfoundation_opts_t *)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to alloc context\n");
		return NULL;
	}

	pthread_cleanup_push(close_thread, (void *)opts);

	opts->card_idx = user_opts->card_idx;
	opts->video_format = user_opts->video_format;
	opts->ndi_name = user_opts->name;

	ctx = &opts->ctx;

	ctx->device = device;
	ctx->h = h;
	ctx->v_counter = 0;
	ctx->a_counter = 0;
        ctx->clock_offset = 0;

	if (open_device(opts) < 0)
		return NULL;

	pthread_create(&ctx->vthreadId, 0, avfoundation_videoThreadFunc, opts);

	sleep(INT_MAX);

	pthread_cleanup_pop(1);

	return NULL;
}

const obe_input_func_t avfoundation_input = { avfoundation_probe_stream, avfoundation_open_input };

#endif /* #if defined(__APPLE__) */
