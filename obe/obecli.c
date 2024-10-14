/*****************************************************************************
 * obecli.c: open broadcast encoder cli
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 * Some code originates from the x264 project
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

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <getopt.h>
#include <ctype.h>
#include <include/DeckLinkAPIVersion.h>
#include <common/scte104filtering.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <signal.h>
#include <libgen.h>
#define _GNU_SOURCE

#include <readline/readline.h>
#include <readline/history.h>
#include <libswresample/swresample.h>
#include <libmpegts.h>

#include <encoders/video/sei-timestamp.h>
#include <json-c/json.h>

#include "obe.h"
#include "obecli.h"
#include "obecli-shared.h"
#include "common/common.h"
#include "ltn_ws.h"
#include "common/http.h"
#include "process.h"

#if HAVE_DTAPI_H
extern char *dektec_sdk_version;
#endif
#if HAVE_VEGA3301_CAP_TYPES_H
extern char *vega3301_sdk_version;
#endif
#if HAVE_VEGA3311_CAP_TYPES_H
extern char *vega3311_sdk_version;
#endif

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "obecli", __VA_ARGS__ )
#define RETURN_IF_ERROR( cond, ... ) RETURN_IF_ERR( cond, "options", NULL, __VA_ARGS__ )

#define MODULE_PREFIX "[core]: "

/* The is the master outer global context for the entire application */
obecli_ctx_t cli;

#if LTN_WS_ENABLE
void *g_ltn_ws_handle = NULL;
#endif

extern int g_decklink_op47_teletext_reverse;

/* TS Mux */
extern int64_t g_mux_pcr_adjustment;

char g_device_input_type[64]; /* decklink, ndi, etc */
char g_device_input_port[128] = { 0 }; /* A, ndi://x.y.z, 0 */

char g_signal_format_detected[64] = { 0 };
struct timespec g_signal_detected_ts;
struct timespec g_signal_los_ts;
struct timespec g_smpte2038_detected_ts;
struct timespec g_cea708_detected_ts;
struct timespec g_scte104_detected_ts = { 0, 0 };
uint64_t g_scte35_trigger_count = 0;

double g_audio_channel_level_dbfs[32];
int g_audio_channel_type[32]; /* 0 = undefined, 1 = pcm, 2 = bitstream */

int  runtime_statistics_start(void **ctx, obecli_ctx_t *cli);
void runtime_statistics_stop(void *ctx);
int  terminate_after_start(void **ctx, obecli_ctx_t *cli, int afterNSeconds);
void terminate_after_stop(void *ctx);

/* Ctrl-C handler */
//static volatile int b_ctrl_c = 0;

static int g_running = 0;
static int system_type_value = OBE_SYSTEM_TYPE_GENERIC;

static const char * const system_types[]             = { "generic", "lowestlatency", "lowlatency", 0 };
static const char * const input_types[]              = { "url", "decklink", "linsys-sdi", "v4l2",
#if HAVE_BLUEDRIVER_P_H
								"bluefish",
#else
								"bluefish-not-available",
#endif
								"v210",
#if HAVE_DTAPI_H
                                                                "dektec",
#else
                                                                "dektec-not-available",
#endif
#if HAVE_PROCESSING_NDI_LIB_H
								"ndi",
#else
								"ndi-not-available",
#endif
#if defined(__APPLE__)
								"avfoundation",
#else
								"avfoundation-no-available",
#endif
#if HAVE_VEGA3301_CAP_TYPES_H
                                                                "vega3301",
#else
                                                                "vega3301-not-available",
#endif
#if HAVE_VEGA3311_CAP_TYPES_H
                                                                "vega3311",
#else
                                                                "vega3311-not-available",
#endif
								0 };
static const char * const input_video_formats[]      = { "pal", "ntsc", "720p50", "720p59.94", "720p60", "1080i50", "1080i59.94", "1080i60",
                                                         "1080p23.98", "1080p24", "1080p25", "1080p29.97", "1080p30", "1080p50", "1080p59.94",
                                                         "1080p60", 0 };
static const char * const input_video_connections[]  = { "sdi", "hdmi", "optical-sdi", "component", "composite", "s-video", 0 };
static const char * const input_audio_connections[]  = { "embedded", "aes-ebu", "analogue", 0 };
static const char * const stream_actions[]           = { "passthrough", "encode", 0 };
static const char * const encode_formats[]           = { "", "avc", "", "", "mp2", "ac3", "e-ac3", "aac", "a52", 0 };
static const char * const frame_packing_modes[]      = { "none", "checkerboard", "column", "row", "side-by-side", "top-bottom", "temporal", 0 };
static const char * const teletext_types[]           = { "", "initial", "subtitle", "additional-info", "program-schedule", "hearing-imp", 0 };
static const char * const audio_types[]              = { "undefined", "clean-effects", "hearing-impaired", "visual-impaired", 0 };
static const char * const aac_profiles[]             = { "aac-lc", "he-aac-v1", "he-aac-v2" };
static const char * const aac_encapsulations[]       = { "adts", "latm", 0 };
static const char * const mp2_modes[]                = { "auto", "stereo", "joint-stereo", "dual-channel", 0 };
static const char * const channel_maps[]             = { "", "mono", "stereo", "5.0", "5.1", 0 };
static const char * const mono_channels[]            = { "left", "right", 0 };
static const char * const output_modules[]           = { "udp", "rtp", "linsys-asi", "filets", 0 };
static const char * const addable_streams[]          = { "audio", "ttx" };
static const char * const preset_names[]        = { "ultrafast", "superfast", "veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow", "placebo", NULL };
static const char * const tuning_names[]        = { "animation", "zerolatency", "fastdecode", "grain", "ssim", "psnr", NULL };
static const char * entropy_modes[] = { "cabac", "cavlc", NULL };

static const char * system_opts[] = { "system-type", "max-probe-time", NULL };
static const char * input_opts[]  = { "location", "card-idx", "video-format", "video-connection", "audio-connection",
                                      "smpte2038", "scte35", "vanc-cache", "bitstream-audio", "patch1", "los-exit-ms",
                                      "frame-injection", /* 11 */
                                      "allow-1080p60", /* 12 */
                                      "name", /* 13 */
                                      "hdr", /* 14 */
                                      "smpte2031", /* 15 */
                                      "timecode", /* 16 */
                                      "4k-quad", /* 17 */
                                      "4k-2si", /* 18 */
                                      NULL };
static const char * add_opts[] =    { "type" };
/* TODO: split the stream options into general options, video options, ts options */
static const char * stream_opts[] = { "action", "format",
                                      /* Encoding options */
                                      "vbv-maxrate", "vbv-bufsize", "bitrate",
                                      "profile", "level", "keyint", "lookahead", "threads", "bframes", "b-pyramid", "weightp",
                                      "interlaced", "tff", "frame-packing", "csp", "filler", "intra-refresh", "aspect-ratio",
                                      "width", "max-refs",

                                      /* Audio options - 22-30 */
                                      "sdi-audio-pair", "channel-map", "mono-channel",
                                      /* AAC options */
                                      "aac-profile", "aac-encap",
                                      /* MP2 options */
                                      "mp2-mode",
                                      /* TS options */
                                      "pid", "lang", "audio-type",
                                      /* Teletext 31-35 */
                                      "num-ttx", "ttx-lang", "ttx-type", "ttx-mag", "ttx-page",
                                      /* VBI options */
                                      "vbi-ttx", "vbi-inv-ttx", "vbi-vps", "vbi-wss",

                                      "opencl", /* 40 */
                                      "preset-name", /* 41 */
                                      "entropy", /* 42 */
                                      "audio-offset", /* 43 */
                                      "video-codec", /* 44 */
                                      "tuning-name", /* 45 */
                                      "dialnorm", /* 46 */
                                      "ttx-reverse", /* 47 */
                                      "gain", /* 48 */
                                      "remap", /* 49 */
                                      "mute", /* 50 */
                                      "gop-closed", /* 51 */
                                      "bit-depth", /* 52 */
                                      NULL };

static const char * muxer_opts[]  = { "ts-type", "cbr", "ts-muxrate", "passthrough", "ts-id", "program-num", "pmt-pid", "pcr-pid",
                                      "pcr-period", "pat-period", "service-name", "provider-name", "scte35-pid", "smpte2038-pid",
                                      "section-padding", "smpte2031-pid", NULL };
static const char * ts_types[]    = { "generic", "dvb", "cablelabs", "atsc", "isdb", NULL };
static const char * output_opts[] = { "type", "target", "trim", NULL };

const static int allowed_resolutions[17][2] =
{
    /* NTSC */
    { 720, 480 },
    { 640, 480 },
    { 544, 480 },
    { 480, 480 },
    { 352, 480 },
    /* PAL */
    { 720, 576 },
    { 544, 576 },
    { 480, 576 },
    { 352, 576 },
    /* HD */
    { 1920, 1080 },
    { 1440, 1080 },
    { 1280, 1080 },
    {  960, 1080 },
    { 1280,  720 },
    {  960,  720 },
    {  640,  720 },
    { 0, 0 }
};

const static uint64_t channel_layouts[] =
{
    AV_CH_LAYOUT_STEREO,
    AV_CH_LAYOUT_MONO,
    AV_CH_LAYOUT_STEREO,
    AV_CH_LAYOUT_5POINT0_BACK,
    AV_CH_LAYOUT_5POINT1_BACK,
 };

int64_t sanitizeParamTrim(int64_t val)
{
       if (val < 0)
               return 0;

       if (val > 2000)
               return 2000;
       else
               return val;
}

static char *getSoftwareVersion()
{
    char *msg = malloc(128);
    sprintf(msg, "Version %d.%d.%d (" GIT_VERSION ")",
	VERSION_MAJOR,
	VERSION_MINOR,
	VERSION_PATCH);

    return msg;
}

void obe_cli_printf( const char *name, const char *fmt, ... )
{
    fprintf( stderr, "%s: ", name );
    va_list arg;
    va_start( arg, fmt );
    vfprintf( stderr, fmt, arg );
    va_end( arg );
}

static char **obe_split_string( char *string, char *sep, uint32_t limit )
{
    if( !string )
        return NULL;
    int sep_count = 0;
    char *tmp = string;
    while( ( tmp = ( tmp = strstr( tmp, sep ) ) ? tmp + strlen( sep ) : 0 ) )
        ++sep_count;
    if( sep_count == 0 )
    {
        if( string[0] == '\0' )
            return calloc( 1, sizeof( char** ) );
        char **ret = calloc( 2, sizeof( char** ) );
        ret[0] = strdup( string );
        return ret;
    }

    char **split = calloc( ( limit > 0 ? limit : sep_count ) + 2, sizeof(char**) );
    int i = 0;
    char *str = strdup( string );
    assert( str );
    char *esc = NULL;
    char *tok = str, *nexttok = str;
    do
    {
        nexttok = strstr( nexttok, sep );
        if( nexttok )
            *nexttok++ = '\0';
        if( ( limit > 0 && i >= limit ) ||
            ( i > 0 && ( ( esc = strrchr( split[i-1], '\\' ) ) ? esc[1] == '\0' : 0 ) ) ) // Allow escaping
        {
            int j = i-1;
            if( esc )
                esc[0] = '\0';
            split[j] = realloc( split[j], strlen( split[j] ) + strlen( sep ) + strlen( tok ) + 1 );
            assert( split[j] );
            strcat( split[j], sep );
            strcat( split[j], tok );
            esc = NULL;
        }
        else
        {
            split[i++] = strdup( tok );
            assert( split[i-1] );
        }
        tok = nexttok;
    } while ( tok );
    free( str );
    assert( !split[i] );

    return split;
}

static void obe_free_string_array( char **array )
{
    if( !array )
        return;
    for( int i = 0; array[i] != NULL; i++ )
        free( array[i] );
    free( array );
}

static char **obe_split_options( const char *opt_str, const char *options[] )
{
    if( !opt_str )
        return NULL;
    char *opt_str_dup = strdup( opt_str );
    char **split = obe_split_string( opt_str_dup, ",", 0 );
    free( opt_str_dup );
    int split_count = 0;
    while( split[split_count] != NULL )
        ++split_count;

    int options_count = 0;
    while( options[options_count] != NULL )
        ++options_count;

    char **opts = calloc( split_count * 2 + 2, sizeof( char ** ) );
    char **arg = NULL;
    int opt = 0, found_named = 0, invalid = 0;
    for( int i = 0; split[i] != NULL; i++, invalid = 0 )
    {
        arg = obe_split_string( split[i], "=", 2 );
        if( arg == NULL )
        {
            if( found_named )
                invalid = 1;
            else RETURN_IF_ERROR( i > options_count || options[i] == NULL, "Too many options given\n" )
            else
            {
                opts[opt++] = strdup( options[i] );
                opts[opt++] = strdup( "" );
            }
        }
        else if( arg[0] == NULL || arg[1] == NULL )
        {
            if( found_named )
                invalid = 1;
            else RETURN_IF_ERROR( i > options_count || options[i] == NULL, "Too many options given\n" )
            else
            {
                opts[opt++] = strdup( options[i] );
                if( arg[0] )
                    opts[opt++] = strdup( arg[0] );
                else
                    opts[opt++] = strdup( "" );
            }
        }
        else
        {
            found_named = 1;
            int j = 0;
            while( options[j] != NULL && strcmp( arg[0], options[j] ) )
                ++j;
            RETURN_IF_ERROR( options[j] == NULL, "Invalid option '%s'\n", arg[0] )
            else
            {
                opts[opt++] = strdup( arg[0] );
                opts[opt++] = strdup( arg[1] );
            }
        }
        RETURN_IF_ERROR( invalid, "Ordered option given after named\n" )
        obe_free_string_array( arg );
    }
    obe_free_string_array( split );
    return opts;
}

static char *obe_get_option( const char *name, char **split_options )
{
    if( !split_options )
        return NULL;
    int last_i = -1;
    for( int i = 0; split_options[i] != NULL; i += 2 )
        if( !strcmp( split_options[i], name ) )
            last_i = i;
    if( last_i >= 0 )
        return split_options[last_i+1][0] ? split_options[last_i+1] : NULL;
    return NULL;
}

static int obe_otob( char *str, int def )
{
   int ret = def;
   if( str )
       ret = !strcasecmp( str, "true" ) ||
             !strcmp( str, "1" ) ||
             !strcasecmp( str, "yes" );
   return ret;
}

static double obe_otof( char *str, double def )
{
   double ret = def;
   if( str )
   {
       char *end;
       ret = strtod( str, &end );
       if( end == str || *end != '\0' )
           ret = def;
   }
   return ret;
}

static int obe_otoi(const char *str, int def)
{
    int ret = def;
    if( str )
    {
        char *end;
        ret = strtol( str, &end, 0 );
        if( end == str || *end != '\0' )
            ret = def;
    }
    return ret;
}

static int check_enum_value( const char *arg, const char * const *names )
{
    for( int i = 0; names[i]; i++ )
        if( !strcasecmp( arg, names[i] ) )
            return 0;

    return -1;
}

static int parse_enum_value( const char *arg, const char * const *names, int *dst )
{
    for( int i = 0; names[i]; i++ )
        if( !strcasecmp( arg, names[i] ) )
        {
            *dst = i;
            return 0;
        }
    return -1;
}

static char *get_format_name(int stream_format, const obecli_format_name_t *names, int long_name)
{
    int i = 0;

    while( names[i].format_name != 0 && names[i].format != stream_format )
        i++;

    return  long_name ? names[i].long_name : names[i].format_name;
}

const char *obe_core_get_format_name_short(enum stream_formats_e stream_format)
{
	return (const char *)get_format_name(stream_format, format_names, 0);
}

/* add/remove functions */
static int add_stream( char *command, obecli_command_t *child )
{
    int stream_format = 0;
    obe_output_stream_t *tmp;
    if( !cli.program.num_streams )
    {
        printf( "No input streams. Please probe a device \n" );
        return -1;
    }

    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, ":" );
    command[tok_len] = 0;

    int output_stream_id = obe_otoi( command, -1 );

    FAIL_IF_ERROR( output_stream_id < 0 || output_stream_id == 0 || output_stream_id > cli.num_output_streams,
                   "Invalid stream id\n" );

    char *params = command + tok_len + 1;
    char **opts = obe_split_options( params, add_opts );
    if( !opts && params )
        return -1;

    char *type     = obe_get_option( add_opts[0], opts );

    FAIL_IF_ERROR( type && ( check_enum_value( type, addable_streams ) < 0 ),
                   "Stream type is not addable\n" )

    if( !strcasecmp( type, addable_streams[1] ) )
    {
        for( int i = 0; i < cli.num_output_streams; i++ )
        {
            FAIL_IF_ERROR( cli.output_streams[i].stream_format == MISC_TELETEXT,
                           "Multiple DVB-TTX PIDs are not supported\n" )
        }
    }

    tmp = realloc( cli.output_streams, sizeof(*cli.output_streams) * (cli.num_output_streams+1) );
    FAIL_IF_ERROR( !tmp, "malloc failed\n" );
    cli.output_streams = tmp;
    memmove( &cli.output_streams[output_stream_id+1], &cli.output_streams[output_stream_id], (cli.num_output_streams-output_stream_id)*sizeof(*cli.output_streams) );
    cli.num_output_streams++;

    for( int i = output_stream_id+1; i < cli.num_output_streams; i++ )
        cli.output_streams[i].output_stream_id++;

    memset( &cli.output_streams[output_stream_id], 0, sizeof(*cli.output_streams) );

    if( !strcasecmp( type, addable_streams[0] ) ) /* Audio */
    {
        cli.output_streams[output_stream_id].input_stream_id = 1; /* FIXME when more stream types are allowed */
        cli.output_streams[output_stream_id].sdi_audio_pair = 1;
        cli.output_streams[output_stream_id].channel_layout = AV_CH_LAYOUT_STEREO;
    }
    else if( !strcasecmp( type, addable_streams[1] ) ) /* DVB-TTX */
    {
        cli.output_streams[output_stream_id].input_stream_id = -1;
        cli.output_streams[output_stream_id].stream_format = stream_format;
    }
    cli.output_streams[output_stream_id].output_stream_id = output_stream_id;

    printf( "NOTE: output-stream-ids have CHANGED! \n" );

    show_output_streams( NULL, NULL );

    return 0;
}

