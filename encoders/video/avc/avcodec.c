/*****************************************************************************
 * avcodec.c : AVC encoding functions
 *****************************************************************************
 * Copyright (C) 2019 LiveTimeNet Inc. All Rights Reserved.
 *
 * Authors: Steven Toth <stoth@ltnglobal.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 ******************************************************************************/

#include "common/common.h"
#include "encoders/video/video.h"
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <assert.h>
#include <sys/mman.h>
#include <libyuv.h>
#include <libltntstools/ltntstools.h>

#define LOCAL_DEBUG 0
#define DEBUG_CODEC_TIMING 0

#define MESSAGE_PREFIX "[avcodec]: "

int g_avcodec_nal_debug = 0;
int g_avcodec_monitor_bps = 1;

struct context_s
{
	obe_vid_enc_params_t *enc_params;
	obe_t *h;
	obe_encoder_t *encoder;

	/* AVCodec */
	AVCodecContext *c;
	const AVCodec *codec;

	uint64_t      raw_frame_count;
	int frame_width, frame_height;

	/* VAAPI */
	AVBufferRef *hw_device_ctx;

	struct SwsContext *encoderSwsContext;
	//pthread_t encode_thread;
	pthread_mutex_t encode_mutex;
	pthread_cond_t encode_cond;

};

static int set_hwframe_ctx(struct context_s *ctx, AVCodecContext *avctx, AVBufferRef *hw_device_ctx)
{
    AVBufferRef *hw_frames_ref;
    AVHWFramesContext *frames_ctx = NULL;
    int err = 0;

    if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx))) {
        fprintf(stderr, "Failed to create VAAPI frame context.\n");
        return -1;
    }
    frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    switch (obe_core_encoder_get_stream_format(ctx->encoder)) {
    case VIDEO_HEVC_GPU_NVENC_AVCODEC:
        frames_ctx->format    = AV_PIX_FMT_CUDA;
        break;
    case VIDEO_AVC_GPU_VAAPI_AVCODEC:
    case VIDEO_HEVC_GPU_VAAPI_AVCODEC:
    default:
        frames_ctx->format    = AV_PIX_FMT_VAAPI;
    }
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width     = ctx->enc_params->avc_param.i_width;
    frames_ctx->height    = ctx->enc_params->avc_param.i_height;
    frames_ctx->initial_pool_size = 20;
    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
        fprintf(stderr, "Failed to initialize GPU frame context."
                "Error code: %s\n",av_err2str(err));
        av_buffer_unref(&hw_frames_ref);
        return err;
    }
    avctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!avctx->hw_frames_ctx)
        err = AVERROR(ENOMEM);

    AVHWFramesConstraints *c = av_hwdevice_get_hwframe_constraints(hw_frames_ref, 0);
    if (c) {
        enum AVPixelFormat *p;
        for (p = c->valid_sw_formats; *p != AV_PIX_FMT_NONE; p++) {
            printf(MESSAGE_PREFIX "GPU supports pixel format %s\n", av_get_pix_fmt_name(*p));
        }
    }

    av_buffer_unref(&hw_frames_ref);
    return err;
}

#define SERIALIZE_CODED_FRAMES 0
#if SERIALIZE_CODED_FRAMES
static void serialize_coded_frame(obe_coded_frame_t *cf)
{
    static FILE *fh = NULL;
    if (!fh) {
        fh = fopen("/storage/dev/avc-vaapi.cf", "wb");
        printf("Warning -- avc vaapi coded frames will persist to disk\n");
    }
    if (fh)
        coded_frame_serializer_write(fh, cf);
}
#endif

static void _monitor_bps(struct context_s *ctx, int lengthBytes)
{
	/* Monitor bps for sanity... */
	static int codec_bps_current = 0;
	static int codec_bps = 0;
	static time_t codec_bps_time = 0;
	time_t now;
	time(&now);
	if (now != codec_bps_time) {
		codec_bps = codec_bps_current;
		codec_bps_current = 0;
		codec_bps_time = now;
		double dbps = (double)codec_bps;
		dbps /= 1e6;
		if (dbps >= ctx->enc_params->avc_param.rc.i_vbv_max_bitrate) {
			fprintf(stderr, MESSAGE_PREFIX " codec output %d bps exceeds vbv_max_bitrate %d @ %s",
				codec_bps,
				ctx->enc_params->avc_param.rc.i_vbv_max_bitrate,
				ctime(&now));
		}
		if (g_avcodec_monitor_bps) {
			printf(MESSAGE_PREFIX " codec output %.02f (Mb/ps) @ %s", dbps, ctime(&now));
		}
	}
	codec_bps_current += (lengthBytes * 8);
}

