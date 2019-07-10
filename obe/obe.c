/*****************************************************************************
 * obe.c: open broadcast encoder functions
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

#include "common/common.h"
#include "common/lavc.h"
#include "input/input.h"
#include "filters/video/video.h"
#include "filters/audio/audio.h"
#include "encoders/video/video.h"
#include "encoders/audio/audio.h"
#include "mux/mux.h"
#include "output/output.h"

/* Avoid a minor compiler warning and defining GNU_SOURCE */
extern int pthread_setname_np(pthread_t thread, const char *name);

/** Utilities **/

const char *obe_ascii_datetime()
{
	char *s = calloc(1, 64);
	time_t now;
	time(&now);
	sprintf(s, "%s", ctime(&now));
	s[ strlen(s) - 1 ] = 0;
 
	return (const char *)s;
}

int64_t obe_mdate( void )
{
    struct timespec ts_current;
    clock_gettime( CLOCK_MONOTONIC, &ts_current );
    return (int64_t)ts_current.tv_sec * 1000000 + (int64_t)ts_current.tv_nsec / 1000;
}

int obe_timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y)
{
     /* Perform the carry for the later subtraction by updating y. */
     if (x->tv_usec < y->tv_usec)
     {
         int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
         y->tv_usec -= 1000000 * nsec;
         y->tv_sec += nsec;
     }
     if (x->tv_usec - y->tv_usec > 1000000)
     {
         int nsec = (x->tv_usec - y->tv_usec) / 1000000;
         y->tv_usec += 1000000 * nsec;
         y->tv_sec -= nsec;
     }

     /* Compute the time remaining to wait. tv_usec is certainly positive. */
     result->tv_sec = x->tv_sec - y->tv_sec;
     result->tv_usec = x->tv_usec - y->tv_usec;

     /* Return 1 if result is negative. */
     return x->tv_sec < y->tv_sec;
}

int64_t obe_timediff_to_msecs(struct timeval *tv)
{
        return (tv->tv_sec * 1000) + (tv->tv_usec / 1000);
}

int64_t obe_timediff_to_usecs(struct timeval *tv)
{
        return (tv->tv_sec * 1000000) + tv->tv_usec;
}

/** Create/Destroy **/
/* Input device */
obe_device_t *new_device( void )
{
    obe_device_t *device = calloc( 1, sizeof(obe_device_t) );

    if( !device )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return NULL;
    }

    return device;
}

void destroy_device( obe_device_t *device )
{
    for( int i = 0; i < device->num_input_streams; i++ )
        free( device->input_streams[i] );
    if( device->probed_streams )
        free( device->probed_streams );
    free( device );
}

/* Raw frame */
obe_raw_frame_t *new_raw_frame( void )
{
    obe_raw_frame_t *raw_frame = calloc( 1, sizeof(*raw_frame) );

    if( !raw_frame )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        return NULL;
    }

    return raw_frame;
}

/* Coded frame */
obe_coded_frame_t *new_coded_frame( int output_stream_id, int len )
{
    obe_coded_frame_t *coded_frame = calloc( 1, sizeof(*coded_frame) );
    if( !coded_frame )
        return NULL;

    coded_frame->output_stream_id = output_stream_id;
    coded_frame->len = len;
    coded_frame->data = malloc( len );
    if( !coded_frame->data )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        free( coded_frame );
        return NULL;
    }

    return coded_frame;
}

void destroy_coded_frame( obe_coded_frame_t *coded_frame )
{
    free( coded_frame->data );
    free( coded_frame );
}

void coded_frame_print(obe_coded_frame_t *cf)
{
	double v = (double)cf->len / (cf->cpb_final_arrival_time - cf->cpb_initial_arrival_time);
	printf("strm %d  type %c  len %7d  rpts %13" PRIi64 "  rdts %13" PRIi64 "  iat %13" PRIi64 "  fat %13" PRIi64 " (%11.09f)  at %13" PRIi64 "  pr %d  ra %d -- ",
		cf->output_stream_id,
		cf->type == CF_VIDEO ? 'V' :
		cf->type == CF_AUDIO ? 'A' : 'U',
		cf->len,
		cf->real_pts,
		cf->real_dts,
		cf->cpb_initial_arrival_time,
		cf->cpb_final_arrival_time,
		v,
		cf->arrival_time,
		cf->priority,
		cf->random_access);

	for (int i = 0; i < 16; i++)
		printf("%02x ", cf->data[i]);

	printf("\n");
}

size_t coded_frame_serializer_read(FILE *fh, obe_coded_frame_t **f)
{
	uint32_t flen = sizeof(*f);

	size_t rlen = 0;
	rlen += fread(&flen, 1, sizeof(flen), fh);
	if (flen != sizeof(obe_coded_frame_t))
		return 0;

	obe_coded_frame_t *cf = malloc(sizeof(*cf));
	if (!cf)
		return 0;

	rlen += fread(cf, 1, sizeof(*cf), fh);

	cf->data = malloc(cf->len);
	if (!cf->data) {
		free(cf);
		return 0;
	}

	rlen += fread(cf->data, 1, cf->len, fh);

	*f = cf;

	return rlen;
}

size_t coded_frame_serializer_write(FILE *fh, obe_coded_frame_t *cf)
{
	uint32_t flen = sizeof(*cf);

	size_t wlen = 0;
	wlen += fwrite(&flen, 1, sizeof(flen), fh);
	wlen += fwrite(cf, 1, sizeof(*cf), fh);
	wlen += fwrite(cf->data, 1, cf->len, fh);
	return wlen;
}

void obe_release_video_data( void *ptr )
{
     obe_raw_frame_t *raw_frame = ptr;
     av_freep( &raw_frame->alloc_img.plane[0] );
}

void obe_release_audio_data( void *ptr )
{
     obe_raw_frame_t *raw_frame = ptr;
     av_freep( &raw_frame->audio_frame.audio_data[0] );
}

void obe_release_frame( void *ptr )
{
     obe_raw_frame_t *raw_frame = ptr;
     for( int i = 0; i < raw_frame->num_user_data; i++ )
         free( raw_frame->user_data[i].data );
     free( raw_frame->user_data );
     free( raw_frame );
}

/* Muxed data */
obe_muxed_data_t *new_muxed_data( int len )
{
    obe_muxed_data_t *muxed_data = calloc( 1, sizeof(*muxed_data) );
    if( !muxed_data )
        return NULL;

    muxed_data->len = len;
    muxed_data->data = malloc( len );
    if( !muxed_data->data )
    {
        syslog( LOG_ERR, "Malloc failed\n" );
        free( muxed_data );
        return NULL;
    }

    return muxed_data;
}

void destroy_muxed_data( obe_muxed_data_t *muxed_data )
{
    if( muxed_data->pcr_list )
        free( muxed_data->pcr_list );

    free( muxed_data->data );
    free( muxed_data );
}

/** Add/Remove misc **/
void add_device( obe_t *h, obe_device_t *device )
{
    pthread_mutex_lock( &h->device_list_mutex );
    h->devices[h->num_devices++] = device;
    pthread_mutex_unlock( &h->device_list_mutex );
}

/* Filter queue */
int add_to_filter_queue( obe_t *h, obe_raw_frame_t *raw_frame )
{
    obe_filter_t *filter = NULL;

    for( int i = 0; i < h->num_filters; i++ )
    {
        for( int j = 0; j < h->filters[i]->num_stream_ids; j++ )
        {
            if( h->filters[i]->stream_id_list[j] == raw_frame->input_stream_id )
            {
                filter = h->filters[i];
                break;
            }
        }
    }

    if( !filter )
        return -1;

#if 0
PRINT_OBE_FILTER(filter, "ADD TO QUEUE");
#endif
    return add_to_queue( &filter->queue, raw_frame );
}