static int remove_stream( char *command, obecli_command_t *child )
{
    obe_output_stream_t *tmp;
    if( !cli.program.num_streams )
    {
        printf( "No input streams. Please probe a device \n" );
        return -1;
    }

    int output_stream_id = obe_otoi( command, -1 );

    FAIL_IF_ERROR( output_stream_id < 0 || output_stream_id == 0 || cli.num_output_streams == 2 || cli.num_output_streams <= output_stream_id,
                   "Invalid stream id\n" );

    free( cli.output_streams[output_stream_id].ts_opts.teletext_opts );
    cli.output_streams[output_stream_id].ts_opts.teletext_opts = NULL;

    memmove( &cli.output_streams[output_stream_id], &cli.output_streams[output_stream_id+1], (cli.num_output_streams-1-output_stream_id)*sizeof(*cli.output_streams) );
    tmp = realloc( cli.output_streams, sizeof(*cli.output_streams) * (cli.num_output_streams-1) );
    cli.num_output_streams--;
    FAIL_IF_ERROR( !tmp, "malloc failed\n" );
    cli.output_streams = tmp;

    printf( "NOTE: output-stream-ids have CHANGED! \n" );

    show_output_streams( NULL, NULL );

    return 0;
}

/* set functions - TODO add lots more opts */
static int set_obe( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        char *params = command + tok_len + 1;
        char **opts = obe_split_options( params, system_opts );
        if( !opts && params )
            return -1;

        char *system_type     = obe_get_option( system_opts[0], opts );

        FAIL_IF_ERROR( system_type && ( check_enum_value( system_type, system_types ) < 0 ),
                       "Invalid system type\n" );

        char *max_probe_time  = obe_get_option(system_opts[1], opts);
        if (max_probe_time) {
            cli.h->probe_time_seconds = atoi(max_probe_time);
            if ((cli.h->probe_time_seconds < 5) || (cli.h->probe_time_seconds > MAX_PROBE_TIME)) {
                printf("%s valid values are %d to %d, defaulting to %d\n",
                    system_opts[1], MIN_PROBE_TIME, MAX_PROBE_TIME, MAX_PROBE_TIME);
                cli.h->probe_time_seconds = MAX_PROBE_TIME;
            } else
                printf("%s is now %d\n", system_opts[1], cli.h->probe_time_seconds);
        }

        FAIL_IF_ERROR( cli.program.num_streams, "Cannot change OBE options after probing\n" )

        if( system_type )
        {
            parse_enum_value( system_type, system_types, &system_type_value );
            obe_set_config( cli.h, system_type_value );
        }

        obe_free_string_array( opts );
    }

    return 0;
}

static int set_input( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        char *params = command + tok_len + 1;
        char **opts = obe_split_options( params, input_opts );
        if( !opts && params )
            return -1;

        char *location     = obe_get_option( input_opts[0], opts );
        char *card_idx     = obe_get_option( input_opts[1], opts );
        char *video_format = obe_get_option( input_opts[2], opts );
        char *video_connection = obe_get_option( input_opts[3], opts );
        char *audio_connection = obe_get_option( input_opts[4], opts );
        char *smpte2038 = obe_get_option( input_opts[5], opts );
        char *scte35 = obe_get_option( input_opts[6], opts );
        char *vanc_cache = obe_get_option( input_opts[7], opts );
        char *bitstream_audio = obe_get_option( input_opts[8], opts );
        char *patch1 = obe_get_option( input_opts[9], opts );
        char *los_exit_ms = obe_get_option( input_opts[10], opts );
        char *frame_injection = obe_get_option(input_opts[11], opts);
        char *allow_1080p60 = obe_get_option(input_opts[12], opts);
        char *name = obe_get_option(input_opts[13], opts);
        char *hdr = obe_get_option( input_opts[14], opts );
        char *smpte2031 = obe_get_option( input_opts[15], opts );
        char *timecode = obe_get_option( input_opts[16], opts );
        char *enable_4k_quad = obe_get_option( input_opts[17], opts );
        char *enable_4k_2si = obe_get_option( input_opts[18], opts );

        FAIL_IF_ERROR( video_format && ( check_enum_value( video_format, input_video_formats ) < 0 ),
                       "Invalid video format\n" );

        FAIL_IF_ERROR( video_connection && ( check_enum_value( video_connection, input_video_connections ) < 0 ),
                       "Invalid video connection\n" );

        FAIL_IF_ERROR( audio_connection && ( check_enum_value( audio_connection, input_audio_connections ) < 0 ),
                       "Invalid audio connection\n" );

        if (name) {
            cli.input.name = strdup(name);
        }

        if( location )
        {
             if( cli.input.location )
                 free( cli.input.location );

             cli.input.location = malloc( strlen( location ) + 1 );
             FAIL_IF_ERROR( !cli.input.location, "malloc failed\n" );
             strcpy( cli.input.location, location );
        }

        cli.input.enable_allow_1080p60 = obe_otoi(allow_1080p60, cli.input.enable_allow_1080p60);
        cli.input.enable_frame_injection = obe_otoi(frame_injection, cli.input.enable_frame_injection);
        cli.input.enable_patch1 = obe_otoi( patch1, cli.input.enable_patch1 );
        cli.input.enable_bitstream_audio = obe_otoi( bitstream_audio, cli.input.enable_bitstream_audio );
        cli.input.enable_smpte2038 = obe_otoi( smpte2038, cli.input.enable_smpte2038 );
        cli.input.enable_smpte2031 = obe_otoi( smpte2031, cli.input.enable_smpte2031 );
        cli.input.enable_hdr = obe_otoi( hdr, cli.input.enable_hdr );
        cli.input.enable_4k_quad = obe_otoi( enable_4k_quad, cli.input.enable_4k_quad );
        cli.input.enable_4k_2si = obe_otoi( enable_4k_2si, cli.input.enable_4k_2si );
        cli.input.enable_scte35 = obe_otoi( scte35, cli.input.enable_scte35 );
        cli.h->enable_scte35 = cli.input.enable_scte35; /* Put this on the core cache, no just in the input content. */
        cli.h->enable_timecode = obe_otoi( timecode, cli.h->enable_timecode ); /* Put this on the core cache, no just in the input content. */

        cli.input.enable_vanc_cache = obe_otoi( vanc_cache, cli.input.enable_vanc_cache );
        cli.input.enable_los_exit_ms = obe_otoi( los_exit_ms, cli.input.enable_los_exit_ms );
        cli.input.card_idx = obe_otoi( card_idx, cli.input.card_idx );
        if( video_format )
            parse_enum_value( video_format, input_video_formats, &cli.input.video_format );
        if( video_connection )
            parse_enum_value( video_connection, input_video_connections, &cli.input.video_connection );
        if( audio_connection )
            parse_enum_value( audio_connection, input_audio_connections, &cli.input.audio_connection );

        obe_free_string_array( opts );
    }
    else
    {
        FAIL_IF_ERROR( ( check_enum_value( command, input_types ) < 0 ), "Invalid input type\n" );
        parse_enum_value( command, input_types, &cli.input.input_type );
        strcpy(&g_device_input_type[0], command);
    }

    return 0;
}

static int set_stream( char *command, obecli_command_t *child )
{
    obe_input_stream_t *input_stream = NULL;
    obe_output_stream_t *output_stream;
    int i = 0;

    FAIL_IF_ERROR( !cli.num_output_streams, "no output streams \n" );

    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        command += tok_len+1;
        int tok_len2 = strcspn( command, ":" );
        int str_len2 = strlen( command );
        command[tok_len2] = 0;

        int output_stream_id = obe_otoi( command, -1 );
#if 1
        int oid = output_stream_id;

        printf("OUTPUT STREAM ID for set ups %d\n", output_stream_id);

        /* Enumerate all streams, looking for output_stream_id X in the array, don't assume
         * that it's in position X, because the array is modified by the 'remove stream' command.
         */
        output_stream = NULL;

        for (int i = 0; i < cli.num_output_streams; i++) {
            if (cli.output_streams[i].output_stream_id == output_stream_id) {
                output_stream = &cli.output_streams[i];
                output_stream_id = i; /* Lots of code below used this in as an index into a statck list */
                break;
            }
        }
        if (output_stream_id < 0 || output_stream == NULL) {
            fprintf(stderr, "obecli: Invalid stream id %d\n", output_stream_id);
            return -1;
        }

        if (output_stream_id != oid) {
            printf("obecli: Adjusted output stream obejct selection from idx %d to %d (SMPTE2031)\n",
                output_stream_id, oid);
        }

        input_stream = &cli.program.streams[output_stream->input_stream_id];
#else
        FAIL_IF_ERROR( output_stream_id < 0 || output_stream_id > cli.num_output_streams-1,
                       "Invalid stream id\n" );

        input_stream = &cli.program.streams[cli.output_streams[output_stream_id].input_stream_id];
        output_stream = &cli.output_streams[output_stream_id];
#endif

        if( str_len > str_len2 )
        {
            char *params = command + tok_len2 + 1;
            char **opts = obe_split_options( params, stream_opts );
            if( !opts && params )
                return -1;

            char *action      = obe_get_option( stream_opts[0], opts );
            char *format      = obe_get_option( stream_opts[1], opts );
            char *vbv_maxrate = obe_get_option( stream_opts[2], opts );
            char *vbv_bufsize = obe_get_option( stream_opts[3], opts );
            char *bitrate     = obe_get_option( stream_opts[4], opts );
            char *profile     = obe_get_option( stream_opts[5], opts );
            char *level       = obe_get_option( stream_opts[6], opts );
            char *keyint      = obe_get_option( stream_opts[7], opts );
            char *lookahead   = obe_get_option( stream_opts[8], opts );
            char *threads     = obe_get_option( stream_opts[9], opts );
            char *bframes     = obe_get_option( stream_opts[10], opts );
            char *b_pyramid   = obe_get_option( stream_opts[11], opts );
            char *weightp     = obe_get_option( stream_opts[12], opts );
            char *interlaced  = obe_get_option( stream_opts[13], opts );
            char *tff         = obe_get_option( stream_opts[14], opts );
            char *frame_packing = obe_get_option( stream_opts[15], opts );
            char *csp         = obe_get_option( stream_opts[16], opts );
            char *filler      = obe_get_option( stream_opts[17], opts );
            char *intra_refresh = obe_get_option( stream_opts[18], opts );
            char *aspect_ratio = obe_get_option( stream_opts[19], opts );
            char *width = obe_get_option( stream_opts[20], opts );
            char *max_refs = obe_get_option( stream_opts[21], opts );

            /* Audio Options */
            char *sdi_audio_pair = obe_get_option( stream_opts[22], opts );
            char *channel_map    = obe_get_option( stream_opts[23], opts );
            char *mono_channel   = obe_get_option( stream_opts[24], opts );

            /* AAC options */
            char *aac_profile = obe_get_option( stream_opts[25], opts );
            char *aac_encap   = obe_get_option( stream_opts[26], opts );

            /* MP2 options */
            char *mp2_mode    = obe_get_option( stream_opts[27], opts );

            /* Audio Gain Options. 0dB is effectively no gain adjustment. Eg 0dB, 3dB -4dB, etc */
            char *gain_db    = obe_get_option( stream_opts[48], opts );

            /* Audio Remapping Options.  */
            char *audio_remap = obe_get_option( stream_opts[49], opts );

            /* Audio Muting Options.  */
            char *audio_mute = obe_get_option( stream_opts[50], opts );

            /* NB: remap these and the ttx values below if more encoding options are added - TODO: split them up */
            char *pid         = obe_get_option( stream_opts[28], opts );
            char *lang        = obe_get_option( stream_opts[29], opts );
            char *audio_type  = obe_get_option( stream_opts[30], opts );

            char *opencl  = obe_get_option( stream_opts[40], opts );
            const char *preset_name  = obe_get_option( stream_opts[41], opts );
            const char *entropy_mode = obe_get_option( stream_opts[42], opts );
            const char *audio_offset = obe_get_option( stream_opts[43], opts );
            const char *video_codec = obe_get_option( stream_opts[44], opts );
            const char *tuning_name  = obe_get_option( stream_opts[45], opts );
            const char *dialnorm  = obe_get_option( stream_opts[46], opts );

            char *gop_closed = obe_get_option( stream_opts[51], opts );

            /* It's 100% always 8bit for X264. It can vary for VEGA codecs. */
            char *bit_depth = obe_get_option( stream_opts[52], opts );
            int n_bit_depth = 8;
            if (bit_depth) {
                n_bit_depth = atoi(bit_depth);
                switch(n_bit_depth) {
                case 8:
                case 10:
                    break;
                default:
                    fprintf(stderr, "Illegal bit depth %d, expected 8 or 10.\n", n_bit_depth);
                    return -1;
                }
            }

            int video_codec_id = 0; /* AVC */
            if (video_codec) {
                if (strcasecmp(video_codec, "AVC") == 0)
                    video_codec_id = 0; /* AVC */
#if HAVE_VEGA3301_CAP_TYPES_H
                else
                if (strcasecmp(video_codec, "HEVC_VEGA3301") == 0) {
                    video_codec_id = 9; /* HEVC */
                    /* Enable SEI timestamping by default, for the code, but NOT for UDP output. */
                    g_sei_timestamping = 1;
                }
#endif
#if HAVE_VEGA3311_CAP_TYPES_H
                else
                if (strcasecmp(video_codec, "HEVC_VEGA3311") == 0) {
                    video_codec_id = 10; /* HEVC_VEGA */
                    /* Enable SEI timestamping by default, for the code, but NOT for UDP output. */
                    g_sei_timestamping = 1;
                    g_mux_pcr_adjustment = -300;
                }
                else
                if (strcasecmp(video_codec, "AVC_VEGA3311") == 0) {
                    video_codec_id = 11; /* AVC_VEGA */
                    /* Enable SEI timestamping by default, for the code, but NOT for UDP output. */
                    g_sei_timestamping = 1;
                    g_mux_pcr_adjustment = -300;
                }
#endif
#if HAVE_X265_H
                else
                if (strcasecmp(video_codec, "HEVC") == 0) {
                    video_codec_id = 1; /* HEVC */
                    /* Enable SEI timestamping by default, for the code, but NOT for UDP output. */
                    g_sei_timestamping = 1;
                }
#endif
#if HAVE_VA_VA_H
                else
                if (strcasecmp(video_codec, "AVC_VAAPI") == 0)
                    video_codec_id = 2; /* AVC via VAAPI */
                else
                if (strcasecmp(video_codec, "HEVC_VAAPI") == 0)
                    video_codec_id = 3; /* HEVC via VAAPI */
                else
                if (strcasecmp(video_codec, "AVC_GPU_VAAPI_AVCODEC") == 0)
                    video_codec_id = 4; /* AVC via AVCODEC (GPU encode) */
                else
                if (strcasecmp(video_codec, "HEVC_GPU_VAAPI_AVCODEC") == 0)
                    video_codec_id = 5; /* HEVC via AVCODEC (GPU encode) */
                else
                if (strcasecmp(video_codec, "AVC_CPU_AVCODEC") == 0)
                    video_codec_id = 6; /* AVC via AVCODEC (CPU encode) */
                else
                if (strcasecmp(video_codec, "HEVC_CPU_AVCODEC") == 0)
                    video_codec_id = 7; /* HEVC via AVCODEC (CPU encode) */
#endif
                else
                if (strcasecmp(video_codec, "HEVC_GPU_NVENC_AVCODEC") == 0)
                    video_codec_id = 8; /* HEVC via AVCODEC (CPU encode) */
                else {
                    fprintf(stderr, "video codec selection is invalid\n" );
                    return -1;
                }
            }

            if( input_stream->stream_type == STREAM_TYPE_VIDEO )
            {
                if (audio_offset)
                    cli.output_streams[output_stream_id].audio_offset_ms = atoi(audio_offset);
                else
                    cli.output_streams[output_stream_id].audio_offset_ms = 0;

                x264_param_t *avc_param = &cli.output_streams[output_stream_id].avc_param;

                FAIL_IF_ERROR(preset_name && (check_enum_value( preset_name, preset_names) < 0),
                              "Invalid preset-name\n" );
                FAIL_IF_ERROR(tuning_name && (check_enum_value( tuning_name, tuning_names) < 0),
                              "Invalid tuning-name\n" );
                FAIL_IF_ERROR(entropy_mode && (check_enum_value(entropy_mode, entropy_modes) < 0),
                              "Invalid entropy coding mode\n" );

extern char g_video_encoder_preset_name[64];

                if (preset_name) {
                    strcpy(g_video_encoder_preset_name, preset_name);
                    obe_populate_avc_encoder_params(cli.h,  input_stream->input_stream_id
			/* cli.program.streams[i].input_stream_id */, avc_param, preset_name);
                } else {
                    sprintf(g_video_encoder_preset_name, "very-fast");
                    obe_populate_avc_encoder_params(cli.h, input_stream->input_stream_id
			/* cli.program.streams[i].input_stream_id */, avc_param, "veryfast");
                }

extern char g_video_encoder_tuning_name[64];
                if (tuning_name) {
                    strcpy(g_video_encoder_tuning_name, tuning_name);
                } else {
                    g_video_encoder_tuning_name[0] = 0;
                }

                /* We default to CABAC if the user has not specified an entropy mode. */
                avc_param->b_cabac = 1;
                if (entropy_mode) {
                    if (strcasecmp(entropy_mode, "cavlc") == 0)
                        avc_param->b_cabac = 0;
                }

                FAIL_IF_ERROR( profile && ( check_enum_value( profile, x264_profile_names ) < 0 ),
                               "Invalid AVC profile\n" );

                FAIL_IF_ERROR( vbv_bufsize && system_type_value == OBE_SYSTEM_TYPE_LOWEST_LATENCY,
                               "VBV buffer size is not user-settable in lowest-latency mode\n" );

                FAIL_IF_ERROR( frame_packing && ( check_enum_value( frame_packing, frame_packing_modes ) < 0 ),
                               "Invalid frame packing mode\n" )

                if( aspect_ratio )
                {
                    int ar_num, ar_den;
                    sscanf( aspect_ratio, "%d:%d", &ar_num, &ar_den );
                    if( ar_num == 16 && ar_den == 9 )
                        cli.output_streams[output_stream_id].is_wide = 1;
                    else if( ar_num == 4 && ar_den == 3 )
                        cli.output_streams[output_stream_id].is_wide = 0;
                    else
                    {
                        fprintf( stderr, "Aspect ratio is invalid\n" );
                        return -1;
                    }
                }

                if( width )
                {
                    int i_width = obe_otoi( width, avc_param->i_width );
                    while( allowed_resolutions[i][0] && ( allowed_resolutions[i][1] != avc_param->i_height ||
                           allowed_resolutions[i][0] != i_width ) )
                       i++;

                    FAIL_IF_ERROR( !allowed_resolutions[i][0], "Invalid resolution. \n" );
                    avc_param->i_width = i_width;
                }

                /* Set it to encode by default */
                cli.output_streams[output_stream_id].stream_action = STREAM_ENCODE;
                cli.output_streams[output_stream_id].video_bit_depth = n_bit_depth;

                if (video_codec_id == 0) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_AVC;
                } else
                if (video_codec_id == 1) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_HEVC_X265;
                } else
                if (video_codec_id == 2) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_AVC_VAAPI;
                } else
                if (video_codec_id == 3) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_HEVC_VAAPI;
                } else
                if (video_codec_id == 4) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_AVC_GPU_VAAPI_AVCODEC;
                } else
                if (video_codec_id == 5) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_HEVC_GPU_VAAPI_AVCODEC;
                } else
                if (video_codec_id == 6) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_AVC_CPU_AVCODEC;
                } else
                if (video_codec_id == 7) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_HEVC_CPU_AVCODEC;
                } else
                if (video_codec_id == 8) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_HEVC_GPU_NVENC_AVCODEC;
                } else
                if (video_codec_id == 10) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_HEVC_VEGA3311;
                } else
                if (video_codec_id == 11) {
                    cli.output_streams[output_stream_id].stream_format = VIDEO_AVC_VEGA3311;
                }

                avc_param->rc.i_vbv_max_bitrate = obe_otoi( vbv_maxrate, 0 );
                FAIL_IF_ERROR(avc_param->rc.i_vbv_max_bitrate < 0, "Invalid vbv_maxrate.\n" );
                avc_param->rc.i_vbv_buffer_size = obe_otoi( vbv_bufsize, 0 );
                FAIL_IF_ERROR(avc_param->rc.i_vbv_buffer_size < 0, "Invalid vbv_buffer_size.\n" );
                avc_param->rc.i_bitrate         = obe_otoi( bitrate, 0 );
                FAIL_IF_ERROR(avc_param->rc.i_bitrate < 0, "Invalid bitrate.\n" );
                avc_param->i_keyint_max        = obe_otoi( keyint, avc_param->i_keyint_max );
                avc_param->rc.i_lookahead      = obe_otoi( lookahead, avc_param->rc.i_lookahead );
                avc_param->i_threads           = obe_otoi( threads, avc_param->i_threads );
                avc_param->i_bframe            = obe_otoi( bframes, avc_param->i_bframe );
                avc_param->i_bframe_pyramid    = obe_otoi( b_pyramid, avc_param->i_bframe_pyramid );
                avc_param->analyse.i_weighted_pred = obe_otoi( weightp, avc_param->analyse.i_weighted_pred );
                avc_param->b_interlaced        = obe_otob( interlaced, avc_param->b_interlaced );
                avc_param->b_tff               = obe_otob( tff, avc_param->b_tff );
                avc_param->b_intra_refresh     = obe_otob( intra_refresh, avc_param->b_intra_refresh );
                avc_param->i_frame_reference   = obe_otoi( max_refs, avc_param->i_frame_reference );

                if (gop_closed && atoi(gop_closed) == 1) {
                        avc_param->b_open_gop = 0;
                }
                if( profile )
                    parse_enum_value( profile, x264_profile_names, &cli.avc_profile );

