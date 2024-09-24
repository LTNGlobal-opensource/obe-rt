#if HAVE_VEGA3311_CAP_TYPES_H

/*****************************************************************************
 * vega-vanc.cpp: VANC processing from Vega encoding cards
 *****************************************************************************
 * Copyright (C) 2023 LTN
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
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <dlfcn.h>

#include "vega-3311.h"

#define LOCAL_DEBUG 1

extern "C"
{
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>
#include <libyuv/convert.h>
#include <libltntstools/ltntstools.h>

extern int ltnpthread_setname_np(pthread_t thread, const char *name);
}

#define MODULE_PREFIX "[vega-ndi]: "


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

static int load_ndi_library(vega_opts_t *opts)
{
	char cwd[256] = { 0 };

	printf(MODULE_PREFIX "%s() cwd = %s\n", __func__, getcwd(&cwd[0], sizeof(cwd)));
	vega_ctx_t *ctx = &opts->ctx;

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

static void vega_ndi_processFrameAudio(vega_opts_t *opts, NDIlib_audio_frame_v2_t *frame)
{
	vega_ctx_t *ctx = &opts->ctx;

        /* Convert 100ns clock to 27MHz clock */
        int64_t pcr = av_rescale_q(frame->timecode, (AVRational){1, 10000000}, (AVRational){1, OBE_CLOCK});

#if LOCAL_DEBUG
        //printf(MODULE_PREFIX "%s() timecode %" PRIi64 "\n", __func__, frame->timecode);
#endif

#if 0
        printf("%s() : ", __func__);
        printf("sample_rate = %d ", frame->sample_rate);
        printf("no_channels = %d ", frame->no_channels);
        printf("no_samples = %d ", frame->no_samples);
        printf("channel_stride_in_bytes = %d\n", frame->channel_stride_in_bytes);
#endif

        if (g_decklink_monitor_hw_clocks) {
                char ts[64];
                vega_pts_to_ascii(&ts[0], pcr / 300);
                printf(MODULE_PREFIX "A/PCR '%s' or %13" PRIi64 ", interval %13" PRIi64 ", frame->timecode %" PRIi64 ", channels %d, channelstride %d\n",
                        ts, pcr, pcr - ctx->ndiLastPCR,
                        frame->timecode,
                        frame->no_channels,
                        frame->channel_stride_in_bytes);
        }

        if (frame->sample_rate != 48000) {
		fprintf(stderr, MODULE_PREFIX "No NDI-to-ip support for %d Hz\n", frame->sample_rate);
                return;
        }

	if (ctx->ndi_clock_offset == 0) {
		printf("%s() clock_offset %" PRIi64 ", skipping\n", __func__, ctx->ndi_clock_offset);
		return;
	}

	ltn_histogram_interval_update(ctx->hg_callback_audio);

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
	int out_samples = av_rescale_rnd(swr_get_delay(ctx->ndiAVR, frame->sample_rate) + frame->no_samples, 48000, frame->sample_rate, AV_ROUND_UP);
	int out_stride = out_samples * 4;

	/* Alloc a large enough buffer for all 16 possible channels at the
	 * output sample rate with 16 channels.
	 */
	uint8_t *data = (uint8_t *)calloc(16, out_stride);
	memcpy(data, a_frame.p_data, a_frame.no_channels * frame->channel_stride_in_bytes);

	rf->release_data             = obe_release_audio_data;
	rf->release_frame            = obe_release_frame;
	rf->audio_frame.num_samples  = out_samples;
	rf->audio_frame.num_channels = 16;
	rf->audio_frame.sample_fmt   = AV_SAMPLE_FMT_S32P;
	rf->input_stream_id          = 1;

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

	/* Convert input samples from S16 interleaved into S32P planer.
	 * Setup the source pointers for all 16 channels.
         */
	uint8_t *src[16] = { 0 };
	for (int x = 0; x < 16; x++) {
		src[x] = data + (x * out_stride);
		//printf("src[%d] %p\n", x, src[x]);
	}

	/* Convert from NDI X format into S32P planer.
         * Our downstream audio encoders want S32P.
         */
	int samplesConverted = swr_convert(ctx->ndiAVR,
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

	rf->audio_frame.num_samples = samplesConverted;

#if 0
	for (int x = 0; x < 16; x++) {
		printf("output ch%02d: ", x);
		for (int y = 0; y < 8 /* samplesConverted */; y++) {
			printf("%08x ", *(((int32_t *)rf->audio_frame.audio_data[x]) + y));
		}
		printf("\n");
	}
#endif

	free(data);

	if (ctx->ndi_reset_a_pts == 1) {
		printf(MODULE_PREFIX "Audio NDI reset_a_pts is 1\n");
		int64_t timecode_pts = av_rescale_q(frame->timecode, (AVRational){1, 10000000}, (AVRational){1, OBE_CLOCK} );
		timecode_pts = timecode_pts + ctx->ndi_clock_offset;

		int64_t pts_diff = av_rescale_q(1, (AVRational){frame->no_samples, frame->sample_rate}, (AVRational){1, OBE_CLOCK} );
		int q = timecode_pts / pts_diff + (timecode_pts % pts_diff > 0);

		ctx->ndi_a_counter = q;
		ctx->ndi_reset_a_pts = 0;
	}

	/* Convert a video frame NDI clock (100us) into a 27MHz clock */
	int64_t pts = av_rescale_q(frame->timecode + ctx->ndi_clock_offset, (AVRational){1, 10000000}, (AVRational){1, OBE_CLOCK} );

	//printf("timecode %" PRIi64 ", offset = %" PRIi64 ", minus %" PRIi64 "\n", frame->timecode, ctx->ndi_clock_offset, frame->timecode - ctx->clock_offset);
	rf->pts = pts;
	//static int64_t lastPTS = 0;
	//int64_t ptsDELTA = pts - lastPTS;
	//lastPTS = pts;
	//printf("pts %" PRIi64 ", timecode %" PRIi64 "\n", pts, frame->timecode);
#if 0
	printf("NDI audio ptsDELTA %" PRIi64 ", no_samples %d, sample_rate %d, ndi_a_counter %" PRIu64 "\n",
		ptsDELTA, frame->no_samples, frame->sample_rate, ctx->ndi_a_counter - 1);
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
	//raw_frame->avfm.hw_audio_correction_clk = ndi_clock_offset;
	//avfm_dump(&raw_frame->avfm);

	if (add_to_filter_queue(ctx->h, rf) < 0 ) {
		printf("%s() Failed to add frame for raw_frame->input_stream_id %d\n", __func__,
			rf->input_stream_id);
	}
}

