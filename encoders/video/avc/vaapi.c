/*****************************************************************************
 * vaapi.c : AVC encoding functions
 *****************************************************************************
 * Copyright (C) 2018 LTN
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
#include <libavutil/mathematics.h>
#include <assert.h>
#include <sys/mman.h>
#include <va/va.h>
#include <va/va_enc_h264.h>
#include "va_display.h"
#include <libyuv.h>

#define LOCAL_DEBUG 1

#define MESSAGE_PREFIX "[avc-vaapi]:"

#define NAL_REF_IDC_NONE        0
#define NAL_REF_IDC_LOW         1
#define NAL_REF_IDC_MEDIUM      2
#define NAL_REF_IDC_HIGH        3

#define NAL_NON_IDR             1
#define NAL_IDR                 5
#define NAL_SPS                 7
#define NAL_PPS                 8
#define NAL_SEI                 6

#define SLICE_TYPE_P            0
#define SLICE_TYPE_B            1
#define SLICE_TYPE_I            2
#define IS_P_SLICE(type) (SLICE_TYPE_P == (type))
#define IS_B_SLICE(type) (SLICE_TYPE_B == (type))
#define IS_I_SLICE(type) (SLICE_TYPE_I == (type))


#define ENTROPY_MODE_CAVLC      0
#define ENTROPY_MODE_CABAC      1

#define PROFILE_IDC_BASELINE    66
#define PROFILE_IDC_MAIN        77
#define PROFILE_IDC_HIGH        100

#define BITSTREAM_ALLOCATE_STEPPING     4096

#define current_slot (ctx->current_frame_display % SURFACE_NUM)
struct storage_task_t {
    void *next;
    unsigned long long display_order;
    unsigned long long encode_order;
};

struct __bitstream {
    unsigned int *buffer;
    int bit_offset;
    int max_size_in_dword;
};
typedef struct __bitstream bitstream;

struct context_s
{
	obe_vid_enc_params_t *enc_params;
	obe_t *h;
	obe_encoder_t *encoder;

	uint64_t      raw_frame_count;

	/* VAAPI */
	struct SwsContext *encoderSwsContext;
	FILE *srcyuv_fp;
	FILE *recyuv_fp;
	FILE *coded_fp;
	VADisplay va_dpy;
	VAProfile h264_profile;
	int constraint_set_flag;
	int ip_period;
	VAConfigAttrib attrib[VAConfigAttribTypeMax];
	VAConfigAttrib config_attrib[VAConfigAttribTypeMax];
	int config_attrib_num;
	int rc_mode;
	int Log2MaxFrameNum;
	int Log2MaxPicOrderCntLsb;
	int h264_packedheader;
	int enc_packed_header_idx;
	int h264_maxref;
	int h264_entropy_mode;
	VAConfigID config_id;
	int frame_width_mbaligned;
	int frame_height_mbaligned;
	int frame_width;
	int frame_height;
	int frame_rate;
	int frame_bitrate;
	int misc_priv_type;
	int misc_priv_value;
#define SURFACE_NUM 16
#define SRC_SURFACE_IN_ENCODING 0
#define SRC_SURFACE_IN_STORAGE  1
	VASurfaceID src_surface[SURFACE_NUM];
	VASurfaceID ref_surface[SURFACE_NUM];
	VABufferID  coded_buf[SURFACE_NUM];
	int srcsurface_status[SURFACE_NUM];
	VAContextID context_id;
	uint64_t current_frame_display;
	int current_frame_type;
	int num_ref_frames;
	unsigned int MaxFrameNum;
	double frame_size;
	unsigned int MaxPicOrderCntLsb;

	VAEncSequenceParameterBufferH264 seq_param;
	VAEncPictureParameterBufferH264 pic_param;
	VAEncSliceParameterBufferH264 slice_param;
	int encode_syncmode;
	//pthread_t encode_thread;
	pthread_mutex_t encode_mutex;
	pthread_cond_t encode_cond;

	struct storage_task_t *storage_task_header;
	struct storage_task_t *storage_task_tail;

	int frame_coded;
	int srcyuv_fourcc;

	uint64_t current_frame_encoding;
	uint64_t srcyuv_frames;

	int initial_qp;
	int minimal_qp;
	int intra_period;
	int intra_idr_period;
	unsigned int current_frame_num;
	unsigned int numShortTerm;
	uint64_t current_IDR_display;
	VAPictureH264 CurrentCurrPic;
	VAPictureH264 ReferenceFrames[16], RefPicList0_P[32], RefPicList0_B[32], RefPicList1_B[32];

	/* tmp */
	time_t tmp_last;
	int tmp_count;
	int tmp_bytes;

};
static int save_codeddata(struct context_s *ctx, uint64_t display_order, uint64_t encode_order, obe_raw_frame_t *rf, int frame_type);
static int load_surface(struct context_s *ctx, VASurfaceID surface_id, unsigned long long display_order);
static int upload_source_YUV_once_for_all(struct context_s *ctx);
static size_t avc_vaapi_deliver_nals(struct context_s *ctx, uint8_t *buf, int lengthBytes, obe_raw_frame_t *rf, int frame_type);

#define SERIALIZE_CODED_FRAMES 0
#if SERIALIZE_CODED_FRAMES
static void serialize_coded_frame(obe_coded_frame_t *cf)
{
    static FILE *fh = NULL;
    if (!fh) {
        fh = fopen("/storage/dev/avc-vaapi.cf", "wb");
        printf("Wwarning -- avc vaapi coded frames will persist to disk\n");
    }
    if (fh)
        coded_frame_serializer_write(fh, cf);
}
#endif

struct userdata_s
{
	struct avfm_s avfm;
};

static struct userdata_s *userdata_calloc()
{
	struct userdata_s *ud = calloc(1, sizeof(*ud));
	return ud;
}

static int userdata_set(struct userdata_s *ud, struct avfm_s *s)
{
	memcpy(&ud->avfm, s, sizeof(struct avfm_s));
	return 0;
}

/*     VAAPI SPECIFIC */

static  int rc_default_modes[] = {
    VA_RC_VBR,
    VA_RC_CQP,
    VA_RC_VBR_CONSTRAINED,
    VA_RC_CBR,
    VA_RC_VCM,
    VA_RC_NONE,
};

#define CHECK_VASTATUS(va_status,func)                                  \
    if (va_status != VA_STATUS_SUCCESS) {                               \
        fprintf(stderr,"%s:%s (%d) failed,exit\n", __func__, func, __LINE__); \
        exit(1);                                                        \
    }


static unsigned int 
va_swap32(unsigned int val)
{
    unsigned char *pval = (unsigned char *)&val;

    return ((pval[0] << 24)     |
            (pval[1] << 16)     |
            (pval[2] << 8)      |
            (pval[3] << 0));
}

static void
bitstream_start(bitstream *bs)
{
    bs->max_size_in_dword = BITSTREAM_ALLOCATE_STEPPING;
    bs->buffer = calloc(bs->max_size_in_dword * sizeof(int), 1);
    assert(bs->buffer);
    bs->bit_offset = 0;
}

static void
bitstream_end(bitstream *bs)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (bit_offset) {
        bs->buffer[pos] = va_swap32((bs->buffer[pos] << bit_left));
    }
}
 
static void
bitstream_put_ui(bitstream *bs, unsigned int val, int size_in_bits)
{
    int pos = (bs->bit_offset >> 5);
    int bit_offset = (bs->bit_offset & 0x1f);
    int bit_left = 32 - bit_offset;

    if (!size_in_bits)
        return;

    bs->bit_offset += size_in_bits;

    if (bit_left > size_in_bits) {
        bs->buffer[pos] = (bs->buffer[pos] << size_in_bits | val);
    } else {
        size_in_bits -= bit_left;
        bs->buffer[pos] = (bs->buffer[pos] << bit_left) | (val >> size_in_bits);
        bs->buffer[pos] = va_swap32(bs->buffer[pos]);

        if (pos + 1 == bs->max_size_in_dword) {
            bs->max_size_in_dword += BITSTREAM_ALLOCATE_STEPPING;
            bs->buffer = realloc(bs->buffer, bs->max_size_in_dword * sizeof(unsigned int));
            assert(bs->buffer);
        }

        bs->buffer[pos + 1] = val;
    }
}

static void
bitstream_put_ue(bitstream *bs, unsigned int val)
{
    int size_in_bits = 0;
    int tmp_val = ++val;

    while (tmp_val) {
        tmp_val >>= 1;
        size_in_bits++;
    }

    bitstream_put_ui(bs, 0, size_in_bits - 1); // leading zero
    bitstream_put_ui(bs, val, size_in_bits);
}

static void
bitstream_put_se(bitstream *bs, int val)
{
    unsigned int new_val;

    if (val <= 0)
        new_val = -2 * val;
    else
        new_val = 2 * val - 1;

    bitstream_put_ue(bs, new_val);
}

static void
bitstream_byte_aligning(bitstream *bs, int bit)
{
    int bit_offset = (bs->bit_offset & 0x7);
    int bit_left = 8 - bit_offset;
    int new_val;

    if (!bit_offset)
        return;

    assert(bit == 0 || bit == 1);

    if (bit)
        new_val = (1 << bit_left) - 1;
    else
        new_val = 0;

    bitstream_put_ui(bs, new_val, bit_left);
}

static void 
rbsp_trailing_bits(bitstream *bs)
{
    bitstream_put_ui(bs, 1, 1);
    bitstream_byte_aligning(bs, 0);
}

static void nal_start_code_prefix(bitstream *bs)
{
    bitstream_put_ui(bs, 0x00000001, 32);
}

static void nal_header(bitstream *bs, int nal_ref_idc, int nal_unit_type)
{
    bitstream_put_ui(bs, 0, 1);                /* forbidden_zero_bit: 0 */
    bitstream_put_ui(bs, nal_ref_idc, 2);
    bitstream_put_ui(bs, nal_unit_type, 5);
}

static void sps_rbsp(struct context_s *ctx, bitstream *bs)
{
    int profile_idc = PROFILE_IDC_BASELINE;

    if (ctx->h264_profile  == VAProfileH264High)
        profile_idc = PROFILE_IDC_HIGH;
    else if (ctx->h264_profile  == VAProfileH264Main)
        profile_idc = PROFILE_IDC_MAIN;

    bitstream_put_ui(bs, profile_idc, 8);               /* profile_idc */
    bitstream_put_ui(bs, !!(ctx->constraint_set_flag & 1), 1);                         /* constraint_set0_flag */
    bitstream_put_ui(bs, !!(ctx->constraint_set_flag & 2), 1);                         /* constraint_set1_flag */
    bitstream_put_ui(bs, !!(ctx->constraint_set_flag & 4), 1);                         /* constraint_set2_flag */
    bitstream_put_ui(bs, !!(ctx->constraint_set_flag & 8), 1);                         /* constraint_set3_flag */
    bitstream_put_ui(bs, 0, 4);                         /* reserved_zero_4bits */
    bitstream_put_ui(bs, ctx->seq_param.level_idc, 8);      /* level_idc */
    bitstream_put_ue(bs, ctx->seq_param.seq_parameter_set_id);      /* seq_parameter_set_id */

    if ( profile_idc == PROFILE_IDC_HIGH) {
        bitstream_put_ue(bs, 1);        /* chroma_format_idc = 1, 4:2:0 */ 
        bitstream_put_ue(bs, 0);        /* bit_depth_luma_minus8 */
        bitstream_put_ue(bs, 0);        /* bit_depth_chroma_minus8 */
        bitstream_put_ui(bs, 0, 1);     /* qpprime_y_zero_transform_bypass_flag */
        bitstream_put_ui(bs, 0, 1);     /* seq_scaling_matrix_present_flag */
    }

    bitstream_put_ue(bs, ctx->seq_param.seq_fields.bits.log2_max_frame_num_minus4); /* log2_max_frame_num_minus4 */
    bitstream_put_ue(bs, ctx->seq_param.seq_fields.bits.pic_order_cnt_type);        /* pic_order_cnt_type */

    if (ctx->seq_param.seq_fields.bits.pic_order_cnt_type == 0)
        bitstream_put_ue(bs, ctx->seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);     /* log2_max_pic_order_cnt_lsb_minus4 */
    else {
        assert(0);
    }

    bitstream_put_ue(bs, ctx->seq_param.max_num_ref_frames);        /* num_ref_frames */
    bitstream_put_ui(bs, 0, 1);                                 /* gaps_in_frame_num_value_allowed_flag */

    bitstream_put_ue(bs, ctx->seq_param.picture_width_in_mbs - 1);  /* pic_width_in_mbs_minus1 */
    bitstream_put_ue(bs, ctx->seq_param.picture_height_in_mbs - 1); /* pic_height_in_map_units_minus1 */
    bitstream_put_ui(bs, ctx->seq_param.seq_fields.bits.frame_mbs_only_flag, 1);    /* frame_mbs_only_flag */

    if (!ctx->seq_param.seq_fields.bits.frame_mbs_only_flag) {
        assert(0);
    }

    bitstream_put_ui(bs, ctx->seq_param.seq_fields.bits.direct_8x8_inference_flag, 1);      /* direct_8x8_inference_flag */
    bitstream_put_ui(bs, ctx->seq_param.frame_cropping_flag, 1);            /* frame_cropping_flag */

    if (ctx->seq_param.frame_cropping_flag) {
        bitstream_put_ue(bs, ctx->seq_param.frame_crop_left_offset);        /* frame_crop_left_offset */
        bitstream_put_ue(bs, ctx->seq_param.frame_crop_right_offset);       /* frame_crop_right_offset */
        bitstream_put_ue(bs, ctx->seq_param.frame_crop_top_offset);         /* frame_crop_top_offset */
        bitstream_put_ue(bs, ctx->seq_param.frame_crop_bottom_offset);      /* frame_crop_bottom_offset */
    }
    
    //if ( frame_bit_rate < 0 ) { //TODO EW: the vui header isn't correct
    if ( 1 ) {
        bitstream_put_ui(bs, 0, 1); /* vui_parameters_present_flag */
    } else {
        bitstream_put_ui(bs, 1, 1); /* vui_parameters_present_flag */
        bitstream_put_ui(bs, 0, 1); /* aspect_ratio_info_present_flag */
        bitstream_put_ui(bs, 0, 1); /* overscan_info_present_flag */
        bitstream_put_ui(bs, 0, 1); /* video_signal_type_present_flag */
        bitstream_put_ui(bs, 0, 1); /* chroma_loc_info_present_flag */
        bitstream_put_ui(bs, 1, 1); /* timing_info_present_flag */
        {
            bitstream_put_ui(bs, 15, 32);
            bitstream_put_ui(bs, 900, 32);
            bitstream_put_ui(bs, 1, 1);
        }
        bitstream_put_ui(bs, 1, 1); /* nal_hrd_parameters_present_flag */
        {
            // hrd_parameters 
            bitstream_put_ue(bs, 0);    /* cpb_cnt_minus1 */
            bitstream_put_ui(bs, 4, 4); /* bit_rate_scale */
            bitstream_put_ui(bs, 6, 4); /* cpb_size_scale */
           
            bitstream_put_ue(bs, ctx->frame_bitrate - 1); /* bit_rate_value_minus1[0] */
            bitstream_put_ue(bs, ctx->frame_bitrate*8 - 1); /* cpb_size_value_minus1[0] */
            bitstream_put_ui(bs, 1, 1);  /* cbr_flag[0] */

            bitstream_put_ui(bs, 23, 5);   /* initial_cpb_removal_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* cpb_removal_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* dpb_output_delay_length_minus1 */
            bitstream_put_ui(bs, 23, 5);   /* time_offset_length  */
        }
        bitstream_put_ui(bs, 0, 1);   /* vcl_hrd_parameters_present_flag */
        bitstream_put_ui(bs, 0, 1);   /* low_delay_hrd_flag */ 

        bitstream_put_ui(bs, 0, 1); /* pic_struct_present_flag */
        bitstream_put_ui(bs, 0, 1); /* bitstream_restriction_flag */
    }

    rbsp_trailing_bits(bs);     /* rbsp_trailing_bits */
}


