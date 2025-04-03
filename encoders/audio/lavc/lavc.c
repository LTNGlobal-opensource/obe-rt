/*****************************************************************************
 * lavc.c: libavcodec audio encoding functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 * Copyright (C) 2017-2019 LTN Global.
 *
 * Authors: Kieran Kunhya <kieran@kunhya.com>
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
#include "common/lavc.h"
#include "encoders/audio/audio.h"
#include <libavutil/fifo.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/audio_fifo.h>

#define MODULE "[lavc]: "
#define LOCAL_DEBUG 0

int g_audio_cf_debug = 0;

struct context_s
{
    obe_aud_enc_params_t *enc_params;
    obe_t *h;
    obe_encoder_t *encoder;
    obe_output_stream_t *stream;
    AVAudioFifo *audio_pcm_fifo;

    struct avfm_s avfm;
    int64_t cur_pts, pts_increment;
    int64_t ptsfixup;
    int64_t lastOutputFramePTS; /* Last pts we output, we'll comare against future version to warn for discontinuities. */
    int64_t frameLengthTicks; /* TODO: Isn't this the same as pts_internal, If yes, remove this */
    int encoderMode;

    int num_frames;
    int total_size_bytes;
#if AUDIO_DEBUG_ENABLE
    uint64_t cfQD;
#endif
};

typedef struct
{
    int obe_name;
    int lavc_name;
    char *display_name;
} lavc_encoder_t;

static const lavc_encoder_t lavc_encoders[] =
{
    { AUDIO_AC_3,   AV_CODEC_ID_AC3,  "AC_3" },
    { AUDIO_E_AC_3, AV_CODEC_ID_EAC3, "E_AC_3" },
    { AUDIO_AAC,    AV_CODEC_ID_AAC,  "AAC" },
    { -1, -1 },
};

/* Function has zero ownership over the lifetime of the packet, so
 * don't unref or free.
 */
