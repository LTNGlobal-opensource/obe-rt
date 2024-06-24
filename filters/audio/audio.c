/*****************************************************************************
 * audio.c: basic audio filtering system
 *****************************************************************************
 * Copyright (C) 2012 Open Broadcast Systems Ltd
 *
 * The entire purpose of this filter is to take in (typically) the entire 16
 * channels of SDI audio, then create multiple new outputs, one for each audio encoder,
 * with the correct audio channels present.
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
 */

#include <libavutil/eval.h>

#include "common/common.h"
#include "audio.h"

#define LOCAL_DEBUG 0
#define MODULE_PREFIX "[audio-filter]: "

/* Bitmask:
 * 0 = mute right
 * 1 = mute left
 * 2 = static right
 * 3 = static left
 * 4 = buzz right
 * 5 = buzz left
 * 6 = attenuate right
 * 7 = attenuate left
 * 8 = clip right
 * 9 = clip left
 */
int g_filter_audio_effect_pcm = 0;

static void obe_aud_filter_mute_samples(obe_output_stream_t *output_stream, obe_raw_frame_t *rf)
{
    for (int i = 0; i < output_stream->audio_mute_count; i++) {
        if (output_stream->audio_mute_table[i].enabled) {
            int i_src = output_stream->audio_mute_table[i].mute;
            memset(rf->audio_frame.audio_data[i_src - 1], 0, rf->audio_frame.num_samples * 4);
        } else {
            /* Finish early, because the mute rules are added top to bottom. */
            return;
        }
    }
}

/* Take a raw frame will all of the SDI samples 16 channels.
 * Use the remap rule table to re-assign the samples into new channels
 * Copy those channels by running through the table and copying src to dst samples.
 */
static obe_raw_frame_t *obe_aud_filter_remap_samples(obe_output_stream_t *output_stream, obe_raw_frame_t *rf)
{
#if LOCAL_DEBUG
    printf("audio linesize %d, sfc %d, rf->audio_frame.num_samples, chan %d\n",
        rf->audio_frame.linesize,
        rf->audio_frame.num_samples,
        rf->audio_frame.num_channels);
#endif

    obe_raw_frame_t *nrf = new_raw_frame();
    memcpy(nrf, rf, sizeof(*rf));

    int l = nrf->audio_frame.num_channels * nrf->audio_frame.num_samples * 4;
    nrf->audio_frame.audio_data[0] = (uint8_t *)malloc(l);

    memcpy(nrf->audio_frame.audio_data[0], rf->audio_frame.audio_data[0], l);

    for (int i = 1; i < MAX_CHANNELS; i++) {
        nrf->audio_frame.audio_data[i] = nrf->audio_frame.audio_data[0] + (i * nrf->audio_frame.linesize);
    }

#if 0
    for (int i = 0; i < MAX_CHANNELS; i++) {
        printf("CH%02d: %p becomes %p\n", i, rf->audio_frame.audio_data[i], nrf->audio_frame.audio_data[i]);
    }
#endif

    for (int i = 0; i < output_stream->audio_remap_count; i++) {
        if (output_stream->audio_remap_table[i].enabled) {
            int src = output_stream->audio_remap_table[i].src;
            int dst = output_stream->audio_remap_table[i].dst;

            /* Copy the entire plan from src to dst. */
            memcpy(nrf->audio_frame.audio_data[dst - 1], nrf->audio_frame.audio_data[src -1], nrf->audio_frame.num_samples * 4);

            /* Or, instead of memcpy, we can tamper with the plane pointers, easier */
        }
    }

    return nrf;
}

static int obe_aud_filter_remapping_reset(obe_output_stream_t *output_stream)
{
    memset(&output_stream->audio_remap_table[0], 0, sizeof(output_stream->audio_remap_table));
    return 0;
}

static void obe_aud_filter_remapping_dump(obe_output_stream_t *output_stream)
{
    for (int i = 0; i < AUDIO_REMAP_RULES_MAX; i++) {
        if (output_stream->audio_remap_table[i].enabled == 1) {
            printf(MODULE_PREFIX "active remap rule[%02d]: %2d to %2d, output_stream_id %d\n", i,
                output_stream->audio_remap_table[i].src,
                output_stream->audio_remap_table[i].dst,
                output_stream->output_stream_id);
        }
    }
}