static void destroy_filter( obe_filter_t *filter )
{
    obe_raw_frame_t *raw_frame;
    pthread_mutex_lock( &filter->queue.mutex );
    for( int i = 0; i < filter->queue.size; i++ )
    {
        raw_frame = filter->queue.queue[i];
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
    }

    obe_destroy_queue( &filter->queue );

    free( filter->stream_id_list );
    free( filter );
}

/* Encode queue */
int add_to_encode_queue( obe_t *h, obe_raw_frame_t *raw_frame, int output_stream_id )
{
    obe_encoder_t *encoder = NULL;

    for( int i = 0; i < h->num_encoders; i++ )
    {
        if( h->encoders[i]->output_stream_id == output_stream_id )
        {
            encoder = h->encoders[i];
            break;
        }
    }

    if( !encoder )
        return -1;

    return add_to_queue( &encoder->queue, raw_frame );
}

static void destroy_encoder( obe_encoder_t *encoder )
{
    obe_raw_frame_t *raw_frame;
    pthread_mutex_lock( &encoder->queue.mutex );
    for( int i = 0; i < encoder->queue.size; i++ )
    {
        raw_frame = encoder->queue.queue[i];
        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
    }

    obe_destroy_queue( &encoder->queue );

    if( encoder->encoder_params )
        free( encoder->encoder_params );

    free( encoder );
}

static void destroy_enc_smoothing( obe_queue_t *queue )
{
    obe_coded_frame_t *coded_frame;
    pthread_mutex_lock( &queue->mutex );
    for( int i = 0; i < queue->size; i++ )
    {
        coded_frame = queue->queue[i];
        destroy_coded_frame( coded_frame );
    }

    obe_destroy_queue( queue );
}

static void destroy_mux( obe_t *h )
{
    pthread_mutex_lock( &h->mux_queue.mutex );
    for( int i = 0; i < h->mux_queue.size; i++ )
        destroy_coded_frame( h->mux_queue.queue[i] );

    obe_destroy_queue( &h->mux_queue );

    if( h->mux_opts.service_name )
        free( h->mux_opts.service_name );
    if( h->mux_opts.provider_name )
        free( h->mux_opts.provider_name );
}

static void destroy_mux_smoothing( obe_queue_t *queue )
{
    obe_muxed_data_t *muxed_data;
    pthread_mutex_lock( &queue->mutex );
    for( int i = 0; i < queue->size; i++ )
    {
        muxed_data = queue->queue[i];
        destroy_muxed_data( muxed_data );
    }

    obe_destroy_queue( queue );
}

int remove_early_frames( obe_t *h, int64_t pts )
{
    void **tmp;
    for( int i = 0; i < h->mux_queue.size; i++ )
    {
        obe_coded_frame_t *frame = h->mux_queue.queue[i];
        if (frame->type != CF_VIDEO && frame->pts < pts)
        {
            destroy_coded_frame( frame );
            memmove( &h->mux_queue.queue[i], &h->mux_queue.queue[i+1], sizeof(*h->mux_queue.queue) * (h->mux_queue.size-1-i) );
            tmp = realloc( h->mux_queue.queue, sizeof(*h->mux_queue.queue) * (h->mux_queue.size-1) );
            h->mux_queue.size--;
            i--;
            if( !tmp && h->mux_queue.size )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return -1;
            }
            h->mux_queue.queue = tmp;
        }
    }

    return 0;
}

/* Output queue */
static void destroy_output( obe_output_t *output )
{
    pthread_mutex_lock( &output->queue.mutex );
    for( int i = 0; i < output->queue.size; i++ )
        av_buffer_unref( output->queue.queue[i] );

    obe_destroy_queue( &output->queue );
    free( output );
}

/** Get items **/
/* Input stream */
obe_int_input_stream_t *get_input_stream( obe_t *h, int input_stream_id )
{
    for( int j = 0; j < h->devices[0]->num_input_streams; j++ )
    {
        if( h->devices[0]->input_streams[j]->input_stream_id == input_stream_id )
            return h->devices[0]->input_streams[j];
    }
    return NULL;
}

/* Encoder */
obe_encoder_t *get_encoder( obe_t *h, int output_stream_id )
{
    for( int i = 0; i < h->num_encoders; i++ )
    {
        if( h->encoders[i]->output_stream_id == output_stream_id )
            return h->encoders[i];
    }
    return NULL;
}

/* Output */
obe_output_stream_t *get_output_stream_by_id(obe_t *h, int output_stream_id)
{
    for( int i = 0; i < h->num_output_streams; i++ )
    {
        obe_output_stream_t *e = obe_core_get_output_stream_by_index(h, i);
        if (e->output_stream_id == output_stream_id)
            return e;
    }
    return NULL;
}

obe_output_stream_t *get_output_stream_by_format( obe_t *h, int format )
{
    for( int i = 0; i < h->num_output_streams; i++ )
    {
        obe_output_stream_t *e = obe_core_get_output_stream_by_index(h, i);
        if (e->stream_format == format)
            return e;
    }
    return NULL;
}

/* Syslog retains a pointer to the label. */
static char g_logSuffix[128] = { 0 };
obe_t *obe_setup(const char *syslogSuffix)
{
    if (syslogSuffix) {
        sprintf(g_logSuffix, "obe-%s", syslogSuffix);
    } else
        strcpy(g_logSuffix, "obe");

    openlog(g_logSuffix, LOG_NDELAY | LOG_PID, LOG_USER);

    if( X264_BIT_DEPTH == 9 || X264_BIT_DEPTH > 10 )
    {
        fprintf( stderr, "x264 bit-depth of %i not supported\n", X264_BIT_DEPTH );
        return NULL;
    }

    obe_t *h = calloc( 1, sizeof(*h) );
    if( !h )
    {
        fprintf( stderr, "Malloc failed\n" );
        return NULL;
    }
    h->probe_time_seconds = MAX_PROBE_TIME;

    pthread_mutex_init( &h->device_list_mutex, NULL );

    if( av_lockmgr_register( obe_lavc_lockmgr ) < 0 )
    {
        fprintf( stderr, "Could not register lavc lock manager\n" );
        free( h );
        return NULL;
    }

    return h;
}

int obe_set_config( obe_t *h, int system_type )
{
    if( system_type < OBE_SYSTEM_TYPE_GENERIC && system_type > OBE_SYSTEM_TYPE_LOW_LATENCY )
    {
        fprintf( stderr, "Invalid OBE system type\n" );
        return -1;
    }

    h->obe_system = system_type;

    return 0;
}

/* TODO handle error conditions */
int64_t get_wallclock_in_mpeg_ticks( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );

    return ((int64_t)ts.tv_sec * (int64_t)27000000) + (int64_t)(ts.tv_nsec * 27 / 1000);
}

void sleep_mpeg_ticks( int64_t i_time )
{
    struct timespec ts;
    ts.tv_sec = i_time / 27000000;
    ts.tv_nsec = ((i_time % 27000000) * 1000) / 27;

    clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, &ts );
}

void obe_clock_tick( obe_t *h, int64_t value )
{
    /* Use this signal as the SDI clocksource */
    pthread_mutex_lock( &h->obe_clock_mutex );
    h->obe_clock_last_pts = value;
    h->obe_clock_last_wallclock = get_wallclock_in_mpeg_ticks();
    pthread_mutex_unlock( &h->obe_clock_mutex );
    pthread_cond_broadcast( &h->obe_clock_cv );
}

int64_t get_input_clock_in_mpeg_ticks( obe_t *h )
{
    int64_t value;
    pthread_mutex_lock( &h->obe_clock_mutex );
    value = h->obe_clock_last_pts + ( get_wallclock_in_mpeg_ticks() - h->obe_clock_last_wallclock );
    pthread_mutex_unlock( &h->obe_clock_mutex );

    return value;
}