static void vega_ndi_processFrameVideo(vega_opts_t *opts, NDIlib_video_frame_v2_t *frame)
{
        /* The important thing to recognize is that NDI is handing us UYVY which is 4:2:2*/
        vega_ctx_t *ctx = &opts->ctx;

        ctx->framecount++;

	if (ctx->ndi_v_counter++ == 0) {
	   //int64_t timecode_pts = av_rescale_q(frame->timecode, (AVRational){1, 10000000}, (AVRational){1, OBE_CLOCK} );
	   ctx->ndi_clock_offset = (frame->timecode * -1);
	   printf(MODULE_PREFIX "%s() Clock offset established as %" PRIi64 "\n", __func__, ctx->ndi_clock_offset);
	}

        /* Convert 100ns clock to 27MHz clock */
        int64_t pcr = av_rescale_q(frame->timecode, (AVRational){1, 10000000}, (AVRational){1, OBE_CLOCK});

        if (g_decklink_monitor_hw_clocks) {
                char ts[64];
                vega_pts_to_ascii(&ts[0], pcr / 300);
                printf(MODULE_PREFIX "V/PCR '%s' or %13" PRIi64 ", interval %13" PRIi64 ", FourCC 0x%x, frame->timecode %" PRIi64 ", linestride %d\n",
                        ts, pcr, pcr - ctx->ndiLastPCR,
                        frame->FourCC,
                        frame->timecode,
                        frame->line_stride_in_bytes);
        }

        if (frame->FourCC != NDIlib_FourCC_type_UYVY) {
                fprintf(stderr, MODULE_PREFIX "%s() NDI colorspace 0x%x is not supported\n", __func__, frame->FourCC);
                return;
        }

	ltn_histogram_interval_update(ctx->hg_callback_video);
	if (g_decklink_histogram_print_secs > 0) {
		ltn_histogram_interval_print(STDOUT_FILENO, ctx->hg_callback_video, g_decklink_histogram_print_secs);
        }

        /* Setup memory for 4:2:2, the encoder itself will decimate to 4:2:0.
         * We're going to pass the codec a NV12 Y plane and a packet UV plane.
         */
        uint32_t ylen = opts->width * opts->height;
        uint32_t uvlen = ylen;

        if (ctx->ndiFrameBuffer == NULL) {
                ctx->ndiFrameBufferLength = ylen + uvlen;
                ctx->ndiFrameBuffer = (uint8_t *)malloc(ctx->ndiFrameBufferLength);
                if (!ctx->ndiFrameBuffer) {
                        return;
                }
        }

	/* 
         * Colorspace convert UYVY to I422.
         * The codec will then decimate further to 4:2:0 when we ask it to do
         * NV12 with a 422_to_420 chroma format.
         */
        unsigned char *y   = ctx->ndiFrameBuffer;
        unsigned char *uv  = ctx->ndiFrameBuffer + ylen;
        unsigned char *src = frame->p_data;
        for (int r = 0; r < (opts->width * opts->height) / 2; r++) {
                /* TODO: Is it more effict to read a single dword and write bytes? */
                *(uv++) = *(src++);  /* U */
                *(y++)  = *(src++);  /* Y */
                *(uv++) = *(src++);  /* V */
                *(y++)  = *(src++);  /* Y */
        }

        API_VEGA_BQB_IMG_T img;
        img.pu8Addr    = ctx->ndiFrameBuffer;
        img.u32Size    = ctx->ndiFrameBufferLength;
        img.eFormat    = opts->codec.eFormat; /* this is NV12 */
        img.pts        = pcr / 300LL;
        img.eTimeBase  = API_VEGA_BQB_TIMEBASE_90KHZ;
        img.bLastFrame = ctx->bDoLastFrame;
        img.u32SeiNum  = 0;

        if (g_sei_timestamping) {
                vega_sei_append_ltn_timing(ctx);
        }

        /* Append any queued SEI (captions typically), from sei index 1 onwards */
        vega_sei_lock(ctx);
        if (ctx->seiCount) {
                for (int i = 0; i < ctx->seiCount; i++) {
                        memcpy(&img.tSeiParam[i], &ctx->sei[i], sizeof(API_VEGA_BQB_SEI_PARAM_T));
                }
                img.u32SeiNum += ctx->seiCount;
                ctx->seiCount = 0;
        }
        vega_sei_unlock(ctx);

        /* Submit the picture to the codec */
        if (VEGA_BQB_ENC_PushImage((API_VEGA_BQB_DEVICE_E)opts->brd_idx, (API_VEGA_BQB_CHN_E)opts->card_idx, &img)) {
                fprintf(stderr, MODULE_PREFIX "Error: PushImage failed\n");
        }

        ctx->ndiLastPCR = pcr;
}

