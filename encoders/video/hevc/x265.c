/*****************************************************************************
 * x265.c : x265 encoding functions
 *****************************************************************************
 * Copyright (C) 2018-2019 LiveTimeNet Inc. All Rights Reserved.
 *
 * Authors: Steven Toth <stoth@ltnglobal.com>
 * Using the x264 template developed by: Kieran Kunhya <kieran@kunhya.com>
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
#include "encoders/codec_metadata.h"
#include <libavutil/mathematics.h>
#include <x265.h>
#include <libklscte35/scte35.h>

#define LOCAL_DEBUG 0
#define USE_CODEC_CLOCKS 1

/* Debug feature, save each 1080i field picture as YUV to disk. */
#define SAVE_FIELDS 0
#define SKIP_ENCODE 0

#define MESSAGE_PREFIX "[x265]:"

extern int helper_vancprocessor_scte104(obe_vid_enc_params_t *enc_params, struct avmetadata_item_s *item, obe_coded_frame_t *cf);

int g_x265_nal_debug = 0;
int g_x265_monitor_bps = 0;
int g_x265_min_qp = 15; /* TODO: Potential quality limiter at high bitrates? */
int g_x265_min_qp_new = 0;
static int64_t g_frame_duration = 0;
char g_video_encoder_preset_name[64] = { 0 };
char g_video_encoder_tuning_name[64] = { 0 };

#if DEV_ABR
int g_x265_bitrate_bps = 0;
int g_x265_bitrate_bps_new = 0;
#endif

#define SERIALIZE_CODED_FRAMES 0
#if SERIALIZE_CODED_FRAMES
static FILE *sfh = NULL;
static void serialize_coded_frame(obe_coded_frame_t *cf)
{
    if (!sfh)
        sfh = fopen("/storage/dev/x265-out.cf", "wb");
    if (sfh)
        coded_frame_serializer_write(sfh, cf);
}
#endif

/* 64 doubles, an array ordered oldeest to newest.
 * We'll use these to track QP over the last 64 frames.
 */

static int qp_measures_index = 0;
static struct qp_measurements_s
{
	double qp;
	int frameLatency;
} qp_measures[64];

static void qp_measure_init(struct qp_measurements_s *array)
{
	memset(array, 0, sizeof(*array) * 32);
};

/* Not thread safe, add only from a single thread. */
static void qp_measures_add(struct qp_measurements_s *array, double qp, int frameLatency)
{
	(array + (qp_measures_index   & 0x1f))->qp = qp;
	(array + (qp_measures_index++ & 0x1f))->frameLatency = frameLatency;
};

double qp_measures_get_average_qp(struct qp_measurements_s *array)
{
	double n = 0;
	for (int i = 0; i < 32; i++)
		n += (array + i)->qp;

	return n / 32;
};

double qp_measures_get_average_frame_latency(struct qp_measurements_s *array)
{
	double n = 0;
	for (int i = 0; i < 32; i++)
		n += (array + i)->frameLatency;

	return n / 32;
};

void hevc_show_stats()
{
	printf(MESSAGE_PREFIX "qp average over last 64 frames %4.2f\n",
		qp_measures_get_average_qp(&qp_measures[0]));
	printf(MESSAGE_PREFIX "latency average over last 64 frames %4.2f (frames)\n",
		qp_measures_get_average_frame_latency(&qp_measures[0]));
}

/* end - 64 doubles */
struct context_s
{
	obe_vid_enc_params_t *enc_params;
	obe_t *h;
	obe_encoder_t *encoder;

	/* */
	x265_encoder *hevc_encoder;
	x265_param   *hevc_params;
	x265_picture *hevc_picture_in;
	x265_picture *hevc_picture_out;

	uint32_t      i_nal;
	x265_nal     *hevc_nals;

	uint64_t      raw_frame_count;
};

/* TODO: Duplicated from video.c */
typedef struct
{
	int planes;
	float width[4];
	float height[4];
	int mod_width;
	int mod_height;
	int bit_depth;
} obe_cli_csp_t;

const static obe_cli_csp_t obe_cli_csps[] =
{
	[AV_PIX_FMT_YUV420P]   = { 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2,  8 },
	[AV_PIX_FMT_NV12]      = { 2, { 1,  1 },     { 1, .5 },     2, 2,  8 },
	[AV_PIX_FMT_YUV420P10] = { 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2, 10 },
	[AV_PIX_FMT_YUV422P10] = { 3, { 1, .5, .5 }, { 1, 1, 1 },   2, 2, 10 },
	[AV_PIX_FMT_YUV420P16] = { 3, { 1, .5, .5 }, { 1, .5, .5 }, 2, 2, 16 },
};

static int csp_num_interleaved( int csp, int plane )
{
	return (csp == AV_PIX_FMT_NV12 && plane == 1) ? 2 : 1;
}
/* end -- Duplicated from video.c */

static void x265_picture_free_userSEI(x265_picture *p)
{
	if (p->userSEI.numPayloads) {
		for (int i = 0; i < p->userSEI.numPayloads; i++) {
			free(p->userSEI.payloads[i].payload);
			p->userSEI.payloads[i].payload = NULL;
			p->userSEI.payloads[i].payloadSize = 0;
		}
		free(p->userSEI.payloads);
		p->userSEI.numPayloads = 0;
	}
	p->userSEI.payloads = NULL;
}

/* Free the shallow copy. */
static void x265_picture_free_all(x265_picture *pic)
{
	/* Originally I was copying the planes also. We
	 * can probably refactor this function away.
	 */
	free(pic->planes[0]);
	x265_picture_free(pic);
}

static void x265_picture_analyze_stats(struct context_s *ctx, x265_picture *pic)
{
	x265_frame_stats *s = &pic->frameData;

	qp_measures_add(&qp_measures[0], s->qp, s->frameLatency);

static int64_t last_dts = 0;
static int64_t last_pts = 0;

	if (g_x265_nal_debug & 0x01) {
		printf(MESSAGE_PREFIX
			"poc %8d type %c bits %9" PRIu64 " latency %d encoderOrder %8d sceneCut:%d qp %6.2f pts %13" PRIi64 " dts %13" PRIi64 " dtsdiff %" PRIi64 " ptsdiff %" PRIi64 "\n",
			s->poc,
			s->sliceType,
			s->bits,
			s->frameLatency,	/* Latency in terms of number of frames between when the frame was given in and when the frame is given out. */
			s->encoderOrder,
			s->bScenecut,
			s->qp,
			((pic->pts) / 300) + 900000,
			((pic->dts) / 300) + 900000,
			(pic->dts - last_dts) / 300,
			(pic->pts - last_pts) / 300);

		last_dts = pic->dts;
		last_pts = pic->pts;

		if (s->bScenecut)
			printf("\n");
	}
}