static size_t _deliver_nals(struct context_s *ctx, AVPacket *pkt, obe_raw_frame_t *rf, int frame_type)
{
#if 0
	static time_t lastTime = 0;
	time_t now = time(NULL);
	static int nps = 0;
	if (lastTime != now) {
		lastTime = now;
		printf("nps %d\n", nps);
		nps = 0;
	}
	nps++;
#endif
	_monitor_bps(ctx, pkt->size);

	obe_coded_frame_t *cf = new_coded_frame(ctx->encoder->output_stream_id, pkt->size);
	if (!cf) {
		fprintf(stderr, MESSAGE_PREFIX " unable to alloc a new coded frame\n");
		return 0;
	}

	char *foundNals;

	if (g_avcodec_nal_debug) {
		/* For each nal, lookup and prepare a list of found NAL types. */
		switch (obe_core_encoder_get_stream_format(ctx->encoder)) {
		case VIDEO_HEVC_GPU_VAAPI_AVCODEC:
			foundNals = ltn_nal_hevc_findNalTypes(pkt->data, pkt->size);
			break;
		default:
			/* No requirement to support */
			foundNals = strdup("");
		}

		static int pktcount = 0;
		static int64_t lastDTS = 0;
		int64_t deltaDTS = pkt->dts - lastDTS;
		lastDTS = pkt->dts;
		printf("%08d AVPacket->pts %13" PRIi64"  dts %13" PRIi64 " (%6" PRIi64 ") -- duration %" PRIi64 "  flags 0x%x %spos %" PRIi64 " side_data_elems %d -- nals [ %s ]\n",
			pktcount++,
			pkt->pts,
			pkt->dts,
			deltaDTS,
			pkt->duration, pkt->flags,
			pkt->flags & AV_PKT_FLAG_KEY ? "k " : "  ",
			pkt->pos,
			pkt->side_data_elems,
			foundNals);
	
		free(foundNals);
	}

	//struct avfm_s *avfm = &rf->avfm;
	memcpy(&cf->avfm, &rf->avfm, sizeof(struct avfm_s));

	//avfm_dump(&rf->avfm);

#if 0
	static FILE *fh = NULL;
	if (!fh) {
		fh = fopen("/tmp/avcodec.nals", "wb");
	}
	if (fh)
		fwrite(pkt->data, 1, pkt->size, fh);
#endif

	memcpy(cf->data, pkt->data, pkt->size);
	cf->len          = pkt->size;
	cf->type         = CF_VIDEO;

	//cf->arrival_time = rf->arrival_time;

	cf->pts          = pkt->pts + (2 * 0);
	cf->real_pts     = pkt->pts + (2 * 0);
	cf->real_dts     = pkt->dts + (2 * 0);

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
		cf->cpb_initial_arrival_time = cf->real_dts - 1351350; /* 3x frame interval */
	} else {
		cf->cpb_initial_arrival_time = cf->real_pts - 1351350; /* 3x frame interval */
	}

	AVCodecContext *c = ctx->c;
	double bit_rate = (double)c->bit_rate;
	double fraction = bit_rate / 216000.0;
        double estimated = ((double)cf->len / fraction);
        double estimated_final = estimated + (double)cf->cpb_initial_arrival_time;
        cf->cpb_final_arrival_time  = estimated_final;

	cf->priority = pkt->flags & AV_PKT_FLAG_KEY;
	cf->random_access = pkt->flags & AV_PKT_FLAG_KEY;

	//coded_frame_print(cf);

