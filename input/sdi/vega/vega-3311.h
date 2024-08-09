#ifndef LTN_VEGA_H
#define LTN_VEGA_H

#include <apiVENC_types.h>
#include <apiVENC.h>
#include <VEGA3311_cap_types.h>
#include <VEGA3311_capture.h>
#include <VEGA_BQB_types.h>
#include <VEGA_BQB_encoder.h>
#include <VEGA_BQB_version.h>

extern "C"
{
#include "common/common.h"
#include "common/lavc.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <encoders/video/sei-timestamp.h>
}

#define MAX_VEGA_AUDIO_CHANNELS 16
#define MAX_VEGA_AUDIO_PAIRS (MAX_VEGA_AUDIO_CHANNELS / 2)

extern int g_decklink_monitor_hw_clocks;
extern int g_decklink_render_walltime;
extern int g_decklink_histogram_print_secs;
extern int g_decklink_histogram_reset;
extern int g_decklink_record_audio_buffers;
extern int g_decklink_inject_scte104_preroll6000;
extern int g_decklink_inject_scte104_fragmented;
extern struct klvanc_callbacks_s vega3311_vanc_callbacks;

struct obe_to_vega_video
{
    int progressive;            /* Boolean - Progressive or interlaced. */
    int obe_name;
    int width;                  /* Visual pixel width / height */
    int height;
    int vegaCaptureResolution;  /* SDK specific enum */
    int vegaEncodingResolution;
    int timebase_num;           /* Eg.  1001 */
    int timebase_den;           /* Eg. 60000 */
    API_VEGA_BQB_FPS_E vegaFramerate;          /* SDK specific enum */
};

const char *lookupVegaeTimeBase(API_VEGA_BQB_TIMEBASE_E tb);
const char *vega_lookupFrameType(API_VEGA_BQB_FRAME_TYPE_E type);
const char *lookupVegaSDILevelName(int v);
const char *lookupVegaPixelFormatName(int v);
const char *lookupVegaInputSourceName(int v);
const char *lookupVegaInputModeName(int v);
const char *lookupVegaBitDepthName(int v);
const char *lookupVegaEncodingChromaName(API_VEGA_BQB_CHROMA_FORMAT_E v);
const char *lookupVegaEncodingResolutionName(int v);
const char *lookupVegaAudioLayoutName(API_VEGA3311_CAP_AUDIO_LAYOUT_E v);
const char *lookupVegaAudioPacketSizeName(API_VEGA3311_CAP_AUDIO_PACKET_SIZE_E v);
const char *lookupVegaEncodingImageFormatName(int v);
int vega_has_source_signal_changed(API_VEGA3311_CAPTURE_FORMAT_T *src, API_VEGA3311_CAPTURE_FORMAT_T *dst);

const struct obe_to_vega_video *lookupVegaCaptureResolution(int std, int framerate, int interlaced);
const struct obe_to_vega_video *lookupVegaStandardByResolution(int width, int height, int framerate);
int lookupVegaFramerate(int num, int den, API_VEGA_BQB_FPS_E *fps);

void klvanc_packet_header_dump_console(struct klvanc_packet_header_s *pkt);
void vega_dump_signals_to_console(
        API_VEGA3311_CAPTURE_DEVICE_INFO_T st_dev_info,
        API_VEGA3311_CAPTURE_FORMAT_T input_src_info);

typedef struct
{
	/* Device input related */
	obe_device_t *device;
	obe_t *h;

        /* Capture layer and encoder layer configuration management */
        API_VEGA_BQB_INIT_PARAM_T init_params;
        API_VEGA_BQB_INIT_PARAM_T init_paramsMACRO;      /* TODO fix this during confirmance checking */
        API_VEGA_BQB_INIT_PARAM_T init_paramsMACROULL;   /* TODO fix this during confirmance checking */
        API_VEGA3311_CAP_INIT_PARAM_T ch_init_param;

        uint64_t framecount;
        int bDoLastFrame;
        int bLastFramePushed;
        pthread_mutex_t bDoLastFrame_lock;

        /* VANC Handling, attached the klvanc library for processing and parsing. */
        struct klvanc_context_s *vanchdl;

        /* Audio - Sample Rate Conversion. We convert S32 interleaved into S32P planer. */
        struct SwrContext *avr;

        /* SEI handling -- all managed through vega_sei_calls, no direct access for writes (only reads with a lock) */
        pthread_mutex_t          seiLock;
        int                      seiCount;
        API_VEGA_BQB_SEI_PARAM_T sei[API_MAX_SEI_NUM];

        /* SMPTE2038 packetizer */
        struct klvanc_smpte2038_packetizer_s *smpte2038_ctx;
        obe_sdi_non_display_data_t non_display_parser;

        /* Video timing tracking */
        int64_t videoLastPCR;
        int64_t videoLastDTS, videoLastPTS;
        int64_t videoDTSOffset;
        int64_t videoPTSOffset;
        int64_t lastAdjustedPicDTS;
        int64_t lastAdjustedPicPTS;
        int64_t lastcorrectedPicPTS; /* 90KHz. Last PTS submitted for video, we'll use this in vanc handling and 2038 as needed. */
        int64_t lastcorrectedPicDTS;

        /* Audio timing tracking */
        int64_t audioLastPCR;
        struct ltn_histogram_s *hg_callback_audio;
        struct ltn_histogram_s *hg_callback_video;

        /* Detected signals during probe */
        API_VEGA3311_CAPTURE_FORMAT_T detectedFormat;

        int interlacedTFF;
} vega_ctx_t;