#if SAVE_FIELDS
static void x265_picture_save(x265_picture *pic)
{
	static int index = 0;

	char fn[64];
	snprintf(fn, sizeof(fn), "field%06d.yuv", index++);
	FILE *fh = fopen(fn, "wb");

	/* Copy all of the planes. */
	uint32_t plane_len[4] = { 0 };
	for (int i = 2; i > 0; i--) {
		plane_len[i - 1] = pic->planes[i] - pic->planes[i - 1];
	}
	if (3) {
		plane_len[2] = plane_len[1];
	}

	//printf("stride[0] = %d\n", pic->stride[0]);
	if (pic->stride[0] == 1280) {
		/* TODO: Highly specific to 720p */
		plane_len[0] = 1280 * 720;
		plane_len[1] = plane_len[0] / 4;
		plane_len[2] = plane_len[0] / 4;
	} else
	if (pic->stride[0] == 1920) {
		/* TODO: Highly specific to 1080i */
		plane_len[0] = 1920 * 540;
		plane_len[1] = plane_len[0] / 4;
		plane_len[2] = plane_len[0] / 4;
	}

	for (int i = 0; i < 3; i++) {
		if (pic->planes[i]) {
			fwrite(pic->planes[i], 1, plane_len[i], fh);
		}
	}

	fclose(fh);
}
#endif

/* Deep / full copy the object and associated pixel planes. */
static x265_picture *x265_picture_copy(x265_picture *pic)
{
	x265_picture *p = malloc(sizeof(*p));
	memcpy(p, pic, sizeof(*p));

	/* Copy all of the planes. */
	uint32_t plane_len[4] = { 0 };
	for (int i = 2; i > 0; i--) {
		plane_len[i - 1] = pic->planes[i] - pic->planes[i - 1];
	}
	if (3) {
		plane_len[2] = plane_len[1];
	}

	uint32_t alloc_size = 0;
	for (int i = 0; i < 3; i++)
		alloc_size += plane_len[i];

        for (int i = 0; i < 3; i++) {

                if (i == 0 && pic->planes[i]) {
                        p->planes[i] = (uint8_t *)malloc(alloc_size);
                        memcpy(p->planes[i], pic->planes[i], alloc_size);
                }
                if (i > 0) {
                        p->planes[i] = p->planes[i - 1] + plane_len[i - 1];
                }

        }

	/* Note that this doesn't copy the SEI */

	return p;
}

static void x265_picture_copy_plane_ptrs(x265_picture *dst, x265_picture *src)
{
	for (int n = 0; n < 3; n++) {
		dst->planes[n] = src->planes[n];
	}
}

/* For a 420 interlaced image, 8 bit, adjust the planes that typically point to the
 * top field, to point to the bottom field.
 */
static void x265_picture_interlaced__update_planes_bottom_field(struct context_s *ctx, x265_picture *pic)
{
	/* Tamper with plane offets to access the bottom field, adjust start of plane to reflect
	 * the second field then push this into the codec.
	 * This is a destructive process, the previous field ptrs are destroyed.
	 */
	for (int n = 0; n < 3; n++) {
		if (n == 0)
			pic->planes[n] += (pic->stride[n] * (ctx->enc_params->avc_param.i_height / 2));
		else
			pic->planes[n] += (pic->stride[n] * (ctx->enc_params->avc_param.i_height / 4));
	}
}

/* Convert an 8bit interleaved pair of fields into TFF top/bottom image. */
static int convert_interleaved_to_topbottom(struct context_s *ctx, obe_raw_frame_t *raw_frame)
{
	/* Create a new image, copy/reformat the source image then discard it. */
	obe_image_t *img = &raw_frame->img;
	obe_image_t tmp_image = {0};
	obe_image_t *out = &tmp_image;

	tmp_image.csp = img->csp == AV_PIX_FMT_YUV422P10 ? AV_PIX_FMT_YUV422P : AV_PIX_FMT_YUV420P;
	tmp_image.width = raw_frame->img.width;
	tmp_image.height = raw_frame->img.height;
        const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(raw_frame->alloc_img.csp);
	tmp_image.planes = d->nb_components;
	tmp_image.format = raw_frame->img.format;

	if (av_image_alloc(tmp_image.plane, tmp_image.stride, tmp_image.width, tmp_image.height + 1, tmp_image.csp, 16) < 0) {
		syslog(LOG_ERR, "%s() Malloc failed", __func__);
		return -1;
	}

	for (int i = 0; i < img->planes; i++) {

		int num_interleaved = csp_num_interleaved(img->csp, i);
		int height = obe_cli_csps[img->csp].height[i] * img->height;
		int width = obe_cli_csps[img->csp].width[i] * img->width / num_interleaved;
/*
plane0 num_interleaved 1 width 1920 height 1080
plane1 num_interleaved 1 width 960 height 540
plane2 num_interleaved 1 width 960 height 540

plane0 num_interleaved 1 width 720 height 480
plane1 num_interleaved 1 width 360 height 240
plane2 num_interleaved 1 width 360 height 240
printf("plane%d num_interleaved %d width %d height %d\n", i, num_interleaved, width, height);
*/
/*
 * in->stride[0] = 720  out->stride[0] = 720
 * in->stride[1] = 368  out->stride[1] = 368
 * in->stride[2] = 368  out->stride[2] = 368
 * printf("in->stride[%d] = %d  out->stride[%d] = %d\n", i, img->stride[i], i, out->stride[i]);
 */
		/* Lets assume the first row belongs in the first field (TFF). */
		uint8_t *src = img->plane[i];
		uint8_t *dstTop = out->plane[i];
		uint8_t *dstBottom = out->plane[i] + ((out->stride[i] * height) / 2);

		for (int j = 0; j < height / 2; j++) {

			/* First field. */
			memcpy(dstTop, src, width);
			src += img->stride[i];
			dstTop += out->stride[i];

			/* Second field. */
			memcpy(dstBottom, src, width);
			src += img->stride[i];
			dstBottom += out->stride[i];

		}
	}

	raw_frame->release_data(raw_frame);
	memcpy(&raw_frame->alloc_img, &tmp_image, sizeof(obe_image_t));
	memcpy(&raw_frame->img, &raw_frame->alloc_img, sizeof(obe_image_t));

	return 0;
}