#if 0
// VAAPI
                avc_param->i_level_idc = 13;
#endif
                if( level )
                {
                    if( !strcasecmp( level, "1b" ) )
                        avc_param->i_level_idc = 9;
                    else if( obe_otof( level, 7.0 ) < 6 )
                        avc_param->i_level_idc = (int)( 10*obe_otof( level, 0.0 ) + .5 );
                    else
                        avc_param->i_level_idc = obe_otoi( level, avc_param->i_level_idc );
                }

                if( frame_packing )
                {
                    parse_enum_value( frame_packing, frame_packing_modes, &avc_param->i_frame_packing );
                    avc_param->i_frame_packing--;
                }

                if (csp) {
                    switch (atoi(csp)) {
                    default:
                    case 420:
                        avc_param->i_csp = X264_CSP_I420;
                        break;
                    case 422:
                        avc_param->i_csp = X264_CSP_I422;
                        break;
                    }
                    if( X264_BIT_DEPTH == 10 )
                        avc_param->i_csp |= X264_CSP_HIGH_DEPTH;
                }

                if (opencl)
                    avc_param->b_opencl = atoi(opencl);
                else
                    avc_param->b_opencl = 0;

                if( filler )
#if X264_BUILD < 148
                    avc_param->i_nal_hrd = obe_otob( filler, 0 ) ? X264_NAL_HRD_FAKE_CBR : X264_NAL_HRD_FAKE_VBR;
#else
                    avc_param->i_nal_hrd = obe_otob( filler, 0 ) ? X264_NAL_HRD_CBR : X264_NAL_HRD_VBR;
#endif

                /* Turn on the 3DTV mux option automatically */
                if( avc_param->i_frame_packing >= 0 )
                    cli.mux_opts.is_3dtv = 1;
            }
            else if( input_stream->stream_type == STREAM_TYPE_AUDIO )
            {
                int default_bitrate = 0, channel_map_idx = 0;
                uint64_t channel_layout;

                /* Set it to encode by default */
                cli.output_streams[output_stream_id].stream_action = STREAM_ENCODE;

                FAIL_IF_ERROR( action && ( check_enum_value( action, stream_actions ) < 0 ),
                              "Invalid stream action\n" );

                FAIL_IF_ERROR( format && ( check_enum_value(format, encode_formats ) < 0),
                              "Invalid stream format '%s'\n", format);

                FAIL_IF_ERROR( aac_profile && ( check_enum_value( aac_profile, aac_profiles ) < 0 ),
                              "Invalid aac encapsulation\n" );

                FAIL_IF_ERROR( aac_encap && ( check_enum_value( aac_encap, aac_encapsulations ) < 0 ),
                              "Invalid aac encapsulation\n" );

                FAIL_IF_ERROR( audio_type && check_enum_value( audio_type, audio_types ) < 0,
                              "Invalid audio type\n" );

                FAIL_IF_ERROR( audio_type && check_enum_value( audio_type, audio_types ) >= 0 &&
                               !cli.output_streams[output_stream_id].ts_opts.write_lang_code && !( lang && strlen( lang ) >= 3 ),
                               "Audio type requires setting a language\n" );

                FAIL_IF_ERROR( mp2_mode && check_enum_value( mp2_mode, mp2_modes ) < 0,
                              "Invalid MP2 mode\n" );

                FAIL_IF_ERROR( channel_map && check_enum_value( channel_map, channel_maps ) < 0,
                              "Invalid Channel Map\n" );

                FAIL_IF_ERROR( mono_channel && check_enum_value( mono_channel, mono_channels ) < 0,
                              "Invalid Mono channel selection\n" );

                int i_dialnorm = -31;
                if (dialnorm) {
                    i_dialnorm = atoi(dialnorm);
                }
                FAIL_IF_ERROR(((i_dialnorm < -31) || (i_dialnorm > 0)), "Invalid Dialnorm\n");

                if( action )
                    parse_enum_value( action, stream_actions, &cli.output_streams[output_stream_id].stream_action );
                if( format )
                    parse_enum_value( format, encode_formats, (int *)&cli.output_streams[output_stream_id].stream_format );
                if( audio_type )
                    parse_enum_value( audio_type, audio_types, &cli.output_streams[output_stream_id].ts_opts.audio_type );
                if( channel_map )
                    parse_enum_value( channel_map, channel_maps, &channel_map_idx );
                if( mono_channel )
                    parse_enum_value( mono_channel, mono_channels, &cli.output_streams[output_stream_id].mono_channel );

                channel_layout = channel_layouts[channel_map_idx];

                if (gain_db) {
                    /* Add the dB suffix so operators don't get this wrong */
                    sprintf(&cli.output_streams[output_stream_id].gain_db[0], "%sdB", gain_db);
                    cli.output_streams[output_stream_id].audioGain = 1.0;
                } else {
                    cli.output_streams[output_stream_id].gain_db[0] = 0;
                    cli.output_streams[output_stream_id].audioGain = 0.0;
                }

                cli.output_streams[output_stream_id].audio_remap_count = 0;
                if (audio_remap) {
                    /* Add the dB suffix so operators don't get this wrong */
                    strncpy(&cli.output_streams[output_stream_id].audio_remap[0],
                        audio_remap,
                        sizeof(cli.output_streams[output_stream_id].audio_remap));
                    cli.output_streams[output_stream_id].audioGain = 1.0;
                } else {
                    cli.output_streams[output_stream_id].audio_remap[0] = 0;
                }

                cli.output_streams[output_stream_id].audio_mute[0] = 0;
                if (audio_mute) {
                    /* Add the dB suffix so operators don't get this wrong */
                    strncpy(&cli.output_streams[output_stream_id].audio_mute[0],
                        audio_mute,
                        sizeof(cli.output_streams[output_stream_id].audio_mute));
                } else {
                    cli.output_streams[output_stream_id].audio_mute_count = 0;
                }

                if( cli.output_streams[output_stream_id].stream_format == AUDIO_MP2 )
                {
                    default_bitrate = 256;

                    FAIL_IF_ERROR( channel_map && av_get_channel_layout_nb_channels( channel_layout ) > 2,
                                   "MP2 audio does not support > 2 channels of audio\n" );

                    if( mp2_mode )
                        parse_enum_value( mp2_mode, mp2_modes, &cli.output_streams[output_stream_id].mp2_mode );
                }
                else if( cli.output_streams[output_stream_id].stream_format == AUDIO_AC_3 )
                    default_bitrate = 192;
                else if( cli.output_streams[output_stream_id].stream_format == AUDIO_AC_3_BITSTREAM) {
                    // Avoid a warning later
                    default_bitrate = 192;
                } else if( cli.output_streams[output_stream_id].stream_format == AUDIO_E_AC_3 )
                    default_bitrate = 192;
                else // AAC
                {
                    default_bitrate = 128;

                    if( aac_profile )
                        parse_enum_value( aac_profile, aac_profiles, &cli.output_streams[output_stream_id].aac_opts.aac_profile );

                    if( aac_encap )
                        parse_enum_value( aac_encap, aac_encapsulations, &cli.output_streams[output_stream_id].aac_opts.latm_output );
                }

                cli.output_streams[output_stream_id].audio_metadata.dialnorm = i_dialnorm;
                cli.output_streams[output_stream_id].bitrate = obe_otoi( bitrate, default_bitrate );
                cli.output_streams[output_stream_id].sdi_audio_pair = obe_otoi( sdi_audio_pair, cli.output_streams[output_stream_id].sdi_audio_pair );
                if( channel_map )
                    cli.output_streams[output_stream_id].channel_layout = channel_layout;

                if( lang && strlen( lang ) >= 3 )
                {
                    cli.output_streams[output_stream_id].ts_opts.write_lang_code = 1;
                    memcpy( cli.output_streams[output_stream_id].ts_opts.lang_code, lang, 3 );
                    cli.output_streams[output_stream_id].ts_opts.lang_code[3] = 0;
                }
                cli.output_streams[output_stream_id].audio_offset_ms =
                    obe_otoi(audio_offset, cli.output_streams[output_stream_id].audio_offset_ms);
            }
            else if( output_stream->stream_format == MISC_TELETEXT ||
                     output_stream->stream_format == VBI_RAW )
            {
                /* NB: remap these if more encoding options are added - TODO: split them up */
                char *ttx_lang = obe_get_option( stream_opts[32], opts );
                char *ttx_type = obe_get_option( stream_opts[33], opts );
                char *ttx_mag  = obe_get_option( stream_opts[34], opts );
                char *ttx_page = obe_get_option( stream_opts[35], opts );

                FAIL_IF_ERROR( ttx_type && ( check_enum_value( ttx_type, teletext_types ) < 0 ),
                               "Invalid Teletext type\n" );

                /* TODO: find a nice way of supporting multiple teletexts in the CLI */
                cli.output_streams[output_stream_id].ts_opts.num_teletexts = 1;

                if( cli.output_streams[output_stream_id].ts_opts.teletext_opts )
                    free( cli.output_streams[output_stream_id].ts_opts.teletext_opts );

                cli.output_streams[output_stream_id].ts_opts.teletext_opts = calloc( 1, sizeof(*cli.output_streams[output_stream_id].ts_opts.teletext_opts) );
                FAIL_IF_ERROR( !cli.output_streams[output_stream_id].ts_opts.teletext_opts, "malloc failed\n" );

                obe_teletext_opts_t *ttx_opts = &cli.output_streams[output_stream_id].ts_opts.teletext_opts[0];

                if( ttx_lang && strlen( ttx_lang ) >= 3 )
                {
                    memcpy( ttx_opts->dvb_teletext_lang_code, ttx_lang, 3 );
                    ttx_opts->dvb_teletext_lang_code[3] = 0;
                }
                if( ttx_type )
                    parse_enum_value( ttx_type, teletext_types, &ttx_opts->dvb_teletext_type );
                ttx_opts->dvb_teletext_magazine_number = obe_otoi( ttx_mag, ttx_opts->dvb_teletext_magazine_number );
                ttx_opts->dvb_teletext_page_number = obe_otoi( ttx_page, ttx_opts->dvb_teletext_page_number );

                if( output_stream->stream_format == VBI_RAW )
                {
                    obe_dvb_vbi_opts_t *vbi_opts = &cli.output_streams[output_stream_id].dvb_vbi_opts;
                    char *vbi_ttx = obe_get_option( stream_opts[36], opts );
                    char *vbi_inv_ttx = obe_get_option( stream_opts[37], opts );
                    char *vbi_vps  = obe_get_option( stream_opts[38], opts );
                    char *vbi_wss = obe_get_option( stream_opts[39], opts );

                    vbi_opts->ttx = obe_otob( vbi_ttx, vbi_opts->ttx );
                    vbi_opts->inverted_ttx = obe_otob( vbi_inv_ttx, vbi_opts->inverted_ttx );
                    vbi_opts->vps = obe_otob( vbi_vps, vbi_opts->vps );
                    vbi_opts->wss = obe_otob( vbi_wss, vbi_opts->wss );
                }
            }
            else if (output_stream->stream_format == SMPTE2031)
            {
                /* NB: remap these if more encoding options are added - TODO: split them up */
                char *ttx_lang = obe_get_option( stream_opts[32], opts );
                char *ttx_type = obe_get_option( stream_opts[33], opts );
                char *ttx_mag  = obe_get_option( stream_opts[34], opts );
                char *ttx_page = obe_get_option( stream_opts[35], opts );
                char *ttx_reverse = obe_get_option( stream_opts[47], opts );

                FAIL_IF_ERROR(ttx_type && (check_enum_value(ttx_type, teletext_types) < 0 ),
                               "Invalid Teletext type\n" );

                /* TODO: find a nice way of supporting multiple teletexts in the CLI */
                cli.output_streams[output_stream_id].ts_opts.num_teletexts = 1;

                if (cli.output_streams[output_stream_id].ts_opts.teletext_opts) {
                    free(cli.output_streams[output_stream_id].ts_opts.teletext_opts);
                }

                cli.output_streams[output_stream_id].ts_opts.teletext_opts = calloc(1, sizeof(*cli.output_streams[output_stream_id].ts_opts.teletext_opts));
                FAIL_IF_ERROR(!cli.output_streams[output_stream_id].ts_opts.teletext_opts, "malloc failed\n");

                obe_teletext_opts_t *ttx_opts = &cli.output_streams[output_stream_id].ts_opts.teletext_opts[0];

                if (ttx_lang && strlen(ttx_lang) >= 3) {
                    memcpy( ttx_opts->dvb_teletext_lang_code, ttx_lang, 3);
                    ttx_opts->dvb_teletext_lang_code[3] = 0;
                }

                if (ttx_type)
                    parse_enum_value(ttx_type, teletext_types, &ttx_opts->dvb_teletext_type);

                ttx_opts->dvb_teletext_magazine_number = obe_otoi( ttx_mag, ttx_opts->dvb_teletext_magazine_number ) % 8;
                ttx_opts->dvb_teletext_page_number = obe_otoi( ttx_page, ttx_opts->dvb_teletext_page_number );

                if (ttx_reverse) {
                    g_decklink_op47_teletext_reverse = atoi( ttx_reverse );
                }

                printf("SMPTE2031 establishining: ttx-type=%s (%d) ttx-lang=%s ttx-page=0x%x ttx-mag=0x%x ttx-reverse=%d\n",
                    ttx_opts->dvb_teletext_type == 2 ? "subtitle" : "undefined",
                    ttx_opts->dvb_teletext_type,
                    ttx_opts->dvb_teletext_lang_code,
                    ttx_opts->dvb_teletext_page_number,
                    ttx_opts->dvb_teletext_magazine_number,
                    g_decklink_op47_teletext_reverse);

            }

            cli.output_streams[output_stream_id].ts_opts.pid = obe_otoi( pid, cli.output_streams[output_stream_id].ts_opts.pid );
            obe_free_string_array( opts );
        }
    }

    return 0;
}

