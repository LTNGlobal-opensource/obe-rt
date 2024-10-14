/*****************************************************************************
 * x264.c : x264 encoding functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
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

#define NEW_X264_720p 0
#define NEW_X264_1080i 0

#include "common/common.h"
#include "common/vancprocessor.h"
#include "encoders/video/video.h"
#include "encoders/codec_metadata.h"
#include <libavutil/mathematics.h>
#include <libklscte35/scte35.h>
#include "common/timecode.h"

#include <histogram.h>

#if DEV_ABR
#include "obe/osd.h"
#endif

#define MESSAGE_PREFIX "[x264]: "
#define DEBUG_CODEC_TIMING 0

int64_t cpb_removal_time = 0;
int64_t g_x264_monitor_bps = 0;
int g_x264_nal_debug = 0;
extern uint64_t g_scte35_trigger_count;

#if DEV_DOWN_UP
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

struct SwsContext *g_sws_ctx = NULL;
void x264_picture_scaleByN(
	x264_picture_t **out, int outwidth, int outheight,
	x264_picture_t *in,   int  inwidth, int  inheight)
{
	if (g_sws_ctx == NULL) {
		/* create scaling context */
		g_sws_ctx = sws_getContext(inwidth, inheight, AV_PIX_FMT_YUV420P,
			outwidth, outheight, AV_PIX_FMT_YUV420P,
			SWS_BILINEAR, NULL, NULL, NULL);
	}

	x264_picture_t *p = calloc(1, sizeof(*p));
	int ret = x264_picture_alloc(p, in->img.i_csp, outwidth, outheight);
#if 0
printf(" in->i_stride[0] %d\n", in->img.i_stride[0]);
printf("out->i_stride[0] %d\n", p->img.i_stride[0]);
#endif

	const uint8_t *srcPlanes[4]  = { in->img.plane[0],    in->img.plane[1],    in->img.plane[2],    NULL };
	int            srcStrides[4] = { in->img.i_stride[0], in->img.i_stride[1], in->img.i_stride[2], 0    };
	uint8_t       *dstPlanes[4]  = { p->img.plane[0],     p->img.plane[1],     p->img.plane[2],     NULL };
	int            dstStrides[4] = { p->img.i_stride[0],  p->img.i_stride[1],  p->img.i_stride[2],  0    };

	/* convert to destination format */
	ret = sws_scale(g_sws_ctx,
		&srcPlanes[0],
		&srcStrides[0],
		0,
		inheight,
		&dstPlanes[0],
		&dstStrides[0]);
	if (ret != outheight) {
		fprintf(stderr, MESSAGE_PREFIX "Warning: scaleByN error\n");
	}

#if 0
	printf("Scale done %dx%d -> %dx%d, ret = %d\n",
		inwidth, inheight, outwidth, outheight, ret);
#endif

	p->opaque = in->opaque;
	*out = p;
}
#endif

#if DEV_ABR
int g_x264_encode_alternate = 0;
int g_x264_encode_alternate_new = 0;
int g_x264_bitrate_bps = 0;
int g_x264_bitrate_bps_new = 0;
int g_x264_keyint_min = 0;
int g_x264_keyint_min_new = 0;
int g_x264_keyint_max = 0;
int g_x264_keyint_max_new = 0;
int g_x264_lookahead = 0;
int g_x264_lookahead_new = 0;
#endif

#define SERIALIZE_CODED_FRAMES 0
#if SERIALIZE_CODED_FRAMES
static void serialize_coded_frame(obe_coded_frame_t *cf)
{
    static FILE *fh = NULL;
    if (!fh) {
        fh = fopen("/storage/dev/x264.cf", "wb");
        printf("Wwarning -- X264 coded frames will persist to disk\n");
    }
    if (fh)
        coded_frame_serializer_write(fh, cf);
}
#endif

#define DO_X264_VERBOSE_LOGGING 0
static void x264_logger( void *p_unused, int i_level, const char *psz_fmt, va_list arg )
{
#if DO_X264_VERBOSE_LOGGING
    time_t now = time(NULL);
    char ts[128];
    sprintf(ts, "%s", ctime(&now));
    ts[strlen(ts) - 1] = 0; /* Trim trailing CR */

printf("%s : ", ts);
    vprintf(psz_fmt, arg);
#else
    if( i_level <= X264_LOG_INFO )
        vsyslog( i_level == X264_LOG_INFO ? LOG_INFO : i_level == X264_LOG_WARNING ? LOG_WARNING : LOG_ERR, psz_fmt, arg );
#endif
}

/* Convert a obe_raw_frame_t into a x264_picture_t struct.
 * Incoming frame is colorspace YUV420P.
 */
