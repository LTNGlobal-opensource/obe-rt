/*****************************************************************************
 * decklink.cpp: BlackMagic DeckLink SDI input module
 *****************************************************************************
 * Copyright (C) 2010 Steinar H. Gunderson
 *
 * Authors: Steinar H. Gunderson <steinar+vlc@gunderson.no>
 *
 * SCTE35 / SCTE104 and general code hardening, debugging features et al.
 * Copyright (C) 2015-2017 Kernel Labs Inc.
 * Authors: Steven Toth <stoth@kernellabs.com>
 * Authors: Devin J Heitmueller <dheitmueller@kernellabs.com>
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

/*
 * ./pcm.x 1   memory bandwidth reporting
 *    dwts 720p59.94 encoding via avc-ll-20mb-1xmp2.cfg
 *       SKT   0     2.03     1.64     0.17      22.22    frame-injection on
 *       SKT   0     1.93     1.40     0.17      21.45    frame-injection off
 *
 */
#define __STDC_FORMAT_MACROS   1
#define __STDC_CONSTANT_MACROS 1

#define READ_OSD_VALUE 0

#define AUDIO_PULSE_OFFSET_MEASURMEASURE 0

#define PREFIX "[decklink]: "

#define typeof __typeof__

extern "C"
{
#include "common/common.h"
#include "common/lavc.h"
#include "common/scte104filtering.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include "input/sdi/ancillary.h"
#include "input/sdi/vbi.h"
#include "input/sdi/x86/sdi.h"
#include "input/sdi/smpte337_detector.h"
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>
#include <libltnsdi/ltnsdi.h>
#include "input/sdi/v210.h"
#include "common/bitstream.h"
}

#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>

#include <input/sdi/v210.h>
#include <assert.h>
#include <include/DeckLinkAPI.h>
#include "include/DeckLinkAPIDispatch.cpp"
#include <include/DeckLinkAPIVersion.h>
#include "histogram.h"
#include "ltn_ws.h"

#define container_of(ptr, type, member) ({          \
    const typeof(((type *)0)->member)*__mptr = (ptr);    \
             (type *)((char *)__mptr - offsetof(type, member)); })

static int64_t clock_offset = 0;
static uint64_t framesQueued = 0;

#define DECKLINK_VANC_LINES 100

extern char g_signal_format_detected[64];
extern struct timespec g_signal_detected_ts;
extern struct timespec g_signal_los_ts;
extern struct timespec g_cea708_detected_ts;
extern struct timespec g_scte104_detected_ts;
extern struct timespec g_smpte2038_detected_ts;

struct obe_to_decklink
{
    int obe_name;
    uint32_t bmd_name;
};

struct obe_to_decklink_video
{
    int obe_name;
    uint32_t bmd_name;
    int timebase_num;
    int timebase_den;
    int is_progressive;
    int visible_width;
    int visible_height;
    int callback_width;     /* Width, height, stride provided during the frame callback. */
    int callback_height;    /* Width, height, stride provided during the frame callback. */
    int callback_stride;    /* Width, height, stride provided during the frame callback. */
    const char *ascii_name;
};

const static struct obe_to_decklink video_conn_tab[] =
{
    { INPUT_VIDEO_CONNECTION_SDI,         bmdVideoConnectionSDI },
    { INPUT_VIDEO_CONNECTION_HDMI,        bmdVideoConnectionHDMI },
    { INPUT_VIDEO_CONNECTION_OPTICAL_SDI, bmdVideoConnectionOpticalSDI },
    { INPUT_VIDEO_CONNECTION_COMPONENT,   bmdVideoConnectionComponent },
    { INPUT_VIDEO_CONNECTION_COMPOSITE,   bmdVideoConnectionComposite },
    { INPUT_VIDEO_CONNECTION_S_VIDEO,     bmdVideoConnectionSVideo },
    { -1, 0 },
};

const static struct obe_to_decklink audio_conn_tab[] =
{
    { INPUT_AUDIO_EMBEDDED,               bmdAudioConnectionEmbedded },
    { INPUT_AUDIO_AES_EBU,                bmdAudioConnectionAESEBU },
    { INPUT_AUDIO_ANALOGUE,               bmdAudioConnectionAnalog },
    { -1, 0 },
};

const static struct obe_to_decklink_video video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_PAL,             bmdModePAL,           1,    25,    0,  720,  288,  720,  576, 1920, "720x576i", },
    { INPUT_VIDEO_FORMAT_NTSC,            bmdModeNTSC,          1001, 30000, 0,  720,  240,  720,  486, 1920, "720x480i",  },
    { INPUT_VIDEO_FORMAT_720P_50,         bmdModeHD720p50,      1,    50,    1, 1280,  720, 1280,  720, 3456, "1280x720p50", },
    { INPUT_VIDEO_FORMAT_720P_5994,       bmdModeHD720p5994,    1001, 60000, 1, 1280,  720, 1280,  720, 3456, "1280x720p59.94", },
    { INPUT_VIDEO_FORMAT_720P_60,         bmdModeHD720p60,      1,    60,    1, 1280,  720, 1280,  720, 3456, "1280x720p60", },
    { INPUT_VIDEO_FORMAT_1080I_50,        bmdModeHD1080i50,     1,    25,    0, 1920,  540, 1920, 1080, 5120, "1920x1080i25", },
    { INPUT_VIDEO_FORMAT_1080I_5994,      bmdModeHD1080i5994,   1001, 30000, 0, 1920,  540, 1920, 1080, 5120, "1920x1080i29.97", },
    { INPUT_VIDEO_FORMAT_1080I_60,        bmdModeHD1080i6000,   1,    30,    0, 1920,  540, 1920, 1080, 5120, "1920x1080i30", },
    { INPUT_VIDEO_FORMAT_1080P_2398,      bmdModeHD1080p2398,   1001, 24000, 1, 1920, 1080, 1920, 1080, 5120, "1920x1080p23.98", },
    { INPUT_VIDEO_FORMAT_1080P_24,        bmdModeHD1080p24,     1,    24,    1, 1920, 1080, 1920, 1080, 5120, "1920x1080p24", },
    { INPUT_VIDEO_FORMAT_1080P_25,        bmdModeHD1080p25,     1,    25,    1, 1920, 1080, 1920, 1080, 5120, "1920x1080p25", },
    { INPUT_VIDEO_FORMAT_1080P_2997,      bmdModeHD1080p2997,   1001, 30000, 1, 1920, 1080, 1920, 1080, 5120, "1920x1080p29.97", },
    { INPUT_VIDEO_FORMAT_1080P_30,        bmdModeHD1080p30,     1,    30,    1, 1920, 1080, 1920, 1080, 5120, "1920x1080p30", },
    { INPUT_VIDEO_FORMAT_1080P_50,        bmdModeHD1080p50,     1,    50,    1, 1920, 1080, 1920, 1080, 5120, "1920x1080p50", },
    { INPUT_VIDEO_FORMAT_1080P_5994,      bmdModeHD1080p5994,   1001, 60000, 1, 1920, 1080, 1920, 1080, 5120, "1920x1080p59.94", },
    { INPUT_VIDEO_FORMAT_1080P_60,        bmdModeHD1080p6000,   1,    60,    1, 1920, 1080, 1920, 1080, 5120, "1920x1080p60", },
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a0b0000 /* 10.11.0 */
/* These are also usable in 10.8.5 */
/* to avoid segfaults, ssearch in this file for TODO: When using 4k formats, other changes needed. */
    /* 4K */
    { INPUT_VIDEO_FORMAT_2160P_25,        bmdMode4K2160p25,     1,    25,    1, 3840, 2160, 3840, 2160, 5120, "3840x2160p25", },
    { INPUT_VIDEO_FORMAT_2160P_2997,      bmdMode4K2160p2997,   1001, 30000, 1, 3840, 2160, 3840, 2160, 5120, "3840x2160p29.97", },
    { INPUT_VIDEO_FORMAT_2160P_30,        bmdMode4K2160p30,     1,    30,    1, 3840, 2160, 3840, 2160, 5120, "3840x2160p30", },
    { INPUT_VIDEO_FORMAT_2160P_50,        bmdMode4K2160p50,     1,    50,    1, 3840, 2160, 3840, 2160, 5120, "3840x2160p50", },
    { INPUT_VIDEO_FORMAT_2160P_5994,      bmdMode4K2160p5994,   1001, 60000, 1, 3840, 2160, 3840, 2160, 5120, "3840x2160p59.94", },
#endif
    { -1, 0, -1, -1 },
};

static char g_modeName[5]; /* Racey */
static const char *getModeName(BMDDisplayMode m)
{
	g_modeName[0] = m >> 24;
	g_modeName[1] = m >> 16;
	g_modeName[2] = m >>  8;
	g_modeName[3] = m >>  0;
	g_modeName[4] = 0;
	return &g_modeName[0];
}

class DeckLinkCaptureDelegate;

struct audio_pair_s {
    int    nr; /* 0 - 7 */
    struct smpte337_detector_s *smpte337_detector;
    int    smpte337_detected_ac3;
    int    smpte337_frames_written;
    void  *decklink_ctx;
    int    input_stream_id; /* We need this during capture, so we can forward the payload to the right output encoder. */
};

typedef struct
{
    IDeckLink *p_card;
    IDeckLinkInput *p_input;
    DeckLinkCaptureDelegate *p_delegate;

    /* we need to hold onto the IDeckLinkConfiguration object, or our settings will not apply.
       see section 2.4.15 of the blackmagic decklink sdk documentation. */
    IDeckLinkConfiguration *p_config;

    /* Video */
    AVCodec         *dec;
    AVCodecContext  *codec;

    /* Audio - Sample Rate Conversion. We convert S32 interleaved into S32P planer. */
    struct SwrContext *avr;

    int64_t last_frame_time;

    /* VBI */
    int has_setup_vbi;

    /* Ancillary */
    void (*unpack_line) ( uint32_t *src, uint16_t *dst, int width );
    void (*downscale_line) ( uint16_t *src, uint8_t *dst, int lines );
    void (*blank_line) ( uint16_t *dst, int width );
    obe_sdi_non_display_data_t non_display_parser;

    obe_device_t *device;
    obe_t *h;
    BMDDisplayMode enabled_mode_id;
    const struct obe_to_decklink_video *enabled_mode_fmt;

    /* LIBKLVANC handle / context */
    struct klvanc_context_s *vanchdl;
#define VANC_CACHE_DUMP_INTERVAL 60
    time_t last_vanc_cache_dump;

    BMDTimeValue stream_time;

    /* SMPTE2038 packetizer */
    struct klvanc_smpte2038_packetizer_s *smpte2038_ctx;

#if KL_PRBS_INPUT
    struct prbs_context_s prbs;
#endif
#define MAX_AUDIO_PAIRS 8
    struct audio_pair_s audio_pairs[MAX_AUDIO_PAIRS];

    int isHalfDuplex;
    BMDTimeValue vframe_duration;

    struct ltn_histogram_s *callback_hdl;
    struct ltn_histogram_s *callback_duration_hdl;
    struct ltn_histogram_s *callback_1_hdl;
    struct ltn_histogram_s *callback_2_hdl;
    struct ltn_histogram_s *callback_3_hdl;
    struct ltn_histogram_s *callback_4_hdl;

    /* The VANC callbacks may need to have their contents cached with the
     * associated video frame. Let's build up any relevant metadata here,
     * for each frame, then attach it to the final video raw_frame prior
     * to submission to the downstream pipeline.
     * At the entry and exit of VideoInputFrameArrived this list is destroyed,
     * the timedVideoInputFrameArrived func manages it general insert/remove.
     * for this array. The duplicated items go into the raw frame and they're
     * managed downstream (in each video codec).
     */
    struct avmetadata_s metadataVANC;

    struct ltnsdi_context_s *sdi_audio_analyzer_ctx;

    struct {
        uint8_t hours;
        uint8_t minutes;
        uint8_t seconds;
        uint8_t frames;
        int32_t drop_frame;
        int32_t present;
    } timecode;

} decklink_ctx_t;

typedef struct
{
    decklink_ctx_t decklink_ctx;

    /* Input */
    int card_idx;
    int video_conn;
    int audio_conn;

    int video_format;
    int num_channels;
    int probe;
#define OPTION_ENABLED(opt) (decklink_opts->enable_##opt)
#define OPTION_ENABLED_(opt) (decklink_opts_->enable_##opt)
    int enable_smpte2038;
    int enable_smpte2031;
    int enable_vanc_cache;
    int enable_bitstream_audio;
    int enable_patch1;
    int enable_los_exit_ms;
    int enable_frame_injection;
    int enable_allow_1080p60;

    /* Output */
    int probe_success;

    int width;
    int coded_height;
    int height;

    int timebase_num;
    int timebase_den;

    /* Some equipment occasionally sends very short audio frames just prior to signal loss.
     * This creates poor MP2 audio PTS timing and results in A/V drift of 800ms or worse.
     * We'll detect and discard these frames by precalculating
     * a valid min and max value, then doing windowed detection during frame arrival.
     */
    int audio_sfc_min, audio_sfc_max;

    int interlaced;
    int tff;
} decklink_opts_t;

struct decklink_status
{
    obe_input_params_t *input;
    decklink_opts_t *decklink_opts;
};

void klsyslog_and_stdout(int level, const char *format, ...)
{
    char buf[2048] = { 0 };
    struct timeval tv;
    gettimeofday(&tv, 0);

    va_list vl;
    va_start(vl,format);
    vsprintf(&buf[strlen(buf)], format, vl);
    va_end(vl);

    syslog(level, "%s", buf);
    printf("%s\n", buf);
}

void kllog(const char *category, const char *format, ...)
{
    char buf[2048] = { 0 };
    struct timeval tv;
    gettimeofday(&tv, 0);

    //sprintf(buf, "%08d.%03d : OBE : ", (unsigned int)tv.tv_sec, (unsigned int)tv.tv_usec / 1000);
    sprintf(buf, "OBE-%s : ", category);

    va_list vl;
    va_start(vl,format);
    vsprintf(&buf[strlen(buf)], format, vl);
    va_end(vl);

    syslog(LOG_INFO | LOG_LOCAL4, "%s", buf);
}

static void endian_flip_array(uint8_t *buf, int bufSize)
{
        unsigned char v;
        for (int i = 0; i < bufSize; i += 2) {
                v = buf[i];
                buf[i] = buf[i + 1];
                buf[i + 1] = v;
        }
}

static int _vancparse(struct klvanc_context_s *ctx, uint8_t *sec, int byteCount, int lineNr)
{
    unsigned short *arr = (unsigned short *)calloc(1, LIBKLVANC_PACKET_MAX_PAYLOAD * sizeof(unsigned short));
    if (arr == NULL)
        return -1;

    for (int i = 0; i < (byteCount / 2); i++) {
        arr[i] = sec[i * 2] << 8 | sec[i * 2 + 1];
    }

    int ret = klvanc_packet_parse(ctx, lineNr, arr, byteCount / sizeof(unsigned short));
    free(arr);

    return ret;
}

static void calculate_audio_sfc_window(decklink_opts_t *opts)
{
    double n = opts->timebase_num;
    double d = opts->timebase_den;
    double fps = d / n;
    double samplerate = 48000;
    double marginpct = 0.005;

    opts->audio_sfc_min = (samplerate * ((double)1 - marginpct)) / fps;
    opts->audio_sfc_max = (samplerate * ((double)1 + marginpct)) / fps;
    //printf("%s() audio_sfc_min/max = %d/%d\n", __func__, opts->audio_sfc_min, opts->audio_sfc_max);
}

static int transmit_pes_to_muxer(decklink_ctx_t *decklink_ctx, uint8_t *buf, uint32_t byteCount, stream_formats_e stream_format);

/* Take one line of V210 from VANC, colorspace convert and feed it to the
 * VANC parser. We'll expect our VANC message callbacks to happen on this
 * same calling thread.
 */
static void convert_colorspace_and_parse_vanc(decklink_ctx_t *decklink_ctx, struct klvanc_context_s *vanchdl, unsigned char *buf, unsigned int uiWidth, unsigned int lineNr)
{
	/* Convert the vanc line from V210 to CrCB422, then vanc parse it */

	/* We need two kinds of type pointers into the source vbi buffer */
	/* TODO: What the hell is this, two ptrs? */
	const uint32_t *src = (const uint32_t *)buf;

	/* Convert Blackmagic pixel format to nv20.
	 * src pointer gets mangled during conversion, hence we need its own
	 * ptr instead of passing vbiBufferPtr.
	 * decoded_words should be atleast 2 * uiWidth.
	 */
	uint16_t decoded_words[LIBKLVANC_PACKET_MAX_PAYLOAD];

	/* On output each pixel will be decomposed into three 16-bit words (one for Y, U, V) */
	assert(uiWidth * 6 < sizeof(decoded_words));

	memset(&decoded_words[0], 0, sizeof(decoded_words));
	uint16_t *p_anc = decoded_words;
	if (uiWidth == 720) {
		klvanc_v210_line_to_uyvy_c(src, p_anc, uiWidth);
	} else {
		if (klvanc_v210_line_to_nv20_c(src, p_anc, sizeof(decoded_words), (uiWidth / 6) * 6) < 0)
			return;
	}

    if (decklink_ctx->smpte2038_ctx)
        klvanc_smpte2038_packetizer_begin(decklink_ctx->smpte2038_ctx);

    if (decklink_ctx->vanchdl) {
        int ret = klvanc_packet_parse(vanchdl, lineNr, decoded_words, sizeof(decoded_words) / (sizeof(unsigned short)));
        if (ret < 0) {
      	  /* No VANC on this line */
        } else
        if (ret >= 1) {
        }
    }

    if (decklink_ctx->smpte2038_ctx) {
        if (klvanc_smpte2038_packetizer_end(decklink_ctx->smpte2038_ctx,
                                     decklink_ctx->stream_time / 300 + (10 * 90000)) == 0) {

            clock_gettime(CLOCK_REALTIME, &g_smpte2038_detected_ts);

            if (transmit_pes_to_muxer(decklink_ctx, decklink_ctx->smpte2038_ctx->buf,
                                      decklink_ctx->smpte2038_ctx->bufused, SMPTE2038) < 0) {
                fprintf(stderr, "%s() failed to xmit PES to muxer\n", __func__);
            }
        }
    }

}