#if DEBUG_CODEC_TIMING
	{
		static int64_t last_real_pts = 0;
		static int64_t last_real_dts = 0;

		printf("adjust: real_pts %12" PRIi64 " ( %12" PRIi64 " )  real_dts %12" PRIi64 " ( %12" PRIi64 " )  iat %12" PRIi64 "  fat %12" PRIi64 "  audio_pts %10" PRIi64" \n",
			cf->real_pts,
			cf->real_pts - last_real_pts,
			cf->real_dts,
			cf->real_dts - last_real_dts,
			cf->cpb_initial_arrival_time,
			cf->cpb_final_arrival_time,
			cf->pts);

		last_real_pts = cf->real_pts;
		last_real_dts = cf->real_dts;
}
#endif
	if (g_sei_timestamping) {
		/* Walk through each of the NALS and insert current time into any LTN sei timestamp frames we find. */
		int offset = ltn_uuid_find(cf->data, pkt->size);
		if (offset >= 0) {
			struct timeval tv;
			gettimeofday(&tv, NULL);

			/* Add the time exit from compressor seconds/useconds. */
			sei_timestamp_field_set(&cf->data[offset], pkt->size - offset, 6, tv.tv_sec);
			sei_timestamp_field_set(&cf->data[offset], pkt->size - offset, 7, tv.tv_usec);
		}
	}

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
		cf->arrival_time = rf->arrival_time;
#if SERIALIZE_CODED_FRAMES
		serialize_coded_frame(cf);
#endif
		add_to_queue(&ctx->h->mux_queue, cf);
	} else {
#if SERIALIZE_CODED_FRAMES
		serialize_coded_frame(cf);
#endif
		add_to_queue(&ctx->h->enc_smoothing_queue, cf);
	}

	return pkt->size;
}

#if 0
static void _av_frame_dump(AVFrame *f)
{
	printf("%dx%d -- \n", f->width, f->height);
	for (int i = 0; i < 3; i++)
		printf("  plane[%d]: data %p linesize %d\n", i, f->data[i], f->linesize[i]);
}
#endif

#if 0
void my_log_callback(void *ptr, int level, const char *fmt, va_list vargs)
{
    vprintf(fmt, vargs);
}
#endif

