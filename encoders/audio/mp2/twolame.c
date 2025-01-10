/*****************************************************************************
 * twolame.c : twolame encoding functions
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
 ******************************************************************************/

#include "common/common.h"
#include "encoders/audio/audio.h"
#include <twolame.h>
#include <libavutil/fifo.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>

//
struct historical_int64_s
{
	int64_t current;
	int64_t last;
	int64_t diff;
};

__inline__ void historical_int64_init(struct historical_int64_s *s) { memset(s, 0, sizeof(*s)); }
__inline__ void historical_int64_set(struct historical_int64_s *s, int64_t value) { s->last = s->current; s->current = value; s->diff = s->current - s->last; }
__inline__ int64_t historical_int64_get_diff(struct historical_int64_s *s) { return s->diff; }
__inline__ void historical_int64_printf(struct historical_int64_s *s, char *prefix)
{
	printf("%s: curr: %" PRIi64 " last: %" PRIi64 " diff: %" PRIi64 "\n",
		prefix, s->current, s->last, s->diff);
}
//

#define REPORT_AUDIO_DISCONTINUITIES 0
#define MP2_AUDIO_BUFFER_SIZE 50000

#define LOCAL_DEBUG 0

#define MODULE "[mp2]: "

int64_t mp2_offset_ms = 0;