static void setup_pixel_funcs( decklink_opts_t *decklink_opts )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts->decklink_ctx;

#if defined(__APPLE__)
#else
    int cpu_flags = av_get_cpu_flags();
#endif

    /* Setup VBI and VANC unpack functions */
    if( IS_SD( decklink_opts->video_format ) )
    {
#if defined(__APPLE__)
        printf("%s() No SD format support on APPLE, yet. Skipping.\n", __func__);
#else   
        decklink_ctx->unpack_line = obe_v210_line_to_uyvy_c;
        decklink_ctx->downscale_line = obe_downscale_line_c;
        decklink_ctx->blank_line = obe_blank_line_uyvy_c;

        if( cpu_flags & AV_CPU_FLAG_MMX )
            decklink_ctx->downscale_line = obe_downscale_line_mmx;

        if( cpu_flags & AV_CPU_FLAG_SSE2 )
            decklink_ctx->downscale_line = obe_downscale_line_sse2;
#endif
    }
    else
    {
        decklink_ctx->unpack_line = obe_v210_line_to_nv20_c;
        decklink_ctx->blank_line = obe_blank_line_nv20_c;
    }
}

static void get_format_opts( decklink_opts_t *decklink_opts, IDeckLinkDisplayMode *p_display_mode )
{
    decklink_opts->width = p_display_mode->GetWidth();
    decklink_opts->coded_height = p_display_mode->GetHeight();

    switch( p_display_mode->GetFieldDominance() )
    {
        case bmdProgressiveFrame:
            decklink_opts->interlaced = 0;
            decklink_opts->tff        = 0;
            break;
        case bmdProgressiveSegmentedFrame:
            /* Assume tff interlaced - this mode should not be used in broadcast */
            decklink_opts->interlaced = 1;
            decklink_opts->tff        = 1;
            break;
        case bmdUpperFieldFirst:
            decklink_opts->interlaced = 1;
            decklink_opts->tff        = 1;
            break;
        case bmdLowerFieldFirst:
            decklink_opts->interlaced = 1;
            decklink_opts->tff        = 0;
            break;
        case bmdUnknownFieldDominance:
        default:
            /* Assume progressive */
            decklink_opts->interlaced = 0;
            decklink_opts->tff        = 0;
            break;
    }

    /* Tested with a MRD4400 upstread, adjusting signal formats:
     * Resolution     Interlaced  TFF
     * 720x480i                1    0
     * 720x576i                1    1
     * 1280x720p50             0    0
     * 1280x720p59.94          0    0
     * 1280x720p60             0    0
     * 1920x1080i25            1    1
     * 1920x1080i29.97         1    1
     * 1920x1080i30            1    1
     * 1920x1080p30            0    0
     */

    decklink_opts->height = decklink_opts->coded_height;
    if( decklink_opts->coded_height == 486 )
        decklink_opts->height = 480;
}

static const struct obe_to_decklink_video *getVideoFormatByOBEName(int obe_name)
{
    const struct obe_to_decklink_video *fmt;

    for (int i = 0; video_format_tab[i].obe_name != -1; i++) {
        fmt = &video_format_tab[i];
        if (fmt->obe_name == obe_name) {
            return fmt; 
        }
    }

    return NULL;
}

static const struct obe_to_decklink_video *getVideoFormatByMode(BMDDisplayMode mode_id)
{
    const struct obe_to_decklink_video *fmt;

    for (int i = 0; video_format_tab[i].obe_name != -1; i++) {
        fmt = &video_format_tab[i];
        if (fmt->bmd_name == mode_id) {
            return fmt; 
        }
    }

    return NULL;
}

class DeckLinkCaptureDelegate : public IDeckLinkInputCallback
{
public:
    DeckLinkCaptureDelegate( decklink_opts_t *decklink_opts ) : decklink_opts_(decklink_opts)
    {
        pthread_mutex_init( &ref_mutex_, NULL );
        pthread_mutex_lock( &ref_mutex_ );
        ref_ = 1;
        pthread_mutex_unlock( &ref_mutex_ );
    }
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) { return E_NOINTERFACE; }

    virtual ULONG STDMETHODCALLTYPE AddRef(void)
    {
        uintptr_t new_ref;
        pthread_mutex_lock( &ref_mutex_ );
        new_ref = ++ref_;
        pthread_mutex_unlock( &ref_mutex_ );
        return new_ref;
    }

    virtual ULONG STDMETHODCALLTYPE Release(void)
    {
        uintptr_t new_ref;
        pthread_mutex_lock( &ref_mutex_ );
        new_ref = --ref_;
        pthread_mutex_unlock( &ref_mutex_ );
        if ( new_ref == 0 )
            delete this;
        return new_ref;
    }

    virtual HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents events, IDeckLinkDisplayMode *p_display_mode, BMDDetectedVideoInputFormatFlags)
    {
        {
            BMDDisplayMode mode_id = p_display_mode->GetDisplayMode();

            static BMDDisplayMode last_mode_id = 0xffffffff;
            if (last_mode_id != 0xffffffff && last_mode_id != mode_id) {
                /* Avoid a race condition where the probed resolution doesn't
                 * match the SDI resolution when the encoder actually starts.
                 * If you don't deal with that condition you end up feeding
                 * 480i video into a 720p configured codec. Why? The probe condition
                 * data is used to configured the codecs and downstream filters,
                 * the startup resolution is completely ignored.
                 * Cause the decklink module never to access a resolution during start
                 * that didn't match what we found during probe. 
                 */
                decklink_opts_->decklink_ctx.last_frame_time = 0;
            }
            last_mode_id = mode_id;

            const struct obe_to_decklink_video *fmt = getVideoFormatByMode(mode_id);
            if (!fmt) {
                syslog(LOG_WARNING, "(1)Unsupported video format %x", mode_id);
                fprintf(stderr, "(1)Unsupported video format %x\n", mode_id);
                return S_OK;
            }
            printf("%s() %x [ %s ]\n", __func__, mode_id, fmt->ascii_name);
            sprintf(g_signal_format_detected, fmt->ascii_name);
            clock_gettime(CLOCK_REALTIME, &g_signal_detected_ts);
            clock_gettime(CLOCK_REALTIME, &g_signal_los_ts);

            if (OPTION_ENABLED_(allow_1080p60) == 0) {
                switch (fmt->obe_name) {
                case INPUT_VIDEO_FORMAT_1080P_50:
                case INPUT_VIDEO_FORMAT_1080P_5994:
                case INPUT_VIDEO_FORMAT_1080P_60:
                    syslog(LOG_WARNING, "Detected Video format '%s' explicitly disabled in configuration", fmt->ascii_name);
                    fprintf(stderr, "Detected Video format '%s' explicitly disabled in configuration\n", fmt->ascii_name);
                    return S_OK;
                }
            }
        }

        decklink_ctx_t *decklink_ctx = &decklink_opts_->decklink_ctx;
        if (0 && decklink_opts_->probe == 0) {
            printf("%s() no format switching allowed outside of probe\n", __func__);
            syslog(LOG_WARNING, "%s() no format switching allowed outside of probe\n", __func__);
            decklink_ctx->last_frame_time = 1;
            exit(0);
        }

        int i = 0;
        if( events & bmdVideoInputDisplayModeChanged )
        {
            BMDDisplayMode mode_id = p_display_mode->GetDisplayMode();
            syslog( LOG_WARNING, "Video input format changed" );

            if( decklink_ctx->last_frame_time == -1 )
            {
                for( i = 0; video_format_tab[i].obe_name != -1; i++ )
                {
                    if( video_format_tab[i].bmd_name == mode_id )
                        break;
                }

                if( video_format_tab[i].obe_name == -1 )
                {
                    syslog( LOG_WARNING, "Unsupported video format" );
                    return S_OK;
                }

                decklink_opts_->video_format = video_format_tab[i].obe_name;
                decklink_opts_->timebase_num = video_format_tab[i].timebase_num;
                decklink_opts_->timebase_den = video_format_tab[i].timebase_den;
                calculate_audio_sfc_window(decklink_opts_);

		if (decklink_opts_->video_format == INPUT_VIDEO_FORMAT_1080P_2997)
		{
		   if (p_display_mode->GetFieldDominance() == bmdProgressiveSegmentedFrame)
		   {
		       /* HACK: The transport is structurally interlaced, so we need
			  to treat it as such in order for VANC processing to
			  work properly (even if the actual video really may be
			  progressive).  This also coincidentally works around a bug
			  in VLC where 1080i/59 content gets put out as 1080psf/29, and
			  that's a much more common use case in the broadcast world
			  than real 1080 progressive video at 30 FPS. */
		       fprintf(stderr, "Treating 1080psf/30 as interlaced\n");
		       decklink_opts_->video_format = INPUT_VIDEO_FORMAT_1080I_5994;
		   }
		}

                get_format_opts( decklink_opts_, p_display_mode );
                setup_pixel_funcs( decklink_opts_ );

                decklink_ctx->p_input->PauseStreams();

                /* We'll allow the input to be reconfigured, so that we can start to receive these
                 * these newly sized frames. The Frame arrival callback will detect that these new
                 * sizes don't match the older sizes and abort frame processing.
                 * If we don't change the mode below, we end up still receiving older resolutions and
                 * we're stuck in limbo, not knowing what to do during frame arrival.
                 */
                //printf("%s() calling enable video with mode %s\n", __func__, getModeName(mode_id));
                decklink_ctx->p_input->EnableVideoInput(mode_id, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection);
                decklink_ctx->p_input->FlushStreams();
                decklink_ctx->p_input->StartStreams();
            } else {
                syslog(LOG_ERR, "Decklink card index %i: Resolution changed from %08x to %08x, aborting.",
                    decklink_opts_->card_idx, decklink_ctx->enabled_mode_id, mode_id);
                printf("Decklink card index %i: Resolution changed from %08x to %08x, aborting.\n",
                    decklink_opts_->card_idx, decklink_ctx->enabled_mode_id, mode_id);
                //exit(0); /* Take an intensional hard exit */
            }
        }
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);
    HRESULT STDMETHODCALLTYPE noVideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);
    HRESULT STDMETHODCALLTYPE timedVideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*);

private:
    pthread_mutex_t ref_mutex_;
    uintptr_t ref_;
    decklink_opts_t *decklink_opts_;
};

static void _vanc_cache_dump(decklink_ctx_t *ctx)
{
    if (ctx->vanchdl == NULL)
        return;

    for (int d = 0; d <= 0xff; d++) {
        for (int s = 0; s <= 0xff; s++) {
            struct klvanc_cache_s *e = klvanc_cache_lookup(ctx->vanchdl, d, s);
            if (!e)
                continue;

            if (e->activeCount == 0)
                continue;

            for (int l = 0; l < 2048; l++) {
                if (e->lines[l].active) {
                    kllog("VANC", "->did/sdid = %02x / %02x: %s [%s] via SDI line %d (%" PRIu64 " packets)\n",
                        e->did, e->sdid, e->desc, e->spec, l, e->lines[l].count);
                }
            }
        }
    }
}

#if KL_PRBS_INPUT
static void dumpAudio(uint16_t *ptr, int fc, int num_channels)
{
        fc = 4;
        uint32_t *p = (uint32_t *)ptr;
        for (int i = 0; i < fc; i++) {
                printf("%d.", i);
                for (int j = 0; j < num_channels; j++)
                        printf("%08x ", *p++);
                printf("\n");
        }
}
static int prbs_inited = 0;
#endif

static int64_t queryAudioClock(IDeckLinkAudioInputPacket *audioframe)
{
    BMDTimeValue time;
#if 0
    audioframe->GetPacketTime(&time, OBE_CLOCK);
#else
    /* Avoid an issues in the BM SDK where the result
     * of the call, time, goes negative after 82.37 days.
     * if the clock is 27 * 1e6.
     */
    audioframe->GetPacketTime(&time, OBE_CLOCK / 100LL);
    time *= 100LL;

#if 0
    /* Accelerate the issue, so the clocks fail/wrap in just over a minute. */
    audioframe->GetPacketTime(&time, OBE_CLOCK * 100000LL);
    time /= 100000LL;
#endif
#endif
    return time;
}

static int processAudio(decklink_ctx_t *decklink_ctx, decklink_opts_t *decklink_opts_, IDeckLinkAudioInputPacket *audioframe, int64_t videoPTS)
{
    obe_raw_frame_t *raw_frame = NULL;
    void *frame_bytes;
    audioframe->GetBytes(&frame_bytes);
    int hasSentAudioBuffer = 0;

    uint32_t strideBytes = decklink_opts_->num_channels * (32 / 8);
    if (ltnsdi_audio_channels_write(decklink_ctx->sdi_audio_analyzer_ctx,
        (uint8_t *)frame_bytes,
        audioframe->GetSampleFrameCount(),
        32,
        decklink_opts_->num_channels,
        strideBytes) < 0)
    {
        fprintf(stderr, "Failed to feed audio analyzer\n");
    }

        for (int i = 0; i < MAX_AUDIO_PAIRS; i++) {
            struct audio_pair_s *pair = &decklink_ctx->audio_pairs[i];

            if (!pair->smpte337_detected_ac3 && hasSentAudioBuffer == 0) {
                /* PCM audio, forward to compressors */
                raw_frame = new_raw_frame();
                if (!raw_frame) {
                    syslog(LOG_ERR, "Malloc failed\n");
                    goto end;
                }
                raw_frame->audio_frame.num_samples = audioframe->GetSampleFrameCount();
                raw_frame->audio_frame.num_channels = decklink_opts_->num_channels;
                raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;
#if KL_PRBS_INPUT
/* ST: This code is optionally compiled in, and hasn't been validated since we refactored a little. */
            {
            uint32_t *p = (uint32_t *)frame_bytes;
            //dumpAudio((uint16_t *)p, audioframe->GetSampleFrameCount(), raw_frame->audio_frame.num_channels);

            if (prbs_inited == 0) {
                for (int i = 0; i < audioframe->GetSampleFrameCount(); i++) {
                    for (int j = 0; j < raw_frame->audio_frame.num_channels; j++) {
                        if (i == (audioframe->GetSampleFrameCount() - 1)) {
                            if (j == (raw_frame->audio_frame.num_channels - 1)) {
                                printf("Seeding audio PRBS sequence with upstream value 0x%08x\n", *p >> 16);
                                prbs15_init_with_seed(&decklink_ctx->prbs, *p >> 16);
                            }
                        }
			p++;
                    }
                }
                prbs_inited = 1;
            } else {
                for (int i = 0; i < audioframe->GetSampleFrameCount(); i++) {
                    for (int j = 0; j < raw_frame->audio_frame.num_channels; j++) {
                        uint32_t a = *p++ >> 16;
                        uint32_t b = prbs15_generate(&decklink_ctx->prbs);
                        if (a != b) {
                            char t[160];
                            sprintf(t, "%s", ctime(&now));
                            t[strlen(t) - 1] = 0;
                            fprintf(stderr, "%s: KL PRSB15 Audio frame discontinuity, expected %08" PRIx32 " got %08" PRIx32 "\n", t, b, a);
                            prbs_inited = 0;

                            // Break the sample frame loop i
                            i = audioframe->GetSampleFrameCount();
                            break;
                        }
                    }
                }
            }

            }
#endif

                /* Allocate a samples buffer for num_samples samples, and fill data pointers and linesize accordingly. */
                if( av_samples_alloc( raw_frame->audio_frame.audio_data, &raw_frame->audio_frame.linesize, decklink_opts_->num_channels,
                              raw_frame->audio_frame.num_samples, (AVSampleFormat)raw_frame->audio_frame.sample_fmt, 0 ) < 0 )
                {
                    syslog( LOG_ERR, "Malloc failed\n" );
                    return -1;
                }

                /* Convert input samples from S32 interleaved into S32P planer. */
                if (swr_convert(decklink_ctx->avr,
                        raw_frame->audio_frame.audio_data,
                        raw_frame->audio_frame.num_samples,
                        (const uint8_t**)&frame_bytes,
                        raw_frame->audio_frame.num_samples) < 0)
                {
                    syslog(LOG_ERR, PREFIX "Sample format conversion failed\n");
                    return -1;
                }

                raw_frame->pts = queryAudioClock(audioframe);

                avfm_init(&raw_frame->avfm, AVFM_AUDIO_PCM);
                avfm_set_hw_status_mask(&raw_frame->avfm,
                    decklink_ctx->isHalfDuplex ? AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_HALF :
                        AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_FULL);
                avfm_set_pts_video(&raw_frame->avfm, videoPTS + clock_offset);
                avfm_set_pts_audio(&raw_frame->avfm, raw_frame->pts + clock_offset);
                avfm_set_hw_received_time(&raw_frame->avfm);
                avfm_set_video_interval_clk(&raw_frame->avfm, decklink_ctx->vframe_duration);
                //raw_frame->avfm.hw_audio_correction_clk = clock_offset;

                raw_frame->release_data = obe_release_audio_data;
                raw_frame->release_frame = obe_release_frame;
                raw_frame->input_stream_id = pair->input_stream_id;
                if (add_to_filter_queue(decklink_ctx->h, raw_frame) < 0)
                    goto fail;
                hasSentAudioBuffer++;

            } /* !pair->smpte337_detected_ac3 */

            if (pair->smpte337_detected_ac3) {

                /* Ship the buffer + offset into it, down to the encoders. The encoders will look at offset 0. */
		/* In summary, we prepare a new and unique buffer for each detected audio pair, unlike PCM - 
		 * which gets a single buffer containing all channels (and the audio filter splits them out).
		 */
                int depth = 32;
                int span = 2;
                int offset = i * ((depth / 8) * span);
                raw_frame = new_raw_frame();
                raw_frame->audio_frame.num_samples = audioframe->GetSampleFrameCount();
                raw_frame->audio_frame.num_channels = decklink_opts_->num_channels;
                raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P; /* No specific format. The audio filter will play passthrough. */

                int l = audioframe->GetSampleFrameCount() * decklink_opts_->num_channels * (depth / 8);
                raw_frame->audio_frame.audio_data[0] = (uint8_t *)malloc(l);
                raw_frame->audio_frame.linesize = raw_frame->audio_frame.num_channels * (depth / 8);

		/* Move the audio to allign the bitstream audio in this specific pair into pair0 then send the buffer downstream. */
                memcpy(raw_frame->audio_frame.audio_data[0], (uint8_t *)frame_bytes + offset, l - offset);

                raw_frame->audio_frame.sample_fmt = AV_SAMPLE_FMT_NONE;

                raw_frame->pts = queryAudioClock(audioframe);

                avfm_init(&raw_frame->avfm, AVFM_AUDIO_A52);
                avfm_set_hw_status_mask(&raw_frame->avfm,
                    decklink_ctx->isHalfDuplex ? AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_HALF :
                        AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_FULL);
                avfm_set_pts_video(&raw_frame->avfm, videoPTS + clock_offset);
                avfm_set_pts_audio(&raw_frame->avfm, raw_frame->pts + clock_offset);
                avfm_set_hw_received_time(&raw_frame->avfm);
                avfm_set_video_interval_clk(&raw_frame->avfm, decklink_ctx->vframe_duration);
                //raw_frame->avfm.hw_audio_correction_clk = clock_offset;
                //avfm_dump(&raw_frame->avfm);

                raw_frame->release_data = obe_release_audio_data;
                raw_frame->release_frame = obe_release_frame;
                raw_frame->input_stream_id = pair->input_stream_id;
                //printf("frame for pair->nr %d rf->input_stream_id %d at offset %d\n", pair->nr, raw_frame->input_stream_id, offset);

                add_to_filter_queue(decklink_ctx->h, raw_frame);
            } /* pair->smpte337_detected_ac3 */
        } /* For all audio pairs... */
end:

    return S_OK;

fail:

    if( raw_frame )
    {
        if (raw_frame->release_data)
            raw_frame->release_data( raw_frame );
        if (raw_frame->release_frame)
            raw_frame->release_frame( raw_frame );
    }

    return S_OK;
}