void sleep_input_clock( obe_t *h, int64_t i_time )
{
    int64_t wallclock_time;
    pthread_mutex_lock( &h->obe_clock_mutex );
    wallclock_time = ( i_time - h->obe_clock_last_pts ) + h->obe_clock_last_wallclock;
    pthread_mutex_unlock( &h->obe_clock_mutex );

    sleep_mpeg_ticks( wallclock_time );
}

int get_non_display_location( int type )
{
    /* Set the appropriate location */
    for( int i = 0; non_display_data_locations[i].service != -1; i++ )
    {
        if( non_display_data_locations[i].service == type )
            return non_display_data_locations[i].location;
    }

    return -1;
}

static int obe_validate_input_params( obe_input_t *input_device )
{
    if( input_device->input_type == INPUT_DEVICE_DECKLINK )
    {
        /* TODO check */
    }

    return 0;
}

static int __pthread_cancel( pthread_t thread )
{
    if ( thread )
        return pthread_cancel( thread );
    return -1;
}

static int __pthread_join( pthread_t thread, void **retval )
{
    if ( thread )
        return pthread_join( thread, retval );
    return -1;
}

int obe_probe_device( obe_t *h, obe_input_t *input_device, obe_input_program_t *program )
{
    pthread_t thread;
    void *ret_ptr;
    obe_int_input_stream_t *stream_in;
    obe_input_stream_t *stream_out;
    obe_input_probe_t *args = NULL;

    obe_input_func_t  input;

    int i = 0;
    int prev_devices = h->num_devices;
    int cur_devices;

    if( !input_device || !program )
    {
        fprintf( stderr, "Invalid input pointers \n" );
        return -1;
    }

    if( h->num_devices == MAX_DEVICES )
    {
        fprintf( stderr, "No more devices allowed \n" );
        return -1;
    }

    if( input_device->input_type == INPUT_URL )
    {
        //input = lavf_input;
        fprintf( stderr, "URL input is not supported currently \n" );
        goto fail;
    }
#if HAVE_DECKLINK
    else if( input_device->input_type == INPUT_DEVICE_DECKLINK )
        input = decklink_input;
#endif
#if HAVE_BLUEDRIVER_P_H
    else if (input_device->input_type == INPUT_DEVICE_BLUEFISH)
        input = bluefish_input;
#endif
#if HAVE_PROCESSING_NDI_LIB_H
    else if (input_device->input_type == INPUT_DEVICE_NDI)
        input = ndi_input;
#endif
    else if (input_device->input_type == INPUT_DEVICE_V210)
        input = v210_input;
    else if( input_device->input_type == INPUT_DEVICE_LINSYS_SDI )
        input = linsys_sdi_input;
    else if (input_device->input_type == INPUT_DEVICE_V4L2)
        input = v4l2_input;
    else
    {
        fprintf( stderr, "Invalid input device \n" );
        return -1;
    }

    if( input_device->input_type == INPUT_URL && !input_device->location )
    {
        fprintf( stderr, "Invalid input location\n" );
        return -1;
    }

    args = malloc( sizeof(*args) );
    if( !args )
    {
        fprintf( stderr, "Malloc failed \n" );
        return -1;
    }

    args->h = h;
    memcpy( &args->user_opts, input_device, sizeof(*input_device) );
    if( input_device->location )
    {
       args->user_opts.location = malloc( strlen( input_device->location ) + 1 );
       if( !args->user_opts.location)
       {
           fprintf( stderr, "Malloc failed \n" );
           goto fail;
        }

        strcpy( args->user_opts.location, input_device->location );
    }

    if( obe_validate_input_params( input_device ) < 0 )
        goto fail;

    if( pthread_create( &thread, NULL, input.probe_input, (void*)args ) < 0 )
    {
        fprintf( stderr, "Couldn't create probe thread \n" );
        goto fail;
    }
    pthread_setname_np(thread, "obe-probe");

    if( input_device->location )
        printf( "Probing device: \"%s\". ", input_device->location );
    else if( input_device->input_type == INPUT_DEVICE_LINSYS_SDI )
        printf( "Probing device: Linsys card %i. ", input_device->card_idx );
    else if (input_device->input_type == INPUT_DEVICE_V4L2)
        printf( "Probing device: V4L2 card %i. ", input_device->card_idx);
#if HAVE_BLUEDRIVER_P_H
    else if (input_device->input_type == INPUT_DEVICE_BLUEFISH)
        printf( "Probing device: BlueFish card %i. ", input_device->card_idx);
#endif
#if HAVE_PROCESSING_NDI_LIB_H
    else if (input_device->input_type == INPUT_DEVICE_NDI)
        printf( "Probing device: NDI input %i. ", input_device->card_idx);
#endif
    else if (input_device->input_type == INPUT_DEVICE_V210)
        printf( "Probing device: YUV card %i. ", input_device->card_idx);
    else
        printf( "Probing device: Decklink card %i. ", input_device->card_idx );

    printf("Timeout %i seconds\n", h->probe_time_seconds);

    while (i++ < h->probe_time_seconds)
    {
        sleep( 1 );
        fprintf( stderr, "." );

        if( pthread_kill( thread, 0 ) == ESRCH )
            break;
    }

    __pthread_cancel( thread );
    __pthread_join( thread, &ret_ptr );

    cur_devices = h->num_devices;

    if( prev_devices == cur_devices )
    {
        fprintf( stderr, "Could not probe device \n" );
        program = NULL;
        args = NULL;
        goto fail;
    }

    // TODO metadata etc
    program->num_streams = h->devices[h->num_devices-1]->num_input_streams;
    program->streams = calloc( program->num_streams, sizeof(*program->streams) );
    if( !program->streams )
    {
        fprintf( stderr, "Malloc failed \n" );
        goto fail;
    }

    h->devices[h->num_devices-1]->probed_streams = program->streams;

    /* Clone all of the probed input parameters into OBE's source abstraction. */
    for( i = 0; i < program->num_streams; i++ )
    {
        stream_in = h->devices[h->num_devices-1]->input_streams[i];
        stream_out = &program->streams[i];

        stream_out->input_stream_id = stream_in->input_stream_id;
        stream_out->stream_type = stream_in->stream_type;
        stream_out->stream_format = stream_in->stream_format;

        stream_out->bitrate = stream_in->bitrate;
        stream_out->sdi_audio_pair = stream_in->sdi_audio_pair;

        stream_out->num_frame_data = stream_in->num_frame_data;
        stream_out->frame_data = stream_in->frame_data;

        if( stream_in->stream_type == STREAM_TYPE_VIDEO )
        {
            memcpy( &stream_out->csp, &stream_in->csp, offsetof( obe_input_stream_t, timebase_num ) - offsetof( obe_input_stream_t, csp ) );
            stream_out->timebase_num = stream_in->timebase_num;
            stream_out->timebase_den = stream_in->timebase_den;
        }
        else if( stream_in->stream_type == STREAM_TYPE_AUDIO )
        {
            memcpy( &stream_out->channel_layout, &stream_in->channel_layout,
            offsetof( obe_input_stream_t, bitrate ) - offsetof( obe_input_stream_t, channel_layout ) );
            stream_out->aac_is_latm = stream_in->is_latm;
        }

        memcpy( stream_out->lang_code, stream_in->lang_code, 4 );
    }

    return 0;

fail:
    if( args )
    {
        if( args->user_opts.location )
            free( args->user_opts.location );
        free( args );
    }

    return -1;
}