static int set_muxer( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "mpegts" ) )
        cli.mux_opts.muxer = MUXERS_MPEGTS;
    else if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        char *params = command + tok_len + 1;
        char **opts = obe_split_options( params, muxer_opts );
        if( !opts && params )
            return -1;

        char *ts_type     = obe_get_option( muxer_opts[0], opts );
        char *ts_cbr      = obe_get_option( muxer_opts[1], opts );
        char *ts_muxrate  = obe_get_option( muxer_opts[2], opts );
        char *passthrough = obe_get_option( muxer_opts[3], opts );
        char *ts_id       = obe_get_option( muxer_opts[4], opts );
        char *program_num = obe_get_option( muxer_opts[5], opts );
        char *pmt_pid     = obe_get_option( muxer_opts[6], opts );
        char *pcr_pid     = obe_get_option( muxer_opts[7], opts );
        char *pcr_period  = obe_get_option( muxer_opts[8], opts );
        char *pat_period  = obe_get_option( muxer_opts[9], opts );
        char *service_name  = obe_get_option( muxer_opts[10], opts );
        char *provider_name = obe_get_option( muxer_opts[11], opts );
        char *scte35_pid    = obe_get_option( muxer_opts[12], opts );
        char *smpte2038_pid = obe_get_option( muxer_opts[13], opts );
        char *sect_padding  = obe_get_option( muxer_opts[14], opts );
        char *smpte2031_pid = obe_get_option( muxer_opts[15], opts );

        FAIL_IF_ERROR( ts_type && ( check_enum_value( ts_type, ts_types ) < 0 ),
                      "Invalid AVC profile\n" );

        if( ts_type )
            parse_enum_value( ts_type, ts_types, &cli.mux_opts.ts_type );

        cli.mux_opts.cbr = obe_otob( ts_cbr, cli.mux_opts.cbr );
        cli.mux_opts.ts_muxrate = obe_otoi( ts_muxrate, cli.mux_opts.ts_muxrate );

        cli.mux_opts.passthrough = obe_otob( passthrough, cli.mux_opts.passthrough );
        cli.mux_opts.ts_id = obe_otoi( ts_id, cli.mux_opts.ts_id );
        cli.mux_opts.program_num = obe_otoi( program_num, cli.mux_opts.program_num );
        cli.mux_opts.pmt_pid    = obe_otoi( pmt_pid, cli.mux_opts.pmt_pid ) & 0x1fff;
        cli.mux_opts.pcr_pid    = obe_otoi( pcr_pid, cli.mux_opts.pcr_pid  ) & 0x1fff;
        cli.mux_opts.pcr_period = obe_otoi( pcr_period, cli.mux_opts.pcr_period );
        cli.mux_opts.pat_period = obe_otoi( pat_period, cli.mux_opts.pat_period );
#if MULTIPLE_SCTE35_PIDS
        /* Support parsing of multiple pids */
        if (scte35_pid)
        {
            int pidnr = 0;
            if (strstr(scte35_pid, ":") == 0) {
                cli.mux_opts.scte35_pids[0] = obe_otoi( scte35_pid, cli.mux_opts.scte35_pids[0] ) & 0x1fff;
                printf("sPID 0x%04x\n", pidnr);
                cli.h->enable_scte35 = 1;
            } else {
                cli.h->enable_scte35 = 0;
                char arg[255] = { 0 };
                strncpy(arg, scte35_pid, sizeof(arg));

                int index = 0;

                char *save;
                char *tok = strtok_r(&arg[0], ":", &save);
                while (tok != NULL) {
                    pidnr = atoi(tok) & 0x1fff;
                    printf("mPID 0x%04x\n", pidnr);
                    cli.mux_opts.scte35_pids[index++] = pidnr;
                    cli.h->enable_scte35++;
                    if (index == MAX_SCTE35_PIDS)
                        break;

                    tok = strtok_r(NULL, ":", &save);
                }
            }
//            exit(0);
        }
#else
        cli.mux_opts.scte35_pid = obe_otoi( scte35_pid, cli.mux_opts.scte35_pid ) & 0x1fff;
#endif
        cli.mux_opts.smpte2038_pid = obe_otoi( smpte2038_pid, cli.mux_opts.smpte2038_pid ) & 0x1fff;
        cli.mux_opts.smpte2031_pid = obe_otoi( smpte2031_pid, cli.mux_opts.smpte2031_pid ) & 0x1fff;
        cli.mux_opts.section_padding = obe_otoi( sect_padding, cli.mux_opts.section_padding );

        if( service_name )
        {
             if( cli.mux_opts.service_name )
                 free( cli.mux_opts.service_name );

             cli.mux_opts.service_name = malloc( strlen( service_name ) + 1 );
             FAIL_IF_ERROR( !cli.mux_opts.service_name, "malloc failed\n" );
             strcpy( cli.mux_opts.service_name, service_name );
        }
        if( provider_name )
        {
             if( cli.mux_opts.provider_name )
                 free( cli.mux_opts.provider_name );

             cli.mux_opts.provider_name = malloc( strlen( provider_name ) + 1 );
             FAIL_IF_ERROR( !cli.mux_opts.provider_name, "malloc failed\n" );
             strcpy( cli.mux_opts.provider_name, provider_name );
        }
        obe_free_string_array( opts );
    }

    return 0;
}

static int set_outputs( char *command, obecli_command_t *child )
{
    int num_outputs = 0;
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    command[tok_len] = 0;

    num_outputs = obe_otoi( command, num_outputs );

    FAIL_IF_ERROR( num_outputs <= 0, "Invalid number of outputs" );
    cli.output.outputs = calloc( num_outputs, sizeof(*cli.output.outputs) );
    FAIL_IF_ERROR( !cli.output.outputs, "Malloc failed" );
    cli.output.num_outputs = num_outputs;
    return 0;
}

#if DO_SET_VARIABLE
extern int g_decklink_monitor_hw_clocks;
extern int g_decklink_histogram_reset;
extern int g_decklink_histogram_print_secs;
extern int g_decklink_render_walltime;
extern int g_decklink_render_timecode;
extern int g_decklink_inject_scte104_preroll6000;
extern int g_decklink_inject_scte104_fragmented;
extern int g_decklink_udp_vanc_receiver_port;
extern int g_timecode_debug;
int g_json_stats_print_console = 0;
extern int g_timecode_trigger_discontinuity;

/* Case 1 */
extern int g_decklink_fake_lost_payload;
extern time_t g_decklink_fake_lost_payload_time;
extern int      g_decklink_burnwriter_enable;
extern uint32_t g_decklink_burnwriter_count;
extern uint32_t g_decklink_burnwriter_linenr;

/* Case 2 */
extern int g_decklink_fake_every_other_frame_lose_audio_payload;
extern time_t g_decklink_fake_every_other_frame_lose_audio_payload_time;

extern int    g_decklink_missing_audio_count;
extern time_t g_decklink_missing_audio_last_time;
extern int    g_decklink_missing_video_count;
extern time_t g_decklink_missing_video_last_time;

extern int g_decklink_record_audio_buffers;
extern unsigned int g_sdi_max_delay;

/* Case 4 audio/video clocks */
extern int64_t cur_pts; /* audio clock */
extern int64_t cpb_removal_time; /* Last video frame clock */

extern int64_t ac3_offset_ms;
extern int64_t mp2_offset_ms;

/* Mux */
extern int64_t initial_audio_latency;
extern int g_mux_audio_mp2_force_pmt_11172;

/* x265 */
extern int g_x265_monitor_bps;
extern int g_x265_nal_debug;
extern int g_x265_min_qp;
extern int g_x265_min_qp_new;
#if DEV_ABR
extern int g_x265_bitrate_bps;
extern int g_x265_bitrate_bps_new;
#endif

/* x264 */
extern int g_x264_monitor_bps;
extern int g_x264_nal_debug;
#if DEV_ABR
extern int g_x264_encode_alternate;
extern int g_x264_encode_alternate_new;
extern int g_x264_bitrate_bps;
extern int g_x264_bitrate_bps_new;
extern int g_x264_keyint_min;
extern int g_x264_keyint_min_new;
extern int g_x264_keyint_max;
extern int g_x264_keyint_max_new;
extern int g_x264_lookahead;
extern int g_x264_lookahead_new;
#endif

/* LAVC */
extern int g_audio_cf_debug;

/* SEI Timestamping. */
extern int g_sei_timestamping;

/* TS Mux */
extern int g_mux_ts_monitor_bps;
extern int64_t g_mux_dtstotal;

/* Mux Smoother */
extern int64_t g_mux_smoother_last_item_count;
extern int64_t g_mux_smoother_last_total_item_size;
extern int64_t g_mux_smoother_fifo_pcr_size;
extern int64_t g_mux_smoother_fifo_data_size;
extern int64_t g_mux_smoother_trim_ms;
extern int64_t g_mux_smoother_dump;

/* UDP Packet output */
extern int g_udp_output_drop_next_video_packet;
extern int g_udp_output_drop_next_audio_packet;
extern int g_udp_output_drop_next_packet;
extern int g_udp_output_drop_next_pat_packet;
extern int g_udp_output_drop_next_pmt_packet;
extern int g_udp_output_mangle_next_pmt_packet;
extern int g_udp_output_scramble_next_video_packet;
extern int g_udp_output_tei_next_packet;
extern int g_udp_output_bad_sync_next_packet;
extern int g_udp_output_stall_packet_ms;
extern int g_udp_output_latency_alert_ms;
extern int g_udp_output_bps;

/* LOS frame injection. */
extern int g_decklink_inject_frame_enable;
extern int g_decklink_injected_frame_count_max;
extern int g_decklink_injected_frame_count;

/* Core */
extern int g_core_runtime_statistics_to_file;
extern int g_core_runtime_terminate_after_seconds;
extern int g_core_runtime_statistics_to_port;

/* Filters */
extern int g_filter_audio_effect_pcm;
extern int g_filter_video_fullsize_jpg;
extern char g_filter_video_fullsize_jpg_filename[256];

/* HTTP */
extern char g_stats_http_push_address[128];

/* Ancillary data */
extern int g_ancillary_disable_captions;

void display_variables()
{
extern int    g_decklink_missing_audio_count;
extern time_t g_decklink_missing_audio_last_time;
extern int    g_decklink_missing_video_count;
extern time_t g_decklink_missing_video_last_time;
    printf("sdi_input.burnwriter_enable = %d [%s]\n",
        g_decklink_burnwriter_enable,
        g_decklink_burnwriter_enable == 0 ? "disabled" : "enabled");
    printf("sdi_input.burnwriter_linenr = %d\n", g_decklink_burnwriter_linenr);
    printf("sdi_input.inject_timecode = %d [%s]\n",
        g_decklink_render_timecode,
        g_decklink_render_timecode == 0 ? "disabled" : "enabled");
    printf("sdi_input.inject_walltime = %d [%s]\n",
        g_decklink_render_walltime,
        g_decklink_render_walltime == 0 ? "disabled" : "enabled");
    printf("sdi_input.inject_scte104_preroll6000 = %d\n",
        g_decklink_inject_scte104_preroll6000);
    printf("sdi_input.inject_scte104_fragmented = %d\n",
        g_decklink_inject_scte104_fragmented);
    printf("sdi_input.inject_frame_enable = %d [%s]\n",
        g_decklink_inject_frame_enable,
        g_decklink_inject_frame_enable == 0 ? "disabled" : "enabled");
    printf("sdi_input.inject_frame_count_max = %d\n", g_decklink_injected_frame_count_max);
    printf("sdi_input.fake_60sec_lost_payload = %d [%s]\n", g_decklink_fake_lost_payload,
        g_decklink_fake_lost_payload == 0 ? "disabled" : "enabled");
    printf("sdi_input.monitor_hw_clocks = %d [%s]\n",
        g_decklink_monitor_hw_clocks,
        g_decklink_monitor_hw_clocks == 0 ? "disabled" : "enabled");
    printf("sdi_input.fake_every_other_frame_lose_audio_payload = %d [%s]\n", g_decklink_fake_every_other_frame_lose_audio_payload,
        g_decklink_fake_every_other_frame_lose_audio_payload == 0 ? "disabled" : "enabled");
    printf("sdi_input.missing_audio_frame_count = %d -- last: %s",
        g_decklink_missing_audio_count,
        ctime(&g_decklink_missing_audio_last_time));
    printf("sdi_input.missing_video_frame_count = %d -- last: %s",
        g_decklink_missing_video_count,
        ctime(&g_decklink_missing_video_last_time));
    printf("sdi_input.record_audio_buffers = %d [%s]\n",
        g_decklink_record_audio_buffers,
        g_decklink_record_audio_buffers ? "enabled": "disabled");
    printf("sdi_input.max_frame_delay_before_error_us = %d\n",
        g_sdi_max_delay);
    printf("sdi_input.histogram_reset = %d\n",
        g_decklink_histogram_reset);
    printf("sdi_input.histogram_print_secs = %d\n",
        g_decklink_histogram_print_secs);
    printf("sdi_input.op47_teletext_reverse = %d\n",
        g_decklink_op47_teletext_reverse); 
    printf("ancillary.disable_captions = %d\n",
        g_ancillary_disable_captions);

    printf("audio_encoder.ac3_offset_ms = %" PRIi64 "\n", ac3_offset_ms);
    printf("audio_encoder.mp2_offset_ms = %" PRIi64 "\n", mp2_offset_ms);
    printf("audio_encoder.last_pts = %" PRIi64 "\n", cur_pts);

    printf("video_encoder.sei_timestamping = %d [%s]\n",
        g_sei_timestamping,
        g_sei_timestamping == 0 ? "disabled" : "enabled");

    printf("codec.x265.monitor_bps = %d [%s]\n",
        g_x265_monitor_bps,
        g_x265_monitor_bps == 0 ? "disabled" : "enabled");

    printf("codec.x265.nal_debug = %d [%s]\n",
        g_x265_nal_debug,
        g_x265_nal_debug == 0 ? "disabled" : "enabled");

    printf("codec.x264.monitor_bps = %d [%s]\n",
        g_x264_monitor_bps,
        g_x264_monitor_bps == 0 ? "disabled" : "enabled");

    printf("codec.x264.nal_debug = %d [%s]\n",
        g_x264_nal_debug,
        g_x264_nal_debug == 0 ? "disabled" : "enabled");

#if DEV_ABR
    printf("codec.x264.bitrate = %d (bps)\n", g_x264_bitrate_bps);
    printf("codec.x264.keyint_min = %d\n", g_x264_keyint_min);
    printf("codec.x264.keyint_max = %d\n", g_x264_keyint_max);
    printf("codec.x264.lookahead = %d\n", g_x264_lookahead);
    printf("codec.x264.encode_alternate = %d\n", g_x264_encode_alternate);
#endif

    printf("codec.audio.mp2.force_pmt_type_11172 = %d [%s]\n",
        g_mux_audio_mp2_force_pmt_11172,
        g_mux_audio_mp2_force_pmt_11172 == 0 ? "disabled" : "enabled");
    printf("codec.audio.debug = %d [%s]\n",
        g_audio_cf_debug,
        g_audio_cf_debug == 0 ? "disabled" : "enabled");

    printf("codec.x265.qpmin = %d\n", g_x265_min_qp);
#if DEV_ABR
    printf("codec.x265.bitrate = %d (bps)\n", g_x265_bitrate_bps);
#endif

    printf("video_encoder.last_pts = %" PRIi64 "\n", cpb_removal_time);
    printf("v - a                  = %" PRIi64 "  %" PRIi64 "(ms)\n", cpb_removal_time - cur_pts,
        (cpb_removal_time - cur_pts) / 27000);
    printf("a - v                  = %" PRIi64 "  %" PRIi64 "(ms)\n", cur_pts - cpb_removal_time,
        (cur_pts - cpb_removal_time) / 27000);

    printf("ts_mux.initial_audio_latency  = %" PRIi64 "\n", initial_audio_latency);
    printf("ts_mux.monitor_bps = %d [%s]\n",
        g_mux_ts_monitor_bps,
        g_mux_ts_monitor_bps == 0 ? "disabled" : "enabled");
    printf("ts_mux.pcr_adjustment  = %" PRIi64 " (ms)\n", g_mux_pcr_adjustment);

    printf("mux_smoother.last_item_count  = %" PRIi64 "\n",
        g_mux_smoother_last_item_count);
    printf("mux_smoother.last_total_item_size  = %" PRIi64 " (bytes)\n",
        g_mux_smoother_last_total_item_size);
    printf("mux_smoother.fifo_pcr_size         = %" PRIi64 " (bytes)\n",
        g_mux_smoother_fifo_pcr_size);
    printf("mux_smoother.fifo_data_size        = %" PRIi64 " (bytes)\n",
        g_mux_smoother_fifo_data_size);
    printf("mux_smoother.dump                  = %" PRIi64 "\n",
        g_mux_smoother_dump);
    printf("udp_output.drop_next_video_packet  = %d\n",
        g_udp_output_drop_next_video_packet);
    printf("udp_output.drop_next_audio_packet  = %d\n",
        g_udp_output_drop_next_audio_packet);
    printf("udp_output.drop_next_packet        = %d\n",
        g_udp_output_drop_next_packet);
    printf("udp_output.drop_next_pat_packet    = %d\n",
        g_udp_output_drop_next_pat_packet);
    printf("udp_output.drop_next_pmt_packet    = %d\n",
        g_udp_output_drop_next_pmt_packet);
    printf("udp_output.mangle_next_pmt_packet  = %d\n",
        g_udp_output_mangle_next_pmt_packet);
    printf("udp_output.scramble_next_video_packet = %d\n",
        g_udp_output_scramble_next_video_packet);
    printf("udp_output.tei_next_packet         = %d\n",
        g_udp_output_tei_next_packet);
    printf("udp_output.bad_sync_next_packet    = %d\n",
        g_udp_output_bad_sync_next_packet);
    printf("udp_output.stall_packet_ms         = %d\n",
        g_udp_output_stall_packet_ms);
    printf("udp_output.latency_alert_ms        = %d\n",
        g_udp_output_latency_alert_ms);
    printf("udp_output.bps                     = %d\n",
        g_udp_output_bps);
    printf("udp_output.transport_payload_size  = %d\n", obe_core_get_payload_size());
    printf("udp_output.trim_ms                 = %" PRIi64 "\n", g_mux_smoother_trim_ms);
    printf("core.runtime_statistics_to_file    = %d\n",
        g_core_runtime_statistics_to_file);
    printf("core.runtime_statistics_to_port    = %d\n",
        g_core_runtime_statistics_to_port);
    printf("core.runtime_statistics_to_url     = %s\n",
        g_stats_http_push_address);
    printf("core.runtime_statistics_print     = %d\n",
        g_json_stats_print_console);
    printf("core.runtime_terminate_after_seconds = %d\n",
        g_core_runtime_terminate_after_seconds);
    printf("filter.audio.pcm.adjustment        = 0x%08x (bitmask)",
        g_filter_audio_effect_pcm);
    if (g_filter_audio_effect_pcm & (1 << 0))
        printf(" MUTE_RIGHT");
    if (g_filter_audio_effect_pcm & (1 << 1))
        printf(" MUTE_LEFT");
    if (g_filter_audio_effect_pcm & (1 << 2))
        printf(" STATIC_RIGHT");
    if (g_filter_audio_effect_pcm & (1 << 3))
        printf(" STATIC_LEFT");
    if (g_filter_audio_effect_pcm & (1 << 4))
        printf(" BUZZ_RIGHT");
    if (g_filter_audio_effect_pcm & (1 << 5))
        printf(" BUZZ_LEFT");
    if (g_filter_audio_effect_pcm & (1 << 6))
        printf(" ATTENUATE_RIGHT");
    if (g_filter_audio_effect_pcm & (1 << 7))
        printf(" ATTENUATE_LEFT");
    if (g_filter_audio_effect_pcm & (1 << 8))
        printf(" CLIP_RIGHT");
    if (g_filter_audio_effect_pcm & (1 << 9))
        printf(" CLIP_LEFT");
    printf("\n");
    if (g_filter_video_fullsize_jpg) {
        printf("filter.video.create_fullsize_jpg   = %s\n",
            g_filter_video_fullsize_jpg_filename);
    } else {
        printf("filter.video.create_fullsize_jpg   = DISABLED\n");
    }

    /* SCTE 104 / 35 filtering */
    printf("scte104.filters.clear              = 0\n");
    printf("scte104.filtercount                = %d\n", g_scte104_filtering_context.count);

    for (int i = 0; i < g_scte104_filtering_context.count; i++) {
        printf("scte104.filter[%d] = PID: 0x%04x (%5d) AS_index:%5d DPI_PID_index:%6d\n",
            i,
            g_scte104_filtering_context.array[i].pid,
            g_scte104_filtering_context.array[i].pid,
            g_scte104_filtering_context.array[i].AS_index,
            g_scte104_filtering_context.array[i].DPI_PID_index);
    }

    printf("vanc_receiver.udp_port             = %d\n", g_decklink_udp_vanc_receiver_port);
    printf("timecode.debug                     = %d\n", g_timecode_debug);
    printf("timecode.force_discontinuity       = %d\n", g_timecode_trigger_discontinuity);
}