static int convert_obe_to_x264_pic( x264_picture_t *pic, obe_raw_frame_t *raw_frame )
{
    obe_image_t *img = &raw_frame->img;
#if 0
PRINT_OBE_IMAGE(img, "      X264->img");
PRINT_OBE_IMAGE(&raw_frame->alloc_img, "alloc X264->img");
#endif
#if DEV_ABR
#else
    int idx = 0, count = 0;
#endif

    x264_picture_init( pic );

//pic->i_type = X264_TYPE_AUTO;
//pic->i_qpplus1 = X264_QP_AUTO;
    memcpy( pic->img.i_stride, img->stride, sizeof(img->stride) );
    memcpy( pic->img.plane, img->plane, sizeof(img->plane) );
    pic->img.i_plane = img->planes;
    pic->img.i_csp = img->csp == AV_PIX_FMT_YUV422P || img->csp == AV_PIX_FMT_YUV422P10 ? X264_CSP_I422 : X264_CSP_I420;
#if 0
    pic->img.i_csp = X264_CSP_I422;
#endif
#if 0
printf("pic->img.i_csp = %d [%s] bits = %d\n",
  pic->img.i_csp,
  pic->img.i_csp == X264_CSP_I422 ? "X264_CSP_I422" : "X264_CSP_I420",
  X264_BIT_DEPTH);
#endif

#if DEV_ABR
    /* TODO: Some kind of leak or double free. fix me for abr. */
#else
    if( X264_BIT_DEPTH == 10 )
        pic->img.i_csp |= X264_CSP_HIGH_DEPTH;

    for( int i = 0; i < raw_frame->num_user_data; i++ )
    {
        /* Only give correctly formatted data to the encoder */
        if( raw_frame->user_data[i].type == USER_DATA_AVC_REGISTERED_ITU_T35 ||
            raw_frame->user_data[i].type == USER_DATA_AVC_UNREGISTERED )
        {
            count++;
        }
    }

    if (g_sei_timestamping) {
        /* Create space for unregister data, containing before and after timestamps. */
        count += 1;
    }

    pic->extra_sei.num_payloads = count;

    if( pic->extra_sei.num_payloads )
    {
        pic->extra_sei.sei_free = free;
        pic->extra_sei.payloads = malloc( pic->extra_sei.num_payloads * sizeof(*pic->extra_sei.payloads) );

        if( !pic->extra_sei.payloads )
            return -1;

        for( int i = 0; i < raw_frame->num_user_data; i++ )
        {
            /* Only give correctly formatted data to the encoder */
            if( raw_frame->user_data[i].type == USER_DATA_AVC_REGISTERED_ITU_T35 ||
                raw_frame->user_data[i].type == USER_DATA_AVC_UNREGISTERED )
            {
                pic->extra_sei.payloads[idx].payload_type = raw_frame->user_data[i].type;
                pic->extra_sei.payloads[idx].payload_size = raw_frame->user_data[i].len;
                pic->extra_sei.payloads[idx].payload = raw_frame->user_data[i].data;
                idx++;
            }
            else
            {
                syslog( LOG_WARNING, "Invalid user data presented to encoder - type %i \n", raw_frame->user_data[i].type );
                free( raw_frame->user_data[i].data );
            }
            /* Set the pointer to NULL so only x264 can free the data if necessary */
            raw_frame->user_data[i].data = NULL;
        }
    }
    else if( raw_frame->num_user_data )
    {
        for( int i = 0; i < raw_frame->num_user_data; i++ )
        {
            syslog( LOG_WARNING, "Invalid user data presented to encoder - type %i \n", raw_frame->user_data[i].type );
            free( raw_frame->user_data[i].data );
        }
    }

    if (g_sei_timestamping) {
        x264_sei_payload_t *p;

        /* Start time - Always the last SEI */
        static uint32_t framecount = 0;
        p = &pic->extra_sei.payloads[count - 1];
        p->payload_type = USER_DATA_AVC_UNREGISTERED;
        p->payload_size = SEI_TIMESTAMP_PAYLOAD_LENGTH;
        p->payload = sei_timestamp_alloc();

        struct timeval tv;
        gettimeofday(&tv, NULL);

        sei_timestamp_field_set(p->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 1, framecount);
        sei_timestamp_field_set(p->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 2, avfm_get_hw_received_tv_sec(&raw_frame->avfm));
        sei_timestamp_field_set(p->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 3, avfm_get_hw_received_tv_usec(&raw_frame->avfm));
        sei_timestamp_field_set(p->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 4, tv.tv_sec);
        sei_timestamp_field_set(p->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 5, tv.tv_usec);
        sei_timestamp_field_set(p->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 6, 0); /* time exit from compressor seconds/useconds. */
        sei_timestamp_field_set(p->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 7, 0); /* time exit from compressor seconds/useconds. */
        sei_timestamp_field_set(p->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 8, 0); /* time transmit to udp seconds/useconds. */
        sei_timestamp_field_set(p->payload, SEI_TIMESTAMP_PAYLOAD_LENGTH, 9, 0); /* time transmit to udp seconds/useconds. */

        /* The remaining 8 bytes (time exit from compressor fields)
         * will be filled when the frame exists the compressor. */
        framecount++;
    }
#endif

    return 0;
}

#if 0
static int x264_image_compare(x264_image_t *a, x264_image_t *b)
{
	uint32_t plane_len[4] = { 0 };
	for (int i = a->i_plane - 1; i > 0; i--) {
		plane_len[i - 1] = a->plane[i] - a->plane[i - 1];
	}
	if (a->i_plane == 3) {
		plane_len[2] = plane_len[1];
	}

	uint32_t alloc_size = 0;
	for (int i = 0; i < a->i_plane; i++)
		alloc_size += plane_len[i];

	if (memcmp(a->plane[0], b->plane[0], alloc_size) != 0)
		return 0;

	return 1; /* Perfect copy. */

}
#endif

#if 0
static void x264_picture_free(x264_picture_t *pic)
{
	free(pic->img.plane[0]);
	free(pic);
}

static x264_picture_t *x264_picture_copy(x264_picture_t *pic)
{
	x264_picture_t *p = malloc(sizeof(*p));
	memcpy(p, pic, sizeof(*p));

	uint32_t plane_len[4] = { 0 };
	for (int i = pic->img.i_plane - 1; i > 0; i--) {
		plane_len[i - 1] = pic->img.plane[i] - pic->img.plane[i - 1];
	}
	if (pic->img.i_plane == 3) {
		plane_len[2] = plane_len[1];
	}

	uint32_t alloc_size = 0;
	for (int i = 0; i < pic->img.i_plane; i++)
		alloc_size += plane_len[i];

        for (int i = 0; i < pic->img.i_plane; i++) {

                if (i == 0 && pic->img.plane[i]) {
                        p->img.plane[i] = (uint8_t *)malloc(alloc_size);
                        memcpy(p->img.plane[i], pic->img.plane[i], alloc_size);
                }
                if (i > 0) {
                        p->img.plane[i] = p->img.plane[i - 1] + plane_len[i - 1];
                }

        }

	/* TODO: We don't clone SEI, yet. */
	p->extra_sei.num_payloads = 0;
	p->extra_sei.payloads = NULL;
	p->extra_sei.sei_free = NULL;

	return p;
}
#endif

static void _monitor_bps(obe_vid_enc_params_t *enc_params, int lengthBytes)
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
		if (dbps >= enc_params->avc_param.rc.i_vbv_max_bitrate) {
			fprintf(stderr, MESSAGE_PREFIX " codec output %d bps exceeds vbv_max_bitrate %d @ %s",
				codec_bps,
				enc_params->avc_param.rc.i_vbv_max_bitrate,
				ctime(&now));
		}
		if (g_x264_monitor_bps) {
			printf(MESSAGE_PREFIX " codec output %.02f (Mb/ps) @ %s", dbps, ctime(&now));
		}
	}
	codec_bps_current += (lengthBytes * 8);
}

