/*****************************************************************************
 * videotoolbox.c : AVC encoding functions using Apple VideoToolbox
 *****************************************************************************
 * Copyright (C) 2026 LiveTimeNet Inc. All Rights Reserved.
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
#include <libyuv.h>

#include <CoreFoundation/CoreFoundation.h>
#include <VideoToolbox/VideoToolbox.h>

#define MESSAGE_PREFIX "[videotoolbox]: "

int g_videotoolbox_monitor_bps = 1;

struct context_s
{
	obe_vid_enc_params_t *enc_params;
	obe_t *h;
	obe_encoder_t *encoder;

	VTCompressionSessionRef session;

	uint64_t raw_frame_count;
	int frame_width, frame_height;
	int bit_rate_bps;
};

static void _monitor_bps(struct context_s *ctx, int lengthBytes)
{
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
		if (g_videotoolbox_monitor_bps) {
			printf(MESSAGE_PREFIX " codec output %.02f (Mb/ps) @ %s", dbps, ctime(&now));
		}
	}
	codec_bps_current += (lengthBytes * 8);
}

/* Extract the encoded access unit from a CMSampleBuffer and hand it downstream.
 * VideoToolbox delivers NALs in AVCC form (4 byte big-endian length prefix).
 * The muxer expects Annex-B (start codes), and expects SPS/PPS repeated on
 * every keyframe, so both are rebuilt here from the sample's format description.
 */
static size_t _deliver_nals(struct context_s *ctx, CMSampleBufferRef sampleBuffer, obe_raw_frame_t *rf, int is_keyframe)
{
	static const uint8_t start_code[4] = { 0x00, 0x00, 0x00, 0x01 };

	CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
	if (!block) {
		fprintf(stderr, MESSAGE_PREFIX "sample buffer has no data\n");
		return 0;
	}
	size_t blockLen = CMBlockBufferGetDataLength(block);

	const uint8_t *sps = NULL, *pps = NULL;
	size_t sps_len = 0, pps_len = 0;
	if (is_keyframe) {
		CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sampleBuffer);
		CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, &sps, &sps_len, NULL, NULL);
		CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 1, &pps, &pps_len, NULL, NULL);
	}

	size_t total = blockLen;
	if (is_keyframe)
		total += (4 + sps_len) + (4 + pps_len);

	obe_coded_frame_t *cf = new_coded_frame(ctx->encoder->output_stream_id, total);
	if (!cf) {
		fprintf(stderr, MESSAGE_PREFIX "unable to alloc a new coded frame\n");
		return 0;
	}

	uint8_t *dst = cf->data;
	if (is_keyframe) {
		memcpy(dst, start_code, 4); dst += 4;
		memcpy(dst, sps, sps_len);  dst += sps_len;
		memcpy(dst, start_code, 4); dst += 4;
		memcpy(dst, pps, pps_len);  dst += pps_len;
	}

	if (CMBlockBufferCopyDataBytes(block, 0, blockLen, dst) != kCMBlockBufferNoErr) {
		fprintf(stderr, MESSAGE_PREFIX "failed to copy encoded data\n");
		destroy_coded_frame(cf);
		return 0;
	}

	/* AVCC length prefixes are 4 bytes, same width as an Annex-B start code,
	 * so the rewrite happens in place.
	 */
	uint8_t *p = dst;
	uint8_t *end = dst + blockLen;
	while (p + 4 <= end) {
		uint32_t nalLen = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
		memcpy(p, start_code, 4);
		p += 4 + nalLen;
	}

	_monitor_bps(ctx, (int)total);

	cf->type = CF_VIDEO;
	cf->len  = (int)total;
	memcpy(&cf->avfm, &rf->avfm, sizeof(struct avfm_s));

	/* Frame reordering is disabled on the session, so encode order == display
	 * order and pts/dts collapse to the same value, mirroring the
	 * zero-b-frame paths in avcodec.c.
	 */
	cf->pts      = rf->avfm.audio_pts;
	cf->real_pts = rf->avfm.audio_pts;
	cf->real_dts = rf->avfm.audio_pts;

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
		cf->cpb_initial_arrival_time = cf->real_dts - 1351350; /* 3x frame interval */
	} else {
		cf->cpb_initial_arrival_time = cf->real_pts - 1351350; /* 3x frame interval */
	}

	double bit_rate = (double)ctx->bit_rate_bps;
	double fraction = bit_rate / 216000.0;
	double estimated = (double)cf->len / fraction;
	cf->cpb_final_arrival_time = estimated + (double)cf->cpb_initial_arrival_time;

	cf->priority = is_keyframe;
	cf->random_access = is_keyframe;

	if (g_sei_timestamping) {
		int offset = ltn_uuid_find(cf->data, cf->len);
		if (offset >= 0) {
			struct timeval tv;
			gettimeofday(&tv, NULL);
			sei_timestamp_field_set(&cf->data[offset], cf->len - offset, 6, tv.tv_sec);
			sei_timestamp_field_set(&cf->data[offset], cf->len - offset, 7, tv.tv_usec);
		}
	}

	if (ctx->h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || ctx->h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY) {
		cf->arrival_time = rf->arrival_time;
		add_to_queue(&ctx->h->mux_queue, cf);
	} else {
		add_to_queue(&ctx->h->enc_smoothing_queue, cf);
	}

	return total;
}