#if DO_SET_VARIABLE
#if 0
static int wipeAudio(IDeckLinkAudioInputPacket *audioframe)
{
	uint8_t *buf;
	audioframe->GetBytes((void **)&buf);
	int hasData[16];
	memset(&hasData[0], 0, sizeof(hasData));

	int sfc = audioframe->GetSampleFrameCount();
	int channels = 16;

	uint32_t *p = (uint32_t *)buf;
	for (int i = 0; i < sfc; i++) {
		for (int j = 0; j < channels; j++) {

			if (*p != 0) {
				hasData[j] = 1;
			}
			*p = 0;
			p++;
		}
	}
	int cnt = 0;
	printf("Channels with data: ");
	for (int i = 0; i < channels; i++) {
		if (hasData[i]) {
			cnt++;
			printf("%d ", i);
		}
	}
	if (cnt == 0)
		printf("none");
	printf("\n");

	return cnt;
}
#endif

#if 0
static int countAudioChannelsWithPayload(IDeckLinkAudioInputPacket *audioframe)
{
	uint8_t *buf;
	audioframe->GetBytes((void **)&buf);
	int hasData[16];
	memset(&hasData[0], 0, sizeof(hasData));

	int sfc = audioframe->GetSampleFrameCount();
	int channels = 16;

	uint32_t *p = (uint32_t *)buf;
	for (int i = 0; i < sfc; i++) {
		for (int j = 0; j < channels; j++) {

			if (*p != 0) {
				hasData[j] = 1;
			}
			p++;
		}
	}
	int cnt = 0;
	printf("Channels with data: ");
	for (int i = 0; i < channels; i++) {
		if (hasData[i]) {
			cnt++;
			printf("%d ", i);
		}
	}
	if (cnt == 0)
		printf("none"); printf("\n");

	return cnt;
}
#endif
#endif

/* If enable, we drop every other audio payload from the input. */
int           g_decklink_fake_every_other_frame_lose_audio_payload = 0;
static int    g_decklink_fake_every_other_frame_lose_audio_payload_count = 0;
time_t        g_decklink_fake_every_other_frame_lose_audio_payload_time = 0;
static double g_decklink_fake_every_other_frame_lose_audio_count = 0;
static double g_decklink_fake_every_other_frame_lose_video_count = 0;

int           g_decklink_histogram_reset = 0;
int           g_decklink_histogram_print_secs = 0;

int           g_decklink_fake_lost_payload = 0;
time_t        g_decklink_fake_lost_payload_time = 0;
static int    g_decklink_fake_lost_payload_interval = 60;
static int    g_decklink_fake_lost_payload_state = 0;
int           g_decklink_burnwriter_enable = 0;
uint32_t      g_decklink_burnwriter_count = 0;
uint32_t      g_decklink_burnwriter_linenr = 0;

int           g_decklink_monitor_hw_clocks = 0;

int           g_decklink_injected_frame_count = 0;
int           g_decklink_injected_frame_count_max = 600;
int           g_decklink_inject_frame_enable = 0;

int           g_decklink_missing_audio_count = 0;
time_t        g_decklink_missing_audio_last_time = 0;
int           g_decklink_missing_video_count = 0;
time_t        g_decklink_missing_video_last_time = 0;

int           g_decklink_record_audio_buffers = 0;

int           g_decklink_render_walltime = 0;
int           g_decklink_render_timecode = 0;
int           g_decklink_inject_scte104_preroll6000 = 0;
int           g_decklink_inject_scte104_fragmented = 0;

int           g_decklink_op47_teletext_reverse = 1;
int           g_timecode_debug = 0;

struct udp_vanc_receiver_s {
    int active;
    int skt;
    struct sockaddr_in sin;
    unsigned char *buf;
    int bufmaxlen;
} g_decklink_udp_vanc_receiver;
int g_decklink_udp_vanc_receiver_port; /* UDP Port number to activate a VANC receiver on */

static obe_raw_frame_t *cached = NULL;
static void cache_video_frame(obe_raw_frame_t *frame)
{
    if (cached != NULL) {
        cached->release_data(cached);
        cached->release_frame(cached);
    }

    cached = obe_raw_frame_copy(frame);
}

HRESULT DeckLinkCaptureDelegate::noVideoInputFrameArrived(IDeckLinkVideoInputFrame *videoframe, IDeckLinkAudioInputPacket *audioframe)
{
    clock_gettime(CLOCK_REALTIME, &g_signal_los_ts);
//struct timespec g_ce708_detected_ts;
//struct timespec g_smpe2038_detected_ts;

	if (!cached)
		return S_OK;

	g_decklink_injected_frame_count++;
	if (g_decklink_injected_frame_count > g_decklink_injected_frame_count_max) {
            char msg[128];
            sprintf(msg, "Decklink card index %i: More than %d frames were injected, aborting.\n",
                decklink_opts_->card_idx,
                g_decklink_injected_frame_count_max);
            syslog(LOG_ERR, "%s", msg);
            fprintf(stderr, "%s", msg);
            exit(1);
        }

	decklink_ctx_t *decklink_ctx = &decklink_opts_->decklink_ctx;
 	BMDTimeValue frame_duration;
	obe_t *h = decklink_ctx->h;

	/* use SDI ticks as clock source */
	videoframe->GetStreamTime(&decklink_ctx->stream_time, &frame_duration, OBE_CLOCK);
	obe_clock_tick(h, (int64_t)decklink_ctx->stream_time);

	obe_raw_frame_t *raw_frame = obe_raw_frame_copy(cached);
	raw_frame->pts = decklink_ctx->stream_time;

	avfm_set_pts_video(&raw_frame->avfm, decklink_ctx->stream_time + clock_offset);

	/* Normally we put the audio and the video clocks into the timing
	 * avfm metadata, and downstream codecs can calculate their timing
	 * from whichever clock they prefer. Generally speaking, that's the
	 * audio clock - for both the video and the audio pipelines.
	 * As long as everyone slaves of a single clock, no problem.
	 * However, interesting observation. In the event of signal loss,
	 * the BlackMagic SDK 10.8.5a continues to report proper video timing
	 * intervals, but the audio clock goes wild and runs faster than
	 * anticipated. The side effect of this, is that video frames (slaved
	 * originally from the audio clock) get into the libmpegts mux and start
	 * experiencing data loss. The ES going into libmpegts is fine, the
	 * PES construction is fine, the output TS packets are properly aligned
	 * and CC stamped, but data is lost.
	 * The remedy, in LOS conditions, use the video clock as the audio clock
	 * when building timing metadata.
	 */
	avfm_set_pts_audio(&raw_frame->avfm, decklink_ctx->stream_time + clock_offset);

	avfm_set_hw_received_time(&raw_frame->avfm);
#if 0
	//avfm_dump(&raw_frame->avfm);
	printf("Injecting cached frame %d for time %" PRIi64 "\n", g_decklink_injected_frame_count, raw_frame->pts);
#endif
	add_to_filter_queue(h, raw_frame);

	return S_OK;
}

HRESULT DeckLinkCaptureDelegate::VideoInputFrameArrived( IDeckLinkVideoInputFrame *videoframe, IDeckLinkAudioInputPacket *audioframe )
{
	decklink_ctx_t *decklink_ctx = &decklink_opts_->decklink_ctx;

	if (g_decklink_histogram_reset) {
		g_decklink_histogram_reset = 0;
		ltn_histogram_reset(decklink_ctx->callback_hdl);
		ltn_histogram_reset(decklink_ctx->callback_duration_hdl);
	}

	ltn_histogram_interval_update(decklink_ctx->callback_hdl);

	avmetadata_reset(&decklink_ctx->metadataVANC);

	ltn_histogram_sample_begin(decklink_ctx->callback_duration_hdl);
	HRESULT hr = timedVideoInputFrameArrived(videoframe, audioframe);
	ltn_histogram_sample_end(decklink_ctx->callback_duration_hdl);


	uint32_t val[2];
	decklink_ctx->p_input->GetAvailableVideoFrameCount(&val[0]);
	decklink_ctx->p_input->GetAvailableAudioSampleFrameCount(&val[1]);

	if (g_decklink_histogram_print_secs > 0) {
		ltn_histogram_interval_print(STDOUT_FILENO, decklink_ctx->callback_hdl, g_decklink_histogram_print_secs);
		ltn_histogram_interval_print(STDOUT_FILENO, decklink_ctx->callback_duration_hdl, g_decklink_histogram_print_secs);
#if 0
		ltn_histogram_interval_print(STDOUT_FILENO, decklink_ctx->callback_1_hdl, 10);
		ltn_histogram_interval_print(STDOUT_FILENO, decklink_ctx->callback_2_hdl, 10);
		ltn_histogram_interval_print(STDOUT_FILENO, decklink_ctx->callback_3_hdl, 10);
		ltn_histogram_interval_print(STDOUT_FILENO, decklink_ctx->callback_4_hdl, 10);
#endif
	}
	//printf("%d.%08d rem vf:%d af:%d\n", diff.tv_sec, diff.tv_usec, val[0], val[1]);

extern double g_audio_channel_level_dbfs[32];
extern int g_audio_channel_type[32]; /* 0 = undefined, 1 = pcm, 2 = bitstream */

    /* Update the stats structs so the UI can be up to date */
    struct ltnsdi_status_s *astatus;
    if (ltnsdi_status_alloc(decklink_ctx->sdi_audio_analyzer_ctx, &astatus) == 0) {
        for (int i = 0; i < decklink_opts_->num_channels; i++) {

            if (astatus->channels[i].type == 1) {
                g_audio_channel_type[i] = 1; /* PCM */
                g_audio_channel_level_dbfs[i] = astatus->channels[i].pcm_dbFS;
            } else
            if (astatus->channels[i].type == 2) {
                g_audio_channel_type[i] = 2; /* bitstream */
                g_audio_channel_level_dbfs[i] = 0.0;
            }
        }
        ltnsdi_status_free(decklink_ctx->sdi_audio_analyzer_ctx, astatus);
        astatus = NULL;
    }

	return hr;
}

static void extractTimecode(decklink_ctx_t *decklink_ctx, IDeckLinkVideoInputFrame *videoframe)
{
        decklink_ctx_t *ctx = decklink_ctx;

        /* Timecode handling */
        IDeckLinkTimecode *timecode;
        BMDTimecodeFormat tcf = bmdTimecodeRP188Any;
#if 0
        BMDTimecodeFormat tcf = bmdTimecodeRP188VITC1;
        BMDTimecodeFormat tcf = bmdTimecodeRP188VITC2;
        BMDTimecodeFormat tcf = bmdTimecodeRP188LTC;
        BMDTimecodeFormat tcf = bmdTimecodeRP188Any;
        BMDTimecodeFormat tcf = bmdTimecodeVITC;
        BMDTimecodeFormat tcf = bmdTimecodeVITCField2;
        BMDTimecodeFormat tcf = bmdTimecodeSerial;
#endif

        memset(&ctx->timecode, 0, sizeof(ctx->timecode));

        if (videoframe->GetTimecode(tcf, &timecode) == S_OK) {
            ctx->timecode.present = 1;
            if (timecode->GetComponents(&ctx->timecode.hours, &ctx->timecode.minutes, &ctx->timecode.seconds, &ctx->timecode.frames) == S_OK) {

                /* Comments: https://www.reddit.com/r/VIDEOENGINEERING/comments/1b946x0/timecode_for_5994fps/
                    * timecode when frame counts exceed 30fps, Mark bit with repeated time.
                    */
                if (timecode->GetFlags() & bmdTimecodeIsDropFrame) {
                    ctx->timecode.drop_frame = 1;
                } else {
                    ctx->timecode.drop_frame = 0;
                }

                if (g_timecode_debug) {
                    printf("sdi timecode: %02d:%02d:%02d%s%02d\n",
                        ctx->timecode.hours, ctx->timecode.minutes, ctx->timecode.seconds,
                        ctx->timecode.drop_frame ? ";" : ":",
                        ctx->timecode.frames);
                }
            }
            timecode->Release();

        } else {
            //fprintf(stderr, PREFIX "No timecode available\n");
        }
        /* End - Timecode handling */

}

HRESULT DeckLinkCaptureDelegate::timedVideoInputFrameArrived( IDeckLinkVideoInputFrame *videoframe, IDeckLinkAudioInputPacket *audioframe )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts_->decklink_ctx;
    obe_raw_frame_t *raw_frame = NULL;
    AVPacket pkt;
    AVFrame *frame = NULL;
    void *frame_bytes, *anc_line;
    obe_t *h = decklink_ctx->h;
    int ret, num_anc_lines = 0, anc_line_stride,
    lines_read = 0, first_line = 0, last_line = 0, line, num_vbi_lines, vii_line;
    uint32_t *frame_ptr;
    uint16_t *anc_buf, *anc_buf_pos;
    uint8_t *vbi_buf;
    int anc_lines[DECKLINK_VANC_LINES];
    IDeckLinkVideoFrameAncillary *ancillary;
    BMDTimeValue frame_duration;
    time_t now = time(0);
    int width = 0;
    int height = 0;
    int stride = 0;
    int sfc = 0;
    BMDTimeValue packet_time = 0;

    ltn_histogram_sample_begin(decklink_ctx->callback_1_hdl);
#if AUDIO_PULSE_OFFSET_MEASURMEASURE
    {
        /* For any given video frame demonstrating the one second blip pulse pattern, search
         * The audio samples for the blip and report its row position (time offset since luma).
         */
        if (videoframe && audioframe) {
            width = videoframe->GetWidth();
            height = videoframe->GetHeight();
            stride = videoframe->GetRowBytes();
            uint8_t *v = NULL;
            videoframe->GetBytes((void **)&v);
            uint8_t *a = NULL;
            audioframe->GetBytes((void **)&a);

            int sfc = audioframe->GetSampleFrameCount();
            int num_channels = decklink_opts_->num_channels;

            if (*(v + 1) == 0xa2) {
                printf("v %p a %p ... v: %02x %02x %02x %02x  a: %02x %02x %02x %02x\n",
                    v, a, 
                    *(v + 0),
                    *(v + 1),
                    *(v + 2),
                    *(v + 3),
                    *(a + 0),
                    *(a + 1),
                    *(a + 2),
                    *(a + 3));
            }

            int disp = 0;
            for (int i = 0; i < sfc; i++) {
                uint8_t *row = a + (i * ((32 / 8) * num_channels));
                if (*(row + 2) && *(row + 3) && disp < 3) {
                    disp++;
                    printf("a%5d: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        i,
                        *(row + 0),
                        *(row + 1),
                        *(row + 2),
                        *(row + 3),
                        *(row + 4),
                        *(row + 5),
                        *(row + 6),
                        *(row + 7));
                }
            }

        }
    }