static int obe_aud_filter_remapping_rule_add(obe_output_stream_t *output_stream, unsigned int src, unsigned int dst)
{
    for (int i = 0; i < AUDIO_REMAP_RULES_MAX; i++) {

        if (output_stream->audio_remap_table[i].enabled == 0) {

            output_stream->audio_remap_table[i].enabled = 1;
            output_stream->audio_remap_table[i].src = src;
            output_stream->audio_remap_table[i].dst = dst;
#if LOCAL_DEBUG
            printf(MODULE_PREFIX "adding remap rule[%02d]: %2d to %2d, output_stream_id %d\n",
                output_stream->audio_remap_count, src, dst,
                output_stream->output_stream_id);
#endif
            output_stream->audio_remap_count++;

            break;
        }

    }

    return 0;
}

static int audio_remapping_configure(obe_output_stream_t *output_stream)
{
    /* Convert a static string passed by the configuration into a baseic set of transform rules
     * <1..16:1..16>
     * from:to
     * 
     * Don't allow remapping from the sample channel to the same channel, Eg: 3:3
     * Don't allow numbers < 1 or > 16
     * Allow a maximum number of rules.
     * Don't allow badly formed rules
     */

    obe_aud_filter_remapping_reset(output_stream);

    int rule_count = 0;
    char *save = NULL;
    char tmp[256] = { 0 };
    strcpy(tmp, output_stream->audio_remap);

    char *rule = strtok_r(tmp, "-", &save);
    while (rule) {
        char *save2 = NULL;
        char *map = strdup(rule);

        int ignore = 0;
        int i_src, i_dst;

        char *src = strtok_r(map, "_", &save2);
        if (src == NULL) {
            fprintf(stderr, MODULE_PREFIX "remap rule: %s -- illegal - ignoring\n", rule);
            ignore++;
        } else {
            i_src = atoi(src);
            if (i_src < 1 || i_src > 16) {
                fprintf(stderr, MODULE_PREFIX "remap rule: %2d to NN -- illegal - ignoring\n", i_src);
                ignore++;
            }
        }

        char *dst = strtok_r(NULL, "_", &save2);
        if (dst == NULL) {
            fprintf(stderr, MODULE_PREFIX "remap rule: %s -- illegal - ignoring\n", rule);
            ignore++;
        } else {
            i_dst = atoi(dst);
            if (i_dst < 1 || i_dst > 16) {
                fprintf(stderr, MODULE_PREFIX "remap rule: %2d to NN -- illegal - ignoring\n", i_dst);
                ignore++;
            }
        }

        if (ignore == 0) {
            obe_aud_filter_remapping_rule_add(output_stream, i_src, i_dst);
        }

        free(map);
        rule = strtok_r(NULL, "-", &save);
    };

    return rule_count;
}

static int audio_mute_configure(obe_output_stream_t *output_stream)
{
    int rule_count = 0;
    char *save = NULL;
    char tmp[256] = { 0 };
    strcpy(tmp, output_stream->audio_mute);

    char *rule = strtok_r(tmp, "_", &save);
    while (rule) {
        int i_src = atoi(rule);
        int ignore = 0;

        if (i_src < 1 || i_src > 6) {
            fprintf(stderr, MODULE_PREFIX "mute rule: %2d to NN -- illegal - ignoring\n", i_src);
            ignore++;
        }

        if (ignore == 0) {
            for (int i = 0; i < AUDIO_MUTE_RULES_MAX; i++) {
                if (output_stream->audio_mute_table[i].enabled == 0) {
                    output_stream->audio_mute_table[i].enabled = 1;
                    output_stream->audio_mute_table[i].mute = i_src;
                    output_stream->audio_mute_count++;
                }
            }
        }
        
        rule = strtok_r(NULL, "_", &save);
    };

    return rule_count;
}

static double compute_dB__to_scaler(const char *dbval)
{
    static const char *const var_names[] = {
        "volume",              ///< last set value
        NULL
    };

    double var_values[4] = { 0 };

    AVExpr *pexpr = NULL;
    const char *expr = dbval;

    int ret = av_expr_parse(&pexpr, expr, var_names, NULL, NULL, NULL, NULL, 0, NULL);
    if (ret < 0) {
        fprintf(stderr, "error evaluating volume expression\n");
    }

    double volume = 1.0;

    if (pexpr) {

        volume = av_expr_eval(pexpr, &var_values[0], NULL);
        //printf("volume = %f\n", volume);

        free(pexpr);
        pexpr = NULL;
    }

    return volume;
}

/* Works for any number of channels and samples, but assumes S32P.
 */
static void applyGain(obe_output_stream_t *output_stream, obe_raw_frame_t *rf, double volumeScaler)
{
    /* Gain adjust audio gain for left and right channels - assumption 32bit samples S32P from decklink */
    int32_t *data = (int32_t *)rf->audio_frame.audio_data[0];

#if 0
    printf("output_stream_id %d, applying gain of %f\n", output_stream->output_stream_id, volumeScaler);
#endif

    for (int i = 0; i < rf->audio_frame.num_samples * rf->audio_frame.num_channels; i++) {
        double a = (double)data[i];
        double b = a * volumeScaler;
        data[i] = (int32_t)b;
    }
}