/* Convert a obe_raw_frame_t into a x264_picture_t struct.
 * Incoming frame is colorspace YUV420P.
 */
static int convert_obe_to_x265_pic(struct context_s *ctx, x265_picture *p, struct opaque_ctx_s *ud, obe_raw_frame_t *rf)
{
	obe_image_t *img = &rf->img;
	int count = 0, idx = 0;

	if (ctx->enc_params->avc_param.b_interlaced) {
		/* Convert from traditional interlaced interleaved lines to a top/bottom image. */
		/* TODO: deal with field dominance. */
		convert_interleaved_to_topbottom(ctx, rf);
	}

#if 0
	/* Save raw image to disk. Its 8bit. Convert for viewing purposes.
	 * Interlaced content will still be interlaced.
	 * Convert to png with:
	 * ffmpeg -y -f rawvideo -pix_fmt yuv420p -s 1920x1080 -i video-frame-00000413.yuv -vframes 1 new.png
	 */
	obe_image_save(&rf->img);
#endif

	/* Dealloc any previous SEI pointers and payload, if we had any. */
	x265_picture_free_userSEI(p);
	x265_picture_init(ctx->hevc_params, p);

	if (obe_core_get_platform_model() == 573) {
		int ve_q = obe_core_get_output_stream_queue_depth(ctx->h, 0);
		if (ve_q > 1) {
			/* Lower the QP drastically to try and quickly catchup with the backlog */
			//p->forceqp = 40;
		}
	}

	p->sliceType = X265_TYPE_AUTO;
	p->bitDepth = 8;
	p->stride[0] = img->stride[0];
	p->stride[1] = img->stride[1]; // >> x265_cli_csps[p->colorSpace].width[1];
	p->stride[2] = img->stride[2]; // >> x265_cli_csps[p->colorSpace].width[2];

	for (int i = 0; i < 3; i++) {
		p->stride[i] = img->stride[i];
		p->planes[i] = img->plane[i];
	}

	p->colorSpace = img->csp == AV_PIX_FMT_YUV422P || img->csp == AV_PIX_FMT_YUV422P10 ? X265_CSP_I422 : X265_CSP_I420;
#ifdef HIGH_BIT_DEPTH
	p->colorSpace |= X265_CSP_HIGH_DEPTH;
#endif

	for (int i = 0; i < rf->num_user_data; i++) {
		/* Only give correctly formatted data to the encoder */
		if (rf->user_data[i].type == USER_DATA_AVC_REGISTERED_ITU_T35 ||
			rf->user_data[i].type == USER_DATA_AVC_UNREGISTERED) {
			count++;
		}
	}

	if (g_sei_timestamping) {
		/* Create space for unregister data, containing before and after timestamps. */
		count += 1;
	}

	p->userSEI.numPayloads = count;

	if (p->userSEI.numPayloads) {
		p->userSEI.payloads = malloc(p->userSEI.numPayloads * sizeof(*p->userSEI.payloads));
		if (!p->userSEI.payloads)
			return -1;

		for (int i = 0; i < rf->num_user_data; i++) {
			/* Only give correctly formatted data to the encoder */

			if (rf->user_data[i].type == USER_DATA_AVC_REGISTERED_ITU_T35 || rf->user_data[i].type == USER_DATA_AVC_UNREGISTERED) {
				p->userSEI.payloads[idx].payloadType = rf->user_data[i].type;
				p->userSEI.payloads[idx].payloadSize = rf->user_data[i].len;
				p->userSEI.payloads[idx].payload = malloc(p->userSEI.payloads[idx].payloadSize);
				memcpy(p->userSEI.payloads[idx].payload, rf->user_data[i].data, p->userSEI.payloads[idx].payloadSize);
				idx++;
			} else {
				syslog(LOG_WARNING, MESSAGE_PREFIX " Invalid user data presented to encoder - type %i\n", rf->user_data[i].type);
				printf(MESSAGE_PREFIX " (1) Invalid user data presented to encoder - type %i\n", rf->user_data[i].type);
				free(rf->user_data[i].data);
			}
		}
	} else if (rf->num_user_data) {
		for (int i = 0; i < rf->num_user_data; i++) {
			syslog(LOG_WARNING, MESSAGE_PREFIX " Invalid user data presented to encoder - type %i\n", rf->user_data[i].type);
			printf(MESSAGE_PREFIX " (2) Invalid user data presented to encoder - type %i\n", rf->user_data[i].type);
			free(rf->user_data[i].data);
		}
	}

	if (g_sei_timestamping) {
		x265_sei_payload *x;

		/* Start time - Always the last SEI */
		static uint32_t framecount = 0;
		x = &p->userSEI.payloads[count - 1];
		x->payloadType = (SEIPayloadType)USER_DATA_AVC_UNREGISTERED;
		x->payloadSize = SEI_TIMESTAMP_PAYLOAD_LENGTH;
		x->payload = sei_timestamp_alloc(); /* Freed when we enter the function prior to pic re-init. */

		struct timeval tv;
		gettimeofday(&tv, NULL);

		sei_timestamp_field_set(x->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 1, framecount);
		sei_timestamp_field_set(x->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 2, avfm_get_hw_received_tv_sec(&rf->avfm));
		sei_timestamp_field_set(x->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 3, avfm_get_hw_received_tv_usec(&rf->avfm));
		sei_timestamp_field_set(x->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 4, tv.tv_sec);
		sei_timestamp_field_set(x->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 5, tv.tv_usec);
		sei_timestamp_field_set(x->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 6, 0);
		sei_timestamp_field_set(x->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 7, 0);
		sei_timestamp_field_set(x->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 8, 0);
		sei_timestamp_field_set(x->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 9, 0);

		/* The remaining 8 bytes (time exit from compressor fields)
		 * will be filled when the frame exists the compressor. */
		framecount++;
	}

	return 0;
}