#endif

    if (videoframe) {
        width = videoframe->GetWidth();
        height = videoframe->GetHeight();
        stride = videoframe->GetRowBytes();

        extractTimecode(decklink_ctx, videoframe);

#if LTN_WS_ENABLE
	ltn_ws_set_property_signal(g_ltn_ws_handle, width, height, 1 /* progressive */, 5994);
#endif

    } else {
        g_decklink_missing_video_count++;
        time(&g_decklink_missing_video_last_time);
    }

    if (audioframe) {
        sfc = audioframe->GetSampleFrameCount();
        packet_time = queryAudioClock(audioframe);

        if (g_decklink_record_audio_buffers) {
            g_decklink_record_audio_buffers--;
            static int aidx = 0;
            char fn[256];
            int len = sfc * decklink_opts_->num_channels * (32 / 8);
#if AUDIO_DEBUG_ENABLE
            sprintf(fn, "/storage/ltn/stoth/cardindex%d-audio%03d-srf%d.raw", decklink_opts_->card_idx, aidx++, sfc);
#else
            sprintf(fn, "/tmp/cardindex%d-audio%03d-srf%d.raw", decklink_opts_->card_idx, aidx++, sfc);
#endif
            FILE *fh = fopen(fn, "wb");
            if (fh) {
                void *p;
                audioframe->GetBytes(&p);
                fwrite(p, 1, len, fh);
                fclose(fh);
                printf("Creating %s\n", fn);
            }
        }

    } else {
        g_decklink_missing_audio_count++;
        time(&g_decklink_missing_audio_last_time);
    }

    if (0 && decklink_opts_->probe == 0 && decklink_ctx->enabled_mode_fmt) {
        const struct obe_to_decklink_video *fmt = decklink_ctx->enabled_mode_fmt;

        if (width != fmt->callback_width) {
//            printf(" !width %d\n", width);
            return S_OK;
        }
        if (height != fmt->callback_height) {
//            printf(" !height %d\n", height);
            return S_OK;
        }
        if (stride != fmt->callback_stride) {
//            printf(" !stride %d\n", stride);
            return S_OK;
        }
    }

    BMDTimeValue vtime = 0;
    if (videoframe) {
       videoframe->GetStreamTime(&vtime, &decklink_ctx->vframe_duration, OBE_CLOCK);
    }

    if (g_decklink_monitor_hw_clocks)
    {
        static BMDTimeValue last_vtime = 0;
        static BMDTimeValue last_atime = 0;

        BMDTimeValue atime = packet_time;

        if (vtime == 0)
            last_vtime = 0;
        if (atime == 0)
            last_atime = 0;

        static struct timeval lastts;
        struct timeval ts;
        struct timeval diff;
        gettimeofday(&ts, NULL);
        obe_timeval_subtract(&diff, &ts, &lastts);
        lastts = ts;
#if defined(__linux__)
        printf("%lu.%08lu -- vtime %012" PRIi64 ":%08" PRIi64 "  atime %012" PRIi64 ":%08" PRIi64 "  vduration %" PRIi64 " a-vdiff: %" PRIi64,
#else
        printf("%lu.%08u -- vtime %012" PRIi64 ":%08" PRIi64 "  atime %012" PRIi64 ":%08" PRIi64 "  vduration %" PRIi64 " a-vdiff: %" PRIi64,
#endif
            diff.tv_sec,
            diff.tv_usec,
            vtime,
            vtime - last_vtime,
            atime,
            atime - last_atime,
            decklink_ctx->vframe_duration,
            atime - vtime);

        BMDTimeValue adiff = atime - last_atime;

        if (last_vtime && (vtime - last_vtime != 450450)) {
            if (last_vtime && (vtime - last_vtime != 900900)) {
                printf(" Bad video interval\n");
            } else {
                printf("\n");
            }
        } else
        if (last_atime && (adiff != 450000) && (adiff != 450562) && (adiff != 450563)) {
            printf(" Bad audio interval\n");
        } else {
            printf("\n");
        }
        last_vtime = vtime;
        last_atime = atime;
    } /* if g_decklink_monitor_hw_clocks */
    ltn_histogram_sample_end(decklink_ctx->callback_1_hdl);

    if (g_decklink_inject_frame_enable) {
        if (videoframe && videoframe->GetFlags() & bmdFrameHasNoInputSource) {
            return noVideoInputFrameArrived(videoframe, audioframe);
        }
    }

#if DO_SET_VARIABLE
    if (g_decklink_fake_every_other_frame_lose_audio_payload) {
        /* Loose the audio for every other video frame. */
        if (g_decklink_fake_every_other_frame_lose_audio_payload_count++ & 1) {
            audioframe = NULL;
        }
    }
#endif

    if (audioframe) {
       g_decklink_fake_every_other_frame_lose_audio_count++;
    }
    if (videoframe) {
       g_decklink_fake_every_other_frame_lose_video_count++;
    }

    /* Reset the audio monitoring timer to a future time measured in seconds. */
    if (g_decklink_fake_every_other_frame_lose_audio_payload_time == 0) {
        g_decklink_fake_every_other_frame_lose_audio_payload_time = now + 30;
    } else
    if (now >= g_decklink_fake_every_other_frame_lose_audio_payload_time) {
        /* Check payload counts when the timer expired, hard exit if we're detecting significant audio loss from the h/w. */

        if (g_decklink_fake_every_other_frame_lose_audio_count && g_decklink_fake_every_other_frame_lose_video_count) {
            
            double diff = abs(g_decklink_fake_every_other_frame_lose_audio_count - g_decklink_fake_every_other_frame_lose_video_count);

            char t[160];
            sprintf(t, "%s", ctime(&now));
            t[strlen(t) - 1] = 0;
            //printf("%s -- decklink a/v ratio loss is %f\n", t, diff);
            /* If loss of a/v frames vs full frames (with a+v) falls below 75%, exit. */
            /* Based on observed condition, the loss quickly reaches 50%, hence 75% is very safe. */
            if (diff > 0 && g_decklink_fake_every_other_frame_lose_audio_count / g_decklink_fake_every_other_frame_lose_video_count < 0.75) {
                char msg[128];
                sprintf(msg, "Decklink card index %i: video (%f) to audio (%f) frames ratio too low, aborting.\n",
                    decklink_opts_->card_idx,
                    g_decklink_fake_every_other_frame_lose_video_count,
                    g_decklink_fake_every_other_frame_lose_audio_count);
                syslog(LOG_ERR, "%s", msg);
                fprintf(stderr, "%s", msg);
                exit(1);
            }
        }

        g_decklink_fake_every_other_frame_lose_audio_count = 0;
        g_decklink_fake_every_other_frame_lose_video_count = 0;
        g_decklink_fake_every_other_frame_lose_audio_payload_time = 0;
    }

#if DO_SET_VARIABLE
    if (g_decklink_fake_lost_payload)
    {
        if (g_decklink_fake_lost_payload_time == 0) {
            g_decklink_fake_lost_payload_time = now;
            g_decklink_fake_lost_payload_state = 0;
        } else
        if (now >= g_decklink_fake_lost_payload_time) {
            g_decklink_fake_lost_payload_time = now + g_decklink_fake_lost_payload_interval;
            //g_decklink_fake_lost_payload_state = 1; /* After this frame, simulate an audio loss too. */
            g_decklink_fake_lost_payload_state = 0; /* Don't drop audio in next frame, resume. */

            char t[160];
            sprintf(t, "%s", ctime(&now));
            t[strlen(t) - 1] = 0;
            printf("%s -- Simulating video loss\n", t);
            if (g_decklink_inject_frame_enable)
                return noVideoInputFrameArrived(videoframe, audioframe);
            else
                videoframe = NULL;
        } else
        if (g_decklink_fake_lost_payload_state == 1) {
            audioframe = NULL;
            g_decklink_fake_lost_payload_state = 0; /* No loss occurs */
            char t[160];
            sprintf(t, "%s", ctime(&now));
            t[strlen(t) - 1] = 0;
            printf("%s -- Simulating audio loss\n", t);
        }

    }
#endif

#if 0
    if ((audioframe == NULL) || (videoframe == NULL)) {
        klsyslog_and_stdout(LOG_ERR, "Decklink card index %i: missing audio (%p) or video (%p) (WARNING)",
            decklink_opts_->card_idx,
            audioframe, videoframe);
    }
    if (videoframe) {
        videoframe->GetBytes(&frame_bytes);
        V210_write_32bit_value(frame_bytes, stride, g_decklink_missing_video_count, 32 /* g_decklink_burnwriter_linenr */, 1);
        V210_write_32bit_value(frame_bytes, stride, g_decklink_missing_audio_count, 64 /* g_decklink_burnwriter_linenr */, 1);
    }
#endif

    if (g_decklink_render_walltime && videoframe) {
        /* Demonstrate V210 writes into the video packet format. */
        /* Tested with 720p and 1080i */
        videoframe->GetBytes(&frame_bytes);
        struct V210_painter_s painter;
        V210_painter_reset(&painter, (unsigned char *)frame_bytes, width, height, stride, 0);

        char ts[64];
        obe_getTimestamp(ts, NULL);
        V210_painter_draw_ascii_at(&painter, 0, 2, ts);
    }
    if (g_decklink_render_timecode && videoframe) {
        /* Demonstrate V210 writes into the video packet format. */
        /* Tested with 720p and 1080i */
        videoframe->GetBytes(&frame_bytes);
        struct V210_painter_s painter;
        V210_painter_reset(&painter, (unsigned char *)frame_bytes, width, height, stride, 0);

        char tc[64];
        sprintf(tc, "sdi timecode: %02d:%02d:%02d.%02d",
            decklink_ctx->timecode.hours,
            decklink_ctx->timecode.minutes,
            decklink_ctx->timecode.seconds,
            decklink_ctx->timecode.frames);
        V210_painter_draw_ascii_at(&painter, 0, 3, tc);
    }
    if (sfc && (sfc < decklink_opts_->audio_sfc_min || sfc > decklink_opts_->audio_sfc_max)) {
        if (videoframe && (videoframe->GetFlags() & bmdFrameHasNoInputSource) == 0) {
            klsyslog_and_stdout(LOG_ERR, "Decklink card index %i: illegal audio sample count %d, wanted %d to %d, "
                "dropping frames to maintain MP2 sync\n",
                decklink_opts_->card_idx, sfc,
                decklink_opts_->audio_sfc_min, decklink_opts_->audio_sfc_max);
        }
#if 1
        /* It's hard to reproduce this. For the time being I'm going to assume that we WANT any audio payload
         * to reach the audio codecs, regardless of how badly we think it's formed.
         */
#else
        return S_OK;
#endif
    }

    if( decklink_opts_->probe_success )
        return S_OK;

    if (OPTION_ENABLED_(vanc_cache)) {
        if (decklink_ctx->last_vanc_cache_dump + VANC_CACHE_DUMP_INTERVAL <= time(0)) {
            decklink_ctx->last_vanc_cache_dump = time(0);
            _vanc_cache_dump(decklink_ctx);
        }
    }

    av_init_packet( &pkt );

    if( videoframe )
    {
        ltn_histogram_sample_begin(decklink_ctx->callback_2_hdl);
        if( videoframe->GetFlags() & bmdFrameHasNoInputSource )
        {
            syslog( LOG_ERR, "Decklink card index %i: No input signal detected", decklink_opts_->card_idx );
            return S_OK;
        }
        else if (decklink_opts_->probe && decklink_ctx->audio_pairs[0].smpte337_frames_written > 6)
            decklink_opts_->probe_success = 1;

        if (g_decklink_injected_frame_count > 0) {
            klsyslog_and_stdout(LOG_INFO, "Decklink card index %i: Injected %d cached video frame(s)",
                decklink_opts_->card_idx, g_decklink_injected_frame_count);
            g_decklink_injected_frame_count = 0;
        }

        /* use SDI ticks as clock source */
        videoframe->GetStreamTime(&decklink_ctx->stream_time, &frame_duration, OBE_CLOCK);
        obe_clock_tick(h, (int64_t)decklink_ctx->stream_time);

        if( decklink_ctx->last_frame_time == -1 )
            decklink_ctx->last_frame_time = obe_mdate();
        else
        {
            int64_t cur_frame_time = obe_mdate();
            if( cur_frame_time - decklink_ctx->last_frame_time >= g_sdi_max_delay )
            {
                //system("/storage/dev/DEKTEC-DTU351/DTCOLLECTOR/obe-error.sh");
                int noFrameMS = (cur_frame_time - decklink_ctx->last_frame_time) / 1000;

                char msg[128];
                sprintf(msg, "Decklink card index %i: No frame received for %d ms", decklink_opts_->card_idx, noFrameMS);
                syslog(LOG_WARNING, "%s", msg);
                printf("%s\n", msg);

                if (OPTION_ENABLED_(los_exit_ms) && noFrameMS >= OPTION_ENABLED_(los_exit_ms)) {
                    sprintf(msg, "Terminating encoder as enable_los_exit_ms is active.");
                    syslog(LOG_WARNING, "%s", msg);
                    printf("%s\n", msg);
                    exit(0);
                }

                if (g_decklink_inject_frame_enable == 0) {
                    pthread_mutex_lock(&h->drop_mutex);
                    h->video_encoder_drop = h->audio_encoder_drop = h->mux_drop = 1;
                    pthread_mutex_unlock(&h->drop_mutex);
                }

            }

            decklink_ctx->last_frame_time = cur_frame_time;
        }

        //printf("video_format = %d, height = %d, width = %d, stride = %d\n", decklink_opts_->video_format, height, width, stride);

        videoframe->GetBytes( &frame_bytes );

        if (g_decklink_burnwriter_enable) {
            V210_write_32bit_value(frame_bytes, stride, g_decklink_burnwriter_count++, g_decklink_burnwriter_linenr, 1);
        }

#if READ_OSD_VALUE
	{
		static uint32_t xxx = 0;
		uint32_t val = V210_read_32bit_value(frame_bytes, stride, 210);
		if (xxx + 1 != val) {
                        char t[160];
                        sprintf(t, "%s", ctime(&now));
                        t[strlen(t) - 1] = 0;
                        fprintf(stderr, "%s: KL OSD counter discontinuity, expected %08" PRIx32 " got %08" PRIx32 "\n", t, xxx + 1, val);
		}
		xxx = val;
	}
#endif

        /* TODO: support format switching (rare in SDI) */
        int j;
        for( j = 0; first_active_line[j].format != -1; j++ )
        {
            if( decklink_opts_->video_format == first_active_line[j].format )
                break;
        }

        videoframe->GetAncillaryData( &ancillary );

        /* NTSC starts on line 4 */
        line = decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC ? 4 : 1;
        anc_line_stride = FFALIGN( (width * 2 * sizeof(uint16_t)), 16 );

        /* Overallocate slightly for VANC buffer
         * Some VBI services stray into the active picture so allocate some extra space */
        anc_buf = anc_buf_pos = (uint16_t*)av_malloc( DECKLINK_VANC_LINES * anc_line_stride );
        if( !anc_buf )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto end;
        }

        /* VANC Receiver for Decklink injection */
        struct udp_vanc_receiver_s *vr = &g_decklink_udp_vanc_receiver;
        if (g_decklink_udp_vanc_receiver_port && vr->active == 0) {

            /* Initialize the UDP socket */
            do {

                vr->bufmaxlen = 65536;
                vr->buf = (unsigned char *)malloc(vr->bufmaxlen);
                if (!vr->buf) {
            		fprintf(stderr, "[decklink] unable to allocate vanc_receiver buffer\n");
                    break;
                }

                vr->skt = socket(AF_INET, SOCK_DGRAM, 0);
                if (vr->skt < 0) {
            		fprintf(stderr, "[decklink] unable to allocate vanc_receiver socket\n");
                    break;
                }

                int reuse = 1;
                if (setsockopt(vr->skt, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            		fprintf(stderr, "[decklink] unable to configure vanc_receiver socket\n");
                    break;
                }

                vr->sin.sin_family = AF_INET;
                vr->sin.sin_port = htons(g_decklink_udp_vanc_receiver_port);
                vr->sin.sin_addr.s_addr = inet_addr("0.0.0.0");
                if (bind(vr->skt, (struct sockaddr *)&vr->sin, sizeof(vr->sin)) < 0) {
            		fprintf(stderr, "[decklink] unable to bind vanc_receiver socket\n");
                    break;
                }

                /* Non-blocking required */
                int fl = fcntl(vr->skt, F_GETFL, 0);
                if (fcntl(vr->skt, F_SETFL, fl | O_NONBLOCK) < 0) {
            		fprintf(stderr, "[decklink] unable to non-block vanc_receiver socket\n");
                    break;
                }

                /* Success */
                vr->active = 1;
            } while (0);
        }

        if (g_decklink_udp_vanc_receiver_port && vr->active) {
            /* Receive any pending message */
            ssize_t len = recv(vr->skt, vr->buf, vr->bufmaxlen, 0);
            if (len > 0) {
                /* Padd the end of the message  */
                for (int i = len; i < len + 8; i+= 2) {
                    vr->buf[i + 0] = 0x40;
                    vr->buf[i + 1] = 0x00;
                }
                len += 8;
                printf("vanc_receiver recd: %d bytes [ ", (int)len);
                for (int i = 0; i < len; i++) {
                    printf("0x%02x ", vr->buf[i]);
                }
                printf("]\n");
                endian_flip_array(vr->buf, len);
                ret = _vancparse(decklink_ctx->vanchdl, vr->buf, len, 10);
                if (ret < 0) {
                    fprintf(stderr, "%s() Unable to parse vanc_receiver message\n", __func__);
                } else {
#if 0
                    /* Paint a message in the V210, so we know per frame when an event was received. */
                    videoframe->GetBytes(&frame_bytes);
                    struct V210_painter_s painter;
                    V210_painter_reset(&painter, (unsigned char *)frame_bytes, width, height, stride, 0);
                    V210_painter_draw_ascii_at(&painter, 0, 3, "vanc_receiver msg injected");
#endif
                }
            }
        }

        if (g_decklink_inject_scte104_preroll6000 > 0 && videoframe) {
            g_decklink_inject_scte104_preroll6000 = 0;
            printf("Injecting a scte104 preroll 6000 message\n");

            /* Insert a static SCTE104 splice messages to occur 6 seconds from now. */
            /* Pass this into the framework just like any other message we'd find
             * on line 10, for DID 41 and SDID 07.
             */
            unsigned char msg[] = {
                0x00, 0x00, 0xff, 0x03, 0xff, 0x03, 0x41, 0x02, 0x07, 0x01, 0x1f, 0x01, 0x08, 0x01, 0xff, 0x02,
                0xff, 0x02, 0x00, 0x02, 0x1e, 0x02, 0x00, 0x02, 0x01, 0x01, 0x3b, 0x01, 0x00, 0x02, 0x01, 0x01,
                0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x02, 0x0e, 0x01, 0x01, 0x01,
                0x00, 0x02, 0x00, 0x02, 0xb1, 0x02, 0xdd, 0x02, 0x00, 0x02, 0x00, 0x02, 0x17, 0x02, 0x70, 0x01,
                0x0b, 0x01, 0xb8, 0x02, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0xb3, 0x01, 0x40, 0x00, 0x40, 0x00,
                0x40, 0x00
            };
            endian_flip_array(msg, sizeof(msg));
            ret = _vancparse(decklink_ctx->vanchdl, &msg[0], sizeof(msg), 10);
            if (ret < 0) {
                fprintf(stderr, "%s() Unable to parse scte104 preroll 6000 packet\n", __func__);
            }

            /* Paint a message in the V210, so we know per frame when an event was received. */
            videoframe->GetBytes(&frame_bytes);
            struct V210_painter_s painter;
            V210_painter_reset(&painter, (unsigned char *)frame_bytes, width, height, stride, 0);
            V210_painter_draw_ascii_at(&painter, 0, 3, "SCTE104 - 6000ms Preroll injected");
        }

        if (g_decklink_inject_scte104_fragmented > 0 && videoframe) {
            g_decklink_inject_scte104_fragmented = 0;
            printf("Injecting a scte104 fragmented frame\n");

            /* Very large multi-fragment SCTE104 message. */
            unsigned char msg[] = {
                0x00, 0x00, 0xff, 0x03, 0xff, 0x03, 0x41, 0x02, 0x07, 0x01, 0xff, 0x02, 0x0c, 0x02, 0xff, 0x02,
                0xff, 0x02, 0x01, 0x01, 0x0e, 0x01, 0x00, 0x02, 0x00, 0x02, 0x09, 0x02, 0x00, 0x02, 0x00, 0x02,
                0x00, 0x02, 0x00, 0x02, 0x05, 0x02, 0x01, 0x01, 0x04, 0x01, 0x00, 0x02, 0x02, 0x01, 0x0f, 0x02,
                0xa0, 0x02, 0x01, 0x01, 0x0b, 0x01, 0x00, 0x02, 0x23, 0x01, 0x07, 0x01, 0xb8, 0x02, 0x42, 0x02,
                0xb9, 0x01, 0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x11, 0x02, 0x55, 0x02, 0x4b, 0x02,
                0x52, 0x01, 0x41, 0x02, 0x49, 0x01, 0x4e, 0x02, 0x45, 0x01, 0x48, 0x02, 0x45, 0x01, 0x4c, 0x01,
                0x50, 0x02, 0x30, 0x02, 0x34, 0x01, 0x57, 0x01, 0x50, 0x02, 0x56, 0x02, 0x49, 0x01, 0x35, 0x02,
                0x01, 0x01, 0x01, 0x01, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02,
                0x01, 0x01, 0x0b, 0x01, 0x00, 0x02, 0x43, 0x01, 0x07, 0x01, 0x82, 0x02, 0xab, 0x01, 0x69, 0x02,
                0x00, 0x02, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x31, 0x01, 0x50, 0x02, 0x43, 0x01, 0x52, 0x01,
                0x31, 0x01, 0x5f, 0x02, 0x30, 0x02, 0x33, 0x02, 0x30, 0x02, 0x35, 0x02, 0x32, 0x01, 0x32, 0x01,
                0x30, 0x02, 0x35, 0x02, 0x35, 0x02, 0x38, 0x01, 0x57, 0x01, 0x50, 0x02, 0x56, 0x02, 0x49, 0x01,
                0x41, 0x02, 0x43, 0x01, 0x54, 0x01, 0x49, 0x01, 0x4f, 0x01, 0x4e, 0x02, 0x4e, 0x02, 0x45, 0x01,
                0x57, 0x01, 0x53, 0x02, 0x53, 0x02, 0x41, 0x02, 0x54, 0x01, 0x55, 0x02, 0x52, 0x01, 0x44, 0x02,
                0x41, 0x02, 0x59, 0x02, 0x4d, 0x02, 0x4f, 0x01, 0x52, 0x01, 0x4e, 0x02, 0x49, 0x01, 0x4e, 0x02,
                0x47, 0x02, 0x41, 0x02, 0x54, 0x01, 0x36, 0x02, 0x41, 0x02, 0x4d, 0x02, 0x11, 0x02, 0x01, 0x01,
                0x01, 0x01, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02, 0x01, 0x01,
                0x0b, 0x01, 0x00, 0x02, 0x43, 0x01, 0x07, 0x01, 0xb9, 0x01, 0xb2, 0x02, 0x16, 0x01, 0x00, 0x02,
                0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x31, 0x01, 0x50, 0x02, 0x43, 0x01, 0x52, 0x01, 0x31, 0x01,
                0x5f, 0x02, 0x30, 0x02, 0x33, 0x02, 0x30, 0x02, 0x35, 0x02, 0x32, 0x01, 0x32, 0x01, 0x30, 0x02,
                0x36, 0x02, 0x35, 0x02, 0x38, 0x01, 0x57, 0x01, 0x50, 0x02, 0x56, 0x02, 0x49, 0x01, 0x41, 0x02,
                0x43, 0x01, 0x54, 0x01, 0x49, 0x01, 0x4f, 0x01, 0x4e, 0x02, 0x4e, 0x02, 0x45, 0x01, 0x57, 0x01,
                0x53, 0x02, 0x53, 0x02, 0x41, 0x02, 0x54, 0x01, 0x55, 0x02, 0x52, 0x01, 0x44, 0x02, 0x41, 0x02,
                0x59, 0x02, 0x4d, 0x02, 0x4f, 0x01, 0x52, 0x01, 0x4e, 0x02, 0x49, 0x01, 0x4e, 0x02, 0x47, 0x02,
                0x41, 0x02, 0x54, 0x01, 0x37, 0x01, 0x41, 0x02, 0x4d, 0x02, 0x10, 0x01, 0x01, 0x01, 0x01, 0x01,
                0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02, 0x01, 0x01, 0x0b, 0x01,
                0x00, 0x02, 0x43, 0x01, 0x07, 0x01, 0xb9, 0x01, 0xb2, 0x02, 0x17, 0x02, 0x00, 0x02, 0x02, 0x01,
                0x62, 0x01, 0x01, 0x01, 0x31, 0x01, 0x50, 0x02, 0x43, 0x01, 0x52, 0x01, 0x31, 0x01, 0x5f, 0x02,
                0x30, 0x02, 0x33, 0x02, 0x30, 0x02, 0x35, 0x02, 0x32, 0x01, 0x32, 0x01, 0x30, 0x02, 0x36, 0x02,
                0x35, 0x02, 0x38, 0x01, 0x57, 0x01, 0x50, 0x02, 0x56, 0x02, 0x49, 0x01, 0x41, 0x02, 0x43, 0x01,
                0x54, 0x01, 0x49, 0x01, 0x4f, 0x01, 0x4e, 0x02, 0x4e, 0x02, 0x45, 0x01, 0x57, 0x01, 0x53, 0x02,
                0x53, 0x02, 0x41, 0x02, 0x54, 0x01, 0x55, 0x02, 0x52, 0x01, 0x44, 0x02, 0x41, 0x02, 0x59, 0x02,
                0x4d, 0x02, 0x4f, 0x01, 0x52, 0x01, 0x4e, 0x02, 0x49, 0x01, 0xd5, 0x02,
                0x00, 0x00, 0xff, 0x03, 0xff, 0x03, 0x41, 0x02, 0x07, 0x01, 0x11, 0x02, 0x0a, 0x02, 0x4e, 0x02,
                0x47, 0x02, 0x41, 0x02, 0x54, 0x01, 0x37, 0x01, 0x41, 0x02, 0x4d, 0x02, 0x20, 0x01, 0x01, 0x01,
                0x01, 0x01, 0x00, 0x02, 0x00, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x03, 0x02, 0x7a, 0x01,
            };
            endian_flip_array(msg, sizeof(msg));
            ret = _vancparse(decklink_ctx->vanchdl, &msg[0], sizeof(msg), 10);
            if (ret < 0) {
                fprintf(stderr, "%s() Unable to parse scte104 fragment\n", __func__);
            }

            /* Paint a message in the V210, so we know per frame when an event was received. */
            videoframe->GetBytes(&frame_bytes);
            struct V210_painter_s painter;
            V210_painter_reset(&painter, (unsigned char *)frame_bytes, width, height, stride, 0);
            V210_painter_draw_ascii_at(&painter, 0, 3, "SCTE104 - fragments injected");
        }

	/* TODO: When using 4k formats, we crash in the blank_line func. fixme.
	 *       When testing 4k, I completely remove this block.
	 */
        while( 1 )
        {
            /* Some cards have restrictions on what lines can be accessed so try them all
             * Some buggy decklink cards will randomly refuse access to a particular line so
             * work around this issue by blanking the line */
            if( ancillary->GetBufferForVerticalBlankingLine( line, &anc_line ) == S_OK ) {

                /* Give libklvanc a chance to parse all vanc, and call our callbacks (same thread) */
                convert_colorspace_and_parse_vanc(decklink_ctx, decklink_ctx->vanchdl,
                                                  (unsigned char *)anc_line, width, line);

                decklink_ctx->unpack_line( (uint32_t*)anc_line, anc_buf_pos, width );
            } else
                decklink_ctx->blank_line( anc_buf_pos, width );

            anc_buf_pos += anc_line_stride / 2;
            anc_lines[num_anc_lines++] = line;

            if( !first_line )
                first_line = line;
            last_line = line;

            lines_read++;
            line = sdi_next_line( decklink_opts_->video_format, line );

            if( line == first_active_line[j].line )
                break;
        }

        ancillary->Release();

        if( !decklink_opts_->probe )
        {
            raw_frame = new_raw_frame();
            if( !raw_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }
        }

        anc_buf_pos = anc_buf;
        for( int i = 0; i < num_anc_lines; i++ )
        {
            parse_vanc_line( h, &decklink_ctx->non_display_parser, raw_frame, anc_buf_pos, width, anc_lines[i] );
            anc_buf_pos += anc_line_stride / 2;
        }

        /* Check to see if 708 was present previously.  If we are expecting 708
           and it's been more than five frames, inject a message to force a
           reset in any downstream decoder */
        if( check_user_selected_non_display_data( h, CAPTIONS_CEA_708,
                                                  USER_DATA_LOCATION_FRAME ) &&
            !check_active_non_display_data( raw_frame, USER_DATA_CEA_708_CDP ) )
        {
            if (h->cea708_missing_count++ == 5)
            {
                /* FIXME: for now only support 1080i (i.e. cc_count=20) */
                const struct obe_to_decklink_video *fmt = decklink_ctx->enabled_mode_fmt;
                if (fmt->timebase_num == 1001 && fmt->timebase_den == 30000) {
                    uint8_t cdp[] = {0x96, 0x69, 0x49, 0x4f, 0x43, 0x02, 0x36, 0x72, 0xf4, 0xff, 0x02, 0x21, 0xfe, 0x8f, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0xf8, 0x00, 0x00, 0x74, 0x02, 0x36, 0xbd };
                    inject_708_cdp(h, raw_frame, cdp, sizeof(cdp));
                }
            }
        } else {
            h->cea708_missing_count = 0;
        }

        if( IS_SD( decklink_opts_->video_format ) && first_line != last_line )
        {
            /* Add a some VBI lines to the ancillary buffer */
            frame_ptr = (uint32_t*)frame_bytes;

            /* NTSC starts from line 283 so add an extra line */
            num_vbi_lines = NUM_ACTIVE_VBI_LINES + ( decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC );
            for( int i = 0; i < num_vbi_lines; i++ )
            {
                decklink_ctx->unpack_line( frame_ptr, anc_buf_pos, width );
                anc_buf_pos += anc_line_stride / 2;
                frame_ptr += stride / 4;
                last_line = sdi_next_line( decklink_opts_->video_format, last_line );
            }
            num_anc_lines += num_vbi_lines;

            vbi_buf = (uint8_t*)av_malloc( width * 2 * num_anc_lines );
            if( !vbi_buf )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }

            /* Scale the lines from 10-bit to 8-bit */
            decklink_ctx->downscale_line( anc_buf, vbi_buf, num_anc_lines );
            anc_buf_pos = anc_buf;

            /* Handle Video Index information */
            int tmp_line = first_line;
            vii_line = decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC ? NTSC_VIDEO_INDEX_LINE : PAL_VIDEO_INDEX_LINE;
            while( tmp_line < vii_line )
            {
                anc_buf_pos += anc_line_stride / 2;
                tmp_line++;
            }

            if( decode_video_index_information( h, &decklink_ctx->non_display_parser, anc_buf_pos, raw_frame, vii_line ) < 0 )
                goto fail;

            if( !decklink_ctx->has_setup_vbi )
            {
                vbi_raw_decoder_init( &decklink_ctx->non_display_parser.vbi_decoder );

                decklink_ctx->non_display_parser.ntsc = decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC;
                decklink_ctx->non_display_parser.vbi_decoder.start[0] = first_line;
                decklink_ctx->non_display_parser.vbi_decoder.start[1] = sdi_next_line( decklink_opts_->video_format, first_line );
                decklink_ctx->non_display_parser.vbi_decoder.count[0] = last_line - decklink_ctx->non_display_parser.vbi_decoder.start[1] + 1;
                decklink_ctx->non_display_parser.vbi_decoder.count[1] = decklink_ctx->non_display_parser.vbi_decoder.count[0];

                if( setup_vbi_parser( &decklink_ctx->non_display_parser ) < 0 )
                    goto fail;

                decklink_ctx->has_setup_vbi = 1;
            }

            if( decode_vbi( h, &decklink_ctx->non_display_parser, vbi_buf, raw_frame ) < 0 )
                goto fail;

            clock_gettime(CLOCK_REALTIME, &g_cea708_detected_ts);

            av_free( vbi_buf );
        }

        av_free( anc_buf );

        if( !decklink_opts_->probe )
        {
            ltn_histogram_sample_begin(decklink_ctx->callback_4_hdl);
            frame = av_frame_alloc();
            if( !frame )
            {
                syslog( LOG_ERR, "[decklink]: Could not allocate video frame\n" );
                goto end;
            }
            decklink_ctx->codec->width = width;
            decklink_ctx->codec->height = height;

            pkt.data = (uint8_t*)frame_bytes;
            pkt.size = stride * height;

//frame->width = width;
//frame->height = height;
//frame->format = decklink_ctx->codec->pix_fmt;
//

            ret = avcodec_send_packet(decklink_ctx->codec, &pkt);
            while (ret >= 0) {
                ret = avcodec_receive_frame(decklink_ctx->codec, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    return -1;
                else if (ret < 0) {
                }
                break;
            }

            raw_frame->release_data = obe_release_video_data;
            raw_frame->release_frame = obe_release_frame;

            memcpy( raw_frame->alloc_img.stride, frame->linesize, sizeof(raw_frame->alloc_img.stride) );
            memcpy( raw_frame->alloc_img.plane, frame->data, sizeof(raw_frame->alloc_img.plane) );
            av_frame_free( &frame );
            raw_frame->alloc_img.csp = decklink_ctx->codec->pix_fmt;

            const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(raw_frame->alloc_img.csp);
            raw_frame->alloc_img.planes = d->nb_components;
            raw_frame->alloc_img.width = width;
            raw_frame->alloc_img.height = height;
            raw_frame->alloc_img.format = decklink_opts_->video_format;
            raw_frame->timebase_num = decklink_opts_->timebase_num;
            raw_frame->timebase_den = decklink_opts_->timebase_den;

            memcpy( &raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->alloc_img) );
//PRINT_OBE_IMAGE(&raw_frame->img      , "      DECK->img");
//PRINT_OBE_IMAGE(&raw_frame->alloc_img, "DECK->alloc_img");
            if( IS_SD( decklink_opts_->video_format ) )
            {
                raw_frame->img.first_line = first_active_line[j].line;
                if( decklink_opts_->video_format == INPUT_VIDEO_FORMAT_NTSC )
                {
                    raw_frame->img.height = 480;
                    while( raw_frame->img.first_line != NTSC_FIRST_CODED_LINE )
                    {
                        for( int i = 0; i < raw_frame->img.planes; i++ )
                            raw_frame->img.plane[i] += raw_frame->img.stride[i];

                        raw_frame->img.first_line = sdi_next_line( INPUT_VIDEO_FORMAT_NTSC, raw_frame->img.first_line );
                    }
                }
            }

            /* If AFD is present and the stream is SD this will be changed in the video filter */
            raw_frame->sar_width = raw_frame->sar_height = 1;
            raw_frame->pts = decklink_ctx->stream_time;

            for( int i = 0; i < decklink_ctx->device->num_input_streams; i++ )
            {
                if (decklink_ctx->device->input_streams[i]->stream_format == VIDEO_UNCOMPRESSED)
                    raw_frame->input_stream_id = decklink_ctx->device->input_streams[i]->input_stream_id;
            }

            if (framesQueued++ == 0) {
                //clock_offset = (packet_time * -1);
                //printf(PREFIX "Clock offset established as %" PRIi64 "\n", clock_offset);

            }

            avfm_init(&raw_frame->avfm, AVFM_VIDEO);
            avfm_set_hw_status_mask(&raw_frame->avfm,
                decklink_ctx->isHalfDuplex ? AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_HALF :
                    AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_FULL);
            avfm_set_pts_video(&raw_frame->avfm, decklink_ctx->stream_time + clock_offset);

            {
                /* Video frames use the audio timestamp. If the audio timestamp is missing we'll
                 * calculate the audio timestamp based on last timestamp.
                 */
                static int64_t lastpts = 0;
                if (packet_time == 0) {
                    packet_time = lastpts + decklink_ctx->vframe_duration;
                }
                lastpts = packet_time;
            }
            avfm_set_pts_audio(&raw_frame->avfm, packet_time + clock_offset);
            avfm_set_hw_received_time(&raw_frame->avfm);
            avfm_set_video_interval_clk(&raw_frame->avfm, decklink_ctx->vframe_duration);
            //raw_frame->avfm.hw_audio_correction_clk = clock_offset;
            //avfm_dump(&raw_frame->avfm);

            if (g_decklink_inject_frame_enable)
                cache_video_frame(raw_frame);

            /* Attach the timecode to the frame, if we have one */
            if (decklink_ctx->timecode.present) {
                raw_frame->timecode.present = 1;
                raw_frame->timecode.hours   = decklink_ctx->timecode.hours;
                raw_frame->timecode.mins    = decklink_ctx->timecode.minutes;
                raw_frame->timecode.seconds = decklink_ctx->timecode.seconds;
                raw_frame->timecode.frames  = decklink_ctx->timecode.frames;
                raw_frame->timecode.drop_frame = decklink_ctx->timecode.drop_frame;
            }

            /* Ensure we put any associated video vanc / metadata into this raw frame. */
            avmetadata_clone(&raw_frame->metadata, &decklink_ctx->metadataVANC);

            if( add_to_filter_queue( h, raw_frame ) < 0 )
                goto fail;

            if( send_vbi_and_ttx( h, &decklink_ctx->non_display_parser, raw_frame->pts ) < 0 )
                goto fail;

            decklink_ctx->non_display_parser.num_vbi = 0;
            decklink_ctx->non_display_parser.num_anc_vbi = 0;
            ltn_histogram_sample_end(decklink_ctx->callback_4_hdl);
        }
        ltn_histogram_sample_end(decklink_ctx->callback_2_hdl);
    } /* if video frame */

    if (audioframe) {
        if(OPTION_ENABLED_(bitstream_audio)) {
            for (int i = 0; i < MAX_AUDIO_PAIRS; i++) {
                audioframe->GetBytes(&frame_bytes);

                /* Look for bitstream in audio channels 0 and 1 */
                /* TODO: Examine other channels. */
                /* TODO: Kinda pointless caching a successful find, because those
                 * values held in decklink_ctx are thrown away when the probe completes. */
                int depth = 32;
                int span = 2;
                struct audio_pair_s *pair = &decklink_ctx->audio_pairs[i];
                if (pair->smpte337_detector) {
                    pair->smpte337_frames_written++;

                    /* Figure out the offset in the line, where this channel pair begins. */
                    int offset = i * ((depth / 8) * span);
                    smpte337_detector_write(pair->smpte337_detector, (uint8_t *)frame_bytes + offset,
                        audioframe->GetSampleFrameCount(),
                        depth,
                        decklink_opts_->num_channels,
                        decklink_opts_->num_channels * (depth / 8),
                        span);

                }
            }
        }
    }

    ltn_histogram_sample_begin(decklink_ctx->callback_3_hdl);
    if( audioframe && !decklink_opts_->probe )
    {
        processAudio(decklink_ctx, decklink_opts_, audioframe, decklink_ctx->stream_time);
    }