static void applyEffects(obe_output_stream_t *output_stream, obe_raw_frame_t *rf)
{
    if (g_filter_audio_effect_pcm & 0x03) {
        /* Mute audio right (or left or both) - assumption 32bit samples S32P from decklink */
        uint32_t *l = (uint32_t *)rf->audio_frame.audio_data[0];
        uint32_t *r = (uint32_t *)rf->audio_frame.audio_data[1];
        for (int i = 0; i < rf->audio_frame.num_samples; i++) {
            if (g_filter_audio_effect_pcm & (1 << 0)) {
                *(r++) = 0; /* Mute Right */
            }
            if (g_filter_audio_effect_pcm & (1 << 1)) {
                *(l++) = 0; /* Mute Left */
            }
        }
    }
    if (g_filter_audio_effect_pcm & 0x0c) {
        /* Static audio right (or left or both) - assumption 32bit samples S32P from decklink */
        int32_t *l = (int32_t *)rf->audio_frame.audio_data[0];
        int32_t *r = (int32_t *)rf->audio_frame.audio_data[1];
        for (int i = 0; i < rf->audio_frame.num_samples; i++) {
            if (g_filter_audio_effect_pcm & (1 << 2)) {
                *(r++) = rand(); /* Right */
            }
            if (g_filter_audio_effect_pcm & (1 << 3)) {
                *(l++) = rand(); /* Left */
            }
        }
    }
    if (g_filter_audio_effect_pcm & 0x30) {
        /* Buzz audio right (or left or both) - assumption 32bit samples S32P from decklink */
        int32_t *l = (int32_t *)rf->audio_frame.audio_data[0];
        int32_t *r = (int32_t *)rf->audio_frame.audio_data[1];
        for (int i = 0; i < rf->audio_frame.num_samples / 16; i++) {
            if (g_filter_audio_effect_pcm & (1 << 4)) {
                *(r++) = -200000000; /* Right */
                *(r++) = -200000000; /* Right */
                *(r++) = -200000000; /* Right */
                *(r++) = -200000000; /* Right */
                r += 12;
            }
            if (g_filter_audio_effect_pcm & (1 << 5)) {
                *(l++) = -200000000; /* left */
                *(l++) = -200000000; /* left */
                *(l++) = -200000000; /* left */
                *(l++) = -200000000; /* left */
                l += 12;
            }
        }
    }
    if (g_filter_audio_effect_pcm & 0xf0) {
        /* attenuate audio right (or left or both) - assumption 32bit samples S32P from decklink */
        int32_t *l = (int32_t *)rf->audio_frame.audio_data[0];
        int32_t *r = (int32_t *)rf->audio_frame.audio_data[1];
        for (int i = 0; i < rf->audio_frame.num_samples; i++) {
            if (g_filter_audio_effect_pcm & (1 << 6)) {
                *r /= 4; /* Right */
                 r++;
            }
            if (g_filter_audio_effect_pcm & (1 << 7)) {
                *l /= 4; /* Right */
                 l++;
            }
        }
    }
    if (g_filter_audio_effect_pcm & 0x300) {
        /* amplify and clip audio right (or left or both) - assumption 32bit samples S32P from decklink */
        int32_t *l = (int32_t *)rf->audio_frame.audio_data[0];
        int32_t *r = (int32_t *)rf->audio_frame.audio_data[1];
        for (int i = 0; i < rf->audio_frame.num_samples; i++) {
            if (g_filter_audio_effect_pcm & (1 << 8)) {
                *r *= 8; /* Right */
                 r++;
            }
            if (g_filter_audio_effect_pcm & (1 << 9)) {
                *l *= 8; /* left */
                 l++;
            }
        }
    }
}