void scte35_update_pts_offset(struct avmetadata_item_s *item, int64_t offset)
{
	struct scte35_splice_info_section_s si = { 0 };

	ssize_t len = scte35_splice_info_section_unpackFrom(&si, item->data, item->dataLengthBytes);
	if (len < 0) {
		printf("%s() Error unpacking SCTE35 struct, len = %lu\n", __func__, len);
		return;
	}
#if 1
	scte35_splice_info_section_print(&si);
#endif
	si.pts_adjustment += offset;
#if 1
	scte35_splice_info_section_print(&si);
#endif

	/* TODO: libklscte35 fails to repack if the buffer is less than 128 bytes. */
	if (item->dataLengthBytes < 128) {
		int ret = avmetadata_item_data_realloc(item, 128);
		if (ret < 0) {
			printf("%s() Error reallocing item buffer, ignoring.\n", __func__);
		}
	}

	int ret = scte35_splice_info_section_packTo(&si, item->data, item->dataLengthAlloc);
	if (ret < 0) {
		printf("%s() Error repacking SCTE35 struct, ret = %d\n", __func__, ret);
	}
}

/* Use some helper loic to parse a VANC SCTE104 message, convert to SCTE35 and ship it to the muxer
 * With the correct timestamps.
 */
int helper_vancprocessor_scte104(obe_vid_enc_params_t *enc_params, struct avmetadata_item_s *item, obe_coded_frame_t *cf)
{
	unsigned short  arrLengthWords = item->dataLengthBytes / 2;
	unsigned short *arr = (unsigned short *)item->data;

	/* DO THIS IN A GENERIC WAY SO EVERY CODEC BENEFITS.
	 *
	 * Allocate a vanc parser with a 104 callback.
	 * Feed the raw into helper
	 *   helper will:
	 *    * process each section
	 *    * add each item to the mux seperately.
	 * free any allocs
	 * destroy the vanc parser.
	 */

	struct vanc_processor_s *v;
	vancprocessor_alloc(&v, &enc_params->h->mux_queue, item->outputStreamId, cf);
	vancprocessor_write(v, arrLengthWords, arr, item->lineNr);
	vancprocessor_free(v);
	
	return 0;
}