end:
    if( frame )
        av_frame_free( &frame );

    av_packet_unref( &pkt );

    ltn_histogram_sample_end(decklink_ctx->callback_3_hdl);
    return S_OK;

fail:

    if( raw_frame )
    {
        if (raw_frame->release_data)
            raw_frame->release_data( raw_frame );
        if (raw_frame->release_frame)
            raw_frame->release_frame( raw_frame );
    }

    return S_OK;
}

static void close_card( decklink_opts_t *decklink_opts )
{
    decklink_ctx_t *decklink_ctx = &decklink_opts->decklink_ctx;

    if( decklink_ctx->p_config )
        decklink_ctx->p_config->Release();

    if( decklink_ctx->p_input )
    {
        decklink_ctx->p_input->StopStreams();
        decklink_ctx->p_input->Release();
    }

    if( decklink_ctx->p_card )
        decklink_ctx->p_card->Release();

    if( decklink_ctx->p_delegate )
        decklink_ctx->p_delegate->Release();

    if( decklink_ctx->codec )
    {
        avcodec_close( decklink_ctx->codec );
        av_free( decklink_ctx->codec );
    }

    if (decklink_ctx->vanchdl) {
        klvanc_context_destroy(decklink_ctx->vanchdl);
        decklink_ctx->vanchdl = 0;
    }

    if (decklink_ctx->sdi_audio_analyzer_ctx) {
        ltnsdi_context_free(decklink_ctx->sdi_audio_analyzer_ctx);
        decklink_ctx->sdi_audio_analyzer_ctx = 0;
    }
    if (decklink_ctx->smpte2038_ctx) {
        klvanc_smpte2038_packetizer_free(&decklink_ctx->smpte2038_ctx);
        decklink_ctx->smpte2038_ctx = 0;
    }

    for (int i = 0; i < MAX_AUDIO_PAIRS; i++) {
        struct audio_pair_s *pair = &decklink_ctx->audio_pairs[i];
        if (pair->smpte337_detector) {
            smpte337_detector_free(pair->smpte337_detector);
            pair->smpte337_detector = 0;
        }
    }

    if( IS_SD( decklink_opts->video_format ) )
        vbi_raw_decoder_destroy( &decklink_ctx->non_display_parser.vbi_decoder );

    if (decklink_ctx->avr)
        swr_free(&decklink_ctx->avr);

}

