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

#define HALF_FRAME_RATE 0

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <Processing.NDI.Lib.h>

using namespace std;

// Other
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>

#define MODULE_PREFIX "[ndi-input]: "

#define TEAMS_16KHZ 0

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
    { INPUT_VIDEO_FORMAT_720P_5994,  1280, 720,  0 /* bmdModeHD720p60 */, 1001, 60000 },
    { INPUT_VIDEO_FORMAT_720P_60,    1280, 720,  0 /* bmdModeHD720p60 */, 1000, 60000 },
    { INPUT_VIDEO_FORMAT_1080P_2997, 1920, 1080, 0 /* bmdModeHD720p60 */, 1001, 30000 },
    { INPUT_VIDEO_FORMAT_1080P_5994, 1920, 1080, 0 /* bmdModeHD720p60 */, 1001, 60000 },
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
	const NDIlib_v3* p_NDILib;
	NDIlib_recv_instance_t pNDI_recv;
	struct timeval lastVideoFrameTime;
	struct timeval currVideoFrameTime;
	int64_t videoFrameIntervalMs;
	/* End: NDI SDK related */

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
} ndi_ctx_t;

typedef struct
{
    ndi_ctx_t ctx;

    /* Input */
    int card_idx;

    char *ndi_name;

    int video_format;
    int audio_channel_count;
    int audio_channel_samplerate;

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

#if 0
	int from = frame->no_samples * 2;
	int to = from + 20;
	for (int x = from - 2; x < to; x++) {
		printf("%08f ", frame->p_data[x]);
	}
	printf("\n");
#endif

static void processFrameAudio(ndi_opts_t *opts, NDIlib_audio_frame_v2_t *frame)
{
	ndi_ctx_t *ctx = &opts->ctx;

#if 0
	static time_t audioin_when = 0;
	time_t now = time(NULL);

	if (audioin_when != now) {
		audioin_when = now;
		/* Dump the first eight samples for each channel, assuming the format is planer */
		for (int ch = 0; ch < frame->no_channels; ch++) {
			float *d = &frame->p_data[ch * frame->no_samples]; 

			printf("planer ch %02d: ", ch);
			for (int s = 0; s < 8; s++) {
				printf(" %08f,", d[s]);
			}
			printf("\n");
		}
		/* Dump the first eight samples for each channel, assuming the format is interleaved */
		for (int ch = 0; ch < frame->no_channels; ch++) {
			float *d = &frame->p_data[ch];
			printf("interl ch %02d: ", ch);
			for (int s = 0; s < 8; s++) {
				printf(" %08f,", d[s * frame->no_channels]);
			}
			printf("\n");
		}
	}
#endif

#if 0
	printf("%s() ", __func__);
	int from = 0;
	int to = 64; //frame->no_samples;
	printf("float myaddr[] = {\n");
	for (int x = from; x < to; x++) {
		printf(" %08f,", frame->p_data[x]);
	}
	printf("};\n");
#endif

	if (0)
	{
		static time_t lastTime;
		static int lastSecondCount = 0;
		static int currentCount = 0;
		time_t now;
		time(&now);
		if (lastTime != now) {
			lastTime = now;
			lastSecondCount = currentCount;
			currentCount = 0;
			printf(MODULE_PREFIX "Audio frames per second %d\n", lastSecondCount);
		}
		currentCount++;
	}

	if (ctx->clock_offset == 0) {
		printf("%s() clock_offset %" PRIi64 ", skipping\n", __func__, ctx->clock_offset);
		return;
	}

#if 0
	int statsdump = 0;
	if ((frame->sample_rate != 48000) ||
		(frame->no_channels != 2) ||
		(frame->no_samples != 960) ||
		(frame->channel_stride_in_bytes != 3840))
	{
		statsdump = 1;
	}

	if (statsdump) {
		printf("%s() : ", __func__);
		printf("sample_rate = %d ", frame->sample_rate);
		printf("no_channels = %d ", frame->no_channels);
		printf("no_samples = %d ", frame->no_samples);
		printf("channel_stride_in_bytes = %d\n", frame->channel_stride_in_bytes);
	}
#endif

	/* Handle all of the Audio..... */
	/* NDI is float planer, convert it to S32 planer, which is what our framework wants. */
	/* We almost may need to sample rate convert to 48KHz */
	obe_raw_frame_t *rf = new_raw_frame();
	if (!rf) {
		fprintf(stderr, MODULE_PREFIX "Could not allocate raw audio frame\n" );
		return;
	}

	NDIlib_audio_frame_interleaved_32s_t a_frame;
	a_frame.p_data = new int32_t[frame->no_samples * frame->no_channels];
	ctx->p_NDILib->NDIlib_util_audio_to_interleaved_32s_v2(frame, &a_frame);

	/* compute destination number of samples */
	int out_samples = av_rescale_rnd(swr_get_delay(ctx->avr, frame->sample_rate) + frame->no_samples, 48000, frame->sample_rate, AV_ROUND_UP);
	int out_stride = out_samples * 4;
//printf("out_stride %d vs frame stride %d\n", out_stride, frame->channel_stride_in_bytes);
//printf("out_samples %d vs frame samples %d\n", out_samples, frame->no_samples);

	switch (frame->sample_rate) {
	case 16000:
#if TEAMS_16KHZ
		out_stride *= 3; /* Output sample stride needs to increase. */
		//printf("out_strideX3 increased to %d\n", out_stride);
		//printf("out_samplesX3 increased to %d\n", out_samples);
#else
		printf("No NDI-to-ip support for 32KHz\n");
#endif
		break;
	case 32000:
		/* Not supported */
		printf("No NDI-to-ip support for 32KHz\n");
		exit(0);
	case 44100:
		/* Not supported */
		printf("No NDI-to-ip support for 44.1KHz\n");
		exit(0);
	case 48000:
		/* Nothing required */
		break;
	}

	/* Alloc a large enough buffer for all 16 possible channels at the
	 * output sample rate with 16 channels.
	 */
	uint8_t *data = (uint8_t *)calloc(16, out_stride);
	memcpy(data, a_frame.p_data, a_frame.no_channels * frame->channel_stride_in_bytes);

	rf->release_data = obe_release_audio_data;
	rf->release_frame = obe_release_frame;
	rf->audio_frame.num_samples = out_samples;
	rf->audio_frame.num_channels = 16;
	rf->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;
	rf->input_stream_id = 1;

	delete[] a_frame.p_data;

//printf("b opts->audio_channel_count %d\n", opts->audio_channel_count);
	/* Allocate a new sample buffer ready to hold S32P, make it large enough for 16 channels. */
	if (av_samples_alloc(rf->audio_frame.audio_data,
		&rf->audio_frame.linesize,
		16 /*opts->audio_channel_count */,
		rf->audio_frame.num_samples,
		(AVSampleFormat)rf->audio_frame.sample_fmt, 0) < 0)
	{
		fprintf(stderr, MODULE_PREFIX "avsample alloc failed\n");
	}
//printf("c rf->audio_frame.linesize = %d\n", rf->audio_frame.linesize);

	/* -- */

	/* Convert input samples from S16 interleaved into S32P planer. */
	/* Setup the source pointers for all 16 channels. */
	uint8_t *src[16] = { 0 };
	for (int x = 0; x < 16; x++) {
		src[x] = data + (x * out_stride);
		//printf("src[%d] %p\n", x, src[x]);
	}

	/* odd number of channels, create a dual mono stereo pair as designed by
	 * adjusting the source pointers, for the last pair.
	 */
#if TEAMS_16KHZ
	if (opts->audio_channel_count % 2) {
		src[opts->audio_channel_count] = src[opts->audio_channel_count - 1];
		printf("remap src[%d] %p\n", opts->audio_channel_count, src[opts->audio_channel_count]);
	}
#endif

//printf("d rf->audio_frame.num_samples %d\n", rf->audio_frame.num_samples);
	/* Convert from NDI X format into S32P planer. */
	int samplesConverted = swr_convert(ctx->avr,
		rf->audio_frame.audio_data,  /* array of 16 planes */
		out_samples,                 /* out_count */
		(const uint8_t**)&src,
		frame->no_samples            /* in_count */
		);
	if (samplesConverted < 0)
	{
		fprintf(stderr, MODULE_PREFIX "Sample format conversion failed\n");
		return;
	}

	//printf("audio samples converted %d\n", samplesConverted);
	rf->audio_frame.num_samples = samplesConverted;

#if 0
	for (int x = 0; x < 2 /*16*/; x++) {
		printf("output ch%02d: ", x);
		for (int y = 0; y < 8 /* samplesConverted */; y++) {
			printf("%08x ", *(((int32_t *)rf->audio_frame.audio_data[x]) + y));
		}
		printf("\n");
	}
#endif

	free(data);

	if (ctx->reset_a_pts == 1) {
		printf("Audio NDI reset_a_pts is 1\n");
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
	//int64_t pts = av_rescale_q(ctx->a_counter++, (AVRational){frame->no_samples, frame->sample_rate}, (AVRational){1, OBE_CLOCK} );
	//int64_t pts = (frame->timecode * 27) / 10;

	/* 27MHZ clock */
	int64_t pts = av_rescale_q(frame->timecode + ctx->clock_offset, (AVRational){1, 10000000}, (AVRational){1, OBE_CLOCK} );
	//printf("timecode %" PRIi64 ", offset = %" PRIi64 ", minus %" PRIi64 "\n", frame->timecode, ctx->clock_offset, frame->timecode - ctx->clock_offset);
	rf->pts = pts;
	//static int64_t lastPTS = 0;
	//int64_t ptsDELTA = pts - lastPTS;
	//lastPTS = pts;
	//printf("pts %" PRIi64 ", timecode %" PRIi64 "\n", pts, frame->timecode);
#if 0
	printf("NDI audio ptsDELTA %" PRIi64 ", no_samples %d, sample_rate %d, a_counter %" PRIu64 "\n",
		ptsDELTA, frame->no_samples, frame->sample_rate, ctx->a_counter - 1);
#endif

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

//printf("audio pts %" PRIi64 "\n", pts);
	if (add_to_filter_queue(ctx->h, rf) < 0 ) {
		printf("%s() Failed to add frame for raw_frame->input_stream_id %d\n", __func__,
			rf->input_stream_id);
	}
}

static void processFrameVideo(ndi_opts_t *opts, NDIlib_video_frame_v2_t *frame)
{
	ndi_ctx_t *ctx = &opts->ctx;

	ctx->lastVideoFrameTime = ctx->currVideoFrameTime;
	gettimeofday(&ctx->currVideoFrameTime, NULL);
	struct timeval diff;
	obe_timeval_subtract(&diff, &ctx->currVideoFrameTime, &ctx->lastVideoFrameTime);
	ctx->videoFrameIntervalMs = obe_timediff_to_msecs(&diff);
	//printf("%" PRIi64 "\n", ctx->videoFrameIntervalMs);

	if (1)
	{
		static time_t lastTime;
		static int lastSecondCount = 0;
		static int currentCount = 0;
		time_t now;
		time(&now);
		if (lastTime != now) {
			lastTime = now;
			lastSecondCount = currentCount;
			currentCount = 0;
			printf(MODULE_PREFIX "Video frames per second %d\n", lastSecondCount);
		}
		currentCount++;
	}

#if 0
printf(MODULE_PREFIX "line_stride_in_bytes = %d, ", frame->line_stride_in_bytes);
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
	rf->alloc_img.csp = AV_PIX_FMT_YUV420P;
	rf->alloc_img.format = opts->video_format;
	rf->alloc_img.width = opts->width;
	rf->alloc_img.height = opts->height;
	rf->alloc_img.first_line = 1;

	const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(rf->alloc_img.csp);
	rf->alloc_img.planes = d->nb_components;

	rf->alloc_img.stride[0] = opts->width;
	rf->alloc_img.stride[1] = opts->width / 2;
	rf->alloc_img.stride[2] = opts->width / 2;
	rf->alloc_img.stride[3] = 0;

	if (ctx->v_counter == 0) {
	   //int64_t timecode_pts = av_rescale_q(frame->timecode, (AVRational){1, 10000000}, (AVRational){1, OBE_CLOCK} );
	   ctx->clock_offset = (frame->timecode * -1);
	   printf("%s() Clock offset established as %" PRIi64 "\n", __func__, ctx->clock_offset);
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

//printf("video pts %" PRIi64 "\n", pts);
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

#if HALF_FRAME_RATE
unsigned int toggleOutput = 1;
#endif

	while (!ctx->vthreadTerminate && opts->probe == 0) {

		NDIlib_video_frame_v2_t video_frame;
		NDIlib_audio_frame_v2_t audio_frame;
        NDIlib_metadata_frame_t metadata;
#if 0
		NDIlib_tally_t tally (true);
		ctx->p_NDILib->NDIlib_recv_set_tally(ctx->pNDI_recv, &tally);
#endif

		int timeout = av_rescale_q(1000, ctx->v_timebase, (AVRational){1, 1});
		timeout = timeout + 500;

		int v = ctx->p_NDILib->NDIlib_recv_capture_v2(ctx->pNDI_recv, &video_frame, &audio_frame, &metadata, timeout);
		switch (v) {
			case NDIlib_frame_type_video:
#if HALF_FRAME_RATE
				if (++toggleOutput & 1)
#endif
				{
					processFrameVideo(opts, &video_frame);
				}
				ctx->p_NDILib->NDIlib_recv_free_video_v2(ctx->pNDI_recv, &video_frame);
				break;
			case NDIlib_frame_type_audio:
				processFrameAudio(opts, &audio_frame);
				ctx->p_NDILib->NDIlib_recv_free_audio_v2(ctx->pNDI_recv, &audio_frame);
				break;
			case NDIlib_frame_type_metadata:
				printf("Metadata content: %s\n", metadata.p_data);
				ctx->p_NDILib->NDIlib_recv_free_metadata(ctx->pNDI_recv, &metadata);
				break;
			case NDIlib_frame_type_none:
				printf("Frame time exceeded timeout of: %d\n", timeout);
				ctx->reset_v_pts = 1;
				ctx->reset_a_pts = 1;
			default:
				printf("no frame? v = 0x%x\n", v);
		}


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
		swr_free(&ctx->avr);

	if (ctx->pNDI_recv) {
		ctx->p_NDILib->NDIlib_recv_destroy(ctx->pNDI_recv);
	}

	ctx->p_NDILib->NDIlib_destroy();

	printf(MODULE_PREFIX "Closed card idx #%d\n", opts->card_idx);
}

static const char *find_ndi_library()
{
	/* User preferred library */
	const char *ndilibpath = getenv("NDI_LIB_PATH");
	if (ndilibpath) {
		return ndilibpath;
	}

	/* FInd library in a specific order. */
	const char *paths[] = {
		"./libndi.so",
		"./lib/libndi.so",
		"./libs/libndi.so",
		"/usr/local/lib/libndi.so.4.6.2",
		"/usr/local/lib/libndi.so.4",
		"/usr/local/lib/libndi.so",
		"/usr/lib/libndi.so",
		NULL
	};

	int i = 0;
	const char *path = paths[i];
	while (1) {
		if (path == NULL)
			break;

		printf(MODULE_PREFIX "Searching for %30s - ", path);
		struct stat st;
		if (stat(path, &st) == 0) {
			printf("found\n");
			break;
		}	

		printf("not found\n");
		path = paths[++i];
	}

	return path;
}

static int load_ndi_library(ndi_opts_t *opts)
{
	char cwd[256] = { 0 };

	printf(MODULE_PREFIX "%s() cwd = %s\n", __func__, getcwd(&cwd[0], sizeof(cwd)));
	ndi_ctx_t *ctx = &opts->ctx;

	const char *libpath = find_ndi_library();
	if (libpath == NULL) {
		fprintf(stderr, "Unable to locate NDI library, aborting\n");
		exit(1);
	}

	printf(MODULE_PREFIX "Dynamically loading library %s\n", libpath);

	void *hNDILib = dlopen(libpath, RTLD_LOCAL | RTLD_LAZY);

	/* Load the library dynamically. */
	const NDIlib_v3* (*NDIlib_v3_load)(void) = NULL;
	if (hNDILib)
		*((void**)&NDIlib_v3_load) = dlsym(hNDILib, "NDIlib_v3_load");

	/* If the library doesn't have a valid function symbol, complain and hard exit. */
	if (!NDIlib_v3_load) {
		if (hNDILib) {
			dlclose(hNDILib);
		}

		fprintf(stderr, MODULE_PREFIX "NewTek NDI Library, missing critical function, aborting.\n");
		exit(1);
	}

	/* Lets get all of the DLL entry points */
	ctx->p_NDILib = NDIlib_v3_load();

	// We can now run as usual
	if (!ctx->p_NDILib->NDIlib_initialize()) {
		// Cannot run NDI. Most likely because the CPU is not sufficient (see SDK documentation).
		// you can check this directly with a call to NDIlib_is_supported_CPU()
		fprintf(stderr, MODULE_PREFIX "Unable to initialize NDI library, aborting.\n");
		exit(1);
	}

	printf(MODULE_PREFIX "Library loaded and initialized.\n");

	return 0; /* Success */
}

static int open_device(ndi_opts_t *opts)
{
	ndi_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "%s()\n", __func__);

	char extraIPS[256] = { 0 };
	if (strstr(opts->ndi_name, " @ ") != NULL) {
		/* Process the IPS */
		char *pos = rindex(opts->ndi_name, '@');
		if (pos && pos[1] == ' ') {
			strcpy(&extraIPS[0], pos + 2);
		}
	}

	/* --- */
	/* Get the current working dir */
	char cwd[256] = { 0 };
	getcwd(&cwd[0], sizeof(cwd));

	/* make an absolute pathname from the cwd and the NDI input ip given during configuration
	 * We'll use this to push a discovery server into the ndi library, via an env var, before we load the library.
	 * We do this because disocvery servers / mdns aren't always available on the platform, but we already have
	 * the ip address of the source discovery server, so use this instead.
	 * Why do we play this game with setting NDI_CONFIG_DIR? Because we can be encoding from different
	 * discovery servers and a global $HOME/.newtek/ndi-v1-json blurb isn't usable, because it can only contain
	 * a single discovery server.
	 */
	char cfgdir[256];
	snprintf(cfgdir, sizeof(cfgdir), "%s/.%s", cwd, extraIPS);
	printf(MODULE_PREFIX "Using NDI discovery configuration directory '%s'\n", cfgdir);

	/* Check if the ndi file exists, if not throw an informational warning */
	char cfgname[256];
	snprintf(cfgname, sizeof(cfgname), "%s/ndi-config.v1.json", cfgdir);
	printf(MODULE_PREFIX "Using NDI discovery configuration absolute filename '%s'\n", cfgname);
	FILE *fh = fopen(cfgname, "rb");
	if (fh) {
		printf(MODULE_PREFIX "NDI discovery configuration absolute filename found\n");
		fclose(fh);
	} else {
		printf(MODULE_PREFIX "NDI discovery configuration absolute filename missing\n");
	}

	setenv("NDI_CONFIG_DIR", cfgdir, 1);
	/* --- */

	if (load_ndi_library(opts) < 0) {
		fprintf(stderr, MODULE_PREFIX "Unable to initialize NDIlib\n");
		return -1;
	}

	printf(MODULE_PREFIX "NDI version: %s\n", ctx->p_NDILib->NDIlib_version());

	NDIlib_find_instance_t pNDI_find = ctx->p_NDILib->NDIlib_find_create_v2(NULL);
	if (!pNDI_find) {
		fprintf(stderr, MODULE_PREFIX "Unable to initialize NDIlib finder\n");
		return -1;
	}

	if (opts->ndi_name != NULL) {
		printf(MODULE_PREFIX "Searching for NDI Source name: '%s'\n", opts->ndi_name);
	} else {
		printf(MODULE_PREFIX "Searching for card idx #%d\n", opts->card_idx);
	}

	const NDIlib_source_t *p_sources = NULL;
	uint32_t sourceCount = 0;
	int i = 0;
	while (sourceCount == 0) {
		if (i++ >= 10) {
			fprintf(stderr, MODULE_PREFIX "No NDI sources detected\n");
			return -1;
		}
		if (!ctx->p_NDILib->NDIlib_find_wait_for_sources(pNDI_find, 5000 /* ms */)) {
			printf("No change to sources found\n");
			continue;
		}
		p_sources = ctx->p_NDILib->NDIlib_find_get_current_sources(pNDI_find, &sourceCount);
	}

	for (uint32_t x = 0; x < sourceCount; x++) {
		printf(MODULE_PREFIX "Discovered[card-idx=%d] '%s' @ %s\n", x,
			p_sources[x].p_ndi_name,
				p_sources[x].p_url_address);
	}

	/* Removed the trailing ' @ blah' */
	char tname[256];
	strcpy(tname, opts->ndi_name);
	char *t = strstr(tname, " @ ");
	if (t) {
		*t = 0;
	}

	uint32_t x;
	for (x = 0; x < sourceCount; x++) {
		printf("Searching for my new input '%s'\n", tname);
		if (opts->ndi_name) {
#if 0
			printf("Comparing '%s' to '%s'\n", tname, p_sources[x].p_ndi_name);

			for (unsigned int i = 0; i < strlen(tname); i++)
				printf("%02x ", tname[i]);
			printf("\n");

			for (unsigned int i = 0; i < strlen(p_sources[x].p_ndi_name); i++)
				printf("%02x ", p_sources[x].p_ndi_name[i]);
			printf("\n");
#endif
			if (strcasestr(&tname[0], p_sources[x].p_ndi_name)) {
				opts->card_idx = x;
				break;
			}
		}
	}
	if (x == sourceCount && (opts->ndi_name != NULL)) {
		printf(MODULE_PREFIX "Unable to find user requested stream '%s', aborting.\n", opts->ndi_name);
		return -1;
	}

	if (opts->card_idx > (int)(sourceCount - 1))
		opts->card_idx = sourceCount - 1;

	/* We now have at least one source, so we create a receiver to look at it. */
	ctx->pNDI_recv = ctx->p_NDILib->NDIlib_recv_create_v3(NULL);
	if (!ctx->pNDI_recv) {
		fprintf(stderr, MODULE_PREFIX "Unable to create v3 receiver\n");
		return -1;
	}

	printf(MODULE_PREFIX "Found user requested stream, via card_idx %d\n", opts->card_idx);
	ctx->p_NDILib->NDIlib_recv_connect(ctx->pNDI_recv, p_sources + opts->card_idx);

	/* We don't need this */
	ctx->p_NDILib->NDIlib_find_destroy(pNDI_find);

	/* Detect signal properties */
	opts->audio_channel_count = 0;
	int l = 0;
	while (++l < 1024) {

		if (opts->width && opts->audio_channel_count)
			break;

		/* Grab a frame of video and audio, probe it. */
		NDIlib_video_frame_v2_t video_frame;
		NDIlib_audio_frame_v2_t audio_frame;
		NDIlib_metadata_frame_t metadata_frame;

		int ftype = ctx->p_NDILib->NDIlib_recv_capture_v2(ctx->pNDI_recv, &video_frame, &audio_frame, NULL, 5000);
		switch (ftype) {
			case NDIlib_frame_type_video:
#if 1
printf(MODULE_PREFIX "video: line_stride_in_bytes = %d -- ", video_frame.line_stride_in_bytes);
static int64_t lastTimestamp = 0;
printf("timestamp = %" PRIi64 " (%" PRIi64 ")\n",
	video_frame.timestamp,
	video_frame.timestamp - lastTimestamp);

lastTimestamp = video_frame.timestamp;
#endif
				opts->width = video_frame.xres;
				opts->height = video_frame.yres;
				opts->timebase_num = video_frame.frame_rate_D;
#if HALF_FRAME_RATE
printf("Reducing input framerate by 2\n");
				opts->timebase_den = video_frame.frame_rate_N / 2;
#else
				opts->timebase_den = video_frame.frame_rate_N;
#endif

				if (video_frame.frame_format_type == NDIlib_frame_format_type_progressive)
					opts->interlaced = 0;
				else
					opts->interlaced = 1;
#if 1
				printf("Detected fourCC 0x%x\n", video_frame.FourCC);
				if (video_frame.FourCC == NDIlib_FourCC_type_UYVY) {
					printf("UYVY\n");
				} else {
					printf("CS unknown\n");
				}
#endif

				ctx->p_NDILib->NDIlib_recv_free_video_v2(ctx->pNDI_recv, &video_frame);
				break;
			case NDIlib_frame_type_audio:
				opts->audio_channel_samplerate = audio_frame.sample_rate;
				opts->audio_channel_count = audio_frame.no_channels;
				if (opts->audio_channel_count & 1)
					opts->audio_channel_count++;
#if 0
				/* Useful when debugging probe audio issues. */
				printf("Detected audio\n");
				printf("sample_rate = %d\n", audio_frame.sample_rate);
				printf("no_channels = %d\n", audio_frame.no_channels);
				printf("no_samples = %d\n", audio_frame.no_samples);
				printf("channel_stride_in_bytes = %d\n", audio_frame.channel_stride_in_bytes);
#endif
				opts->audio_channel_count = audio_frame.no_channels;
				ctx->p_NDILib->NDIlib_recv_free_audio_v2(ctx->pNDI_recv, &audio_frame);
				break;
			case NDIlib_frame_type_none:
				printf(MODULE_PREFIX "unsupported NDIlib_frame_type_none\n");
				break;
			case NDIlib_frame_type_metadata:
				printf(MODULE_PREFIX "unsupported NDIlib_frame_type_metadata\n");
				ctx->p_NDILib->NDIlib_recv_free_metadata(ctx->pNDI_recv, &metadata_frame);
				break;
			case NDIlib_frame_type_error:
				printf(MODULE_PREFIX "unsupported NDIlib_frame_type_error\n");
				break;
			case NDIlib_frame_type_status_change:
				printf(MODULE_PREFIX "unsupported NDIlib_frame_type_status_change\n");
				break;
			default:
				printf(MODULE_PREFIX "other frame type not supported, type 0x%x\n", ftype);
		}
	}

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

	ctx->avr = swr_alloc();
        if (!ctx->avr) {
            fprintf(stderr, MODULE_PREFIX "Unable to alloc libswresample context\n");
        }

	/* Give libavresample our custom audio channel map */
	printf(MODULE_PREFIX "audio num_channels detected = %d @ %d, (1 << opts->audio_channel_count) - 1 = %d\n",
		opts->audio_channel_count,
		opts->audio_channel_samplerate,
		(1 << opts->audio_channel_count) - 1);
	av_opt_set_int(ctx->avr, "in_channel_layout",   (1 << opts->audio_channel_count) - 1, 0 );
#if TEAMS_16KHZ
	av_opt_set_int(ctx->avr, "in_channel_layout",   1, 0 );
#endif
	av_opt_set_int(ctx->avr, "in_sample_fmt",       AV_SAMPLE_FMT_S32, 0 );
	av_opt_set_int(ctx->avr, "in_sample_rate",      opts->audio_channel_samplerate, 0 );
	av_opt_set_int(ctx->avr, "out_channel_layout",  (1 << opts->audio_channel_count) - 1, 0 );
#if TEAMS_16KHZ
	av_opt_set_int(ctx->avr, "out_channel_layout",  1, 0 );
#endif
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
	int num_streams = 1 /* video */ + 1 /* audio pair */;

	printf(MODULE_PREFIX "%s()\n", __func__);

	ndi_ctx_t *ctx;

	ndi_opts_t *opts = (ndi_opts_t*)calloc(1, sizeof(*opts));
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
	num_streams = 1 /* Video */ + ((opts->audio_channel_count + 1) / 2);


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
	opts->ndi_name = user_opts->name;

	ctx = &opts->ctx;

	ctx->device = device;
	ctx->h = h;
	ctx->v_counter = 0;
	ctx->a_counter = 0;
    ctx->clock_offset = 0;

	if (open_device(opts) < 0) {
		return NULL;
	}

	pthread_create(&ctx->vthreadId, 0, ndi_videoThreadFunc, opts);

	sleep(INT_MAX);

	pthread_cleanup_pop(1);

	return NULL;
}

const obe_input_func_t ndi_input = { ndi_probe_stream, ndi_open_input };

#endif /* HAVE_PROCESSING_NDI_LIB_H */