static const char *sliceTypeDesc(int num)
{
	switch (num) {
	case 0: return "AUTO";
	case 1: return "IDR";
	case 2: return "I";
	case 3: return "P";
	case 4: return "BREF";
	case 5: return "B";
	default: return "?";
	}
}

static int dispatch_payload(struct context_s *ctx, const unsigned char *buf, int lengthBytes, int64_t arrival_time)
{
	obe_coded_frame_t *cf = new_coded_frame(ctx->encoder->output_stream_id, lengthBytes);
	if (!cf) {
		fprintf(stderr, MESSAGE_PREFIX " unable to alloc a new coded frame\n");
		return -1;
	}

	if (g_x265_nal_debug & 0x02) {

		int64_t codecms = -1;
		if (g_sei_timestamping) {
			int offset = ltn_uuid_find(buf, lengthBytes);
			if (offset >= 0) {
				codecms = sei_timestamp_query_codec_latency_ms(&buf[offset], lengthBytes - offset);
			}
		}

		printf(MESSAGE_PREFIX " --  acquired                                                                            pts %13" PRIi64 " dts %13" PRIi64 ", ",
			ctx->hevc_picture_out->pts,
			ctx->hevc_picture_out->dts);
		printf("sliceType %d [%4s]",
			ctx->hevc_picture_out->sliceType,
			sliceTypeDesc(ctx->hevc_picture_out->sliceType));
		if (!g_sei_timestamping) {
			printf("\n");
		} else {
			printf(", codec frame time %" PRIi64 "ms\n", codecms);
		}
	}

	static int64_t last_hw_pts = 0;
	struct opaque_ctx_s *out_ud = ctx->hevc_picture_out->userData; 
	if (out_ud) {
		/* Make sure we push the original hardware timing into the new frame. */
		memcpy(&cf->avfm, &out_ud->avfm, sizeof(struct avfm_s));

		cf->pts = out_ud->avfm.audio_pts;
		last_hw_pts = out_ud->avfm.audio_pts;
	} else {
		//fprintf(stderr, MESSAGE_PREFIX " missing pic out userData\n");
		cf->pts = last_hw_pts;
	}

	memcpy(cf->data, buf, lengthBytes);
	cf->len                      = lengthBytes;
	cf->type                     = CF_VIDEO;
#if USE_CODEC_CLOCKS
	/* Rely on the PTS time that comes from the s/w codec. */
	cf->pts                      = ctx->hevc_picture_out->pts + (2 * g_frame_duration);
	cf->real_pts                 = ctx->hevc_picture_out->pts + (2 * g_frame_duration);
	cf->real_dts                 = ctx->hevc_picture_out->dts + (2 * g_frame_duration);
#else
	/* Recalculate a new real pts/dts based on the hardware time, not the codec time. */
	cf->real_pts                 = cf->pts;
	cf->real_dts                 = ctx->hevc_picture_out->dts;
#endif

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
        	cf->cpb_initial_arrival_time = cf->real_dts - 1351350; /* 3x frame interval */
	} else {
        	cf->cpb_initial_arrival_time = cf->real_pts - 1351350; /* 3x frame interval */
	}

	double bit_rate = ctx->enc_params->avc_param.rc.i_vbv_max_bitrate;
        double fraction = bit_rate / 216000.0;
        double estimated = ((double)cf->len / fraction);
        double estimated_final = estimated + (double)cf->cpb_initial_arrival_time;
        cf->cpb_final_arrival_time  = estimated_final;

	cf->priority = IS_X265_TYPE_I(ctx->hevc_picture_out->sliceType);
	cf->random_access = IS_X265_TYPE_I(ctx->hevc_picture_out->sliceType);

	/* If interlaced is active, and the pts time has repeated, increment clocks
	 * by the field rate (frame_duration / 2). Why do we increment clocks? We can either
	 * take the PTS from the codec (which repeats) or derive the PTS from the actual
	 * capture hardware. The codec PTS is guaranteed sequence, a problem during signal loss.
	 * The hardware clock is non-sequential - a better clock to handle signal loss.
	 */
#if USE_CODEC_CLOCKS
#else
	static int64_t last_dispatch_pts = 0;
#endif
	if (ctx->enc_params->avc_param.b_interlaced) {
#if USE_CODEC_CLOCKS
#else
		if (cf->pts == last_dispatch_pts) {
			cf->pts += (g_frame_duration / 2);
			/* Recalculate a new real pts/dts based on the hardware time, not the codec time. */
			cf->real_pts = cf->pts;
			cf->real_dts = cf->pts;
		}
#endif
	}
#if USE_CODEC_CLOCKS
#else
	last_dispatch_pts = cf->pts;
#endif

	if (g_x265_nal_debug & 0x02) {
		printf(MESSAGE_PREFIX " --    output                                                                            pts %13" PRIi64 " dts %13" PRIi64 "\n",
			cf->pts,
			cf->real_dts);
	}

	if (g_x265_nal_debug & 0x04)
		coded_frame_print(cf);

	/* Process any attached metadata.
	 * Eg. Handle the SCTE104 to 35 conversion, with the right timing, batsed on video codec latency.
	 */
	if (out_ud && out_ud->metadata.count > 0) {
		/* We need to process any associated metadata before we destroy the frame. */

		for (int i = 0; i < out_ud->metadata.count; i++) {
			/* Process any scte35, trash any others we don't understand. */

			struct avmetadata_item_s *e = out_ud->metadata.array[i];
			switch (e->item_type) {
            case AVMETADATA_VANC_SCTE104:
				helper_vancprocessor_scte104(ctx->enc_params, e, cf);
				break;
			default:
				printf("%s() warning, no handling of item type 0x%x\n", __func__, e->item_type);
			}

			avmetadata_item_free(e);
			out_ud->metadata.array[i] = NULL;
		}
		avmetadata_reset(&out_ud->metadata);
	}

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
		cf->arrival_time = arrival_time;
#if SERIALIZE_CODED_FRAMES
		serialize_coded_frame(cf);