/* Called by VideoToolbox, synchronously, from within VTCompressionSessionCompleteFrames()
 * further down in this file -- encode_frame() always drains the session after submitting
 * a single frame, so at most one callback is ever in flight and ctx needs no locking here.
 */
static void vt_compression_output_callback(void *outputCallbackRefCon, void *sourceFrameRefCon,
	OSStatus status, VTEncodeInfoFlags infoFlags, CMSampleBufferRef sampleBuffer)
{
	struct context_s *ctx = outputCallbackRefCon;
	obe_raw_frame_t *rf = sourceFrameRefCon;

	if (status != noErr) {
		fprintf(stderr, MESSAGE_PREFIX "encode callback error %d\n", (int)status);
		return;
	}
	if (infoFlags & kVTEncodeInfo_FrameDropped) {
		fprintf(stderr, MESSAGE_PREFIX "frame dropped by VideoToolbox\n");
		return;
	}
	if (!sampleBuffer || !CMSampleBufferDataIsReady(sampleBuffer)) {
		fprintf(stderr, MESSAGE_PREFIX "sample buffer not ready\n");
		return;
	}

	int is_keyframe = 1;
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
	if (attachments && CFArrayGetCount(attachments) > 0) {
		CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
		is_keyframe = !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);
	}

	_deliver_nals(ctx, sampleBuffer, rf, is_keyframe);
}

static int vt_session_create(struct context_s *ctx)
{
	obe_vid_enc_params_t *ep = ctx->enc_params;

	ctx->frame_width  = ep->avc_param.i_width;
	ctx->frame_height = ep->avc_param.i_height;
	ctx->bit_rate_bps = ep->avc_param.rc.i_bitrate * 1000;

	printf(MESSAGE_PREFIX "Initializing as %dx%d\n", ctx->frame_width, ctx->frame_height);
	printf(MESSAGE_PREFIX "bitrate %d\n", ep->avc_param.rc.i_bitrate);
	printf(MESSAGE_PREFIX "vbv_max_bitrate %d\n", ep->avc_param.rc.i_vbv_max_bitrate);
	printf(MESSAGE_PREFIX "i_fps_num / den = { %d , %d }\n", ep->avc_param.i_fps_num, ep->avc_param.i_fps_den);

	CFMutableDictionaryRef encoderSpec = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionarySetValue(encoderSpec, kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder, kCFBooleanTrue);

	CFMutableDictionaryRef srcAttrs = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
		&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	int pixfmt = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	CFNumberRef pixfmtRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pixfmt);
	CFDictionarySetValue(srcAttrs, kCVPixelBufferPixelFormatTypeKey, pixfmtRef);
	CFRelease(pixfmtRef);

	OSStatus status = VTCompressionSessionCreate(kCFAllocatorDefault,
		ctx->frame_width, ctx->frame_height,
		kCMVideoCodecType_H264,
		encoderSpec,
		srcAttrs,
		NULL,
		vt_compression_output_callback,
		ctx,
		&ctx->session);

	CFRelease(encoderSpec);
	CFRelease(srcAttrs);

	if (status != noErr) {
		fprintf(stderr, MESSAGE_PREFIX "VTCompressionSessionCreate failed, err %d\n", (int)status);
		return -1;
	}

	VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
	VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse);
	VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_High_AutoLevel);

	CFNumberRef bitrateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &ctx->bit_rate_bps);
	VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
	CFRelease(bitrateRef);

	if (ep->avc_param.rc.i_vbv_max_bitrate > 0) {
		int64_t bytesPerSecond = (int64_t)ep->avc_param.rc.i_vbv_max_bitrate * 1000 / 8;
		double seconds = 1.0;
		CFNumberRef bytesRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &bytesPerSecond);
		CFNumberRef secondsRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &seconds);
		const void *limits[2] = { bytesRef, secondsRef };
		CFArrayRef limitsArray = CFArrayCreate(kCFAllocatorDefault, limits, 2, &kCFTypeArrayCallBacks);
		VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_DataRateLimits, limitsArray);
		CFRelease(limitsArray);
		CFRelease(bytesRef);
		CFRelease(secondsRef);
	}

	int keyint = ep->avc_param.i_keyint_max > 0 ? ep->avc_param.i_keyint_max : 30;
	CFNumberRef keyintRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &keyint);
	VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_MaxKeyFrameInterval, keyintRef);
	CFRelease(keyintRef);

	if (ep->avc_param.i_fps_num > 0) {
		double keyintSeconds = (double)keyint * ep->avc_param.i_fps_den / ep->avc_param.i_fps_num;
		CFNumberRef keyintSecondsRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &keyintSeconds);
		VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, keyintSecondsRef);
		CFRelease(keyintSecondsRef);

		int expectedFrameRate = ep->avc_param.i_fps_num / (ep->avc_param.i_fps_den > 0 ? ep->avc_param.i_fps_den : 1);
		CFNumberRef fpsRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &expectedFrameRate);
		VTSessionSetProperty(ctx->session, kVTCompressionPropertyKey_ExpectedFrameRate, fpsRef);
		CFRelease(fpsRef);
	}

	VTCompressionSessionPrepareToEncodeFrames(ctx->session);

	return 0;
}

