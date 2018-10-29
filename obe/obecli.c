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
#include <include/DeckLinkAPIVersion.h>

#include <signal.h>
#define _GNU_SOURCE

#include <readline/readline.h>
#include <readline/history.h>
#include <libavresample/avresample.h>
#include <libmpegts.h>

#include "obe.h"
#include "obecli.h"
#include "common/common.h"

#define FAIL_IF_ERROR( cond, ... ) FAIL_IF_ERR( cond, "obecli", __VA_ARGS__ )
#define RETURN_IF_ERROR( cond, ... ) RETURN_IF_ERR( cond, "options", NULL, __VA_ARGS__ )

typedef struct
{
    obe_t *h;
    obe_input_t input;
    obe_input_program_t program;

    /* Configuration params from the command line configure these output streams.
     * before they're finally cloned into the obe_t struct as 'output_streams'.
     * See obe_setup_streams() for the cloning action.
     */
    int num_output_streams;
    obe_output_stream_t *output_streams;
    obe_mux_opts_t mux_opts;
    obe_output_opts_t output;
    int avc_profile;
} obecli_ctx_t;

obecli_ctx_t cli;

/* Ctrl-C handler */
static volatile int b_ctrl_c = 0;

static int g_running = 0;
static int system_type_value = OBE_SYSTEM_TYPE_GENERIC;

static const char * const system_types[]             = { "generic", "lowestlatency", "lowlatency", 0 };
static const char * const input_types[]              = { "url", "decklink", "linsys-sdi", "v4l2", 0 };
static const char * const input_video_formats[]      = { "pal", "ntsc", "720p50", "720p59.94", "720p60", "1080i50", "1080i59.94", "1080i60",
                                                         "1080p23.98", "1080p24", "1080p25", "1080p29.97", "1080p30", "1080p50", "1080p59.94",
                                                         "1080p60", 0 };
static const char * const input_video_connections[]  = { "sdi", "hdmi", "optical-sdi", "component", "composite", "s-video", 0 };
static const char * const input_audio_connections[]  = { "embedded", "aes-ebu", "analogue", 0 };
static const char * const ttx_locations[]            = { "dvb-ttx", "dvb-vbi", "both", 0 };
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
static const char * entropy_modes[] = { "cabac", "cavlc", NULL };

static const char * system_opts[] = { "system-type", "max-probe-time", NULL };
static const char * input_opts[]  = { "location", "card-idx", "video-format", "video-connection", "audio-connection",
                                      "smpte2038", "scte35", "vanc-cache", "bitstream-audio", "patch1", "los-exit-ms",
                                      "frame-injection", NULL };
static const char * add_opts[] =    { "type" };
/* TODO: split the stream options into general options, video options, ts options */
static const char * stream_opts[] = { "action", "format",
                                      /* Encoding options */
                                      "vbv-maxrate", "vbv-bufsize", "bitrate",
                                      "profile", "level", "keyint", "lookahead", "threads", "bframes", "b-pyramid", "weightp",
                                      "interlaced", "tff", "frame-packing", "csp", "filler", "intra-refresh", "aspect-ratio",
                                      "width", "max-refs",

                                      /* Audio options */
                                      "sdi-audio-pair", "channel-map", "mono-channel",
                                      /* AAC options */
                                      "aac-profile", "aac-encap",
                                      /* MP2 options */
                                      "mp2-mode",
                                      /* TS options */
                                      "pid", "lang", "audio-type", "num-ttx", "ttx-lang", "ttx-type", "ttx-mag", "ttx-page",
                                      /* VBI options */
                                      "vbi-ttx", "vbi-inv-ttx", "vbi-vps", "vbi-wss",

                                      "opencl", /* 40 */
                                      "preset-name", /* 41 */
                                      "entropy", /* 42 */
                                      "audio-offset", /* 43 */
                                      "video-codec", /* 44 */
                                      NULL };

static const char * muxer_opts[]  = { "ts-type", "cbr", "ts-muxrate", "passthrough", "ts-id", "program-num", "pmt-pid", "pcr-pid",
                                      "pcr-period", "pat-period", "service-name", "provider-name", "scte35-pid", "smpte2038-pid", NULL };
static const char * ts_types[]    = { "generic", "dvb", "cablelabs", "atsc", "isdb", NULL };
static const char * output_opts[] = { "type", "target", NULL };

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