#endif
		add_to_queue(&ctx->h->mux_queue, cf);
		//printf(MESSAGE_PREFIX " Encode Latency %"PRIi64" \n", obe_mdate() - cf->arrival_time);
	} else {
#if SERIALIZE_CODED_FRAMES
		serialize_coded_frame(cf);
#endif
		add_to_queue(&ctx->h->enc_smoothing_queue, cf);
	}
	
	if (out_ud) {
		codec_metadata_free(out_ud);
		out_ud = NULL;
		ctx->hevc_picture_out->userData = 0;
	}

	return 0;
}

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
		if (g_x265_monitor_bps) {
			printf(MESSAGE_PREFIX " codec output %.02f (Mb/ps) @ %s", dbps, ctime(&now));
		}
	}
	codec_bps_current += (lengthBytes * 8);
}

static void _process_nals(struct context_s *ctx, int64_t arrival_time)
{
	if (ctx->i_nal == 0)
		return;

	if (g_sei_timestamping) {
		/* Walk through each of the NALS and insert current time into any LTN sei timestamp frames we find. */
		for (int m = 0; m < ctx->i_nal; m++) {
			int offset = ltn_uuid_find(&ctx->hevc_nals[m].payload[0], ctx->hevc_nals[m].sizeBytes);
			if (offset >= 0) {
				struct timeval tv;
				gettimeofday(&tv, NULL);

				/* Add the time exit from compressor seconds/useconds. */
				sei_timestamp_field_set(&ctx->hevc_nals[m].payload[offset], ctx->hevc_nals[m].sizeBytes - offset, 6, tv.tv_sec);
				sei_timestamp_field_set(&ctx->hevc_nals[m].payload[offset], ctx->hevc_nals[m].sizeBytes - offset, 7, tv.tv_usec);
			}
		}
	}

	/* Collapse N nals into a single allocation, so we can submit a single coded_frame, with a single clock. */
	int nalbuffer_size = 0;
	for (int z = 0; z < ctx->i_nal; z++) {
		nalbuffer_size += ctx->hevc_nals[z].sizeBytes;
	}

	unsigned char *nalbuffer = malloc(((nalbuffer_size / getpagesize()) + 1) * getpagesize());
	int offset = 0;
	for (int z = 0; z < ctx->i_nal; z++) {
		memcpy(nalbuffer + offset, ctx->hevc_nals[z].payload, ctx->hevc_nals[z].sizeBytes);
		offset += ctx->hevc_nals[z].sizeBytes;
	}

	dispatch_payload(ctx, nalbuffer, nalbuffer_size, arrival_time);
	free(nalbuffer);

	/* Monitor bps for sanity... */
	_monitor_bps(ctx, nalbuffer_size);
}

#if DEV_ABR
static int rapid_reconfigure_encoder(struct context_s *ctx)
{
	int ret;
	char val[64];

	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.rc.i_bitrate);
	x265_param_parse(ctx->hevc_params, "bitrate", val);
	printf(MESSAGE_PREFIX "%s() bitrate %s\n", __func__, val);

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY) {
		/* Found that in lowest mode, obe doesn't accept the param, but the codec reports underruns. */
		ctx->enc_params->avc_param.rc.i_vbv_buffer_size = ctx->enc_params->avc_param.rc.i_vbv_max_bitrate;
	}
	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.rc.i_vbv_buffer_size);
	x265_param_parse(ctx->hevc_params, "vbv-bufsize", val);
	printf(MESSAGE_PREFIX "%s() vbv-bufsize = %s\n", __func__, val);

	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.rc.i_vbv_max_bitrate);
	x265_param_parse(ctx->hevc_params, "vbv-maxrate", val);
	printf(MESSAGE_PREFIX "%s() vbv-maxrate = %d\n", __func__, ctx->enc_params->avc_param.rc.i_vbv_max_bitrate);

	//int ret = x265_encoder_reconfig_zone(ctx->hevc_encoder, ctx->hevc_params);
	ret =  x265_encoder_reconfig(ctx->hevc_encoder, ctx->hevc_params);

	return ret;
}
#endif

