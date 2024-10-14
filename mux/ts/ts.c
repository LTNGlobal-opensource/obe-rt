/*****************************************************************************
 * ts.c: ts muxing functions
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

/* How can the mux "fail", and what error conditions does it have to deal with?
 * By fail we mean, become confused and fail to adapt in the case of lost video
 * or lost audio frames.
 *
 * When the mux starts, its input queue is empty. This queue all any audio or
 * video frames from the upstream compression codecs.
 * Upstream 'encoders' place coded_frame structs 'frames' into the input queue.
 *
 * Upstream encoders run at different processing speeds so frames that enter
 * the mux queue are temporaly unordered, and can arrive in non-realtime
 * conditions. Eg 500 ms or more video video, where as AC3 frames arrive within
 * 35ms of being received from the SDI card. Almost no latency for AC3.
 *
 * So that's a fun design challenge, temporaly unordered frame types.
 * For audio, frames arrive ordered temporaly, both A52/AC3 and MP2.
 * For video, frames arrive out of order, and can be ordered by inspecting
 * the coded_frame->real_dts field (order asencing). As a nitpick detail,
 * video frames don't HAVE to arrive temporaly unordered, it does depend
 * on the configuration of the compressor. Lets assume however that
 * OBE always generates unordered frames (h264 slices).
 *
 * The important (timing related) 'struct coded_frame' vars for video are:
 * real_dts
 *
 * Important coded_frame timing vars for any audio is are:
 * pts
 *
 * 1. One role of the muxes' is to process a queue of frames, deal with any
 *    time related ordering problems caused by the video DTS.
 *
 * Because the input queue has out of ourder PTS/DTS frames, and those
 * frames arrive for video and audio and significantly different rates,
 * the mux may need to 'stall' processing, waiting for its preferred
 * type of frame to arrive. This preferred type of frame is then used
 * to syncronize everything else to.
 *
 * 2. The muxes' preferred type of coded_frame is CF_VIDEO. Each
 *    iterations over the work queue will involve searching for one
 *    or more CF_VIDEO frames. It may not find any, but it almost
 *    certainly will find non-video frames.
 *    If no video frames every arrive, no audio is ever processed, without
 *    video the mux slowly allos all system memory to be exhausted.
 *
 * 3. The mux inspects the queue and waits for a video frame
 *    to arrive. If this is the first video
 *    frame ever processed, it measures how much audio it already has
 *    queued*. A poor assumption is made that audio pts exceeds
 *    video dts, and this assumption is used to remove any audio
 *    frames from the queue earlier than the first video frame.
 *    The 'poor' statement here referrs to the fact that other
 *    audio codecs (unlikely but possible) could take longer
 *    to arrive than video..... That's completely possible but
 *    generally never happens with MP2 or A52/AC3 during testing.
 *
 *    How can this actually happen?
 *    Video frames take much longer to compress than audio frames.
 *    So if the SDI card accepts 60 frames of audio and video,
 *    the majority of the audio frames will appear on the mux
 *    queue long before the first video frame arrives.
 *    The mux makes as assumption that everything with a PTS
 *    less then the first video DTS should be deleted. It does this.
 *
 *    Additionally, when we've processed the very first video frame
 *    then any 'early' audio has been removed, we measure the total
 *    amount of audio in the queue (initial_audio_latency) in 
 *    units of HZ. We'll use this value later when attempting to
 *    compensate for a/v drift through the audio_drift_correction bias.
 *
 * 4. The main loop of the mux can be described as follows:
 *    while(1) {
 *       find a CF_VIDEO frame from the queue or wait until one arrives.
 *       If its the very first frame, measure a few things that help establish
 *       an initial audio pts vs video dts offset.
 *       
 *       Prepare an output 'frames' array.
 *       for all frames in the mux queue {
 *         measure a few things, don't change anything, just gather data. We've
 *         added this because its useful to check this on demand.
 *         Importantly, increment audio_drift_correction based on a heuristic
 *         that measures the last video frame vs the last audio frames, and measures
 *         drifting between their clocks to measure loss. No wall-times are used,
 *         only stream time. This means the corrections survive less-than-realtime
 *         behavioural problems as a result of uncontrolled system load.
 *         The way we do this is different for normal latency vs low latency, but
 *         only slightly.
 *
 *         We measure a few other things to..... more on that later.
 *       }
 *
 *       for all frames in the queue (again) {
 *          if next frame is video
 *            Insert this video frame into 'frames', and use the exact PTS/DTS timing that came from the
 *            video compression codec:
 *              frames[num_frames].cpb_initial_arrival_time = coded_frame->cpb_initial_arrival_time;
 *              frames[num_frames].cpb_final_arrival_time = coded_frame->cpb_final_arrival_time;
 *              frames[num_frames].dts = coded_frame->real_dts;
 *              frames[num_frames].pts = coded_frame->real_pts;
 *              Noticed that no games are played with timing, regardless of the fact that
 *              the PTS or DTS could (in theory) be discontinuious.
 *              In fact, they're never discontinious, but that's a side effect of the upstream
 *              codec (x264) hiding data loss, and actually more of a hassle than a nice design win.
 *           if next frame is audio
 *            Insert this frame into 'frames', and monkey with the PTS/DTS timing.
 *              frames[num_frames].dts = coded_frame->pts - first_video_pts + first_video_real_pts;
 *              frames[num_frames].pts = coded_frame->pts - first_video_pts + first_video_real_pts;
 *              Notice above that the mux rebases the dts of the 'frames' output frame, based on the
 *              current audio pts, minus that initial 'virst_video_pts' and 'first_video_real_pts'
 *              values we measured during mux start? Yeah. This is the (item#3 above) calculated offsets
 *              that were done during first-video-frame-arrival.
 *
 *              What this really does is to reduce the audio PTS/DTS to match that of the current video frame,
 *              by subtracting (essentially) a fixed runtime offset. The offset value is usually one value
 *              for normal latency and a smaller value for low latency.
 *
 *              Let me explain.... Imagine a whackey world where time video frames are measured in minutes....
 *              stick with me....
 *              Eg. if the mux started processing at 1300hrs, and the first video frame arrived at 1305hrs
 *              then most likely the mux queue also contains audio frames at 1301, 1302, 1003.
 *              The video frame will be stamped as 1300hrs. We play some games to ensure that the audio
 *              frame with time 1301 now reads 1305 onwards.... so the video and audio fames both are 1305hrs.
 *              This continues for all audio frames, forever. Yay. Syncronization (usually).
 *
 *              In a perfectly clean SDI signal world, this model never breaks. We don't live in that world.
 *
 *              Occasionally, AC3 frames upstream of the mux get lost (for any number of reasons).
 *              Due to the way the upstream audio encoder filters are written, this means their PTS
 *              values (which increment by a fixed interval). When they don't seem to arrive in the mux as frequently
 *              as they normally do. IE, we lost audio.
 *
 *              The monkeying with the audio PTS/DTS that we mentioned above breaks badly, because it
 *              assumes audio and video frames are never lost. One of the things we've added to this
 *              section of code, is a compensation for audio_drift.
 *
 *              frames[num_frames].pts += audio_drift_correction;
 *              frames[num_frames].dts += audio_drift_correction;
 *
 *              I won't mention how we measure audio drift yet, lets just accept that when we bend the
 *              audio PTS/DTS time (for ac3 or MP2 by the way), we taking into the consideration that
 *              we probably have a really good video frame for 1300hrs, but the associated audio frame went missing
 *              and thus the audio frame for 1301 should NOT be improperly given the 1300 timestamp. Essentially,
 *              we create a gap in the audio PTS/DTS clock to reflect loss. The audio clock 'skips
 *              a beat'. Its works great.
 *
 *              Lastly, we convert the 27MHz DTS/PTS clocks into 90KHz clocks, which libmpegts prefers.
 *
 *       }
 *       Elementary codec stream (ES) is then convert to TS by pushing 'frames' to libmpegts.
 *       libmpegts returns a new ISO13818 transport buffer to us.
 *       Send these TS packets downstream.
 *       Remove / dealloc all of the mux queue frames we've processed, and any 'frames' array entries.
 *    }
 *
 * 5. The end goal of the mux is to prepare an array of 'ts_frame_t'
 *    called 'frames', which uses 90KHz clock. We mentioned this briefly a few moments ago (See #4).
 *    ts_frame_t structs contains elementary stream (codec) data and presentation/display timing.
 *    It passes this to libmpegts (via ts_write_frames()) inorder to have elementary streams
 *    and timing converted into full ISO13818 packets, including PAT, PMT, PES with PTS/DTS.
 *    Properly timed for an external decoder later process. Side effect, 10 seconds of 90KHz
 *    adjustment timing is also added to the PTS/DTS audio/video times by libmpegts (unknown reason).
 *    Once ES to TS conversion is done and the function returns, the TS packets
 *    are handed off by the muxer to the downstream mux smoother,
 *    Nitpick: I'm skimping because I haven't mentioned that the mux smoother want's PCR's too, provided by the
 *    limpegts, and they're also returned along with TS packets. Its not important to the mux
 *    drift/calculation processing.
 *
 * End that's basically how the mux works, mostly.
 *
 * History
 * One of the things we fixed early in the mux process was dealing with audio loss, this is - 
 * the mux queue missing frames of audio. How can this happen?
 * In the case of MP2, the SDI card could (in theory - although we fixed it*) deliver
 * very short audio payload. This doesn't provide enough payload to the audio compressor
 * so the resulting output PTS on the compressed audio is less than expected. (its running behind).
 * "The SDI card 'shorted' the audio compressor by N audio frames upstream", the audio compressor
 * is blind to this, and as a result output time for the audio compressor advanced more
 * slowly, drift is created compared to realtime and compared to a video stream which
 * (in principle) suffered no loss.
 *
 * *When I say we fixed it. We actively discard video frames during SDI capture if the
 * associated audio wasn't of the appropriate length. We 'short' the video compressor by the
 * same amount (a single frame), and the audio compressor by exactly the same amount, and the resuting output
 * PTS/DTS clocks from each compressor remain perfectly in sync. This works exceptionally well.
 *
 * OK, and what about AC3 drift issues?
 * Interesting problem. AC3 frame payload is received via SDI at different intervals to regular
 * video frames or regular PCM (mp2). So we can't just throw away a video frame and AC3 frame
 * to keep the clocks inalignment. To make matters worse, a fully formed AC3 frame is exactly
 * 32ms long. So for every two 720p60 video frames SDI receives, on average OBE "just about"
 * received enough AC3, but likely not.
 *
 * AC3 loss occurs because the AC3 payload itself was interrupted upstream of the mux, usually
 * it's improperly formed and doesn't validate against CRC checks. The ac3bitstream filter
 * intentionally throws it away. The ac3bitstream filter is designed to increment its timing
 * output PTS/DTS by a fixed 32ms for every frame it outputs. So, if the ac3bitstream filter 
 * is rejecting a frame for validation purposes then it has to ensure it keeps the clock moving
 * regardless. This works well for intermittent or sustained minor SDI noise conditions but doesn't work
 * at all when the upstream device stops sending ac3 at all.
 *
 * So AC3 loss is when a frame doesn't validate and we throw it away.
 *   In this case we simply increment the output PTS/DTS regardless in order to keep accurate time.
 *
 * Additionally:
 * AC3 loss happens when the upstream device stops sending it, then later resumes.
 *   "Its shorting the OBE AC3 audio pipeline, but NOT shorting the video pipeline."
 *   In this case, we tried fixing this in the ac3bitstream filter itself, but ultimately
 *   the right fix was to push the problem into the mux, where other timing changes are being
 *   made..... Lets keep all the time adjustment code in one place, so its easier to maintain and
 *   improve, and hopefully describe.
 *   So what did we do to solve this, and how did we test it?
 *   a) In the main mux loop, we monitor the last video frame pts vs the last audio pts and
 *      if they drift too far from each other then we make an adjustment to audio_drift_correction. The
 *      calculation is slightly different for normal vs low latency, its a minor detail.
 *      This technique works really well.
 *      To test it, we trigger a debug feature in the SDI input to 'wipe' all audio data
 *      from a series of frames, watch time bend, then watch it auto-correct itself.
 *
 * Video loss is a real thing too!
 * The last thing we adjusted the mux for was video loss.
 * How on earth can we lose video?
 * We can:
 * a) Throw it away intensionally (see above, short audio frames, to keep MP2 and video time aligned).
 * b) We can have some 'unusual' circumstances from the SDI card when audio payload is received but
 *    no video payload is received.
 *    We fixed this by discarding any frames that contain video or audio, but not both. This works well.
 * c) Lastly, we can trigger video loss using a debug technique, were we simply discard N raw video frames
 *    from the video compressor raw video input queue.
 *    The result is that the video compressor doesn't see the loss, and raw video frames A B C D E get
 *    timestamp 1301 to 1305..... but frames F G H are lost, so frames I J K get timestamps incorrect
 *    1306, 1307, 1308 timestamps..... In order words, the video compressor lost time with respect to audio.
 *    This is passed to the mux and the mux has to deal with that condition.
 *    I've never seen this actually occur, but testing in extremely noisy conditions suggests that it
 *    is occuring and has been measured. How did we fix it?
 *    Using a technique similar to audio_drift_correction, the video compressor filter was adjusted to
 *    measured discrepencies in the hard stream time on each raw video frame, it passes this to
 *    the mux via a discontinuity var we've added.
 *    The mux notices the discontinuity then compensates for that gap by adjusting the
 *    video_drift_correction bias, and thus all future video frames and adjusted routinely.
 *              coded_frame->cpb_initial_arrival_time += video_drift_correction;
 *              coded_frame->cpb_final_arrival_time += video_drift_correction;
 *              coded_frame->real_dts += video_drift_correction;
 *              coded_frame->real_pts += video_drift_correction;
 *
 *    We can notice the gap by monitoring the coded_frame->pts field, which is a reflection of
 *    time as provided by the capture hardware itself. The easy way to do this, because of the temporal
 *    alignment problem in the mux, is to do this upstream in the video compressor. If the video compressor
 *    notices the input PTS time from the audio queue is non-contigious, we measure the amount of time lost
 *    and populate a 'discontinuity' field in the coded_frame struct.
 *    The mux the observes this discontinuity and acts accordingly, adjusting the video_drift_correction field.
 *
 * Closing thoughts:
 * We generally don't need two biases. We should be maintaining a single bias with respect to video.
 * Meh, perhaps a subject for another day. Two biases limits the amount of data we may need to
 * discard from the input queue..... and that's a troublesome though.
 *
 */