/* VANC Callbacks */
static int cb_EIA_708B(void *callback_context, struct klvanc_context_s *ctx, struct klvanc_packet_eia_708b_s *pkt)
{
	decklink_ctx_t *decklink_ctx = (decklink_ctx_t *)callback_context;
	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		klvanc_dump_EIA_708B(ctx, pkt); /* vanc lib helper */
	}

	return 0;
}

static int cb_EIA_608(void *callback_context, struct klvanc_context_s *ctx, struct klvanc_packet_eia_608_s *pkt)
{
	decklink_ctx_t *decklink_ctx = (decklink_ctx_t *)callback_context;
	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		klvanc_dump_EIA_608(ctx, pkt); /* vanc library helper */
	}

	return 0;
}

/* For a given SCTE35 stream 0..N, find it */
static int findOutputStreamIdByFormat(decklink_ctx_t *decklink_ctx, enum stream_type_e stype, enum stream_formats_e fmt, int instancenr)
{
	if (decklink_ctx && decklink_ctx->device == NULL)
		return -1;

    int instance = 0;
	for(int i = 0; i < decklink_ctx->device->num_input_streams; i++) {
		if ((decklink_ctx->device->input_streams[i]->stream_type == stype) &&
			(decklink_ctx->device->input_streams[i]->stream_format == fmt)) {
			    if (instance++ == instancenr)
                    return i;
            }
        }

	return -1;
}

/* We're given some VANC data, create a SCTE104 metadata entry for it
 * and we'll send it to the encoder later, by an attachment to the video frame.
 */
static int add_metadata_scte104_vanc_section(decklink_ctx_t *decklink_ctx,
	struct avmetadata_s *md,
	uint8_t *section, uint32_t section_length, int lineNr, int streamId)
{
	/* Put the coded frame into an raw_frame attachment and discard the coded frame. */
	int idx = md->count;
	if (idx >= MAX_RAW_FRAME_METADATA_ITEMS) {
		printf("%s() already has %d items, unable to add additional. Skipping\n",
			__func__, idx);
		return -1;
	}

	md->array[idx] = avmetadata_item_alloc(section_length, AVMETADATA_VANC_SCTE104);
	if (!md->array[idx])
		return -1;
	md->count++;

	avmetadata_item_data_write(md->array[idx], section, section_length);
	avmetadata_item_data_set_linenr(md->array[idx], lineNr);

	avmetadata_item_data_set_outputstreamid(md->array[idx], streamId);

	return 0;
}

static int transmit_pes_to_muxer(decklink_ctx_t *decklink_ctx, uint8_t *buf, uint32_t byteCount, stream_formats_e stream_format)
{
	int streamId = findOutputStreamIdByFormat(decklink_ctx, STREAM_TYPE_MISC, stream_format, 0);
	if (streamId < 0)
		return 0;

	/* Now send the constructed frame to the mux */
	obe_coded_frame_t *coded_frame = new_coded_frame(streamId, byteCount);
	if (!coded_frame) {
		syslog(LOG_ERR, "Malloc failed during %s, needed %d bytes\n", __func__, byteCount);
		return -1;
	}
	coded_frame->pts = decklink_ctx->stream_time;
	coded_frame->random_access = 1; /* ? */
	memcpy(coded_frame->data, buf, byteCount);
	add_to_queue(&decklink_ctx->h->mux_queue, coded_frame);

	return 0;
}

extern struct scte104_filtering_syntax_s g_scte104_filtering_context;

static int cb_SCTE_104(void *callback_context, struct klvanc_context_s *ctx, struct klvanc_packet_scte_104_s *pkt)
{
    /* 20231004 - Adding the ability to put specific 104 messages on specific SCTE35 pids */
    /* See ANSI_SCTE-104-2019a-1582645426999.pdf */
    /*
     * scte35 pid = 56, 57    (enables two scte pids and these numbers will be used to filter below with additional syntax)
     */
    /* We going to filter 104 messages based on 104's interpretation of two fields:
     * DPI_PID_index: uint16_r, 8.2.3.2. Format of the multiple_operation_message() structure
     * AS_index field: uint8_t, 8.2.3.2. Format of the multiple_operation_message() structure
     *   encoder controller cfg syntax for this will be (examples):
     *      <SCTE35 filter> = pid = 56, AS_index = all,   DPI_PID_index = all
     *      <SCTE35 filter> = pid = 56, AS_index = all,   DPI_PID_index = uint8
     *      <SCTE35 filter> = pid = 56, AS_index = uint8, DPI_PID_index = all
     *      <SCTE35 filter> = pid = 56, AS_index = uint8, DPI_PID_index = uint8
     *      <SCTE35 filter> = pid = 57, AS_index = uint8, DPI_PID_index = all
     * -
     * # Set the SCTE 35 encoding pid number, if empty, pid is audio pid + 1
     * # If multiple SCTE35 pids are required, with SCTE104 filters, talk to the video team
     * # see the "SCTE35 filter" syntax for configuring SCTE104 filter details
     * scte35 pid = 56, 57
     * 
     * # For every SCTE35 pid, apply an optional filter
     * # no values implies no SCTE104 filtering, all messages will be handled.
     * # See the video team for exact configuration syntax. 
     * SCTE104 filter 1 = pid = 56, AS_index = all, DPI_PID_index = 1
     * SCTE104 filter 2 = pid = 57, AS_index = all, DPI_PID_index = 80
     * 
     * This syntax will get passed directly by the controller into the encoder
     * using set variable commands, immediately before the start call has been issues.
     * set variable scte104.filter1 = pid = 56, AS_index = all, DPI_PID_index = 1
     * set variable scte104.filter2 = pid = 57, AS_index = all, DPI_PID_index = 80
     */

    clock_gettime(CLOCK_REALTIME, &g_scte104_detected_ts);
//struct timespec g_ce708_detected_ts;

	/* It should be impossible to get here until the user has asked to enable SCTE35 */

	decklink_ctx_t *decklink_ctx = (decklink_ctx_t *)callback_context;
	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_SCTE104) {
		printf("%s:%s()\n", __FILE__, __func__);
		klvanc_dump_SCTE_104(ctx, pkt); /* vanc library helper */
	}

	if (klvanc_packetType1(&pkt->hdr)) {
		/* Silently discard type 1 SCTE104 packets, as per SMPTE 291 section 6.3 */
		return 0;
	}

    if (scte104filter_count(&g_scte104_filtering_context) == 0) {
        /* no filters at all, as in the default use case, just pass everything downstream */
        int streamId = findOutputStreamIdByFormat(decklink_ctx, STREAM_TYPE_MISC, DVB_TABLE_SECTION, 0);
        if (streamId < 0)
            return 0;

        //printf("Unfiltered: Sending SCTE104 MOM to default SCTE35 pid via streamId %d\n", streamId);

        /* Append all the original VANC to the metadata object, we'll process it later. */
        /* TODO, pass a SCTE instance here, or refactor the stream lookup by PID nr */
        add_metadata_scte104_vanc_section(decklink_ctx, &decklink_ctx->metadataVANC,
            (uint8_t *)&pkt->hdr.raw[0],
            pkt->hdr.rawLengthWords * 2,
            pkt->hdr.lineNr, streamId);

    } else {
        /* Some filters, lets run through the table */

        /* Obtain a list of pids that needs this SCTE35 Message */
        uint16_t *pids = NULL;
        uint8_t pidcount = 0;
        int ret = scte104filter_lookup_scte35_pids(&g_scte104_filtering_context, &pkt->mo_msg, &pids, &pidcount);
        if (ret < 0)
            return 0;
#if 1
        if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_SCTE104) {
            printf("%s() SCTE104 AS_Index %d DPI_PID_index %d\n",
                __func__,
                pkt->mo_msg.AS_index, pkt->mo_msg.DPI_PID_index);

            for (int i = 0; i < pidcount; i++) {
                printf("%s() the following pids need SCTE104 output, any others do not:\n", __func__);
                printf("%s() array[%d] = 0x%04x (%4d)\n", __func__, i, pids[i], pids[i]);
            }
        }
#endif
        for (int i = 0; i < pidcount; i++) {
            int streamId = obe_core_findOutputStreamIdByPID(pids[i]);
            if (streamId < 0)
                continue;

            if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_SCTE104) {
                printf("Filtered: Sending SCTE104 MOM to pid 0x%04x via streamId %d\n",
                    pids[i], streamId);
            }

            /* Append all the original VANC to the metadata object, we'll process it later. */
            /* TODO, pass a SCTE instance here, or refactor the stream lookup by PID nr */
            add_metadata_scte104_vanc_section(decklink_ctx, &decklink_ctx->metadataVANC,
                (uint8_t *)&pkt->hdr.raw[0],
                pkt->hdr.rawLengthWords * 2,
                pkt->hdr.lineNr, streamId);
        }

        if (pids) {
            free(pids);
            pids = NULL;
        }
    }

	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_SCTE104) {
		static time_t lastErrTime = 0;
		time_t now = time(0);
		if (lastErrTime != now) {
			lastErrTime = now;

			char t[64];
			sprintf(t, "%s", ctime(&now));
			t[ strlen(t) - 1] = 0;
			syslog(LOG_INFO, "[decklink] SCTE104 frames present on SDI");
			fprintf(stdout, "[decklink] SCTE104 frames present on SDI  @ %s", t);
			printf("\n");
			fflush(stdout);

		}
	}

	return 0;
}

static int cb_all(void *callback_context, struct klvanc_context_s *ctx, struct klvanc_packet_header_s *pkt)
{
	decklink_ctx_t *decklink_ctx = (decklink_ctx_t *)callback_context;
	if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s()\n", __func__);
	}

	/* We've been called with a VANC frame. Pass it to the SMPTE2038 packetizer.
	 * We'll be called here from the thread handing the VideoFrameArrived
	 * callback, which calls vanc_packet_parse for each ANC line.
	 * Push the pkt into the SMPTE2038 layer, its collecting VANC data.
	 */
	if (decklink_ctx->smpte2038_ctx) {
		if (klvanc_smpte2038_packetizer_append(decklink_ctx->smpte2038_ctx, pkt) < 0) {
		}
	}

	decklink_opts_t *decklink_opts = container_of(decklink_ctx, decklink_opts_t, decklink_ctx);
	if (OPTION_ENABLED(patch1) && decklink_ctx->vanchdl && pkt->did == 0x52 && pkt->dbnsdid == 0x01) {

		/* Patch#1 -- SCTE104 VANC appearing in a non-standard DID.
		 * Modify the DID to reflect traditional SCTE104 and re-parse.
		 * Any potential multi-entrant libklvanc issues here? No now, future probably yes.
		 */

		/* DID 0x62 SDID 01 : 0000 03ff 03ff 0162 0101 0217 01ad 0115 */
		pkt->raw[3] = 0x241;
		pkt->raw[4] = 0x107;
		int ret = klvanc_packet_parse(decklink_ctx->vanchdl, pkt->lineNr, pkt->raw, pkt->rawLengthWords);
		if (ret < 0) {
			/* No VANC on this line */
			fprintf(stderr, "%s() patched VANC for did 0x52 failed\n", __func__);
		}
	}

	return 0;
}

static int cb_VANC_TYPE_KL_UINT64_COUNTER(void *callback_context, struct klvanc_context_s *ctx, struct klvanc_packet_kl_u64le_counter_s *pkt)
{
        /* Have the library display some debug */
	static uint64_t lastGoodKLFrameCounter = 0;
        if (lastGoodKLFrameCounter && lastGoodKLFrameCounter + 1 != pkt->counter) {
                char t[160];
                time_t now = time(0);
                sprintf(t, "%s", ctime(&now));
                t[strlen(t) - 1] = 0;

                fprintf(stderr, "%s: KL VANC frame counter discontinuity was %" PRIu64 " now %" PRIu64 "\n",
                        t,
                        lastGoodKLFrameCounter, pkt->counter);
        }
        lastGoodKLFrameCounter = pkt->counter;

        return 0;
}

const uint8_t REVERSE[256] = {
        0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0, 0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
        0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8, 0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
        0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4, 0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
        0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec, 0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
        0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2, 0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
        0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea, 0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
        0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6, 0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
        0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee, 0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
        0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1, 0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
        0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9, 0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
        0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5, 0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
        0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed, 0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
        0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3, 0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
        0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb, 0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
        0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7, 0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
        0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef, 0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff
};