static int reconfigure_encoder(struct context_s *ctx)
{
	x265_param_free(ctx->hevc_params);

	ctx->hevc_params = x265_param_alloc();
	if (!ctx->hevc_params) {
		fprintf(stderr, MESSAGE_PREFIX " failed to allocate params\n");
		return -1;
	}

	x265_param_default(ctx->hevc_params);

	char preset_name[64];
	if (strlen(g_video_encoder_preset_name) > 0) {
		snprintf(preset_name, sizeof(preset_name), "%s", g_video_encoder_preset_name);
	} else {
		snprintf(preset_name, sizeof(preset_name), "ultrafast");
	}

	char tuning_name[64];
	if (strlen(g_video_encoder_tuning_name) > 0) {
		snprintf(tuning_name, sizeof(tuning_name), "%s", g_video_encoder_tuning_name);
	} else {
		tuning_name[0] = '\0';
	}

	int ret;
	if (strlen(tuning_name) == 0) {
		ret = x265_param_default_preset(ctx->hevc_params, g_video_encoder_preset_name, NULL);
	} else {
		ret = x265_param_default_preset(ctx->hevc_params, g_video_encoder_preset_name, g_video_encoder_tuning_name);
	}

	if (ret < 0) {
		fprintf(stderr, MESSAGE_PREFIX " failed to set default params\n");
		return -1;
	}


//	ctx->hevc_params->fpsDenom = ctx->enc_params->avc_param.i_fps_den;
//	ctx->hevc_params->fpsNum = ctx->enc_params->avc_param.i_fps_num;

#if 0
                avc_param->rc.i_vbv_max_bitrate = obe_otoi( vbv_maxrate, 0 );
                avc_param->rc.i_vbv_buffer_size = obe_otoi( vbv_bufsize, 0 );
                avc_param->rc.i_bitrate         = obe_otoi( bitrate, 0 );
                avc_param->i_keyint_max        = obe_otoi( keyint, avc_param->i_keyint_max );
                avc_param->rc.i_lookahead      = obe_otoi( lookahead, avc_param->rc.i_lookahead );
                avc_param->i_threads           = obe_otoi( threads, avc_param->i_threads );
#endif

	char val[64];
	if (ctx->enc_params->avc_param.b_interlaced) {
		snprintf(val, sizeof(val), "%dx%d",
			ctx->enc_params->avc_param.i_width,
			ctx->enc_params->avc_param.i_height / 2);
	} else {
		snprintf(val, sizeof(val), "%dx%d", ctx->enc_params->avc_param.i_width, ctx->enc_params->avc_param.i_height);
	}
	x265_param_parse(ctx->hevc_params, "input-res", val);

	if (getenv("OBE_X265_STREAM_INJECT_DETAILS") == NULL)
		x265_param_parse(ctx->hevc_params, "info", "0");

	//x265_param_parse(ctx->hevc_params, "hrd", "1");

	/* The decklink module sets TFF to always true for interlaced formats, which isn't technically
         * correct. NTSC is BFF. A comment suggests NTSC should be BFF but is packed as TFF.
         * We'll obey the params here, and leave this comment that NTSC could be wrong if the original
         * packing issues was specific to a decklink driver version.
         */
	if (ctx->enc_params->avc_param.b_interlaced) {
		if (ctx->enc_params->avc_param.b_tff)
			x265_param_parse(ctx->hevc_params, "interlace", "tff");
		else
			x265_param_parse(ctx->hevc_params, "interlace", "bff");
	} else {
		x265_param_parse(ctx->hevc_params, "interlace", "0");
	}

	ctx->hevc_params->internalCsp = X265_CSP_I420;
	x265_param_parse(ctx->hevc_params, "repeat-headers", "1");

	if (ctx->enc_params->avc_param.b_interlaced) {
		/* X265 wants the rate in fields per second, instead of (progressive) frames per second. */
		snprintf(&val[0], sizeof(val), "%d/%d", ctx->enc_params->avc_param.i_fps_num * 2, ctx->enc_params->avc_param.i_fps_den);
	} else {
		snprintf(&val[0], sizeof(val), "%d/%d", ctx->enc_params->avc_param.i_fps_num, ctx->enc_params->avc_param.i_fps_den);
	}
	x265_param_parse(ctx->hevc_params, "fps", val);

//ctx->enc_params->avc_param.i_keyint_max = 60;
	snprintf(&val[0], sizeof(val), "%d",ctx->enc_params->avc_param.i_keyint_max);
	x265_param_parse(ctx->hevc_params, "keyint", val);
	printf(MESSAGE_PREFIX "keyint = %s\n", val);

	if (obe_core_get_platform_model() == 573) {
		x265_param_parse(ctx->hevc_params, "min-keyint", val);
		printf(MESSAGE_PREFIX "min-keyint = %s\n", val);
	}

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY) {
		/* Found that in lowest mode, obe doesn't accept the param, but the codec reports underruns. */
		ctx->enc_params->avc_param.rc.i_vbv_buffer_size = ctx->enc_params->avc_param.rc.i_vbv_max_bitrate;
		printf(MESSAGE_PREFIX "vbv_bufsize = %d\n", ctx->enc_params->avc_param.rc.i_vbv_buffer_size);
	}
	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.rc.i_vbv_buffer_size);
	x265_param_parse(ctx->hevc_params, "vbv-bufsize", val);

	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.rc.i_vbv_max_bitrate);
	x265_param_parse(ctx->hevc_params, "vbv-maxrate", val);
	printf(MESSAGE_PREFIX "vbv-maxrate = %d\n", ctx->enc_params->avc_param.rc.i_vbv_max_bitrate);
	x265_param_parse(ctx->hevc_params, "vbv-init", "0.9");

	if (ctx->enc_params->avc_param.rc.i_lookahead > 0) {
		snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.rc.i_lookahead);
		printf(MESSAGE_PREFIX "lookahead = %s\n", val); 
		x265_param_parse(ctx->hevc_params, "rc-lookahead", val);
	}

#if 0
	if ((strcmp(preset_name, "faster") == 0) && strcmp(tuning_name, "") == 0) {
		printf(MESSAGE_PREFIX "Assuming coffeelake performance via adjustments\n");
		x265_param_parse(ctx->hevc_params, "ctu", "32");
		x265_param_parse(ctx->hevc_params, "bframes", "8");
		x265_param_parse(ctx->hevc_params, "ref", "3");
	}
#endif

	if (obe_core_get_platform_model() == 573 && ctx->enc_params->avc_param.b_interlaced) {
		g_x265_min_qp = 25;
		printf(MESSAGE_PREFIX "pushing min-qp to %d for 573 with interlaced\n", g_x265_min_qp); 
	}

	snprintf(&val[0], sizeof(val), "%d", g_x265_min_qp);
	printf(MESSAGE_PREFIX "Setting QPmin to %s\n", val);
	x265_param_parse(ctx->hevc_params, "qpmin", val);

	/* 0 Is preferred, which is 'autodetect' */
	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.i_threads);
	x265_param_parse(ctx->hevc_params, "frame-threads", val);

	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.rc.i_bitrate);
	x265_param_parse(ctx->hevc_params, "bitrate", val);
	printf(MESSAGE_PREFIX "bitrate %s\n", val);

	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.i_nal_hrd == 3 ? 0 : 1);
	printf(MESSAGE_PREFIX "strict cbr is %s\n", val);
	x265_param_parse(ctx->hevc_params, "strict-cbr", val);

	if (obe_core_get_platform_model() == 573) {
		if (ctx->enc_params->avc_param.b_interlaced)
			snprintf(val, sizeof(val), "16");
		else
			snprintf(val, sizeof(val), "64");
		printf(MESSAGE_PREFIX "ctu %s\n", val);
		x265_param_parse(ctx->hevc_params, "ctu", val);

		printf(MESSAGE_PREFIX "no-open-gop\n");
		printf(MESSAGE_PREFIX "intra-refresh\n");
		printf(MESSAGE_PREFIX "me is hex\n");
		x265_param_parse(ctx->hevc_params, "no-open-gop", "1");
		x265_param_parse(ctx->hevc_params, "intra-refresh", "1");
		x265_param_parse(ctx->hevc_params, "me", "hex");
		x265_param_parse(ctx->hevc_params, "qpstep", "12");
	}
	x265_param_parse(ctx->hevc_params, "aud", "1");