static void processCodecOutput(struct context_s *ctx, AVPacket *pkt, AVFifoBuffer *fifo)
{
    obe_coded_frame_t *coded_frame;

#if LOCAL_DEBUG
    printf(MODULE "codec output %d bytes\n", pkt->size);
#endif

    if (av_fifo_realloc2(fifo, av_fifo_size(fifo) + pkt->size) < 0) {
        char msg[128];
        sprintf(msg, MODULE "malloc av_fifo_realloc2(?, %d + %d)\n", av_fifo_size(fifo), pkt->size);
        fprintf(stderr, "%s\n", msg);
        syslog(LOG_ERR, "%s", msg);
        return;
    }

    /* Write the output codec data into a fifo, because we want to make downstream packets
     * of exactly N frames (frames_per_pes).
     */
    av_fifo_generic_write(fifo, pkt->data, pkt->size, NULL);

    if (ctx->num_frames != ctx->enc_params->frames_per_pes) {
        /* Buffer enough codec content frames, to file the desired pes size. */
        return;
    }

    coded_frame = new_coded_frame(ctx->encoder->output_stream_id, ctx->total_size_bytes);
    if (!coded_frame)
        return;

    //printf("Fifo read of %d\n", ctx->total_size_bytes);
    av_fifo_generic_read(fifo, coded_frame->data, ctx->total_size_bytes, NULL);
    coded_frame->pts = ctx->cur_pts;

    /* Fixup the clock for 10.11.2, due to an audio clocking bug.
     * On startup of the sdk, the audio offset vs video offset differs a lot in
     * half-duplex mode, it doesn't vary in full duplex mode.
     * Additionally, cable pulls causes the audio offset to shift vs the video offset,
     * quite substantially.
     * Fixup the audio offset to compensate for all of this.
     * Currently this is all highly specific to 1080i29.97.
     */
    if (ctx->ptsfixup == 0 &&
        avfm_get_hw_status_mask(&ctx->avfm, AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_HALF) &&
        avfm_get_video_interval_clk(&ctx->avfm) == 900900) {

        int64_t drift = avfm_get_av_drift(&ctx->avfm);
        int64_t video_interval_clk = avfm_get_video_interval_clk(&ctx->avfm);
        int64_t drifted_frames = (drift / video_interval_clk);

        ctx->ptsfixup = drift % video_interval_clk;
        if (drifted_frames == 0) {
            if (ctx->ptsfixup < -181000) {
                drifted_frames++;
            } else
            if (ctx->ptsfixup > (video_interval_clk - 181000)) {
                drifted_frames--;
            }
        } else {
            if (drift > 0) {
                drifted_frames = 0;
                if (ctx->ptsfixup > (video_interval_clk - 181000))
                    drifted_frames--;
            } else {
                drifted_frames *= -1;
            }

        }
        ctx->ptsfixup += (drifted_frames * video_interval_clk);
    }

    /* We seem to be 33.2ms latent for 1080i, adjust it. Does this vary for low vs normal latency? */
    coded_frame->pts += (-33 * 27000LL);
    coded_frame->pts += (-2 * 2700LL);
    if (ctx->h->obe_system == OBE_SYSTEM_TYPE_GENERIC) {
        //coded_frame->pts += (8 * 2700LL);
    }
    if (ctx->encoderMode == AV_CODEC_ID_AC3) {
        coded_frame->pts += (38 * 27000LL);
    }

    coded_frame->pts += (ctx->ptsfixup * -1);
    coded_frame->pts += ((int64_t)ctx->stream->audio_offset_ms * 27000);
    coded_frame->random_access = 1; /* Every frame output is a random access point */
    coded_frame->type = CF_AUDIO;

    if (g_audio_cf_debug && ctx->encoder->output_stream_id == 1) {
        double interval = coded_frame->pts - ctx->lastOutputFramePTS;
        printf(MODULE "strm %d output pts %13" PRIi64 " size %6d bytes, pts-interval %6.0fticks /%6.2fms\n",
            ctx->encoder->output_stream_id,
            coded_frame->pts,
            ctx->total_size_bytes,
            interval,
            interval / 27000.0);
    }
    if (ctx->lastOutputFramePTS + (ctx->frameLengthTicks * ctx->enc_params->frames_per_pes) != coded_frame->pts) {
        if (ctx->encoder->output_stream_id == 1) {
            printf(MODULE "strm %d Output PTS discontinuity\n\tShould be %" PRIi64 " was %" PRIi64 " diff %9" PRIi64 " frames_per_pes %d\n",
                ctx->encoder->output_stream_id,
                ctx->lastOutputFramePTS + (ctx->frameLengthTicks * ctx->enc_params->frames_per_pes),
                coded_frame->pts,
                coded_frame->pts - (ctx->lastOutputFramePTS + (ctx->frameLengthTicks * ctx->enc_params->frames_per_pes)),
                ctx->enc_params->frames_per_pes);
        }
    }

    ctx->lastOutputFramePTS = coded_frame->pts;
#if AUDIO_DEBUG_ENABLE
    ctx->cfQD++;

        /* write post-resamples payload to disk for debug */
        if (g_audio_cf_debug & 0x80) {
            char fn[128];
            sprintf(fn, "/storage/ltn/stoth/audio-debug-cf---%02x-strm-%d-framenr-%012" PRIu64 ".raw",
                g_audio_cf_debug,
                ctx->encoder->output_stream_id,
                ctx->cfQD);
            printf("Creating %s\n", fn);
            FILE *fh = fopen(fn, "wb");
            if (fh) {
                coded_frame_serializer_write(fh, coded_frame);
                fclose(fh);
            }
        }
        if (g_audio_cf_debug & 0x80) {
            char fn[128];
            sprintf(fn, "/storage/ltn/stoth/audio-debug-comp-%02x-strm-%d-framenr-%012" PRIu64 ".raw",
                g_audio_cf_debug,
                ctx->encoder->output_stream_id,
                ctx->cfQD);
            printf("Creating %s\n", fn);
            FILE *fh = fopen(fn, "wb");
            if (fh) {
                fwrite(coded_frame->data, 1, coded_frame->len, fh);
                fclose(fh);
            }
        }
#endif

    add_to_queue(&ctx->h->mux_queue, coded_frame);

    ctx->cur_pts += ctx->pts_increment;
    ctx->total_size_bytes = ctx->num_frames = 0;
}

