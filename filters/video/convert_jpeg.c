#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include "common/common.h"
#include "common/bitstream.h"
#include "ltn_ws.h"

struct filter_compress_ctx
{
	time_t lastFrameTime;
	char *filename;
};

void filter_compress_free(struct filter_compress_ctx *ctx)
{
	free(ctx->filename);
	free(ctx);
}

int filter_compress_alloc(struct filter_compress_ctx **p, const char *filename)
{
	struct filter_compress_ctx *ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -1;

	ctx->filename = strdup(filename);

	*p = ctx;
	return 0;
}

int filter_compress_jpg(struct filter_compress_ctx *ctx, obe_raw_frame_t *rf)
{
	time_t now = time(0);
	if (ctx->lastFrameTime == now) {
		return -1;
	}

	ctx->lastFrameTime = now;

	AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	if (!codec) {
		printf("Codec not found\n");
		exit(1);
	}

	AVCodecContext *c = avcodec_alloc_context3(codec);
	if (!c) {
		printf("Could not allocate video codec context\n");
		exit(1);
	}

	c->bit_rate = 400000;
	c->width = rf->img.width;
	c->height = rf->img.height;
	c->time_base = (AVRational) { 1, 25 };
	c->pix_fmt = AV_PIX_FMT_YUVJ420P;
	av_opt_set(c->priv_data, "preset", "fast", 0);

	if (avcodec_open2(c, codec, NULL) < 0) {
		printf("Could not open codec\n");
		exit(1);
	}

	AVFrame *frame = av_frame_alloc();
	if (!frame) {
		printf("Could not allocate video frame\n");
		exit(1);
	}
	frame->format = c->pix_fmt;
	frame->width = c->width;
	frame->height = c->height;

	int ret = av_image_alloc(frame->data, frame->linesize, c->width,
		c->height, c->pix_fmt, 32);
	if (ret < 0) {
		printf("Could not allocate raw picture buffer\n");
		exit(1);
	}

	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	/* TODO: Is this correct for all formats and colorspaces / subsampling / 8/10bit? No */
	memcpy(frame->data[0], rf->img.plane[0], c->height * c->width);
	memcpy(frame->data[1], rf->img.plane[1], (c->height * c->width) / 4);
	memcpy(frame->data[2], rf->img.plane[2], (c->height * c->width) / 4);

	frame->pts = 1;

	int got_output = 0;
	ret = avcodec_send_frame(c, frame);
	while (ret >= 0) {
			ret = avcodec_receive_packet(c, &pkt);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
					return -1;
			else if (ret < 0) {
			}
			got_output = 1;
			break;
	}

	if (got_output) {
#if LTN_WS_ENABLE
		ltn_ws_set_thumbnail_jpg(g_ltn_ws_handle, pkt.data, pkt.size);
#endif

		char ofn[256];
		sprintf(ofn, "%s.new", ctx->filename);

		FILE *f = fopen(ofn, "wb");
		if (f) {
			fwrite(pkt.data, 1, pkt.size, f);
			fclose(f);
			rename(ofn, ctx->filename);
		}

		av_packet_unref(&pkt);
	}

	avcodec_close(c);
	av_free(c);
	av_freep(&frame->data[0]);
	av_frame_free(&frame);

	return 0;
}