#if 0
	snprintf(&val[0], sizeof(val), "%d", 1);
	printf(MESSAGE_PREFIX "hrd is %s\n", val);
	x265_param_parse(ctx->hevc_params, "hrd", val);

	snprintf(&val[0], sizeof(val), "%d", 1);
	printf(MESSAGE_PREFIX "frame duplication is %s\n", val);
	x265_param_parse(ctx->hevc_params, "frame-dup", val);
#endif
	return 0;
}

/* OBE will pass us a AVC struct initially. Pull out any important pieces
 * and pass those to x265.
 */
static void *x265_start_encoder( void *ptr )
{
	struct context_s ectx, *ctx = &ectx;
	memset(ctx, 0, sizeof(*ctx));

	qp_measure_init(&qp_measures[0]);

	ctx->enc_params = ptr;
	ctx->h = ctx->enc_params->h;
	ctx->encoder = ctx->enc_params->encoder;
	int ret;

	printf(MESSAGE_PREFIX "Starting encoder: %s\n",
		stream_format_name(obe_core_encoder_get_stream_format(ctx->encoder)));
	printf(MESSAGE_PREFIX "%s() preset_name = %s\n", __func__, g_video_encoder_preset_name);
	printf(MESSAGE_PREFIX "%s() tuning_name = %s\n", __func__, g_video_encoder_tuning_name);

	reconfigure_encoder(ctx);

	ctx->hevc_picture_in = x265_picture_alloc();
	if (!ctx->hevc_picture_in) {
		fprintf(stderr, MESSAGE_PREFIX " failed to allocate picture\n");
		goto out2;
	}

	ctx->hevc_picture_out = x265_picture_alloc();
	if (!ctx->hevc_picture_out) {
		fprintf(stderr, MESSAGE_PREFIX " failed to allocate picture\n");
		goto out3;
	}

	/* Fix this? its AVC specific. */
	ctx->encoder->encoder_params = malloc(sizeof(ctx->enc_params->avc_param) );
	if (!ctx->encoder->encoder_params) {
		pthread_mutex_unlock(&ctx->encoder->queue.mutex);
		fprintf(stderr, MESSAGE_PREFIX " failed to allocate encoder params\n");
		goto out4;
	}
	memcpy(ctx->encoder->encoder_params, &ctx->enc_params->avc_param, sizeof(ctx->enc_params->avc_param));

	ctx->hevc_encoder = x265_encoder_open(ctx->hevc_params);
	if (!ctx->hevc_encoder) {
		pthread_mutex_unlock(&ctx->encoder->queue.mutex);
		fprintf(stderr, MESSAGE_PREFIX " failed to open encoder\n");
		goto out5;
	}

	/* Lock the mutex until we verify and fetch new parameters */
	pthread_mutex_lock(&ctx->encoder->queue.mutex);

	ctx->encoder->is_ready = 1;

	g_frame_duration = av_rescale_q( 1, (AVRational){ ctx->enc_params->avc_param.i_fps_den, ctx->enc_params->avc_param.i_fps_num}, (AVRational){ 1, OBE_CLOCK } );
	printf("frame_duration = %" PRIi64 "\n", g_frame_duration);
	//buffer_duration = frame_duration * ctx->enc_params->avc_param.sc.i_buffer_size;

	/* Wake up the muxer */
	pthread_cond_broadcast(&ctx->encoder->queue.in_cv);
	pthread_mutex_unlock(&ctx->encoder->queue.mutex);

	x265_picture_init(ctx->hevc_params, ctx->hevc_picture_in);

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
			ctx->h->video_encoder_drop = 0;
		}
		pthread_mutex_unlock(&ctx->h->drop_mutex);

		obe_raw_frame_t *rf = ctx->encoder->queue.queue[0];
		ctx->raw_frame_count++;
		pthread_mutex_unlock(&ctx->encoder->queue.mutex);

#if LOCAL_DEBUG
		//printf(MESSAGE_PREFIX " popped a raw frame[%" PRIu64 "] -- pts %" PRIi64 "\n", ctx->raw_frame_count, rf->avfm.audio_pts);