static void *aac_start_encoder(void *ptr)
{
    struct context_s *ctx = calloc(1, sizeof(*ctx));
    ctx->enc_params = ptr;
    ctx->h = ctx->enc_params->h;
    ctx->encoder = ctx->enc_params->encoder;
    ctx->cur_pts = -1;
    ctx->stream = ctx->enc_params->stream;
    ctx->frameLengthTicks = 576000;

    obe_raw_frame_t *raw_frame;
    int i, frame_size, ret;
    AVFifoBuffer *out_fifo_compressed = NULL;
    struct SwrContext *avr = NULL;
    AVPacket pkt;
    AVCodecContext *codec = NULL;
    AVFrame *frame = NULL;
    AVDictionary *opts = NULL;
    char is_latm[2];
    uint8_t *audio_planes[8] = { NULL };
    int dst_linesize = -1;
#if AUDIO_DEBUG_ENABLE
    uint64_t audioFramesDQ = 0;
#endif
    codec = avcodec_alloc_context3(NULL);
    if (!codec) {
        fprintf(stderr, MODULE "avcodec_alloc_context3 failed\n");
        goto finish;
    }

    for( i = 0; lavc_encoders[i].obe_name != -1; i++ )
    {
        if( lavc_encoders[i].obe_name == ctx->stream->stream_format ) {
            break;
        }
    }

    if( lavc_encoders[i].obe_name == -1 )
    {
        fprintf(stderr, MODULE "Could not find encoder1\n");
        goto finish;
    }

    printf(MODULE "Searching for audio encoder %s (id 0x%08x)\n",
        lavc_encoders[i].display_name,
        lavc_encoders[i].lavc_name);

    AVCodec *enc = NULL;
    if (lavc_encoders[i].lavc_name == AV_CODEC_ID_AC3) {
        ctx->frameLengthTicks = 864000; /* 32ms = 32 * 90 * 300 */
        enc = avcodec_find_encoder(lavc_encoders[i].lavc_name);
    } else
    if (lavc_encoders[i].lavc_name == AV_CODEC_ID_EAC3) {
        ctx->frameLengthTicks = 864000; /* 32ms = 32 * 90 * 300 */
        enc = avcodec_find_encoder(lavc_encoders[i].lavc_name);
    } else
    if (lavc_encoders[i].lavc_name == AV_CODEC_ID_AAC) {
        /* Bugfix: newer ffmpeg build are using the aac encoder, instead of
         * the preferred Fraunhoffer encoder. The regular aac encoder isn't
         * working, and we don't want to us it anyway. Specifically call
         * for the fraunhoffer codec.
         */
        ctx->frameLengthTicks = 576000;
        enc = avcodec_find_encoder_by_name("libfdk_aac");
    } else {
        printf(MODULE "Abnormal, no such audio encoder, aborting\n");
        exit(1);
    }
    if (!enc) {
        fprintf(stderr, MODULE "Could not find audio encoder\n");
        exit(1);
    }
    ctx->encoderMode = lavc_encoders[i].lavc_name;

    if( enc->sample_fmts[0] == -1 )
    {
        fprintf(stderr, MODULE "No valid sample formats\n");
        goto finish;
    }

    codec->sample_rate = ctx->enc_params->sample_rate;
    codec->bit_rate = ctx->stream->bitrate * 1000;
    codec->sample_fmt = enc->sample_fmts[0];
    printf(MODULE "codec->sample_fmt = %d\n", codec->sample_fmt);
    printf(MODULE "codec->bit_rate   = %" PRIi64 "\n", codec->bit_rate);
    printf(MODULE "long_name         = %s\n", enc->long_name);
    printf(MODULE "dialnorm          = %d\n", ctx->enc_params->dialnorm);
    printf(MODULE "compression_ctrl  = %s\n", ctx->enc_params->compression_gain >= 0 ? "enabled" : "disabled");
    printf(MODULE "compression_gain  = %d\n", ctx->enc_params->compression_gain);
    codec->channels = av_get_channel_layout_nb_channels(ctx->stream->channel_layout);
    codec->channel_layout = ctx->stream->channel_layout;
    codec->time_base.num = 1;
    codec->time_base.den = OBE_CLOCK;
    codec->profile = ctx->stream->aac_opts.aac_profile == AAC_HE_V2 ? FF_PROFILE_AAC_HE_V2 :
                     ctx->stream->aac_opts.aac_profile == AAC_HE_V1 ? FF_PROFILE_AAC_HE :
                     FF_PROFILE_AAC_LOW;

    snprintf( is_latm, sizeof(is_latm), "%i", ctx->stream->aac_opts.latm_output );
    av_dict_set( &opts, "latm", is_latm, 0 );
    av_dict_set( &opts, "header_period", "2", 0 );
    av_dict_set_int(&opts, "dialnorm", ctx->enc_params->dialnorm, 0);
    if (ctx->enc_params->compression_gain >= 0) {
        av_dict_set_int(&opts, "compression_gain", ctx->enc_params->compression_gain, 0);
    }

    if( avcodec_open2( codec, enc, &opts ) < 0 )
    {
        fprintf(stderr, MODULE "Could not open encoder\n");
        goto finish;
    }

    avr = swr_alloc();
    if (!avr) {
        fprintf(stderr, MODULE "swr_alloc() failed\n");
        goto finish;
    }

    av_opt_set_int( avr, "in_channel_layout",    codec->channel_layout, 0 );
    av_opt_set_int( avr, "in_sample_rate",       ctx->enc_params->sample_rate, 0 );
    av_opt_set_sample_fmt( avr, "in_sample_fmt", ctx->enc_params->input_sample_format, 0 );

    av_opt_set_int( avr, "out_channel_layout",    codec->channel_layout, 0 );
    av_opt_set_int( avr, "out_sample_rate",       ctx->enc_params->sample_rate, 0 );
    av_opt_set_sample_fmt( avr, "out_sample_fmt", codec->sample_fmt,     0 );
    av_opt_set_int( avr, "dither_method",         SWR_DITHER_TRIANGULAR, 0 );

    printf(MODULE "in_channel_layout  = %" PRIi64 "\n", codec->channel_layout);
    printf(MODULE "in_sample_fmt      = %d %s\n", ctx->enc_params->input_sample_format, av_get_sample_fmt_name(ctx->enc_params->input_sample_format));
    printf(MODULE "in_sample_rate     = %d\n", ctx->enc_params->sample_rate);
    printf(MODULE "out_channel_layout = %" PRIi64 "\n", codec->channel_layout);
    printf(MODULE "out_sample_fmt     = %d %s\n", codec->sample_fmt, av_get_sample_fmt_name(codec->sample_fmt));
    printf(MODULE "out_sample_rate    = %d\n", ctx->enc_params->sample_rate);

    if (swr_init(avr) < 0)
    {
        fprintf(stderr, MODULE "Could not open AVResample\n");
        goto finish;
    }

    /* The number of samples per E-AC3 frame is unknown until the encoder is ready */
    if (ctx->stream->stream_format == AUDIO_E_AC_3 || ctx->stream->stream_format == AUDIO_AAC)
    {
        pthread_mutex_lock(&ctx->encoder->queue.mutex);
        ctx->encoder->is_ready = 1;
        ctx->encoder->num_samples = codec->frame_size;
        /* Broadcast because input and muxer can be stuck waiting for encoder */
        pthread_cond_broadcast(&ctx->encoder->queue.in_cv);
        pthread_mutex_unlock(&ctx->encoder->queue.mutex);
    }

    frame_size = (double)codec->frame_size * 125 * ctx->stream->bitrate *
                 ctx->enc_params->frames_per_pes / ctx->enc_params->sample_rate;

    /* NB: libfdk-aac already doubles the frame size appropriately */
    ctx->pts_increment = (double)codec->frame_size * OBE_CLOCK * ctx->enc_params->frames_per_pes / ctx->enc_params->sample_rate;

    ctx->audio_pcm_fifo = av_audio_fifo_alloc(codec->sample_fmt, codec->channels, 8000);
    if (!ctx->audio_pcm_fifo) {
        fprintf(stderr, MODULE "audio fifo alloc failed\n");
        goto finish;
    }

    out_fifo_compressed = av_fifo_alloc(frame_size);
    if (!out_fifo_compressed) {
        fprintf(stderr, MODULE "compressed fifo alloc failed\n");
        goto finish;
    }

    frame = av_frame_alloc();
    if( !frame )
    {
        fprintf(stderr, MODULE "Could not allocate frame\n");
        goto finish;
    }

/* AAC has 1 frame per pes in lowest latency mode, frame size 1024. */
/* AAC has 6 frame per pes in normal latency mode, frame size 2048. */
    printf(MODULE "frames per pes     = %d\n", ctx->enc_params->frames_per_pes);
    printf(MODULE "dst_linesize       = %d\n", dst_linesize);
    printf(MODULE "codec->frame_size  = %d\n", codec->frame_size);
    printf(MODULE "codec->channels    = %d\n", codec->channels);
    printf(MODULE "codec->sample_fmt  = %d\n", codec->sample_fmt);
    printf(MODULE "bytes_per_sample   = %d\n", av_get_bytes_per_sample(codec->sample_fmt));
    printf(MODULE "is_planar          = %d\n", av_sample_fmt_is_planar(codec->sample_fmt));
    printf(MODULE "frameLengthTicks   = %" PRIi64 "\n", ctx->frameLengthTicks);
    printf(MODULE "pts_increment      = %" PRIi64 "\n", ctx->pts_increment);

    while (1)
    {
        /* TODO: detect bitrate or channel reconfig */
        pthread_mutex_lock(&ctx->encoder->queue.mutex);

        while (!ctx->encoder->queue.size && !ctx->encoder->cancel_thread)
            pthread_cond_wait(&ctx->encoder->queue.in_cv, &ctx->encoder->queue.mutex );

        if (ctx->encoder->cancel_thread)
        {
            pthread_mutex_unlock(&ctx->encoder->queue.mutex);
            goto finish;
        }

        raw_frame = ctx->encoder->queue.queue[0];
#if AUDIO_DEBUG_ENABLE
        audioFramesDQ++;
#endif
#if LOCAL_DEBUG
        if (ctx->encoder->output_stream_id == 1) {
            printf("\n");
            printf(MODULE "strm %d raw audio frame pts %" PRIi64 " linesize %d channels %d num_samples %d, pts delta %" PRIi64 "\n",
                ctx->encoder->output_stream_id,
                raw_frame->avfm.audio_pts,
                raw_frame->audio_frame.linesize,
                av_get_channel_layout_nb_channels(raw_frame->audio_frame.channel_layout),
                raw_frame->audio_frame.num_samples,
                raw_frame->avfm.audio_pts - ctx->avfm.audio_pts);
        }
#endif
        if (raw_frame->avfm.audio_pts - ctx->avfm.audio_pts >= (2 * ctx->frameLengthTicks)) {
            printf("Reset the cur_pts because of the hardware\n");
            printf("raw_frame->avfm.audio_pts %" PRIi64 "\n", raw_frame->avfm.audio_pts);
            printf("ctx->avfm.audio_pts %" PRIi64 "\n", ctx->avfm.audio_pts);
            printf("ctx->frameLengthTicks %" PRIi64 "\n", ctx->frameLengthTicks);
            printf("violation %" PRIi64 " >= %" PRIi64 "\n", raw_frame->avfm.audio_pts - ctx->avfm.audio_pts, 2 * ctx->frameLengthTicks);
            ctx->cur_pts = -1; /* Reset the audio timebase from the hardware. */
        }
        memcpy(&ctx->avfm, &raw_frame->avfm, sizeof(ctx->avfm));

        pthread_mutex_unlock(&ctx->encoder->queue.mutex);

        if (ctx->cur_pts == -1) {
            /* Drain any fifos and zero our processing latency, the clock has been
             * reset so we're rebasing time from the audio hardward clock.
             */
            ctx->cur_pts = ctx->avfm.audio_pts;
            ctx->ptsfixup = 0;

            printf(MODULE "strm %d audio pts reset to %" PRIi64 "\n",
                ctx->encoder->output_stream_id,
                ctx->cur_pts);

            /* Drain the conversion fifos else we induce drift. */
            av_fifo_drain(out_fifo_compressed, av_fifo_size(out_fifo_compressed));
            av_audio_fifo_drain(ctx->audio_pcm_fifo, av_audio_fifo_size(ctx->audio_pcm_fifo));
            swr_drop_output(avr, 65535);
            ctx->num_frames = 0;
            ctx->total_size_bytes = 0;
        }

        /* Create some data planes that we'll fill with raw input audio samples.
         * We'll pass these planes into the audio format convertor.
         * We'll re-use these planes later again, when reading converted audio
         * samples so they need to be capable of holding codec->frame_size samples.
         * For AAC, frame_size is 1000, for AC3 its 1536. This number is given to us
         * by libavcodec, and represents the minimum number of samples we must
         * pass to the compression codec.
         * The slower the framerate, the higher the linesize is the more audio at 48Khz,
         * is associated with a frame.
         *
         *                   raw_frame->audio_frame.linesize   codec->frame_size   Codec  Card
         *  1080i29.97                                  6528                1024     AAC  duo2
         *  1080i29.97                                  6528                1536     AC3  duo2
         *   720p59.94                                  3328                1024     AAC  duo2
         *   720p59.94                                  3328                1536     AC3  duo2
         *  1080p24                                     8086                1024     AAC  duo2
         *  1080p24                                     8086                1536     AC3  duo2
         *  1080p30                                     1024                1536     AC3  vega
         *  1080p30                                     1024                1024     AAC  vega
         *  1080p59.94                                  1024                1536     AC3  vega
         *  1080p59.94                                  1024                1024     AAC  vega
         * 
         * Instead of using raw_frame->audio_frame.linesize or codec->frame_size, lets
         * create a massive buffer to handle very small and very large cases in a single size.
         */
        int newlinesize = 8086 * 4;
	    if (raw_frame->audio_frame.linesize > newlinesize) {
		    newlinesize = raw_frame->audio_frame.linesize * 4;
	    }
        if (av_samples_alloc(audio_planes, NULL,
                av_get_channel_layout_nb_channels(raw_frame->audio_frame.channel_layout),
                newlinesize, codec->sample_fmt, 0) < 0) {
            fprintf(stderr, MODULE "Could not allocate audio samples\n");
            goto finish;
        }

        int64_t swrdelay = swr_get_delay(avr, 1000);
        if (swrdelay != 0) {
           printf(MODULE "SWR delay %" PRIi64 ", warning!\n", swrdelay);
        }

//printf("swr_convert(avr, planes, %d, data, %d)\n", raw_frame->audio_frame.num_samples, raw_frame->audio_frame.num_samples);
#if AUDIO_DEBUG_ENABLE
        /* write pre-resample payload to disk for debug */
        if (g_audio_cf_debug & 0x80) {
            char fn[128];
            sprintf(fn, "/storage/ltn/stoth/audio-debug-pre--%02x-strm-%d-framenr-%012" PRIu64 "-ch%d-samples%d.raw",
                g_audio_cf_debug,
                ctx->encoder->output_stream_id,
                audioFramesDQ,
                av_get_channel_layout_nb_channels(raw_frame->audio_frame.channel_layout),
                raw_frame->audio_frame.num_samples);
            printf("Creating %s\n", fn);
            FILE *fh = fopen(fn, "wb");
            if (fh) {
                for (int i = 0; i < 8; i++) {
                    if (audio_planes[i]) {
                        fwrite(raw_frame->audio_frame.audio_data[i], 4, raw_frame->audio_frame.num_samples, fh);
                    }
                }
                fclose(fh);
            }
        }
#endif
        int count;
        count = swr_convert(avr,
                (uint8_t **)audio_planes,
                raw_frame->audio_frame.num_samples,
                (const uint8_t **)raw_frame->audio_frame.audio_data,
                raw_frame->audio_frame.num_samples);
        if (count < 0)
        {
            fprintf(stderr, MODULE "Sample format conversion failed\n");
            syslog(LOG_ERR, MODULE "Sample format conversion failed\n");
            break;
        }

#if AUDIO_DEBUG_ENABLE
        /* write post-resamples payload to disk for debug */
        if (g_audio_cf_debug & 0x80) {
            char fn[128];
            sprintf(fn, "/storage/ltn/stoth/audio-debug-post-%02x-strm-%d-framenr-%012" PRIu64 "-ch%d-samples%d.raw",
                g_audio_cf_debug,
                ctx->encoder->output_stream_id,
                audioFramesDQ,
                av_get_channel_layout_nb_channels(raw_frame->audio_frame.channel_layout),
                raw_frame->audio_frame.num_samples);
            printf("Creating %s\n", fn);
            FILE *fh = fopen(fn, "wb");
            if (fh) {
                for (int i = 0; i < 8; i++) {
                    if (audio_planes[i]) {
                        fwrite(audio_planes[i], 4, raw_frame->audio_frame.num_samples, fh);
                    }
                }
                fclose(fh);
            }
        }
#endif

        /* Push the converted samples into the audio pcm fifo. */
        ret = av_audio_fifo_write(ctx->audio_pcm_fifo, (void **)audio_planes, raw_frame->audio_frame.num_samples);
        if (ret != raw_frame->audio_frame.num_samples) {
            fprintf(stderr, MODULE "Unable to write to audio fifo, ret = %d - should be %d\n", ret, raw_frame->audio_frame.num_samples);
            exit(1);
        }

        raw_frame->release_data(raw_frame);
        raw_frame->release_frame(raw_frame);
        remove_from_queue(&ctx->encoder->queue);

	/* Number of samples less than required codec samples reqd? */
        while (av_audio_fifo_size(ctx->audio_pcm_fifo) >= codec->frame_size) {

            /* Construct the AVFrame then compress it. */
            frame->nb_samples = codec->frame_size;

            /* Read sample pointers from the AVR convert into AV frame pointer area */
            memcpy(frame->data, audio_planes, sizeof(frame->data));
            int sr = av_audio_fifo_read(ctx->audio_pcm_fifo, (void **)frame->data, frame->nb_samples);
            if (sr != codec->frame_size) {
                fprintf(stderr, MODULE "Error reading from fifo\n");
		exit(1);
            }

            av_init_packet(&pkt);
            pkt.data = NULL;
            pkt.size = 0;

            ret = avcodec_send_frame(codec, frame);
            if (ret < 0) {
                fprintf(stderr, MODULE "avcodec_send_frame failed %d\n", ret);
                /* Now what? */
                exit(1);
            }

            /* Read any available frames and output them as obe coded_frames. */
            while (ret >= 0) {
               ret = avcodec_receive_packet(codec, &pkt);
               if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                   continue;
               else if (ret < 0) {
                   fprintf(stderr, MODULE "Error compressing audio frame\n");
                   exit(1);
               }

               /* Process the compressed audio */
               //printf(MODULE "pkt.size = %d\n", pkt.size);
               ctx->total_size_bytes += pkt.size;
               ctx->num_frames++;

               processCodecOutput(ctx, &pkt, out_fifo_compressed);
               av_packet_unref(&pkt);
            }
        }

        /* Compression done, cleanup. */
        av_freep(&audio_planes[0]);
        audio_planes[0] = NULL;

    } /* While 1 */

finish:
    if( frame )
       av_frame_free( &frame );

    if( audio_planes[0] )
        av_free( audio_planes[0] );

    if (ctx->audio_pcm_fifo)
        av_audio_fifo_free(ctx->audio_pcm_fifo);

    if( out_fifo_compressed )
        av_fifo_free( out_fifo_compressed );

    if (avr)
        swr_free(&avr);

    if( codec )
    {
        avcodec_close( codec );
        av_free( codec );
    }

    aud_enc_params_free(ctx->enc_params);

    return NULL;
}

const obe_aud_enc_func_t lavc_encoder = { aac_start_encoder };
