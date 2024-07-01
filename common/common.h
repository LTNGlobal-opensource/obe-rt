/*****************************************************************************
 * common.h: OBE common headers and structures
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
 *****************************************************************************/

#ifndef OBE_COMMON_H
#define OBE_COMMON_H

#define AUDIO_DEBUG_ENABLE 0
#if AUDIO_DEBUG_ENABLE
#pragma message "Remove before flight - AUDIO_DEBUG_ENABLE is active"
#endif

#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
#include <libavutil/common.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stddef.h>
#include <errno.h>
#include <syslog.h>
#include <sys/time.h>
#include <time.h>
#include "obe.h"
#include "stream_formats.h"
#include <common/queue.h>
#include <common/metadata.h>

/* Enable some realtime debugging commands */
#define DO_SET_VARIABLE 1

#define MAX_DEVICES 1
#define MAX_STREAMS 40
#define MAX_CHANNELS 16

#define MIN_PROBE_TIME  5
#define MAX_PROBE_TIME 20

#define OBE_CLOCK 27000000LL

/* Macros */
#define BOOLIFY(x) x = !!x
#define MIN(a,b) ( (a)<(b) ? (a) : (b) )
#define MAX(a,b) ( (a)>(b) ? (a) : (b) )

#define IS_SD(x) ((x) == INPUT_VIDEO_FORMAT_PAL || (x) == INPUT_VIDEO_FORMAT_NTSC)
#define IS_INTERLACED(x) (IS_SD(x) || (x) == INPUT_VIDEO_FORMAT_1080I_50 || \
                          (x) == INPUT_VIDEO_FORMAT_1080I_5994 || (x) == INPUT_VIDEO_FORMAT_1080I_60)
#define IS_PROGRESSIVE(x) (!IS_INTERLACED(x))

/* Audio formats */
#define AC3_NUM_SAMPLES 1536
#define MP2_NUM_SAMPLES 1152
#define AAC_NUM_SAMPLES 1024

/* T-STD buffer sizes */
#define AC3_BS_ATSC     2592
#define AC3_BS_DVB      5696
#define MISC_AUDIO_BS   3584

/* Audio sample patterns */
#define MAX_AUDIO_SAMPLE_PATTERN 5

/* NTSC */
#define NTSC_FIRST_CODED_LINE 23

static inline int obe_clip3( int v, int i_min, int i_max )
{
    return ( (v < i_min) ? i_min : (v > i_max) ? i_max : v );
}

typedef void *hnd_t;

typedef struct
{
    int dvb_subtitling_type;
    int composition_page_id;
    int ancillary_page_id;
} obe_dvb_sub_info_t;

typedef struct
{
    int input_stream_id;
    int stream_type;
    int stream_format;

    /** libavformat **/
    int lavf_stream_idx;

    char lang_code[4];

    char *transport_desc_text;
    char *codec_desc_text;

    /* Timebase of transport */
    int transport_timebase_num;
    int transport_timebase_den;

    /* Timebase of codec */
    int timebase_num;
    int timebase_den;

    /* MPEG-TS */
    int pid;
    int ts_stream_id;
    int has_stream_identifier;
    int stream_identifier;
    int audio_type;

    /** Video **/
    enum AVPixelFormat csp;
    int width;
    int height;
    int sar_num;
    int sar_den;
    int interlaced;
    int tff;

    /* Per-frame Data */
    int num_frame_data;
    obe_frame_data_t *frame_data;

    /** Audio **/
    uint64_t channel_layout;
    int num_channels; /* set if channel layout is 0 */
    int sample_rate;
    int sdi_audio_pair; /* 1-8 */

    /* Raw Audio */
    int sample_format;

    /* Compressed Audio */
    int bitrate;

    /* AAC */
    int aac_profile_and_level;
    int aac_type;
    int is_latm;

    /** Subtitles **/
    /* DVB */
    int has_dds;
    int dvb_subtitling_type;
    int composition_page_id;
    int ancillary_page_id;

    /** DVB Teletext **/
    int dvb_teletext_type;
    int dvb_teletext_magazine_number;
    int dvb_teletext_page_number;

    /* VBI */
    int vbi_ntsc;
    int is_hdr;
} obe_int_input_stream_t;