static int _init_codec(struct context_s *ctx)
{
	obe_vid_enc_params_t *ep = ctx->enc_params;
	AVCodecContext *c = ctx->c;

	int err;
	switch (obe_core_encoder_get_stream_format(ctx->encoder)) {
	case VIDEO_AVC_GPU_VAAPI_AVCODEC:
	case VIDEO_HEVC_GPU_VAAPI_AVCODEC:
		err = av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
		if (err < 0) {
			fprintf(stderr, MESSAGE_PREFIX "Failed to initialize GPU\n");
			return -1;
		}
		break;
	case VIDEO_HEVC_GPU_NVENC_AVCODEC:
		err = av_hwdevice_ctx_create(&ctx->hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, NULL, NULL, 0);
		if (err < 0) {
			fprintf(stderr, MESSAGE_PREFIX "Failed to initialize GPU\n");
			return -1;
		}
		break;
	default:
		break;
	}

	printf(MESSAGE_PREFIX "Initializing as %dx%d\n", ep->avc_param.i_width, ep->avc_param.i_height);
	printf(MESSAGE_PREFIX "bitrate %d\n", ep->avc_param.rc.i_bitrate);
	printf(MESSAGE_PREFIX "vbv_max_bitrate %d\n", ep->avc_param.rc.i_vbv_max_bitrate);
	printf(MESSAGE_PREFIX "sample_aspect_ratio = { %d, %d }\n",
		ep->avc_param.vui.i_sar_width, ep->avc_param.vui.i_sar_height);
	printf(MESSAGE_PREFIX "i_fps_num / den = { %d , %d}\n",
		ep->avc_param.i_fps_num, ep->avc_param.i_fps_den);

	c->bit_rate = ep->avc_param.rc.i_bitrate * 1000;
#if 0
	c->thread_count = 8;
	c->rc_buffer_size = ep->avc_param.rc.i_bitrate * 1000;
	c->rc_max_rate = ep->avc_param.rc.i_bitrate * 1000;
#endif

	c->width = ep->avc_param.i_width;
	c->height = ep->avc_param.i_height;

	c->framerate = (AVRational){ ep->avc_param.i_fps_num, ep->avc_param.i_fps_den };
	c->time_base = (AVRational){ ep->avc_param.i_fps_den, ep->avc_param.i_fps_num };

	ctx->c->sample_aspect_ratio = (AVRational) {
		ep->avc_param.vui.i_sar_width,
		ep->avc_param.vui.i_sar_height };

        switch (obe_core_encoder_get_stream_format(ctx->encoder)) {
        case VIDEO_AVC_GPU_VAAPI_AVCODEC:
        case VIDEO_HEVC_GPU_VAAPI_AVCODEC:
		c->pix_fmt = AV_PIX_FMT_VAAPI;
		break;
        case VIDEO_HEVC_GPU_NVENC_AVCODEC:
		c->pix_fmt = AV_PIX_FMT_CUDA;
		break;
	default:
		c->pix_fmt = AV_PIX_FMT_YUV420P;
	}

	if (obe_core_encoder_get_stream_format(ctx->encoder) == VIDEO_AVC_GPU_VAAPI_AVCODEC) {
		/* gpu codec --  H264 */
printf("mode VIDEO_AVC_GPU_VAAPI_AVCODEC\n");
		av_opt_set(c->priv_data, "aud", "1", 0); /* Generate Access Unit Delimiters */
		av_opt_set(c->priv_data, "sei", "1", 0);
		av_opt_set(c->priv_data, "timing", "1", 0);
		av_opt_set(c->priv_data, "profile", "high", 0);
		av_opt_set(c->priv_data, "level", "51", 0);
		av_opt_set(c->priv_data, "g", "30", 0);
		//av_opt_set(c->priv_data, "bf", "0", 0);
		//av_opt_set(c->priv_data, "qp", "28", 0);
	} else
	if (obe_core_encoder_get_stream_format(ctx->encoder) == VIDEO_HEVC_GPU_VAAPI_AVCODEC) {
		/* gpu codec --  HEVC */
printf("mode VIDEO_HEVC_GPU_VAAPI_AVCODEC\n");
		av_opt_set(c->priv_data, "aud", "1", 0); /* Generate Access Unit Delimiters */
		av_opt_set(c->priv_data, "sei", "1", 0);
		av_opt_set(c->priv_data, "timing", "1", 0);
		av_opt_set(c->priv_data, "profile", "main", 0);
		av_opt_set(c->priv_data, "level", "51", 0);
		av_opt_set(c->priv_data, "g", "30", 0);
		//av_opt_set(c->priv_data, "bf", "0", 0);
		//av_opt_set(c->priv_data, "qp", "28", 0);
	} else
	if (obe_core_encoder_get_stream_format(ctx->encoder) == VIDEO_HEVC_GPU_NVENC_AVCODEC) {
		/* gpu codec --  NVENC HEVC */
printf("mode VIDEO_HEVC_GPU_NVENC_AVCODEC\n");
		av_opt_set(c->priv_data, "aud", "1", 0); /* Generate Access Unit Delimiters */
		av_opt_set(c->priv_data, "cbr", "1", 0);
		av_opt_set(c->priv_data, "preset", "ll", 0);
		//av_opt_set(c->priv_data, "sei", "1", 0);
		//av_opt_set(c->priv_data, "timing", "1", 0);
		//av_opt_set(c->priv_data, "profile", "main", 0);
		//av_opt_set(c->priv_data, "level", "51", 0);
		//av_opt_set(c->priv_data, "g", "30", 0);
		//av_opt_set(c->priv_data, "bf", "0", 0);
		//av_opt_set(c->priv_data, "qp", "28", 0);
	} else
	if (obe_core_encoder_get_stream_format(ctx->encoder) == VIDEO_AVC_CPU_AVCODEC) {
		/* s/w codec - libx264 */
printf("mode VIDEO_AVC_CPU_AVCODEC\n");
#if 0
		av_opt_set(c->priv_data, "profile", "main", 0);
#endif
		av_opt_set(c->priv_data, "preset", "faster", 0);
		av_opt_set(c->priv_data, "tune", "zerolatency", 0);
		av_opt_set(c->priv_data, "threads", "4", 0);
		av_opt_set(c->priv_data, "slices", "1", 0);
		av_opt_set(c->priv_data, "aud", "1", 0); /* Generate Access Unit Delimiters */
		int ret = av_opt_set_int(c->priv_data, "afd", 1, 0);
		printf("AFD set ret %d\n", ret);
	} else
	if (obe_core_encoder_get_stream_format(ctx->encoder) == VIDEO_HEVC_CPU_AVCODEC) {
		/* s/w codec -- libx265 */
#if 0
printf("mode VIDEO_HEVC_CPU_AVCODEC\n");
av_log_set_level(9);
av_log_set_callback(my_log_callback);
#endif

		av_opt_set(c->priv_data, "preset", "superfast", 0);
		av_opt_set(c->priv_data, "aud", "1", 0); /* Generate Access Unit Delimiters */
		//int ret = av_opt_set_int(c->priv_data, "afd", 1, 0);
		//printf("AFD set ret %d\n", ret);

		av_opt_set(c->priv_data, "x265-params", "ctu=32:rc-lookahead=15:min-keyint=6:frame-threads=8:bframes=8:ref=3:keyint=60:vbv-maxrate=5000:vbv-bufsize=20000:log-level=4:qpmin=15", 0);
#if 0
                x265_param_parse(ctx->hevc_params, "ctu", "32");
                x265_param_parse(ctx->hevc_params, "bframes", "8");
                x265_param_parse(ctx->hevc_params, "ref", "3");
        }

        sprintf(&val[0], "%d", g_x265_min_qp);
        printf(MESSAGE_PREFIX "Setting QPmin to %s\n", val);
        x265_param_parse(ctx->hevc_params, "qpmin", val);

#endif
	} else {
printf("mode undefined\n");
	}

        switch (obe_core_encoder_get_stream_format(ctx->encoder)) {
        case VIDEO_AVC_GPU_VAAPI_AVCODEC:
        case VIDEO_HEVC_GPU_VAAPI_AVCODEC:
        case VIDEO_HEVC_GPU_NVENC_AVCODEC:
		/* set hw_frames_ctx for encoder's AVCodecContext */
		if ((err = set_hwframe_ctx(ctx, ctx->c, ctx->hw_device_ctx)) < 0) {
			fprintf(stderr, MESSAGE_PREFIX "Failed to set hwframe context.\n");
			exit(1);
		}
		break;
	default:
		break;
	}

	int ret = avcodec_open2(ctx->c, ctx->codec, NULL);
	if (ret < 0) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to create codec\n");
		exit(1);
	}

	return 0;
}