static void * vega_ndi_threadfunc(void *p)
{
        vega_opts_t *opts = (vega_opts_t *)p;
        vega_ctx_t *ctx = &opts->ctx;

        ltnpthread_setname_np(ctx->ndiThreadId, "obe-ndi-rx");
        pthread_detach(ctx->ndiThreadId);

        ctx->ndiTimebase.num = 60000;
        ctx->ndiTimebase.den = 1001;

        ctx->ndiThreadRunning = 1;

	while (!ctx->ndiThreadTerminate) {

                if (ctx->bLastFramePushed) {
                        /* Encoder wants to shut down */
                        break;
                }

		NDIlib_video_frame_v2_t video_frame;
		NDIlib_audio_frame_v2_t audio_frame;
                NDIlib_metadata_frame_t metadata;
#if 0
		NDIlib_tally_t tally (true);
		ctx->p_NDILib->NDIlib_recv_set_tally(ctx->pNDI_recv, &tally);
#endif

		int timeout = av_rescale_q(1000, ctx->ndiTimebase, (AVRational){1, 1}) + 500;

		int v = ctx->p_NDILib->NDIlib_recv_capture_v2(ctx->pNDI_recv, &video_frame, &audio_frame, &metadata, timeout);
		switch (v) {
			case NDIlib_frame_type_video:
                                vega_ndi_processFrameVideo(opts, &video_frame);
				ctx->p_NDILib->NDIlib_recv_free_video_v2(ctx->pNDI_recv, &video_frame);
				break;
			case NDIlib_frame_type_audio:
                                ctx->ndi_audio_channel_count = audio_frame.no_channels;
                                ctx->ndi_audio_sample_rate = audio_frame.sample_rate;;
				vega_ndi_processFrameAudio(opts, &audio_frame);
				ctx->p_NDILib->NDIlib_recv_free_audio_v2(ctx->pNDI_recv, &audio_frame);
				break;
			case NDIlib_frame_type_metadata:
				printf("Metadata content: %s\n", metadata.p_data);
				ctx->p_NDILib->NDIlib_recv_free_metadata(ctx->pNDI_recv, &metadata);
				break;
			case NDIlib_frame_type_none:
				printf("Frame time exceeded timeout of: %d\n", timeout);
				//ctx->ndi_reset_v_pts = 1;
				ctx->ndi_reset_a_pts = 1;
			default:
				printf("no frame? v = 0x%x\n", v);
		}
	}
	printf(MODULE_PREFIX "Video thread complete\n");

        ctx->ndiThreadTerminated = 1;
        pthread_exit(0);

        return NULL;
}