static int cb_SDP(void *callback_context, struct klvanc_context_s *ctx, struct klvanc_packet_sdp_s *pkt)
{
    /* Its worth pointing out that with the exception of the PMT descriptor tag id, the
     * actual PES structure of EN301775 and EN300472 are identical, one beingan older spec
     * than the other.
     */
    decklink_ctx_t *decklink_ctx = (decklink_ctx_t *)callback_context;
    decklink_opts_t *decklink_opts = container_of(decklink_ctx, decklink_opts_t, decklink_ctx);

    if (decklink_ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_RDD8) {
        klvanc_dump_SDP(ctx, pkt); /* vanc lib helper */
    }

    if (OPTION_ENABLED(smpte2031)) {

        if (pkt->format_code != SDP_WSS_TELETEXT) {
            return 0; /* Silently abort parsing */
        }
        if (pkt->identifier != 0x5115) {
            return 0; /* Silently abort parsing */
        }

	int N_units = 0; /* See EN301755 or EN300472 Section 4.2 */
        for (int j = 0; j < 5; j++) {

            /* Skip any illegal / undefined lines */
            if (pkt->descriptors[j].line == 0)
                continue;

		N_units++;
	}

	/* We could be here with N=0 and we'll need to stuff all data_units */
	/* N can never be more than 5, so we're either prepping a pes for the muxer
	 * to fit in either one or two TS packets.
	 */

	int stuffing_units;
	if (N_units <= 3)
		stuffing_units = 3 - N_units;
	else
		stuffing_units = 7 - N_units;

	/* Total pes length will be:
	 * header              45 bytes
	 * data_identifier      1 byte
         * N_units * 46         bytes
         * stuffing_units * 46  bytes
	 */
	int PES_packet_length = ((1 + N_units + stuffing_units) * 46) - 6;

        bs_t bs;

        /* Enough space for 45 + 1 + ((n + stuff) * 46) = 368 bytes */
        unsigned char buf[380];

        bs_init(&bs, buf, sizeof(buf));
        int cnt = 8 + 31;  /* PES_packet_length header 4 + stuffing payload + payload */

        /* PES Header - Bug: bitstream can't write 32bit values */
        bs_write(&bs, 24, 1);         /* packet_start_code_prefix */
        bs_write(&bs,  8, 0xBD);      /* stream_id private_stream_1 */
        bs_write(&bs, 16, 0);         /* PES_packet_length header 4 + stuffing payload + payload */
        bs_write(&bs,  2, 2);         /* '10' fixed value */
        bs_write(&bs,  2, 0);         /* PES_scrambling_control (not scrambled) */
        bs_write(&bs,  1, 0);         /* PES_priority */
        bs_write(&bs,  1, 1);         /* data_alignment_indicator (aligned) */
        bs_write(&bs,  1, 0);         /* copyright (not-copyright) */
        bs_write(&bs,  1, 0);         /* original-or-copy (copy) */
        bs_write(&bs,  2, 2);         /* PTS_DTS_flags (PTS Present) */
        bs_write(&bs,  1, 0);         /* ESCR_flag (not present) */
        bs_write(&bs,  1, 0);         /* ES_RATE_flag (not present) */
        bs_write(&bs,  1, 0);         /* DSM_TRICK_MODE_flag (not present) */
        bs_write(&bs,  1, 0);         /* additional_copy_info_flag (not present) */
        bs_write(&bs,  1, 0);         /* PES_CRC_flag (not present) */
        bs_write(&bs,  1, 0);         /* PES_EXTENSION_flag (not present) */
        bs_write(&bs,  8, 0x24);      /* PES_HEADER_DATA_length */
        bs_write(&bs,  4, 2);         /* '0010' fixed value */

        int64_t pts = decklink_ctx->stream_time / 300 + (10 * 90000);

        bs_write(&bs,  3, (pts >> 30));           /* PTS[32:30] */
        bs_write(&bs,  1, 1);                     /* marker_bit */
        bs_write(&bs, 15, (pts >> 15) & 0x7fff);  /* PTS[29:15] */
        bs_write(&bs,  1, 1);                     /* marker_bit */
        bs_write(&bs, 15, (pts & 0x7fff));        /* PTS[14:0] */
        bs_write(&bs,  1, 1);                     /* marker_bit */

        for (int i = 0; i < 31; i++) {
            bs_write(&bs, 8, 0xff);               /* stuffing byte - Gets the PES header match spec 45 bytes */
        }

        /* See ETSI EN 301775 V1.2.1 page 8 Table 1. */
        bs_write(&bs, 8, 0x10);               /* data_identifier - EBU Data - EN400472 */
        cnt++;

	/* Process each OP-47 data_unit */
        for (int j = 0; j < 5; j++) {

            /* Skip any illegal / undefined lines */
            if (pkt->descriptors[j].line == 0)
                continue;

            uint8_t a[4];

#if 0
            for (int i = 0; i < 8; i++) {
                printf("%02x ", pkt->descriptors[j].data[i]);
            }
            printf("\n");
#endif

            /* We only support data_unit_id = 0x03 */
            bs_write(&bs, 8, 0x03);               /* data_unit_id - EBU Teletext subtitle data */
            bs_write(&bs, 8, 0x2c);               /* data_unit_length - fixed at 2c as per spec */

            /* txt_data_field */
            a[0] = 0x03 << 6 | (pkt->descriptors[j].field & 0x01) << 5 | (pkt->descriptors[j].line & 0x1f);
            a[1] = pkt->descriptors[j].data[2];  /* framing_code - EBU Teletext */
            a[2] = pkt->descriptors[j].data[3];  /* mag */
            a[3] = pkt->descriptors[j].data[4];  /* mag */

            if (g_decklink_op47_teletext_reverse) {
                for (int i = 0; i < 4; i++) {
                    a[i] = REVERSE[a[i]];    
                }            
            }

            /* Write the header */
            for (int i = 0; i < 4; i++) {
                bs_write(&bs, 8, a[i]);
            }

            /* And then the teletext 40 code body */
            for (int i = 0; i < 40; i++) {
                if (g_decklink_op47_teletext_reverse) {
                    pkt->descriptors[j].data[i + 5] = REVERSE[pkt->descriptors[j].data[i + 5]];
                }
                bs_write(&bs, 8, pkt->descriptors[j].data[i + 5]);  /* txt_data_block */
            }

            cnt += (6 + 40);
        }

        /* Now take care of stuffing_units to ensure we're closing the PES at the end of a
         * transport packet, when the mux finally packs it.
         */
        for (int i = 0; i < stuffing_units; i++) {
            bs_write(&bs, 8, 0xff);               /* data_unit_id - stuffing - section 4.4 */
            bs_write(&bs, 8, 0x2c);               /* fixed stuffing length to meet the 46 byte unit obligation */
            for (int j = 0; j < 0x2c; j++) {
                bs_write(&bs, 8, 0xff);           /* stuffing block - No need to reverse FF's */
            }
            cnt += (2 + 0x2c);
        }

        /* Close (actually its 'align') the bitstream buffer */
        bs_flush(&bs);

        if (cnt != PES_packet_length) {
            fprintf(stderr, PREFIX "Calculation error generating WST PES got %d wanted %d\n",
                cnt, PES_packet_length);
        }

        /* Correct the overall length in the header */
        buf[4] = cnt >> 8;
        buf[5] = cnt & 0xff;

        if (transmit_pes_to_muxer(decklink_ctx, buf, bs_pos(&bs) >> 3, SMPTE2031) < 0) {
            fprintf(stderr, "%s() failed to xmit SMPTE2031 PES to muxer\n", __func__);
        }
    }

    return 0;
}

static struct klvanc_callbacks_s callbacks = 
{
	.afd			= NULL,
	.eia_708b		= cb_EIA_708B,
	.eia_608		= cb_EIA_608,
	.scte_104		= cb_SCTE_104,
	.all			= cb_all,
	.kl_i64le_counter       = cb_VANC_TYPE_KL_UINT64_COUNTER,
	.sdp			= cb_SDP,
};
/* End: VANC Callbacks */

static void * detector_callback(void *user_context,
        struct smpte337_detector_s *ctx,
        uint8_t datamode, uint8_t datatype, uint32_t payload_bitCount, uint8_t *payload)
{
	struct audio_pair_s *pair = (struct audio_pair_s *)user_context;
#if 0
	decklink_ctx_t *decklink_ctx = pair->decklink_ctx;
        printf("%s() datamode = %d [%sbit], datatype = %d [payload: %s]"
                ", payload_bitcount = %d, payload = %p\n",
                __func__,
                datamode,
                datamode == 0 ? "16" :
                datamode == 1 ? "20" :
                datamode == 2 ? "24" : "Reserved",
                datatype,
                datatype == 1 ? "SMPTE338 / AC-3 (audio) data" : "TBD",
                payload_bitCount,
		payload);
#endif

	if (datatype == 1 /* AC3 */) {
		pair->smpte337_detected_ac3 = 1;
	} else
		fprintf(stderr, "[decklink] Detected datamode %d on pair %d, we don't support it.",
			datamode,
			pair->nr);

        return 0;
}

static int open_card( decklink_opts_t *decklink_opts, int allowFormatDetection)
{
    decklink_ctx_t *decklink_ctx = &decklink_opts->decklink_ctx;
    int         found_mode;
    int         ret = 0;
    int         i;
    const int   sample_rate = 48000;
    BMDDisplayMode wanted_mode_id;
    BMDDisplayMode start_mode_id = bmdModeNTSC;
#if BLACKMAGIC_DECKLINK_API_VERSION < 0x0c090000 /* 12.9 */
    IDeckLinkAttributes *decklink_attributes = NULL;
#endif
    uint32_t    flags = 0;
    bool        supported;

    const struct obe_to_decklink_video *fmt = NULL;
    IDeckLinkDisplayModeIterator *p_display_iterator = NULL;
    IDeckLinkIterator *decklink_iterator = NULL;
    HRESULT result;
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a080500 /* 10.8.5 */
#if BLACKMAGIC_DECKLINK_API_VERSION < 0x0c090000 /* 12.9 */
    IDeckLinkStatus *status = NULL;
#endif
#endif

    /* Avoid compilier bug that throws a spurious warning because it thinks fmt
     * is never used.
     */
    if (fmt == NULL) {
        i = 0;
    }

    if (klvanc_context_create(&decklink_ctx->vanchdl) < 0) {
        fprintf(stderr, "[decklink] Error initializing VANC library context\n");
    } else {
        decklink_ctx->vanchdl->verbose = 0;
        decklink_ctx->vanchdl->callbacks = &callbacks;
        decklink_ctx->vanchdl->callback_context = decklink_ctx;
        decklink_ctx->vanchdl->allow_bad_checksums = 1;
        decklink_ctx->last_vanc_cache_dump = 0;

        if (OPTION_ENABLED(vanc_cache)) {
            /* Turn on the vanc cache, we'll want to query it later. */
            decklink_ctx->last_vanc_cache_dump = 1;
            fprintf(stdout, "Enabling option VANC CACHE, interval %d seconds\n", VANC_CACHE_DUMP_INTERVAL);
            klvanc_context_enable_cache(decklink_ctx->vanchdl);
        }
    }

    if (OPTION_ENABLED(frame_injection)) {
        klsyslog_and_stdout(LOG_INFO, "Enabling option frame injection");
        g_decklink_inject_frame_enable = 1;
    }

    if (OPTION_ENABLED(allow_1080p60)) {
        klsyslog_and_stdout(LOG_INFO, "Enabling option 1080p60");
    }

    if (decklink_ctx->h->enable_timecode) {
        klsyslog_and_stdout(LOG_INFO, "Enabling option 'Timecode'");
    }

    if (decklink_ctx->h->enable_scte35) {
        klsyslog_and_stdout(LOG_INFO, "Enabling option SCTE35 with %d streams", decklink_ctx->h->enable_scte35);
    } else
	callbacks.scte_104 = NULL;

    if (OPTION_ENABLED(smpte2038)) {
        klsyslog_and_stdout(LOG_INFO, "Enabling option SMPTE2038");
        if (klvanc_smpte2038_packetizer_alloc(&decklink_ctx->smpte2038_ctx) < 0) {
            fprintf(stderr, "Unable to allocate a SMPTE2038 context.\n");
        }
    }

    if (OPTION_ENABLED(smpte2031)) {
        klsyslog_and_stdout(LOG_INFO, "Enabling option SMPTE2031");
    } else {
    	callbacks.sdp = NULL;
    }

    ltn_histogram_alloc_video_defaults(&decklink_ctx->callback_hdl, "frame arrival latency");
    ltn_histogram_alloc_video_defaults(&decklink_ctx->callback_duration_hdl, "frame processing time");
    ltn_histogram_alloc_video_defaults(&decklink_ctx->callback_1_hdl, "frame section1 time");
    ltn_histogram_alloc_video_defaults(&decklink_ctx->callback_2_hdl, "frame section2 time");
    ltn_histogram_alloc_video_defaults(&decklink_ctx->callback_3_hdl, "frame section3 time");
    ltn_histogram_alloc_video_defaults(&decklink_ctx->callback_4_hdl, "frame section4 time");

    if (ltnsdi_context_alloc(&decklink_ctx->sdi_audio_analyzer_ctx) < 0) {
        klsyslog_and_stdout(LOG_INFO, "Enabling SDI Audio analyzer");
    }

    for (int i = 0; i < MAX_AUDIO_PAIRS; i++) {
        struct audio_pair_s *pair = &decklink_ctx->audio_pairs[i];

        pair->nr = i;
        pair->smpte337_detected_ac3 = 0;
        pair->decklink_ctx = decklink_ctx;
        pair->input_stream_id = i + 1; /* Video is zero, audio onwards. */

        if (OPTION_ENABLED(bitstream_audio)) {
            pair->smpte337_detector = smpte337_detector_alloc((smpte337_detector_callback)detector_callback, pair);
        } else {
            pair->smpte337_frames_written = 256;
        }
    }

    decklink_ctx->dec = avcodec_find_decoder( AV_CODEC_ID_V210 );
    if( !decklink_ctx->dec )
    {
        fprintf( stderr, "[decklink] Could not find v210 decoder\n" );
        goto finish;
    }

    decklink_ctx->codec = avcodec_alloc_context3( decklink_ctx->dec );
    if( !decklink_ctx->codec )
    {
        fprintf( stderr, "[decklink] Could not allocate AVCodecContext\n" );
        goto finish;
    }

    decklink_ctx->codec->get_buffer2 = obe_get_buffer2;
#if 0
    decklink_ctx->codec->release_buffer = obe_release_buffer;
    decklink_ctx->codec->reget_buffer = obe_reget_buffer;
    decklink_ctx->codec->flags |= CODEC_FLAG_EMU_EDGE;
#endif

    /* TODO: setup custom strides */
    if( avcodec_open2( decklink_ctx->codec, decklink_ctx->dec, NULL ) < 0 )
    {
        fprintf( stderr, "[decklink] Could not open libavcodec\n" );
        goto finish;
    }

    decklink_iterator = CreateDeckLinkIteratorInstance();
    if( !decklink_iterator )
    {
        fprintf( stderr, "[decklink] DeckLink drivers not found\n" );
        ret = -1;
        goto finish;
    }

    if( decklink_opts->card_idx < 0 )
    {
        fprintf( stderr, "[decklink] Invalid card index %d \n", decklink_opts->card_idx );
        ret = -1;
        goto finish;
    }

    for( i = 0; i <= decklink_opts->card_idx; ++i )
    {
        if( decklink_ctx->p_card )
            decklink_ctx->p_card->Release();
        result = decklink_iterator->Next( &decklink_ctx->p_card );
        if( result != S_OK )
            break;
    }

    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] DeckLink PCI card %d not found\n", decklink_opts->card_idx );
        ret = -1;
        goto finish;
    }

#if defined(__linux__)
    const char *model_name;
    result = decklink_ctx->p_card->GetModelName( &model_name );

    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Could not get model name\n" );
        ret = -1;
        goto finish;
    }

    syslog( LOG_INFO, "Opened DeckLink PCI card %d (%s)", decklink_opts->card_idx, model_name );
    free( (char *)model_name );
#endif

    if( decklink_ctx->p_card->QueryInterface( IID_IDeckLinkInput, (void**)&decklink_ctx->p_input ) != S_OK )
    {
        fprintf( stderr, "[decklink] Card has no inputs\n" );
        ret = -1;
        goto finish;
    }


    decklink_ctx->isHalfDuplex = 0;
#if BLACKMAGIC_DECKLINK_API_VERSION >= 0x0a080500 /* 10.8.5 */
    /* The half duplex detection is used by other parts of the stack
     * to help assist with audio/video sync issues during loss of signal conditions.
     * For 12.9 we're going to assume no issues exist, and if they do we'll discover them
     * doing testing and come back here to investigate/remedy anyway.
     * So for 12.9, we're going to assume full duplex timing behaviour by default.
     */
#if BLACKMAGIC_DECKLINK_API_VERSION < 0x0c090000 /* 12.9 */
    if (decklink_ctx->p_card->QueryInterface(IID_IDeckLinkStatus, (void**)&status) == S_OK) {
        int64_t ds = bmdDuplexStatusFullDuplex;
        if (status->GetInt(bmdDeckLinkStatusDuplexMode, &ds) == S_OK) {
        }
        status->Release();
        if (ds == bmdDuplexStatusFullDuplex) {
            decklink_ctx->isHalfDuplex = 0;
        } else
        if (ds == bmdDuplexStatusHalfDuplex) {
            decklink_ctx->isHalfDuplex = 1;
        } else {
            decklink_ctx->isHalfDuplex = 0;
        }
    }
#endif
#endif

    /* Set up the video and audio sources. */
    if( decklink_ctx->p_card->QueryInterface( IID_IDeckLinkConfiguration, (void**)&decklink_ctx->p_config ) != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to get configuration interface\n" );
        ret = -1;
        goto finish;
    }

    /* Setup video connection */
    for( i = 0; video_conn_tab[i].obe_name != -1; i++ )
    {
        if( video_conn_tab[i].obe_name == decklink_opts->video_conn )
            break;
    }

    if( video_conn_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[decklink] Unsupported video input connection\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_ctx->p_config->SetInt( bmdDeckLinkConfigVideoInputConnection, video_conn_tab[i].bmd_name );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to set video input connection\n" );
        ret = -1;
        goto finish;
    }