const char *obe_core_get_format_name_short(int stream_format)
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

        FAIL_IF_ERROR( video_format && ( check_enum_value( video_format, input_video_formats ) < 0 ),
                       "Invalid video format\n" );

        FAIL_IF_ERROR( video_connection && ( check_enum_value( video_connection, input_video_connections ) < 0 ),
                       "Invalid video connection\n" );

        FAIL_IF_ERROR( audio_connection && ( check_enum_value( audio_connection, input_audio_connections ) < 0 ),
                       "Invalid audio connection\n" );

        if( location )
        {
             if( cli.input.location )
                 free( cli.input.location );

             cli.input.location = malloc( strlen( location ) + 1 );
             FAIL_IF_ERROR( !cli.input.location, "malloc failed\n" );
             strcpy( cli.input.location, location );
        }

        cli.input.enable_frame_injection = obe_otoi(frame_injection, cli.input.enable_frame_injection);
        cli.input.enable_patch1 = obe_otoi( patch1, cli.input.enable_patch1 );
        cli.input.enable_bitstream_audio = obe_otoi( bitstream_audio, cli.input.enable_bitstream_audio );
        cli.input.enable_smpte2038 = obe_otoi( smpte2038, cli.input.enable_smpte2038 );
        cli.input.enable_scte35 = obe_otoi( scte35, cli.input.enable_scte35 );
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
        FAIL_IF_ERROR( output_stream_id < 0 || output_stream_id > cli.num_output_streams-1,
                       "Invalid stream id\n" );

        input_stream = &cli.program.streams[cli.output_streams[output_stream_id].input_stream_id];
        output_stream = &cli.output_streams[output_stream_id];

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

            /* NB: remap these and the ttx values below if more encoding options are added - TODO: split them up */
            char *pid         = obe_get_option( stream_opts[28], opts );
            char *lang        = obe_get_option( stream_opts[29], opts );
            char *audio_type  = obe_get_option( stream_opts[30], opts );

            char *opencl  = obe_get_option( stream_opts[40], opts );
            const char *preset_name  = obe_get_option( stream_opts[41], opts );
            const char *entropy_mode = obe_get_option( stream_opts[42], opts );
            const char *audio_offset = obe_get_option( stream_opts[43], opts );
            const char *video_codec = obe_get_option( stream_opts[44], opts );

            int video_codec_id = 0; /* AVC */
            if (video_codec) {
                if (strcasecmp(video_codec, "AVC") == 0)
                    video_codec_id = 0; /* AVC */
#if HAVE_X265_H
                else
                if (strcasecmp(video_codec, "HEVC") == 0)
                    video_codec_id = 1; /* HEVC */
#endif
#if HAVE_VA_VA_H
                else
                if (strcasecmp(video_codec, "AVC_VAAPI") == 0)
                    video_codec_id = 2; /* AVC via VAAPI */
                else
                if (strcasecmp(video_codec, "HEVC_VAAPI") == 0)
                    video_codec_id = 3; /* HEVC via VAAPI */
#endif
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
                FAIL_IF_ERROR(entropy_mode && (check_enum_value(entropy_mode, entropy_modes) < 0),
                              "Invalid entropy coding mode\n" );

                if (preset_name) {
                    obe_populate_avc_encoder_params(cli.h,  input_stream->input_stream_id
			/* cli.program.streams[i].input_stream_id */, avc_param, preset_name);
                } else {
                    obe_populate_avc_encoder_params(cli.h, input_stream->input_stream_id
			/* cli.program.streams[i].input_stream_id */, avc_param, "veryfast");
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
                }

                avc_param->rc.i_vbv_max_bitrate = obe_otoi( vbv_maxrate, 0 );
                avc_param->rc.i_vbv_buffer_size = obe_otoi( vbv_bufsize, 0 );
                avc_param->rc.i_bitrate         = obe_otoi( bitrate, 0 );
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

                if( action )
                    parse_enum_value( action, stream_actions, &cli.output_streams[output_stream_id].stream_action );
                if( format )
                    parse_enum_value( format, encode_formats, &cli.output_streams[output_stream_id].stream_format );
                if( audio_type )
                    parse_enum_value( audio_type, audio_types, &cli.output_streams[output_stream_id].ts_opts.audio_type );
                if( channel_map )
                    parse_enum_value( channel_map, channel_maps, &channel_map_idx );
                if( mono_channel )
                    parse_enum_value( mono_channel, mono_channels, &cli.output_streams[output_stream_id].mono_channel );

                channel_layout = channel_layouts[channel_map_idx];

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
        cli.mux_opts.scte35_pid = obe_otoi( scte35_pid, cli.mux_opts.scte35_pid ) & 0x1fff;
        cli.mux_opts.smpte2038_pid = obe_otoi( smpte2038_pid, cli.mux_opts.smpte2038_pid ) & 0x1fff;

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

/* Case 1 */
extern int g_decklink_fake_lost_payload;
extern time_t g_decklink_fake_lost_payload_time;

/* Case 2 */
extern int g_decklink_fake_every_other_frame_lose_audio_payload;
extern time_t g_decklink_fake_every_other_frame_lose_audio_payload_time;

/* Case 4 audio/video clocks */
extern int64_t cur_pts; /* audio clock */
extern int64_t cpb_removal_time; /* Last video frame clock */

extern int64_t ac3_offset_ms;

/* Mux */
extern int64_t initial_audio_latency;

/* Mux Smoother */
extern int64_t g_mux_smoother_last_item_count;
extern int64_t g_mux_smoother_last_total_item_size;
extern int64_t g_mux_smoother_fifo_pcr_size;
extern int64_t g_mux_smoother_fifo_data_size;

/* UDP Packet output */
extern int g_udp_output_drop_next_video_packet;
extern int g_udp_output_drop_next_audio_packet;
extern int g_udp_output_drop_next_packet;
extern int g_udp_output_stall_packet_ms;

/* LOS frame injection. */
extern int g_decklink_inject_frame_enable;
extern int g_decklink_injected_frame_count_max;
extern int g_decklink_injected_frame_count;

void display_variables()
{
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

    printf("audio_encoder.ac3_offset_ms = %" PRIi64 "\n", ac3_offset_ms);
    printf("audio_encoder.last_pts = %" PRIi64 "\n", cur_pts);
    printf("video_encoder.last_pts = %" PRIi64 "\n", cpb_removal_time);
    printf("v - a                  = %" PRIi64 "  %" PRIi64 "(ms)\n", cpb_removal_time - cur_pts,
        (cpb_removal_time - cur_pts) / 27000);
    printf("a - v                  = %" PRIi64 "  %" PRIi64 "(ms)\n", cur_pts - cpb_removal_time,
        (cur_pts - cpb_removal_time) / 27000);

    printf("ts_mux.initial_audio_latency  = %" PRIi64 "\n", initial_audio_latency);
    printf("mux_smoother.last_item_count  = %" PRIi64 "\n",
        g_mux_smoother_last_item_count);
    printf("mux_smoother.last_total_item_size  = %" PRIi64 " (bytes)\n",
        g_mux_smoother_last_total_item_size);
    printf("mux_smoother.fifo_pcr_size         = %" PRIi64 " (bytes)\n",
        g_mux_smoother_fifo_pcr_size);
    printf("mux_smoother.fifo_data_size        = %" PRIi64 " (bytes)\n",
        g_mux_smoother_fifo_data_size);
    printf("udp_output.drop_next_video_packet  = %d\n",
        g_udp_output_drop_next_video_packet);
    printf("udp_output.drop_next_audio_packet  = %d\n",
        g_udp_output_drop_next_audio_packet);
    printf("udp_output.drop_next_packet  = %d\n",
        g_udp_output_drop_next_packet);
    printf("udp_output.stall_packet_ms  = %d\n",
        g_udp_output_stall_packet_ms);

}

static int set_variable(char *command, obecli_command_t *child)
{
    int64_t val = 0;
    char var[128];

    if (!strlen(command)) {
        /* Missing arg, display the current value. */
        display_variables();
        return 0;
    }

    if (sscanf(command, "%s = %" PRIi64, &var[0], &val) != 2) {
        printf("illegal variable name.\n");
        return -1;
    }

    if (strcasecmp(var, "sdi_input.fake_60sec_lost_payload") == 0) {
        printf("setting %s to %" PRIi64 "\n", var, val);
        g_decklink_fake_lost_payload = val;
        if (val == 0)
            g_decklink_fake_lost_payload_time = 0;
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
    if (strcasecmp(var, "audio_encoder.ac3_offset_ms") == 0) {
        ac3_offset_ms = val;
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
    if (strcasecmp(var, "udp_output.stall_packet_ms") == 0) {
        g_udp_output_stall_packet_ms = val;
    } else {
        printf("illegal variable name.\n");
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
    printf( "AVC output bit depth: %i bits per sample\n", x264_bit_depth );

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
        if (e->stream_action == STREAM_ENCODE ) {
            q = &cli.h->encoders[i]->queue;
            printf("name: %s depth: %d item(s)\n", q->name, q->size);
        }
    }

extern ts_writer_t *g_mux_ts_writer_handle;
    ts_show_queues(g_mux_ts_writer_handle);

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
            printf( "Input-stream-id: %d - Video: %s %dx%d%s %d/%dfps \n", stream->input_stream_id,
                    format_name, stream->width, stream->height, stream->interlaced ? "i" : "p",
                    stream->timebase_den, stream->timebase_num );

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
            printf("PSIP: DVB_TABLE_SECTION\n");
        }
        else if(input_stream->stream_type == STREAM_TYPE_MISC && input_stream->stream_format == SMPTE2038)
        {
            printf("PES_PRIVATE_1: SMPTE2038 packets\n");
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
        if (input_stream->stream_format == DVB_TABLE_SECTION) {
            if (cli.mux_opts.scte35_pid)
                output_stream->ts_opts.pid = cli.mux_opts.scte35_pid;
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

    return 0;
}

static int stop_encode( char *command, obecli_command_t *child )
{
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

    char msg[128];
    sprintf(msg, "Version 2.0 (" GIT_VERSION ")");
    printf("%s\n", msg);
    syslog(LOG_INFO, msg);

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
    printf("Supports  AVC via VAAPI: %s\n",
#if HAVE_VA_VA_H
        "true"
#else
        "false"
#endif
    );
    printf("Decklink SDK %s\n", BLACKMAGIC_DECKLINK_API_VERSION_STRING);
    printf("\n");

    if (exitcode) {
        printf("%s -s <script.txt>\n", prog);
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