typedef struct
{
    int device_type;

    obe_input_t user_opts;

    /* MPEG-TS */
    int program_num;
    int ts_id;
    int pmt_pid;
    int pcr_pid;

    pthread_mutex_t device_mutex;
    pthread_t device_thread;

    int num_input_streams;
    obe_int_input_stream_t *input_streams[MAX_STREAMS];

    obe_input_stream_t *probed_streams;
} obe_device_t;

typedef struct
{
    enum AVPixelFormat csp;       /* colorspace */
    int     width;     /* width of the picture */
    int     height;    /* height of the picture */
    int     planes;    /* number of planes */
    uint8_t *plane[4]; /* pointers for each plane */
    int     stride[4]; /* strides for each plane */
    int     format;    /* image format Eg: INPUT_VIDEO_FORMAT_1080I_5994 */
    int     first_line; /* first line of image (SD from SDI only) */
} obe_image_t;
#define PRINT_OBE_IMAGE(i, prefix) { \
	printf("%s: obj = %p, w=%d h=%d planes=%d csp=%d [%s] fmt=%d fl=%d\n", \
		prefix, i, (i)->width, (i)->height, (i)->planes, (i)->csp, \
		(i)->csp == PIX_FMT_YUV420P   ? "PIX_FMT_YUV420P" : \
		(i)->csp == PIX_FMT_YUV420P10 ? "PIX_FMT_YUV420P10" : \
		(i)->csp == PIX_FMT_YUV422P   ? "PIX_FMT_YUV422P" : \
		(i)->csp == PIX_FMT_YUV422P10 ? "PIX_FMT_YUV422P10" : "UNDEFINED", \
		(i)->format, (i)->first_line); \
};

typedef struct
{
     int type;  /* For VBI (including VANC VBI) during input this uses stream_formats_e */
     int source;
     int num_lines;
     int lines[100];
     int location;
} obe_int_frame_data_t;

typedef struct
{
    uint8_t *audio_data[MAX_CHANNELS];
    int      linesize;
    uint64_t channel_layout; /* If this is zero then num_channels is set */
    int      num_channels;
    int      num_samples;
    int      sample_fmt;
} obe_audio_frame_t;

enum user_data_types_e
{
    /* Encapsulated frame data formats */
    USER_DATA_AVC_REGISTERED_ITU_T35 = 4,
    USER_DATA_AVC_UNREGISTERED,

    /* Encapsulated stream data formats */
    USER_DATA_DVB_VBI                = 32, /* VBI packets */

    /* Raw data formats */
    USER_DATA_CEA_608                = 64, /* Raw CEA-608 data 4 bytes, 2 bytes per field */
    USER_DATA_CEA_708_CDP,                 /* CEA-708 Caption Distribution Packet */
    USER_DATA_AFD,                         /* AFD word 1 from SMPTE 2016-3 */
    USER_DATA_BAR_DATA,                    /* Bar Data 5 words from SMPTE 2016-3 */
    USER_DATA_WSS,                         /* 3 bits of WSS to be converted to AFD */
};

enum user_data_location_e
{
    USER_DATA_LOCATION_FRAME,      /* e.g. AFD, Captions */
    USER_DATA_LOCATION_DVB_STREAM, /* e.g. DVB-VBI, DVB-TTX */
    /* TODO: various other minor locations */
};

typedef struct
{
    int service;
    int location;
} obe_non_display_data_location_t;