static void *x264_start_encoder( void *ptr )
{
    obe_vid_enc_params_t *enc_params = ptr;
    obe_t *h = enc_params->h;
    obe_encoder_t *encoder = enc_params->encoder;
    x264_t *s = NULL;
    x264_picture_t pic, pic_out;
#if DEV_ABR
    x264_picture_t *pic_copy = NULL;
#endif
    x264_nal_t *nal;
    int i_nal, frame_size = 0;
    int64_t pts = 0, arrival_time = 0, frame_duration, buffer_duration;

    float buffer_fill;
    obe_raw_frame_t *raw_frame;
    obe_coded_frame_t *coded_frame;
    int64_t last_raw_frame_pts = 0;
    int64_t current_raw_frame_pts = 0;
    int upstream_signal_lost = 0;
    struct timecode_context_s tc;

#if DEV_ABR
    int encode_alternate_copy = 0;
#endif

#if DEV_ABR
    struct vc8x0_display_context osdctx;
    vc8x0_display_init(&osdctx);
#endif

    if (enc_params->avc_param.i_fps_num > 1000) {
        obe_timecode_clear(&tc, enc_params->avc_param.i_fps_num / 1000);
    } else {
        obe_timecode_clear(&tc, enc_params->avc_param.i_fps_num);
    }

    /* TODO: check for width, height changes */

    /* Lock the mutex until we verify and fetch new parameters */
    pthread_mutex_lock( &encoder->queue.mutex );

    enc_params->avc_param.pf_log = x264_logger;
#if DO_X264_VERBOSE_LOGGING
    enc_params->avc_param.i_log_level = 65535;
#endif

    printf(MESSAGE_PREFIX "Input resolution: %d x %d\n", enc_params->avc_param.i_width, enc_params->avc_param.i_height);
    printf(MESSAGE_PREFIX "Input framerate : %d / %d\n",
		enc_params->avc_param.i_fps_num,
		enc_params->avc_param.i_fps_den);

    if (h->obe_system == OBE_SYSTEM_TYPE_GENERIC) {
        if ((enc_params->avc_param.i_width == 1920) && (enc_params->avc_param.i_height == 1080)) {
            /* For 1080p60 we can't sustain realtime. Adjust some params. */
            if ((enc_params->avc_param.i_fps_num > 30000) || ((enc_params->avc_param.i_fps_num < 1000) && (enc_params->avc_param.i_fps_num > 30))) {
                /* For framerates above p30 assume worst case p60. */
                if (enc_params->avc_param.i_threads < 8) {
                    printf(MESSAGE_PREFIX "configuration threads defined as %d, need a minimum of 8. Adjusting to 8\n",
                        enc_params->avc_param.i_threads);
                    enc_params->avc_param.i_threads = 8;
                }

                if (enc_params->avc_param.i_keyint_max > 4) {
                    printf(MESSAGE_PREFIX "configuration keyint defined as %d, need a maximum of 4. Adjusting to 4\n",
                        enc_params->avc_param.i_keyint_max);
                    enc_params->avc_param.i_keyint_max = 4;
                }

                if (enc_params->avc_param.rc.i_lookahead != enc_params->avc_param.i_keyint_max) {
                    printf(MESSAGE_PREFIX "configuration lookahead defined as %d, need a maximum of %d. Adjusting to %d\n",
                        enc_params->avc_param.rc.i_lookahead,
                        enc_params->avc_param.i_keyint_max,
                        enc_params->avc_param.i_keyint_max);
                    enc_params->avc_param.rc.i_lookahead = enc_params->avc_param.i_keyint_max;
                }
            }
        }
    } else
    if (h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
        if ((enc_params->avc_param.i_width == 1920) && (enc_params->avc_param.i_height == 1080)) {
            /* For 1080p60 we can't sustain realtime. Adjust some params, but not as many as normal lat.. */
            if ((enc_params->avc_param.i_fps_num > 30000) || ((enc_params->avc_param.i_fps_num < 1000) && (enc_params->avc_param.i_fps_num > 30))) {
                /* For framerates above p30 assume worst case p60. */
                if (enc_params->avc_param.i_threads < 8) {
                    printf(MESSAGE_PREFIX "configuration threads defined as %d, need a minimum of 8. Adjusting to 8\n",
                        enc_params->avc_param.i_threads);
                    enc_params->avc_param.i_threads = 8;
                }
            }
	}
    }

#if 0
    enc_params->avc_param.i_csp = X264_CSP_I422;
#endif

#if NEW_X264_1080i
    printf(MESSAGE_PREFIX "X264 Testing - Hardcoded 1080i29.97 with iFrames and moderate Latency\n");
    x264_param_t param = { 0 };
    x264_param_default_preset(&param, "veryfast", "");

    param.rc.i_bitrate = 17000;
    param.rc.i_vbv_max_bitrate = 17000;
    param.rc.i_vbv_buffer_size = 1700;
    param.rc.i_lookahead = 2;
    param.rc.i_rc_method = 2; /* ABR */
    param.i_threads = 8;
    param.i_scenecut_threshold = 40;
    param.i_keyint_max = 30;
    param.i_keyint_min = 0;
    param.i_fps_num = 30000;
    param.i_fps_den = 1001;
    param.i_bframe = 3;
    param.i_timebase_num = 0;
    param.i_timebase_den = 0;
    param.i_width = 1920;
    param.i_height = 1080;
    param.b_interlaced = 1;
    param.b_tff = 1;
    param.b_open_gop = 1;
    param.b_vfr_input = 0;
    param.b_pic_struct = 1;
    param.b_aud = 1;
    param.i_nal_hrd = 3; /* Adds PTS/DTS output timing to callbacks */
    param.i_frame_reference = 4;

    param.vui.i_sar_height = 1;
    param.vui.i_sar_width = 1;
    param.vui.i_overscan = 2;
    param.vui.i_vidformat = 2;
    param.vui.b_fullrange = -1;
    param.vui.i_colorprim = 1;
    param.vui.i_transfer = 1;
    param.vui.i_colmatrix = 1;
    param.vui.i_chroma_loc = 0;
    //param.rc.i_vbv_buffer_size = param.rc.i_vbv_max_bitrate / (param.i_fps_num / param.i_fps_den);

printf("param.rc.i_vbv_buffer_size = %d\n", param.rc.i_vbv_buffer_size);

    x264_param_apply_profile(&param, "high");

    /* Shunt all of the params into the global space, just as if the
     * core had originally configured them.
     */
    memcpy(&enc_params->avc_param, &param, sizeof(param));
#endif

#if NEW_X264_720p
    printf(MESSAGE_PREFIX "X264 Testing - Hardcoded 720p59.94 with iFrames and moderate Latency\n");
    x264_param_t param = { 0 };
    x264_param_default_preset(&param, "veryfast", "");

    param.rc.i_bitrate = 17000;
    param.rc.i_vbv_max_bitrate = 17000;
    param.rc.i_vbv_buffer_size = 1700;
    param.rc.i_lookahead = 2;
    param.rc.i_rc_method = 2; /* ABR */
    param.i_threads = 4;
    param.i_scenecut_threshold = 40;
    param.i_keyint_max = 60;
    param.i_keyint_min = 0;
    param.i_fps_num = 60000;
    param.i_fps_den = 1001;
    param.i_bframe = 3;
    param.i_timebase_num = 0;
    param.i_timebase_den = 0;
    param.i_width = 1280;
    param.i_height = 720;
    //param.b_interlaced = 1;
    param.b_open_gop = 1;
    param.b_vfr_input = 0;
    param.b_pic_struct = 1;
    param.b_aud = 1;
    param.i_nal_hrd = 3; /* Adds PTS/DTS output timing to callbacks */
    param.i_frame_reference = 4;

    param.vui.i_sar_height = 1;
    param.vui.i_sar_width = 1;
    param.vui.i_overscan = 2;
    param.vui.i_vidformat = 2;
    param.vui.b_fullrange = -1;
    param.vui.i_colorprim = 1;
    param.vui.i_transfer = 1;
    param.vui.i_colmatrix = 1;
    param.vui.i_chroma_loc = 0;
    //param.rc.i_vbv_buffer_size = param.rc.i_vbv_max_bitrate / (param.i_fps_num / param.i_fps_den);

printf("param.rc.i_vbv_buffer_size = %d\n", param.rc.i_vbv_buffer_size);

    x264_param_apply_profile(&param, "high");

    /* Shunt all of the params into the global space, just as if the
     * core had originally configured them.
     */
    memcpy(&enc_params->avc_param, &param, sizeof(param));
#endif

    s = x264_encoder_open( &enc_params->avc_param );
    if( !s )
    {
        pthread_mutex_unlock( &encoder->queue.mutex );
        fprintf( stderr, "[x264]: encoder configuration failed\n" );
        goto end;
    }

    printf(MESSAGE_PREFIX "lookahead = %d\n", enc_params->avc_param.rc.i_lookahead);
    printf(MESSAGE_PREFIX "open_gop = %d\n", enc_params->avc_param.b_open_gop);
    printf(MESSAGE_PREFIX "profile = %d\n", enc_params->avc_param.i_profile);
    printf(MESSAGE_PREFIX "level_idc = %d\n", enc_params->avc_param.i_level_idc);
    x264_encoder_parameters( s, &enc_params->avc_param );

    encoder->encoder_params = malloc( sizeof(enc_params->avc_param) );
    if( !encoder->encoder_params )
    {
        pthread_mutex_unlock( &encoder->queue.mutex );
        syslog( LOG_ERR, "Malloc failed\n" );
        goto end;
    }
    memcpy( encoder->encoder_params, &enc_params->avc_param, sizeof(enc_params->avc_param) );

    encoder->is_ready = 1;
    /* XXX: This will need fixing for soft pulldown streams */
    frame_duration = av_rescale_q( 1, (AVRational){enc_params->avc_param.i_fps_den, enc_params->avc_param.i_fps_num}, (AVRational){1, OBE_CLOCK} );
#if X264_BUILD < 148
    buffer_duration = frame_duration * enc_params->avc_param.sc.i_buffer_size;
#endif

    /* Broadcast because input and muxer can be stuck waiting for encoder */
    pthread_cond_broadcast( &encoder->queue.in_cv );
    pthread_mutex_unlock( &encoder->queue.mutex );

    while( 1 )
    {
        pthread_mutex_lock( &encoder->queue.mutex );

        while( !encoder->queue.size && !encoder->cancel_thread )
            pthread_cond_wait( &encoder->queue.in_cv, &encoder->queue.mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->queue.mutex );
            break;
        }

        upstream_signal_lost = 0;

        /* Safety: Abort if the encoder backlog exceeds unreasonable levels (30-60 seconds).
         * Take a hard exit and make sure we signal to syslog.
         * 1800 frames of 1280x720p is approx 20% of 8GB RAM.
         * 1800 is 60fps 30 seconds, it would be 60 seconds for 30fps.
         * TODO: Implement in HEVC.
         */
        if (encoder->queue.size > 1800) {
            klsyslog_and_stdout(LOG_ERR,
                "LTN Encoder service abnormality: encoder queue backlog (%d), hard exit occuring.\n",
                encoder->queue.size);
            exit(1);
        }

        /* Reset the speedcontrol buffer if the source has dropped frames. Otherwise speedcontrol
         * stays in an underflow state and is locked to the fastest preset */
        pthread_mutex_lock( &h->drop_mutex );
        if( h->video_encoder_drop )
        {
            pthread_mutex_lock( &h->enc_smoothing_queue.mutex );
            h->enc_smoothing_buffer_complete = 0;
            pthread_mutex_unlock( &h->enc_smoothing_queue.mutex );
            syslog( LOG_INFO, "Speedcontrol reset\n" );
#if X264_BUILD < 148
            x264_speedcontrol_sync( s, enc_params->avc_param.sc.i_buffer_size, enc_params->avc_param.sc.f_buffer_init, 0 );
#endif
            h->video_encoder_drop = 0;
            upstream_signal_lost = 1;
        }
        pthread_mutex_unlock( &h->drop_mutex );

        raw_frame = encoder->queue.queue[0];
        pthread_mutex_unlock( &encoder->queue.mutex );

#if 0
	/* Useful debug code that caches the last raw_frame then compares it to
         * the current frame. If its not identical then throw a warning. This is helpful
         * when we're synthesizing frames due to LOS and we want to check that nobody
         * has tampered with the current frame, it should be a ferfect match.
         */
        static obe_raw_frame_t *cached = NULL;
        if (cached) {
            if (obe_image_compare(&raw_frame->alloc_img, &cached->alloc_img) != 1) {
                printf("X264 says image cached is bad\n");
            }

            cached->release_data(cached);
            cached->release_frame(cached);
            //obe_raw_frame_free(cached);
        }
        cached = obe_raw_frame_copy(raw_frame);
#endif

#if 0
        static int drop_count = 0;
        FILE *fh = fopen("/tmp/dropvideoframe.cmd", "rb");
        if (fh) {
            fclose(fh);
            unlink("/tmp/dropvideoframe.cmd");
            drop_count = 60;
        }
        if (drop_count-- > 0) {
            raw_frame->release_data( raw_frame );
            raw_frame->release_frame( raw_frame );
            remove_from_queue( &encoder->queue );
            const char *ts = obe_ascii_datetime();
            fprintf(stderr, "[X264] %s -- Faking a dropped raw video frame\n", ts);
            free((void *)ts);
            continue;
        }
#endif

#if DEV_ABR
	if (g_x264_lookahead_new) {
		g_x264_lookahead_new = 0;

		enc_params->avc_param.rc.i_lookahead = g_x264_lookahead;

		printf(MESSAGE_PREFIX "Adjusting codec with new lookahead %d\n",
			enc_params->avc_param.rc.i_lookahead);

		int ret = x264_encoder_reconfig(s, &enc_params->avc_param);
		if (ret < 0) {
			fprintf(stderr, MESSAGE_PREFIX " failed to reconfigure encoder.\n");
			exit(1);
		}
	}
	if (g_x264_keyint_min_new) {
		g_x264_keyint_min_new = 0;

		enc_params->avc_param.i_keyint_min = g_x264_keyint_min;

		printf(MESSAGE_PREFIX "Adjusting codec with new keyint_min gop %d\n",
			enc_params->avc_param.i_keyint_min);

		int ret = x264_encoder_reconfig(s, &enc_params->avc_param);
		if (ret < 0) {
			fprintf(stderr, MESSAGE_PREFIX " failed to reconfigure encoder.\n");
			exit(1);
		}
	}
	if (g_x264_keyint_max_new) {
		g_x264_keyint_max_new = 0;

		enc_params->avc_param.i_keyint_max = g_x264_keyint_max;

		printf(MESSAGE_PREFIX "Adjusting codec with new keyint_max gop %d\n",
			enc_params->avc_param.i_keyint_max);

		int ret = x264_encoder_reconfig(s, &enc_params->avc_param);
		if (ret < 0) {
			fprintf(stderr, MESSAGE_PREFIX " failed to reconfigure encoder.\n");
			exit(1);
		}
	}
	if (g_x264_bitrate_bps_new) {
		g_x264_bitrate_bps_new = 0;

		enc_params->avc_param.rc.i_bitrate = g_x264_bitrate_bps / 1000;
		enc_params->avc_param.rc.i_vbv_max_bitrate = enc_params->avc_param.rc.i_bitrate;

		printf(MESSAGE_PREFIX "Adjusting codec with new bitrate %dkbps, vbvmax %d\n",
			enc_params->avc_param.rc.i_bitrate,
			enc_params->avc_param.rc.i_vbv_max_bitrate);

		int ret = x264_encoder_reconfig(s, &enc_params->avc_param);
		if (ret < 0) {
			fprintf(stderr, MESSAGE_PREFIX " failed to reconfigure encoder.\n");
			exit(1);
		}
	}
#endif
        /* convert obe_frame_t into x264 friendly struct */
        if( convert_obe_to_x264_pic( &pic, raw_frame ) < 0 )
        {
            printf("Malloc failed\n");
            syslog( LOG_ERR, "Malloc failed\n" );
            break;
        }

        /* FIXME: if frames are dropped this might not be true */
        pic.i_pts = pts++;

        current_raw_frame_pts = raw_frame->pts;

        /* Allocate an opaque struct that we'll attach to a frame and pass to the
         * codec. The opaque will be returned to us later.
         * We'll hold timing and scte metadata in the opaque.
         */
        struct opaque_ctx_s *opaque = codec_metadata_alloc();
        if (!opaque) {
            printf("Malloc failed\n");
            syslog(LOG_ERR, "Malloc failed\n");
            break;
        }
        codec_metadata_set_avfm(opaque, &raw_frame->avfm);
        codec_metadata_set_avmetadata(opaque, &raw_frame->metadata);

#if 0
        if (raw_frame->dup)
            printf("next frame is a dup\n");
        avfm_dump(avfm);
#endif
        pic.opaque = opaque;
        pic.param = NULL;

        /* If the AFD has changed, then change the SAR. x264 will write the SAR at the next keyframe
         * TODO: allow user to force keyframes in order to be frame accurate */
        if( raw_frame->sar_width  != enc_params->avc_param.vui.i_sar_width ||
            raw_frame->sar_height != enc_params->avc_param.vui.i_sar_height )
        {
            enc_params->avc_param.vui.i_sar_width  = raw_frame->sar_width;
            enc_params->avc_param.vui.i_sar_height = raw_frame->sar_height;

            pic.param = &enc_params->avc_param;
        }

        /* Update speedcontrol based on the system state */
        if( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
        {
            pthread_mutex_lock( &h->enc_smoothing_queue.mutex );
            if( h->enc_smoothing_buffer_complete )
            {
                /* Wait until a frame is sent out. */
                while( !h->enc_smoothing_last_exit_time )
                    pthread_cond_wait( &h->enc_smoothing_queue.out_cv, &h->enc_smoothing_queue.mutex );

                /* time elapsed since last frame was removed */
                int64_t last_frame_delta = get_input_clock_in_mpeg_ticks( h ) - h->enc_smoothing_last_exit_time;

                if( h->enc_smoothing_queue.size )
                {
                    obe_coded_frame_t *first_frame, *last_frame;
                    first_frame = h->enc_smoothing_queue.queue[0];
                    last_frame = h->enc_smoothing_queue.queue[h->enc_smoothing_queue.size-1];
                    int64_t frame_durations = last_frame->real_dts - first_frame->real_dts + frame_duration;
                    buffer_fill = (float)(frame_durations - last_frame_delta)/buffer_duration;
                }
                else
                    buffer_fill = (float)(-1 * last_frame_delta)/buffer_duration;

#if X264_BUILD < 148
                x264_speedcontrol_sync( s, buffer_fill, enc_params->avc_param.sc.i_buffer_size, 1 );
#endif
            }

            pthread_mutex_unlock( &h->enc_smoothing_queue.mutex );
        }

#if 0
	/* Helpful code that caches x264_image_t objects before they go
         * into the compressor. In theory, if we feed a static image A
         * into the compressor, then the compressor should not touch or
         * tamper with image A. Used this code to track down pixel glitching
         * I was seeing (which turned out to be not related to x264 at all).
         * Keeping the code, it will be occasionally useful.
         */
        static x264_picture_t *cached = NULL;
        if (cached) {
            if (x264_image_compare(&pic.img, &cached->img) != 1) {
                printf("X264 says pic image cached is bad\n");
            }

            x264_picture_free(cached);
        }

        cached = x264_picture_copy(&pic);
#endif

#if DEV_ABR
        vc8x0_display_render_reset(&osdctx, pic.img.plane[0], pic.img.i_stride[0], pic.img.i_stride[0]);

        char line[80];
        int linepos = 0;
        sprintf(line, "bitrate: %d", enc_params->avc_param.rc.i_bitrate);
        vc8x0_display_render_string(&osdctx, line, strlen(line), 0, linepos++);

        sprintf(line, "vbv_max_bitrate: %d", enc_params->avc_param.rc.i_vbv_max_bitrate);
        vc8x0_display_render_string(&osdctx, line, strlen(line), 0, linepos++);

        sprintf(line, "vbv_buf_size: %d", enc_params->avc_param.rc.i_vbv_buffer_size);
        vc8x0_display_render_string(&osdctx, line, strlen(line), 0, linepos++);

        sprintf(line, "keyint_min: %d", enc_params->avc_param.i_keyint_min);
        vc8x0_display_render_string(&osdctx, line, strlen(line), 0, linepos++);

        sprintf(line, "keyint_max: %d", enc_params->avc_param.i_keyint_max);
        vc8x0_display_render_string(&osdctx, line, strlen(line), 0, linepos++);

        sprintf(line, "lookahead: %d", enc_params->avc_param.rc.i_lookahead);
        vc8x0_display_render_string(&osdctx, line, strlen(line), 0, linepos++);

        sprintf(line, "alterate encoding: %d", g_x264_encode_alternate);
        vc8x0_display_render_string(&osdctx, line, strlen(line), 0, linepos++);

#endif

#if DEV_DOWN_UP
	if (g_x264_encode_alternate_new == 1) {
		g_x264_encode_alternate_new = 0;
		encode_alternate_copy = 0;
		printf("Resetting encode_alternate_copy to 0\n");
	}
	if (g_x264_encode_alternate) {
		if (encode_alternate_copy == 0) {
			/* Free the cached frame, copy the current frame, encode the current frame. */
			if (pic_copy)
				x264_picture_free(pic_copy);
			pic_copy = x264_picture_copy(&pic);
        		frame_size = x264_encoder_encode(s, &nal, &i_nal, &pic, &pic_out );
			encode_alternate_copy = 1;
		} else
		if (encode_alternate_copy == 1) {
			/* Encode the previous frame, with some minor metadata updates. */
        		pic_copy->i_pts = pic.i_pts;
			pic_copy->opaque = pic.opaque;
        		frame_size = x264_encoder_encode(s, &nal, &i_nal, pic_copy, &pic_out);
			encode_alternate_copy = 2;
		} else
		if (encode_alternate_copy == 2) {
			/* Encode the previous frame, with some minor metadata updates. */
        		pic_copy->i_pts = pic.i_pts;
			pic_copy->opaque = pic.opaque;
        		frame_size = x264_encoder_encode(s, &nal, &i_nal, pic_copy, &pic_out);
			encode_alternate_copy = 3;
		} else {
			/* Encode the previous frame, with some minor metadata updates. */
        		pic_copy->i_pts = pic.i_pts;
			pic_copy->opaque = pic.opaque;
        		frame_size = x264_encoder_encode(s, &nal, &i_nal, pic_copy, &pic_out);
			x264_picture_free(pic_copy);
			pic_copy = NULL;
			encode_alternate_copy = 0;
		}
	} else {
                x264_picture_t *down = NULL, *up = NULL;
                int scaleby = 2; /* Half resolution. */

                x264_picture_scaleByN(&down, raw_frame->img.width / scaleby, raw_frame->img.height / scaleby,
                                      &pic, raw_frame->img.width, raw_frame->img.height);
                x264_picture_scaleByN(&up, raw_frame->img.width, raw_frame->img.height,
                                      down, raw_frame->img.width / scaleby, raw_frame->img.height / scaleby);

                frame_size = x264_encoder_encode(s, &nal, &i_nal, up, &pic_out);

                x264_picture_free(down);
                x264_picture_free(up);
	}
#else

        if (h->enable_timecode && raw_frame->timecode.present) {
            /* Timecode handling */
            obe_timecode_update(&tc,
                raw_frame->timecode.hours,
                raw_frame->timecode.mins,
                raw_frame->timecode.seconds,
                raw_frame->timecode.frames);

            if (obe_timecode_get_corrected_frame(&tc) >= 0) {
                pic.timecode[0].b_discontinuity = obe_timecode_get_discontinuity(&tc);
                pic.timecode[0].b_valid = 1;
                pic.timecode[0].i_hours = raw_frame->timecode.hours;
                pic.timecode[0].i_minutes = raw_frame->timecode.mins;
                pic.timecode[0].i_seconds = raw_frame->timecode.seconds;
                pic.timecode[0].i_frame = obe_timecode_get_corrected_frame(&tc);
                pic.timecode[0].b_drop = raw_frame->timecode.drop_frame;
                /* ISO14496-10 Table D-3: '6' dropping of unspecified
                * numbers of unspecified n_frames count values.
                */
                pic.timecode[0].i_counting_type = 6;
                pic.timecode[0].i_type = X264_TIMECODE_FULL;
            } else {
                /* The timecode is > 30fps and we need to wait for the
                 * incoming frame counter to reach 0 so we can syncronize
                 * our corrected frame count with a known good frame count.
                 * In practise this means we don't apply a timecode to the
                 * first N frames, until a second has wrapped.
                 */
                pic.timecode[0].b_valid = 0;                
            }
            /* End - Timecode handling */
        } else {
            pic.timecode[0].b_valid = 0;
        }
        if (pic.timecode[0].b_valid) {
            //printf("sending frame#%2d to encoder\n", pic.timecode[0].i_frame);
        }
        /* End - Timecode handling */

struct timeval begin, end, diff;
gettimeofday(&begin, NULL);
        frame_size = x264_encoder_encode( s, &nal, &i_nal, &pic, &pic_out );
gettimeofday(&end, NULL);
obe_timeval_subtract(&diff, &end, &begin);
//int us = ltn_histogram_timeval_to_us(&diff);

#if 0
printf("frame_size = %7d pic_type %d (%s) time %6d keyframe %d qp %d\n", frame_size,
  pic_out.i_type,
  pic_out.i_type == X264_TYPE_I  ? "I    " :
  pic_out.i_type == X264_TYPE_IDR ? "IDR  " :
  pic_out.i_type == X264_TYPE_P ? "P    " :
  pic_out.i_type == X264_TYPE_B ? "B    " :
  pic_out.i_type == X264_TYPE_BREF ? "BREF " :
  "UNKNO",
  us, pic_out.b_keyframe, pic_out.i_qpplus1);
#endif
#endif

        if (g_sei_timestamping) {
            /* Walk through each of the NALS and insert current time into any LTN sei timestamp frames we find. */
            for (int m = 0; m < i_nal; m++) {
                int offset = ltn_uuid_find(&nal[m].p_payload[0], nal[m].i_payload);
                if (offset >= 0) {
                    struct timeval tv;
                    gettimeofday(&tv, NULL);

                    /* Add the time exit from compressor seconds/useconds. */
                    sei_timestamp_field_set(&nal[m].p_payload[offset], nal[m].i_payload - offset, 6, tv.tv_sec);
                    sei_timestamp_field_set(&nal[m].p_payload[offset], nal[m].i_payload - offset, 7, tv.tv_usec);

#if 0
                    /* Obtain total codec time and histogram it. */
                    int64_t ms = sei_timestamp_query_codec_latency_ms(&nal[m].p_payload[offset], nal[m].i_payload - offset);
                    if (ms > 22 ) {
                        //printf("encoded frame: %" PRIi64 "\n", ms);
                    }
#endif
                }
            }
        }

        arrival_time = raw_frame->arrival_time;
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        remove_from_queue( &encoder->queue );

        if( frame_size < 0 )
        {
            printf("x264_encoder_encode failed\n");
            syslog( LOG_ERR, "x264_encoder_encode failed\n" );
            break;
        }

        if( frame_size )
        {
            if (g_x264_monitor_bps) {
                _monitor_bps(enc_params, frame_size);
            }

            coded_frame = new_coded_frame( encoder->output_stream_id, frame_size );
            if( !coded_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                break;
            }
            memcpy( coded_frame->data, nal[0].p_payload, frame_size );
            coded_frame->type = CF_VIDEO;
            coded_frame->len = frame_size;

#if 0
static FILE *fh = NULL;
if (fh == NULL)
  fh = fopen("/tmp/x264.nals", "wb");
if (fh)
  fwrite(coded_frame->data, 1, frame_size, fh);
#endif

            /* We've detected video frame loss that wasn't related to an upstream signal loss.
             * ensure we pass that data to the mux.
             */
            if (last_raw_frame_pts && !upstream_signal_lost) {
                //coded_frame->discontinuity_hz = (current_raw_frame_pts - last_raw_frame_pts) - frame_duration;
            }
            last_raw_frame_pts = current_raw_frame_pts;

#if X264_BUILD < 148
            coded_frame->cpb_initial_arrival_time = pic_out.hrd_timing.cpb_initial_arrival_time;
            coded_frame->cpb_final_arrival_time = pic_out.hrd_timing.cpb_final_arrival_time;
            coded_frame->real_dts = pic_out.hrd_timing.cpb_removal_time;
            coded_frame->real_pts = pic_out.hrd_timing.dpb_output_time;

            cpb_removal_time = pic_out.hrd_timing.cpb_removal_time;

            opaque = pic_out.opaque;

            memcpy(&coded_frame->avfm, &opaque->avfm, sizeof(struct avfm_s));
            coded_frame->pts = opaque->avfm.audio_pts;

#if DEBUG_CODEC_TIMING
            {
                static int64_t last_real_pts = 0;
                static int64_t last_real_dts = 0;

                printf(" codec: real_pts %12" PRIi64 " ( %12" PRIi64 " )  real_dts %12" PRIi64 " ( %12" PRIi64 " )  iat %12" PRIi64 "  fat %12" PRIi64 "  audio_pts %10" PRIi64" \n",
                    coded_frame->real_pts,
                    coded_frame->real_pts - last_real_pts,
                    coded_frame->real_dts,
                    coded_frame->real_dts - last_real_dts,
                    coded_frame->cpb_initial_arrival_time,
                    coded_frame->cpb_final_arrival_time,
                    coded_frame->pts);

                last_real_pts = coded_frame->real_pts;
                last_real_dts = coded_frame->real_dts;
            }
#endif
#else
            coded_frame->cpb_initial_arrival_time = pic_out.hrd_timing.cpb_initial_arrival_time * 27000000.0;
            coded_frame->cpb_final_arrival_time = pic_out.hrd_timing.cpb_final_arrival_time * 27000000.0;
            coded_frame->real_dts = (pic_out.hrd_timing.cpb_removal_time * 27000000.0);
            coded_frame->real_pts = (pic_out.hrd_timing.dpb_output_time  * 27000000.0);
#endif

            /* The audio and video clocks jump with different intervals when the cable
             * is disconnected, suggestedint a BM firmware bug.
             * We'll use the audio clock regardless, for both audio and video compressors.
             */
            int64_t new_dts = 0;
            if (h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
                new_dts = opaque->avfm.audio_pts + 0 -
                    llabs(coded_frame->real_dts - coded_frame->real_pts) + (2 * frame_duration);
            } else {
                new_dts = opaque->avfm.audio_pts + 24299700 -
                    llabs(coded_frame->real_dts - coded_frame->real_pts) + (2 * frame_duration);
            }

            /* We need to userstand, for this temporal frame, how much it varies from the dts. */
            int64_t pts_diff = coded_frame->real_dts - coded_frame->real_pts;

            /* Construct a new PTS based on the hardware DTS and the PTS offset difference. */
            int64_t new_pts  = new_dts - pts_diff;

            coded_frame->real_dts = new_dts;
            coded_frame->real_pts = new_pts;
#if 0
            coded_frame->cpb_initial_arrival_time = new_dts;
            coded_frame->cpb_final_arrival_time   = new_dts + abs(pic_out.hrd_timing.cpb_final_arrival_time - pic_out.hrd_timing.cpb_final_arrival_time);
#else

            static int64_t last_dts = 0;
            static int64_t dts_diff_accum = 0;
            int64_t dts_diff = 0;
            if (last_dts > 0) {
                dts_diff = coded_frame->real_dts - last_dts - (1 * frame_duration);
                dts_diff_accum += dts_diff;
            }
            last_dts = coded_frame->real_dts;

            int64_t ft = pic_out.hrd_timing.cpb_final_arrival_time;
            int64_t it = pic_out.hrd_timing.cpb_initial_arrival_time;
            int64_t fit = llabs(ft - it);

            coded_frame->cpb_initial_arrival_time += dts_diff_accum;

            coded_frame->cpb_final_arrival_time = coded_frame->cpb_initial_arrival_time + fit;
#endif

#if DEBUG_CODEC_TIMING
            {
                static int64_t last_real_pts = 0;
                static int64_t last_real_dts = 0;

                printf("adjust: real_pts %12" PRIi64 " ( %12" PRIi64 " )  real_dts %12" PRIi64 " ( %12" PRIi64 " )  iat %12" PRIi64 "  fat %12" PRIi64 "  audio_pts %10" PRIi64" \n",
                    coded_frame->real_pts,
                    coded_frame->real_pts - last_real_pts,
                    coded_frame->real_dts,
                    coded_frame->real_dts - last_real_dts,
                    coded_frame->cpb_initial_arrival_time,
                    coded_frame->cpb_final_arrival_time,
                    coded_frame->pts);

                last_real_pts = coded_frame->real_pts;
                last_real_dts = coded_frame->real_dts;
            }
#endif
            cpb_removal_time = coded_frame->real_pts; /* Only used for manually eyeballing the video output clock. */
            coded_frame->random_access = pic_out.b_keyframe;
            coded_frame->priority = IS_X264_TYPE_I( pic_out.i_type );

            if (g_x264_nal_debug & 0x04)
                coded_frame_print(coded_frame);

            if (opaque->metadata.count > 0) {
                /* We need to process any associated metadata before we destroy the frame. */

                for (int i = 0; i < opaque->metadata.count; i++) {
                    /* Process any scte35, trash any others we don't understand. */

                    struct avmetadata_item_s *e = opaque->metadata.array[i];
                    switch (e->item_type) {
                    case AVMETADATA_VANC_SCTE104:
                        helper_vancprocessor_scte104(enc_params, e, coded_frame);
                        g_scte35_trigger_count++;
                        break;
                    default:
                        printf("%s() warning, no handling of item type 0x%x\n", __func__, e->item_type);
                    }

                    avmetadata_item_free(e);
                    opaque->metadata.array[i] = NULL;
                }
                avmetadata_reset(&opaque->metadata);
            }

            if( h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY )
            {
                coded_frame->arrival_time = arrival_time;
#if SERIALIZE_CODED_FRAMES
                serialize_coded_frame(coded_frame);
#endif
                add_to_queue( &h->mux_queue, coded_frame );
                //printf("\n Encode Latency %"PRIi64" \n", obe_mdate() - coded_frame->arrival_time );
            }
            else {
#if SERIALIZE_CODED_FRAMES
                serialize_coded_frame(coded_frame);
#endif
                add_to_queue( &h->enc_smoothing_queue, coded_frame );
            }
            codec_metadata_free(pic_out.opaque);

        } /* if frame_size */
     } /* While(1) Main loop */

end:
    if( s )
        x264_encoder_close( s );
    free( enc_params );

    return NULL;
}

const obe_vid_enc_func_t x264_obe_encoder = { x264_start_encoder };
