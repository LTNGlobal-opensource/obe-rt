#ifndef OBE_FILTERS_VIDEO_CONVERT_H
#define OBE_FILTERS_VIDEO_CONVERT_H

struct filter_compress_ctx;

int  filter_compress_alloc(struct filter_compress_ctx **ctx, const char *filename);
void filter_compress_free(struct filter_compress_ctx *ctx);
int  filter_compress_jpg(struct filter_compress_ctx *ctx, obe_raw_frame_t *rf);

#endif /* OBE_FILTERS_VIDEO_CONVERT_H */