static void pps_rbsp(struct context_s *ctx, bitstream *bs)
{
    bitstream_put_ue(bs, ctx->pic_param.pic_parameter_set_id);      /* pic_parameter_set_id */
    bitstream_put_ue(bs, ctx->pic_param.seq_parameter_set_id);      /* seq_parameter_set_id */

    bitstream_put_ui(bs, ctx->pic_param.pic_fields.bits.entropy_coding_mode_flag, 1);  /* entropy_coding_mode_flag */

    bitstream_put_ui(bs, 0, 1);                         /* pic_order_present_flag: 0 */

    bitstream_put_ue(bs, 0);                            /* num_slice_groups_minus1 */

    bitstream_put_ue(bs, ctx->pic_param.num_ref_idx_l0_active_minus1);      /* num_ref_idx_l0_active_minus1 */
    bitstream_put_ue(bs, ctx->pic_param.num_ref_idx_l1_active_minus1);      /* num_ref_idx_l1_active_minus1 1 */

    bitstream_put_ui(bs, ctx->pic_param.pic_fields.bits.weighted_pred_flag, 1);     /* weighted_pred_flag: 0 */
    bitstream_put_ui(bs, ctx->pic_param.pic_fields.bits.weighted_bipred_idc, 2);	/* weighted_bipred_idc: 0 */

    bitstream_put_se(bs, ctx->pic_param.pic_init_qp - 26);  /* pic_init_qp_minus26 */
    bitstream_put_se(bs, 0);                            /* pic_init_qs_minus26 */
    bitstream_put_se(bs, 0);                            /* chroma_qp_index_offset */

    bitstream_put_ui(bs, ctx->pic_param.pic_fields.bits.deblocking_filter_control_present_flag, 1); /* deblocking_filter_control_present_flag */
    bitstream_put_ui(bs, 0, 1);                         /* constrained_intra_pred_flag */
    bitstream_put_ui(bs, 0, 1);                         /* redundant_pic_cnt_present_flag */
    
    /* more_rbsp_data */
    bitstream_put_ui(bs, ctx->pic_param.pic_fields.bits.transform_8x8_mode_flag, 1);    /*transform_8x8_mode_flag */
    bitstream_put_ui(bs, 0, 1);                         /* pic_scaling_matrix_present_flag */
    bitstream_put_se(bs, ctx->pic_param.second_chroma_qp_index_offset );    /*second_chroma_qp_index_offset */

    rbsp_trailing_bits(bs);
}

static void slice_header(struct context_s *ctx, bitstream *bs)
{
    int first_mb_in_slice = ctx->slice_param.macroblock_address;

    bitstream_put_ue(bs, first_mb_in_slice);        /* first_mb_in_slice: 0 */
    bitstream_put_ue(bs, ctx->slice_param.slice_type);   /* slice_type */
    bitstream_put_ue(bs, ctx->slice_param.pic_parameter_set_id);        /* pic_parameter_set_id: 0 */
    bitstream_put_ui(bs, ctx->pic_param.frame_num, ctx->seq_param.seq_fields.bits.log2_max_frame_num_minus4 + 4); /* frame_num */

    /* frame_mbs_only_flag == 1 */
    if (!ctx->seq_param.seq_fields.bits.frame_mbs_only_flag) {
        /* FIXME: */
        assert(0);
    }

    if (ctx->pic_param.pic_fields.bits.idr_pic_flag)
        bitstream_put_ue(bs, ctx->slice_param.idr_pic_id);		/* idr_pic_id: 0 */

    if (ctx->seq_param.seq_fields.bits.pic_order_cnt_type == 0) {
        bitstream_put_ui(bs, ctx->pic_param.CurrPic.TopFieldOrderCnt, ctx->seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4);
        /* pic_order_present_flag == 0 */
    } else {
        /* FIXME: */
        assert(0);
    }

    /* redundant_pic_cnt_present_flag == 0 */
    /* slice type */
    if (IS_P_SLICE(ctx->slice_param.slice_type)) {
        bitstream_put_ui(bs, ctx->slice_param.num_ref_idx_active_override_flag, 1);            /* num_ref_idx_active_override_flag: */

        if (ctx->slice_param.num_ref_idx_active_override_flag)
            bitstream_put_ue(bs, ctx->slice_param.num_ref_idx_l0_active_minus1);

        /* ref_pic_list_reordering */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l0: 0 */
    } else if (IS_B_SLICE(ctx->slice_param.slice_type)) {
        bitstream_put_ui(bs, ctx->slice_param.direct_spatial_mv_pred_flag, 1);            /* direct_spatial_mv_pred: 1 */

        bitstream_put_ui(bs, ctx->slice_param.num_ref_idx_active_override_flag, 1);       /* num_ref_idx_active_override_flag: */

        if (ctx->slice_param.num_ref_idx_active_override_flag) {
            bitstream_put_ue(bs, ctx->slice_param.num_ref_idx_l0_active_minus1);
            bitstream_put_ue(bs, ctx->slice_param.num_ref_idx_l1_active_minus1);
        }

        /* ref_pic_list_reordering */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l0: 0 */
        bitstream_put_ui(bs, 0, 1);            /* ref_pic_list_reordering_flag_l1: 0 */
    }

    if ((ctx->pic_param.pic_fields.bits.weighted_pred_flag &&
         IS_P_SLICE(ctx->slice_param.slice_type)) ||
        ((ctx->pic_param.pic_fields.bits.weighted_bipred_idc == 1) &&
         IS_B_SLICE(ctx->slice_param.slice_type))) {
        /* FIXME: fill weight/offset table */
        assert(0);
    }

    /* dec_ref_pic_marking */
    if (ctx->pic_param.pic_fields.bits.reference_pic_flag) {     /* nal_ref_idc != 0 */
        unsigned char no_output_of_prior_pics_flag = 0;
        unsigned char long_term_reference_flag = 0;
        unsigned char adaptive_ref_pic_marking_mode_flag = 0;

        if (ctx->pic_param.pic_fields.bits.idr_pic_flag) {
            bitstream_put_ui(bs, no_output_of_prior_pics_flag, 1);            /* no_output_of_prior_pics_flag: 0 */
            bitstream_put_ui(bs, long_term_reference_flag, 1);            /* long_term_reference_flag: 0 */
        } else {
            bitstream_put_ui(bs, adaptive_ref_pic_marking_mode_flag, 1);            /* adaptive_ref_pic_marking_mode_flag: 0 */
        }
    }

    if (ctx->pic_param.pic_fields.bits.entropy_coding_mode_flag &&
        !IS_I_SLICE(ctx->slice_param.slice_type))
        bitstream_put_ue(bs, ctx->slice_param.cabac_init_idc);               /* cabac_init_idc: 0 */

    bitstream_put_se(bs, ctx->slice_param.slice_qp_delta);                   /* slice_qp_delta: 0 */

    /* ignore for SP/SI */

    if (ctx->pic_param.pic_fields.bits.deblocking_filter_control_present_flag) {
        bitstream_put_ue(bs, ctx->slice_param.disable_deblocking_filter_idc);           /* disable_deblocking_filter_idc: 0 */

        if (ctx->slice_param.disable_deblocking_filter_idc != 1) {
            bitstream_put_se(bs, ctx->slice_param.slice_alpha_c0_offset_div2);          /* slice_alpha_c0_offset_div2: 2 */
            bitstream_put_se(bs, ctx->slice_param.slice_beta_offset_div2);              /* slice_beta_offset_div2: 2 */
        }
    }

    if (ctx->pic_param.pic_fields.bits.entropy_coding_mode_flag) {
        bitstream_byte_aligning(bs, 1);
    }
}