int obe_populate_avc_encoder_params( obe_t *h, int input_stream_id, x264_param_t *param, const char *preset_name)
{
    obe_int_input_stream_t *stream = get_input_stream( h, input_stream_id );
    if( !stream )
    {
        fprintf( stderr, "Could not find stream \n" );
        return -1;
    }

    if( stream->stream_type != STREAM_TYPE_VIDEO )
    {
        fprintf( stderr, "Stream type is not video \n" );
        return -1;
    }

    if( !param )
    {
        fprintf( stderr, "Invalid parameter pointer \n" );
        return -1;
    }

    if( h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY || h->obe_system == OBE_SYSTEM_TYPE_LOW_LATENCY ) {
        x264_param_default_preset(param, preset_name, "zerolatency");
        // printf("Using x264 preset: %s\n", stream->preset_name);
    } else
        x264_param_default( param );

    param->b_deterministic = 0;
    param->b_vfr_input = 0;
    param->b_pic_struct = 1;
    param->b_open_gop = 1;
    param->rc.i_rc_method = X264_RC_ABR;

    param->i_width = stream->width;
    param->i_height = stream->height;

    param->i_fps_num = stream->timebase_den;
    param->i_fps_den = stream->timebase_num;
    param->b_interlaced = stream->interlaced;
    if( param->b_interlaced )
        param->b_tff = stream->tff;

    /* A reasonable default. x264 won't go higher than this parameter irrespective of speedcontrol */
    if( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
        param->i_frame_reference = 4;

    if( stream->sar_num && stream->sar_den )
    {
        param->vui.i_sar_width  = stream->sar_num;
        param->vui.i_sar_height = stream->sar_den;
    }

    param->vui.i_overscan = 2;

    if( ( param->i_fps_num == 25 || param->i_fps_num == 50 ) && param->i_fps_den == 1 )
    {
        param->vui.i_vidformat = 1; // PAL
        param->vui.i_colorprim = 5; // BT.470-2 bg
        param->vui.i_transfer  = 5; // BT.470-2 bg
        param->vui.i_colmatrix = 5; // BT.470-2 bg
        param->i_keyint_max = param->i_fps_num == 50 ? 48 : 24;
    }
    else if( ( param->i_fps_num == 30000 || param->i_fps_num == 60000 ) && param->i_fps_den == 1001 )
    {
        param->vui.i_vidformat = 2; // NTSC
        param->vui.i_colorprim = 6; // BT.601-6
        param->vui.i_transfer  = 6; // BT.601-6
        param->vui.i_colmatrix = 6; // BT.601-6
        param->i_keyint_max = param->i_fps_num / 1000;
    } else if( ( param->i_fps_num == 30 || param->i_fps_num == 60 ) && param->i_fps_den == 1 )
    {
        param->vui.i_vidformat = 2; // NTSC
        param->vui.i_colorprim = 6; // BT.601-6
        param->vui.i_transfer  = 6; // BT.601-6
        param->vui.i_colmatrix = 6; // BT.601-6
        param->i_keyint_max = param->i_fps_num == 60 ? 59 : 29;
    }
    else
    {
        param->vui.i_vidformat = 5; // undefined
        param->vui.i_colorprim = 2; // undefined
        param->vui.i_transfer  = 2; // undefined
        param->vui.i_colmatrix = 2; // undefined
    }

    /* Change to BT.709 for HD resolutions */
    if( param->i_width >= 1280 && param->i_height >= 720 )
    {
        param->vui.i_colorprim = 1;
        param->vui.i_transfer  = 1;
        param->vui.i_colmatrix = 1;
    }

    x264_param_apply_profile( param, X264_BIT_DEPTH == 10 ? "high10" : "high" );
#if X264_BUILD < 148
    param->i_nal_hrd = X264_NAL_HRD_FAKE_VBR;
#else
    param->i_nal_hrd = X264_NAL_HRD_VBR;
#endif
    param->b_aud = 1;
    param->i_log_level = X264_LOG_INFO;

    //param->rc.f_vbv_buffer_init = 0.1;

    if( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
    {
#if X264_BUILD < 148
        param->sc.f_speed = 1.0;
        param->sc.b_alt_timer = 1;
        if( param->i_width >= 1280 && param->i_height >= 720 )
            param->sc.max_preset = 7; /* on the conservative side for HD */
        else
        {
            param->sc.max_preset = 10;
            param->i_bframe_adaptive = X264_B_ADAPT_TRELLIS;
        }
#endif

        param->rc.i_lookahead = param->i_keyint_max;
    }

    return 0;
}

int obe_setup_streams( obe_t *h, obe_output_stream_t *output_streams, int num_streams )
{
    if( num_streams <= 0 )
    {
        fprintf( stderr, "Must have at least one stream \n" );
        return -1;
    }
    // TODO sanity check the inputs

    h->num_output_streams = num_streams;
    h->priv_output_streams = malloc(num_streams * sizeof(*h->priv_output_streams));
    if (!h->priv_output_streams)
    {
        fprintf( stderr, "Malloc failed \n" );
        return -1;
    }
    memcpy(h->priv_output_streams, output_streams, num_streams * sizeof(*h->priv_output_streams));

    for (int i = 0; i < num_streams; i++)
        obe_core_dump_output_stream(&output_streams[i], i);

    // TODO sort out VBI

    return 0;
}

int obe_setup_muxer( obe_t *h, obe_mux_opts_t *mux_opts )
{
    // TODO sanity check

    memcpy( &h->mux_opts, mux_opts, sizeof(obe_mux_opts_t) );

    if( mux_opts->service_name )
    {
        h->mux_opts.service_name = malloc( strlen( mux_opts->service_name ) + 1 );
        if( !h->mux_opts.service_name )
        {
           fprintf( stderr, "Malloc failed \n" );
           return -1;
        }

        strcpy( h->mux_opts.service_name, mux_opts->service_name );
    }
    if( mux_opts->provider_name )
    {
        h->mux_opts.provider_name = malloc( strlen( mux_opts->provider_name ) + 1 );
        if( !h->mux_opts.provider_name )
        {
            fprintf( stderr, "Malloc failed \n" );
            return -1;
        }

        strcpy( h->mux_opts.provider_name, mux_opts->provider_name );
    }

    return 0;
}

int obe_setup_output( obe_t *h, obe_output_opts_t *output_opts )
{
    // TODO further sanity checks
    if( output_opts->num_outputs <= 0 )
    {
       fprintf( stderr, "Invalid number of outputs \n" );
       return -1;
    }

    h->outputs = malloc( output_opts->num_outputs * sizeof(*h->outputs) );
    if( !h->outputs )
    {
       fprintf( stderr, "Malloc failed\n" );
       return -1;
    }

    for( int i = 0; i < output_opts->num_outputs; i++ )
    {
        h->outputs[i] = calloc( 1, sizeof(*h->outputs[i]) );
        if( !h->outputs[i] )
        {
           fprintf( stderr, "Malloc failed\n" );
           return -1;
        }
        h->outputs[i]->output_dest.type = output_opts->outputs[i].type;
        if( output_opts->outputs[i].target )
        {
            h->outputs[i]->output_dest.target = malloc( strlen( output_opts->outputs[i].target ) + 1 );
            if( !h->outputs[i]->output_dest.target )
            {
                fprintf( stderr, "Malloc failed\n" );
                return -1;
            }
            strcpy( h->outputs[i]->output_dest.target, output_opts->outputs[i].target );
        }
    }
    h->num_outputs = output_opts->num_outputs;

    return 0;
}

/* LOS frame injection. */
extern int g_decklink_inject_frame_enable;

int obe_start( obe_t *h )
{
    obe_int_input_stream_t  *input_stream;
    obe_vid_filter_params_t *vid_filter_params;
    obe_aud_filter_params_t *aud_filter_params;
    obe_vid_enc_params_t *vid_enc_params;
    obe_aud_enc_params_t *aud_enc_params;

    obe_input_func_t  input;
    obe_aud_enc_func_t audio_encoder;
    obe_output_func_t output;

    int num_samples = 0;

    /* TODO: a lot of sanity checks */
    /* TODO: decide upon thread priorities */

    /* Setup mutexes and cond vars */
    pthread_mutex_init( &h->devices[0]->device_mutex, NULL );
    pthread_mutex_init( &h->drop_mutex, NULL );
    obe_init_queue( &h->enc_smoothing_queue, "encoder smoothing" );
    obe_init_queue( &h->mux_queue, "mux" );
    obe_init_queue( &h->mux_smoothing_queue, "mux smoothing" );
    pthread_mutex_init( &h->obe_clock_mutex, NULL );
    pthread_cond_init( &h->obe_clock_cv, NULL );

    if( h->devices[0]->device_type == INPUT_URL )
    {
        //input = lavf_input;
        fprintf( stderr, "URL input is not supported currently \n" );
        goto fail;
    }
#if HAVE_DECKLINK
    else if( h->devices[0]->device_type == INPUT_DEVICE_DECKLINK )
        input = decklink_input;
#endif
#if HAVE_BLUEDRIVER_P_H
    else if( h->devices[0]->device_type == INPUT_DEVICE_BLUEFISH )
        input = bluefish_input;
#endif
#if HAVE_PROCESSING_NDI_LIB_H
    else if (h->devices[0]->device_type == INPUT_DEVICE_NDI)
        input = ndi_input;
#endif
    else if( h->devices[0]->device_type == INPUT_DEVICE_V210)
        input = v210_input;
    else if( h->devices[0]->device_type == INPUT_DEVICE_LINSYS_SDI )
        input = linsys_sdi_input;
    else if (h->devices[0]->device_type == INPUT_DEVICE_V4L2)
        input = v4l2_input;
    else
    {
        fprintf( stderr, "Invalid input device \n" );
        goto fail;
    }

    /* Open Output Threads */
    for( int i = 0; i < h->num_outputs; i++ )
    {
        char n[64];
        sprintf(n, "outputs #%d", i);
        obe_init_queue( &h->outputs[i]->queue, n );

        switch (h->outputs[i]->output_dest.type) {
        case OUTPUT_UDP:
        case OUTPUT_RTP:
            output = ip_output;
            break;
        case OUTPUT_FILE_TS:
            output = file_ts_output;
            break;
        default:
            fprintf(stderr, "Invalid output type, undefined.\n");
            goto fail;
        }

        if( pthread_create( &h->outputs[i]->output_thread, NULL, output.open_output, (void*)h->outputs[i] ) < 0 )
        {
            fprintf( stderr, "Couldn't create output thread \n" );
            goto fail;
        }
        pthread_setname_np(h->outputs[i]->output_thread, "obe-output");
    }

    /* Open Encoder Threads */
    for( int i = 0; i < h->num_output_streams; i++ )
    {
        obe_output_stream_t *os = obe_core_get_output_stream_by_index(h, i);
        if (os->stream_action == STREAM_ENCODE )
        {
            h->encoders[h->num_encoders] = calloc( 1, sizeof(obe_encoder_t) );
            if( !h->encoders[h->num_encoders] )
            {
                fprintf( stderr, "Malloc failed \n" );
                goto fail;
            }
            char n[64];
            sprintf(n, "output stream #%d", i);
            obe_init_queue( &h->encoders[h->num_encoders]->queue, n);
            h->encoders[h->num_encoders]->output_stream_id = os->output_stream_id;

            obe_output_stream_t *ostream = obe_core_get_output_stream_by_index(h, i);

            if (ostream->stream_format == VIDEO_AVC )
            {
                x264_param_t *x264_param = &os->avc_param;
                if( h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY )
                {
                    /* This doesn't need to be particularly accurate since x264 calculates the correct value internally */
                    x264_param->rc.i_vbv_buffer_size = (double)x264_param->rc.i_vbv_max_bitrate * x264_param->i_fps_den / x264_param->i_fps_num;
                }

                vid_enc_params = calloc( 1, sizeof(*vid_enc_params) );
                if( !vid_enc_params )
                {
                    fprintf( stderr, "Malloc failed \n" );
                    goto fail;
                }
                vid_enc_params->h = h;
                vid_enc_params->encoder = h->encoders[h->num_encoders];
                h->encoders[h->num_encoders]->is_video = 1;

                memcpy(&vid_enc_params->avc_param, &ostream->avc_param, sizeof(x264_param_t));
                if( pthread_create( &h->encoders[h->num_encoders]->encoder_thread, NULL, x264_obe_encoder.start_encoder, (void*)vid_enc_params ) < 0 )
                {
                    fprintf( stderr, "Couldn't create x264 encode thread\n" );
                    goto fail;
                }
                pthread_setname_np(h->encoders[h->num_encoders]->encoder_thread, "obe-x264-encoder");
            }
#if HAVE_X265_H
            else if (ostream->stream_format == VIDEO_HEVC_X265)
            {
                x264_param_t *x264_param = &ostream->avc_param;
                if( h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY )
                {
                    /* This doesn't need to be particularly accurate since x264 calculates the correct value internally */
                    x264_param->rc.i_vbv_buffer_size = (double)x264_param->rc.i_vbv_max_bitrate * x264_param->i_fps_den / x264_param->i_fps_num;
                }

                vid_enc_params = calloc( 1, sizeof(*vid_enc_params) );
                if( !vid_enc_params )
                {
                    fprintf( stderr, "Malloc failed\n" );
                    goto fail;
                }
                vid_enc_params->h = h;
                vid_enc_params->encoder = h->encoders[h->num_encoders];
                h->encoders[h->num_encoders]->is_video = 1;

                memcpy(&vid_enc_params->avc_param, &ostream->avc_param, sizeof(x264_param_t));
                if (pthread_create(&h->encoders[h->num_encoders]->encoder_thread, NULL, x265_obe_encoder.start_encoder, (void*)vid_enc_params) < 0)
                {
                    fprintf( stderr, "Couldn't create x265 encode thread\n" );
                    goto fail;
                }
                pthread_setname_np(h->encoders[h->num_encoders]->encoder_thread, "obe-x265-encoder");
            }
#endif
#if HAVE_VA_VA_H
            else if (ostream->stream_format == VIDEO_AVC_VAAPI)
            {
                x264_param_t *x264_param = &ostream->avc_param;
                if( h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY )
                {
                    /* This doesn't need to be particularly accurate since x264 calculates the correct value internally */
                    x264_param->rc.i_vbv_buffer_size = (double)x264_param->rc.i_vbv_max_bitrate * x264_param->i_fps_den / x264_param->i_fps_num;
                }

                vid_enc_params = calloc( 1, sizeof(*vid_enc_params) );
                if( !vid_enc_params )
                {
                    fprintf( stderr, "Malloc failed\n" );
                    goto fail;
                }
                vid_enc_params->h = h;
                vid_enc_params->encoder = h->encoders[h->num_encoders];
                h->encoders[h->num_encoders]->is_video = 1;

                memcpy(&vid_enc_params->avc_param, &ostream->avc_param, sizeof(x264_param_t));
                if (pthread_create(&h->encoders[h->num_encoders]->encoder_thread, NULL, avc_vaapi_obe_encoder.start_encoder, (void*)vid_enc_params) < 0)
                {
                    fprintf( stderr, "Couldn't create x265 encode thread\n" );
                    goto fail;
                }
                pthread_setname_np(h->encoders[h->num_encoders]->encoder_thread, "obe-vid-avcva");
            }
            else if (ostream->stream_format == VIDEO_HEVC_VAAPI)
            {
                x264_param_t *x264_param = &ostream->avc_param;
                if( h->obe_system == OBE_SYSTEM_TYPE_LOWEST_LATENCY )
                {
                    /* This doesn't need to be particularly accurate since x264 calculates the correct value internally */
                    x264_param->rc.i_vbv_buffer_size = (double)x264_param->rc.i_vbv_max_bitrate * x264_param->i_fps_den / x264_param->i_fps_num;
                }

                vid_enc_params = calloc( 1, sizeof(*vid_enc_params) );
                if( !vid_enc_params )
                {
                    fprintf( stderr, "Malloc failed\n" );
                    goto fail;
                }
                vid_enc_params->h = h;
                vid_enc_params->encoder = h->encoders[h->num_encoders];
                h->encoders[h->num_encoders]->is_video = 1;

                memcpy( &vid_enc_params->avc_param, &ostream->avc_param, sizeof(x264_param_t) );
                if (pthread_create(&h->encoders[h->num_encoders]->encoder_thread, NULL, hevc_vaapi_obe_encoder.start_encoder, (void*)vid_enc_params) < 0)
                {
                    fprintf( stderr, "Couldn't create x265 encode thread\n" );
                    goto fail;
                }
                pthread_setname_np(h->encoders[h->num_encoders]->encoder_thread, "obe-vid-hevcva");
            }
#endif
            else if (ostream->stream_format == AUDIO_AC_3_BITSTREAM) {
                input_stream = get_input_stream(h, ostream->input_stream_id);
                ostream->sdi_audio_pair = input_stream->sdi_audio_pair;
                aud_enc_params = calloc(1, sizeof(*aud_enc_params));
                if(!aud_enc_params) {
                    fprintf(stderr, "Malloc failed\n");
                    goto fail;
                }
                aud_enc_params->h = h;
                aud_enc_params->encoder = h->encoders[h->num_encoders];
                aud_enc_params->stream = ostream;

                if (pthread_create(&h->encoders[h->num_encoders]->encoder_thread, NULL, ac3bitstream_encoder.start_encoder, (void*)aud_enc_params ) < 0 )
                {
                    fprintf(stderr, "Couldn't create ac3bitstream encode thread\n");
                    goto fail;
                }
                pthread_setname_np(h->encoders[h->num_encoders]->encoder_thread, "obe-aud-encoder");
            }
            else if (ostream->stream_format == AUDIO_AC_3 || ostream->stream_format == AUDIO_E_AC_3 ||
                     ostream->stream_format == AUDIO_AAC  || ostream->stream_format == AUDIO_MP2)
            {
                audio_encoder = ostream->stream_format == AUDIO_MP2 ? twolame_encoder : lavc_encoder;
                num_samples = ostream->stream_format == AUDIO_MP2 ? MP2_NUM_SAMPLES :
                              ostream->stream_format == AUDIO_AAC ? AAC_NUM_SAMPLES : AC3_NUM_SAMPLES;

                aud_enc_params = calloc( 1, sizeof(*aud_enc_params) );
                if( !aud_enc_params )
                {
                    fprintf( stderr, "Malloc failed \n" );
                    goto fail;
                }
                aud_enc_params->h = h;
                aud_enc_params->encoder = h->encoders[h->num_encoders];
                aud_enc_params->stream = ostream;

                input_stream = get_input_stream(h, ostream->input_stream_id);
                aud_enc_params->input_sample_format = input_stream->sample_format;
                aud_enc_params->sample_rate = input_stream->sample_rate;
                /* TODO: check the bitrate is allowed by the format */

                ostream->sdi_audio_pair = input_stream->sdi_audio_pair;

                /* Choose the optimal number of audio frames per PES
                 * TODO: This should be set after the encoder has told us the frame size */
                if( !ostream->ts_opts.frames_per_pes && h->obe_system == OBE_SYSTEM_TYPE_GENERIC &&
                    ostream->stream_format != AUDIO_E_AC_3 )
                {
                    int buf_size = ostream->stream_format == AUDIO_MP2 || ostream->stream_format == AUDIO_AAC ? MISC_AUDIO_BS : AC3_BS_DVB;
                    if( buf_size == AC3_BS_DVB && ( h->mux_opts.ts_type == OBE_TS_TYPE_CABLELABS || h->mux_opts.ts_type == OBE_TS_TYPE_ATSC ) )
                        buf_size = AC3_BS_ATSC;
                    /* AAC does not have exact frame sizes but this should be a good approximation */
                    int single_frame_size = (double)num_samples * 125 * ostream->bitrate / input_stream->sample_rate;
                    if (ostream->aac_opts.aac_profile == AAC_HE_V1 || ostream->aac_opts.aac_profile == AAC_HE_V2)
                        single_frame_size <<= 1;
                    int frames_per_pes = MAX( buf_size / single_frame_size, 1 );
                    frames_per_pes = MIN( frames_per_pes, 6 );
                    ostream->ts_opts.frames_per_pes = aud_enc_params->frames_per_pes = frames_per_pes;
                }
                else
                    ostream->ts_opts.frames_per_pes = aud_enc_params->frames_per_pes = 1;

                /* Determine whether we want the TWOLAME (only) audio encoder to rebase its time from the head of its fifo,
                 * and reset bases its clock from the h/w every 100ms or so.
                 */
                if (g_decklink_inject_frame_enable)
                    aud_enc_params->use_fifo_head_timing = 1;
                else
                    aud_enc_params->use_fifo_head_timing = 0;

                if( pthread_create( &h->encoders[h->num_encoders]->encoder_thread, NULL, audio_encoder.start_encoder, (void*)aud_enc_params ) < 0 )
                {
                    fprintf( stderr, "Couldn't create encode thread \n" );
                    goto fail;
                }
                pthread_setname_np(h->encoders[h->num_encoders]->encoder_thread, "obe-aud-encoder");
            }

            h->num_encoders++;
        }
    }

    if( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
    {
        /* Open Encoder Smoothing Thread */
        if( pthread_create( &h->enc_smoothing_thread, NULL, enc_smoothing.start_smoothing, (void*)h ) < 0 )
        {
            fprintf( stderr, "Couldn't create encoder smoothing thread \n" );
            goto fail;
        }
        pthread_setname_np(h->enc_smoothing_thread, "obe-enc-smoothing");
    }

    /* Open Mux Smoothing Thread */
    if( pthread_create( &h->mux_smoothing_thread, NULL, mux_smoothing.start_smoothing, (void*)h ) < 0 )
    {
        fprintf( stderr, "Couldn't create mux smoothing thread \n" );
        goto fail;
    }
    pthread_setname_np(h->mux_smoothing_thread, "obe-mux-smoothing");

    /* Open Mux Thread */
    obe_mux_params_t *mux_params = calloc( 1, sizeof(*mux_params) );
    if( !mux_params )
    {
        fprintf( stderr, "Malloc failed \n" );
        goto fail;
    }
    mux_params->h = h;
    mux_params->device = h->devices[0];
    mux_params->num_output_streams = h->num_output_streams;
    mux_params->output_streams = obe_core_get_output_stream_by_index(h, 0);

    if( pthread_create( &h->mux_thread, NULL, ts_muxer.open_muxer, (void*)mux_params ) < 0 )
    {
        fprintf( stderr, "Couldn't create mux thread \n" );
        goto fail;
    }
    pthread_setname_np(h->mux_thread, "obe-muxer");

    /* Open Filter Thread */
    for( int i = 0; i < h->devices[0]->num_input_streams; i++ )
    {
        input_stream = h->devices[0]->input_streams[i];
        if( input_stream && ( input_stream->stream_type == STREAM_TYPE_VIDEO || input_stream->stream_type == STREAM_TYPE_AUDIO ) )
        {
            h->filters[h->num_filters] = calloc( 1, sizeof(obe_filter_t) );
            if( !h->filters[h->num_filters] )
                goto fail;

            char n[64];
            if (input_stream->stream_type == STREAM_TYPE_VIDEO)
                sprintf(n, "input stream #%d [VIDEO]", i);
            else
            if (input_stream->stream_type == STREAM_TYPE_AUDIO)
                sprintf(n, "input stream #%d [AUDIO]", i);
            else
                sprintf(n, "input stream #%d [OTHER]", i);

            obe_init_queue( &h->filters[h->num_filters]->queue, n );

            h->filters[h->num_filters]->num_stream_ids = 1;
            h->filters[h->num_filters]->stream_id_list = malloc( sizeof(*h->filters[h->num_filters]->stream_id_list) );
            if( !h->filters[h->num_filters]->stream_id_list )
            {
                fprintf( stderr, "Malloc failed\n" );
                goto fail;
            }

            h->filters[h->num_filters]->stream_id_list[0] = input_stream->input_stream_id;

            if( input_stream->stream_type == STREAM_TYPE_VIDEO )
            {
                vid_filter_params = calloc( 1, sizeof(*vid_filter_params) );
                if( !vid_filter_params )
                {
                    fprintf( stderr, "Malloc failed\n" );
                    goto fail;
                }

                vid_filter_params->h = h;
                vid_filter_params->filter = h->filters[h->num_filters];
                vid_filter_params->input_stream = input_stream;
                obe_output_stream_t *ostream = obe_core_get_output_stream_by_index(h, i);
                vid_filter_params->target_csp = ostream->avc_param.i_csp & X264_CSP_MASK;
#if 0
                vid_filter_params->target_csp = X264_CSP_I422;
#endif

                if( pthread_create( &h->filters[h->num_filters]->filter_thread, NULL, video_filter.start_filter, vid_filter_params ) < 0 )
                {
                    fprintf( stderr, "Couldn't create video filter thread \n" );
                    goto fail;
                }
                pthread_setname_np(h->filters[h->num_filters]->filter_thread, "obe-vid-filter");
#if 0
PRINT_OBE_FILTER(h->filters[h->num_filters], "VIDEO FILTER");
#endif
            }
            else
            {
#if 0
PRINT_OBE_FILTER(h->filters[h->num_filters], "AUDIO FILTER");
#endif
                aud_filter_params = calloc( 1, sizeof(*aud_filter_params) );
                if( !aud_filter_params )
                {
                    fprintf( stderr, "Malloc failed\n" );
                    goto fail;
                }

                aud_filter_params->h = h;
                aud_filter_params->filter = h->filters[h->num_filters];

                if( pthread_create( &h->filters[h->num_filters]->filter_thread, NULL, audio_filter.start_filter, aud_filter_params ) < 0 )
                {
                    fprintf( stderr, "Couldn't create filter thread \n" );
                    goto fail;
                }
                pthread_setname_np(h->filters[h->num_filters]->filter_thread, "obe-aud-filter");
            }

            h->num_filters++;
        }
    }

    /* Open Input Thread */
    obe_input_params_t *input_params = calloc( 1, sizeof(*input_params) );
    if( !input_params )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto fail;
    }
    input_params->h = h;
    input_params->device = h->devices[0];

    /* TODO: in the future give it only the streams which are necessary */
    input_params->audio_samples = num_samples;

    if( pthread_create( &h->devices[0]->device_thread, NULL, input.open_input, (void*)input_params ) < 0 )
    {
        fprintf( stderr, "Couldn't create input thread \n" );
        goto fail;
    }
    pthread_setname_np(h->devices[0]->device_thread, "obe-device");

    h->is_active = 1;

    return 0;

fail:

    obe_close( h );

    return -1;
};

void obe_close( obe_t *h )
{
    void *ret_ptr;

    fprintf( stderr, "closing obe \n" );

    /* Cancel input thread */
    for( int i = 0; i < h->num_devices; i++ )
    {
        __pthread_cancel( h->devices[i]->device_thread );
        __pthread_join( h->devices[i]->device_thread, &ret_ptr );
    }

    fprintf( stderr, "input cancelled \n" );

    /* Cancel filter threads */
    for( int i = 0; i < h->num_filters; i++ )
    {
        pthread_mutex_lock( &h->filters[i]->queue.mutex );
        h->filters[i]->cancel_thread = 1;
        pthread_cond_signal( &h->filters[i]->queue.in_cv );
        pthread_mutex_unlock( &h->filters[i]->queue.mutex );
        __pthread_join( h->filters[i]->filter_thread, &ret_ptr );
    }

    fprintf( stderr, "filters cancelled \n" );

    /* Cancel encoder threads */
    for( int i = 0; i < h->num_encoders; i++ )
    {
        pthread_mutex_lock( &h->encoders[i]->queue.mutex );
        h->encoders[i]->cancel_thread = 1;
        pthread_cond_signal( &h->encoders[i]->queue.in_cv );
        pthread_mutex_unlock( &h->encoders[i]->queue.mutex );
        __pthread_join( h->encoders[i]->encoder_thread, &ret_ptr );
    }

    fprintf( stderr, "encoders cancelled \n" );

    /* Cancel encoder smoothing thread */
    if ( h->obe_system == OBE_SYSTEM_TYPE_GENERIC )
    {
        pthread_mutex_lock( &h->enc_smoothing_queue.mutex );
        h->cancel_enc_smoothing_thread = 1;
        pthread_cond_signal( &h->enc_smoothing_queue.in_cv );
        pthread_mutex_unlock( &h->enc_smoothing_queue.mutex );
        /* send a clock tick in case smoothing is waiting for one */
        pthread_mutex_lock( &h->obe_clock_mutex );
        pthread_cond_broadcast( &h->obe_clock_cv );
        pthread_mutex_unlock( &h->obe_clock_mutex );
        if ( h->enc_smoothing_thread )
            __pthread_join( h->enc_smoothing_thread, &ret_ptr );
    }

    fprintf( stderr, "encoder smoothing cancelled \n" );

    /* Cancel mux thread */
    pthread_mutex_lock( &h->mux_queue.mutex );
    h->cancel_mux_thread = 1;
    pthread_cond_signal( &h->mux_queue.in_cv );
    pthread_mutex_unlock( &h->mux_queue.mutex );
    __pthread_join( h->mux_thread, &ret_ptr );

    fprintf( stderr, "mux cancelled \n" );

    /* Cancel mux smoothing thread */
    pthread_mutex_lock( &h->mux_smoothing_queue.mutex );
    h->cancel_mux_smoothing_thread = 1;
    pthread_cond_signal( &h->mux_smoothing_queue.in_cv );
    pthread_mutex_unlock( &h->mux_smoothing_queue.mutex );
    __pthread_join( h->mux_smoothing_thread, &ret_ptr );

    fprintf( stderr, "mux smoothing cancelled \n" );

    /* Cancel output threads */
    for( int i = 0; i < h->num_outputs; i++ )
    {
        pthread_mutex_lock( &h->outputs[i]->queue.mutex );
        h->outputs[i]->cancel_thread = 1;
        pthread_cond_signal( &h->outputs[i]->queue.in_cv );
        pthread_mutex_unlock( &h->outputs[i]->queue.mutex );
        /* could be blocking on OS so have to cancel thread too */
        __pthread_cancel( h->outputs[i]->output_thread );
        __pthread_join( h->outputs[i]->output_thread, &ret_ptr );
    }

    fprintf( stderr, "output thread cancelled \n" );

    /* Destroy devices */
    for( int i = 0; i < h->num_devices; i++ )
        destroy_device( h->devices[i] );

    fprintf( stderr, "devices destroyed \n" );

    /* Destroy filters */
    for( int i = 0; i < h->num_filters; i++ )
        destroy_filter( h->filters[i] );

    fprintf( stderr, "filters destroyed \n" );

    /* Destroy encoders */
    for( int i = 0; i < h->num_encoders; i++ )
        destroy_encoder( h->encoders[i] );

    fprintf( stderr, "encoders destroyed \n" );

    destroy_enc_smoothing( &h->enc_smoothing_queue );
    fprintf( stderr, "encoder smoothing destroyed \n" );

    /* Destroy mux */
    destroy_mux( h );

    fprintf( stderr, "mux destroyed \n" );

    destroy_mux_smoothing( &h->mux_smoothing_queue );
    fprintf( stderr, "mux smoothing destroyed \n" );

    /* Destroy output */
    for( int i = 0; i < h->num_outputs; i++ )
        destroy_output( h->outputs[i] );

    free( h->outputs );

    fprintf( stderr, "output destroyed \n" );

    free(obe_core_get_output_stream_by_index(h, 0));
    /* TODO: free other things */

    /* Destroy lock manager */
    av_lockmgr_register( NULL );

    free( h );
    h = NULL;
}

void obe_raw_frame_printf(obe_raw_frame_t *rf)
{
    printf("raw_frame %p width = %d ", rf, rf->img.width);
    printf("height = %d ", rf->img.height);
    printf("csp = %d (%s) ", rf->img.csp, rf->img.csp == PIX_FMT_YUV422P10 ? "PIX_FMT_YUV422P10" : "PIX_FMT_YUV422");
    printf("planes[%d] = ", rf->img.planes);
    for (int i = 0; i < rf->img.planes; i++) {
        printf("%p ", rf->img.plane[i]);
    }
    printf("strides = ");
    for (int i = 0; i < rf->img.planes; i++) {
        printf("%d ", rf->img.stride[i]);
    }
    printf("\n");
}

#if 0
/* Return 1 if the images are byte for byte identical, else 0. */
int obe_image_compare(obe_image_t *a, obe_image_t *b)
{
	/* Its OK for the plane addresses not to match, but the
	 * plane contents (pixels) must match.... and the number
	 * of planes, strides and CSC must be identical.
	 */
	{
		obe_image_t x = *a;
		for (int i = 0; i < 4; i++)
			x.plane[i] = NULL;

		obe_image_t y = *b;
		for (int i = 0; i < 4; i++)
			y.plane[i] = NULL;

		if (memcmp(&x, &y, sizeof(y) != 0)) {
			printf("core object doesn't match\n");
			return 0;
		}
	}

	uint32_t plane_len[2][4] = { { 0 } };
	for (int j = 0; j < 2; j++) {
		obe_image_t *p = a;
		if (j == 1)
			p = b;

		for (int i = p->planes - 1; i > 0; i--) {
			plane_len[j][i - 1] = p->plane[i] - p->plane[i - 1];
		}
		if (p->planes == 3) {
			plane_len[j][2] = plane_len[j][1];
		}
	}

	for (int i = 0; i < a->planes; i++) {
		if (plane_len[0][i] != plane_len[1][i]) {
			printf("plane lengths don't match\n");
			return 0;
		}
	}

	/* Plane sizes match, now compare the planes themselves. */

	uint32_t alloc_size = 0;
	for (int i = 0; i < a->planes; i++)
		alloc_size += plane_len[0][i];

	if (memcmp(a->plane[0], b->plane[0], alloc_size) != 0) {
		printf("plane itself has changed\n");
		return 0;
	}

	return 1; /* Perfect image copy. */
}
#endif

void obe_image_save(obe_image_t *src)
{
	static int g_image_save_idx = 0;

	char fn[256];
	sprintf(fn, "video-frame-%08d.yuv", g_image_save_idx++);
	FILE *fh = fopen(fn, "wb");
	if (!fh)
		return;

	uint32_t plane_len[4] = { 0 };
	for (int i = src->planes - 1; i > 0; i--) {
		plane_len[i - 1] = src->plane[i] - src->plane[i - 1];
	}
	if (src->planes == 3) {
		plane_len[2] = plane_len[1];
	}

	for (int i = 0; i < src->planes; i++) {
		fwrite(src->plane[i], 1, plane_len[i], fh);
	}

	fclose(fh);
}

void obe_image_copy(obe_image_t *dst, obe_image_t *src)
{
	memcpy(dst, src, sizeof(obe_image_t));

	uint32_t plane_len[4] = { 0 };
	for (int i = src->planes - 1; i > 0; i--) {
		plane_len[i - 1] = src->plane[i] - src->plane[i - 1];
	}
	if (src->planes == 3) {
		plane_len[2] = plane_len[1];
	}

	uint32_t alloc_size = 0;
	for (int i = 0; i < src->planes; i++)
		alloc_size += plane_len[i];

	for (int i = 0; i < src->planes; i++) {

		if (i == 0 && src->plane[i]) {
			dst->plane[i] = (uint8_t *)malloc(alloc_size);
			memcpy(dst->plane[i], src->plane[i], alloc_size);
		}
		if (i > 0) {
			dst->plane[i] = dst->plane[i - 1] + plane_len[i - 1];
		}

	}
}

#if 0
void obe_raw_frame_free(obe_raw_frame_t *frame)
{
	free(frame->alloc_img.plane[0]);
	for (int i = 0; i < frame->num_user_data; i++)
		free(frame->user_data[i].data);
	free(frame->user_data);
	free(frame);
}
#endif

obe_raw_frame_t *obe_raw_frame_copy(obe_raw_frame_t *frame)
{
    obe_raw_frame_t *f = new_raw_frame();

    memcpy(f, frame, sizeof(*frame));

    obe_image_copy(&f->alloc_img, &frame->alloc_img);

    memcpy(&f->img, &f->alloc_img, sizeof(frame->alloc_img));

    if (f->num_user_data) {
        f->user_data = (obe_user_data_t *)malloc(sizeof(obe_user_data_t) * f->num_user_data);
        memcpy(f->user_data, frame->user_data, sizeof(obe_user_data_t) * f->num_user_data);

        for (int i = 0; i < f->num_user_data; i++) {
            f->user_data[i].data = (uint8_t *)malloc(frame->user_data[i].len);
            memcpy(f->user_data[i].data, frame->user_data[i].data, f->user_data[i].len);
        }

    } else {
        f->user_data = NULL;
    }

//    obe_raw_frame_printf(f);

    return f;
}

void obe_core_dump_output_stream(obe_output_stream_t *s, int index)
{
	const char *format_name = obe_core_get_format_name_short(s->stream_format);

	printf("%s(index = %d)\n", __func__, index);
	printf("  input_stream_id = %4d\n", s->input_stream_id);
	printf(" output_stream_id = %4d\n", s->output_stream_id);
	printf("    stream_action = %4d [%s]\n", s->stream_action,
		s->stream_action == STREAM_PASSTHROUGH ? "PASSTHROUGH" : "STREAM_ENCODE");
	printf("    stream_format = %4d [%s]\n", s->stream_format, format_name);
}

int obe_getTimestamp(char *s, time_t *when)
{
	struct tm tm;
	time_t now;

	if (when)
		now = *when;
	else
		time(&now);

	localtime_r(&now, &tm);

	sprintf(s, "%04d%02d%02d-%02d%02d%02d",
		tm.tm_year + 1900,
		tm.tm_mon  + 1,
		tm.tm_mday,
		tm.tm_hour,
		tm.tm_min,
		tm.tm_sec);

	return 0;
}