const static obe_non_display_data_location_t non_display_data_locations[] =
{
    { MISC_TELETEXT,    USER_DATA_LOCATION_DVB_STREAM },
    { MISC_TELETEXT_INVERTED, USER_DATA_LOCATION_DVB_STREAM },
    { MISC_WSS,         USER_DATA_LOCATION_DVB_STREAM },
    { MISC_VPS,         USER_DATA_LOCATION_DVB_STREAM },
    { CAPTIONS_CEA_608, USER_DATA_LOCATION_FRAME },
    { CAPTIONS_CEA_708, USER_DATA_LOCATION_FRAME },
    { MISC_AFD,         USER_DATA_LOCATION_FRAME },
    { MISC_BAR_DATA,    USER_DATA_LOCATION_FRAME },
    { MISC_PAN_SCAN,    USER_DATA_LOCATION_FRAME },
    { VBI_AMOL_48,      USER_DATA_LOCATION_DVB_STREAM },
    { VBI_AMOL_96,      USER_DATA_LOCATION_DVB_STREAM },
    { VBI_NABTS,        USER_DATA_LOCATION_DVB_STREAM },
    { VBI_TVG2X,        USER_DATA_LOCATION_DVB_STREAM },
    { VBI_CP,           USER_DATA_LOCATION_DVB_STREAM },
    /* Does VITC go in the codec or in the VBI? */
    { -1, -1 },
};

typedef struct
{
    int format;
    int pattern[MAX_AUDIO_SAMPLE_PATTERN];
    int max;
} obe_audio_sample_pattern_t;

const static obe_audio_sample_pattern_t audio_sample_patterns[] =
{
    { INPUT_VIDEO_FORMAT_NTSC,       { 1602, 1601, 1602, 1601, 1602 }, 1602 },
    { INPUT_VIDEO_FORMAT_720P_5994,  {  801,  800,  801,  801,  801 },  801 },
    { INPUT_VIDEO_FORMAT_1080P_2997, { 1602, 1601, 1602, 1601, 1602 }, 1602 },
    { INPUT_VIDEO_FORMAT_1080P_5994, {  801,  800,  801,  801,  801 },  801 },
    { -1 },
};

typedef struct
{
    int type;
    int source;
    int field; /* for single line of CEA-608 data, 0 if both lines, otherwise field number */
    int len;
    uint8_t *data;
} obe_user_data_t;

typedef struct
{
    int hours;
    int mins;
    int seconds;
    int frames;
    int drop_frame;
} obe_timecode_t;

enum avfm_frame_type_e { AVFM_AUDIO_A52, AVFM_VIDEO, AVFM_AUDIO_PCM };
struct avfm_s {
    enum avfm_frame_type_e frame_type; /* This this matching frame belong inside an audio or video frame? */
    int64_t audio_pts; /* 27MHz */
    int64_t audio_pts_corrected; /* 27MHz */
    int64_t video_pts; /* 27MHz */
    int64_t video_dts; /* 27MHz */
    struct timeval hw_received_tv; /* Wall clock time the frame was received from the hardware. */
    int64_t av_drift; /* 27MHz - Calculation of audio_pts minus video_pts */

    int64_t video_interval_clk; /* 27MHz. Time between two consecutive video frames. Eg 450450 for 60fps */

    /* Bit - desc.
     * ALL BITS DEFAULT TO ZERO during avfm_init(). Hardware core is expected to set accordingly. 
     *   0   Blackmagic specific. Is the port operating in Full (0) or Half duplex mode (1).
     */
#define AVFM_HW_STATUS__MASK_BLACKMAGIC_DUPLEX (0 << 0)
#define AVFM_HW_STATUS__MASK_VEGA              (1 << 1)
#define AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_FULL (0 << AVFM_HW_STATUS__MASK_BLACKMAGIC_DUPLEX)
#define AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_HALF (1 << AVFM_HW_STATUS__MASK_BLACKMAGIC_DUPLEX)
    uint64_t hw_status_flags; /* Bitmask flags indicating hardware status, sample status or such. */
};

__inline__ void avfm_init(struct avfm_s *s, enum avfm_frame_type_e frame_type) {
    s->frame_type = frame_type;
    s->audio_pts = -1;
    s->audio_pts_corrected = -1;
    s->video_pts = -1;
    s->hw_received_tv.tv_sec = 0;
    s->hw_received_tv.tv_usec = 0;
    s->av_drift = 0;
    s->hw_status_flags =  0;
};

__inline__ void avfm_set_pts_video(struct avfm_s *s, int64_t pts) {
    s->video_pts = pts;
    s->av_drift = s->audio_pts - s->video_pts;
}

__inline__ void avfm_set_dts_video(struct avfm_s *s, int64_t dts) {
    s->video_dts = dts;
}