#include "common/common.h"
#include "mux/mux.h"
#include <libmpegts.h>
#include <libswresample/swresample.h>
#include <libltntstools/ltntstools.h>

#define MIN_PID 0x30

#define PREFIX "[Mux]: "

int g_mux_audio_mp2_force_pmt_11172 = 0;
int64_t g_mux_pcr_adjustment = 0;

static const int mpegts_stream_info[][3] =
{
    { VIDEO_AVC,   LIBMPEGTS_VIDEO_AVC,      LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_MPEG2, LIBMPEGTS_VIDEO_MPEG2,    LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_HEVC_X265,  LIBMPEGTS_VIDEO_HEVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_HEVC_VEGA3301,  LIBMPEGTS_VIDEO_HEVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_HEVC_VEGA3311,  LIBMPEGTS_VIDEO_HEVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_AVC_VEGA3311,  LIBMPEGTS_VIDEO_AVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_AVC_VAAPI,  LIBMPEGTS_VIDEO_AVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_AVC_CPU_AVCODEC,  LIBMPEGTS_VIDEO_AVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_AVC_GPU_VAAPI_AVCODEC,  LIBMPEGTS_VIDEO_AVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_HEVC_CPU_AVCODEC,  LIBMPEGTS_VIDEO_HEVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_HEVC_GPU_VAAPI_AVCODEC,  LIBMPEGTS_VIDEO_HEVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_HEVC_GPU_NVENC_AVCODEC,  LIBMPEGTS_VIDEO_HEVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    { VIDEO_HEVC_VAAPI,  LIBMPEGTS_VIDEO_HEVC,     LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    /* TODO 302M */
    { AUDIO_MP2,   LIBMPEGTS_AUDIO_MPEG2,    LIBMPEGTS_STREAM_ID_MPEGAUDIO },
    { AUDIO_AC_3,  LIBMPEGTS_AUDIO_AC3,      LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { AUDIO_AC_3_BITSTREAM,  LIBMPEGTS_AUDIO_AC3,      LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { AUDIO_E_AC_3,  LIBMPEGTS_AUDIO_EAC3,   LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { AUDIO_AAC,     LIBMPEGTS_AUDIO_ADTS,   LIBMPEGTS_STREAM_ID_MPEGAUDIO },
    { AUDIO_AAC,     LIBMPEGTS_AUDIO_LATM,   LIBMPEGTS_STREAM_ID_MPEGAUDIO },
    { SUBTITLES_DVB, LIBMPEGTS_DVB_SUB,      LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { MISC_TELETEXT, LIBMPEGTS_DVB_TELETEXT, LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { VBI_RAW,       LIBMPEGTS_DVB_VBI,      LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { DVB_TABLE_SECTION, LIBMPEGTS_TABLE_SECTION,  LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { SMPTE2038, LIBMPEGTS_ANCILLARY_2038, LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { SMPTE2031, LIBMPEGTS_ANCILLARY_2031, LIBMPEGTS_STREAM_ID_PRIVATE_1 },
    { -1, -1, -1 },
};

static const int vbi_service_ids[][2] =
{
    { MISC_TELETEXT, LIBMPEGTS_DVB_VBI_DATA_SERVICE_ID_TTX },
    { MISC_WSS,      LIBMPEGTS_DVB_VBI_DATA_SERVICE_ID_WSS },
    { MISC_VPS,      LIBMPEGTS_DVB_VBI_DATA_SERVICE_ID_VPS },
    { 0, 0 },
};

static const int avc_profiles[][2] =
{
    { 66,  AVC_BASELINE },
    { 77,  AVC_MAIN },
    { 100, AVC_HIGH },
    { 110, AVC_HIGH_10 },
    { 122, AVC_HIGH_422 },
    { 244, AVC_HIGH_444_PRED },
    { 0, 0 },
};

static obe_output_stream_t *get_output_mux_stream( obe_mux_params_t *mux_params, int output_stream_id )
{
    for( int i = 0; i < mux_params->num_output_streams; i++ )
    {
        if( mux_params->output_streams[i].output_stream_id == output_stream_id )
            return &mux_params->output_streams[i];
    }
    return NULL;
}

static void encoder_wait( obe_t *h, int output_stream_id )
{
    /* Wait for encoder to be ready */
    obe_encoder_t *encoder = get_encoder( h, output_stream_id );
    pthread_mutex_lock( &encoder->queue.mutex );
    while( !encoder->is_ready )
        pthread_cond_wait( &encoder->queue.in_cv, &encoder->queue.mutex );
    pthread_mutex_unlock( &encoder->queue.mutex );
}

struct queue_size_s {
    int entries;
    int totalSizeBytes;
};
__inline__ static void queue_size_init(struct queue_size_s *q)
{
    q->entries = 0;
    q->totalSizeBytes = 0;
}

/* Must be called with the mutex already held. */
static void mux_get_queue_counts(obe_t *h,
	struct queue_size_s *vq,
	struct queue_size_s *aq,
	struct queue_size_s *oq)
{
    queue_size_init(vq); /* Video q */
    queue_size_init(aq); /* Audio q */
    queue_size_init(oq); /* Other q */

    for (int i = 0; i < h->mux_queue.size; i++) {
        obe_coded_frame_t *cf = h->mux_queue.queue[i];
        if (cf->type == CF_VIDEO) {
            vq->entries++;
            vq->totalSizeBytes += cf->len;
        } else
        if (cf->type == CF_AUDIO) {
            aq->entries++;
            aq->totalSizeBytes += cf->len;
        } else {
            oq->entries++;
            oq->totalSizeBytes += cf->len;
        }
    }
}

int64_t last_video_dts = 0;

/* For a given video pts, count how many items one the queue are older and newer than this value */
static void verify_audio_pts_queue_content(obe_t *h, int64_t videoDTS)
{
    int older = 0, newer = 0;
    int64_t min_a_pts = 0xffffffffffLL;
    int64_t min_v_pts = 0xffffffffffLL;
    int64_t max_a_pts = 0;

    //pthread_mutex_lock(&h->mux_queue.mutex);
    for (int i = 0; i < h->mux_queue.size; i++) {
        obe_coded_frame_t *cf = h->mux_queue.queue[i];
        if (cf->type == CF_VIDEO) {
            if (cf->pts < min_v_pts)
                min_v_pts = cf->pts;
            continue;
        }
        if (cf->pts <= videoDTS)
            older++;
        else
            newer++;

        if (cf->pts < min_a_pts)
            min_a_pts = cf->pts;
        if (cf->pts >= max_a_pts)
            max_a_pts = cf->pts;
    }
    //pthread_mutex_unlock(&h->mux_queue.mutex);
#if 0
    printf("older %5d newer %5d    min_a_pts %12" PRIi64 " max_a_pts %12" PRIi64 " delta %12" PRIi64 "(ms) video_dts %12" PRIi64 ", delta %12" PRIi64 "\n",
        older, newer,
        min_a_pts, max_a_pts,
        (max_a_pts - min_a_pts) / 300 / 90,
        videoDTS,
        (min_a_pts - min_v_pts) / 300 / 90);
#endif
}

static void mux_dump_queue_content(obe_t *h)
{
    pthread_mutex_lock(&h->mux_queue.mutex);
    for (int i = 0; i < h->mux_queue.size; i++) {
        obe_coded_frame_t *cf = h->mux_queue.queue[i];
        coded_frame_print(cf);
    }
    pthread_mutex_unlock(&h->mux_queue.mutex);
}

#define MAX_QUEUED_AUDIO_WARNING 100
void mux_dump_queue(obe_t *h)
{
    struct queue_size_s vq;
    struct queue_size_s aq;
    struct queue_size_s oq;

    pthread_mutex_lock(&h->mux_queue.mutex);
    mux_get_queue_counts(h, &vq, &aq, &oq);
    pthread_mutex_unlock(&h->mux_queue.mutex);

    printf("\tmux.video frames = %d totalsize %d [%s]\n",
        vq.entries, vq.totalSizeBytes,
        vq.entries >= 60 ? "Too many frames - unusual" : "OK");

    printf("\tmux.audio frames = %d totalsize %d [%s]\n",
        aq.entries, aq.totalSizeBytes,
        aq.entries >= MAX_QUEUED_AUDIO_WARNING ? "Too many frames - video compressor too slow" : "OK");

    printf("\tmux.other frames = %d totalsize %d\n",
        oq.entries, oq.totalSizeBytes);

#if 1
    mux_dump_queue_content(h);
#endif
}

/* Must be called with the queue already locked. */
static void mux_monitor_queue(obe_t *h)
{
    struct queue_size_s vq;
    struct queue_size_s aq;
    struct queue_size_s oq;

    mux_get_queue_counts(h, &vq, &aq, &oq);

    if (aq.entries > MAX_QUEUED_AUDIO_WARNING) {
        static time_t lastReport = 0;
        time_t now;
        time(&now);
        /* Rate limit warning to every 15 seconds. */
        if (now >= lastReport + 15) {
            char msg[256];
            sprintf(msg, "Warning: Encoder video codec is probably running less than realtime, usually bad.");
            syslog(LOG_ERR, "%s", msg);
            fprintf(stderr, "%s\n", msg);
            lastReport = now;
        }
    }
}

int64_t initial_audio_latency = -1; /* ticks of 27MHz clock. Amount of audio (in time) we have buffered before the first video frame appeared. */

ts_writer_t *g_mux_ts_writer_handle = NULL;
int g_mux_ts_monitor_bps = 0;
int64_t g_mux_dtstotal = 0;

void *open_muxer( void *ptr )
{
    obe_mux_params_t *mux_params = ptr;
    obe_t *h = mux_params->h;
    obe_mux_opts_t *mux_opts = &h->mux_opts;
    int cur_pid = MIN_PID;
    int stream_format, video_pid = 0, video_found = 0, width = 0,
    height = 0, has_dds = 0, len = 0, num_frames = 0;
    uint8_t *output;
    int64_t first_video_pts = -1, video_dts, first_video_real_pts = -1;
    int64_t *pcr_list;
    ts_writer_t *w;
    ts_main_t params = {0};
    ts_program_t program = {0};
    ts_stream_t *stream;
    ts_dvb_sub_t subtitles;
    ts_dvb_vbi_t *vbi_services;
    ts_frame_t *frames;
    obe_int_input_stream_t *input_stream;
    obe_output_stream_t *output_stream;
    obe_encoder_t *encoder;
    obe_muxed_data_t *muxed_data;
    obe_coded_frame_t *coded_frame;
    char *service_name = "OBE Service";
    char *provider_name = "Open Broadcast Encoder";
    struct ltntstools_stream_statistics_s *streamstats = NULL;

    struct sched_param param = {0};
    param.sched_priority = 99;
    pthread_setschedparam( pthread_self(), SCHED_RR, &param );

    // TODO sanity check the options

    params.ts_type = mux_opts->ts_type;
    params.cbr = !!mux_opts->cbr;
    params.muxrate = mux_opts->ts_muxrate;

    params.pcr_period = mux_opts->pcr_period;
    params.pat_period = mux_opts->pat_period;

    w = ts_create_writer();
    g_mux_ts_writer_handle = w;
    if( !w )
    {
        fprintf( stderr, "[ts] could not create writer\n" );
        return NULL;
    }

    ts_set_scte35_enable(w, h->enable_scte35);

    const char *tswriter_filename = getenv("OBE_LIBMPEGTS_WRITER_FILENAME");
    if (tswriter_filename) {
        printf(PREFIX "Warning -- Dumping all TSWRITER payload to '%s'\n", tswriter_filename);
        if (libmpegts_frame_serializer_open_write(w, tswriter_filename) < 0) {
            fprintf(stderr, PREFIX "Unable to create file %s, aborting.\n", tswriter_filename);
            ts_close_writer(w);
            return NULL;
        }
    }

    params.num_programs = 1;
    params.programs = &program;
    program.is_3dtv = !!mux_opts->is_3dtv;
    // TODO more mux opts

    program.streams = calloc( mux_params->num_output_streams, sizeof(*program.streams) );
    if( !program.streams )
    {
        fprintf( stderr, "malloc failed\n" );
        goto end;
    }

    program.num_streams = mux_params->num_output_streams;

    if( mux_opts->passthrough )
    {
        /* TODO lock when we can add multiple devices */
        params.ts_id = h->devices[0]->ts_id;
        program.program_num = h->devices[0]->program_num;
        program.pmt_pid = h->devices[0]->pmt_pid;
        program.pcr_pid = h->devices[0]->pcr_pid;
    }
    else
    {
        params.ts_id = mux_opts->ts_id ? mux_opts->ts_id : 1;
        program.program_num = mux_opts->program_num ? mux_opts->program_num : 1;
        program.pmt_pid = mux_opts->pmt_pid ? mux_opts->pmt_pid : cur_pid++;
        /* PCR PID is done later once we know the video pid */
    }

    for( int i = 0; i < program.num_streams; i++ )
    {
        stream = &program.streams[i];
        output_stream = &mux_params->output_streams[i];
        input_stream = get_input_stream( h, output_stream->input_stream_id );

        if( output_stream->stream_action == STREAM_ENCODE )
            stream_format = output_stream->stream_format;
        else
            stream_format = input_stream->stream_format;

        int j = 0;
        while( mpegts_stream_info[j][0] != -1 && stream_format != mpegts_stream_info[j][0] )
            j++;

        /* OBE does not distinguish between ADTS and LATM but MPEG-TS does */
        if( stream_format == AUDIO_AAC && ( ( output_stream->stream_action == STREAM_PASSTHROUGH && input_stream->is_latm ) ||
            ( output_stream->stream_action == STREAM_ENCODE && output_stream->aac_opts.latm_output ) ) )
            j++;

        stream->stream_format = mpegts_stream_info[j][1];

        /* Override the stream type, declare this in the PMT as 11172-3 instead of the default 13818-3 */
        if (stream->stream_format == LIBMPEGTS_AUDIO_MPEG2 && g_mux_audio_mp2_force_pmt_11172) {
            stream->stream_format = LIBMPEGTS_AUDIO_MPEG1;
        }

        stream->stream_id = mpegts_stream_info[j][2]; /* Note this is the MPEG-TS stream_id, not the OBE stream_id */
        if( mux_opts->passthrough )
        {
            output_stream->ts_opts.pid = stream->pid = input_stream->pid ? input_stream->pid : cur_pid++;
            if( input_stream->stream_type == STREAM_TYPE_AUDIO )
            {
                stream->write_lang_code = !!strlen( input_stream->lang_code );
                memcpy( stream->lang_code, input_stream->lang_code, 4 );
                stream->audio_type = input_stream->audio_type;
            }
            stream->has_stream_identifier = input_stream->has_stream_identifier;
            stream->stream_identifier = input_stream->stream_identifier;
        }
        else
        {
            output_stream->ts_opts.pid = stream->pid = output_stream->ts_opts.pid ? output_stream->ts_opts.pid : cur_pid++;

            if ((input_stream->stream_type == STREAM_TYPE_MISC) && (input_stream->stream_format == DVB_TABLE_SECTION)) {
                /* Detected a SCTE35 PID */
            }

            if( input_stream->stream_type == STREAM_TYPE_AUDIO )
            {
                stream->write_lang_code = !!strlen( output_stream->ts_opts.lang_code );
                memcpy( stream->lang_code, output_stream->ts_opts.lang_code, 4 );
                stream->audio_type = output_stream->ts_opts.audio_type;
            }
            stream->has_stream_identifier = output_stream->ts_opts.has_stream_identifier;
            stream->stream_identifier = output_stream->ts_opts.stream_identifier;
        }

        if (stream_format == VIDEO_AVC || stream_format == VIDEO_HEVC_X265 || stream_format == VIDEO_AVC_VAAPI || stream_format == VIDEO_HEVC_VAAPI || stream_format == VIDEO_AVC_GPU_VAAPI_AVCODEC || stream_format == VIDEO_HEVC_GPU_VAAPI_AVCODEC ||
            stream_format == VIDEO_AVC_CPU_AVCODEC || stream_format == VIDEO_HEVC_CPU_AVCODEC || stream_format == VIDEO_HEVC_GPU_NVENC_AVCODEC || stream_format == VIDEO_HEVC_VEGA3301 ||
            stream_format == VIDEO_HEVC_VEGA3311 || stream_format == VIDEO_AVC_VEGA3311)
        {
            encoder_wait( h, output_stream->output_stream_id );

            width = output_stream->avc_param.i_width;
            height = output_stream->avc_param.i_height;
            video_pid = stream->pid;
        }
        else if( stream_format == AUDIO_MP2 )
            stream->audio_frame_size = (double)MP2_NUM_SAMPLES * 90000LL * output_stream->ts_opts.frames_per_pes / input_stream->sample_rate;
        else if( stream_format == AUDIO_AC_3 )
            stream->audio_frame_size = (double)AC3_NUM_SAMPLES * 90000LL * output_stream->ts_opts.frames_per_pes / input_stream->sample_rate;
        else if( stream_format == AUDIO_E_AC_3 || stream_format == AUDIO_AAC )
        {
            encoder_wait( h, output_stream->output_stream_id );
            encoder = get_encoder( h, output_stream->output_stream_id );
            stream->audio_frame_size = (double)encoder->num_samples * 90000LL * output_stream->ts_opts.frames_per_pes / input_stream->sample_rate;
        }
        else if (stream_format == AUDIO_AC_3_BITSTREAM) {
            output_stream->ts_opts.frames_per_pes = 1;
            stream->audio_frame_size = 2880; /* In 90KHz ticks - 32ms */
        } else {
            printf("stream_format = %d\n", stream_format);
            printf("frames_per_pes = %d\n", output_stream->ts_opts.frames_per_pes);
            printf("sample_rate = %d\n", input_stream->sample_rate);
            printf("WARNING: audio frame size is unknown -- Poorly defined new audio codec?\n");
	}
    }

    /* Video stream isn't guaranteed to be first so populate program parameters here */
    if( !mux_opts->passthrough ) {
        program.pcr_pid = mux_opts->pcr_pid ? mux_opts->pcr_pid : video_pid;
    }

    program.sdt.service_type = height >= 720 ? DVB_SERVICE_TYPE_ADVANCED_CODEC_HD : DVB_SERVICE_TYPE_ADVANCED_CODEC_SD;
    program.sdt.service_name = mux_opts->service_name ? mux_opts->service_name : service_name;
    program.sdt.provider_name = mux_opts->provider_name ? mux_opts->provider_name : provider_name;

    if( ts_setup_transport_stream( w, &params ) < 0 )
    {
        fprintf( stderr, "[ts] Transport stream setup failed\n" );
        goto end;
    }

    ts_set_ve_version(w, h->sw_major, h->sw_minor, h->sw_patch);

    ts_set_section_padding(w, mux_opts->section_padding);
    ts_setup_pcr_adjustment(w, g_mux_pcr_adjustment); /* Add Optional delay to a gold leaf mux PCR */
    
    if( mux_opts->ts_type == OBE_TS_TYPE_GENERIC || mux_opts->ts_type == OBE_TS_TYPE_DVB )
    {
        if( ts_setup_sdt( w ) < 0 )
        {
            fprintf( stderr, "[ts] SDT setup failed\n" );
            goto end;
        }
    }

    /* setup any streams if necessary */
    for( int i = 0; i < program.num_streams; i++ )
    {
        stream = &program.streams[i];
        output_stream = &mux_params->output_streams[i];
        input_stream = get_input_stream( h, output_stream->input_stream_id );
        encoder = get_encoder( h, output_stream->output_stream_id );

        if( output_stream->stream_action == STREAM_ENCODE )
            stream_format = output_stream->stream_format;
        else
            stream_format = input_stream->stream_format;

        if (stream_format == VIDEO_AVC)
        {
            x264_param_t *p_param = encoder->encoder_params;
            int j = 0;
//p_param->i_profile = 100;
            while( avc_profiles[j][0] && p_param->i_profile != avc_profiles[j][0] )
                j++;
//p_param->i_level_idc = 32;
//printf("p_param->i_level_idc = %d\n", p_param->i_level_idc);

            if( ts_setup_mpegvideo_stream( w, stream->pid, p_param->i_level_idc, avc_profiles[j][1], 0, 0, 0 ) < 0 )
            {
                fprintf( stderr, "[ts] Could not setup AVC video stream\n" );
                goto end;
            }
        } else if (stream_format == VIDEO_AVC_VEGA3311)
        {
            /* TODO: Make sure this is correct for the VEGA GPU codec settings.
             * We're fixed to 5.2 and HIGH currently
             */
            if (ts_setup_mpegvideo_stream(w, stream->pid, 52, AVC_HIGH, 0, 0, 0) < 0) {
                fprintf(stderr, PREFIX "Could not setup VEGA AVC GPU video stream\n");
                goto end;
            }
        } else if ((stream_format == VIDEO_AVC_GPU_VAAPI_AVCODEC) || (stream_format == VIDEO_AVC_CPU_AVCODEC))
        {
            if (ts_setup_mpegvideo_stream(w, stream->pid, 40, AVC_HIGH, 0, 0, 0) < 0) {
                fprintf(stderr, "[ts] Could not setup AVC GPU video stream\n");
                goto end;
            }
        }
        else if (stream_format == VIDEO_AVC_VAAPI)
        {
            if (ts_setup_mpegvideo_stream(w, stream->pid, 40, AVC_HIGH, 0, 0, 0) < 0) {
                fprintf(stderr, "[ts] Could not setup HEVC video stream\n");
                goto end;
            }
        }
        else if (stream_format == VIDEO_HEVC_X265 || stream_format == VIDEO_HEVC_GPU_VAAPI_AVCODEC ||
            stream_format == VIDEO_HEVC_GPU_NVENC_AVCODEC || stream_format == VIDEO_HEVC_CPU_AVCODEC ||
            stream_format == VIDEO_HEVC_VEGA3301 || stream_format == VIDEO_HEVC_VEGA3311)

        {
            if (ts_setup_mpegvideo_stream(w, stream->pid, 51, HEVC_PROFILE_MAIN, 0, 0, 0) < 0) {
                fprintf(stderr, "[ts] Could not setup HEVC video stream\n");
                goto end;
            }
        }
        else if (stream_format == VIDEO_HEVC_VAAPI)
        {
            if (ts_setup_mpegvideo_stream(w, stream->pid, 40, HEVC_PROFILE_MAIN, 0, 0, 0) < 0) {
                fprintf(stderr, "[ts] Could not setup HEVC VAAPI video stream\n");
                goto end;
            }
        }
        else if( stream_format == AUDIO_AAC )
        {
            /* TODO: handle associated switching */
            int profile_and_level, num_channels = av_get_channel_layout_nb_channels( output_stream->channel_layout );

            if( num_channels > 2 )
                profile_and_level = output_stream->aac_opts.aac_profile == AAC_HE_V2 ? LIBMPEGTS_MPEG4_HE_AAC_V2_PROFILE_LEVEL_5 :
                                    output_stream->aac_opts.aac_profile == AAC_HE_V1 ? LIBMPEGTS_MPEG4_HE_AAC_PROFILE_LEVEL_5 :
                                                                                       LIBMPEGTS_MPEG4_AAC_PROFILE_LEVEL_5;
            else
                profile_and_level = output_stream->aac_opts.aac_profile == AAC_HE_V2 ? LIBMPEGTS_MPEG4_HE_AAC_V2_PROFILE_LEVEL_2 :
                                    output_stream->aac_opts.aac_profile == AAC_HE_V1 ? LIBMPEGTS_MPEG4_HE_AAC_PROFILE_LEVEL_2 :
                                                                                       LIBMPEGTS_MPEG4_AAC_PROFILE_LEVEL_2;

            /* T-STD ignores LFE channel */
            if( output_stream->channel_layout & AV_CH_LOW_FREQUENCY )
                num_channels--;

            if( ts_setup_mpeg4_aac_stream( w, stream->pid, profile_and_level, num_channels ) < 0 )
            {
                fprintf( stderr, "[ts] Could not setup AAC stream\n" );
                goto end;
            }
        }
        else if( stream_format == SUBTITLES_DVB )
        {
            memcpy( subtitles.lang_code, input_stream->lang_code, 4 );
            subtitles.subtitling_type = input_stream->dvb_subtitling_type;
            subtitles.composition_page_id = input_stream->composition_page_id;
            subtitles.ancillary_page_id = input_stream->ancillary_page_id;
            /* A lot of streams don't have DDS flagged correctly so we assume all HD uses DDS */
            has_dds = width >= 1280 && height >= 720;
            if( ts_setup_dvb_subtitles( w, stream->pid, has_dds, 1, &subtitles ) < 0 )
            {
                fprintf( stderr, "[ts] Could not setup DVB Subtitle stream\n" );
                goto end;
            }
        }
        else if(stream_format == SMPTE2031)
        {
            if (ts_setup_dvb_teletext(w, stream->pid, output_stream->ts_opts.num_teletexts, (ts_dvb_ttx_t *)output_stream->ts_opts.teletext_opts ) < 0) {
                fprintf( stderr, "[ts] Could not setup SMPTE2031 / Teletext stream\n" );
                goto end;
            }
        }
        else if( stream_format == VBI_RAW || stream_format == MISC_TELETEXT )
        {
            if( output_stream->ts_opts.num_teletexts )
            {
                if( ts_setup_dvb_teletext( w, stream->pid, output_stream->ts_opts.num_teletexts,
                    (ts_dvb_ttx_t*)output_stream->ts_opts.teletext_opts ) < 0 )
                {
                    fprintf( stderr, "[ts] Could not setup Teletext stream\n" );
                    goto end;
                }
            }

            /* FIXME: let users specify VBI lines */
            if( stream_format == VBI_RAW && input_stream )
            {
                vbi_services = calloc( input_stream->num_frame_data, sizeof(*vbi_services) );
                if( !vbi_services )
                {
                    fprintf( stderr, "malloc failed\n" );
                    goto end;
                }

                for( int j = 0; j < input_stream->num_frame_data; j++ )
                {
                    for( int k = 0; vbi_service_ids[k][0] != 0; k++ )
                    {
                        if( input_stream->frame_data[j].type == vbi_service_ids[k][0] )
                            vbi_services[j].data_service_id = vbi_service_ids[k][1];
                    }

                    /* This check is not strictly necessary */
                    if( !vbi_services[j].data_service_id )
                        goto end;

                    vbi_services[j].num_lines = input_stream->frame_data[j].num_lines;
                    vbi_services[j].lines = malloc( vbi_services[j].num_lines * sizeof(*vbi_services[j].lines) );
                    if( !vbi_services[j].lines )
                    {
                        fprintf( stderr, "malloc failed\n" );
                        goto end;
                    }

                    for( int k = 0; k < input_stream->frame_data[j].num_lines; k++ )
                    {
                        int tmp_line, field;

                        obe_convert_smpte_to_analogue( input_stream->vbi_ntsc ? INPUT_VIDEO_FORMAT_NTSC : INPUT_VIDEO_FORMAT_PAL, input_stream->frame_data[j].lines[k],
                                                       &tmp_line, &field );

                        vbi_services[j].lines[k].field_parity = field == 1 ? 1 : 0;
                        vbi_services[j].lines[k].line_offset = tmp_line;
                    }
                }

                if( ts_setup_dvb_vbi( w, stream->pid, input_stream->num_frame_data, vbi_services ) < 0 )
                {
                    fprintf( stderr, "[ts] Could not setup VBI stream\n" );
                    goto end;
                }

                for( int j = 0; j < input_stream->num_frame_data; j++ )
                    free( vbi_services[j].lines );
                free( vbi_services );
            }
        }
    }

    //FILE *fp = fopen( "test.ts", "wb" );

    streamstats = malloc(sizeof(*streamstats));
    ltntstools_pid_stats_reset(streamstats);

    while( 1 )
    {
        video_found = 0;
        video_dts = 0;

        pthread_mutex_lock( &h->mux_queue.mutex );

        if( h->cancel_mux_thread )
        {
            pthread_mutex_unlock( &h->mux_queue.mutex );
            goto end;
        }

        mux_monitor_queue(h);

        /* We need to find the audio frame immediately prior to the first video frame,
         * if such a thing exists..... We'll use this in calculating how much queue audio
         * data (by pts) we have, so when AC3 drifts we have a helper to understand our orignal
         * stream offset.
         */
        int64_t current_audio_pts = 0;
        while( !video_found )
        {
            for( int i = 0; i < h->mux_queue.size; i++ )
            {
                coded_frame = h->mux_queue.queue[i];
                if (coded_frame->type == CF_VIDEO)
                {
                    video_found = 1;
                    video_dts = coded_frame->real_dts;
#if 0
                    printf("dts delta %12" PRIi64 "   last %12" PRIi64 "  next %12" PRIi64 "  len %d\n",
                        coded_frame->real_dts - last_video_dts,
                        last_video_dts,
                        coded_frame->real_dts,
                        coded_frame->len);
#endif
                    last_video_dts = coded_frame->real_dts;

                    /* FIXME: handle case where first_video_pts < coded_frame->real_pts */
                    if( first_video_pts == -1 )
                    {
                        /* Get rid of frames which are too early */
                        first_video_pts = coded_frame->pts;
                        first_video_real_pts = coded_frame->real_pts;
                        remove_early_frames( h, first_video_pts );
                        printf("Frame too early, removing ---- BAD\n");
                    }
                    break;
                } else {
                    current_audio_pts = coded_frame->pts;
                }
            }
            if (video_found) {
                    /* In AC3 passthorugh and normal latency, we queue 40-50
                     * frames of audio data before a video frame arrives. 
                     * If the upstream device stops sending audio we burn though
                     * these frames then stop outputting audio.
                     * In order to correctly adjust the audio clode when frames
                     * arrive sometime in the future, we need to understad our
                     * original and initial audio offset, else we'll incorrectly
                     * calculate a new pts offset and we'll have a/v sync issues.
                     * initial_audio_latency mresure how much data (in time)
                     * we always need to keep the PTS clock ahead by.
                     */
                    if (initial_audio_latency == -1) {
                        initial_audio_latency = current_audio_pts;
                    }
            }

            if( !video_found )
                pthread_cond_wait( &h->mux_queue.in_cv, &h->mux_queue.mutex );

            if( h->cancel_mux_thread )
            {
                pthread_mutex_unlock( &h->mux_queue.mutex );
                goto end;
            }
        } // while

        frames = calloc( h->mux_queue.size, sizeof(*frames) );
        if( !frames )
        {
            syslog( LOG_ERR, "Malloc failed\n" );
            pthread_mutex_unlock( &h->mux_queue.mutex );
            goto end;
        }

        //printf("\n START - queuelen %i \n", h->mux_queue.size);

    verify_audio_pts_queue_content(h, video_dts);

	/* Prpare the 'frames' array with any audio and video frames.
	 * If we detect any video frames with discontinuities, adjust out video_drift_correction.
	 */
        num_frames = 0;
        for (int i = 0; i < h->mux_queue.size; i++)
        {
            coded_frame = h->mux_queue.queue[i];

            if (h->verbose_bitmask & MUX__DQ_HEXDUMP) {
                printf("coded_frame: output_stream_id = %d, type = %d, len = %6d -- ",
                    coded_frame->output_stream_id, coded_frame->type, coded_frame->len);
                for (int x = 0; x < coded_frame->len; x++) {
                    printf("%02x ", *(coded_frame->data + x));
                    if (x > 8)
                        break;
                }
                printf("\n");
            }

            output_stream = get_output_mux_stream( mux_params, coded_frame->output_stream_id );
            // FIXME name
            /* Rescaled_dts only applies to non-video frames, in the queue prior to related video frames,
             * such as when running in normal latency and AC3 bitstream, were 50 or so AC3 frames arrive
             * before the first video frame.
             */
            int64_t rescaled_dts = coded_frame->pts - first_video_pts + first_video_real_pts;
#if 0
            printf("A: %d was rescaled_dts %" PRIi64 "  video_dts %" PRIi64 "  first_video_pts %" PRIi64 "  first_video_real_pts %" PRIi64 "  cf->pts %" PRIi64 "\n",
                coded_frame->type == CF_AUDIO,
                rescaled_dts, video_dts, first_video_pts, first_video_real_pts, coded_frame->pts);
#endif
            if (coded_frame->type == CF_VIDEO)
                rescaled_dts = coded_frame->real_dts;

            //printf("\n stream-id %i ours: %"PRIi64" \n", coded_frame->output_stream_id, coded_frame->pts );
#if 0
            printf("A: %d now rescaled_dts %" PRIi64 "  video_dts %" PRIi64 "  first_video_pts %" PRIi64 "  first_video_real_pts %" PRIi64 "\n",
                coded_frame->type == CF_AUDIO,
                rescaled_dts, video_dts, first_video_pts, first_video_real_pts);
#endif
#if 0
            if (coded_frame->type == CF_AUDIO) {
                printf("video_dts %" PRIi64 "\n",
                    video_dts);
            }
#endif
            if( rescaled_dts <= video_dts )
            {
                frames[num_frames].opaque = h->mux_queue.queue[i];
                frames[num_frames].size = coded_frame->len;
                frames[num_frames].data = coded_frame->data;
                frames[num_frames].pid = output_stream->ts_opts.pid;
                if (coded_frame->type == CF_VIDEO)
                {
                    frames[num_frames].cpb_initial_arrival_time = coded_frame->cpb_initial_arrival_time;
                    frames[num_frames].cpb_final_arrival_time = coded_frame->cpb_final_arrival_time;
                    frames[num_frames].dts = coded_frame->real_dts;
                    frames[num_frames].pts = coded_frame->real_pts;
                }
                else
                {
                    /* This has always applied equally to audio or 'other' non video frames. */
                    frames[num_frames].dts = coded_frame->pts - first_video_pts + first_video_real_pts;
                    frames[num_frames].pts = coded_frame->pts - first_video_pts + first_video_real_pts;
                }

                frames[num_frames].dts /= 300LL;
                frames[num_frames].pts /= 300LL;

                //printf("\n pid: %i ours: %"PRIi64" \n", frames[num_frames].pid, frames[num_frames].dts );
                frames[num_frames].random_access = coded_frame->random_access;
                frames[num_frames].priority = coded_frame->priority;
                num_frames++;
            } else {
                /* Skipping this frame, we're not ready to mux it yet, its in the future. */
                /* This happens a lot with AC3 / bitstream frames in normal latency mode. */
#if 0
            printf("%s: skp rescaled_dts %" PRIi64 "  video_dts %" PRIi64 "  first_video_pts %" PRIi64 "  first_video_real_pts %" PRIi64 "\n",
                coded_frame->type == CF_AUDIO ? "A" : "V",
                rescaled_dts, video_dts, first_video_pts, first_video_real_pts);
#endif
            }
        }

        pthread_mutex_unlock( &h->mux_queue.mutex );

        // TODO figure out last frame
        if (ts_write_frames( w, frames, num_frames, &output, &len, &pcr_list, &g_mux_dtstotal) != 0) {
            fprintf(stderr, "ts_write_frames failed\n");
        }        

        /* Safety: Abort if the mux queue appears to have stalled for 30 or more seconds.
         * 2200 is 30 seconds of NL with 8xstereo audio,
         * where the mux is queueing approx 75 frames per second and they're
         * not being dequeued. Take a hard exit and make sure we signal to syslog
         * that an abnormal situation has occured.
         */
        if (ts_query_num_buffered_frames(w) > 2200) {
            klsyslog_and_stdout(LOG_ERR,
                "LTN Encoder service abnormality: mux queue stalled (%d), hard exit occuring.\n",
                ts_query_num_buffered_frames(w));
            exit(1);
        }

        if (g_mux_ts_monitor_bps) {
            static int lenbps_old = 0;
            static int len_current = 0;
            static time_t lentv, now;
            now = time(0);
            if (now != lentv) {
                lenbps_old = len_current * 8;
                printf(PREFIX "dequeued bps %d\n", lenbps_old);
                len_current = 0;
                lentv = now;
            }
            len_current += len;
        }
#if 0
//printf("bb = %d len = %d\n", bb, len);
static FILE *fh = NULL;

if (fh == NULL)
  fh = fopen("/tmp/hevc.ts", "wb");

if (fh)
  fwrite(output, 1, len, fh);
#endif

        if( len )
        {
            ltntstools_pid_stats_update(streamstats, output, len / 188);

            uint32_t null_pct = ltntstools_pid_stats_stream_padding_pct(streamstats);
            uint32_t null_pct_val = 3;
            /* Report null padding issues after the few first seconds of startup. */
            if (null_pct <= null_pct_val && obe_getProcessRuntimeSeconds() > 3) {
                char ts[64];
                obe_getTimestamp(ts, NULL);
                static time_t lastPrint = 0;
                if (lastPrint + 1 < time(NULL)) {
                    lastPrint = time(NULL);
                    printf(PREFIX "%s : Warning: null padding %d%% or less (%d%%), codec exceeding the muxer capability.\n", ts, null_pct_val, null_pct);
                }
            }

            muxed_data = new_muxed_data( len );
            if( !muxed_data )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                goto end;
            }

            memcpy( muxed_data->data, output, len );
            muxed_data->pcr_list = malloc( (len / 188) * sizeof(int64_t) );
            if( !muxed_data->pcr_list )
            {
                syslog( LOG_ERR, "Malloc failed\n" );
                destroy_muxed_data( muxed_data );
                goto end;
            }
            memcpy( muxed_data->pcr_list, pcr_list, (len / 188) * sizeof(int64_t) );
            add_to_queue( &h->mux_smoothing_queue, muxed_data );
        }

        for( int i = 0; i < num_frames; i++ )
        {
            remove_item_from_queue( &h->mux_queue, frames[i].opaque );
            destroy_coded_frame( frames[i].opaque );
        }

        free( frames );
    }

end:
    free(streamstats);

    ts_close_writer( w );

    /* TODO: clean more */

    free( program.streams );
    free( ptr );

    return NULL;

}

const obe_mux_func_t ts_muxer = { open_muxer };