extern char *strcasestr(const char *haystack, const char *needle);

static int set_variable(char *command, obecli_command_t *child)
{
    int64_t val = 0;
    char var[256];
    char vars[256];

    if (!strlen(command)) {
        /* Missing arg, display the current value. */
        display_variables();
        return 0;
    }

    if (sscanf(command, "%s = %" PRIi64, &var[0], &val) != 2) {
        if (strncasecmp(var, "scte104.filter.add", 18) == 0) {
            strcpy(&vars[0], &command[20]);
            /* handle this below */
        }
        else
        if (strncasecmp(var, "filter.video.create_fullsize_jpg", 32) == 0) {
            strcpy(&vars[0], &command[35]);
            /* handle this below */
        } else
        if (strncasecmp(var, "core.runtime_statistics_to_url", 30) == 0) {
            strcpy(&vars[0], &command[33]);
            /* handle this below */
        } else {
            printf("illegal variable name.\n");
            return -1;
        }
    }

    if (strcasecmp(var, "sdi_input.fake_60sec_lost_payload") == 0) {
        printf("setting %s to %" PRIi64 "\n", var, val);
        g_decklink_fake_lost_payload = val;
        if (val == 0)
            g_decklink_fake_lost_payload_time = 0;
    } else
    if (strcasecmp(var, "sdi_input.burnwriter_enable") == 0) {
        g_decklink_burnwriter_count = 0;
        g_decklink_burnwriter_enable = val;
    } else
    if (strcasecmp(var, "sdi_input.burnwriter_linenr") == 0) {
        g_decklink_burnwriter_linenr = val;
    } else
    if (strcasecmp(var, "sdi_input.inject_walltime") == 0) {
        g_decklink_render_walltime = val;
    } else
    if (strcasecmp(var, "sdi_input.inject_timecode") == 0) {
        g_decklink_render_timecode = val;
    } else
    if (strcasecmp(var, "sdi_input.inject_scte104_preroll6000") == 0) {
        g_decklink_inject_scte104_preroll6000 = val;
    } else
    if (strcasecmp(var, "sdi_input.inject_scte104_fragmented") == 0) {
        g_decklink_inject_scte104_fragmented = val;
    } else
    if (strcasecmp(var, "sdi_input.fake_every_other_frame_lose_audio_payload") == 0) {
        g_decklink_fake_every_other_frame_lose_audio_payload = val;
        g_decklink_fake_every_other_frame_lose_audio_payload_time = 0;
    } else
    if (strcasecmp(var, "sdi_input.inject_frame_enable") == 0) {
        g_decklink_injected_frame_count = 0;
        g_decklink_inject_frame_enable = val;
    } else
    if (strcasecmp(var, "sdi_input.inject_frame_count_max") == 0) {
        g_decklink_injected_frame_count_max = val;
    } else
    if (strcasecmp(var, "sdi_input.monitor_hw_clocks") == 0) {
        g_decklink_monitor_hw_clocks = val;
    } else
    if (strcasecmp(var, "sdi_input.record_audio_buffers") == 0) {
        g_decklink_record_audio_buffers = val;
    } else
    if (strcasecmp(var, "sdi_input.max_frame_delay_before_error_us") == 0) {
        g_sdi_max_delay = val;
    } else
    if (strcasecmp(var, "sdi_input.histogram_reset") == 0) {
        g_decklink_histogram_reset = val;
    } else
    if (strcasecmp(var, "sdi_input.histogram_print_secs") == 0) {
        g_decklink_histogram_print_secs = val;
    } else
    if (strcasecmp(var, "sdi_input.op47_teletext_reverse") == 0) {
        g_decklink_op47_teletext_reverse = val;
    } else
    if (strcasecmp(var, "audio_encoder.ac3_offset_ms") == 0) {
        ac3_offset_ms = val;
    } else
    if (strcasecmp(var, "audio_encoder.mp2_offset_ms") == 0) {
        mp2_offset_ms = val;
    } else
    if (strcasecmp(var, "udp_output.drop_next_video_packet") == 0) {
        g_udp_output_drop_next_video_packet = val;
    } else
    if (strcasecmp(var, "udp_output.drop_next_audio_packet") == 0) {
        g_udp_output_drop_next_audio_packet = val;
    } else
    if (strcasecmp(var, "udp_output.drop_next_packet") == 0) {
        g_udp_output_drop_next_packet = val;
    } else
    if (strcasecmp(var, "udp_output.drop_next_pat_packet") == 0) {
        g_udp_output_drop_next_pat_packet = val;
    } else
    if (strcasecmp(var, "udp_output.drop_next_pmt_packet") == 0) {
        g_udp_output_drop_next_pmt_packet = val;
    } else
    if (strcasecmp(var, "udp_output.drop_next_pmt_packet") == 0) {
        g_udp_output_drop_next_pmt_packet = val;
    } else
    if (strcasecmp(var, "udp_output.mangle_next_pmt_packet") == 0) {
        g_udp_output_mangle_next_pmt_packet = val;
    } else
    if (strcasecmp(var, "udp_output.scramble_next_video_packet") == 0) {
        g_udp_output_scramble_next_video_packet = val;
    } else
    if (strcasecmp(var, "udp_output.tei_next_packet") == 0) {
        g_udp_output_tei_next_packet = val;
    } else
    if (strcasecmp(var, "udp_output.bad_sync_next_packet") == 0) {
        g_udp_output_bad_sync_next_packet = val;
    } else
    if (strcasecmp(var, "udp_output.stall_packet_ms") == 0) {
        g_udp_output_stall_packet_ms = val;
    } else
    if (strcasecmp(var, "udp_output.latency_alert_ms") == 0) {
        g_udp_output_latency_alert_ms = val;
    } else
    if (strcasecmp(var, "udp_output.trim_ms") == 0) {
        g_mux_smoother_trim_ms = sanitizeParamTrim(val);
    } else
    if (strcasecmp(var, "vanc_receiver.udp_port") == 0) {
        g_decklink_udp_vanc_receiver_port = val;
    } else
    if (strcasecmp(var, "codec.x265.monitor_bps") == 0) {
        g_x265_monitor_bps = val;
    } else
    if (strcasecmp(var, "codec.x264.monitor_bps") == 0) {
        g_x264_monitor_bps = val;
    } else
    if (strcasecmp(var, "codec.x264.nal_debug") == 0) {
        g_x264_nal_debug = val;
    } else
#if DEV_ABR
    if (strcasecmp(var, "codec.x264.bitrate") == 0) {
        g_x264_bitrate_bps = val;
        g_x264_bitrate_bps_new = 1;
    } else
    if (strcasecmp(var, "codec.x264.keyint_min") == 0) {
        g_x264_keyint_min = val;
        g_x264_keyint_min_new = 1;
    } else
    if (strcasecmp(var, "codec.x264.keyint_max") == 0) {
        g_x264_keyint_max = val;
        g_x264_keyint_max_new = 1;
    } else
    if (strcasecmp(var, "codec.x264.lookahead") == 0) {
        g_x264_lookahead = val;
        g_x264_lookahead_new = 1;
    } else
    if (strcasecmp(var, "codec.x264.encode_alternate") == 0) {
        g_x264_encode_alternate = val;
        g_x264_encode_alternate_new = 1;
    } else
#endif
    if (strcasecmp(var, "codec.audio.mp2.force_pmt_type_11172") == 0) {
        g_mux_audio_mp2_force_pmt_11172 = val;
    } else
    if (strcasecmp(var, "codec.audio.debug") == 0) {
        g_audio_cf_debug = val;
    } else
    if (strcasecmp(var, "codec.x265.nal_debug") == 0) {
        g_x265_nal_debug = val;
    } else
    if (strcasecmp(var, "codec.x265.qpmin") == 0) {
        g_x265_min_qp = val;
        g_x265_min_qp_new = 1;
    } else
#if DEV_ABR
    if (strcasecmp(var, "codec.x265.bitrate") == 0) {
        g_x265_bitrate_bps = val;
        g_x265_bitrate_bps_new = 1;
    } else
#endif
    if (strcasecmp(var, "udp_output.transport_payload_size") == 0) {
        obe_core_set_payload_size(val);
    } else
    if (strcasecmp(var, "core.runtime_statistics_to_file") == 0) {
        g_core_runtime_statistics_to_file = val;
    } else
    if (strcasecmp(var, "core.runtime_statistics_to_port") == 0) {
        g_core_runtime_statistics_to_port = val;
    } else
    if (strcasecmp(var, "core.runtime_statistics_to_url") == 0) {
        strncpy(&g_stats_http_push_address[0], vars, 128);
    } else
    if (strcasecmp(var, "core.runtime_statistics_print") == 0) {
        g_json_stats_print_console = val;
    } else
    if (strcasecmp(var, "core.runtime_terminate_after_seconds") == 0) {
        g_core_runtime_terminate_after_seconds = val;
    } else
    if (strcasecmp(var, "mux_smoother.dump") == 0) {
        g_mux_smoother_dump = val;
    } else
    if (strcasecmp(var, "ts_mux.monitor_bps") == 0) {
        g_mux_ts_monitor_bps = val;
    } else
    if (strcasecmp(var, "ts_mux.pcr_adjustment") == 0) {
        g_mux_pcr_adjustment = val;
    } else
    if (strcasecmp(var, "video_encoder.sei_timestamping") == 0) {
        g_sei_timestamping = val;
    } else
    if (strcasecmp(var, "filter.audio.pcm.adjustment") == 0) {
        g_filter_audio_effect_pcm = val;
    } else
    if (strcasecmp(var, "filter.video.create_fullsize_jpg") == 0) {
        g_filter_video_fullsize_jpg = 1;
        strncpy(&g_filter_video_fullsize_jpg_filename[0], vars, 256);
    } else
    if (strcasecmp(var, "ancillary.disable_captions") == 0) {
        g_ancillary_disable_captions = val;
    } else
    if (strcasecmp(var, "timecode.debug") == 0) {
        g_timecode_debug = val;
    } else
    if (strcasecmp(var, "timecode.force_discontinuity") == 0) {
        g_timecode_trigger_discontinuity = val;
    } else
    if (strcasecmp(var, "scte104.filters.clear") == 0) {
        scte104filter_init(&g_scte104_filtering_context);
    } else
    if (strcasecmp(var, "scte104.filter.add") == 0) {
        /* convert string pid = 56, AS_index = all, DPI_PID_index = 1 */

        int asi = -2, dpi = -2, pid = -2;

        char *save[2];
        char *s = strtok_r(vars, ",", &save[0]);
        while (s) {
            //printf("s = %s\n", s);
            char *key = strtok_r(s, "=", &save[1]);
            char *val = strtok_r(NULL, "=", &save[1]);

            //printf("key = %s\n", key);
            //printf("val = %s\n", val);

            if (strcasestr(key, "DPI_PID_index")) {
                if (strcasestr(val, "all")) {
                    dpi = -1;
                } else {
                    dpi = atoi(val);
                    if (dpi < 0 || dpi > 65535) {
                        printf("malformed dpi, aborting\n");
                        return -1;
                    }
                }
            } else
            if (strcasestr(key, "AS_index")) {
                if (strcasestr(val, "all")) {
                    asi = -1;
                } else {
                    asi = atoi(val);
                    if (asi < 0 || asi > 255) {
                        printf("malformed asi, aborting\n");
                        return -1;
                    }
                }
            } else
            if (strcasestr(key, "pid")) {
                pid = atoi(val);
                if (pid < 16 || pid > 8191) {
                    printf("malformed pid, aborting\n");
                    return -1;
                }
            } else {
                printf("malformed command arguments, unable to parse fields\n");
                return -1;
            }

            s = strtok_r(NULL, ",", &save[0]);
        }

        if (pid == -2 || asi == -2 || dpi == -2) {
            printf("malformed command arguments, unable to parse fields\n");
            return -1;
        }
        int ret = scte104filter_add_filter(&g_scte104_filtering_context, pid, asi, dpi);
        if (ret < 0) {
            printf("Malformed command, aborting\n");
            return -1;
        }

    } else {
        printf("illegal variable name '%s'.\n", var);
        return -1;
    }

    //if (sscanf(command, "0x%x", &bitmask) != 1)
    //    return -1;

    return 0;
}
#endif