__inline__ void avfm_set_pts_audio(struct avfm_s *s, int64_t pts) {
    s->audio_pts = pts;
    s->av_drift = s->audio_pts - s->video_pts;
}

__inline__ void avfm_set_video_interval_clk(struct avfm_s *s, int64_t clk) {
    s->video_interval_clk = clk;
}

__inline__ int64_t avfm_get_video_interval_clk(struct avfm_s *s) {
    return s->video_interval_clk;
}

__inline__ void avfm_set_pts_audio_corrected(struct avfm_s *s, int64_t pts) {
    s->audio_pts_corrected = pts;
}

__inline__ void avfm_set_hw_received_time(struct avfm_s *s) {
    gettimeofday(&s->hw_received_tv, NULL);
}

__inline__ unsigned int avfm_get_hw_received_tv_sec(struct avfm_s *s) {
    return (unsigned int)s->hw_received_tv.tv_sec;
}

__inline__ int64_t avfm_get_av_drift(struct avfm_s *s) {
    return s->av_drift;
}

__inline__ unsigned int avfm_get_hw_received_tv_usec(struct avfm_s *s) {
    return (unsigned int)s->hw_received_tv.tv_usec;
}

__inline__ uint64_t avfm_get_hw_status_mask(struct avfm_s *s, uint64_t mask) {
    return s->hw_status_flags & mask;
}

__inline__ void avfm_set_hw_status_mask(struct avfm_s *s, uint64_t mask) {
    s->hw_status_flags |= mask;
}

__inline__ void avfm_dump(struct avfm_s *s) {
    printf("%s, a:%14" PRIi64 ", v:%14" PRIi64 ", ac:%14" PRIi64 ", diffabs:%" PRIi64 " -- hw_tv: %d.%d\n",
        s->frame_type == AVFM_AUDIO_A52 ? "A52" :
        s->frame_type == AVFM_AUDIO_PCM ? "PCM" :
        s->frame_type == AVFM_VIDEO     ? "  V" : "U",
        s->audio_pts, s->video_pts,
        s->audio_pts_corrected,
        (int64_t)llabs(s->audio_pts - s->video_pts),
        (unsigned int)s->hw_received_tv.tv_sec,
        (unsigned int)s->hw_received_tv.tv_usec);
}

typedef struct
{
    int input_stream_id;
    int64_t pts; /* PTS time (27MHz) when this frame was received from the capture card. */
                 /* If audio, or video, regardless. */

    /* Regardless of whether raw_frame is of type audio, or video, we need to contain exact PTS
     * hardware references, as provided by the hardware. (Units of 27MHz).
     */
    struct avfm_s avfm; /* TODO: Migrate this into metadata. */
    struct avmetadata_s metadata;

    void *opaque;

    void (*release_data)( void* );
    void (*release_frame)( void* );

    /* Video */
    /* Some devices output visible and VBI/VANC data together. In order
     * to avoid memcpying raw frames, we create two image structures.
     * The first one points to the allocated memory and the second points to the visible frame.
     * For most devices these are the same. */
    obe_image_t alloc_img;
    obe_image_t img;
    int sar_width;
    int sar_height;
    int sar_guess; /* This is set if the SAR cannot be determined from any WSS/AFD that might exist in the stream */
    int64_t arrival_time;
    int timebase_num;
    int timebase_den;

    /* Ancillary / User-data */
    int num_user_data;
    obe_user_data_t *user_data;

    /* Audio */
    obe_audio_frame_t audio_frame;
    // TODO channel order
    // TODO audio metadata

    obe_timecode_t timecode;

    int reset_obe;
} obe_raw_frame_t;

typedef struct
{
    int num_stream_ids;
    int *stream_id_list;

    pthread_t filter_thread;
    obe_queue_t queue;
    int cancel_thread;

} obe_filter_t;
#define PRINT_OBE_FILTER(f, prefix) { \
	printf("%s: obj = %p, num_ids=%d list[0]=%d\n", \
		prefix, f, (f)->num_stream_ids, (f)->stream_id_list[0]); \
};

typedef struct
{
    int output_stream_id;
    int is_ready;
    int is_video;

    enum stream_formats_e priv_stream_format;

    pthread_t encoder_thread;
    obe_queue_t queue;
    int cancel_thread;

    hnd_t encoder_params;

    /* HE-AAC and E-AC3 */
    int num_samples;
} obe_encoder_t;