int vega_ndi_start(vega_opts_t *opts)
{
        vega_ctx_t *ctx = &opts->ctx;

        pthread_mutex_init(&ctx->ndiLock, NULL);

#if LOCAL_DEBUG
	printf(MODULE_PREFIX "%s()\n", __func__);
#endif

        opts->ndi_name = strdup("LTN-DELL-5820 (vMix - Output 1) @ 192.168.20.243");

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
	sprintf(cfgdir, "%s/.%s", cwd, extraIPS);
	printf(MODULE_PREFIX "Using NDI discovery configuration directory '%s'\n", cfgdir);

	/* Check if the ndi file exists, if not throw an informational warning */
	char cfgname[256];
	sprintf(cfgname, "%s/ndi-config.v1.json", cfgdir);
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

	ctx->ndiAVR = swr_alloc();
        if (!ctx->ndiAVR) {
            fprintf(stderr, MODULE_PREFIX "Unable to alloc libswresample context\n");
        }

	/* Give libavresample our custom audio channel map */
        ctx->ndi_audio_channel_count = 16;
        ctx->ndi_audio_sample_rate = 48000;

	printf(MODULE_PREFIX "audio num_channels detected = %d @ %d, (1 << ctx->audio_channel_count) - 1 = %d\n",
		ctx->ndi_audio_channel_count,
		ctx->ndi_audio_sample_rate,
		(1 << ctx->ndi_audio_channel_count) - 1);
	av_opt_set_int(ctx->ndiAVR, "in_channel_layout",   (1 << ctx->ndi_audio_channel_count) - 1, 0);
	av_opt_set_int(ctx->ndiAVR, "in_sample_fmt",       AV_SAMPLE_FMT_S32, 0);
	av_opt_set_int(ctx->ndiAVR, "in_sample_rate",      ctx->ndi_audio_sample_rate, 0);
	av_opt_set_int(ctx->ndiAVR, "out_channel_layout",  (1 << ctx->ndi_audio_channel_count) - 1, 0);
	av_opt_set_int(ctx->ndiAVR, "out_sample_fmt",      AV_SAMPLE_FMT_S32P, 0);
	av_opt_set_int(ctx->ndiAVR, "out_sample_rate",     48000, 0);

	if (swr_init(ctx->ndiAVR) < 0) {
		fprintf(stderr, MODULE_PREFIX "Could not configure libswresample\n");
	}

        /* This runs the main NDI frame processing loop */
        if (pthread_create(&ctx->ndiThreadId, NULL, vega_ndi_threadfunc, opts) != 0) {
                fprintf(stderr, MODULE_PREFIX "Unable to create NDI processing thread\n");
                return -1;
        }

        return 0; /* Success */
}

void vega_ndi_stop(vega_opts_t *opts)
{
        vega_ctx_t *ctx = &opts->ctx;

#if LOCAL_DEBUG
	printf(MODULE_PREFIX "%s()\n", __func__);
#endif
        ctx->ndiThreadTerminate = 1;
        int count = 0;
        while (ctx->ndiThreadRunning && !ctx->ndiThreadTerminated) {
                usleep(100 * 1000);
                if (count++ > 20) {
                        break;
                }
        }

        if (ctx->p_NDILib && ctx->pNDI_recv) {
		ctx->p_NDILib->NDIlib_recv_destroy(ctx->pNDI_recv);
	}

	if (ctx->p_NDILib) {
                ctx->p_NDILib->NDIlib_destroy();
        }

        if (ctx->ndiFrameBuffer) {
                free(ctx->ndiFrameBuffer);
                ctx->ndiFrameBuffer = NULL;
        }

        if (ctx->ndiAVR) {
		swr_free(&ctx->ndiAVR);
                ctx->ndiAVR = NULL;
        }

#if LOCAL_DEBUG
	printf(MODULE_PREFIX "%s() Stopped the NDI layer\n", __func__);
#endif

}

#endif /* #if HAVE_VEGA3311_CAP_TYPES_H */