static void *start_encoder_mp2( void *ptr )
{
#if LOCAL_DEBUG
    printf("%s()\n", __func__);
#endif

    obe_aud_enc_params_t *enc_params = ptr;
    obe_t *h = enc_params->h;
    obe_encoder_t *encoder = enc_params->encoder;
    obe_output_stream_t *stream = enc_params->stream;
    obe_raw_frame_t *raw_frame;
    obe_coded_frame_t *coded_frame;
    int64_t ptsfixup = 0;

    struct avfm_s avfm;
    int64_t cur_pts = -1, pts_increment = 0;


    /* Interesting 10.8.5 quirk related to the audio clock occasionally being ahead
     * of the video clock. This translates into 'too much' data in the audio fifo.
     * Because we previously used to put the same timestamp on multiple packets, or overly
     * adjust the PTS, things break.
     * The right approach is to accurately track the timestamp based on the head of the fifo,
     * so enable this feature by default.
     * If this is deemed risky, we could always enable it for 480i and 576i only..... but
     * I think we should go ahead, fully retest and just enable this better time calculation
     * mechanism.
     */
    enc_params->use_fifo_head_timing = 1;

    struct historical_int64_s rf_pts;
    historical_int64_init(&rf_pts);

    struct historical_int64_s avfm_pts;
    historical_int64_init(&avfm_pts);

    struct historical_int64_s cf_pts;
    historical_int64_init(&cf_pts);

#if LOCAL_DEBUG
    printf("%s() output_stream_id = %d\n", __func__, encoder->output_stream_id);
#endif

    twolame_options *tl_opts = NULL;
    int output_size, frame_size, linesize; /* Linesize in libavresample terminology is the entire buffer size for packed formats */
    float *audio_buf = NULL;
    uint8_t *output_buf = NULL;
    struct SwrContext *avr = NULL;
    AVFifoBuffer *fifo = NULL;

#if REPORT_AUDIO_DISCONTINUITIES
    int64_t lastOutputFramePTS = 0; /* Last pts we output, we'll comare against future version to warn for discontinuities. */
#endif

    /* 648000 is 24ms
     * lowest latency has frames_per_pes = 1
     */
    pts_increment = 648000 * enc_params->frames_per_pes; /* 24ms, the codec frame size * number of frames per pes. */

    /* Lock the mutex until we verify parameters */
    pthread_mutex_lock( &encoder->queue.mutex );

    tl_opts = twolame_init();
    if( !tl_opts )
    {
        fprintf( stderr, "[twolame] could not load options" );
        pthread_mutex_unlock( &encoder->queue.mutex );
        goto end;
    }

    /* TODO: setup bitrate reconfig, errors */
    twolame_set_bitrate( tl_opts, stream->bitrate );
    twolame_set_in_samplerate( tl_opts, enc_params->sample_rate );
    twolame_set_out_samplerate( tl_opts, enc_params->sample_rate );
    twolame_set_copyright( tl_opts, 1 );
    twolame_set_original( tl_opts, 1 );
    twolame_set_num_channels( tl_opts, av_get_channel_layout_nb_channels( stream->channel_layout ) );
    twolame_set_error_protection( tl_opts, 1 );
    if( stream->channel_layout == AV_CH_LAYOUT_STEREO )
        twolame_set_mode( tl_opts, stream->mp2_mode-1 );

    twolame_init_params( tl_opts );

    /* frame_size is 768 */
    frame_size = twolame_get_framelength( tl_opts ) * enc_params->frames_per_pes;

    encoder->is_ready = 1;
    /* Broadcast because input and muxer can be stuck waiting for encoder */
    pthread_cond_broadcast( &encoder->queue.in_cv );
    pthread_mutex_unlock( &encoder->queue.mutex );

    output_buf = malloc( MP2_AUDIO_BUFFER_SIZE );
    if( !output_buf )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    avr = swr_alloc();
    if (!avr)
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    av_opt_set_int( avr, "in_channel_layout",   stream->channel_layout,  0 );
    av_opt_set_int( avr, "in_sample_fmt",       enc_params->input_sample_format, 0 );
    av_opt_set_int( avr, "in_sample_rate",      enc_params->sample_rate, 0 );
    av_opt_set_int( avr, "out_channel_layout",  stream->channel_layout, 0 );
    av_opt_set_int( avr, "out_sample_fmt",      AV_SAMPLE_FMT_FLT,   0 );
    av_opt_set_int( avr, "dither_method",       SWR_DITHER_TRIANGULAR, 0 );
    av_opt_set_int( avr, "out_sample_rate",     enc_params->sample_rate, 0 );

    if (swr_init(avr) < 0)
    {
        fprintf( stderr, "Could not open AVResample\n" );
        goto end;
    }

    /* Setup the output FIFO */
    fifo = av_fifo_alloc( frame_size );
    if( !fifo )
    {
        fprintf( stderr, "Malloc failed\n" );
        goto end;
    }

    while( 1 )
    {
        pthread_mutex_lock( &encoder->queue.mutex );

        while( !encoder->queue.size && !encoder->cancel_thread )
            pthread_cond_wait( &encoder->queue.in_cv, &encoder->queue.mutex );

        if( encoder->cancel_thread )
        {
            pthread_mutex_unlock( &encoder->queue.mutex );
            break;
        }

        raw_frame = encoder->queue.queue[0];

#define DEKTEC 0
#if DEKTEC
        if (raw_frame->avfm.audio_pts - avfm.audio_pts >= (2 * 648000)) {
#else
	if (raw_frame->avfm.audio_pts - avfm.audio_pts >= (4 * 648000)) {
#endif
            cur_pts = -1; /* Reset the audio timebase from the hardware. */
#if DEKTEC
/* 720p30 seems to go through constant reset because the math exceeds our sense of normality.
 * I changed to be 4 * 648000 to keep the console debug noise down.
 */
printf("Reset audio because raw_frame->avfm.audio_pts %" PRIi64 " - avfm.audio_pts %" PRIi64 " >= (2 * 648000)\n",
raw_frame->avfm.audio_pts, avfm.audio_pts);
#endif

        }
        memcpy(&avfm, &raw_frame->avfm, sizeof(avfm));

        historical_int64_set(&avfm_pts, avfm.audio_pts);
        //historical_int64_printf(&avfm_pts, "avfm_pts");

        pthread_mutex_unlock( &encoder->queue.mutex );

        if (cur_pts == -1) {
            /* Drain any fifos and zero our processing latency, the clock has been
             * reset so we're rebasing time from the audio hardward clock.
             */
            cur_pts = avfm.audio_pts;
            ptsfixup = 0;

            printf(MODULE "strm %d audio pts reset to %" PRIi64 "\n",
                encoder->output_stream_id,
                cur_pts);

            /* Drain the conversion fifos else we induce drift. */
            av_fifo_drain(fifo, av_fifo_size(fifo));
            swr_drop_output(avr, 65535);

            output_size = twolame_encode_flush(tl_opts, output_buf, MP2_AUDIO_BUFFER_SIZE);
        }

        historical_int64_set(&rf_pts, raw_frame->pts);
        //historical_int64_printf(&rf_pts, "  rf_pts");

#if LOCAL_DEBUG
        /* Send any audio to the AC3 frame slicer.
         * Push the buffer starting at the channel containing bitstream, and span 2 channels,
         * we'll get called back with a completely aligned, crc'd and valid AC3 frame.
         */
        printf("%s() output_stream_id = %d linesize = %d, num_samples = %d, num_channels = %d, sample_fmt = %d, raw_frame->input_stream_id = %d\n",
                __func__,
                encoder->output_stream_id,
                raw_frame->audio_frame.linesize,
                raw_frame->audio_frame.num_samples,
                raw_frame->audio_frame.num_channels,
                raw_frame->audio_frame.sample_fmt,
                raw_frame->input_stream_id);
#endif

        /* Allocate the output buffer */
        if( av_samples_alloc( (uint8_t**)&audio_buf, &linesize, av_get_channel_layout_nb_channels( raw_frame->audio_frame.channel_layout ),
                              raw_frame->audio_frame.linesize, AV_SAMPLE_FMT_FLT, 0 ) < 0 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            goto end;
        }

        if (swr_convert(avr, (uint8_t **)&audio_buf, raw_frame->audio_frame.num_samples, (const uint8_t **)raw_frame->audio_frame.audio_data,
                                raw_frame->audio_frame.num_samples) < 0)
        {
            syslog( LOG_ERR, "[twolame] Sample format conversion failed\n" );
            break;
        }

        /* In low latency, this is 768.
         * Resulting PES should contain 768 data bytes.
         */
        output_size = twolame_encode_buffer_float32_interleaved( tl_opts, audio_buf, raw_frame->audio_frame.num_samples, output_buf, MP2_AUDIO_BUFFER_SIZE );

        if( output_size < 0 )
        {
            syslog( LOG_ERR, "[twolame] Encode failed\n" );
            break;
        }

        free( audio_buf );
        audio_buf = NULL;


        raw_frame->release_data( raw_frame );
        raw_frame->release_frame( raw_frame );
        remove_from_queue( &encoder->queue );

        if( av_fifo_realloc2( fifo, av_fifo_size( fifo ) + output_size ) < 0 )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            break;
        }

        av_fifo_generic_write( fifo, output_buf, output_size, NULL );

        while( av_fifo_size( fifo ) >= frame_size )
        {
            coded_frame = new_coded_frame( encoder->output_stream_id, frame_size );
            if( !coded_frame )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }
            av_fifo_generic_read( fifo, coded_frame->data, frame_size, NULL );
            memcpy(&coded_frame->avfm, &avfm, sizeof(avfm));

            /* 648000 27MHz ticks equates to 24ms.
             * 24ms is the smallest amount of time this audio codec and eject as compressed data.
             * Hence, lowest latency means we get 24ms PES frames.
             * Hence, normal latency is N * pes frames.
             * OBE itself determines what N should be in various latency modes.
             */
            coded_frame->pts = cur_pts;

            /* This code originated from the aac implementation in lavc.c
             * The comments related to how and why should be maintained there.
             * Highly specific to 29.97 for the time being.
             */
            if (ptsfixup == 0 &&
                avfm_get_hw_status_mask(&avfm, AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_HALF) &&
                avfm_get_video_interval_clk(&avfm) == 900900) {

                /* Fixup the clock for 10.11.2, due to an audio clocking bug. */
                int64_t drift = avfm_get_av_drift(&avfm);
                int64_t video_interval_clk = avfm_get_video_interval_clk(&avfm);
                int64_t drifted_frames = (drift / video_interval_clk);

                ptsfixup = drift % video_interval_clk;

                if (drifted_frames == 0) {
                    if (ptsfixup < -181000) {
                        drifted_frames++;
                    } else
                    if (ptsfixup > (video_interval_clk - 181000)) {
                        drifted_frames--;
                    }
                } else {
                    if (drift > 0) {
                        drifted_frames = 0;
                        if (ptsfixup > (video_interval_clk - 181000))
                            drifted_frames--;
                    } else {
                        drifted_frames *= -1;
                    }

                }
                ptsfixup += (drifted_frames * video_interval_clk);
            }

            coded_frame->pts += (ptsfixup * -1);

            /* Testing shows 720p59.94 is 16ms ahead of where it should be. */
            if (avfm_get_video_interval_clk(&avfm) == 450450) {
                coded_frame->pts -=  (16LL * 27000LL);
                coded_frame->pts -=  (67LL * 270LL);
            }
            coded_frame->pts +=  ((int64_t)stream->audio_offset_ms * 27000LL);
            coded_frame->pts +=  (mp2_offset_ms * 27000LL);
            coded_frame->random_access = 1; /* Every frame output is a random access point */
            coded_frame->type = CF_AUDIO;

            historical_int64_set(&cf_pts, coded_frame->pts);
            //historical_int64_printf(&cf_pts, "  cf_pts");

#if REPORT_AUDIO_DISCONTINUITIES
            if (lastOutputFramePTS + (648000 * enc_params->frames_per_pes) != coded_frame->pts) {
                printf(MODULE "Output PTS discontinuity\n\tShould be %" PRIi64 " was %" PRIi64 " diff %9" PRIi64 " frames_per_pes %d\n",
                    lastOutputFramePTS + (648000 * enc_params->frames_per_pes),
                    coded_frame->pts,
                    (lastOutputFramePTS + (648000 * enc_params->frames_per_pes)) - coded_frame->pts,
                    enc_params->frames_per_pes);
            }
            lastOutputFramePTS = coded_frame->pts;
#endif
            add_to_queue( &h->mux_queue, coded_frame );

            cur_pts += pts_increment;
        }
    }

end:
    if( output_buf )
        free( output_buf );

    if( audio_buf )
        free( audio_buf );

    if( avr )
        swr_free( &avr );

    if( fifo )
        av_fifo_free( fifo );

    if( tl_opts )
        twolame_close( &tl_opts );
    aud_enc_params_free(enc_params);

    return NULL;
}

const obe_aud_enc_func_t twolame_encoder = { start_encoder_mp2 };