typedef struct
{
    /* Output */
    pthread_t output_thread;
    int cancel_thread;
    obe_output_dest_t output_dest;

    /* Muxed frame queue for transmission */
    obe_queue_t queue;
} obe_output_t;

enum obe_coded_frame_type_e {
  CF_UNDEFINED = 0,
  CF_VIDEO,
  CF_AUDIO,
};

typedef struct
{
    int output_stream_id;

    enum obe_coded_frame_type_e type;

    struct avfm_s avfm;

    int64_t pts;

    /* Video Only */
    int64_t cpb_initial_arrival_time;
    int64_t cpb_final_arrival_time;
    int64_t real_dts;
    int64_t real_pts;
    int random_access;
    int priority;
    int64_t arrival_time;

    int len;
    uint8_t *data;

    struct timeval creationDate;
} obe_coded_frame_t;

typedef struct
{
    time_t ts;

    int len;
    uint8_t *data;

    /* MPEG-TS */
    int64_t *pcr_list;
} obe_muxed_data_t;
void obe_muxed_data_print(obe_muxed_data_t *ptr, int nr);

struct obe_t
{
    /* bitmask, def:0. */
#define INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY (1 <<  0)
#define INPUTSOURCE__SDI_VANC_DISCOVERY_SCTE104 (1 <<  1)
#define MUX__DQ_HEXDUMP                         (1 <<  4)
#define MUX__PTS_REPORT_TIMES                   (1 <<  5)
#define MUX__REPORT_Q                           (1 <<  6)
#define INPUTSOURCE__SDI_VANC_DISCOVERY_RDD8    (1 <<  7)
    uint32_t verbose_bitmask;
    int is_active;
    int obe_system;

    /* OBE recovered clock */
    pthread_mutex_t obe_clock_mutex;
    pthread_cond_t  obe_clock_cv;
    int64_t         obe_clock_last_pts; /* from sdi clock */
    int64_t         obe_clock_last_wallclock; /* from cpu clock */

    /* Devices */
    pthread_mutex_t device_list_mutex;
    int num_devices;
    obe_device_t *devices[MAX_DEVICES];
    int cur_input_stream_id;

    /* Frame drop flags
     * TODO: make this work for multiple inputs and outputs */
    pthread_mutex_t drop_mutex;
    int video_encoder_drop;
    int audio_encoder_drop;
    int mux_drop;

    /* Streams */
    int num_output_streams;
    obe_output_stream_t *priv_output_streams;

    /** Individual Threads */
    /* Smoothing (video) */
    pthread_t enc_smoothing_thread;
    int cancel_enc_smoothing_thread;

    /* Mux */
    pthread_t mux_thread;
    int cancel_mux_thread;
    obe_mux_opts_t mux_opts;

    /* Smoothing (video) */
    pthread_t mux_smoothing_thread;
    int cancel_mux_smoothing_thread;

    /* Filtering */
    int num_filters;
    obe_filter_t *filters[MAX_STREAMS];

    /** Multiple Threads **/
    /* Input or Postfiltered frames for encoding */
    int num_encoders;
    obe_encoder_t *encoders[MAX_STREAMS];

    /* Output data */
    int num_outputs;
    obe_output_t **outputs;

    /* Encoded frames in smoothing buffer */
    obe_queue_t     enc_smoothing_queue;

    int             enc_smoothing_buffer_complete;
    int64_t         enc_smoothing_last_exit_time;

    /* Encoded frame queue for muxing */
    obe_queue_t mux_queue;

    /* Muxed frames in smoothing buffer */
    obe_queue_t mux_smoothing_queue;

    /* Statistics and Monitoring */
    int cea708_missing_count;

    /* Misc configurable system parameters */
    unsigned int probe_time_seconds;

    /* Runtime statistics */
    void *runtime_statistics;

    /* Terminate After capability */
    void *terminate_after;

    /* Version information */
    uint8_t sw_major;
    uint8_t sw_minor;
    uint8_t sw_patch;