#endif

		/* Allocate an opaque struct, add metadata to this and attached
		 * this to the frame we're passing to the codec.
		 * We'll extract the metadata when the frame returns form the codec.
		 * for SCTE and other processing.
		 */
		struct opaque_ctx_s *ud = codec_metadata_alloc();

		/* convert obe_frame_t into x264 friendly struct.
		 * Bundle up and incoming SEI etc, into the userdata context
		 */
		if (convert_obe_to_x265_pic(ctx, ctx->hevc_picture_in, ud, rf) < 0) {
			fprintf(stderr, MESSAGE_PREFIX " pic prepare failed\n");
			break;
		}

		ctx->hevc_picture_in->userData = ud;

		/* Cache the upstream timing information in userdata. */
		codec_metadata_set_avfm(ud, &rf->avfm);
		codec_metadata_set_avmetadata(ud, &rf->metadata);

		/* If the AFD has changed, then change the SAR. x264 will write the SAR at the next keyframe
		 * TODO: allow user to force keyframes in order to be frame accurate.
		 */
		if (rf->sar_width != ctx->enc_params->avc_param.vui.i_sar_width ||
			rf->sar_height != ctx->enc_params->avc_param.vui.i_sar_height) {

			ctx->enc_params->avc_param.vui.i_sar_width  = rf->sar_width;
			ctx->enc_params->avc_param.vui.i_sar_height = rf->sar_height;
//			pic.param = &enc_params->avc_param;

		}

		int leave = 0;
		while (!leave) { 
			/* Compress PIC to NALS. */
			/* Once the pipeline is completely full, x265_encoder_encode() will block until the next output picture is complete. */

			if (rf) {
				ctx->hevc_picture_in->pts = rf->avfm.audio_pts;

#if DEV_ABR
				if (g_x265_bitrate_bps_new) {
					g_x265_bitrate_bps_new = 0;

					//x265_encoder_close(ctx->hevc_encoder);
					ctx->enc_params->avc_param.rc.i_bitrate = g_x265_bitrate_bps / 1000;
					ctx->enc_params->avc_param.rc.i_vbv_max_bitrate = ctx->enc_params->avc_param.rc.i_bitrate - 2000;

printf("Restarting codec with new bitrate %dkbps, vbvmax %d\n", ctx->enc_params->avc_param.rc.i_bitrate, ctx->enc_params->avc_param.rc.i_vbv_max_bitrate);
					ret = rapid_reconfigure_encoder(ctx);
					if (ret < 0) {
						fprintf(stderr, MESSAGE_PREFIX " failed to reconfigre encoder.\n");
						exit(1);
					}
				}
#endif
				if (g_x265_min_qp_new) {
					g_x265_min_qp_new = 0;
					x265_encoder_close(ctx->hevc_encoder);

printf("Restarting codec with new params\n");
					ret = reconfigure_encoder(ctx);
					if (ret < 0) {
						fprintf(stderr, MESSAGE_PREFIX " failed to reconfigre encoder.\n");
						exit(1);
					}

					ctx->hevc_encoder = x265_encoder_open(ctx->hevc_params);
					if (!ctx->hevc_encoder) {
						fprintf(stderr, MESSAGE_PREFIX " failed to open encoder, again.\n");
						exit(1);
					}
printf("Restarting codec with new params ... done\n");

				}
				if (ctx->enc_params->avc_param.b_interlaced) {

					/* Complete image copy of the original top/bottom frame. We'll submit this to the encoder as a bottom field
					 * sans any structs we don't specifically want to copy.
					 */
					x265_picture *cpy = x265_picture_copy(ctx->hevc_picture_in);
					cpy->userData = NULL;
					cpy->rcData = NULL;
					cpy->quantOffsets = NULL;
					cpy->userSEI.numPayloads = 0;
					cpy->userSEI.payloads = NULL;

					ctx->i_nal = 0;
					/* TFF or BFF? */
#if SAVE_FIELDS
					x265_picture_save(ctx->hevc_picture_in);
#endif
#if SKIP_ENCODE
					ret = 0;
#else
					ret = x265_encoder_encode(ctx->hevc_encoder, &ctx->hevc_nals, &ctx->i_nal, ctx->hevc_picture_in, ctx->hevc_picture_out);
#endif
					if (ret > 0) {
						x265_picture_analyze_stats(ctx, ctx->hevc_picture_out);
						_process_nals(ctx, rf->arrival_time);
						ret = 0;
						ctx->i_nal = 0;
					}

					ctx->i_nal = 0;

					/* Backup the frame pointers, restore them later, so a free() doesn't complain that
					 * we've adjusted the pointers.
					 */
					x265_picture cpy_ptrs;
					x265_picture_copy_plane_ptrs(&cpy_ptrs, cpy);

					/* Adjust the planes, point them to the bottom field. */
					x265_picture_interlaced__update_planes_bottom_field(ctx, cpy);

#if SAVE_FIELDS
					x265_picture_save(cpy);
#endif

#if USE_CODEC_CLOCKS
					cpy->pts = rf->avfm.audio_pts + (g_frame_duration / 2);
#endif
#if SKIP_ENCODE
					ret = 0;
#else
					ret = x265_encoder_encode(ctx->hevc_encoder, &ctx->hevc_nals, &ctx->i_nal, cpy, ctx->hevc_picture_out);
#endif
					if (ret > 0) {
						x265_picture_analyze_stats(ctx, ctx->hevc_picture_out);
						_process_nals(ctx, rf->arrival_time);
						ret = 0;
						ctx->i_nal = 0;
					}

					/* Restore our original pointers and free the object. */
					x265_picture_copy_plane_ptrs(cpy, &cpy_ptrs);
					x265_picture_free_all(cpy);
				} else {
#if SAVE_FIELDS
					x265_picture_save(ctx->hevc_picture_in);
#endif
#if SKIP_ENCODE
					ret = 0;
#else

#define MEASURE_CODEC_LATENCY 0

#if MEASURE_CODEC_LATENCY
					printf("x264 avfm/audio pts going in %" PRIi64 ", pts %" PRIi64 "\n",
						ud->avfm.audio_pts, ctx->hevc_picture_in->pts);
#endif

					ret = x265_encoder_encode(ctx->hevc_encoder, &ctx->hevc_nals, &ctx->i_nal, ctx->hevc_picture_in, ctx->hevc_picture_out);

#if MEASURE_CODEC_LATENCY
					if (ret > 0 && ctx->hevc_picture_out) {
						struct userdata_s *out_ud = ctx->hevc_picture_out->userData;
						if (out_ud) {
							printf("x264 avfm/audio pts coming out in %" PRIi64
								" (diff %" PRIi64 "), out pts %" PRIi64 "\n",
								out_ud->avfm.audio_pts,
								rf->avfm.audio_pts - out_ud->avfm.audio_pts,
								ctx->hevc_picture_in->pts);
						}
					}
#endif /* MEASURE_CODEC_LATENCY */

#endif /* SKIP_ENCODE */
					if (ret > 0) {
						x265_picture_analyze_stats(ctx, ctx->hevc_picture_out);
					}
				}
			}

			int64_t arrival_time = 0;
			if (rf) {
				arrival_time = rf->arrival_time;
				rf->release_data(rf);
				rf->release_frame(rf);
				remove_from_queue(&ctx->encoder->queue);
				rf = 0;
			}

			if (ret < 0) {
				fprintf(stderr, MESSAGE_PREFIX " picture encode failed, ret = %d\n", ret);
				break;
			}

			if (ret == 0) {
				//fprintf(stderr, MESSAGE_PREFIX " ret = %d\n", ret);
				leave = 1;
				continue;
			}

			/* Send the compressed nals down the workflow.
			 * Also, take care of any associated picture metadata.
			 */
			if (ret > 0) {
				_process_nals(ctx, arrival_time);
			} /* if nal_bytes > 0 */

			leave = 1;
		} /* While ! leave */

	} /* While (1) */

	if (ctx->hevc_encoder)
		x265_encoder_close(ctx->hevc_encoder);

out5:
	free(ctx->enc_params);

out4:
	if (ctx->hevc_picture_out)
		x265_picture_free(ctx->hevc_picture_out);
out3:
	if (ctx->hevc_picture_in)
		x265_picture_free(ctx->hevc_picture_in);
out2:
	if (ctx->hevc_params)
		x265_param_free(ctx->hevc_params);

	x265_cleanup();

	return NULL;
}

const obe_vid_enc_func_t x265_obe_encoder = { x265_start_encoder };