static void *start_filter_audio( void *ptr )
{
    obe_raw_frame_t *raw_frame, *split_raw_frame;
    obe_aud_filter_params_t *filter_params = ptr;
    obe_t *h = filter_params->h;
    obe_filter_t *filter = filter_params->filter;
    obe_output_stream_t *output_stream;
    int num_channels;

    /* ignore the video track, process all PCM encoders first */
    for (int i = 1; i < h->num_encoders; i++)
    {
        output_stream = get_output_stream_by_id(h, h->encoders[i]->output_stream_id);
        if (output_stream->stream_format == AUDIO_AC_3_BITSTREAM)
            continue; /* Ignore downstream AC3 bitstream encoders */

        num_channels = av_get_channel_layout_nb_channels(output_stream->channel_layout);
        output_stream->audioGain = 0.0;

        if ((num_channels == 2 || num_channels == 6) && strlen(output_stream->gain_db) > 0) {
            output_stream->audioGain = compute_dB__to_scaler(output_stream->gain_db);
            printf(MODULE_PREFIX "pid %d, output_stream_id %d, applying audio gain of %f, num_encoders %d, num_channels %d\n",
                getpid(), output_stream->output_stream_id, output_stream->audioGain, h->num_encoders,
                num_channels);
        } else
        if (strlen(output_stream->gain_db) > 0) {
            printf(MODULE_PREFIX "pid %d, output_stream_id %d, num_channels %d, ignoring gain request\n",
                getpid(), output_stream->output_stream_id, num_channels);
        }

        audio_remapping_configure(output_stream);
        audio_mute_configure(output_stream);
        obe_aud_filter_remapping_dump(output_stream);
    }

    while( 1 )
    {
        pthread_mutex_lock( &filter->queue.mutex );

        while( !filter->queue.size && !filter->cancel_thread )
            pthread_cond_wait( &filter->queue.in_cv, &filter->queue.mutex );

        if( filter->cancel_thread )
        {
            pthread_mutex_unlock( &filter->queue.mutex );
            break;
        }

        raw_frame = filter->queue.queue[0];
        pthread_mutex_unlock( &filter->queue.mutex );

#if LOCAL_DEBUG
        printf("%s() raw_frame->input_stream_id = %d, num_encoders = %d\n", __func__,
            raw_frame->input_stream_id, h->num_encoders);
        printf("%s() linesize = %d, num_samples = %d, num_channels = %d, sample_fmt = %d\n",
            __func__,
            raw_frame->audio_frame.linesize,
            raw_frame->audio_frame.num_samples, raw_frame->audio_frame.num_channels,
            raw_frame->audio_frame.sample_fmt);
#endif

        /* ignore the video track, process all PCM encoders first */
        for (int i = 1; i < h->num_encoders; i++)
        {
            output_stream = get_output_stream_by_id(h, h->encoders[i]->output_stream_id);
            if (output_stream->stream_format == AUDIO_AC_3_BITSTREAM)
                continue; /* Ignore downstream AC3 bitstream encoders */

            if (raw_frame->audio_frame.sample_fmt == AV_SAMPLE_FMT_NONE)
                continue; /* Ignore non-pcm frames */

//printf("output_stream->stream_format = %d other\n", output_stream->stream_format);
            num_channels = av_get_channel_layout_nb_channels( output_stream->channel_layout );

#if LOCAL_DEBUG
            printf("%s() encoder#%d: output_stream->sdi_audio_pair %d, num_channels %d\n", __func__, i,
                output_stream->sdi_audio_pair, num_channels);
#endif

            split_raw_frame = new_raw_frame();
            if (!split_raw_frame)
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                return NULL;
            }
            memcpy(split_raw_frame, raw_frame, sizeof(*split_raw_frame));
            memset(split_raw_frame->audio_frame.audio_data, 0, sizeof(split_raw_frame->audio_frame.audio_data));
            split_raw_frame->audio_frame.linesize = split_raw_frame->audio_frame.num_channels = 0;
            split_raw_frame->audio_frame.channel_layout = output_stream->channel_layout;
            split_raw_frame->audio_frame.num_channels = num_channels;

            if (av_samples_alloc(split_raw_frame->audio_frame.audio_data, &split_raw_frame->audio_frame.linesize, num_channels,
                              split_raw_frame->audio_frame.num_samples, split_raw_frame->audio_frame.sample_fmt, 0) < 0)
            {
                syslog(LOG_ERR, "Malloc failed\n");
                return NULL;
            }

#if LOCAL_DEBUG
printf("%s() output_stream->mono_channel %d", __func__, output_stream->mono_channel);
printf(" num_channels %d", num_channels);
printf(" split_raw_frame->audio_frame.num_samples %d", split_raw_frame->audio_frame.num_samples);
printf(" split_raw_frame->audio_frame.sample_fmt %d", split_raw_frame->audio_frame.sample_fmt);
printf(" split_raw_frame->audio_frame.linesize %d", split_raw_frame->audio_frame.linesize);
#endif


            if (output_stream->audio_remap_count > 0) {
                /* Audio Remapping done here.
                 * Duplicate the original raw. Adjust it, the copy ot into the split_raw_frame object.
                 * Free out newly created raw frame (and it's duplicated samples)
                 */
                obe_raw_frame_t *rf = obe_aud_filter_remap_samples(output_stream, raw_frame);

                /* Copy samples for each channel into a new buffer, so each downstream encoder can
                 * compress the channels the user has selected via sdi_audio_pair.
                 */
                av_samples_copy(split_raw_frame->audio_frame.audio_data, /* dst */
                                &rf->audio_frame.audio_data[((output_stream->sdi_audio_pair - 1) << 1) + output_stream->mono_channel], /* src */
                                0, /* dst offset */
                                0, /* src offset */
                                split_raw_frame->audio_frame.num_samples,
                                num_channels,
                                split_raw_frame->audio_frame.sample_fmt);
                free(rf->audio_frame.audio_data[0]);
                free(rf);
            } else {
                /* No audio remapping - the default typical use case */
                /* Copy samples for each channel into a new buffer, so each downstream encoder can
                 * compress the channels the user has selected via sdi_audio_pair.
                 */
                av_samples_copy(split_raw_frame->audio_frame.audio_data, /* dst */
                                &raw_frame->audio_frame.audio_data[((output_stream->sdi_audio_pair - 1) << 1) + output_stream->mono_channel], /* src */
                                0, /* dst offset */
                                0, /* src offset */
                                split_raw_frame->audio_frame.num_samples,
                                num_channels,
                                split_raw_frame->audio_frame.sample_fmt);
            }

            /* Apply muting here */

            /* Audio Effects */
            applyEffects(output_stream, split_raw_frame);

            if ((num_channels == 2 || num_channels == 6) && strlen(output_stream->gain_db) > 0) {
                applyGain(output_stream, split_raw_frame, output_stream->audioGain);
            }

            if (output_stream->audio_mute_count > 0) {
                obe_aud_filter_mute_samples(output_stream, split_raw_frame);
            }

#if LOCAL_DEBUG
            obe_raw_frame_printf(split_raw_frame);
#endif
            add_to_encode_queue(h, split_raw_frame, h->encoders[i]->output_stream_id);
        } /* For all PCM encoders */

        /* ignore the video track, process all AC3 bitstream encoders.... */
	/* TODO: Only one buffer can be passed to one encoder, as the input SDI
	 * group defines a single stream of data, so this buffer can only end up at one
	 * ac3bitstream encoder.
	 * That being said, the decklink input creates one bitstream buffer per detected pair.
	 */
        int didForward = 0;
        for (int i = 1; i < h->num_encoders; i++)
        {
            output_stream = get_output_stream_by_id(h, h->encoders[i]->output_stream_id);
            if (output_stream->stream_format != AUDIO_AC_3_BITSTREAM)
                continue; /* Ignore downstream AC3 bitstream encoders */

            if (raw_frame->audio_frame.sample_fmt != AV_SAMPLE_FMT_NONE)
                continue; /* Ignore pcm frames */

#if 0
            obe_int_input_stream_t *input_stream = get_input_stream(h, output_stream->input_stream_id);
            printf("raw_frame->input_stream_id %d != h->encoders[i]->output_stream_id %d\n", raw_frame->input_stream_id, h->encoders[i]->output_stream_id);
            printf("input_stream->sdi_audio_pair %d, output->sdi_audio_pair %d\n", input_stream->sdi_audio_pair, output_stream->sdi_audio_pair);
            printf("input_stream->input_stream_id %d\n", input_stream->input_stream_id);
#endif
            /* Discard this buffer if it's not destined for our encoders sdi_audio_pair. */
            if (raw_frame->input_stream_id != output_stream->sdi_audio_pair)
                continue;

            /* PTS is the standard 27MHz clock. Adjust by ms. */
            raw_frame->pts += ((int64_t)output_stream->audio_offset_ms * (OBE_CLOCK/1000));
#if LOCAL_DEBUG
            printf("%s() adding A52 frame for input_stream_id %d to encoder output_stream_id %d sdi_audio_pair %d\n", __func__,
                raw_frame->input_stream_id, h->encoders[i]->output_stream_id, output_stream->sdi_audio_pair);
#endif

            remove_from_queue(&filter->queue);
            add_to_encode_queue(h, raw_frame, h->encoders[i]->output_stream_id);
            didForward = 1;
            break;

        } /* For each AC3 bitstream encoder */

        if (!didForward) {
            remove_from_queue(&filter->queue);
            raw_frame->release_data(raw_frame);
            raw_frame->release_frame(raw_frame);
            raw_frame = NULL;
        }
    }

    free( filter_params );

    return NULL;
}

const obe_aud_filter_func_t audio_filter = { start_filter_audio };