    /* SCTE35 visibility needs to be at the core
     * so that various disconnect components can be
     * correctly enabled or disabled. The Mux and the Decklink input.
     */
    int enable_scte35;
};

typedef struct
{
    void* (*start_smoothing)( void *ptr );
} obe_smoothing_func_t;

extern const obe_smoothing_func_t enc_smoothing;
extern const obe_smoothing_func_t mux_smoothing;

int obe_timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y);
int64_t obe_timediff_to_msecs(struct timeval *tv);
int64_t obe_timediff_to_usecs(struct timeval *tv);

const char *obe_ascii_datetime();
int64_t obe_mdate( void );

obe_device_t *new_device( void );
void destroy_device( obe_device_t *device );
obe_raw_frame_t *new_raw_frame( void );
void destroy_raw_frame( obe_raw_frame_t *raw_frame );
obe_coded_frame_t *new_coded_frame( int stream_id, int len );
size_t coded_frame_serializer_write(FILE *fh, obe_coded_frame_t *cf);
size_t coded_frame_serializer_read(FILE *fh, obe_coded_frame_t **f);
void coded_frame_print(obe_coded_frame_t *cf);
void destroy_coded_frame( obe_coded_frame_t *coded_frame );
void obe_release_video_data( void *ptr );
void obe_release_audio_data( void *ptr );
void obe_release_frame( void *ptr );

obe_muxed_data_t *new_muxed_data( int len );
void destroy_muxed_data( obe_muxed_data_t *muxed_data );

void add_device( obe_t *h, obe_device_t *device );

int add_to_filter_queue( obe_t *h, obe_raw_frame_t *raw_frame );
int add_to_encode_queue( obe_t *h, obe_raw_frame_t *raw_frame, int output_stream_id );
int remove_early_frames( obe_t *h, int64_t pts );
int add_to_output_queue( obe_t *h, obe_muxed_data_t *muxed_data );
int remove_from_output_queue( obe_t *h );

obe_int_input_stream_t *get_input_stream( obe_t *h, int input_stream_id );
obe_encoder_t *get_encoder( obe_t *h, int stream_id );
obe_output_stream_t *get_output_stream_by_id( obe_t *h, int stream_id);
obe_output_stream_t *get_output_stream_by_format( obe_t *h, int format );

__inline__ static obe_output_stream_t *obe_core_get_output_stream_by_index(struct obe_t *s, int nr)
{
	return &s->priv_output_streams[nr];
}
void obe_core_dump_output_stream(obe_output_stream_t *s, int index);

__inline__ enum stream_formats_e obe_core_encoder_get_stream_format(obe_encoder_t *e)
{
	return e->priv_stream_format;
}

__inline__ static obe_encoder_t *obe_core_encoder_alloc(enum stream_formats_e stream_format)
{
	obe_encoder_t *e = (obe_encoder_t *)calloc(1, sizeof(*e));
	e->priv_stream_format = stream_format;
	printf("%s() Created encoder for stream_format id %2d (%s)\n",
		__func__, stream_format, stream_format_name(stream_format));
	return e;
}

int64_t get_wallclock_in_mpeg_ticks( void );
void sleep_mpeg_ticks( int64_t i_delay );
void obe_clock_tick( obe_t *h, int64_t value );
int64_t get_input_clock_in_mpeg_ticks( obe_t *h );
void sleep_input_clock( obe_t *h, int64_t i_delay );

int get_non_display_location( int type );
void obe_raw_frame_printf(obe_raw_frame_t *rf);
obe_raw_frame_t *obe_raw_frame_copy(obe_raw_frame_t *frame);
void obe_image_save(obe_image_t *src);

#if 0
void obe_image_copy(obe_image_t *dst, obe_image_t *src);
int obe_image_compare(obe_image_t *dst, obe_image_t *src);
void obe_raw_frame_free(obe_raw_frame_t *frame);
#endif

#ifdef __cplusplus
extern "C" {
#endif

int obe_getTimestamp(char *s, time_t *when);
void klsyslog_and_stdout(int level, const char *format, ...);

int obe_getProcessRuntimeAscii(char *buf, int len);

#ifdef __cplusplus
};
#endif

#endif