#if BLACKMAGIC_DECKLINK_API_VERSION < 0x0c090000 /* 12.9 */
    result = decklink_ctx->p_card->QueryInterface(IID_IDeckLinkAttributes, (void**)&decklink_attributes );
    if( result != S_OK )
    {
        fprintf(stderr, "[decklink] Could not obtain the IDeckLinkAttributes interface\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_attributes->GetFlag( BMDDeckLinkSupportsInputFormatDetection, &supported );
    if( result != S_OK )
    {
        fprintf(stderr, "[decklink] Could not query card for format detection\n" );
        ret = -1;
        goto finish;
    }
#else
    /* Format detction is known to work with 10.8 and higher. For 12.9 we're just going
     * to flag this as supported and not check.
     * If for some reason format detection proves unreluable or is a regression, we'll 
     * discover this during testing and revisit.
     */
    supported = allowFormatDetection;
#endif
    if (supported && allowFormatDetection)
        flags = bmdVideoInputEnableFormatDetection;

    /* Get the list of display modes. */
    result = decklink_ctx->p_input->GetDisplayModeIterator( &p_display_iterator );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enumerate display modes\n" );
        ret = -1;
        goto finish;
    }

    if (decklink_opts->video_format == INPUT_VIDEO_FORMAT_UNDEFINED && decklink_opts->probe) {
        decklink_opts->video_format = INPUT_VIDEO_FORMAT_NTSC;
    }
    fmt = getVideoFormatByOBEName(decklink_opts->video_format);

    //printf("%s() decklink_opts->video_format = %d %s\n", __func__,
    //    decklink_opts->video_format, getModeName(fmt->bmd_name));
    for( i = 0; video_format_tab[i].obe_name != -1; i++ )
    {
        if( video_format_tab[i].obe_name == decklink_opts->video_format )
            break;
    }

    if( video_format_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[decklink] Unsupported video format\n" );
        ret = -1;
        goto finish;
    }

    wanted_mode_id = video_format_tab[i].bmd_name;
    found_mode = false;
    decklink_opts->timebase_num = video_format_tab[i].timebase_num;
    decklink_opts->timebase_den = video_format_tab[i].timebase_den;
    calculate_audio_sfc_window(decklink_opts);

    for (;;)
    {
        IDeckLinkDisplayMode *p_display_mode;
        result = p_display_iterator->Next( &p_display_mode );
        if( result != S_OK || !p_display_mode )
            break;

        BMDDisplayMode mode_id = p_display_mode->GetDisplayMode();

        BMDTimeValue frame_duration, time_scale;
        result = p_display_mode->GetFrameRate( &frame_duration, &time_scale );
        if( result != S_OK )
        {
            fprintf( stderr, "[decklink] Failed to get frame rate\n" );
            ret = -1;
            p_display_mode->Release();
            goto finish;
        }

        if( wanted_mode_id == mode_id )
        {
            found_mode = true;
            get_format_opts( decklink_opts, p_display_mode );
            setup_pixel_funcs( decklink_opts );
        }

        p_display_mode->Release();
    }

    if( !found_mode )
    {
        fprintf( stderr, "[decklink] Unsupported video mode\n" );
        ret = -1;
        goto finish;
    }

    /* Setup audio connection */
    for( i = 0; audio_conn_tab[i].obe_name != -1; i++ )
    {
        if( audio_conn_tab[i].obe_name == decklink_opts->audio_conn )
            break;
    }

    if( audio_conn_tab[i].obe_name == -1 )
    {
        fprintf( stderr, "[decklink] Unsupported audio input connection\n" );
        ret = -1;
        goto finish;
    }

    result = decklink_ctx->p_config->SetInt( bmdDeckLinkConfigAudioInputConnection, audio_conn_tab[i].bmd_name );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to set audio input connection\n" );
        ret = -1;
        goto finish;
    }

    decklink_ctx->enabled_mode_id = wanted_mode_id;
    decklink_ctx->enabled_mode_fmt = getVideoFormatByMode(decklink_ctx->enabled_mode_id);

#if 0
// MMM
    result = decklink_ctx->p_input->EnableVideoInput(decklink_ctx->enabled_mode_id, bmdFormat10BitYUV, flags);
    printf("%s() startup. calling enable video with startup mode %s flags 0x%x\n", __func__,
        getModeName(decklink_ctx->enabled_mode_id), flags);
#else
    /* Probe for everything in PAL mode, unless the user wants to start in PAL mode, then
     * configure HW for 1080P60 and let detection take care of things.
     */
    if (decklink_ctx->enabled_mode_id == start_mode_id) {
        start_mode_id = bmdModeHD1080p2398;
    }
    //printf("%s() startup. calling enable video with startup mode %s flags 0x%x\n", __func__, getModeName(start_mode_id), flags);
    result = decklink_ctx->p_input->EnableVideoInput(start_mode_id, bmdFormat10BitYUV, flags);
#endif

    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enable video input\n" );
        ret = -1;
        goto finish;
    }

    /* Set up audio. */
    result = decklink_ctx->p_input->EnableAudioInput( sample_rate, bmdAudioSampleType32bitInteger, decklink_opts->num_channels );
    if( result != S_OK )
    {
        fprintf( stderr, "[decklink] Failed to enable audio input\n" );
        ret = -1;
        goto finish;
    }

    if( !decklink_opts->probe )
    {
        decklink_ctx->avr = swr_alloc();
        if (!decklink_ctx->avr)
        {
            fprintf(stderr, PREFIX "Could not alloc libswresample context\n");
            ret = -1;
            goto finish;
        }

        /* Give libswresample a made up channel map */
        av_opt_set_int( decklink_ctx->avr, "in_channel_layout",   (1 << decklink_opts->num_channels) - 1, 0 );
        av_opt_set_int( decklink_ctx->avr, "in_sample_fmt",       AV_SAMPLE_FMT_S32, 0 );
        av_opt_set_int( decklink_ctx->avr, "in_sample_rate",      48000, 0 );
        av_opt_set_int( decklink_ctx->avr, "out_channel_layout",  (1 << decklink_opts->num_channels) - 1, 0 );
        av_opt_set_int( decklink_ctx->avr, "out_sample_fmt",      AV_SAMPLE_FMT_S32P, 0 );
        av_opt_set_int( decklink_ctx->avr, "out_sample_rate",     48000, 0 );

        if (swr_init(decklink_ctx->avr) < 0)
        {
            fprintf(stderr, PREFIX "couldn't setup sample rate conversion\n");
            goto finish;
        }
    }

    decklink_ctx->p_delegate = new DeckLinkCaptureDelegate( decklink_opts );
    decklink_ctx->p_input->SetCallback( decklink_ctx->p_delegate );

    result = decklink_ctx->p_input->StartStreams();
    if( result != S_OK )
    {
        fprintf(stderr, PREFIX "Could not start streaming from card\n");
        ret = -1;
        goto finish;
    }

    ret = 0;

finish:
    if( decklink_iterator )
        decklink_iterator->Release();

    if( p_display_iterator )
        p_display_iterator->Release();

#if BLACKMAGIC_DECKLINK_API_VERSION < 0x0c090000 /* 12.9 */
    if( decklink_attributes )
        decklink_attributes->Release();
#endif

    if( ret )
        close_card( decklink_opts );

    return ret;
}

static void close_thread( void *handle )
{
    struct decklink_status *status = (decklink_status *)handle;

    if( status->decklink_opts )
    {
        close_card( status->decklink_opts );
        free( status->decklink_opts );
    }

    free( status->input );
}

static void *probe_stream( void *ptr )
{
    obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
    obe_t *h = probe_ctx->h;
    obe_input_t *user_opts = &probe_ctx->user_opts;
    obe_device_t *device;
    obe_int_input_stream_t *streams[MAX_STREAMS];
    int cur_stream = 0;
    obe_sdi_non_display_data_t *non_display_parser;
    decklink_ctx_t *decklink_ctx;
    const struct obe_to_decklink_video *fmt = NULL;

    decklink_opts_t *decklink_opts = (decklink_opts_t*)calloc( 1, sizeof(*decklink_opts) );
    if( !decklink_opts )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto finish;
    }

    non_display_parser = &decklink_opts->decklink_ctx.non_display_parser;

    /* TODO: support multi-channel */
    decklink_opts->num_channels = 16;
    decklink_opts->card_idx = user_opts->card_idx;
    decklink_opts->video_conn = user_opts->video_connection;
    decklink_opts->audio_conn = user_opts->audio_connection;
    decklink_opts->video_format = user_opts->video_format;
    decklink_opts->enable_smpte2038 = user_opts->enable_smpte2038;
    decklink_opts->enable_smpte2031 = user_opts->enable_smpte2031;
    decklink_opts->enable_vanc_cache = user_opts->enable_vanc_cache;
    decklink_opts->enable_bitstream_audio = user_opts->enable_bitstream_audio;
    decklink_opts->enable_patch1 = user_opts->enable_patch1;
    decklink_opts->enable_los_exit_ms = user_opts->enable_los_exit_ms;
    decklink_opts->enable_frame_injection = user_opts->enable_frame_injection;
    decklink_opts->enable_allow_1080p60 = user_opts->enable_allow_1080p60;

    decklink_opts->probe = non_display_parser->probe = 1;

    decklink_ctx = &decklink_opts->decklink_ctx;
    decklink_ctx->h = h;
    decklink_ctx->last_frame_time = -1;

    if (open_card(decklink_opts, 1) < 0)
        goto finish;

    /* Wait for up to 10 seconds, checking for a probe success every 100ms.
     * Avoid issues with some links where probe takes an unusually long time.
     */
    for (int z = 0; z < 10 * 10; z++) {
        usleep(100 * 1000);
        if (decklink_opts->probe_success)
            break;
    }

    close_card( decklink_opts );

    if( !decklink_opts->probe_success )
    {
        fprintf( stderr, "[decklink] No valid frames received - check connection and input format\n" );
        goto finish;
    }

    /* Store the detected signal conditions in the user props, because OBE
     * will use those then the stream starts. We then no longer have a race
     * condition because the probe detected one format and the auto-mode
     * during start would detect another. By telling OBE to startup in the mode
     * we probed, if OBE dectects another format - the frames are discarded
     * we no attempt is made to compress in the probe resolution with the startup
     * resolution.
     */
    user_opts->video_format = decklink_opts->video_format;
    fmt = getVideoFormatByOBEName(user_opts->video_format);
    printf("%s() Detected signal: user_opts->video_format = %d %s\n", __func__, 
        user_opts->video_format, getModeName(fmt->bmd_name));

#define ALLOC_STREAM(nr) \
    streams[cur_stream] = (obe_int_input_stream_t*)calloc(1, sizeof(*streams[cur_stream])); \
    if (!streams[cur_stream]) goto finish;

    ALLOC_STREAM(cur_stream);
    pthread_mutex_lock(&h->device_list_mutex);
    streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
    pthread_mutex_unlock(&h->device_list_mutex);

    streams[cur_stream]->stream_type = STREAM_TYPE_VIDEO;
    streams[cur_stream]->stream_format = VIDEO_UNCOMPRESSED;
    streams[cur_stream]->width  = decklink_opts->width;
    streams[cur_stream]->height = decklink_opts->height;
    streams[cur_stream]->timebase_num = decklink_opts->timebase_num;
    streams[cur_stream]->timebase_den = decklink_opts->timebase_den;
    streams[cur_stream]->csp    = AV_PIX_FMT_YUV422P10;
    streams[cur_stream]->interlaced = decklink_opts->interlaced;
    streams[cur_stream]->tff = 1; /* NTSC is bff in baseband but coded as tff */
    streams[cur_stream]->sar_num = streams[cur_stream]->sar_den = 1; /* The user can choose this when encoding */

    if (add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_FRAME) < 0)
        goto finish;
    cur_stream++;

    for (int i = 0; i < MAX_AUDIO_PAIRS; i++) {
        struct audio_pair_s *pair = &decklink_ctx->audio_pairs[i];

        ALLOC_STREAM(cur_stream);
        streams[cur_stream]->sdi_audio_pair = i + 1;

        pthread_mutex_lock(&h->device_list_mutex);
        streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock(&h->device_list_mutex);

        if (!pair->smpte337_detected_ac3)
        {
            streams[cur_stream]->stream_type = STREAM_TYPE_AUDIO;
            streams[cur_stream]->stream_format = AUDIO_PCM;
            streams[cur_stream]->num_channels  = 2;
            streams[cur_stream]->sample_format = AV_SAMPLE_FMT_S32P;
            /* TODO: support other sample rates */
            streams[cur_stream]->sample_rate = 48000;
        } else {

            streams[cur_stream]->stream_type = STREAM_TYPE_AUDIO;
            streams[cur_stream]->stream_format = AUDIO_AC_3_BITSTREAM;

            /* In reality, the muxer inspects the bistream for these details before constructing a descriptor.
             * We expose it here show the probe message on the console are a little more reasonable.
             * TODO: Fill out sample_rate and bitrate from the SMPTE337 detector.
             */
            streams[cur_stream]->sample_rate = 48000;
            streams[cur_stream]->bitrate = 384;
            streams[cur_stream]->pid = 0x124; /* TODO: hardcoded PID not currently used. */
            if (add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM) < 0)
                goto finish;
        }
        cur_stream++;
    } /* For all audio pairs.... */

    /* Add a new output stream type, a TABLE_SECTION mechanism.
     * We use this to pass DVB table sections direct to the muxer,
     * for SCTE35, and other sections in the future.
     */
    if (decklink_ctx->h->enable_scte35) {
        int i = decklink_ctx->h->enable_scte35;
        while (i-- != 0)
        {
            /* Create N SCTE35 streams */
            ALLOC_STREAM(cur_stream);

            pthread_mutex_lock(&h->device_list_mutex);
            streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
            pthread_mutex_unlock(&h->device_list_mutex);

            streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
            streams[cur_stream]->stream_format = DVB_TABLE_SECTION;
            streams[cur_stream]->pid = 0x130 + i; /* TODO: hardcoded PID not currently used. */
            if(add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM) < 0 )
                goto finish;
            cur_stream++;
        }
    }

    /* Add a new output stream type, a SCTE2038 mechanism.
     * We use this to pass PES direct to the muxer.
     */
    if (OPTION_ENABLED(smpte2038))
    {
        ALLOC_STREAM(cur_stream);

        pthread_mutex_lock(&h->device_list_mutex);
        streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock(&h->device_list_mutex);

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = SMPTE2038;
        streams[cur_stream]->pid = 0x124; /* TODO: hardcoded PID not currently used. */
        if(add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM) < 0 )
            goto finish;
        cur_stream++;
    }

    /* Add a new output stream type, a SMPTE2031 / EN301775 mechanism.
     * We use this to pass PES direct to the muxer.
     */
    if (OPTION_ENABLED(smpte2031))
    {
        ALLOC_STREAM(cur_stream);

        pthread_mutex_lock(&h->device_list_mutex);
        streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock(&h->device_list_mutex);

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = SMPTE2031;
        streams[cur_stream]->pid = 0x125; /* TODO: hardcoded PID not currently used. */
#if 0
        streams[cur_stream]->lang_code[0] = 'e';
        streams[cur_stream]->lang_code[1] = 'n';
        streams[cur_stream]->lang_code[2] = 'g';
        streams[cur_stream]->lang_code[3] = 0;
        streams[cur_stream]->dvb_teletext_type = DVB_TTX_TYPE_SUB;
        streams[cur_stream]->dvb_teletext_magazine_number = 0x8;
        streams[cur_stream]->dvb_teletext_page_number = 0x88;
#endif
        if(add_non_display_services(non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM) < 0 )
            goto finish;
        cur_stream++;
    }

    if( non_display_parser->has_vbi_frame )
    {
        ALLOC_STREAM(cur_stream);

        pthread_mutex_lock( &h->device_list_mutex );
        streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock( &h->device_list_mutex );

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = VBI_RAW;
        streams[cur_stream]->vbi_ntsc = decklink_opts->video_format == INPUT_VIDEO_FORMAT_NTSC;
        if( add_non_display_services( non_display_parser, streams[cur_stream], USER_DATA_LOCATION_DVB_STREAM ) < 0 )
            goto finish;
        cur_stream++;
    }

    if( non_display_parser->has_ttx_frame )
    {
        ALLOC_STREAM(cur_stream);

        pthread_mutex_lock( &h->device_list_mutex );
        streams[cur_stream]->input_stream_id = h->cur_input_stream_id++;
        pthread_mutex_unlock( &h->device_list_mutex );

        streams[cur_stream]->stream_type = STREAM_TYPE_MISC;
        streams[cur_stream]->stream_format = MISC_TELETEXT;
        if( add_teletext_service( non_display_parser, streams[cur_stream] ) < 0 )
            goto finish;
        cur_stream++;
    }

    if( non_display_parser->num_frame_data )
        free( non_display_parser->frame_data );

    device = new_device();

    if( !device )
        goto finish;

    device->num_input_streams = cur_stream;
    memcpy(device->input_streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**) );
    device->device_type = INPUT_DEVICE_DECKLINK;
    memcpy( &device->user_opts, user_opts, sizeof(*user_opts) );

    /* Upstream is responsible for freeing streams[x] allocations */

    /* add device */
    add_device( h, device );

finish:
    if( decklink_opts )
        free( decklink_opts );

    free( probe_ctx );

    return NULL;
}

static void *open_input( void *ptr )
{
    obe_input_params_t *input = (obe_input_params_t*)ptr;
    obe_t *h = input->h;
    obe_device_t *device = input->device;
    obe_input_t *user_opts = &device->user_opts;
    decklink_ctx_t *decklink_ctx;
    obe_sdi_non_display_data_t *non_display_parser;
    struct decklink_status status;

    decklink_opts_t *decklink_opts = (decklink_opts_t*)calloc( 1, sizeof(*decklink_opts) );
    if( !decklink_opts )
    {
        fprintf( stderr, "Malloc failed\n" );
        return NULL;
    }

    status.input = input;
    status.decklink_opts = decklink_opts;
    pthread_cleanup_push( close_thread, (void*)&status );

    decklink_opts->num_channels = 16;
    decklink_opts->card_idx = user_opts->card_idx;
    decklink_opts->video_conn = user_opts->video_connection;
    decklink_opts->audio_conn = user_opts->audio_connection;
    decklink_opts->video_format = user_opts->video_format;
    //decklink_opts->video_format = INPUT_VIDEO_FORMAT_PAL;
    decklink_opts->enable_smpte2038 = user_opts->enable_smpte2038;
    decklink_opts->enable_smpte2031 = user_opts->enable_smpte2031;
    decklink_opts->enable_vanc_cache = user_opts->enable_vanc_cache;
    decklink_opts->enable_bitstream_audio = user_opts->enable_bitstream_audio;
    decklink_opts->enable_patch1 = user_opts->enable_patch1;
    decklink_opts->enable_los_exit_ms = user_opts->enable_los_exit_ms;
    decklink_opts->enable_allow_1080p60 = user_opts->enable_allow_1080p60;

    decklink_ctx = &decklink_opts->decklink_ctx;

    decklink_ctx->device = device;
    decklink_ctx->h = h;
    decklink_ctx->last_frame_time = -1;

    non_display_parser = &decklink_ctx->non_display_parser;
    non_display_parser->device = device;

    /* TODO: wait for encoder */

    if (open_card(decklink_opts, 1) < 0)
        return NULL;

    sleep( INT_MAX );

    pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_input_func_t decklink_input = { probe_stream, open_input };