static void display_verbose()
{
    uint32_t bm = cli.h->verbose_bitmask;
    printf("verbose = 0x%08x\n", cli.h->verbose_bitmask);

#define DISPLAY_VERBOSE_MASK(bm, mask) \
	printf("\t%s = %s\n", #mask, bm & mask ? "enabled" : "disabled");
    /* INPUT SOURCES */
    DISPLAY_VERBOSE_MASK(bm, INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY);
    DISPLAY_VERBOSE_MASK(bm, INPUTSOURCE__SDI_VANC_DISCOVERY_SCTE104);
    DISPLAY_VERBOSE_MASK(bm, INPUTSOURCE__SDI_VANC_DISCOVERY_RDD8);

    /* MUXER */
    DISPLAY_VERBOSE_MASK(bm, MUX__DQ_HEXDUMP);
    DISPLAY_VERBOSE_MASK(bm, MUX__PTS_REPORT_TIMES);
    DISPLAY_VERBOSE_MASK(bm, MUX__REPORT_Q);
}

static int set_verbose(char *command, obecli_command_t *child)
{
    unsigned int bitmask = 0;
    if (!strlen(command)) {
        /* Missing arg, display the current value. */
        display_verbose();
        return 0;
    }

    int tok_len = strcspn(command, " ");
    command[tok_len] = 0;

    if (sscanf(command, "0x%x", &bitmask) != 1)
        return -1;

    cli.h->verbose_bitmask = bitmask;
    return 0;
}

static int set_output( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    if( !strcasecmp( command, "opts" ) && str_len > tok_len )
    {
        command += tok_len+1;
        int tok_len2 = strcspn( command, ":" );
        command[tok_len2] = 0;
        int output_id = obe_otoi( command, -1 );
        FAIL_IF_ERROR( output_id < 0 || output_id > cli.output.num_outputs-1, "Invalid output id\n" );

        char *params = command + tok_len2 + 1;
        char **opts = obe_split_options( params, output_opts );
        if( !opts && params )
            return -1;

        char *type = obe_get_option( output_opts[0], opts );
        char *target = obe_get_option( output_opts[1], opts );
        char *trim = obe_get_option( output_opts[2], opts );
        if (trim) {
            g_mux_smoother_trim_ms = sanitizeParamTrim(atoi(trim));
        }

        FAIL_IF_ERROR( type && ( check_enum_value( type, output_modules ) < 0 ),
                      "Invalid Output Type\n" );

        if( type )
            parse_enum_value( type, output_modules, &cli.output.outputs[output_id].type );
        if( target )
        {
             if( cli.output.outputs[output_id].target )
                 free( cli.output.outputs[output_id].target );

             cli.output.outputs[output_id].target = malloc( strlen( target ) + 1 );
             FAIL_IF_ERROR( !cli.output.outputs[output_id].target, "malloc failed\n" );
             strcpy( cli.output.outputs[output_id].target, target );
        }
        obe_free_string_array( opts );
    }

    return 0;
}


/* show functions */
static int show_bitdepth( char *command, obecli_command_t *child )
{
    printf( "AVC output bit depth: %i bits per sample\n", X264_BIT_DEPTH );

    return 0;
}

static int show_decoders( char *command, obecli_command_t *child )
{
    printf( "\nSupported Decoders: \n" );

    for( int i = 0; format_names[i].decoder_name != 0; i++ )
    {
        if( strcmp( format_names[i].decoder_name, "N/A" ) )
            printf( "       %-*s %-*s - %s \n", 7, format_names[i].format_name, 22, format_names[i].long_name, format_names[i].decoder_name );
    }

    return 0;
}

static int show_queues(char *command, obecli_command_t *child)
{
    printf( "Global queues:\n" );

    obe_filter_t *f = NULL;
    obe_queue_t *q = NULL;

    {
        q = &cli.h->enc_smoothing_queue;
        printf("name: %s depth: %d item(s)\n", q->name, q->size);
extern void encoder_smoothing_dump(obe_t *h);
        encoder_smoothing_dump(cli.h);
    }
    {
        q = &cli.h->mux_queue;
        printf("name: %s depth: %d item(s)\n", q->name, q->size);
extern void mux_dump_queue(obe_t *h);
        mux_dump_queue(cli.h);
    }

    q = &cli.h->mux_smoothing_queue;
    printf("name: %s depth: %d item(s)\n", q->name, q->size);

    printf( "Filter queues:\n" );
    for (int i = 0; i < cli.h->num_filters; i++) {
        f = cli.h->filters[i];
        printf("name: %s depth: %d item(s)\n", f->queue.name, f->queue.size);
    }

    printf( "Output queues:\n" );
    for (int i = 0; i < cli.h->num_outputs; i++) {
        q = &cli.h->outputs[i]->queue;
        printf("name: %s depth: %d item(s)\n", q->name, q->size);
    }

    printf( "Encoder queues:\n" );
    for( int i = 0; i < cli.h->num_output_streams; i++ ) {
        obe_output_stream_t *e = obe_core_get_output_stream_by_index(cli.h, i);
        if (e && e->stream_action == STREAM_ENCODE ) {
            q = &cli.h->encoders[i]->queue;
            printf("name: %s depth: %d item(s)\n", q->name, q->size);
        }
    }

extern ts_writer_t *g_mux_ts_writer_handle;
    ts_show_queues(g_mux_ts_writer_handle);

extern void hevc_show_stats();
    hevc_show_stats();

    return 0;
}

static int show_encoders( char *command, obecli_command_t *child )
{
    printf( "\nSupported Encoders: \n" );

    for( int i = 0; format_names[i].encoder_name != 0; i++ )
    {
        if( strcmp( format_names[i].encoder_name, "N/A" ) )
            printf( "       %-*s %-*s - %s \n", 7, format_names[i].format_name, 22, format_names[i].long_name, format_names[i].encoder_name );
    }

    return 0;
}

static int show_help( char *command, obecli_command_t *child )
{

#define H0 printf
    H0( "OBE Commands:\n" );

    H0( "\n" );

    H0( "show - Show supported items\n" );
    for( int i = 0; show_commands[i].name != 0; i++ )
        H0( "       %-*s %-*s  - %s \n", 8, show_commands[i].name, 21, show_commands[i].child_opts, show_commands[i].description );

    H0( "\n" );

    H0( "add  - Add item\n" );
    for( int i = 0; add_commands[i].name != 0; i++ )
    {
        H0( "       %-*s          - %s \n", 8, add_commands[i].name, add_commands[i].description );
    }

    H0( "\n" );

#if 0
    H0( "load - Load configuration\n" );
#endif

    H0( "set  - Set parameter\n" );
    for( int i = 0; set_commands[i].name != 0; i++ )
        H0( "       %-*s %-*s  - %s \n", 8, set_commands[i].name, 21, set_commands[i].child_opts, set_commands[i].description );

    H0( "\n" );

    H0( "Starting/Stopping OBE:\n" );
    H0( "start - Start encoding\n" );
    H0( "stop  - Stop encoding\n" );

    H0( "\n" );

    return 0;
}

static int show_input( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    command[tok_len] = 0;

    if( !strcasecmp( command, "streams" ) )
        return show_input_streams( NULL, NULL );

    return -1;
}

static int show_inputs( char *command, obecli_command_t *child )
{
    int i = 0;

    printf( "\nSupported Inputs: \n" );

    while( input_names[i].input_name )
    {
        printf( "       %-*s          - %s \n", 8, input_names[i].input_name, input_names[i].input_lib_name );
        i++;
    }

    return 0;
}

static int show_muxers( char *command, obecli_command_t *child )
{
    int i = 0;

    printf( "\nSupported Muxers: \n" );

    while( muxer_names[i].muxer_name )
    {
        printf( "       %-*s          - %s \n", 8, muxer_names[i].muxer_name, muxer_names[i].mux_lib_name );
        i++;
    }

    return 0;
}

static int show_output( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    command[tok_len] = 0;

    if( !strcasecmp( command, "streams" ) )
        return show_output_streams( NULL, NULL );

    return -1;
}

static int show_outputs( char *command, obecli_command_t *child )
{
    int i = 0;

    printf( "\nSupported Outputs: \n" );

    while( output_names[i].output_name )
    {
        printf( "       %-*s          - %s \n", 8, output_names[i].output_name, output_names[i].output_lib_name );
        i++;
    }

    return 0;
}

static int show_input_streams( char *command, obecli_command_t *child )
{
    obe_input_stream_t *stream;
    char buf[200];
    char *format_name;

    printf( "\n" );

    if( !cli.program.num_streams )
    {
        printf( "No input streams. Please probe a device" );
        return -1;
    }

    printf( "Detected input streams: \n" );

    for( int i = 0; i < cli.program.num_streams; i++ )
    {
        stream = &cli.program.streams[i];
        format_name = get_format_name( stream->stream_format, format_names, 0 );
        if( stream->stream_type == STREAM_TYPE_VIDEO )
        {
            /* TODO: show profile, level, csp etc */
            printf( "Input-stream-id: %d - Video: %s %dx%d%s %d/%dfps %s\n", stream->input_stream_id,
                    format_name, stream->width, stream->height, stream->interlaced ? "i" : "p",
                    stream->timebase_den, stream->timebase_num,
                    stream->is_hdr ? "HDR" : "SDR");

            for( int j = 0; j < stream->num_frame_data; j++ )
            {
                format_name = get_format_name( stream->frame_data[j].type, format_names, 1 );
                /* TODO make this use the proper names */
                printf( "                     %s:   %s\n", stream->frame_data[j].source == MISC_WSS ? "WSS (converted)" :
                        stream->frame_data[j].source == VBI_RAW ? "VBI" : stream->frame_data[j].source == VBI_VIDEO_INDEX ? "VII" : "VANC", format_name );
            }
        }
        else if (stream->stream_type == STREAM_TYPE_AUDIO && stream->stream_format == AUDIO_AC_3_BITSTREAM) {
            printf( "Input-stream-id: %d - Audio: %s digital bitstream - SDI audio pair: %d\n", stream->input_stream_id, format_name, stream->sdi_audio_pair);
        }
        else if( stream->stream_type == STREAM_TYPE_AUDIO )
        {
            if( !stream->channel_layout )
                snprintf( buf, sizeof(buf), "%2i channels", stream->num_channels );
            else
                av_get_channel_layout_string( buf, sizeof(buf), 0, stream->channel_layout );
            printf( "Input-stream-id: %d - Audio: %s%s %s %ikHz - SDI audio pair: %d\n", stream->input_stream_id, format_name,
                    stream->stream_format == AUDIO_AAC ? stream->aac_is_latm ? " LATM" : " ADTS" : "",
                    buf, stream->sample_rate / 1000,
                    stream->sdi_audio_pair);
        }
        else if( stream->stream_format == SUBTITLES_DVB )
        {
            printf( "Input-stream-id: %d - DVB Subtitles: Language: %s DDS: %s \n", stream->input_stream_id, stream->lang_code,
                    stream->dvb_has_dds ? "yes" : "no" );
        }
        else if( stream->stream_format == MISC_TELETEXT )
        {
            printf( "Input-stream-id: %d - Teletext: \n", stream->input_stream_id );
        }
        else if( stream->stream_format == VBI_RAW )
        {
            printf( "Input-stream-id: %d - VBI: \n", stream->input_stream_id );
            for( int j = 0; j < stream->num_frame_data; j++ )
            {
                format_name = get_format_name( stream->frame_data[j].type, format_names, 1 );
                printf( "               %s:   %s\n", stream->frame_data[j].source == VBI_RAW ? "VBI" : "", format_name );
            }
        }
        else if(stream->stream_format == DVB_TABLE_SECTION)
        {
            printf( "Input-stream-id: %d - DVB Table Section\n", stream->input_stream_id);
        }
        else if(stream->stream_format == SMPTE2038)
        {
            printf( "Input-stream-id: %d - PES_PRIVATE_1 SMPTE2038\n", stream->input_stream_id);
        }
        else if(stream->stream_format == SMPTE2031)
        {
            printf( "Input-stream-id: %d - PES_PRIVATE_1 SMPTE2031\n", stream->input_stream_id);
        }
        else
            printf( "Input-stream-id: %d \n", stream->input_stream_id );
    }

    printf("\n");

    return 0;
}

static int show_output_streams( char *command, obecli_command_t *child )
{
    obe_input_stream_t *input_stream;
    obe_output_stream_t *output_stream;
    char *format_name;

    printf( "Encoder outputs: \n" );

    for( int i = 0; i < cli.num_output_streams; i++ )
    {
        output_stream = &cli.output_streams[i];
        input_stream = &cli.program.streams[output_stream->input_stream_id];
        printf( "Output-stream-id: %d - Input-stream-id: %d - ", output_stream->output_stream_id, output_stream->input_stream_id );

        if( output_stream->stream_format == MISC_TELETEXT )
            printf( "DVB-Teletext\n" );
        else if( output_stream->stream_format == VBI_RAW )
            printf( "DVB-VBI\n" );
        else if (input_stream->stream_type == STREAM_TYPE_VIDEO)
        {
            if (output_stream->stream_format == VIDEO_AVC)
                printf( "Video: AVC\n" );
            else if (output_stream->stream_format == VIDEO_HEVC_X265)
                printf( "Video: HEVC (X265)\n" );
            else if (output_stream->stream_format == VIDEO_AVC_VAAPI)
                printf( "Video: AVC (VAAPI)\n" );
            else if (output_stream->stream_format == VIDEO_HEVC_VAAPI)
                printf( "Video: HEVC (VAAPI)\n" );
            else if (output_stream->stream_format == VIDEO_AVC_CPU_AVCODEC)
                printf( "Video: AVC (AVCODEC CPU)\n" );
            else if (output_stream->stream_format == VIDEO_AVC_GPU_VAAPI_AVCODEC)
                printf( "Video: AVC (AVCODEC GPU VAAPI)\n" );
            else if (output_stream->stream_format == VIDEO_HEVC_GPU_VAAPI_AVCODEC)
                printf( "Video: HEVC (AVCODEC GPU VAAPI)\n" );
            else if (output_stream->stream_format == VIDEO_HEVC_GPU_NVENC_AVCODEC)
                printf( "Video: HEVC (AVCODEC GPU NVENC)\n" );
            else if (output_stream->stream_format == VIDEO_HEVC_CPU_AVCODEC)
                printf( "Video: HEVC (AVCODEC CPU)\n" );
            else 
                printf( "Video: AVC OR HEVC\n");
        }
        else if( input_stream->stream_type == STREAM_TYPE_AUDIO )
        {
            format_name = get_format_name( cli.output_streams[i].stream_format, format_names, 0 );
            printf( "Audio: %s - SDI audio pair: %d \n", format_name, cli.output_streams[i].sdi_audio_pair );
        }
        else if(input_stream->stream_type == STREAM_TYPE_MISC && input_stream->stream_format == DVB_TABLE_SECTION)
        {
            /* SCTE35 - almost certainly */
            printf("PSIP: DVB_TABLE_SECTION\n");
        }
        else if(input_stream->stream_type == STREAM_TYPE_MISC && input_stream->stream_format == SMPTE2038)
        {
            printf("PES_PRIVATE_1: SMPTE2038 packets\n");
        }
        else if(input_stream->stream_type == STREAM_TYPE_MISC && input_stream->stream_format == SMPTE2031)
        {
            printf("PES_PRIVATE_1: SMPTE2031 packets\n");
        }

    }

    printf( "\n" );

    return 0;
}

static int start_encode( char *command, obecli_command_t *child )
{
    obe_input_stream_t *input_stream;
    obe_output_stream_t *output_stream;
    FAIL_IF_ERROR( g_running, "Encoder already running\n" );
    FAIL_IF_ERROR( !cli.program.num_streams, "No active devices\n" );

    int scte_index = 0;
    for( int i = 0; i < cli.num_output_streams; i++ )
    {
        output_stream = &cli.output_streams[i];
        if( output_stream->input_stream_id >= 0 )
            input_stream = &cli.program.streams[output_stream->input_stream_id];
        else
            input_stream = NULL;
        if (input_stream->stream_type == STREAM_TYPE_MISC && input_stream->stream_format == SMPTE2038) {
            if (cli.mux_opts.smpte2038_pid)
                output_stream->ts_opts.pid = cli.mux_opts.smpte2038_pid;
        } else
        if (input_stream->stream_type == STREAM_TYPE_MISC && input_stream->stream_format == SMPTE2031) {
            if (cli.mux_opts.smpte2031_pid)
                output_stream->ts_opts.pid = cli.mux_opts.smpte2031_pid;
        } else
        if (input_stream->stream_format == DVB_TABLE_SECTION) {
            if (cli.mux_opts.scte35_pids[scte_index]) {
                output_stream->ts_opts.pid = cli.mux_opts.scte35_pids[scte_index];
                printf("*************** stream - scte pid 0x%x\n", cli.mux_opts.scte35_pids[scte_index]);
            }
            scte_index++;
        }

        if( input_stream && input_stream->stream_type == STREAM_TYPE_VIDEO )
        {
            /* x264 calculates the single-frame VBV size later on */
            FAIL_IF_ERROR( system_type_value != OBE_SYSTEM_TYPE_LOWEST_LATENCY && !cli.output_streams[i].avc_param.rc.i_vbv_buffer_size,
                           "No VBV buffer size chosen\n" );

            FAIL_IF_ERROR( !cli.output_streams[i].avc_param.rc.i_vbv_max_bitrate && !cli.output_streams[i].avc_param.rc.i_bitrate,
                           "No bitrate chosen\n" );

            if( !cli.output_streams[i].avc_param.rc.i_vbv_max_bitrate && cli.output_streams[i].avc_param.rc.i_bitrate )
                cli.output_streams[i].avc_param.rc.i_vbv_max_bitrate = cli.output_streams[i].avc_param.rc.i_bitrate;

            if( cli.avc_profile >= 0 )
                x264_param_apply_profile( &cli.output_streams[i].avc_param, x264_profile_names[cli.avc_profile] );
        }
        else if( input_stream && input_stream->stream_type == STREAM_TYPE_AUDIO )
        {
            if( cli.output_streams[i].stream_action == STREAM_PASSTHROUGH && input_stream->stream_format == AUDIO_PCM &&
                cli.output_streams[i].stream_format != AUDIO_MP2 && cli.output_streams[i].stream_format != AUDIO_AC_3 &&
                cli.output_streams[i].stream_format != AUDIO_AAC )
            {
                fprintf( stderr, "Output-stream-id %i: Uncompressed audio cannot yet be placed in TS\n", cli.output_streams[i].output_stream_id );
                return -1;
            }
            else if( cli.output_streams[i].stream_action == STREAM_ENCODE && !cli.output_streams[i].bitrate )
            {
                fprintf( stderr, "Output-stream-id %i: Audio stream requires bitrate\n", cli.output_streams[i].output_stream_id );
                return -1;
            }
        }
        else if( output_stream->stream_format == MISC_TELETEXT || output_stream->stream_format == VBI_RAW )
        {
            int has_ttx = output_stream->stream_format == MISC_TELETEXT;

            /* Search VBI for teletext and complain if teletext isn't set up properly */
            if( output_stream->stream_format == VBI_RAW )
            {
                int num_vbi = 0;
                has_ttx = output_stream->dvb_vbi_opts.ttx;

                num_vbi += output_stream->dvb_vbi_opts.ttx;
                num_vbi += output_stream->dvb_vbi_opts.inverted_ttx;
                num_vbi += output_stream->dvb_vbi_opts.vps;
                num_vbi += output_stream->dvb_vbi_opts.wss;

                FAIL_IF_ERROR( !num_vbi, "No DVB-VBI data added\n" );
                FAIL_IF_ERROR( !input_stream, "DVB-VBI can only be used with a probed stream\n" );
            }

            FAIL_IF_ERROR( has_ttx && !cli.output_streams[i].ts_opts.num_teletexts,
                           "Teletext stream setup is mandatory\n" );
        }
    }

    FAIL_IF_ERROR( !cli.mux_opts.ts_muxrate, "No mux rate selected\n" );
    FAIL_IF_ERROR( cli.mux_opts.ts_muxrate < 100000, "Mux rate too low - mux rate is in bits/s, not kb/s\n" );

    FAIL_IF_ERROR( !cli.output.num_outputs, "No outputs selected\n" );
    for( int i = 0; i < cli.output.num_outputs; i++ )
    {
        if( ( cli.output.outputs[i].type == OUTPUT_UDP || cli.output.outputs[i].type == OUTPUT_RTP || cli.output.outputs[i].type == OUTPUT_FILE_TS ) &&
             !cli.output.outputs[i].target )
        {
            fprintf( stderr, "No output target chosen. Output-ID %d\n", i );
            return -1;
        }
    }

    obe_setup_streams( cli.h, cli.output_streams, cli.num_output_streams );
    obe_setup_muxer( cli.h, &cli.mux_opts );
    obe_setup_output( cli.h, &cli.output );
    if( obe_start( cli.h ) < 0 )
        return -1;

    g_running = 1;
    printf( "Encoding started\n" );

    if (g_core_runtime_statistics_to_file || g_core_runtime_statistics_to_port)
        runtime_statistics_start(&cli.h->runtime_statistics, &cli);

    if (g_core_runtime_terminate_after_seconds)
        terminate_after_start(&cli.h->terminate_after, &cli, g_core_runtime_terminate_after_seconds);

//    if (g_core_runtime_statistics_to_port)
//        runtime_statistics_json_start(&cli.h->runtime_statistics_port, &cli);

#if LTN_WS_ENABLE
    ltn_ws_alloc(&g_ltn_ws_handle, &cli, 8443);
    char *version = getSoftwareVersion();
    ltn_ws_set_property_software_version(g_ltn_ws_handle, version);
    ltn_ws_set_property_hardware_version(g_ltn_ws_handle, "571");
    free(version);
#endif

    return 0;
}

static int stop_encode( char *command, obecli_command_t *child )
{
#if LTN_WS_ENABLE
    ltn_ws_free(g_ltn_ws_handle);
#endif

    if (cli.h->runtime_statistics)
        runtime_statistics_stop(cli.h->runtime_statistics);
    if (cli.h->terminate_after)
        terminate_after_stop(cli.h->terminate_after);

    obe_close( cli.h );
    cli.h = NULL;

    if( cli.input.location )
    {
        free( cli.input.location );
        cli.input.location = NULL;
    }

    if( cli.mux_opts.service_name )
    {
        free( cli.mux_opts.service_name );
        cli.mux_opts.service_name = NULL;
    }

    if( cli.mux_opts.provider_name )
    {
        free( cli.mux_opts.provider_name );
        cli.mux_opts.provider_name = NULL;
    }

    if( cli.output_streams )
    {
        free( cli.output_streams );
        cli.output_streams = NULL;
    }

    for( int i = 0; i < cli.output.num_outputs; i++ )
    {
        if( cli.output.outputs[i].target )
            free( cli.output.outputs[i].target );
    }
    free( cli.output.outputs );

    memset( &cli, 0, sizeof(cli) );
    g_running = 0;

    return 0;
}

static int probe_device( char *command, obecli_command_t *child )
{
    if( !strlen( command ) )
        return -1;

    FAIL_IF_ERROR( strcasecmp( command, "input" ), "%s is not a valid item to probe\n", command )

    /* TODO check for validity */

    if( obe_probe_device( cli.h, &cli.input, &cli.program ) < 0 )
        return -1;

    show_input_streams( NULL, NULL );

    if( cli.program.num_streams )
    {
        if( cli.output_streams )
            free( cli.output_streams );

        cli.num_output_streams = cli.program.num_streams;
        cli.output_streams = calloc( cli.num_output_streams, sizeof(*cli.output_streams) );
        if( !cli.output_streams )
        {
            fprintf( stderr, "Malloc failed \n" );
            return -1;
        }
        for( int i = 0; i < cli.num_output_streams; i++ )
        {
            cli.output_streams[i].input_stream_id = i;
            cli.output_streams[i].output_stream_id = cli.program.streams[i].input_stream_id;
            cli.output_streams[i].stream_format = cli.program.streams[i].stream_format;
            if( cli.program.streams[i].stream_type == STREAM_TYPE_VIDEO )
            {
                cli.output_streams[i].video_anc.cea_608 = cli.output_streams[i].video_anc.cea_708 = 1;
                cli.output_streams[i].video_anc.afd = cli.output_streams[i].video_anc.wss_to_afd = 1;
            }
            else if( cli.program.streams[i].stream_type == STREAM_TYPE_AUDIO )
            {
                cli.output_streams[i].sdi_audio_pair = cli.program.streams[i].sdi_audio_pair;
                cli.output_streams[i].channel_layout = AV_CH_LAYOUT_STEREO;
            }
        }
    }

    show_output_streams( NULL, NULL );

    return 0;
}

static int parse_command( char *command, obecli_command_t *commmand_list )
{
    if( !strlen( command ) )
        return -1;

    int tok_len = strcspn( command, " " );
    int str_len = strlen( command );
    command[tok_len] = 0;

    int i = 0;

    while( commmand_list[i].name != 0 && strcasecmp( commmand_list[i].name, command ) )
        i++;

    if( commmand_list[i].name )
        commmand_list[i].cmd_func( command+tok_len+(str_len > tok_len), commmand_list[i].child_commands );
    else
        return -1;

    return 0;
}

static int processCommand(char *line_read)
{
    if (!strcasecmp(line_read, "exit") || !strcasecmp(line_read, "quit"))
        return -1;

    add_history(line_read);

    int ret = parse_command( line_read, main_commands );
    if (ret == -1)
        fprintf( stderr, "%s: command not found \n", line_read );

    /* TODO: I'm pretty sure this entire section is never executed. */
    if (!cli.h)
    {
        cli.h = obe_setup(NULL);
        if( !cli.h )
        {
            fprintf( stderr, "obe_setup failed\n" );
            return -1;
        }
        cli.avc_profile = -1;
    }

    return 0;
}

static void _usage(const char *prog, int exitcode)
{
    printf("\nOpen Broadcast Encoder command line interface.\n");
    printf("Including Kernel Labs enhancements.\n");

    char *version = getSoftwareVersion();
    printf("%s\n", version);
    syslog(LOG_INFO, "%s", version);
    free(version);

    printf("Built %s @ %s\n", __DATE__, __TIME__);
    printf("x264 build#%d (%dbit support)\n", X264_BUILD, X264_BIT_DEPTH);
    printf("Supports HEVC via  X265: %s\n",
#if HAVE_X265_H
        "true"
#else
        "false"
#endif
    );

    printf("Supports HEVC via VAAPI: %s\n",
#if HAVE_VA_VA_H
        "true"
#else
        "false"
#endif
    );
    printf("Supports  YUV via  FILE: true\n");
    printf("Supports  AVC via VAAPI: %s\n",
#if HAVE_VA_VA_H
        "true"
#else
        "false"
#endif
    );
    printf("Supports YUV VIA DekTec: %s\n",
#if HAVE_DTAPI_H
        "true"
#else
        "false"
#endif
    );
    printf("Supports      Vega 3301: %s\n",
#if HAVE_VEGA3301_CAP_TYPES_H
        "true"
#else
        "false"
#endif
    );
    printf("Supports      Vega 3311: %s\n",
#if HAVE_VEGA3311_CAP_TYPES_H
        "true"
#else
        "false"
#endif
    );

    printf("Supports RAW VIA NDISDK: %s\n",
#if HAVE_PROCESSING_NDI_LIB_H
        "true"
#else
        "false"
#endif
    );
    printf("Supports   AVFoundation: %s\n",
#if defined(__APPLE__)
	"true (ish)"
#else
	"false"
#endif
        );
#if HAVE_BLUEDRIVER_P_H
    printf("BlueFish444 Epoch SDK\n");
#endif
    printf("Decklink SDK %s\n", BLACKMAGIC_DECKLINK_API_VERSION_STRING);
#if HAVE_DTAPI_H
    printf("DekTec SDK %s\n", dektec_sdk_version);
#endif
#if HAVE_VEGA3301_CAP_TYPES_H
    printf("Vega 3301 SDK %s\n", vega3301_sdk_version);
#endif
#if HAVE_VEGA3311_CAP_TYPES_H
    printf("Vega 3311 SDK %s\n", vega3311_sdk_version);
#endif

    printf("\n");

    if (exitcode) {
        printf("%s -c <script.txt>\n", prog);
        printf("\t-h              - Display command line helps.\n");
        printf("\t-c <script.txt> - Start OBE and begin executing a list of commands.\n");
        printf("\t-L <string>     - When writing to syslog, use the 'obe-<string>' name/tag. [def: unset]\n");
        printf("\t-C <file.cf>    - Read and consoledump a codec.cf file.\n");
        printf("\n");
        exit(exitcode);
    }
}

int main( int argc, char **argv )
{
    char *home_dir = getenv( "HOME" );
    char *history_filename;
    char *prompt = "obecli> ";
    char *script = NULL;
    int   scriptInitialized = 0;
    char *line_read = NULL;
    int opt;
    const char *syslogSuffix = NULL;

    obe_setProcessStartTime();

#if MULTIPLE_SCTE35_PIDS
    scte104filter_init(&g_scte104_filtering_context);
#endif

#if 0
// APPLE
    avf_capture_init();
    avf_capture_start();
    sleep(5);
    avf_capture_stop();
    printf("Stopped\n");
#endif

    while ((opt = getopt(argc, argv, "c:C:hL:")) != -1) {
        switch (opt) {
        case 'C':
        {
            FILE *fh = fopen(optarg, "rb");
            if (!fh) {
                    fprintf(stderr, "Unable to open cf input file '%s'.\n", optarg);
                    return 0;
            }

            unsigned int count = 0;
            while (!feof(fh)) {
                    obe_coded_frame_t *f;
                    size_t rlen = coded_frame_serializer_read(fh, &f);
                    if (rlen <= 0)
                            break;

                    printf("[%8d]  ", count++);
                    coded_frame_print(f);

                    destroy_coded_frame(f);
            }

            fclose(fh);
            exit(0);
         }
            break;
        case 'c':
            script = optarg;
            {
                /* Check it exists */
                FILE *fh = fopen(script, "r");
                if (!fh)
                    _usage(argv[0], 1);
                fclose(fh);
            }
            break;
        case 'L':
        {
            int failed = 0;
            int l = strlen(optarg);
            if (l <= 0 || l >= 32)
                failed = 1;
            for (int i = 0; i < l; i++) {
                if (isspace(optarg[i]))
                    failed = 1;
            }
            if (failed) {
                fprintf(stderr, "-L string length must 1-31 characters long, with no whitespace.\n" );
                return -1;
            }
            syslogSuffix = optarg;
            break;
        }
        case 'h':
        default:
            _usage(argv[0], 1);
        }
    }

    history_filename = malloc( strlen( home_dir ) + 16 + 1 );
    if( !history_filename )
    {
        fprintf( stderr, "malloc failed\n" );
        return -1;
    }

    sprintf( history_filename, "%s/.obecli_history", home_dir );
    read_history(history_filename);

    cli.h = obe_setup(syslogSuffix);
    if( !cli.h )
    {
        fprintf( stderr, "obe_setup failed\n" );
        return -1;
    }

    cli.avc_profile = -1;

    _usage(argv[0], 0);

    while (1) {
        if (line_read) {
            free(line_read);
            line_read = NULL;
        }


        if (script && !scriptInitialized) {
            line_read = malloc(256);
            if (!line_read) {
                fprintf(stderr, "Unable to allocate ram for script command, aborting.\n");
                break;
            }
            sprintf(line_read, "@%s", script);
            scriptInitialized = 1;
        } else
            line_read = readline( prompt );

	if (line_read && line_read[0] == '#') {
            /* comment  - do nothing */
        } else
	if (line_read && strlen(line_read) > 0 && line_read[0] == '!') {
            printf("Spawning a shell, use 'exit' to close shell and return to OBE.\n");
            system("bash");
        } else
	if (line_read && strlen(line_read) > 0 && line_read[0] != '@') {
		if (processCommand(line_read) < 0)
                    break;
	} else
	if (line_read && line_read[0] == '@' && strlen(line_read) > 1) {
            line_read = realloc(line_read, 256);
            FILE *fh = fopen(&line_read[1], "r");
            while (fh && !feof(fh)) {
                if (fgets(line_read, 256, fh) == NULL)
                    break;
                if (feof(fh))
                    break;
                if (line_read[0] == '#')
                    continue; /* Comment - do nothing */

                if (strlen(line_read) <= 1)
                    continue;

                line_read[strlen(line_read) - 1] = 0;

		if (processCommand(line_read) < 0)
                    break;
            }
            if (fh) {
                fclose(fh);
                fh = NULL;
            }
        }
    }

    write_history( history_filename );

    if (history_filename)
        free(history_filename);
    if (line_read)
        free(line_read);

    stop_encode( NULL, NULL );

    return 0;
}

/* RUNTIME STATISTICS */

#include <stdio.h> // printf
#include <stdlib.h> // labs
#include <sys/time.h>

void getISO8601(struct timespec *tspec, char *timestring)
{
    struct timespec tp;

    if (tspec) {
        tp = *tspec;
    } else {
        clock_gettime(CLOCK_REALTIME, &tp);
    }

    char buf[sizeof "9999-12-31T23:59:59.999+00:00"]; // 29 chars + '\0'

    size_t bufsize = sizeof buf;
    int off = 0;
    struct tm *local = localtime(&tp.tv_sec);
    off = strftime(buf, bufsize, "%FT%T", local); // same as "%Y-%m-%dT%H:%M:%S" 
    off += snprintf(buf+off, bufsize-off, ".%03ld", tp.tv_nsec/1000000);
    off += snprintf(buf+off, bufsize-off, "%c%02ld:%02ld", local->tm_gmtoff >= 0 ? '+' : '-', labs(local->tm_gmtoff)/3600, labs(local->tm_gmtoff)%3600/60); 

    sprintf(timestring, buf);
}


#define LOCAL_DEBUG 0


int g_core_runtime_statistics_to_file = 0;
int g_core_runtime_statistics_to_port = 0;
extern int ltnpthread_setname_np(pthread_t thread, const char *name);

struct runtime_statistics_ctx
{
	obecli_ctx_t *cli;

	/* Thermal bitmasks */
	uint64_t thermal_bm;
	int therm_max;

	pthread_t threadId;
	int running, terminate, terminated;
};

#define APPEND(s) ((s) + strlen(s))
static void *runtime_statistics_thread(void *p)
{
#if LOCAL_DEBUG
	printf("%s()\n", __func__);
#endif
	struct runtime_statistics_ctx *ctx = (struct runtime_statistics_ctx *)p;

	ltnpthread_setname_np(ctx->threadId, "obe-rt-stats");

#if 0
    int tx_skt = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tx_skt < 0) {

    }

    struct sockaddr_in tx_sa;
    tx_sa.sin_family = AF_INET;
    tx_sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    tx_sa.sin_port = htons(g_core_runtime_statistics_to_port);
#endif

    int bitrate_history_secs = 60;
    int64_t bitrate_history[60]; /* where idx 0 is now and idx 1 is 1 second ago */
    
    if (strcmp(g_device_input_type, "decklink") == 0) {
        switch (cli.input.card_idx) {
        case 0: strcpy(g_device_input_port, "A"); break;
        case 1: strcpy(g_device_input_port, "C"); break;
        case 2: strcpy(g_device_input_port, "B"); break;
        case 3: strcpy(g_device_input_port, "C"); break;
        default: strcpy(g_device_input_port, "?"); break;
        }
    }

	ctx->running = 1;
	char ts[64];
	char line[256] = { 0 };
	while (!ctx->terminate) {
        double t1, t2, total_cpu_used_pct;
        obe_process_calculate_cpu_usage(getpid(), &t1);
		sleep(1);
        obe_process_calculate_cpu_usage(getpid(), &t2);
        total_cpu_used_pct = t2 - t1;
		obe_getTimestamp(ts, NULL);
		line[0] = 0;

		/* 1. Timestamp,
		 * 2. bps output
		 * 3. pid
		 * 4. encoder 0 (video codec) raw frame queue depth
		 * 5..... cpu thermals in degC.
		 */
		sprintf(APPEND(line), ",pid=%d", getpid());
		sprintf(APPEND(line), ",bps=%d", g_udp_output_bps);

		// /sys/devices/platform/coretemp.0/hwmon/hwmon1

		obe_t *h = ctx->cli->h;
		for (int i = 0; i < ctx->cli->h->num_output_streams; i++) {
			obe_output_stream_t *e = obe_core_get_output_stream_by_index(h, i);
			if (e->stream_action == STREAM_ENCODE && i == 0) {
				obe_queue_t *q = &h->encoders[i]->queue;
				sprintf(APPEND(line), ",ve_q=%d", q->size);
				break;
			}
		}

		/* Mux */
		sprintf(APPEND(line), ",mux_dtstotal=%" PRIi64, g_mux_dtstotal);

		/* Thermals */
		if (ctx->thermal_bm == 0) {
			char tmp[256];
			for (int i = 0; i < 63; i++) {
				char fn[256];
				sprintf(fn, "/sys/devices/platform/coretemp.0/hwmon/hwmon1/temp%d_label", i);
				FILE *fh = fopen(fn, "rb");
				if (!fh) {
					sprintf(fn, "/sys/devices/platform/coretemp.0/hwmon/hwmon2/temp%d_label", i);
					fh = fopen(fn, "rb");
					if (!fh)
						continue;
				}

				tmp[0] = 0;
				fread(tmp, 1, sizeof(tmp), fh);
				if (strncmp(tmp, "Core ", 5) == 0) {
					ctx->thermal_bm |= (1 << i);
					ctx->therm_max = i + 1;
				}
				fclose(fh);
			}
		}

        int core_temp = -1;
		if (ctx->thermal_bm) {
			int t = 0;
			for (int i = 0; i < ctx->therm_max; i++) {
				if ((ctx->thermal_bm & ((uint64_t)(1 << i))) == 0)
					continue;

				char fn[256];
				char val[16];
				sprintf(fn, "/sys/devices/platform/coretemp.0/hwmon/hwmon1/temp%d_input", i);
				FILE *fh = fopen(fn, "rb");
				if (!fh) {
					sprintf(fn, "/sys/devices/platform/coretemp.0/hwmon/hwmon2/temp%d_input", i);
					fh = fopen(fn, "rb");
					if (!fh)
						continue;
				}

				fread(val, 1, sizeof(val), fh);
                if (core_temp == -1)
                    core_temp = atoi(val) / 1000;

				fclose(fh);
				sprintf(APPEND(line),",cpu%d_temp=%d", t++, atoi(val) / 1000);
			}
		}

		/* Load averages */
		double la[3] = { 0.0, 0.0, 0.0 };
		if (getloadavg(&la[0], 3) == 3) {
			sprintf(APPEND(line),",load1=%.02f,load5=%.02f,load15=%.02f", la[0], la[1], la[2]);
		}

		char msg[256];
		sprintf(msg, "ts=%s%s\n", ts, line);

		if (g_core_runtime_statistics_to_file > 1)
			printf("%s", msg);

        if (g_core_runtime_statistics_to_file) {
            char statsfile[256];
            sprintf(statsfile, "/tmp/%d-obe-runtime-statistics.log", getpid());
            FILE *fh = fopen(statsfile, "a+");
            if (fh) {
                fwrite(msg, 1, strlen(msg), fh);
                fclose(fh);
            }
        }

        if (!g_core_runtime_statistics_to_port)
            continue;

        json_object *j = json_object_new_object();

        switch(g_device_input_port[0]) {
        default:
        case 'A':
            json_object_object_add(j, "uuid", json_object_new_string("ltnt-nyc-stevenlab-prod1-e1"));
            break;
        case 'B':
            json_object_object_add(j, "uuid", json_object_new_string("ltnt-nyc-stevenlab-prod1-e3"));
            break;
        case 'C':
            json_object_object_add(j, "uuid", json_object_new_string("ltnt-nyc-stevenlab-prod1-e2"));
            break;
        case 'D':
            json_object_object_add(j, "uuid", json_object_new_string("ltnt-nyc-stevenlab-prod1-e4"));
            break;
        }

        char timestamp[64];
        getISO8601(NULL, &timestamp[0]);
        json_object_object_add(j, "now", json_object_new_string(timestamp));

        struct timespec x;
        x.tv_sec = obe_getProcessStartTime();
        x.tv_nsec = 0;
        getISO8601(&x, &timestamp[0]);
        json_object_object_add(j, "startup", json_object_new_string(timestamp));


        char runtime[64];
        obe_getProcessRuntimeAscii(&runtime[0], sizeof(runtime));
        json_object_object_add(j, "runtime", json_object_new_string(runtime));

        json_object *plat = json_object_new_object();

        char hname[128];
        gethostname(&hname[0], sizeof(hname));
        json_object_object_add(plat, "host", json_object_new_string(hname));

        char encoder_vers[64];
            FILE *fh = fopen("/home/ltn/ltn_encoder/VERSION", "rb");
            if (fh) {
                if (fread(&encoder_vers[0], 1, sizeof(encoder_vers), fh) > 0) {
                    encoder_vers[ strlen(encoder_vers) - 1] = 0;
                }
                fclose(fh);
            } else {
                encoder_vers[0] = 'N';
                encoder_vers[1] = 'A';
                encoder_vers[2] = 0;
            }
        json_object_object_add(plat, "pkg_version", json_object_new_string(encoder_vers));

        char s[64];
        sprintf(s, "LTN%d", obe_core_get_platform_model());
        json_object_object_add(plat, "model", json_object_new_string(s));

        json_object_object_add(plat, "pid", json_object_new_int64(getpid()));

        sprintf(s, "%5.02f", la[0]);
        json_object_object_add(plat, "la1", json_object_new_string(s));
        sprintf(s, "%5.02f", la[1]);
        json_object_object_add(plat, "la5", json_object_new_string(s));
        sprintf(s, "%5.02f", la[2]);
        json_object_object_add(plat, "la15", json_object_new_string(s));

        json_object_object_add(plat, "temperature", json_object_new_int64(core_temp));
        json_object_object_add(plat, "cpu_pct", json_object_new_int64(total_cpu_used_pct));

        struct rusage r_usage;
        getrusage(RUSAGE_SELF, &r_usage);
        json_object_object_add(plat, "memused", json_object_new_int64(r_usage.ru_maxrss));

        struct statvfs st[2];
        statvfs("/", &st[0]);
        statvfs("/storage", &st[1]);
        unsigned long root_free_space = st[0].f_bfree * st[0].f_frsize;
        unsigned long storage_free_space = st[1].f_bfree * st[1].f_frsize;
        json_object_object_add(plat, "hdfree_root", json_object_new_int64(root_free_space));
        json_object_object_add(plat, "hdfree_storage", json_object_new_int64(storage_free_space));

        char cfg_changed[64];
        struct stat attr;
        if (stat("/home/ltn/ltn_encoder/ltn_encoder0.cfg", &attr) == 0) {
            strftime(cfg_changed, 20, "%Y-%m-%d %H:%M:%S", localtime(&(attr.st_ctime)));
        } else {
            cfg_changed[0] = 'N';
            cfg_changed[1] = 'A';
            cfg_changed[2] = 0;
        }

        json_object_object_add(plat, "last_conf_file_change", json_object_new_string(cfg_changed));

        json_object_object_add(j, "platform", plat); /* End of platform */

        json_object *sig = json_object_new_object();
        sprintf(s, "/analyzer/%s", basename(g_filter_video_fullsize_jpg_filename));
        json_object_object_add(sig, "thumbnail", json_object_new_string(s));
        json_object_object_add(sig, "type", json_object_new_string(g_device_input_type));
        json_object_object_add(sig, "inputport", json_object_new_string(g_device_input_port));
        json_object_object_add(sig, "present", json_object_new_int64(1));


        if (strlen(g_signal_format_detected) > 0) {
            json_object_object_add(sig, "format", json_object_new_string(g_signal_format_detected));
        } else {
            json_object_object_add(sig, "format", json_object_new_string("NA"));
        }

        getISO8601(&g_signal_detected_ts, &timestamp[0]);
        json_object_object_add(sig, "acq_time", json_object_new_string(timestamp));
        getISO8601(&g_signal_los_ts, &timestamp[0]);
        json_object_object_add(sig, "los_time", json_object_new_string(timestamp));

        if (g_scte104_detected_ts.tv_sec) {
            getISO8601(&g_scte104_detected_ts, &timestamp[0]);
            json_object_object_add(sig, "scte104_time", json_object_new_string(timestamp));
        } else {
            json_object_object_add(sig, "scte104_time", json_object_new_string("NA"));
        }

        if (g_smpte2038_detected_ts.tv_sec) {
            getISO8601(&g_smpte2038_detected_ts, &timestamp[0]);
            json_object_object_add(sig, "smpte2038_time", json_object_new_string(timestamp));
        } else {
            json_object_object_add(sig, "smpte2038_time", json_object_new_string("NA"));
        }

        if (g_cea708_detected_ts.tv_sec) {
            getISO8601(&g_cea708_detected_ts, &timestamp[0]);
            json_object_object_add(sig, "cea708_time", json_object_new_string(timestamp));
        } else {
            json_object_object_add(sig, "cea708_time", json_object_new_string("NA"));
        }
        json_object_object_add(j, "signal", sig); /* End of signal */

        /* SDI audio levels */
        for (int i = 0; i < 16; i++) {
            char l[32], m[32];
            sprintf(l, "a%d_level", i + 1);
            sprintf(m, "%6.1f", g_audio_channel_level_dbfs[i]);
            json_object_object_add(j, l, json_object_new_double(g_audio_channel_level_dbfs[i]));

            sprintf(l, "a%d_type", i + 1);
            sprintf(m, "%s",
                g_audio_channel_type[i] == 1 ? "pcm" :
                g_audio_channel_type[i] == 2 ? "bitstream" : "NA");

            json_object_object_add(j, l, json_object_new_string(m));
        }

        json_object *outp = json_object_new_object();
        for (int i = bitrate_history_secs - 1; i > 0; i--) {
            bitrate_history[i] = bitrate_history[i - 1];
        }
        bitrate_history[0] = g_udp_output_bps;

        json_object *br_history = json_object_new_array();
        for (int i = 0; i < bitrate_history_secs; i++) {
            json_object_array_add(br_history, json_object_new_int64(bitrate_history[i]));
        }
        json_object_object_add(outp, "br_history", br_history);
        
        json_object_object_add(outp, "br", json_object_new_int64(g_udp_output_bps));
        json_object_object_add(outp, "scte35_count", json_object_new_int64(g_scte35_trigger_count));
        json_object_object_add(outp, "url", json_object_new_string(cli.output.outputs[0].target));

        json_object *pids = json_object_new_array();

        json_object *p1 = json_object_new_object();

        sprintf(s, "0x%04x (%4d)", 32, 32);
        json_object_object_add(p1, "nr", json_object_new_string(s));
        json_object_object_add(p1, "cc", json_object_new_int64(1));
        json_object_object_add(p1, "type", json_object_new_string("video"));
        json_object_array_add(pids, p1);

        json_object *p2 = json_object_new_object();
        sprintf(s, "0x%04x (%4d)", 33, 33);
        json_object_object_add(p2, "nr", json_object_new_string(s));
        json_object_object_add(p2, "cc", json_object_new_int64(2));
        json_object_object_add(p2, "type", json_object_new_string("audio"));
        json_object_array_add(pids, p2);

        json_object_object_add(outp, "pids", pids);
        json_object_object_add(j, "output", outp); /* End of output section */

        ///   END

        const char *resp_str = strdup(json_object_to_json_string_ext(j, JSON_C_TO_STRING_PRETTY));
        //const char *resp_str = strdup(json_object_to_json_string_ext(j, JSON_C_TO_STRING_PLAIN));
        if (g_json_stats_print_console) {
            printf("jsonc = [%s]\n", resp_str);
        }
#if 0
        if (sendto(tx_skt, resp_str, strlen(resp_str), 0, (struct sockaddr *)&tx_sa, sizeof(tx_sa)) < 0) {
            fprintf(stderr, "Error transmitting runtime stats to UDP\n");
        }
#endif
        if (obe_http_post(resp_str) < 0) {
            //fprintf(stderr, MODULE_PREFIX "Failed to transmit stats\n");
        }

        json_object_put(j); /* Free all the json objects */
        free((char *)resp_str);

	} /* while !ctx->terminate */

	ctx->terminated = 1;
	ctx->running = 0;
	pthread_exit(0);

#if 0
    close(tx_skt);
#endif

	return NULL;
}