void vega_sei_init(vega_ctx_t *ctx);
void vega_sei_lock(vega_ctx_t *ctx);
void vega_sei_unlock(vega_ctx_t *ctx);
int  vega_sei_append(vega_ctx_t *ctx, API_VEGA_BQB_SEI_PARAM_T *item);
void vega_sei_append_ltn_timing(vega_ctx_t *ctx);

typedef struct
{
    vega_ctx_t ctx;

    /* Input */
    int  brd_idx;       /* Board instance # */
    int card_idx;                       /* Port index # */

    int video_format;                   /* Eg. INPUT_VIDEO_FORMAT_720P_5994 */
    int num_audio_channels;             /* MAX_AUDIO_CHANNELS */

    int probe;                          /* Boolean. True if the hardware is currently in probe mode. */

    int probe_success;                  /* Boolean. Signal to the outer OBE core that probing is done. */

    int width;                          /* Eg. 1280 */
    int height;                         /* Eg. 720 */
    int timebase_num;                   /* Eg.  1001 */
    int timebase_den;                   /* Eg. 60000 */

    int interlaced;                     /* Boolean */
    //int tff;                            /* Boolean */

    /* configuration for the codec. */
    struct {
            int                             interlaced;
            int bitrate_kbps;
            API_VEGA_BQB_GOP_SIZE_E         gop_size;
            API_VEGA_BQB_B_FRAME_NUM_E      bframes;
            int width;
            int height;
            API_VEGA_BQB_RESOLUTION_E       encodingResolution;
            API_VEGA_BQB_IMAGE_FORMAT_E     eFormat;
            API_VEGA_BQB_FPS_E              fps;
            API_VEGA_BQB_CHROMA_FORMAT_E    chromaFormat;
            API_VEGA_BQB_BIT_DEPTH_E        bitDepth;
            API_VEGA3311_CAP_INPUT_MODE_E   inputMode;
            API_VEGA3311_CAP_INPUT_SOURCE_E inputSource;
            API_VEGA3311_CAP_SDI_LEVEL_E    sdiLevel;
            API_VEGA3311_CAP_AUDIO_LAYOUT_E audioLayout;
            API_VEGA3311_CAP_IMAGE_FORMAT_E pixelFormat;
    } codec;

#define OPTION_ENABLED(opt) (opts->enable_##opt)
#define OPTION_ENABLED_(opt) (opts_->enable_##opt)
    int enable_smpte2038;
    int enable_hdr;
    int enable_4k_quad;
    int enable_4k_2si;
#if 0
    int enable_vanc_cache;
    int enable_bitstream_audio;
    int enable_patch1;
    int enable_los_exit_ms;
    int enable_frame_injection;
    int enable_allow_1080p60;
#endif

} vega_opts_t;

void vega3311_vanc_callback(uint32_t u32DevId,
        API_VEGA3311_CAP_CHN_E eCh,
        API_VEGA3311_CAPTURE_FRAME_INFO_T* st_frame_info,
        API_VEGA3311_CAPTURE_FORMAT_T* st_input_info,
        void *pv_user_arg);

void vega3311_audio_callback(uint32_t u32DevId,
        API_VEGA3311_CAP_CHN_E eCh,
        API_VEGA3311_CAPTURE_FRAME_INFO_T *st_frame_info,
        API_VEGA3311_CAPTURE_FORMAT_T *st_input_info,
        void* pv_user_arg);

void vega3311_video_capture_callback(uint32_t u32DevId,
        API_VEGA3311_CAP_CHN_E eCh,
        API_VEGA3311_CAPTURE_FRAME_INFO_T *st_frame_info,
        API_VEGA3311_CAPTURE_FORMAT_T *st_input_info,
        void *pv_user_arg);

void vega3311_video_hevc_compressed_callback(API_VEGA_BQB_HEVC_CODED_PICT_T *p_pict, void *args);
void vega3311_video_avc_compressed_callback(API_VEGA_BQB_AVC_CODED_PICT_T *p_pict, void *args);
int  vega3311_video_configure_hevc(vega_opts_t *opts);
int  vega3311_video_configure_avc(vega_opts_t *opts);

#endif /* LTN_VEGA_H */