static int
build_packed_pic_buffer(struct context_s *ctx, unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_PPS);
    pps_rbsp(ctx, &bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

static int
build_packed_seq_buffer(struct context_s *ctx, unsigned char **header_buffer)
{
    bitstream bs;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);
    nal_header(&bs, NAL_REF_IDC_HIGH, NAL_SPS);
    sps_rbsp(ctx, &bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

#if 0
static int 
build_packed_sei_buffer_timing(struct context_s *ctx, unsigned int init_cpb_removal_length,
				unsigned int init_cpb_removal_delay,
				unsigned int init_cpb_removal_delay_offset,
				unsigned int cpb_removal_length,
				unsigned int cpb_removal_delay,
				unsigned int dpb_output_length,
				unsigned int dpb_output_delay,
				unsigned char **sei_buffer)
{
    unsigned char *byte_buf;
    int bp_byte_size, i, pic_byte_size;

    bitstream nal_bs;
    bitstream sei_bp_bs, sei_pic_bs;

    bitstream_start(&sei_bp_bs);
    bitstream_put_ue(&sei_bp_bs, 0);       /*seq_parameter_set_id*/
    bitstream_put_ui(&sei_bp_bs, init_cpb_removal_delay, cpb_removal_length); 
    bitstream_put_ui(&sei_bp_bs, init_cpb_removal_delay_offset, cpb_removal_length); 
    if ( sei_bp_bs.bit_offset & 0x7) {
        bitstream_put_ui(&sei_bp_bs, 1, 1);
    }
    bitstream_end(&sei_bp_bs);
    bp_byte_size = (sei_bp_bs.bit_offset + 7) / 8;
    
    bitstream_start(&sei_pic_bs);
    bitstream_put_ui(&sei_pic_bs, cpb_removal_delay, cpb_removal_length); 
    bitstream_put_ui(&sei_pic_bs, dpb_output_delay, dpb_output_length); 
    if ( sei_pic_bs.bit_offset & 0x7) {
        bitstream_put_ui(&sei_pic_bs, 1, 1);
    }
    bitstream_end(&sei_pic_bs);
    pic_byte_size = (sei_pic_bs.bit_offset + 7) / 8;
    
    bitstream_start(&nal_bs);
    nal_start_code_prefix(&nal_bs);
    nal_header(&nal_bs, NAL_REF_IDC_NONE, NAL_SEI);

	/* Write the SEI buffer period data */    
    bitstream_put_ui(&nal_bs, 0, 8);
    bitstream_put_ui(&nal_bs, bp_byte_size, 8);
    
    byte_buf = (unsigned char *)sei_bp_bs.buffer;
    for(i = 0; i < bp_byte_size; i++) {
        bitstream_put_ui(&nal_bs, byte_buf[i], 8);
    }
    free(byte_buf);
	/* write the SEI timing data */
    bitstream_put_ui(&nal_bs, 0x01, 8);
    bitstream_put_ui(&nal_bs, pic_byte_size, 8);
    
    byte_buf = (unsigned char *)sei_pic_bs.buffer;
    for(i = 0; i < pic_byte_size; i++) {
        bitstream_put_ui(&nal_bs, byte_buf[i], 8);
    }
    free(byte_buf);

    rbsp_trailing_bits(&nal_bs);
    bitstream_end(&nal_bs);

    *sei_buffer = (unsigned char *)nal_bs.buffer; 
   
    return nal_bs.bit_offset;
}
#endif

static int build_packed_slice_buffer(struct context_s *ctx, unsigned char **header_buffer)
{
    bitstream bs;
    int is_idr = !!ctx->pic_param.pic_fields.bits.idr_pic_flag;
    int is_ref = !!ctx->pic_param.pic_fields.bits.reference_pic_flag;

    bitstream_start(&bs);
    nal_start_code_prefix(&bs);

    if (IS_I_SLICE(ctx->slice_param.slice_type)) {
        nal_header(&bs, NAL_REF_IDC_HIGH, is_idr ? NAL_IDR : NAL_NON_IDR);
    } else if (IS_P_SLICE(ctx->slice_param.slice_type)) {
        nal_header(&bs, NAL_REF_IDC_MEDIUM, NAL_NON_IDR);
    } else {
        assert(IS_B_SLICE(ctx->slice_param.slice_type));
        nal_header(&bs, is_ref ? NAL_REF_IDC_LOW : NAL_REF_IDC_NONE, NAL_NON_IDR);
    }

    slice_header(ctx, &bs);
    bitstream_end(&bs);

    *header_buffer = (unsigned char *)bs.buffer;
    return bs.bit_offset;
}

static char *rc_to_string(int rcmode)
{
    switch (rcmode) {
    case VA_RC_NONE:
        return "NONE";
    case VA_RC_CBR:
        return "CBR";
    case VA_RC_VBR:
        return "VBR";
    case VA_RC_VCM:
        return "VCM";
    case VA_RC_CQP:
        return "CQP";
    case VA_RC_VBR_CONSTRAINED:
        return "VBR_CONSTRAINED";
    default:
        return "Unknown";
    }
}

static int vaapi_setup_encode(struct context_s *ctx)
{
    VAStatus va_status;
    VASurfaceID *tmp_surfaceid;
    int codedbuf_size, i;
    
    va_status = vaCreateConfig(ctx->va_dpy, ctx->h264_profile, VAEntrypointEncSlice,
            &ctx->config_attrib[0], ctx->config_attrib_num, &ctx->config_id);
    CHECK_VASTATUS(va_status, "vaCreateConfig");

    /* create source surfaces */
    va_status = vaCreateSurfaces(ctx->va_dpy,
                                 VA_RT_FORMAT_YUV420, ctx->frame_width_mbaligned, ctx->frame_height_mbaligned,
                                 &ctx->src_surface[0], SURFACE_NUM,
                                 NULL, 0);
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    /* create reference surfaces */
    va_status = vaCreateSurfaces(
        ctx->va_dpy,
        VA_RT_FORMAT_YUV420, ctx->frame_width_mbaligned, ctx->frame_height_mbaligned,
        &ctx->ref_surface[0], SURFACE_NUM,
        NULL, 0
        );
    CHECK_VASTATUS(va_status, "vaCreateSurfaces");

    tmp_surfaceid = calloc(2 * SURFACE_NUM, sizeof(VASurfaceID));
    assert(tmp_surfaceid);
    memcpy(tmp_surfaceid, ctx->src_surface, SURFACE_NUM * sizeof(VASurfaceID));
    memcpy(tmp_surfaceid + SURFACE_NUM, ctx->ref_surface, SURFACE_NUM * sizeof(VASurfaceID));
    
    /* Create a context for this encode pipe */
    va_status = vaCreateContext(ctx->va_dpy, ctx->config_id,
                                ctx->frame_width_mbaligned, ctx->frame_height_mbaligned,
                                VA_PROGRESSIVE,
                                tmp_surfaceid, 2 * SURFACE_NUM,
                                &ctx->context_id);
    CHECK_VASTATUS(va_status, "vaCreateContext");
    free(tmp_surfaceid);

    codedbuf_size = (ctx->frame_width_mbaligned * ctx->frame_height_mbaligned * 400) / (16*16);

    for (i = 0; i < SURFACE_NUM; i++) {
        /* create coded buffer once for all
         * other VA buffers which won't be used again after vaRenderPicture.
         * so APP can always vaCreateBuffer for every frame
         * but coded buffer need to be mapped and accessed after vaRenderPicture/vaEndPicture
         * so VA won't maintain the coded buffer
         */
        va_status = vaCreateBuffer(ctx->va_dpy, ctx->context_id, VAEncCodedBufferType,
                codedbuf_size, 1, NULL, &ctx->coded_buf[i]);
        CHECK_VASTATUS(va_status,"vaCreateBuffer");
    }
    
    return 0;
}

#if 0
static struct storage_task_t * storage_task_dequeue(struct context_s *ctx)
{
    pthread_mutex_lock(&ctx->encode_mutex);

    struct storage_task_t *header = ctx->storage_task_header;
    if (ctx->storage_task_header != NULL) {
        if (ctx->storage_task_tail == ctx->storage_task_header)
            ctx->storage_task_tail = NULL;
        ctx->storage_task_header = header->next;
    }

    pthread_mutex_unlock(&ctx->encode_mutex);
    return header;
}

static int storage_task_queue(struct context_s *ctx, unsigned long long display_order, unsigned long long encode_order, obe_raw_frame_t *rf)
{
    struct storage_task_t *tmp = calloc(1, sizeof(struct storage_task_t));
    assert(tmp);
    tmp->display_order = display_order;
    tmp->encode_order = encode_order;

    pthread_mutex_lock(&ctx->encode_mutex);

    if (ctx->storage_task_header == NULL) {
        ctx->storage_task_header = tmp;
        ctx->storage_task_tail = tmp;
    } else {
        ctx->storage_task_tail->next = tmp;
        ctx->storage_task_tail = tmp;
    }

    ctx->srcsurface_status[display_order % SURFACE_NUM] = SRC_SURFACE_IN_STORAGE;
    pthread_cond_signal(&ctx->encode_cond);

    pthread_mutex_unlock(&ctx->encode_mutex);

    return 0;
}
#endif

static int save_recyuv(struct context_s *ctx, VASurfaceID surface_id,
                       unsigned long long display_order,
                       unsigned long long encode_order);

// MMM
static void storage_task(struct context_s *ctx, unsigned long long display_order, unsigned long long encode_order, obe_raw_frame_t *rf, int frame_type)
{
    VAStatus va_status = vaSyncSurface(ctx->va_dpy, ctx->src_surface[display_order % SURFACE_NUM]);
    CHECK_VASTATUS(va_status,"vaSyncSurface");
    save_codeddata(ctx, display_order, encode_order, rf, frame_type);

    save_recyuv(ctx, ctx->ref_surface[display_order % SURFACE_NUM], display_order, encode_order);

    /* reload a new frame data */
    if (ctx->srcyuv_fp != NULL)
        load_surface(ctx, ctx->src_surface[display_order % SURFACE_NUM], display_order + SURFACE_NUM);

    pthread_mutex_lock(&ctx->encode_mutex);
    ctx->srcsurface_status[display_order % SURFACE_NUM] = SRC_SURFACE_IN_ENCODING;
    pthread_mutex_unlock(&ctx->encode_mutex);
}

#if 0
static void *vaapi_storage_task_thread(void *t)
{
    struct context_s *ctx = (struct context_s *)t;

    while (1) {
        struct storage_task_t *current;

        current = storage_task_dequeue(ctx);
        if (current == NULL) {
            pthread_mutex_lock(&ctx->encode_mutex);
            pthread_cond_wait(&ctx->encode_cond, &ctx->encode_mutex);
            pthread_mutex_unlock(&ctx->encode_mutex);
            continue;
        }

        storage_task(ctx, current->display_order, current->encode_order);

        free(current);

        /* all frames are saved, exit the thread */
        if (++ctx->frame_coded >= 9999999999999999)
            break;
    }

    return 0;
}
#endif

// MMM
static int vaapi_init_va(struct context_s *ctx)
{
    ctx->MaxPicOrderCntLsb = (2 << 8);
    ctx->srcyuv_frames = 0;
    ctx->h264_profile = ~0;
    ctx->constraint_set_flag = 0;
    ctx->ip_period = 1;
    ctx->config_attrib_num = 0;
    ctx->rc_mode = -1;
    ctx->h264_packedheader = 0;
    ctx->enc_packed_header_idx = 0;
    ctx->h264_maxref = (1<<16|1);
    ctx->h264_entropy_mode = 1; /* cabac */
    ctx->frame_rate = 60;
    ctx->frame_width = ctx->enc_params->avc_param.i_width;
    ctx->frame_height = ctx->enc_params->avc_param.i_height;
    ctx->frame_width_mbaligned = (ctx->frame_width + 15) & (~15);
    ctx->frame_height_mbaligned = (ctx->frame_height + 15) & (~15);
    ctx->encode_syncmode = 1;
    ctx->frame_coded = 0;
    ctx->srcyuv_fp = NULL;
    ctx->recyuv_fp = NULL;
    ctx->coded_fp = fopen("/tmp/vaapi.nals", "wb");
    ctx->Log2MaxFrameNum = 16;
    ctx->Log2MaxPicOrderCntLsb = 8;
    ctx->current_frame_encoding = 0;
    ctx->initial_qp = 26;
    ctx->minimal_qp = 0;
    ctx->intra_period = 60;
    ctx->intra_idr_period = 60;
    ctx->current_frame_num = 0;
    ctx->storage_task_header = NULL;
    ctx->storage_task_tail = NULL;
    ctx->current_frame_display = 0;
    ctx->current_frame_type = 0;
    ctx->numShortTerm = 0;
    ctx->current_IDR_display = 0;
    ctx->num_ref_frames = 2;
    ctx->MaxFrameNum = (2<<16);
    ctx->srcyuv_fourcc = VA_FOURCC_NV12;
    ctx->frame_bitrate = ctx->frame_width * ctx->frame_height * 12 * ctx->frame_rate / 50;
    ctx->frame_bitrate /= 5;
printf("frame_bitrate = %d\n", ctx->frame_bitrate);
    ctx->misc_priv_type = 0;
    ctx->misc_priv_value = 0;

    pthread_mutex_init(&ctx->encode_mutex, NULL);
    pthread_cond_init(&ctx->encode_cond, NULL);

    VAProfile profile_list[]={VAProfileH264High,VAProfileH264Main,VAProfileH264ConstrainedBaseline};
    int slice_entrypoint;
    int support_encode = 0;    
    int major_ver, minor_ver;

    ctx->va_dpy = va_open_display();
    VAStatus va_status = vaInitialize(ctx->va_dpy, &major_ver, &minor_ver);
    CHECK_VASTATUS(va_status, "vaInitialize");

    int num_entrypoints = vaMaxNumEntrypoints(ctx->va_dpy);
    VAEntrypoint *entrypoints = malloc(num_entrypoints * sizeof(*entrypoints));
    if (!entrypoints) {
        fprintf(stderr, "error: failed to initialize VA entrypoints array\n");
        return -1;
    }

    /* use the highest profile */
    for (int i = 0; i < sizeof(profile_list) / sizeof(profile_list[0]); i++) {
        if ((ctx->h264_profile != ~0) && ctx->h264_profile != profile_list[i])
            continue;
        
        ctx->h264_profile = profile_list[i];
        vaQueryConfigEntrypoints(ctx->va_dpy, ctx->h264_profile, entrypoints, &num_entrypoints);
        for (slice_entrypoint = 0; slice_entrypoint < num_entrypoints; slice_entrypoint++) {
            if (entrypoints[slice_entrypoint] == VAEntrypointEncSlice) {
                support_encode = 1;
                break;
            }
        }
        if (support_encode == 1)
            break;
    }
    
    if (support_encode == 0) {
        printf("Can't find VAEntrypointEncSlice for H264 profiles\n");
        return -1;
    } else {
        switch (ctx->h264_profile) {
            case VAProfileH264ConstrainedBaseline:
                printf("Use profile VAProfileH264ConstrainedBaseline\n");
                ctx->constraint_set_flag |= (1 << 0 | 1 << 1); /* Annex A.2.2 */
                ctx->ip_period = 1;
                break;

            case VAProfileH264Main:
                printf("Use profile VAProfileH264Main\n");
                ctx->constraint_set_flag |= (1 << 1); /* Annex A.2.2 */
                break;

            case VAProfileH264High:
                ctx->constraint_set_flag |= (1 << 3); /* Annex A.2.4 */
                printf("Use profile VAProfileH264High\n");
                break;
            default:
                printf("unknow profile. Set to Constrained Baseline");
                ctx->h264_profile = VAProfileH264ConstrainedBaseline;
                ctx->constraint_set_flag |= (1 << 0 | 1 << 1); /* Annex A.2.1 & A.2.2 */
                ctx->ip_period = 1;
                break;
        }
    }

    /* find out the format for the render target, and rate control mode */
    for (int i = 0; i < VAConfigAttribTypeMax; i++)
        ctx->attrib[i].type = i;

    va_status = vaGetConfigAttributes(ctx->va_dpy, ctx->h264_profile, VAEntrypointEncSlice,
                                      &ctx->attrib[0], VAConfigAttribTypeMax);
    CHECK_VASTATUS(va_status, "vaGetConfigAttributes");
    /* check the interested configattrib */
    if ((ctx->attrib[VAConfigAttribRTFormat].value & VA_RT_FORMAT_YUV420) == 0) {
        printf("Not find desired YUV420 RT format\n");
        return -1;
    } else {
        ctx->config_attrib[ctx->config_attrib_num].type = VAConfigAttribRTFormat;
        ctx->config_attrib[ctx->config_attrib_num].value = VA_RT_FORMAT_YUV420;
        ctx->config_attrib_num++;
    }
    
    if (ctx->attrib[VAConfigAttribRateControl].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = ctx->attrib[VAConfigAttribRateControl].value;

        printf("Support rate control mode (0x%x):", tmp);
        
        if (tmp & VA_RC_NONE)
            printf("NONE ");
        if (tmp & VA_RC_CBR)
            printf("CBR ");
        if (tmp & VA_RC_VBR)
            printf("VBR ");
        if (tmp & VA_RC_VCM)
            printf("VCM ");
        if (tmp & VA_RC_CQP)
            printf("CQP ");
        if (tmp & VA_RC_VBR_CONSTRAINED)
            printf("VBR_CONSTRAINED ");

        printf("\n");

        if (ctx->rc_mode == -1 || !(ctx->rc_mode & tmp))  {
            if (ctx->rc_mode != -1) {
                printf("Warning: Don't support the specified RateControl mode: %s!!!, switch to ", rc_to_string(ctx->rc_mode));
            }

            for (int i = 0; i < sizeof(rc_default_modes) / sizeof(rc_default_modes[0]); i++) {
                if (rc_default_modes[i] & tmp) {
                    ctx->rc_mode = rc_default_modes[i];
                    break;
                }
            }

            printf("RateControl mode: %s\n", rc_to_string(ctx->rc_mode));
        }

        ctx->config_attrib[ctx->config_attrib_num].type = VAConfigAttribRateControl;
        ctx->config_attrib[ctx->config_attrib_num].value = ctx->rc_mode;
        ctx->config_attrib_num++;
    }
    

    if (ctx->attrib[VAConfigAttribEncPackedHeaders].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = ctx->attrib[VAConfigAttribEncPackedHeaders].value;

        printf("Support VAConfigAttribEncPackedHeaders\n");
        
        ctx->h264_packedheader = 1;
        ctx->config_attrib[ctx->config_attrib_num].type = VAConfigAttribEncPackedHeaders;
        ctx->config_attrib[ctx->config_attrib_num].value = VA_ENC_PACKED_HEADER_NONE;
        
        if (tmp & VA_ENC_PACKED_HEADER_SEQUENCE) {
            printf("Support packed sequence headers\n");
            ctx->config_attrib[ctx->config_attrib_num].value |= VA_ENC_PACKED_HEADER_SEQUENCE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_PICTURE) {
            printf("Support packed picture headers\n");
            ctx->config_attrib[ctx->config_attrib_num].value |= VA_ENC_PACKED_HEADER_PICTURE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_SLICE) {
            printf("Support packed slice headers\n");
            ctx->config_attrib[ctx->config_attrib_num].value |= VA_ENC_PACKED_HEADER_SLICE;
        }
        
        if (tmp & VA_ENC_PACKED_HEADER_MISC) {
            printf("Support packed misc headers\n");
            ctx->config_attrib[ctx->config_attrib_num].value |= VA_ENC_PACKED_HEADER_MISC;
        }
        
        ctx->enc_packed_header_idx = ctx->config_attrib_num;
        ctx->config_attrib_num++;
    }

    if (ctx->attrib[VAConfigAttribEncInterlaced].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = ctx->attrib[VAConfigAttribEncInterlaced].value;
        
        printf("Support VAConfigAttribEncInterlaced\n");

        if (tmp & VA_ENC_INTERLACED_FRAME)
            printf("support VA_ENC_INTERLACED_FRAME\n");
        if (tmp & VA_ENC_INTERLACED_FIELD)
            printf("Support VA_ENC_INTERLACED_FIELD\n");
        if (tmp & VA_ENC_INTERLACED_MBAFF)
            printf("Support VA_ENC_INTERLACED_MBAFF\n");
        if (tmp & VA_ENC_INTERLACED_PAFF)
            printf("Support VA_ENC_INTERLACED_PAFF\n");
        
        ctx->config_attrib[ctx->config_attrib_num].type = VAConfigAttribEncInterlaced;
        ctx->config_attrib[ctx->config_attrib_num].value = VA_ENC_PACKED_HEADER_NONE;
        ctx->config_attrib_num++;
    }
    
    if (ctx->attrib[VAConfigAttribEncMaxRefFrames].value != VA_ATTRIB_NOT_SUPPORTED) {
        ctx->h264_maxref = ctx->attrib[VAConfigAttribEncMaxRefFrames].value;
        
        printf("Support %d RefPicList0 and %d RefPicList1\n",
               ctx->h264_maxref & 0xffff, (ctx->h264_maxref >> 16) & 0xffff );
    }

    if (ctx->attrib[VAConfigAttribEncMaxSlices].value != VA_ATTRIB_NOT_SUPPORTED)
        printf("Support %d slices\n", ctx->attrib[VAConfigAttribEncMaxSlices].value);

    if (ctx->attrib[VAConfigAttribEncSliceStructure].value != VA_ATTRIB_NOT_SUPPORTED) {
        int tmp = ctx->attrib[VAConfigAttribEncSliceStructure].value;
        
        printf("Support VAConfigAttribEncSliceStructure\n");

        if (tmp & VA_ENC_SLICE_STRUCTURE_ARBITRARY_ROWS)
            printf("Support VA_ENC_SLICE_STRUCTURE_ARBITRARY_ROWS\n");
        if (tmp & VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS)
            printf("Support VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS\n");
        if (tmp & VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS)
            printf("Support VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS\n");
    }
    if (ctx->attrib[VAConfigAttribEncMacroblockInfo].value != VA_ATTRIB_NOT_SUPPORTED) {
        printf("Support VAConfigAttribEncMacroblockInfo\n");
    }

    free(entrypoints);

    /* ready for encoding */
    memset(ctx->srcsurface_status, SRC_SURFACE_IN_ENCODING, sizeof(ctx->srcsurface_status));
    memset(&ctx->seq_param, 0, sizeof(ctx->seq_param));
    memset(&ctx->pic_param, 0, sizeof(ctx->pic_param));
    memset(&ctx->slice_param, 0, sizeof(ctx->slice_param));

    //upload_source_YUV_once_for_all(ctx);
#if 0
    if (ctx->encode_syncmode == 0)
        pthread_create(&ctx->encode_thread, NULL, vaapi_storage_task_thread, ctx);
#endif
    
    return 0;
}


/*
  Assume frame sequence is: Frame#0,#1,#2,...,#M,...,#X,... (encoding order)
  1) period between Frame #X and Frame #N = #X - #N
  2) 0 means infinite for intra_period/intra_idr_period, and 0 is invalid for ip_period
  3) intra_idr_period % intra_period (intra_period > 0) and intra_period % ip_period must be 0
  4) intra_period and intra_idr_period take precedence over ip_period
  5) if ip_period > 1, intra_period and intra_idr_period are not  the strict periods 
     of I/IDR frames, see bellow examples
  -------------------------------------------------------------------
  intra_period intra_idr_period ip_period frame sequence (intra_period/intra_idr_period/ip_period)
  0            ignored          1          IDRPPPPPPP ...     (No IDR/I any more)
  0            ignored        >=2          IDR(PBB)(PBB)...   (No IDR/I any more)
  1            0                ignored    IDRIIIIIII...      (No IDR any more)
  1            1                ignored    IDR IDR IDR IDR...
  1            >=2              ignored    IDRII IDRII IDR... (1/3/ignore)
  >=2          0                1          IDRPPP IPPP I...   (3/0/1)
  >=2          0              >=2          IDR(PBB)(PBB)(IBB) (6/0/3)
                                              (PBB)(IBB)(PBB)(IBB)... 
  >=2          >=2              1          IDRPPPPP IPPPPP IPPPPP (6/18/1)
                                           IDRPPPPP IPPPPP IPPPPP...
  >=2          >=2              >=2        {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)} (6/18/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)(IBB)(PBB)}           (6/12/3)
                                           {IDR(PBB)(PBB)(IBB)(PBB)}...
                                           {IDR(PBB)(PBB)}                     (6/6/3)
                                           {IDR(PBB)(PBB)}.
*/

/*
 * Return displaying order with specified periods and encoding order
 * displaying_order: displaying order
 * frame_type: frame type 
 */
#define FRAME_P 0
#define FRAME_B 1
#define FRAME_I 2
#define FRAME_IDR 7
static void encoding2display_order(
    struct context_s *ctx,
    unsigned long long encoding_order,int intra_period,
    int intra_idr_period,int ip_period,
    uint64_t *displaying_order,
    int *frame_type)
{
    int encoding_order_gop = 0;

    if (intra_period == 1) { /* all are I/IDR frames */
        *displaying_order = encoding_order;
        if (intra_idr_period == 0)
            *frame_type = (encoding_order == 0)?FRAME_IDR:FRAME_I;
        else
            *frame_type = (encoding_order % intra_idr_period == 0)?FRAME_IDR:FRAME_I;
        return;
    }

    if (intra_period == 0)
        intra_idr_period = 0;

    /* new sequence like
     * IDR PPPPP IPPPPP
     * IDR (PBB)(PBB)(IBB)(PBB)
     */
    encoding_order_gop = (intra_idr_period == 0)? encoding_order:
        (encoding_order % (intra_idr_period + ((ip_period == 1)?0:1)));
         
    if (encoding_order_gop == 0) { /* the first frame */
        *frame_type = FRAME_IDR;
        *displaying_order = encoding_order;
    } else if (((encoding_order_gop - 1) % ip_period) != 0) { /* B frames */
	*frame_type = FRAME_B;
        *displaying_order = encoding_order - 1;
    } else if ((intra_period != 0) && /* have I frames */
               (encoding_order_gop >= 2) &&
               ((ip_period == 1 && encoding_order_gop % intra_period == 0) || /* for IDR PPPPP IPPPP */
                /* for IDR (PBB)(PBB)(IBB) */
                (ip_period >= 2 && ((encoding_order_gop - 1) / ip_period % (intra_period / ip_period)) == 0))) {
	*frame_type = FRAME_I;
	*displaying_order = encoding_order + ip_period - 1;
    } else {
	*frame_type = FRAME_P;
	*displaying_order = encoding_order + ip_period - 1;
    }
}


#define partition(ctx, ref, field, key, ascending)   \
    while (i <= j) {                            \
        if (ascending) {                        \
            while (ref[i].field < key)          \
                i++;                            \
            while (ref[j].field > key)          \
                j--;                            \
        } else {                                \
            while (ref[i].field > key)          \
                i++;                            \
            while (ref[j].field < key)          \
                j--;                            \
        }                                       \
        if (i <= j) {                           \
            tmp = ref[i];                       \
            ref[i] = ref[j];                    \
            ref[j] = tmp;                       \
            i++;                                \
            j--;                                \
        }                                       \
    }                                           \

static void sort_one(struct context_s *ctx, VAPictureH264 ref[], int left, int right,
                     int ascending, int frame_idx)
{
    int i = left, j = right;
    unsigned int key;
    VAPictureH264 tmp;

    if (frame_idx) {
        key = ref[(left + right) / 2].frame_idx;
        partition(ctx, ref, frame_idx, key, ascending);
    } else {
        key = ref[(left + right) / 2].TopFieldOrderCnt;
        partition(ctx, ref, TopFieldOrderCnt, (signed int)key, ascending);
    }
    
    /* recursion */
    if (left < j)
        sort_one(ctx, ref, left, j, ascending, frame_idx);
    
    if (i < right)
        sort_one(ctx, ref, i, right, ascending, frame_idx);
}

static void sort_two(struct context_s *ctx, VAPictureH264 ref[], int left, int right, unsigned int key, unsigned int frame_idx,
                     int partition_ascending, int list0_ascending, int list1_ascending)
{
    int i = left, j = right;
    VAPictureH264 tmp;

    if (frame_idx) {
        partition(ctx, ref, frame_idx, key, partition_ascending);
    } else {
        partition(ctx, ref, TopFieldOrderCnt, (signed int)key, partition_ascending);
    }
    

    sort_one(ctx, ref, left, i-1, list0_ascending, frame_idx);
    sort_one(ctx, ref, j+1, right, list1_ascending, frame_idx);
}

static int update_ReferenceFrames(struct context_s *ctx)
{
    int i;
    
    if (ctx->current_frame_type == FRAME_B)
        return 0;

    ctx->CurrentCurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    ctx->numShortTerm++;
    if (ctx->numShortTerm > ctx->num_ref_frames)
        ctx->numShortTerm = ctx->num_ref_frames;
    for (i=ctx->numShortTerm-1; i>0; i--)
        ctx->ReferenceFrames[i] = ctx->ReferenceFrames[i-1];
    ctx->ReferenceFrames[0] = ctx->CurrentCurrPic;
    
    if (ctx->current_frame_type != FRAME_B)
        ctx->current_frame_num++;
    if (ctx->current_frame_num > ctx->MaxFrameNum)
        ctx->current_frame_num = 0;
    
    return 0;
}


static int update_RefPicList(struct context_s *ctx)
{
    unsigned int current_poc = ctx->CurrentCurrPic.TopFieldOrderCnt;
    
    if (ctx->current_frame_type == FRAME_P) {
        memcpy(ctx->RefPicList0_P, ctx->ReferenceFrames, ctx->numShortTerm * sizeof(VAPictureH264));
        sort_one(ctx, ctx->RefPicList0_P, 0, ctx->numShortTerm-1, 0, 1);
    }
    
    if (ctx->current_frame_type == FRAME_B) {
        memcpy(ctx->RefPicList0_B, ctx->ReferenceFrames, ctx->numShortTerm * sizeof(VAPictureH264));
        sort_two(ctx, ctx->RefPicList0_B, 0, ctx->numShortTerm-1, current_poc, 0,
                 1, 0, 1);

        memcpy(ctx->RefPicList1_B, ctx->ReferenceFrames, ctx->numShortTerm * sizeof(VAPictureH264));
        sort_two(ctx, ctx->RefPicList1_B, 0, ctx->numShortTerm-1, current_poc, 0,
                 0, 1, 0);
    }
    
    return 0;
}


static int render_sequence(struct context_s *ctx)
{
    VABufferID seq_param_buf, rc_param_buf, misc_param_tmpbuf, render_id[2];
    VAStatus va_status;
    VAEncMiscParameterBuffer *misc_param, *misc_param_tmp;
    VAEncMiscParameterRateControl *misc_rate_ctrl;
    
    ctx->seq_param.level_idc = 41 /*SH_LEVEL_3*/;
    ctx->seq_param.picture_width_in_mbs = ctx->frame_width_mbaligned / 16;
    ctx->seq_param.picture_height_in_mbs = ctx->frame_height_mbaligned / 16;
    ctx->seq_param.bits_per_second = ctx->frame_bitrate;

    ctx->seq_param.intra_period = ctx->intra_period;
    ctx->seq_param.intra_idr_period = ctx->intra_idr_period;
    ctx->seq_param.ip_period = ctx->ip_period;

    ctx->seq_param.max_num_ref_frames = ctx->num_ref_frames;
    ctx->seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
    ctx->seq_param.time_scale = 900;
    ctx->seq_param.num_units_in_tick = 15; /* Tc = num_units_in_tick / time_sacle */
    ctx->seq_param.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = ctx->Log2MaxPicOrderCntLsb - 4;
    ctx->seq_param.seq_fields.bits.log2_max_frame_num_minus4 = ctx->Log2MaxFrameNum - 4;
    ctx->seq_param.seq_fields.bits.frame_mbs_only_flag = 1;
    ctx->seq_param.seq_fields.bits.chroma_format_idc = 1;
    ctx->seq_param.seq_fields.bits.direct_8x8_inference_flag = 1;
    
    if (ctx->frame_width != ctx->frame_width_mbaligned ||
        ctx->frame_height != ctx->frame_height_mbaligned) {
        ctx->seq_param.frame_cropping_flag = 1;
        ctx->seq_param.frame_crop_left_offset = 0;
        ctx->seq_param.frame_crop_right_offset = (ctx->frame_width_mbaligned - ctx->frame_width)/2;
        ctx->seq_param.frame_crop_top_offset = 0;
        ctx->seq_param.frame_crop_bottom_offset = (ctx->frame_height_mbaligned - ctx->frame_height)/2;
    }
    
    va_status = vaCreateBuffer(ctx->va_dpy, ctx->context_id,
                               VAEncSequenceParameterBufferType,
                               sizeof(ctx->seq_param), 1, &ctx->seq_param, &seq_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");
    
    va_status = vaCreateBuffer(ctx->va_dpy, ctx->context_id,
                               VAEncMiscParameterBufferType,
                               sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterRateControl),
                               1, NULL, &rc_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");
    
    vaMapBuffer(ctx->va_dpy, rc_param_buf,(void **)&misc_param);
    misc_param->type = VAEncMiscParameterTypeRateControl;
    misc_rate_ctrl = (VAEncMiscParameterRateControl *)misc_param->data;
    memset(misc_rate_ctrl, 0, sizeof(*misc_rate_ctrl));
    misc_rate_ctrl->bits_per_second = ctx->frame_bitrate;
    misc_rate_ctrl->target_percentage = 66;
    misc_rate_ctrl->window_size = 1000;
    misc_rate_ctrl->initial_qp = ctx->initial_qp;
    misc_rate_ctrl->min_qp = ctx->minimal_qp;
    misc_rate_ctrl->basic_unit_size = 0;
    vaUnmapBuffer(ctx->va_dpy, rc_param_buf);

    render_id[0] = seq_param_buf;
    render_id[1] = rc_param_buf;
    
    va_status = vaRenderPicture(ctx->va_dpy, ctx->context_id, &render_id[0], 2);
    CHECK_VASTATUS(va_status,"vaRenderPicture");;

    if (ctx->misc_priv_type != 0) {
        va_status = vaCreateBuffer(ctx->va_dpy, ctx->context_id,
                                   VAEncMiscParameterBufferType,
                                   sizeof(VAEncMiscParameterBuffer),
                                   1, NULL, &misc_param_tmpbuf);
        CHECK_VASTATUS(va_status,"vaCreateBuffer");
        vaMapBuffer(ctx->va_dpy, misc_param_tmpbuf,(void **)&misc_param_tmp);
        misc_param_tmp->type = ctx->misc_priv_type;
        misc_param_tmp->data[0] = ctx->misc_priv_value;
        vaUnmapBuffer(ctx->va_dpy, misc_param_tmpbuf);
    
        va_status = vaRenderPicture(ctx->va_dpy, ctx->context_id, &misc_param_tmpbuf, 1);
    }
    
    return 0;
}

static int calc_poc(struct context_s *ctx, int pic_order_cnt_lsb)
{
    static int PicOrderCntMsb_ref = 0, pic_order_cnt_lsb_ref = 0;
    int prevPicOrderCntMsb, prevPicOrderCntLsb;
    int PicOrderCntMsb, TopFieldOrderCnt;
    
    if (ctx->current_frame_type == FRAME_IDR)
        prevPicOrderCntMsb = prevPicOrderCntLsb = 0;
    else {
        prevPicOrderCntMsb = PicOrderCntMsb_ref;
        prevPicOrderCntLsb = pic_order_cnt_lsb_ref;
    }
    
    if ((pic_order_cnt_lsb < prevPicOrderCntLsb) &&
        ((prevPicOrderCntLsb - pic_order_cnt_lsb) >= (int)(ctx->MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = prevPicOrderCntMsb + ctx->MaxPicOrderCntLsb;
    else if ((pic_order_cnt_lsb > prevPicOrderCntLsb) &&
             ((pic_order_cnt_lsb - prevPicOrderCntLsb) > (int)(ctx->MaxPicOrderCntLsb / 2)))
        PicOrderCntMsb = prevPicOrderCntMsb - ctx->MaxPicOrderCntLsb;
    else
        PicOrderCntMsb = prevPicOrderCntMsb;
    
    TopFieldOrderCnt = PicOrderCntMsb + pic_order_cnt_lsb;

    if (ctx->current_frame_type != FRAME_B) {
        PicOrderCntMsb_ref = PicOrderCntMsb;
        pic_order_cnt_lsb_ref = pic_order_cnt_lsb;
    }
    
    return TopFieldOrderCnt;
}

static int render_picture(struct context_s *ctx)
{
    VABufferID pic_param_buf;
    VAStatus va_status;
    int i = 0;

    ctx->pic_param.CurrPic.picture_id = ctx->ref_surface[current_slot];
    ctx->pic_param.CurrPic.frame_idx = ctx->current_frame_num;
    ctx->pic_param.CurrPic.flags = 0;
    ctx->pic_param.CurrPic.TopFieldOrderCnt = calc_poc(ctx, (ctx->current_frame_display - ctx->current_IDR_display) % ctx->MaxPicOrderCntLsb);
    ctx->pic_param.CurrPic.BottomFieldOrderCnt = ctx->pic_param.CurrPic.TopFieldOrderCnt;
    ctx->CurrentCurrPic = ctx->pic_param.CurrPic;

    if (getenv("TO_DEL")) { /* set RefPicList into ReferenceFrames */
        update_RefPicList(ctx); /* calc RefPicList */
        memset(ctx->pic_param.ReferenceFrames, 0xff, 16 * sizeof(VAPictureH264)); /* invalid all */
        if (ctx->current_frame_type == FRAME_P) {
            ctx->pic_param.ReferenceFrames[0] = ctx->RefPicList0_P[0];
        } else if (ctx->current_frame_type == FRAME_B) {
            ctx->pic_param.ReferenceFrames[0] = ctx->RefPicList0_B[0];
            ctx->pic_param.ReferenceFrames[1] = ctx->RefPicList1_B[0];
        }
    } else {
        memcpy(ctx->pic_param.ReferenceFrames, ctx->ReferenceFrames, ctx->numShortTerm*sizeof(VAPictureH264));
        for (i = ctx->numShortTerm; i < SURFACE_NUM; i++) {
            ctx->pic_param.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
            ctx->pic_param.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
        }
    }
    
    ctx->pic_param.pic_fields.bits.idr_pic_flag = (ctx->current_frame_type == FRAME_IDR);
    ctx->pic_param.pic_fields.bits.reference_pic_flag = (ctx->current_frame_type != FRAME_B);
    ctx->pic_param.pic_fields.bits.entropy_coding_mode_flag = ctx->h264_entropy_mode;
    ctx->pic_param.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    ctx->pic_param.frame_num = ctx->current_frame_num;
    ctx->pic_param.coded_buf = ctx->coded_buf[current_slot];
#if 0
    ctx->pic_param.last_picture = (ctx->current_frame_encoding == ctx->frame_count);
#else
    ctx->pic_param.last_picture = 0; /* No, we're never the last picture. */
#endif
    ctx->pic_param.pic_init_qp = ctx->initial_qp;

    va_status = vaCreateBuffer(ctx->va_dpy, ctx->context_id, VAEncPictureParameterBufferType,
                               sizeof(ctx->pic_param), 1, &ctx->pic_param, &pic_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");;

    va_status = vaRenderPicture(ctx->va_dpy, ctx->context_id, &pic_param_buf, 1);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    return 0;
}

static int render_packedsequence(struct context_s *ctx)
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedseq_para_bufid, packedseq_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedseq_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_seq_buffer(ctx, &packedseq_buffer); 
    
    packedheader_param_buffer.type = VAEncPackedHeaderSequence;
    
    packedheader_param_buffer.bit_length = length_in_bits; /*length_in_bits*/
    packedheader_param_buffer.has_emulation_bytes = 0;
    va_status = vaCreateBuffer(ctx->va_dpy,
                               ctx->context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedseq_para_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    va_status = vaCreateBuffer(ctx->va_dpy,
                               ctx->context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedseq_buffer,
                               &packedseq_data_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    render_id[0] = packedseq_para_bufid;
    render_id[1] = packedseq_data_bufid;
    va_status = vaRenderPicture(ctx->va_dpy, ctx->context_id, render_id, 2);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    free(packedseq_buffer);
    
    return 0;
}

static int render_packedpicture(struct context_s *ctx)
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedpic_para_bufid, packedpic_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedpic_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_pic_buffer(ctx, &packedpic_buffer); 
    packedheader_param_buffer.type = VAEncPackedHeaderPicture;
    packedheader_param_buffer.bit_length = length_in_bits;
    packedheader_param_buffer.has_emulation_bytes = 0;

    va_status = vaCreateBuffer(ctx->va_dpy,
                               ctx->context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedpic_para_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    va_status = vaCreateBuffer(ctx->va_dpy,
                               ctx->context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedpic_buffer,
                               &packedpic_data_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    render_id[0] = packedpic_para_bufid;
    render_id[1] = packedpic_data_bufid;
    va_status = vaRenderPicture(ctx->va_dpy, ctx->context_id, render_id, 2);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    free(packedpic_buffer);
    
    return 0;
}

#if 0
static void render_packedsei(struct context_s *ctx)
{
    VAEncPackedHeaderParameterBuffer packed_header_param_buffer;
    VABufferID packed_sei_header_param_buf_id, packed_sei_buf_id, render_id[2];
    unsigned int length_in_bits /*offset_in_bytes*/;
    unsigned char *packed_sei_buffer = NULL;
    VAStatus va_status;
    int init_cpb_size, target_bit_rate, i_initial_cpb_removal_delay_length, i_initial_cpb_removal_delay;
    int i_cpb_removal_delay, i_dpb_output_delay_length, i_cpb_removal_delay_length;

    /* it comes for the bps defined in SPS */
    target_bit_rate = ctx->frame_bitrate;
    init_cpb_size = (target_bit_rate * 8) >> 10;
    i_initial_cpb_removal_delay = init_cpb_size * 0.5 * 1024 / target_bit_rate * 90000;

    i_cpb_removal_delay = 2;
    i_initial_cpb_removal_delay_length = 24;
    i_cpb_removal_delay_length = 24;
    i_dpb_output_delay_length = 24;

    length_in_bits = build_packed_sei_buffer_timing(ctx,
        i_initial_cpb_removal_delay_length,
        i_initial_cpb_removal_delay,
        0,
        i_cpb_removal_delay_length,
        i_cpb_removal_delay * ctx->current_frame_encoding,
        i_dpb_output_delay_length,
        0,
        &packed_sei_buffer);

    //offset_in_bytes = 0;
    packed_header_param_buffer.type = VAEncPackedHeaderRawData;
    packed_header_param_buffer.bit_length = length_in_bits;
    packed_header_param_buffer.has_emulation_bytes = 0;

    va_status = vaCreateBuffer(ctx->va_dpy,
                               ctx->context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packed_header_param_buffer), 1, &packed_header_param_buffer,
                               &packed_sei_header_param_buf_id);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    va_status = vaCreateBuffer(ctx->va_dpy,
                               ctx->context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packed_sei_buffer,
                               &packed_sei_buf_id);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");


    render_id[0] = packed_sei_header_param_buf_id;
    render_id[1] = packed_sei_buf_id;
    va_status = vaRenderPicture(ctx->va_dpy, ctx->context_id, render_id, 2);
    CHECK_VASTATUS(va_status,"vaRenderPicture");
    
    free(packed_sei_buffer);
        
    return;
}

static int render_hrd(struct context_s *ctx)
{
    VABufferID misc_parameter_hrd_buf_id;
    VAStatus va_status;
    VAEncMiscParameterBuffer *misc_param;
    VAEncMiscParameterHRD *misc_hrd_param;
    
    va_status = vaCreateBuffer(ctx->va_dpy, ctx->context_id,
                   VAEncMiscParameterBufferType,
                   sizeof(VAEncMiscParameterBuffer) + sizeof(VAEncMiscParameterHRD),
                   1,
                   NULL, 
                   &misc_parameter_hrd_buf_id);
    CHECK_VASTATUS(va_status, "vaCreateBuffer");

    vaMapBuffer(ctx->va_dpy,
                misc_parameter_hrd_buf_id,
                (void **)&misc_param);
    misc_param->type = VAEncMiscParameterTypeHRD;
    misc_hrd_param = (VAEncMiscParameterHRD *)misc_param->data;

    if (ctx->frame_bitrate > 0) {
        misc_hrd_param->initial_buffer_fullness = ctx->frame_bitrate * 1024 * 4;
        misc_hrd_param->buffer_size = ctx->frame_bitrate * 1024 * 8;
    } else {
        misc_hrd_param->initial_buffer_fullness = 0;
        misc_hrd_param->buffer_size = 0;
    }
    vaUnmapBuffer(ctx->va_dpy, misc_parameter_hrd_buf_id);

    va_status = vaRenderPicture(ctx->va_dpy, ctx->context_id, &misc_parameter_hrd_buf_id, 1);
    CHECK_VASTATUS(va_status,"vaRenderPicture");;

    return 0;
}
#endif

static void render_packedslice(struct context_s *ctx)
{
    VAEncPackedHeaderParameterBuffer packedheader_param_buffer;
    VABufferID packedslice_para_bufid, packedslice_data_bufid, render_id[2];
    unsigned int length_in_bits;
    unsigned char *packedslice_buffer = NULL;
    VAStatus va_status;

    length_in_bits = build_packed_slice_buffer(ctx, &packedslice_buffer);
    packedheader_param_buffer.type = VAEncPackedHeaderSlice;
    packedheader_param_buffer.bit_length = length_in_bits;
    packedheader_param_buffer.has_emulation_bytes = 0;

    va_status = vaCreateBuffer(ctx->va_dpy,
                               ctx->context_id,
                               VAEncPackedHeaderParameterBufferType,
                               sizeof(packedheader_param_buffer), 1, &packedheader_param_buffer,
                               &packedslice_para_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    va_status = vaCreateBuffer(ctx->va_dpy,
                               ctx->context_id,
                               VAEncPackedHeaderDataBufferType,
                               (length_in_bits + 7) / 8, 1, packedslice_buffer,
                               &packedslice_data_bufid);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");

    render_id[0] = packedslice_para_bufid;
    render_id[1] = packedslice_data_bufid;
    va_status = vaRenderPicture(ctx->va_dpy, ctx->context_id, render_id, 2);
    CHECK_VASTATUS(va_status,"vaRenderPicture");

    free(packedslice_buffer);
}

static int render_slice(struct context_s *ctx)
{
    VABufferID slice_param_buf;
    VAStatus va_status;
    int i;

    update_RefPicList(ctx);
    
    /* one frame, one slice */
    ctx->slice_param.macroblock_address = 0;
    ctx->slice_param.num_macroblocks = ctx->frame_width_mbaligned * ctx->frame_height_mbaligned/(16*16); /* Measured by MB */
    ctx->slice_param.slice_type = (ctx->current_frame_type == FRAME_IDR)?2:ctx->current_frame_type;
    if (ctx->current_frame_type == FRAME_IDR) {
        if (ctx->current_frame_encoding != 0)
            ++ctx->slice_param.idr_pic_id;
    } else if (ctx->current_frame_type == FRAME_P) {
        int refpiclist0_max = ctx->h264_maxref & 0xffff;
        memcpy(ctx->slice_param.RefPicList0, ctx->RefPicList0_P, ((refpiclist0_max > 32) ? 32 : refpiclist0_max)*sizeof(VAPictureH264));

        for (i = refpiclist0_max; i < 32; i++) {
            ctx->slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
            ctx->slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        }
    } else if (ctx->current_frame_type == FRAME_B) {
        int refpiclist0_max = ctx->h264_maxref & 0xffff;
        int refpiclist1_max = (ctx->h264_maxref >> 16) & 0xffff;

        memcpy(ctx->slice_param.RefPicList0, ctx->RefPicList0_B, ((refpiclist0_max > 32) ? 32 : refpiclist0_max)*sizeof(VAPictureH264));
        for (i = refpiclist0_max; i < 32; i++) {
            ctx->slice_param.RefPicList0[i].picture_id = VA_INVALID_SURFACE;
            ctx->slice_param.RefPicList0[i].flags = VA_PICTURE_H264_INVALID;
        }

        memcpy(ctx->slice_param.RefPicList1, ctx->RefPicList1_B, ((refpiclist1_max > 32) ? 32 : refpiclist1_max)*sizeof(VAPictureH264));
        for (i = refpiclist1_max; i < 32; i++) {
            ctx->slice_param.RefPicList1[i].picture_id = VA_INVALID_SURFACE;
            ctx->slice_param.RefPicList1[i].flags = VA_PICTURE_H264_INVALID;
        }
    }

    ctx->slice_param.slice_alpha_c0_offset_div2 = 0;
    ctx->slice_param.slice_beta_offset_div2 = 0;
    ctx->slice_param.direct_spatial_mv_pred_flag = 1;
    ctx->slice_param.pic_order_cnt_lsb = (ctx->current_frame_display - ctx->current_IDR_display) % ctx->MaxPicOrderCntLsb;
    

    if (ctx->h264_packedheader &&
        ctx->config_attrib[ctx->enc_packed_header_idx].value & VA_ENC_PACKED_HEADER_SLICE)
        render_packedslice(ctx);

    va_status = vaCreateBuffer(ctx->va_dpy, ctx->context_id, VAEncSliceParameterBufferType,
                               sizeof(ctx->slice_param), 1, &ctx->slice_param, &slice_param_buf);
    CHECK_VASTATUS(va_status,"vaCreateBuffer");;

    va_status = vaRenderPicture(ctx->va_dpy, ctx->context_id, &slice_param_buf, 1);
    CHECK_VASTATUS(va_status,"vaRenderPicture");
    
    return 0;
}

#if 0
static int scale_2dimage(unsigned char *src_img, int src_imgw, int src_imgh,
                         unsigned char *dst_img, int dst_imgw, int dst_imgh)
{
    int row=0, col=0;

    for (row=0; row<dst_imgh; row++) {
        for (col=0; col<dst_imgw; col++) {
            *(dst_img + row * dst_imgw + col) = *(src_img + (row * src_imgh/dst_imgh) * src_imgw + col * src_imgw/dst_imgw);
        }
    }

    return 0;
}

static int YUV_blend_with_pic(int width, int height,
                              unsigned char *Y_start, int Y_pitch,
                              unsigned char *U_start, int U_pitch,
                              unsigned char *V_start, int V_pitch,
                              unsigned int fourcc, int fixed_alpha)
{
    /* PIC YUV format */
    unsigned char *pic_y_old = yuvga_pic;
    unsigned char *pic_u_old = pic_y_old + 640*480;
    unsigned char *pic_v_old = pic_u_old + 640*480/4;
    unsigned char *pic_y, *pic_u, *pic_v;

    int alpha_values[] = {100,90,80,70,60,50,40,30,20,30,40,50,60,70,80,90};
    
    static int alpha_idx = 0;
    int alpha;
    int allocated = 0;
    
    int row, col;

    if (fixed_alpha == 0) {
        alpha = alpha_values[alpha_idx % 16 ];
        alpha_idx ++;
    } else
        alpha = fixed_alpha;

    //alpha = 0;
    
    pic_y = pic_y_old;
    pic_u = pic_u_old;
    pic_v = pic_v_old;
    
    if (width != 640 || height != 480) { /* need to scale the pic */
        pic_y = (unsigned char *)malloc(width * height);
        if(pic_y == NULL) {
           printf("Failed to allocate memory for pic_y\n");
           return -1;
        }

        pic_u = (unsigned char *)malloc(width * height/4);
        if(pic_u == NULL) {
           printf("Failed to allocate memory for pic_u\n");
           free(pic_y);
           return -1;
        }

        pic_v = (unsigned char *)malloc(width * height/4);
        if(pic_v == NULL) {
           printf("Failed to allocate memory for pic_v\n");
           free(pic_y);
           free(pic_u);
           return -1;
        }
        allocated = 1;

        memset(pic_y, 0, width * height);
        memset(pic_u, 0, width * height /4);
        memset(pic_v, 0, width * height /4);
        
        scale_2dimage(pic_y_old, 640, 480,
                      pic_y, width, height);
        scale_2dimage(pic_u_old, 320, 240,
                      pic_u, width/2, height/2);
        scale_2dimage(pic_v_old, 320, 240,
                      pic_v, width/2, height/2);
    }

    /* begin blend */

    /* Y plane */
    int Y_pixel_stride = 1;
    if (fourcc == VA_FOURCC_YUY2) 
        Y_pixel_stride = 2;
         
    for (row=0; row<height; row++) {
        unsigned char *p = Y_start + row * Y_pitch;
        unsigned char *q = pic_y + row * width;
        for (col=0; col<width; col++, q++) {
            *p  = *p * (100 - alpha) / 100 + *q * alpha/100;
            p += Y_pixel_stride;
        }
    }

    /* U/V plane */
    int U_pixel_stride = 0, V_pixel_stride = 0;
    int v_factor_to_nv12 = 1;
    switch (fourcc) {
    case VA_FOURCC_YV12:
        U_pixel_stride = V_pixel_stride = 1;
        break;
    case VA_FOURCC_NV12:
        U_pixel_stride = V_pixel_stride = 2;
        break;
    case VA_FOURCC_YUY2:
        U_pixel_stride = V_pixel_stride = 4;
        v_factor_to_nv12 = 2;
        break;
    default:
        break;
    }
    for (row=0; row<height/2*v_factor_to_nv12; row++) {
        unsigned char *pU = U_start + row * U_pitch;
        unsigned char *pV = V_start + row * V_pitch;
        unsigned char *qU = pic_u + row/v_factor_to_nv12 * width/2;
        unsigned char *qV = pic_v + row/v_factor_to_nv12 * width/2;
            
        for (col=0; col<width/2; col++, qU++, qV++) {
            *pU  = *pU * (100 - alpha) / 100 + *qU * alpha/100;
            *pV  = *pV * (100 - alpha) / 100 + *qV * alpha/100;

            pU += U_pixel_stride;
            pV += V_pixel_stride;
        }
    }
        
    if (allocated) {
        free(pic_y);
        free(pic_u);
        free(pic_v);
    }
    
    return 0;
}
#endif

static int yuvgen_planar(int width, int height,
                         unsigned char *Y_start, int Y_pitch,
                         unsigned char *U_start, int U_pitch,
                         unsigned char *V_start, int V_pitch,
                         unsigned int fourcc, int box_width, int row_shift,
                         int field)
{
    int row;
#if 0
    int alpha;
#endif

    unsigned char uv_value = 0x80;

    /* copy Y plane */
    int y_factor = 1;
    if (fourcc == VA_FOURCC_YUY2) y_factor = 2;
    for (row=0;row<height;row++) {
        unsigned char *Y_row = Y_start + row * Y_pitch;
        int jj, xpos, ypos;

        ypos = (row / box_width) & 0x1;

        /* fill garbage data into the other field */
        if (((field == VA_TOP_FIELD) && (row &1))
            || ((field == VA_BOTTOM_FIELD) && ((row &1)==0))) { 
            memset(Y_row, 0xff, width);
            continue;
        }
        
        for (jj=0; jj<width; jj++) {
            xpos = ((row_shift + jj) / box_width) & 0x1;
            if (xpos == ypos)
                Y_row[jj*y_factor] = 0xeb;
            else 
                Y_row[jj*y_factor] = 0x10;

            if (fourcc == VA_FOURCC_YUY2) {
                Y_row[jj*y_factor+1] = uv_value; // it is for UV
            }
        }
    }
  
    /* copy UV data */
    for( row =0; row < height/2; row++) {

        /* fill garbage data into the other field */
        if (((field == VA_TOP_FIELD) && (row &1))
            || ((field == VA_BOTTOM_FIELD) && ((row &1)==0))) {
            uv_value = 0xff;
        }

        unsigned char *U_row = U_start + row * U_pitch;
        unsigned char *V_row = V_start + row * V_pitch;
        switch (fourcc) {
        case VA_FOURCC_NV12:
            memset(U_row, uv_value, width);
            break;
        case VA_FOURCC_YV12:
            memset (U_row,uv_value,width/2);
            memset (V_row,uv_value,width/2);
            break;
        case VA_FOURCC_YUY2:
            // see above. it is set with Y update.
            break;
        default:
            printf("unsupported fourcc in loadsurface.h\n");
            assert(0);
        }
    }

#if 0
    if (getenv("AUTO_NOUV"))
        return 0;
 
    if (getenv("AUTO_ALPHA"))
        alpha = 0;
    else
        alpha = 70;

printf("%s() pre blend\n", __func__); 
    YUV_blend_with_pic(width,height,
                       Y_start, Y_pitch,
                       U_start, U_pitch,
                       V_start, V_pitch,
                       fourcc, alpha);
printf("%s() post blend\n", __func__); 
#endif
    
    return 0;
}

static int upload_surface(struct context_s *ctx, VADisplay va_dpy, VASurfaceID surface_id,
                          int box_width, int row_shift,
                          int field)
{
    VAImage surface_image;
    void *surface_p=NULL, *U_start = NULL,*V_start = NULL;
    VAStatus va_status;
    unsigned int pitches[3]={0,0,0};
    
    va_status = vaDeriveImage(va_dpy, surface_id, &surface_image);
    CHECK_VASTATUS(va_status,"vaDeriveImage");

    vaMapBuffer(va_dpy, surface_image.buf, &surface_p);
    assert(VA_STATUS_SUCCESS == va_status);

    pitches[0] = surface_image.pitches[0];
    switch (surface_image.format.fourcc) {
    case VA_FOURCC_NV12:
        printf("%s() VA_FOURCC_NV12\n", __func__);
        U_start = (char *)surface_p + surface_image.offsets[1];
        V_start = (char *)U_start + 1;
        pitches[1] = surface_image.pitches[1];
        pitches[2] = surface_image.pitches[1];
        break;
    case VA_FOURCC_IYUV:
        printf("%s() VA_FOURCC_IYUV\n", __func__);
        U_start = (char *)surface_p + surface_image.offsets[1];
        V_start = (char *)surface_p + surface_image.offsets[2];
        pitches[1] = surface_image.pitches[1];
        pitches[2] = surface_image.pitches[2];
        break;
    case VA_FOURCC_YV12:
        printf("%s() VA_FOURCC_YV12\n", __func__);
        U_start = (char *)surface_p + surface_image.offsets[2];
        V_start = (char *)surface_p + surface_image.offsets[1];
        pitches[1] = surface_image.pitches[2];
        pitches[2] = surface_image.pitches[1];
        break;
    case VA_FOURCC_YUY2:
        printf("%s() VA_FOURCC_YUY2\n", __func__);
        U_start = (char *)surface_p + 1;
        V_start = (char *)surface_p + 3;
        pitches[1] = surface_image.pitches[0];
        pitches[2] = surface_image.pitches[0];
        break;
    default:
        assert(0);
    }

printf("%s() planar\n", __func__);
    /* assume surface is planar format */
    yuvgen_planar(surface_image.width, surface_image.height,
                  (unsigned char *)surface_p, pitches[0],
                  (unsigned char *)U_start, pitches[1],
                  (unsigned char *)V_start, pitches[2],
                  surface_image.format.fourcc,
                  box_width, row_shift, field);
        
    vaUnmapBuffer(va_dpy,surface_image.buf);

printf("%s() destroy\n", __func__);
    vaDestroyImage(va_dpy,surface_image.image_id);

    return 0;
}

/*
 * Upload YUV data from memory into a surface
 * if src_fourcc == NV12, assume the buffer pointed by src_U
 * is UV interleaved (src_V is ignored)
 */
static int upload_surface_yuv(struct context_s *ctx, VADisplay va_dpy, VASurfaceID surface_id,
                              int src_fourcc, int src_width, int src_height,
                              unsigned char *src_Y, unsigned char *src_U, unsigned char *src_V)
{
    VAImage surface_image;
    unsigned char *surface_p=NULL, *Y_start=NULL, *U_start=NULL;
    int Y_pitch=0, U_pitch=0, row;
    VAStatus va_status;
    
    va_status = vaDeriveImage(va_dpy,surface_id, &surface_image);
    CHECK_VASTATUS(va_status,"vaDeriveImage");

    vaMapBuffer(va_dpy,surface_image.buf,(void **)&surface_p);
    assert(VA_STATUS_SUCCESS == va_status);

    Y_start = surface_p;
    Y_pitch = surface_image.pitches[0];
    switch (surface_image.format.fourcc) {
    case VA_FOURCC_NV12:
        U_start = (unsigned char *)surface_p + surface_image.offsets[1];
        U_pitch = surface_image.pitches[1];
        break;
    case VA_FOURCC_IYUV:
        U_start = (unsigned char *)surface_p + surface_image.offsets[1];
        U_pitch = surface_image.pitches[1];
        break;
    case VA_FOURCC_YV12:
        U_start = (unsigned char *)surface_p + surface_image.offsets[2];
        U_pitch = surface_image.pitches[2];
        break;
    case VA_FOURCC_YUY2:
        U_start = surface_p + 1;
        U_pitch = surface_image.pitches[0];
        break;
    default:
        assert(0);
    }

    /* copy Y plane */
    for (row=0;row<src_height;row++) {
        unsigned char *Y_row = Y_start + row * Y_pitch;
        memcpy(Y_row, src_Y + row*src_width, src_width);
    }
  
    for (row =0; row < src_height/2; row++) {
        unsigned char *U_row = U_start + row * U_pitch;
        unsigned char *u_ptr = NULL, *v_ptr=NULL;
        int j;
        switch (surface_image.format.fourcc) {
        case VA_FOURCC_NV12:
            if (src_fourcc == VA_FOURCC_NV12) {
                memcpy(U_row, src_U + row * src_width, src_width);
                break;
            } else if (src_fourcc == VA_FOURCC_IYUV) {
                u_ptr = src_U + row * (src_width/2);
                v_ptr = src_V + row * (src_width/2);
            } else if (src_fourcc == VA_FOURCC_YV12) {
                v_ptr = src_U + row * (src_width/2);
                u_ptr = src_V + row * (src_width/2);
            }
            if ((src_fourcc == VA_FOURCC_IYUV) ||
                (src_fourcc == VA_FOURCC_YV12)) {
                for(j = 0; j < src_width/2; j++) {
                    U_row[2*j] = u_ptr[j];
                    U_row[2*j+1] = v_ptr[j];
                }
            }
            break;
        case VA_FOURCC_IYUV:
        case VA_FOURCC_YV12:
        case VA_FOURCC_YUY2:
        default:
            printf("unsupported fourcc in load_surface_yuv\n");
            assert(0);
        }
    }
    
    vaUnmapBuffer(va_dpy,surface_image.buf);

    vaDestroyImage(va_dpy,surface_image.image_id);

    return 0;
}

static int upload_source_YUV_once_for_all(struct context_s *ctx)
{
    int box_width=8;
    int row_shift=0;
    int i;

    for (i = 0; i < SURFACE_NUM; i++) {
        printf("%s() Loading data into surface %d.....\n", __func__, i);
        upload_surface(ctx, ctx->va_dpy, ctx->src_surface[i], box_width, row_shift, 0);

        row_shift++;
        if (row_shift==(2*box_width)) row_shift= 0;
    }
    printf("Complete surface loading\n");

    return 0;
}

static int load_surface(struct context_s *ctx, VASurfaceID surface_id, unsigned long long display_order)
{
    unsigned char *srcyuv_ptr = NULL, *src_Y = NULL, *src_U = NULL, *src_V = NULL;
    unsigned long long frame_start, mmap_start;
    char *mmap_ptr = NULL;
    int frame_size, mmap_size;
    
    if (ctx->srcyuv_fp == NULL)
        return 0;
    
    /* allow encoding more than srcyuv_frames */    
    display_order = display_order % ctx->srcyuv_frames;
    frame_size = ctx->frame_width * ctx->frame_height * 3 / 2; /* for YUV420 */
    frame_start = display_order * frame_size;
    
    mmap_start = frame_start & (~0xfff);
    mmap_size = (frame_size + (frame_start & 0xfff) + 0xfff) & (~0xfff);
    mmap_ptr = mmap(0, mmap_size, PROT_READ, MAP_SHARED,
                    fileno(ctx->srcyuv_fp), mmap_start);
    if (mmap_ptr == MAP_FAILED) {
        printf("Failed to mmap YUV file (%s)\n", strerror(errno));
        return 1;
    }
    srcyuv_ptr = (unsigned char *)mmap_ptr +  (frame_start & 0xfff);
    if (ctx->srcyuv_fourcc == VA_FOURCC_NV12) {
        src_Y = srcyuv_ptr;
        src_U = src_Y + ctx->frame_width * ctx->frame_height;
        src_V = NULL;
    } else if (ctx->srcyuv_fourcc == VA_FOURCC_IYUV ||
        ctx->srcyuv_fourcc == VA_FOURCC_YV12) {
        src_Y = srcyuv_ptr;
        if (ctx->srcyuv_fourcc == VA_FOURCC_IYUV) {
            src_U = src_Y + ctx->frame_width * ctx->frame_height;
            src_V = src_U + (ctx->frame_width/2) * (ctx->frame_height/2);
        } else { /* YV12 */
            src_V = src_Y + ctx->frame_width * ctx->frame_height;
            src_U = src_V + (ctx->frame_width/2) * (ctx->frame_height/2);
        } 
    } else {
        printf("Unsupported source YUV format\n");
        if (mmap_ptr)
            munmap(mmap_ptr, mmap_size);
        exit(1);
    }
    
    upload_surface_yuv(ctx, ctx->va_dpy, surface_id,
                       ctx->srcyuv_fourcc, ctx->frame_width, ctx->frame_height,
                       src_Y, src_U, src_V);
    if (mmap_ptr)
        munmap(mmap_ptr, mmap_size);

    return 0;
}

/*
 * Download YUV data from a surface into memory
 * Some hardward doesn't have a aperture for linear access of
 * tiled surface, thus use vaGetImage to expect the implemnetion
 * to do tile to linear convert
 * 
 * if dst_fourcc == NV12, assume the buffer pointed by dst_U
 * is UV interleaved (src_V is ignored)
 */
static int download_surface_yuv(struct context_s *ctx, VADisplay va_dpy, VASurfaceID surface_id,
                                int dst_fourcc, int dst_width, int dst_height,
                                unsigned char *dst_Y, unsigned char *dst_U, unsigned char *dst_V)
{
    VAImage surface_image;
    unsigned char *surface_p=NULL, *Y_start=NULL, *U_start=NULL;
    int Y_pitch=0, U_pitch=0, row;
    VAStatus va_status;
    
    va_status = vaDeriveImage(va_dpy,surface_id, &surface_image);
    CHECK_VASTATUS(va_status,"vaDeriveImage");

    vaMapBuffer(va_dpy,surface_image.buf,(void **)&surface_p);
    assert(VA_STATUS_SUCCESS == va_status);

    Y_start = surface_p;
    Y_pitch = surface_image.pitches[0];
    switch (surface_image.format.fourcc) {
    case VA_FOURCC_NV12:
        U_start = (unsigned char *)surface_p + surface_image.offsets[1];
        U_pitch = surface_image.pitches[1];
        break;
    case VA_FOURCC_IYUV:
        U_start = (unsigned char *)surface_p + surface_image.offsets[1];
        U_pitch = surface_image.pitches[1];
        break;
    case VA_FOURCC_YV12:
        U_start = (unsigned char *)surface_p + surface_image.offsets[2];
        U_pitch = surface_image.pitches[2];
        break;
    case VA_FOURCC_YUY2:
        U_start = surface_p + 1;
        U_pitch = surface_image.pitches[0];
        break;
    default:
        assert(0);
    }

    /* copy Y plane */
    for (row=0;row<dst_height;row++) {
        unsigned char *Y_row = Y_start + row * Y_pitch;
        memcpy(dst_Y + row*dst_width, Y_row, dst_width);
    }
  
    for (row =0; row < dst_height/2; row++) {
        unsigned char *U_row = U_start + row * U_pitch;
        unsigned char *u_ptr = NULL, *v_ptr = NULL;
        int j;
        switch (surface_image.format.fourcc) {
        case VA_FOURCC_NV12:
            if (dst_fourcc == VA_FOURCC_NV12) {
                memcpy(dst_U + row * dst_width, U_row, dst_width);
                break;
            } else if (dst_fourcc == VA_FOURCC_IYUV) {
                u_ptr = dst_U + row * (dst_width/2);
                v_ptr = dst_V + row * (dst_width/2);
            } else if (dst_fourcc == VA_FOURCC_YV12) {
                v_ptr = dst_U + row * (dst_width/2);
                u_ptr = dst_V + row * (dst_width/2);
            }
            if ((dst_fourcc == VA_FOURCC_IYUV) ||
                (dst_fourcc == VA_FOURCC_YV12)) {
                for(j = 0; j < dst_width/2; j++) {
                    u_ptr[j] = U_row[2*j];
                    v_ptr[j] = U_row[2*j+1];
                }
            }
            break;
        case VA_FOURCC_IYUV:
        case VA_FOURCC_YV12:
        case VA_FOURCC_YUY2:
        default:
            printf("unsupported fourcc in load_surface_yuv\n");
            assert(0);
        }
    }
    
    vaUnmapBuffer(va_dpy,surface_image.buf);

    vaDestroyImage(va_dpy,surface_image.image_id);

    return 0;
}

static int save_recyuv(struct context_s *ctx, VASurfaceID surface_id,
                       unsigned long long display_order,
                       unsigned long long encode_order)
{
    unsigned char *dst_Y = NULL, *dst_U = NULL, *dst_V = NULL;

    if (ctx->recyuv_fp == NULL)
        return 0;

    if (ctx->srcyuv_fourcc == VA_FOURCC_NV12) {
        int uv_size = 2 * (ctx->frame_width/2) * (ctx->frame_height/2);
        dst_Y = malloc(2*uv_size);
        if(dst_Y == NULL) {
           printf("Failed to allocate memory for dst_Y\n");
           exit(1);
        }

        dst_U = malloc(uv_size);
        if(dst_U == NULL) {
           printf("Failed to allocate memory for dst_U\n");
           free(dst_Y);
           exit(1);
        }

        memset(dst_Y, 0, 2*uv_size);
        memset(dst_U, 0, uv_size);
    } else if (ctx->srcyuv_fourcc == VA_FOURCC_IYUV ||
               ctx->srcyuv_fourcc == VA_FOURCC_YV12) {
        int uv_size = (ctx->frame_width/2) * (ctx->frame_height/2);
        dst_Y = malloc(4*uv_size);
        if(dst_Y == NULL) {
           printf("Failed to allocate memory for dst_Y\n");
           exit(1);
        }

        dst_U = malloc(uv_size);
        if(dst_U == NULL) {
           printf("Failed to allocate memory for dst_U\n");
           free(dst_Y);
           exit(1);
        }

        dst_V = malloc(uv_size);
        if(dst_V == NULL) {
           printf("Failed to allocate memory for dst_V\n");
           free(dst_Y);
           free(dst_U);
           exit(1);
        }

        memset(dst_Y, 0, 4*uv_size);
        memset(dst_U, 0, uv_size);
        memset(dst_V, 0, uv_size);
    } else {
        printf("Unsupported source YUV format\n");
        exit(1);
    }
    
    download_surface_yuv(ctx, ctx->va_dpy, surface_id,
                         ctx->srcyuv_fourcc, ctx->frame_width, ctx->frame_height,
                         dst_Y, dst_U, dst_V);
    fseek(ctx->recyuv_fp, display_order * ctx->frame_width * ctx->frame_height * 1.5, SEEK_SET);

    if (ctx->srcyuv_fourcc == VA_FOURCC_NV12) {
        int uv_size = 2 * (ctx->frame_width/2) * (ctx->frame_height/2);
        fwrite(dst_Y, uv_size * 2, 1, ctx->recyuv_fp);
        fwrite(dst_U, uv_size, 1, ctx->recyuv_fp);
    } else if (ctx->srcyuv_fourcc == VA_FOURCC_IYUV ||
               ctx->srcyuv_fourcc == VA_FOURCC_YV12) {
        int uv_size = (ctx->frame_width/2) * (ctx->frame_height/2);
        fwrite(dst_Y, uv_size * 4, 1, ctx->recyuv_fp);
        
        if (ctx->srcyuv_fourcc == VA_FOURCC_IYUV) {
            fwrite(dst_U, uv_size, 1, ctx->recyuv_fp);
            fwrite(dst_V, uv_size, 1, ctx->recyuv_fp);
        } else {
            fwrite(dst_V, uv_size, 1, ctx->recyuv_fp);
            fwrite(dst_U, uv_size, 1, ctx->recyuv_fp);
        }
    }
    
    if (dst_Y)
        free(dst_Y);
    if (dst_U)
        free(dst_U);
    if (dst_V)
        free(dst_V);

    fflush(ctx->recyuv_fp);

    return 0;
}

static int save_codeddata(struct context_s *ctx, uint64_t display_order, uint64_t encode_order, obe_raw_frame_t *rf, int frame_type)
{    
    VACodedBufferSegment *buf_list = NULL;
    VAStatus va_status;
    unsigned int coded_size = 0;

    va_status = vaMapBuffer(ctx->va_dpy, ctx->coded_buf[display_order % SURFACE_NUM],(void **)(&buf_list));
    CHECK_VASTATUS(va_status,"vaMapBuffer");
    while (buf_list != NULL) {
        coded_size = avc_vaapi_deliver_nals(ctx, buf_list->buf, buf_list->size, rf, frame_type);
        buf_list = (VACodedBufferSegment *) buf_list->next;

        ctx->frame_size += coded_size;
    }
    vaUnmapBuffer(ctx->va_dpy, ctx->coded_buf[display_order % SURFACE_NUM]);

    return 0;
}

static int vaapi_encode_frame(struct context_s *ctx, obe_raw_frame_t *rf, const uint8_t *image_y, const uint8_t *image_u, const uint8_t *image_v)
{
	encoding2display_order(ctx, ctx->current_frame_encoding, ctx->intra_period, ctx->intra_idr_period, ctx->ip_period,
		&ctx->current_frame_display, &ctx->current_frame_type);

	if (ctx->current_frame_type == FRAME_IDR) {
		ctx->numShortTerm = 0;
		ctx->current_frame_num = 0;
		ctx->current_IDR_display = ctx->current_frame_display;
	}

	/* check if the source frame is ready */
	while (ctx->srcsurface_status[current_slot] != SRC_SURFACE_IN_ENCODING) {
		usleep(1);
	}

	/* Upload YUV into ctx->src_surface[current_slot] */
	upload_surface_yuv(ctx, ctx->va_dpy, ctx->src_surface[current_slot],
		ctx->srcyuv_fourcc, ctx->frame_width, ctx->frame_height,
		(unsigned char *)image_y, /* Plane Y */
		(unsigned char *)image_u, /* Plane U */
		(unsigned char *)image_v);/* Plane V */

	VAStatus va_status = vaBeginPicture(ctx->va_dpy, ctx->context_id, ctx->src_surface[current_slot]);
	CHECK_VASTATUS(va_status,"vaBeginPicture");
        
	if (ctx->current_frame_type == FRAME_IDR) {
		render_sequence(ctx);
		render_picture(ctx);            
		if (ctx->h264_packedheader) {
			render_packedsequence(ctx);
			render_packedpicture(ctx);
		}
		//if (rc_mode == VA_RC_CBR)
		//    render_packedsei();
		//render_hrd();
	} else {
		//render_sequence();
		render_picture(ctx);
		//if (rc_mode == VA_RC_CBR)
		//    render_packedsei();
		//render_hrd();
	}
	render_slice(ctx);
        
	va_status = vaEndPicture(ctx->va_dpy, ctx->context_id);
	CHECK_VASTATUS(va_status,"vaEndPicture");;

	if (ctx->encode_syncmode)
		storage_task(ctx, ctx->current_frame_display, ctx->current_frame_encoding, rf, ctx->current_frame_type);
#if 0
	else /* queue the storage task queue */
		storage_task_queue(ctx, ctx->current_frame_display, ctx->current_frame_encoding, rf);
#endif
        
	update_ReferenceFrames(ctx);

	ctx->current_frame_encoding++;
    
	return 0;
}

static int vaapi_release_encode(struct context_s *ctx)
{
    int i;

    vaDestroySurfaces(ctx->va_dpy, &ctx->src_surface[0], SURFACE_NUM);
    vaDestroySurfaces(ctx->va_dpy, &ctx->ref_surface[0], SURFACE_NUM);

    for (i = 0; i < SURFACE_NUM; i++)
        vaDestroyBuffer(ctx->va_dpy, ctx->coded_buf[i]);

    vaDestroyContext(ctx->va_dpy, ctx->context_id);
    vaDestroyConfig(ctx->va_dpy, ctx->config_id);

    return 0;
}

static int vaapi_deinit_va(struct context_s *ctx)
{
    vaTerminate(ctx->va_dpy);

    va_close_display(ctx->va_dpy);

    return 0;
}

/* END VAAPI SPECIFIC */

static size_t avc_vaapi_deliver_nals(struct context_s *ctx, uint8_t *buf, int lengthBytes, obe_raw_frame_t *rf, int frame_type)
{
	size_t len = lengthBytes;

	ctx->tmp_count++;
	ctx->tmp_bytes += lengthBytes;
	time_t now;
	time(&now);
	if (ctx->tmp_last != now) {
		printf("%s() nal buffers per sec = %d, bps = %d\n", __func__, ctx->tmp_count, ctx->tmp_bytes * 8);
		ctx->tmp_count = 0;
		ctx->tmp_last = now;
		ctx->tmp_bytes = 0;
	}
#if 0
	int hexlen = lengthBytes;
	if (hexlen > 16)
		hexlen = 16;

	printf("%s -- ",
		frame_type == FRAME_P ? "  P" :
		frame_type == FRAME_B ? "  B" :
		frame_type == FRAME_I ? "  I" :
		frame_type == FRAME_IDR ? "IDR" : "?");

	for (int i = 0; i < hexlen; i++)
		printf("%02x ", buf[i]);
	printf("\n");

//	printf("Write %d bytes\n", lengthBytes);
	fwrite(buf, 1, lengthBytes, ctx->coded_fp);
	if (ctx->coded_fp)
		fflush(ctx->coded_fp);
#endif

	obe_coded_frame_t *cf = new_coded_frame(ctx->encoder->output_stream_id, lengthBytes);
	if (!cf) {
		fprintf(stderr, MESSAGE_PREFIX " unable to alloc a new coded frame\n");
		return 0;
	}

	struct avfm_s *avfm = &rf->avfm;
	memcpy(&cf->avfm, &rf->avfm, sizeof(struct avfm_s));
	memcpy(cf->data, buf, lengthBytes);
	cf->len                      = lengthBytes;
	cf->type                     = CF_VIDEO;
	cf->arrival_time             = rf->arrival_time;

	int64_t offset = 24299700;
	offset = 450000 * 0;

	cf->real_pts                 = avfm->audio_pts + offset;
	cf->real_dts                 = avfm->audio_pts + offset;
	cf->pts                      = cf->real_pts;
	cf->cpb_initial_arrival_time = cf->real_dts - offset;

	double estimated_final = ((double)cf->len / 0.0810186) + (double)cf->cpb_initial_arrival_time;
	cf->cpb_final_arrival_time   = estimated_final;

	cf->priority = (frame_type == FRAME_IDR);
	cf->random_access = (frame_type == FRAME_IDR);

	coded_frame_print(cf);

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
		//cf->arrival_time = arrival_time;
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

	return len;
}

/* OBE will pass us a AVC struct initially. Pull out any important pieces
 * and pass those to x265.
 */
static void *avc_vaapi_start_encoder( void *ptr )
{
	struct context_s ectx, *ctx = &ectx;
	memset(ctx, 0, sizeof(*ctx));
	int ret = 0;

	ctx->enc_params = ptr;
	ctx->h = ctx->enc_params->h;
	ctx->encoder = ctx->enc_params->encoder;

	/* Fix this? its AVC specific. */
	ctx->encoder->encoder_params = malloc(sizeof(ctx->enc_params->avc_param) );
	if (!ctx->encoder->encoder_params) {
		fprintf(stderr, MESSAGE_PREFIX " failed to allocate encoder params\n");
		goto out1;
	}

	memcpy(ctx->encoder->encoder_params, &ctx->enc_params->avc_param, sizeof(ctx->enc_params->avc_param));
	ret = vaapi_init_va(ctx);
	if (ret < 0) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to initialize VA-API, ret = %d\n", ret);
		goto out2;
	}

	ret = vaapi_setup_encode(ctx);
	if (ret < 0) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to configure VA-API, ret = %d\n", ret);
		goto out3;
	}

	/* upload RAW YUV data into all surfaces */
	if (ctx->srcyuv_fp != NULL) {
		for (int i = 0; i < SURFACE_NUM; i++)
			load_surface(ctx, ctx->src_surface[i], i);
	} else
		upload_source_YUV_once_for_all(ctx);

#if 0
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
	x265_param_parse(ctx->hevc_params, "input-res", "1280x720");

	ctx->hevc_params->internalCsp = X265_CSP_I420;
	x265_param_parse(ctx->hevc_params, "repeat-headers", "1");

	snprintf(&val[0], sizeof(val), "%.3f", (float)ctx->enc_params->avc_param.i_fps_num / (float)ctx->enc_params->avc_param.i_fps_den);
	x265_param_parse(ctx->hevc_params, "fps", val);

	snprintf(&val[0], sizeof(val), "%d",ctx->enc_params->avc_param.i_keyint_max);
	x265_param_parse(ctx->hevc_params, "keyint", val);

	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.rc.i_vbv_buffer_size);
	x265_param_parse(ctx->hevc_params, "vbv-bufsize", val);

	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.rc.i_vbv_max_bitrate);
	x265_param_parse(ctx->hevc_params, "vbv-maxrate", val);

	/* 0 Is preferred, which is 'autodetect' */
	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.i_threads);
	x265_param_parse(ctx->hevc_params, "frame-threads", val);

//	x265_param_parse(ctx->hevc_params, "rc-lookahead", "4");
//	x265_param_parse(ctx->hevc_params, "vbv-minrate", "6000");
	snprintf(&val[0], sizeof(val), "%d", ctx->enc_params->avc_param.rc.i_bitrate);
	x265_param_parse(ctx->hevc_params, "bitrate", val);

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
#endif

	/* Lock the mutex until we verify and fetch new parameters */
	pthread_mutex_lock(&ctx->encoder->queue.mutex);

	ctx->encoder->is_ready = 1;

	int64_t frame_duration = av_rescale_q(1, (AVRational){ ctx->enc_params->avc_param.i_fps_den, ctx->enc_params->avc_param.i_fps_num},
		(AVRational){ 1, OBE_CLOCK } );

	printf("frame_duration = %" PRIi64 "\n", frame_duration);
	//buffer_duration = frame_duration * ctx->enc_params->avc_param.sc.i_buffer_size;

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

		struct userdata_s *ud = userdata_calloc();

		/* Cache the upstream timing information in userdata. */
		userdata_set(ud, &rf->avfm);

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
			/* Compress raw_frame to NALS. */
			/* Once the pipeline is completely full, x265_encoder_encode() will block until the next output picture is complete. */

			//obe_raw_frame_printf(rf);

			/* Technically it should be:
			 *  Y size = (ctx->frame_width * ctx->frame_height)
			 *  U size += ((ctx->frame_width * ctx->frame_height) / 4)
			 *  V size += ((ctx->frame_width * ctx->frame_height) / 4)
			 */
			uint8_t *f = (uint8_t *)malloc(ctx->frame_width * 2 * ctx->frame_height);

			uint8_t *dst_y = f;
			uint8_t *dst_uv = f + (ctx->frame_width * ctx->frame_height);

			/* This costs a few percent of a cpu */
			I420ToNV12(
				rf->img.plane[0], ctx->frame_width,
				rf->img.plane[1], ctx->frame_width / 4,
				rf->img.plane[2], ctx->frame_width / 4,
				dst_y, ctx->frame_width,
				dst_uv, ctx->frame_width / 2,
				ctx->frame_width, ctx->frame_height);

#if SEI_TIMESTAMPING
#if 0
/* NO SEI support yet. When we do add it, watch out for mallocs and small leaks
 * if we clone the HEVC iplementaiton.
 */
/* Start time - Always the last SEI */
static uint32_t framecount = 0;
x = &p->userSEI.payloads[count - 1];
x->payloadType = USER_DATA_AVC_UNREGISTERED;
x->payloadSize = SEI_TIMESTAMP_PAYLOAD_LENGTH;
x->payload = set_timestamp_alloc();

struct timeval tv;
gettimeofday(&tv, NULL);

set_timestamp_field_set(x->payload, 1, framecount);
set_timestamp_field_set(x->payload, 2, avfm_get_hw_received_tv_sec(&rf->avfm));
set_timestamp_field_set(x->payload, 3, avfm_get_hw_received_tv_usec(&rf->avfm));
set_timestamp_field_set(x->payload, 4, tv.tv_sec);
set_timestamp_field_set(x->payload, 5, tv.tv_usec);
set_timestamp_field_set(x->payload, 6, 0);
set_timestamp_field_set(x->payload, 7, 0);
set_timestamp_field_set(x->payload, 8, 0);
set_timestamp_field_set(x->payload, 9, 0);

/* The remaining 8 bytes (time exit from compressor fields)
 * will be filled when the frame exists the compressor. */
framecount++;
#endif
#endif

			vaapi_encode_frame(ctx, rf, f, dst_uv, NULL);

			free(f);

#if SEI_TIMESTAMPING
#if 0
			/* Walk through each of the NALS and insert current time into any LTN sei timestamp frames we find. */
			for (int m = 0; m < ctx->i_nal; m++) {
				int offset = ltn_uuid_find(&ctx->hevc_nals[m].payload[0], ctx->hevc_nals[m].sizeBytes);
				if (offset >= 0) {
				{
					struct timeval tv;
					gettimeofday(&tv, NULL);

					/* Add the time exit from compressor seconds/useconds. */
					set_timestamp_field_set(&ctx->hevc_nals[m].payload[offset], 6, tv.tv_sec);
					set_timestamp_field_set(&ctx->hevc_nals[m].payload[offset], 7, tv.tv_usec);
				}
			}
#endif
#endif

			rf->release_data(rf);
			rf->release_frame(rf);
			remove_from_queue(&ctx->encoder->queue);


			if (ret == 0) {
				//fprintf(stderr, MESSAGE_PREFIX " ret = %d\n", ret);
				leave = 1;
				continue;
			}
#if 0
			if (ret > 0) {
				for (int z = 0; z < ctx->i_nal; z++) {
					obe_coded_frame_t *cf = new_coded_frame(ctx->encoder->output_stream_id, ctx->hevc_nals[z].sizeBytes);
					if (!cf) {
						fprintf(stderr, MESSAGE_PREFIX " unable to alloc a new coded frame\n");
						break;
					}
#if LOCAL_DEBUG
					printf(MESSAGE_PREFIX " acquired %7d nals bytes (%d nals), pts = %12" PRIi64 " dts = %12" PRIi64 ", ret = %d, ",
						ctx->hevc_nals[z].sizeBytes, ctx->i_nal - z,
						ctx->hevc_picture_out->pts,
						ctx->hevc_picture_out->dts,
						ret);
					printf("poc %8d  sliceType %d [%s]\n",
						ctx->hevc_picture_out->poc, ctx->hevc_picture_out->sliceType,
						sliceTypeLookup(ctx->hevc_picture_out->sliceType));
#endif
					/* Prep the frame. */
#if 0
static FILE *fh = NULL;
if (fh == NULL)
  fh = fopen("/tmp/hevc.nals", "wb");

if (fh)
  fwrite(ctx->hevc_nals[z].payload, 1, ctx->hevc_nals[z].sizeBytes, fh);
#endif

					struct userdata_s *out_ud = ctx->hevc_picture_out->userData; 
					if (out_ud) {
						/* Make sure we push the original hardware timing into the new frame. */
						memcpy(&cf->avfm, &out_ud->avfm, sizeof(struct avfm_s));
						free(ctx->hevc_picture_out->userData);
						ctx->hevc_picture_out->userData = 0;

						cf->pts = out_ud->avfm.audio_pts;
					} else {
						//fprintf(stderr, MESSAGE_PREFIX " missing pic out userData\n");
					}

					memcpy(cf->data, ctx->hevc_nals[z].payload, ctx->hevc_nals[z].sizeBytes);
					cf->len                      = ctx->hevc_nals[z].sizeBytes;
					cf->type                     = CF_VIDEO;
					cf->pts                      = ctx->hevc_picture_out->pts + 45000;
					cf->real_pts                 = ctx->hevc_picture_out->pts + 45000;
					cf->real_dts                 = ctx->hevc_picture_out->dts;
					cf->cpb_initial_arrival_time = cf->real_pts;
					cf->cpb_final_arrival_time   = cf->real_pts + 45000;

#if 0
// X264 specific, I don't think we need to do this for HEVC
            cf->pts = coded_frame->avfm.audio_pts;

            /* The audio and video clocks jump with different intervals when the cable
             * is disconnected, suggestedint a BM firmware bug.
             * We'll use the audio clock regardless, for both audio and video compressors.
             */
            int64_t new_dts  = avfm->audio_pts + 24299700 - abs(cf->real_dts - cf->real_pts) + (2 * 450450);

            /* We need to userstand, for this temporal frame, how much it varies from the dts. */
            int64_t pts_diff = cf->real_dts - cf->real_pts;

            /* Construct a new PTS based on the hardware DTS and the PTS offset difference. */
            int64_t new_pts  = new_dts - pts_diff;

            cf->real_dts = new_dts;
            cf->real_pts = new_pts;
            cf->cpb_initial_arrival_time = new_dts;
            cf->cpb_final_arrival_time   = new_dts + abs(pic_out.hrd_timing.cpb_final_arrival_time - pic_out.hrd_timing.cpb_final_arrival_time);

            cpb_removal_time = cf->real_pts; /* Only used for manually eyeballing the video output clock. */

#endif
#if 0
			printf(MESSAGE_PREFIX " real_pts:%" PRIi64 " real_dts:%" PRIi64 " (%.3f %.3f)\n",
				cf->real_pts, cf->real_dts,
				ctx->hevc_picture_out->hrd_timing.dpb_output_time, pic_out.hrd_timing.cpb_removal_time);
#endif

					cf->priority = IS_X265_TYPE_I(ctx->hevc_picture_out->sliceType);
					cf->random_access = IS_X265_TYPE_I(ctx->hevc_picture_out->sliceType);

					if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
						cf->arrival_time = arrival_time;
						add_to_queue(&ctx->h->mux_queue, cf);
						//printf(MESSAGE_PREFIX " Encode Latency %"PRIi64" \n", obe_mdate() - cf->arrival_time);
					} else {
						add_to_queue(&ctx->h->enc_smoothing_queue, cf);
					}
				} /* For each NAL */
			} /* if nal_bytes > 0 */
#endif

			leave = 1;
		} /* While ! leave */

	} /* While (1) */

#if 0
	if (ctx->encode_syncmode == 0) {
		int ret;
		pthread_join(ctx->encode_thread, (void **)&ret);
	}
#endif

	vaapi_release_encode(ctx);
out3:
	vaapi_deinit_va(ctx);
out2:
	free(ctx->enc_params);
out1:
	return NULL;
}

const obe_vid_enc_func_t avc_vaapi_obe_encoder = { avc_vaapi_start_encoder };