static int _encode_frame(struct context_s *ctx, obe_raw_frame_t *rf, AVFrame *frame)
{
	AVPacket enc_pkt;

#if 0
	/* TODO: Handle SEI / AFD / User payloads. */
	for (int i = 0; i < rf->num_user_data; i++) {
		/* Disregard any data types that we don't fully support. */
		int type = rf->user_data[i].type;
		printf("[side%d] type 0x%02x len 0x%02x data : ", i,
			rf->user_data[i].type, rf->user_data[i].len);
		for (int j = 0; j < rf->user_data[i].len; j++)
			printf("%02x ", rf->user_data[i].data[j]);
		printf("\n");
		if (type == USER_DATA_AVC_REGISTERED_ITU_T35 /*|| type == USER_DATA_AVC_UNREGISTERED */) {
			count++;
		}
	}
#endif

	/* TODO: sei timestamping */
	//frame->nb_size_data = count;
	AVFrameSideData *sd = av_frame_new_side_data(frame, AV_FRAME_DATA_AFD, sizeof(uint8_t));
	if (sd) {
		*sd->data = 10; /* sei.afd.active_format_description */
	}

	if (g_sei_timestamping) {
#if 0
		sd = av_frame_new_side_data(frame, AV_FRAME_DATA_ISO14496_USER_UNREGISTERED, SEI_TIMESTAMP_PAYLOAD_LENGTH);
		if (sd) {
			if (sei_timestamp_init(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH) == 0) {
				struct timeval tv;
				gettimeofday(&tv, NULL);

				sei_timestamp_field_set(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH, 1, ctx->raw_frame_count);
				sei_timestamp_field_set(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH, 2, avfm_get_hw_received_tv_sec(&rf->avfm));
				sei_timestamp_field_set(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH, 3, avfm_get_hw_received_tv_usec(&rf->avfm));
				sei_timestamp_field_set(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH, 4, tv.tv_sec);
				sei_timestamp_field_set(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH, 5, tv.tv_usec);
				sei_timestamp_field_set(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH, 6, 0); /* time exit from compressor seconds/useconds. */
				sei_timestamp_field_set(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH, 7, 0); /* time exit from compressor seconds/useconds. */
				sei_timestamp_field_set(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH, 8, 0); /* time transmit to udp seconds/useconds. */
				sei_timestamp_field_set(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH, 9, 0); /* time transmit to udp seconds/useconds. */
			
				//sei_timestamp_hexdump(sd->data, SEI_TIMESTAMP_PAYLOAD_LENGTH);
			}
		}
#endif
	} /* if (g_sei_timestamping) */

	int ret = avcodec_send_frame(ctx->c, frame);
	if (ret < 0) {
		fprintf(stderr, MESSAGE_PREFIX "error encoding frame\n");
		exit(1);
	}

	while (ret >= 0) {
		av_init_packet(&enc_pkt);
		enc_pkt.data = NULL;
		enc_pkt.size = 0;

		ret = avcodec_receive_packet(ctx->c, &enc_pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			return 1;
		} else if (ret < 0) {
			fprintf(stderr, MESSAGE_PREFIX "Error during encoding\n");
			exit(1);
		}

		/* TODO: This is defintely wrong. We're delivering the WRONG rf for the current nal. */
		enc_pkt.stream_index = 0;
		_deliver_nals(ctx, &enc_pkt, rf, 0);
		av_packet_unref(&enc_pkt);
	}

	return 0;
}