int runtime_statistics_start(void **p, obecli_ctx_t *cli)
{
	struct runtime_statistics_ctx *ctx = calloc(1, sizeof(*ctx));
	*p = ctx;
	ctx->cli = cli;

	printf("%s()\n", __func__);

	pthread_create(&ctx->threadId, NULL, runtime_statistics_thread, ctx);
	return 0;
}

void runtime_statistics_stop(void *p)
{
	printf("%s() ctx %p\n", __func__, p);

	struct runtime_statistics_ctx *ctx = (struct runtime_statistics_ctx *)p;
	ctx->terminate = 1;
	while (!ctx->terminated)
		usleep(100 * 1000);

printf("terminated pre-cancel\n");
	pthread_cancel(ctx->threadId);
printf("terminated clean\n");
}

/* "Process teminate after N seconds capability" */
int g_core_runtime_terminate_after_seconds = 0;
struct terminate_after_ctx
{
	obecli_ctx_t *cli;

	pthread_t threadId;
	int running, terminate, terminated;

	time_t terminateWhen;
};

static void *terminate_after_thread(void *p)
{
#if LOCAL_DEBUG
	printf(MODULE_PREFIX "%s()\n", __func__);
#endif
	struct terminate_after_ctx *ctx = (struct terminate_after_ctx *)p;

	ltnpthread_setname_np(ctx->threadId, "obe-ta-after");

	char ts[64];
	char line[256] = { 0 };

	sprintf(line, MODULE_PREFIX "WARNING: process configured to self terminate in %lu seconds.\n", ctx->terminateWhen - time(NULL));
	fprintf(stderr, "%s", line);
	syslog(LOG_INFO | LOG_LOCAL4, "%s", line);

	ctx->running = 1;
	while (!ctx->terminate) {
		usleep(100 * 1000);
		time_t now = time(NULL);
		if (now >= ctx->terminateWhen) {
			obe_getTimestamp(ts, NULL);
			sprintf(line, MODULE_PREFIX "FATAL: Self terminating on command.\n");
			fprintf(stderr, "%s", line);
			syslog(LOG_INFO | LOG_LOCAL4, "%s", line);
			exit(0);
		}
	}
	printf(MODULE_PREFIX "Self terminate feature was prematurely cancelled\n");
	ctx->terminated = 1;
	ctx->running = 0;
	pthread_exit(0);
//	pthread_cancel(ctx->threadId);
	return NULL;
}

int terminate_after_start(void **p, obecli_ctx_t *cli, int afterNSeconds)
{
#if LOCAL_DEBUG
	printf(MODULE_PREFIX "%s(%d)\n", __func__, afterNSeconds);
#endif
	struct terminate_after_ctx *ctx = calloc(1, sizeof(*ctx));
	*p = ctx;
	ctx->cli = cli;

	if (afterNSeconds < 5)
		afterNSeconds = 5;
	ctx->terminateWhen = time(NULL) + afterNSeconds;

	printf(MODULE_PREFIX "%s() aborting after %d seconds\n", __func__, afterNSeconds);

	pthread_create(&ctx->threadId, NULL, terminate_after_thread, ctx);
	return 0;
}

void terminate_after_stop(void *p)
{
	struct terminate_after_ctx *ctx = (struct terminate_after_ctx *)p;
	ctx->terminate = 1;
	while (!ctx->terminated)
		usleep(100 * 1000);

	pthread_cancel(ctx->threadId);
}