static int vt_encode_frame(struct context_s *ctx, obe_raw_frame_t *rf)
{
	CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(ctx->session);
	CVPixelBufferRef pixelBuffer = NULL;
	CVReturn cvret = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixelBuffer);
	if (cvret != kCVReturnSuccess || !pixelBuffer) {
		fprintf(stderr, MESSAGE_PREFIX "unable to allocate a pixel buffer, err %d\n", cvret);
		return -1;
	}

	CVPixelBufferLockBaseAddress(pixelBuffer, 0);

	uint8_t *dst_y  = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);
	size_t dst_y_stride  = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
	uint8_t *dst_uv = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);
	size_t dst_uv_stride = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);

	I420ToNV12(
		rf->img.plane[0], rf->img.stride[0],
		rf->img.plane[1], rf->img.stride[1],
		rf->img.plane[2], rf->img.stride[2],
		dst_y, (int)dst_y_stride,
		dst_uv, (int)dst_uv_stride,
		ctx->frame_width, ctx->frame_height);

	CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);

	CMTime pts = CMTimeMake(rf->avfm.audio_pts, 27000000);
	CMTime duration = CMTimeMake(ctx->enc_params->avc_param.i_fps_den, ctx->enc_params->avc_param.i_fps_num);

	OSStatus status = VTCompressionSessionEncodeFrame(ctx->session, pixelBuffer, pts, duration,
		NULL, rf, NULL);

	CVPixelBufferRelease(pixelBuffer);

	if (status != noErr) {
		fprintf(stderr, MESSAGE_PREFIX "VTCompressionSessionEncodeFrame failed, err %d\n", (int)status);
		return -1;
	}

	/* Blocks until the callback for this frame (and any prior frame still in flight)
	 * has run, keeping this a synchronous send/receive cycle like avcodec.c's
	 * avcodec_send_frame()/avcodec_receive_packet() loop.
	 */
	VTCompressionSessionCompleteFrames(ctx->session, kCMTimeIndefinite);

	return 0;
}

static void *avc_videotoolbox_start_encoder(void *ptr)
{
	struct context_s ectx, *ctx = &ectx;
	memset(ctx, 0, sizeof(*ctx));

	ctx->enc_params = ptr;
	ctx->h = ctx->enc_params->h;
	ctx->encoder = ctx->enc_params->encoder;

	printf(MESSAGE_PREFIX "Starting encoder: %s\n",
		stream_format_name(obe_core_encoder_get_stream_format(ctx->encoder)));

	ctx->encoder->encoder_params = malloc(sizeof(ctx->enc_params->avc_param));
	if (!ctx->encoder->encoder_params) {
		fprintf(stderr, MESSAGE_PREFIX "failed to allocate encoder params\n");
		goto out1;
	}
	memcpy(ctx->encoder->encoder_params, &ctx->enc_params->avc_param, sizeof(ctx->enc_params->avc_param));

	if (vt_session_create(ctx) < 0) {
		fprintf(stderr, MESSAGE_PREFIX "Unable to initialize VideoToolbox\n");
		goto out2;
	}

	pthread_mutex_lock(&ctx->encoder->queue.mutex);
	ctx->encoder->is_ready = 1;
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

		vt_encode_frame(ctx, rf);

		rf->release_data(rf);
		rf->release_frame(rf);
		remove_from_queue(&ctx->encoder->queue);
	}

	VTCompressionSessionCompleteFrames(ctx->session, kCMTimeIndefinite);
	VTCompressionSessionInvalidate(ctx->session);
	CFRelease(ctx->session);

out2:
	free(ctx->enc_params);
out1:
	return NULL;
}

const obe_vid_enc_func_t avc_videotoolbox_obe_encoder = { avc_videotoolbox_start_encoder };