/* OBE will pass us a AVC struct initially. Pull out any important pieces
 * and pass those to x265.
 */
static void *avc_gpu_avcodec_start_encoder(void *ptr)
{
	struct context_s ectx, *ctx = &ectx;
	memset(ctx, 0, sizeof(*ctx));
	int ret = 0;

	ctx->enc_params = ptr;
	ctx->h = ctx->enc_params->h;
	ctx->encoder = ctx->enc_params->encoder;

	printf(MESSAGE_PREFIX "Starting encoder: %s\n",
		stream_format_name(obe_core_encoder_get_stream_format(ctx->encoder)));

#if 0
	printf(MESSAGE_PREFIX "Initializing as %dx%d\n",
		ctx->enc_params->avc_param.i_width,
		ctx->enc_params->avc_param.i_height);

	printf(MESSAGE_PREFIX "sample_aspect_ratio = { %d, %d }\n",
		ctx->enc_params->avc_param.vui.i_sar_width, ctx->enc_params->avc_param.vui.i_sar_height);
#endif

	ctx->frame_width = ctx->enc_params->avc_param.i_width;
	ctx->frame_height = ctx->enc_params->avc_param.i_height;

	/* Fix this? its AVC specific. */
	ctx->encoder->encoder_params = malloc(sizeof(ctx->enc_params->avc_param) );
	if (!ctx->encoder->encoder_params) {
		fprintf(stderr, MESSAGE_PREFIX " failed to allocate encoder params\n");
		goto out1;
	}

	memcpy(ctx->encoder->encoder_params, &ctx->enc_params->avc_param, sizeof(ctx->enc_params->avc_param));

	if (obe_core_encoder_get_stream_format(ctx->encoder) == VIDEO_AVC_GPU_VAAPI_AVCODEC) {
		ctx->codec = avcodec_find_encoder_by_name("h264_vaapi");
	} else
	if (obe_core_encoder_get_stream_format(ctx->encoder) == VIDEO_AVC_CPU_AVCODEC) {
		ctx->codec = avcodec_find_encoder_by_name("libx264");
	} else
	if (obe_core_encoder_get_stream_format(ctx->encoder) == VIDEO_HEVC_GPU_VAAPI_AVCODEC) {
		ctx->codec = avcodec_find_encoder_by_name("hevc_vaapi");
	} else
	if (obe_core_encoder_get_stream_format(ctx->encoder) == VIDEO_HEVC_GPU_NVENC_AVCODEC) {
		ctx->codec = avcodec_find_encoder_by_name("hevc_nvenc");
	} else
	if (obe_core_encoder_get_stream_format(ctx->encoder) == VIDEO_HEVC_CPU_AVCODEC) {
		ctx->codec = avcodec_find_encoder_by_name("libx265");
	}

	if (!ctx->codec) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to locate codec\n");
		exit(1);
	}

	printf(MESSAGE_PREFIX "Support pixel formats:\n");
	if (ctx->codec->pix_fmts) {
		for (int i = 0; ctx->codec->pix_fmts[i] != AV_PIX_FMT_NONE; i++)
			printf("fmt[%d] = %s\n", i, av_get_pix_fmt_name(ctx->codec->pix_fmts[i]));
	}

	ctx->c = avcodec_alloc_context3(ctx->codec);
	if (!ctx->c) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to allocate codec\n");
		exit(1);
	}
	printf(MESSAGE_PREFIX "Allocated codec\n");

	AVPacket *pkt = av_packet_alloc();
	if (!pkt) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to allocate packet\n");
		exit(1);
	}

	/* For libva to use the haswell compatible (older) opensource i965
	 * driver, instead of the default iHD newer intel driver.
	 */
	setenv("LIBVA_DRIVER_NAME", "i965", 1);

	ret = _init_codec(ctx);
	if (ret < 0) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to initialize VA-API, ret = %d\n", ret);
		goto out2;
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to allocate frame\n");
		exit(1);
	}

	switch (obe_core_encoder_get_stream_format(ctx->encoder)) {
	case VIDEO_AVC_GPU_VAAPI_AVCODEC:
	case VIDEO_HEVC_GPU_VAAPI_AVCODEC:
	case VIDEO_HEVC_GPU_NVENC_AVCODEC:
		frame->format = AV_PIX_FMT_NV12;
		break;
	default:
		frame->format = ctx->c->pix_fmt;
	}

	frame->width = ctx->c->width;
	frame->height = ctx->c->height;

	ret = av_frame_get_buffer(frame, 32);
	if (ret < 0) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to get frame buffer\n");
		exit(1);
	}

	/* Lock the mutex until we verify and fetch new parameters */
	pthread_mutex_lock(&ctx->encoder->queue.mutex);

	ctx->encoder->is_ready = 1;

	//int64_t frame_duration = av_rescale_q(1, (AVRational){ ctx->enc_params->avc_param.i_fps_den, ctx->enc_params->avc_param.i_fps_num}, (AVRational){ 1, OBE_CLOCK } );

	/* Wake up the muxer */
	pthread_cond_broadcast(&ctx->encoder->queue.in_cv);
	pthread_mutex_unlock(&ctx->encoder->queue.mutex);

	while (1) {
		pthread_mutex_lock(&ctx->encoder->queue.mutex);

		while (!ctx->encoder->queue.size && !ctx->encoder->cancel_thread) {
			pthread_cond_wait(&ctx->encoder->queue.in_cv, &ctx->encoder->queue.mutex);
		}

		if (ctx->encoder->cancel_thread) {
			pthread_mutex_unlock(&ctx->encoder->queue.mutex);
			break;
		}

		/* Reset the speedcontrol buffer if the source has dropped frames. Otherwise speedcontrol
		 * stays in an underflow state and is locked to the fastest preset.
		 */
		pthread_mutex_lock(&ctx->h->drop_mutex);
		if (ctx->h->video_encoder_drop) {
			pthread_mutex_lock(&ctx->h->enc_smoothing_queue.mutex);
			ctx->h->enc_smoothing_buffer_complete = 0;
			pthread_mutex_unlock(&ctx->h->enc_smoothing_queue.mutex);
#if 0
			fprintf(stderr, MESSAGE_PREFIX " Speedcontrol reset\n");
			x264_speedcontrol_sync( s, enc_params->avc_param.sc.i_buffer_size, enc_params->avc_param.sc.f_buffer_init, 0 );
#endif
			ctx->h->video_encoder_drop = 0;
		}
		pthread_mutex_unlock(&ctx->h->drop_mutex);

		/* Input colorspace from decklink (through the upstream dither filter), is always 8bit YUV420P. */
		obe_raw_frame_t *rf = ctx->encoder->queue.queue[0];
		ctx->raw_frame_count++;
		pthread_mutex_unlock(&ctx->encoder->queue.mutex);

#if LOCAL_DEBUG
		printf(MESSAGE_PREFIX " popped raw_frame[%" PRIu64 "] -- pts %" PRIi64 "\n", ctx->raw_frame_count, rf->avfm.audio_pts);
#endif

		/* Re-using the prior frame, remove previous side data else we append. Bug */
#if 0
		av_frame_remove_side_data(frame, AV_FRAME_DATA_ISO14496_USER_UNREGISTERED);
#endif
		av_frame_remove_side_data(frame, AV_FRAME_DATA_AFD);
		ret = av_frame_make_writable(frame);
		if (ret < 0) {
			fprintf(stderr, MESSAGE_PREFIX "Unable to get frame buffer\n");
			exit(1);
		}

		frame->pts = rf->avfm.audio_pts;
		frame->key_frame = 1;
		frame->pict_type = AV_PICTURE_TYPE_I;

		//struct userdata_s *ud = userdata_calloc();

		/* Cache the upstream timing information in userdata. */
		//userdata_set(ud, &rf->avfm);

		ctx->c->sample_aspect_ratio = (AVRational){ rf->sar_width, rf->sar_height };

		uint32_t plane_len[4] = { 0 };
		for (int i = rf->img.planes - 1; i > 0; i--) {
			plane_len[i - 1] = rf->img.plane[i] - rf->img.plane[i - 1];
		}
		if (rf->img.planes == 3) {
			plane_len[2] = plane_len[1];
		}

//		obe_raw_frame_printf(rf);

		int useHW;
		switch (obe_core_encoder_get_stream_format(ctx->encoder)) {
		case VIDEO_AVC_GPU_VAAPI_AVCODEC:
		case VIDEO_HEVC_GPU_VAAPI_AVCODEC:
		case VIDEO_HEVC_GPU_NVENC_AVCODEC:
			useHW = 1;
			break;
		default:
			useHW = 0;
			break;
		}

		if (useHW) {
			AVFrame *hw_frame = av_frame_alloc();
			hw_frame->pts = rf->avfm.audio_pts;
			hw_frame->sample_aspect_ratio = (AVRational){ rf->sar_width, rf->sar_height };
			frame->sample_aspect_ratio = (AVRational){ rf->sar_width, rf->sar_height };
			av_hwframe_get_buffer(ctx->c->hw_frames_ctx, hw_frame, 0);

                        /* Technically it should be:
                         *  Y size = (ctx->frame_width * ctx->frame_height)
                         *  U size += ((ctx->frame_width * ctx->frame_height) / 4)
                         *  V size += ((ctx->frame_width * ctx->frame_height) / 4)
                         */
                        //uint8_t *f = (uint8_t *)malloc(ctx->frame_width * 2 * ctx->frame_height);
                        uint8_t *f = (uint8_t *)frame->data[0];

                        uint8_t *dst_y = f;
                        //uint8_t *dst_uv = f + (ctx->frame_width * ctx->frame_height);
                        uint8_t *dst_uv = (uint8_t *)frame->data[1];

                        /* This costs a few percent of a cpu */
                        I420ToNV12(
                                rf->img.plane[0], ctx->frame_width,
                                rf->img.plane[1], ctx->frame_width / 4,
                                rf->img.plane[2], ctx->frame_width / 4,
                                dst_y, ctx->frame_width,
                                dst_uv, ctx->frame_width / 2,
                                ctx->frame_width, ctx->frame_height);

			ret = av_hwframe_transfer_data(hw_frame, frame, 0);
			if (ret < 0) {
				fprintf(stderr, MESSAGE_PREFIX "HW xfer failed\n");
				exit(1);
			}

			_encode_frame(ctx, rf, hw_frame);

			av_frame_free(&hw_frame);
//			av_frame_free(&frame);
		} else {
			memcpy(frame->data[0], rf->img.plane[0], plane_len[0]);
			memcpy(frame->data[1], rf->img.plane[1], plane_len[1]);
			memcpy(frame->data[2], rf->img.plane[2], plane_len[2]);
			//_av_frame_dump(frame);

			_encode_frame(ctx, rf, frame);
		}

		/* TODO: Add the SCTE35 changes to compute codec latency, patch the SCTE35 etc. */

		rf->release_data(rf);
		rf->release_frame(rf);
		remove_from_queue(&ctx->encoder->queue);

	} /* While (1) */

	avcodec_free_context(&ctx->c);
	av_frame_free(&frame);
	av_packet_free(&pkt);

out2:
	free(ctx->enc_params);
out1:
	return NULL;
}

const obe_vid_enc_func_t avc_gpu_avcodec_obe_encoder = { avc_gpu_avcodec_start_encoder };
