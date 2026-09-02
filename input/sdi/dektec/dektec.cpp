#if HAVE_DTAPI_H

/*****************************************************************************
 * dektec.cpp: Audio/Video from the Dektec Matrix API
 *****************************************************************************
 * Copyright (C) 2019 LTN
 *
 * Authors: Steven Toth <stoth@ltnglobal.com>
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

#include <stdio.h>
#include <string.h>
#include <iostream>

using namespace std;

#define MODULE_PREFIX "[dektec]: "

extern "C"
{
#include "common/common.h"
#include "common/lavc.h"
#include "input/input.h"
#include "input/sdi/sdi.h"
#include "input/sdi/ancillary.h"
#include "input/sdi/vbi.h"
#include "input/sdi/x86/sdi.h"
#include "input/sdi/v210.h"
}

#include <DTAPI.h>
#include <input/sdi/dektec/MxAvRecorderDemo.h>

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)
#define DEKTEC_VERSION STR(DTAPI_VERSION_MAJOR) "." STR(DTAPI_VERSION_MINOR) "." STR(DTAPI_VERSION_BUGFIX) "." STR(DTAPI_VERSION_BUILD)

const char *dektec_sdk_version = DEKTEC_VERSION;

struct obe_to_dektec_video
{
    int obe_name;
    int width, height;
    int dektecVideoStandard;
    int timebase_num;
    int timebase_den;
};

const static struct obe_to_dektec_video video_format_tab[] =
{
    { INPUT_VIDEO_FORMAT_720P_5994, 1280, 720,  DTAPI_VIDSTD_720P59_94, 1001, 60000 },
    { INPUT_VIDEO_FORMAT_720P_60,   1280, 720,     DTAPI_VIDSTD_720P60, 1000, 60000 },
};

static const struct obe_to_dektec_video *lookupDektecStandard(int std)
{
	for (unsigned int i = 0; i < (sizeof(video_format_tab) / sizeof(struct obe_to_dektec_video)); i++) {
		const struct obe_to_dektec_video *fmt = &video_format_tab[i];
		if (fmt->dektecVideoStandard == std)
		{
			return fmt;
		}
	}

	return NULL;
}

DtDevice TheCard;

typedef struct
{
	/* Dektec input related */
	DtDevice *TheCard;
	MxAvRecorderDemo *R;

	unsigned int currentFrame;
	uint64_t v_counter;

	/* AVCodec for V210 conversion. */
	AVCodec         *dec;
	AVCodecContext  *codec;
	/* End: AVCodec for V210 conversion. */

	obe_device_t *device;
	obe_t *h;

	AVRational   v_timebase;
} dektec_ctx_t;

typedef struct
{
    dektec_ctx_t ctx;

    /* Input */
    int card_idx;

    int video_format;
    int num_channels;

    /* True if we're problem, else false during normal streaming. */
    int probe;
    int detectedVideoStandard;

    /* Output */
    int probe_success;

    int width;
    int height;
    int timebase_num;
    int timebase_den;

    int interlaced;
    int tff;
} dektec_opts_t;

static void deliver_video_frame(dektec_opts_t *opts, unsigned char *plane, int sizeBytes, int strideBytes);
static void deliver_audio_frame(dektec_opts_t *opts, unsigned char *plane, int sizeBytes, int sampleCount, int64_t frametime);

/* All of the dektec specific class implemenation */

static const int DEF_CARD_TYPE = 2172;
static const int DEF_IN_PORT = 1;

#define DTAPI_VIDSTD_AUTO  -2   // Extra helper for auto detection of video standard
static const int  DEF_IN_VIDSTD = DTAPI_VIDSTD_AUTO;
//static const MxAvRecorderDemo::AudioMode DEF_AUDIO_MODE = MxAvRecorderDemo::AUDMODE_CHANNEL_32B;

static const int IN_ROW=0;
static const int MAX_NUM_FRAMES = 16;
static const int VIDEO_MAX_NUM_STREAMS = 1;
static const int VIDEO_BUF_SIZE = 8*1024*1024;
static const int VIDEO_NUM_BUFFERS = VIDEO_MAX_NUM_STREAMS * MAX_NUM_FRAMES;
static const int AUDIO_MAX_NUM_STREAMS = 16;
static const int AUDIO_BUF_SIZE = 32*1024;
static const int AUDIO_NUM_BUFFERS = AUDIO_MAX_NUM_STREAMS * MAX_NUM_FRAMES;

MxAvRecorderDemo::MxAvRecorderDemo()
{
    // Create av-stream descriptors
    for (int  i=0; i<VIDEO_MAX_NUM_STREAMS; i++)
        m_VidStreams.push_back(new AvStream(AvStream::VIDEO_STREAM));
    for (int  i=0; i<AUDIO_MAX_NUM_STREAMS; i++)
        m_AudStreams.push_back(new AvStream(AvStream::AUDIO_STREAM, i));

    // Create av-stream-buffers
    for (int  i=0; i<VIDEO_NUM_BUFFERS; i++)
    {
        AvStreamBuf  AvBuf;
        AvBuf.Alloc(VIDEO_BUF_SIZE);
        m_VidBuffers.push_back(AvBuf);
    }
    for (int  i=0; i<AUDIO_NUM_BUFFERS; i++)
    {
        AvStreamBuf  AvBuf;
        AvBuf.Alloc(AUDIO_BUF_SIZE);
        m_AudBuffers.push_back(AvBuf);
    }
}

MxAvRecorderDemo::~MxAvRecorderDemo()
{
    Stop();

    // Clean-up av-stream descriptors
    for (int  i=0; i<(int)m_VidStreams.size(); i++)
        delete m_VidStreams[i];
    for (int  i=0; i<(int)m_AudStreams.size(); i++)
        delete m_AudStreams[i];

    // Clean-up av-stream-buffers
    AvStreamBufList::iterator  it = m_VidBuffers.begin();
    for ( ; it!=m_VidBuffers.end(); it++)
        it->Free();
    it = m_AudBuffers.begin();
    for ( ; it!=m_AudBuffers.end(); it++)
        it->Free();
}

bool MxAvRecorderDemo::Detect(DtDevice& TheCard)
{
    if (!TheCard.IsAttached())
        return false;

    if (!PrepCard(TheCard))
        return false;

    return true;
}

bool MxAvRecorderDemo::Init(DtDevice& TheCard)
{
    DTAPI_RESULT  dr;

    //-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Prep card for matrix -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-

#if 1
    // The card must have been attached
    if (!TheCard.IsAttached())
        return false;

    if (!PrepCard(TheCard))
        return false;
#endif

    //+=+=+=+=+=+=+=+=+=+=+=+=+=+=+ Configure the input row +=+=+=+=+=+=+=+=+=+=+=+=+=+=+=

    // Step 1: Attach row to the input
    DtMxPort  InPort;
    dr = InPort.AddPhysicalPort(&TheCard, 1);
    if (dr != DTAPI_OK)
        MX_THROW_EXC(dr, "Failed to add port %d as physical in-port", 1);

    dr = m_TheMatrix.AttachRowToInput(IN_ROW, InPort);
    if (dr != DTAPI_OK)
        MX_THROW_EXC(dr, "Failed to attach row %d to in-port %d", IN_ROW, 1);

    // Step 2: configure the row
    DtMxRowConfig  RowConfig;
    RowConfig.m_Enable = true;
    RowConfig.m_RowSize = 1;       // Keep a history of one frame

    // Enable the video and get scaled full picture
    RowConfig.m_VideoEnable = true;
    RowConfig.m_Video.m_PixelFormat = DT_PXFMT_V210;
    RowConfig.m_Video.m_StartLine1 = RowConfig.m_Video.m_StartLine2 = 1;
    RowConfig.m_Video.m_NumLines1 = RowConfig.m_Video.m_NumLines2 = -1;
    //RowConfig.m_Video.m_Scaling = DTAPI_SCALING_1_16;   // 1/16 scaling
    RowConfig.m_Video.m_LineAlignment = 8; // Want an 8-byte alignment
    RowConfig.m_Video.m_LineAlignment = 64; // Want an 8-byte alignment

    // Enable audio
    RowConfig.m_AudioEnable = true;

    // We want to de-embed all available audio channels
    RowConfig.m_AudioDef.m_DeEmbed = true;
    RowConfig.m_AudioDef.m_Format = DT_AUDIO_SAMPLE_PCM;  // De-embed as PCM samples
    // m_OutputMode is irrelevant for input-only rows

    // Disable aux data
    RowConfig.m_AuxDataEnable = false;

    // Apply the row configuration
    dr = m_TheMatrix.SetRowConfig(IN_ROW, RowConfig);
    if (dr != DTAPI_OK)
        MX_THROW_EXC(dr, "Failed set row %d config", IN_ROW);

    // Step 3: Final configuration

    // Register our callback
    dr = m_TheMatrix.AddMatrixCbFunc(MxAvRecorderDemo::OnNewFrame, this);
    if (dr != DTAPI_OK)
        MX_THROW_EXC(dr, "Failed register audio callback function");

    return true;
}

bool MxAvRecorderDemo::PrepCard(DtDevice& TheCard)
{
    DTAPI_RESULT  dr;
    dektec_opts_t *opts = (dektec_opts_t *)userContext;

    //-.-.-.-.-.-.-.-.-.-.-.-.-.- Configure port io-direction -.-.-.-.-.-.-.-.-.-.-.-.-.-.

    DtIoConfig  Cfg;

    Cfg.m_Port = 1;
    Cfg.m_Group = DTAPI_IOCONFIG_IODIR;
    Cfg.m_Value = DTAPI_IOCONFIG_INPUT;
    Cfg.m_SubValue = DTAPI_IOCONFIG_INPUT;
    Cfg.m_ParXtra[0] = Cfg.m_ParXtra[1] = -1;

    dr = TheCard.SetIoConfig(&Cfg, 1);
    if (dr != DTAPI_OK)
        MX_THROW_EXC(dr, "Failed to apply IO-DIR for port");

    //-.-.-.-.-.-.-.-.-.-.-.-.-.- Configure port io-standard -.-.-.-.-.-.-.-.-.-.-.-.-.-.

    // Should we auto detect?
    int  VidStd=DTAPI_VIDSTD_AUTO, IoStdValue=-1, IoStdSubValue=-1;

    if (VidStd == DTAPI_VIDSTD_AUTO)
    {
        do
        {
            printf(MODULE_PREFIX "Detecting\n");
            IoStdValue = -1, IoStdSubValue = -1;
            dr = TheCard.DetectIoStd(1, IoStdValue, IoStdSubValue);
            usleep(10);
        }
        while (dr != DTAPI_OK);

        ::DtapiIoStd2VidStd(IoStdValue, IoStdSubValue, VidStd);
        opts->detectedVideoStandard = VidStd;
        printf(MODULE_PREFIX "Detected video standard = %s (type 0x%x)\n", ::DtapiVidStd2Str(VidStd), opts->detectedVideoStandard);

    }

    dr = ::DtapiVidStd2IoStd(VidStd, IoStdValue, IoStdSubValue);
    if (dr != DTAPI_OK)
        MX_THROW_EXC(dr, "Failed to deduce IO-STD from video and link standard");

    Cfg.m_Port = 1;
    Cfg.m_Group = DTAPI_IOCONFIG_IOSTD;
    Cfg.m_Value = IoStdValue;
    Cfg.m_SubValue = IoStdSubValue;
    Cfg.m_ParXtra[0] = Cfg.m_ParXtra[1] = -1;

    dr = TheCard.SetIoConfig(&Cfg, 1);
    if (dr != DTAPI_OK)
        MX_THROW_EXC(dr, "Failed to apply IO-STD for port");

    return (dr == DTAPI_OK);
}

bool MxAvRecorderDemo::Start()
{
	AvStreamBufList::iterator  it = m_VidBuffers.begin();
	for ( ; it!=m_VidBuffers.end(); it++)
		it->Clear();

	it = m_AudBuffers.begin();
	for ( ; it!=m_AudBuffers.end(); it++)
		it->Clear();

	return MxDemoMatrixBase::Start();
}

void  MxAvRecorderDemo::Stop()
{
	MxDemoMatrixBase::Stop();
}

//-.-.-.-.-.-.-.-.-.-.-.-.-.-.- MxAvRecorderDemo::OnNewFrame -.-.-.-.-.-.-.-.-.-.-.-.-.-.-
//
void  MxAvRecorderDemo::OnNewFrame(DtMxData* pData, void* pOpaque)
{
	((MxAvRecorderDemo*)pOpaque)->OnNewFrame(pData);
}

void MxAvRecorderDemo::OnNewFrame(DtMxData* pData)
{
	dektec_opts_t *opts = (dektec_opts_t *)userContext;
	//dektec_ctx_t *ctx = &opts->ctx;

	// Get the frame data and check it is valid
	DtMxFrame*  pTheFrame = pData->m_Rows[IN_ROW].m_CurFrame;
	if (pTheFrame->m_Status != DT_FRMSTATUS_OK) {
		printf("[Frame %lld] frame status (%d) is not ok => skipping frame\n",
			pData->m_Frame, pTheFrame->m_Status);
		return;
	}

	// Get properties for video standard
	DtVidStdInfo VidInfo;
	DTAPI_RESULT dr = ::DtapiGetVidStdInfo(pTheFrame->m_VidStd, VidInfo);

	if (pTheFrame->m_VideoValid) {
#if 0
		printf("%s() got a video frame %" PRIi64 " num_lines %d stride %d\n",
			__func__,
			pTheFrame->m_RawTimestamp,
			pTheFrame->m_Video[0].m_Planes[0].m_NumLines,
			pTheFrame->m_Video[0].m_Planes[0].m_Stride);
#endif

		deliver_video_frame(opts,
			pTheFrame->m_Video[0].m_Planes[0].m_pBuf,
			pTheFrame->m_Video[0].m_Planes[0].m_BufSize,
			pTheFrame->m_Video[0].m_Planes[0].m_Stride);

	}

	if (pTheFrame->m_AudioValid) {

		/* Audio is delivered as 8 pairs (interleaved). Each time we call GetAudio, we
		 * get the audio for a specific pair as a series of LEFT.RIGHT.LEFT.RIGHT 32bit
		 * samples, alligned in an array. We need to convert that left/right array
		 * into a planer S32P buffer (abuf). We do this by means of tbuf.
		 * This whole thing needs some proper technical investigation of the SDK for
		 * better alternatives.
		 */
		int SAMPLE_SIZE = 32;
		uint32_t *abuf = (uint32_t *)calloc(1, 8192 * 16 * (32 / 8));
		uint32_t *tbuf = (uint32_t *)calloc(1, 8192 * 16 * (32 / 8));
		uint32_t *l = abuf;
		uint32_t *r = abuf;
		int tbufdepth;

		DtFixedVector<DtMxAudioService>&  Services = pTheFrame->m_Audio.m_Services;
		for (int  i=0; i<(int)Services.size(); i++) {
			if (!Services[i].m_Valid) {
				/* TODO: If its NOT valid then l/r pts get messed up.
				 * so far, it's always been valid.
				 */
				continue;
			}

			tbufdepth = 8192; /* Maximum allowable samples. */

			dr = pTheFrame->m_Audio.GetAudio(Services[i], (unsigned char *)tbuf, tbufdepth, 32);
			if (dr != DTAPI_OK) {
				//printf("%d.NumValid = %d / %d\n", i, tbufdepth, tbufdepth * (SAMPLE_SIZE / 8));
				continue;
			}

			/* First time only, once we know the number of samples we're collecting per channel (hence div 2),
			 * offset the right channel pointer so all of our future increments work as desired.
			 */
			if (i == 0)
				r += (tbufdepth / 2);

			/* unpack interleaved into planer */
			uint32_t *s = tbuf;
			for (int z = 0; z < tbufdepth; z++) {
				*(l++) = *(s++); /* left */
				*(r++) = *(s++); /* right */
			}

			/* Now increment but an entire channel, so we don't overwrite good data the next time around. */
			l += (tbufdepth / 2);
			r += (tbufdepth / 2);
		}
		deliver_audio_frame(opts, (unsigned char *)abuf, tbufdepth * (16 / 2) * (SAMPLE_SIZE / 4), tbufdepth / 2, pTheFrame->m_RawTimestamp);
		free(abuf);
		free(tbuf);
        }
}

static void deliver_audio_frame(dektec_opts_t *opts, unsigned char *plane, int sizeBytes, int sampleCount, int64_t frametime)
{
	dektec_ctx_t *ctx = &opts->ctx;

#if 0
	int offset = 0;
	for (int i = offset; i < (offset + 64); i++)
		printf("%02x", plane[i]);
	printf("\n");
#endif

	//printf("%s() %d bytes numsamples %d\n", __func__, sizeBytes, sampleCount);
	obe_raw_frame_t *rf = new_raw_frame();
	if (!rf)
		return;

	rf->audio_frame.num_samples = sampleCount;
	rf->audio_frame.num_channels = 16;
	rf->audio_frame.sample_fmt = AV_SAMPLE_FMT_S32P;
	rf->audio_frame.linesize = 3328;

	if (av_samples_alloc(rf->audio_frame.audio_data, &rf->audio_frame.linesize, rf->audio_frame.num_channels,
                              rf->audio_frame.num_samples, (AVSampleFormat)rf->audio_frame.sample_fmt, 0) < 0)
	{
		fprintf(stderr, MODULE_PREFIX "av_samples_alloc failed\n");
		return;
	}

	memcpy(rf->audio_frame.audio_data[0], plane, 3328);

	avfm_init(&rf->avfm, AVFM_AUDIO_PCM);

#if 0
	avfm_set_hw_status_mask(&rf->avfm,
		decklink_ctx->isHalfDuplex ? AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_HALF :
		AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_FULL);

	//avfm_set_pts_video(&rf->avfm, videoPTS + clock_offset);
#endif

	/* Hardware clock of the dektec starts up non-zero, increments regardless,
	 * out stack assumes the clock starts at zero. Compensate by subtracting
	 * the base start value.
	 */
	static int64_t frametime_base = 0;
	if (frametime_base == 0)
		frametime_base = frametime;

	avfm_set_pts_audio(&rf->avfm, frametime - frametime_base);
	avfm_set_hw_received_time(&rf->avfm);
	//avfm_set_video_interval_clk(&rf->avfm, decklink_ctx->vframe_duration);

	rf->release_data = obe_release_audio_data;
	rf->release_frame = obe_release_frame;
	rf->input_stream_id = 1;

	if (add_to_filter_queue(ctx->h, rf) < 0 ) {
	}
}

static void deliver_video_frame(dektec_opts_t *opts, unsigned char *plane, int sizeBytes, int strideBytes)
{
	dektec_ctx_t *ctx = &opts->ctx;

#if 1
// MMM
	struct V210_painter_s painter;
	V210_painter_reset(&painter, plane, 1280, 720, strideBytes, 0);
	V210_painter_draw_ascii_at(&painter, 0, 2, "This is a test");
	V210_painter_draw_ascii_at(&painter, 0, 3, "And so is this 1234");
#endif
	do {
		/* Ship the payload into the OBE pipeline. */
		obe_raw_frame_t *raw_frame = new_raw_frame();
		if (!raw_frame) {
			fprintf(stderr, MODULE_PREFIX "Could not allocate raw video frame\n");
			break;
		}

		AVFrame *frame = av_frame_alloc();
		ctx->codec->width = opts->width;
		ctx->codec->height = opts->height;

		AVPacket *pkt = av_packet_alloc();
		pkt->data = plane;
		pkt->size = sizeBytes;

		int ret = avcodec_send_packet(ctx->codec, pkt);
		while (ret >= 0) {
			ret = avcodec_receive_frame(ctx->codec, frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				return;
			} else if (ret < 0) {
			}
			break;
		}

		raw_frame->release_data = obe_release_video_data;
		raw_frame->release_frame = obe_release_frame;

		memcpy(raw_frame->alloc_img.stride, frame->linesize, sizeof(raw_frame->alloc_img.stride));
		memcpy(raw_frame->alloc_img.plane, frame->data, sizeof(raw_frame->alloc_img.plane));
		av_frame_free(&frame);

		raw_frame->alloc_img.csp = ctx->codec->pix_fmt;
		const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(raw_frame->alloc_img.csp);
		raw_frame->alloc_img.planes = d->nb_components;
		raw_frame->alloc_img.width = opts->width;
		raw_frame->alloc_img.height = opts->height;
		raw_frame->alloc_img.format = opts->video_format;
		raw_frame->timebase_num = opts->timebase_num;
		raw_frame->timebase_den = opts->timebase_den;
		memcpy(&raw_frame->img, &raw_frame->alloc_img, sizeof(raw_frame->alloc_img));

		int64_t pts = av_rescale_q(ctx->v_counter++, ctx->v_timebase, (AVRational){1, OBE_CLOCK} );
		obe_clock_tick(ctx->h, pts);
		raw_frame->pts = pts;

		/* AVFM */
		avfm_init(&raw_frame->avfm, AVFM_VIDEO);
		avfm_set_hw_status_mask(&raw_frame->avfm, AVFM_HW_STATUS__BLACKMAGIC_DUPLEX_FULL);

		/* Remember that we drive everything in the pipeline from the audio clock. */
		avfm_set_pts_video(&raw_frame->avfm, pts);
		avfm_set_pts_audio(&raw_frame->avfm, pts);

		avfm_set_hw_received_time(&raw_frame->avfm);
		double dur = 27000000 / (opts->timebase_den / opts->timebase_num);
		avfm_set_video_interval_clk(&raw_frame->avfm, dur);
		//raw_frame->avfm.hw_audio_correction_clk = clock_offset;
			//avfm_dump(&raw_frame->avfm);

		if (add_to_filter_queue(ctx->h, raw_frame) < 0 ) {
		}

		if (frame)
			av_frame_free(&frame);

		av_packet_free(&pkt);
	} while(0);
}

static void close_device(dektec_opts_t *opts)
{
	dektec_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Closing card idx #%d\n", opts->card_idx);

	ctx->R->Stop();

	if (ctx->codec) {
		avcodec_close(ctx->codec);
		av_free(ctx->codec);
	}

	printf(MODULE_PREFIX "Closed card idx #%d\n", opts->card_idx);
}

static int open_device(dektec_opts_t *opts, int mute)
{
	printf(MODULE_PREFIX "%s()\n", __func__);

	dektec_ctx_t *ctx = &opts->ctx;

	printf(MODULE_PREFIX "Searching for device port%d\n", opts->card_idx);

	ctx->TheCard = &TheCard;
	ctx->R = new MxAvRecorderDemo();
	ctx->R->userContext = opts;

	if (mute) {
		/*
		 * Terminology: A CARD in the SDK is a physical card, so trying to
		 * attached to CARD 1 will look for card 1, not port 1 on card 0.
		 * In A system with a single dektec card, we'll always attach to card0.
		 */
		DTAPI_RESULT dr = ctx->TheCard->AttachToType(DEF_CARD_TYPE, opts->card_idx);
		if (dr != DTAPI_OK) {
			fprintf(stderr, MODULE_PREFIX "Failed to attached to DTA-%d\n", DEF_CARD_TYPE);
			return -1;
		}

		if ((ctx->TheCard->m_pHwf[DEF_IN_PORT].m_Flags & DTAPI_CAP_MATRIX2) == 0) {
			fprintf(stderr, MODULE_PREFIX "Port %d does not have the MATRIX2 capability\n", DEF_IN_PORT);
			return -1;
		}

		printf(MODULE_PREFIX "Using DTA-%d, Port=%d, Standard=auto\n", DEF_CARD_TYPE, DEF_IN_PORT);
	}

	if (!ctx->R->Detect(TheCard)) {
		fprintf(stderr, MODULE_PREFIX "No signal found\n");
		return -1;
	}

	if (!mute) {
		//.-.-.-.-.-.-.-.-.-.-.-.-.-.- Create the PIP matrix -.-.-.-.-.-.-.-.-.-.-.-.-.-.-
		// Init the matrix
		if (!ctx->R->Init(TheCard)) {
			MX_THROW_EXC_NO_ERR("Failed to init demo");
		}

		// Start the matrix
		if (!ctx->R->Start()) {
			MX_THROW_EXC_NO_ERR("Failed to start demo");
		}
	}

	/* We need to understand how much VANC we're going to be receiving. */
	const struct obe_to_dektec_video *std = lookupDektecStandard(opts->detectedVideoStandard);
	if (std == NULL) {
		fprintf(stderr, MODULE_PREFIX "No detected standard for dektect type 0x%x, aborting\n",
			opts->detectedVideoStandard);
		exit(0);
	}

	opts->width = std->width;
	opts->height = std->height;
	opts->interlaced = 0;
	opts->timebase_den = std->timebase_den;
	opts->timebase_num = std->timebase_num;
	opts->video_format = std->obe_name;

	ctx->v_timebase.den = opts->timebase_den;
	ctx->v_timebase.num = opts->timebase_num;

	fprintf(stderr, MODULE_PREFIX "Detected resolution %dx%d @ %d/%d\n",
		opts->width, opts->height,
		opts->timebase_den, opts->timebase_num);

	ctx->dec = avcodec_find_decoder(AV_CODEC_ID_V210);
	if (!ctx->dec) {
		fprintf(stderr, MODULE_PREFIX "Could not find v210 decoder\n");
	}

	ctx->codec = avcodec_alloc_context3(ctx->dec);
	if (!ctx->codec) {
		fprintf(stderr, MODULE_PREFIX "Could not allocate a codec context\n");
		return -1;
	}

	ctx->codec->get_buffer2 = obe_get_buffer2;

	if (avcodec_open2(ctx->codec, ctx->dec, NULL) < 0) {
		fprintf(stderr, MODULE_PREFIX "Could not open libavcodec\n");
	}

	return 0; /* Success */
}

/* Called from open_input() */
static void close_thread(void *handle)
{
	printf(MODULE_PREFIX "%s()\n", __func__);

	if (!handle)
		return;

	dektec_opts_t *opts = (dektec_opts_t*)handle;
	close_device(opts);
	free(opts);
}

static void *dektec_probe_stream(void *ptr)
{
	obe_input_probe_t *probe_ctx = (obe_input_probe_t*)ptr;
	obe_t *h = probe_ctx->h;
	obe_input_t *user_opts = &probe_ctx->user_opts;
	obe_device_t *device;
	obe_int_input_stream_t *streams[MAX_STREAMS];
	int num_streams = 1 + 8;

	printf(MODULE_PREFIX "%s()\n", __func__);

	dektec_ctx_t *ctx;

	dektec_opts_t *opts = (dektec_opts_t*)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to malloc opts\n");
		goto finish;
	}

	/* TODO: support multi-channel */
	opts->num_channels = 16;
	opts->card_idx = user_opts->card_idx;
	opts->video_format = user_opts->video_format;
	opts->probe = 1;

	ctx = &opts->ctx;
	ctx->h = h;

	/* Open device */
	if (open_device(opts, 1) < 0) {
		fprintf(stderr, MODULE_PREFIX "Unable to open device.\n");
		goto finish;
	}

	sleep(1);

	close_device(opts);

	opts->probe_success = 1;
	fprintf(stderr, MODULE_PREFIX "Probe success\n");

	if (!opts->probe_success) {
		fprintf(stderr, MODULE_PREFIX "No valid frames received - check connection and input format\n");
		goto finish;
	}

	/* TODO: probe for SMPTE 337M */
	/* TODO: factor some of the code below out */

	for( int i = 0; i < 9; i++ ) {

		streams[i] = (obe_int_input_stream_t*)calloc( 1, sizeof(*streams[i]) );
		if (!streams[i])
			goto finish;

		/* TODO: make it take a continuous set of stream-ids */
		pthread_mutex_lock( &h->device_list_mutex );
		streams[i]->input_stream_id = h->cur_input_stream_id++;
		pthread_mutex_unlock( &h->device_list_mutex );

		if (i == 0) {
			streams[i]->stream_type = STREAM_TYPE_VIDEO;
			streams[i]->stream_format = VIDEO_UNCOMPRESSED;
			streams[i]->width  = opts->width;
			streams[i]->height = opts->height;
			streams[i]->timebase_num = opts->timebase_num;
			streams[i]->timebase_den = opts->timebase_den;
			streams[i]->csp    = AV_PIX_FMT_YUV422P10;
			streams[i]->interlaced = opts->interlaced;
			streams[i]->tff = 1; /* NTSC is bff in baseband but coded as tff */
			streams[i]->sar_num = streams[i]->sar_den = 1; /* The user can choose this when encoding */
		}
		else if( i >= 1 ) {
			/* TODO: various v4l2 assumptions about audio being 48KHz need resolved.
         		 * Some sources could be 44.1 and this module will fall down badly.
			 */
			streams[i]->stream_type = STREAM_TYPE_AUDIO;
			streams[i]->stream_format = AUDIO_PCM;
			streams[i]->num_channels  = 2;
			streams[i]->sample_format = AV_SAMPLE_FMT_S16;
			streams[i]->sample_rate = 48000;
			streams[i]->sdi_audio_pair = i;
		}
	}

	device = new_device();
	if (!device)
		goto finish;

	device->num_input_streams = num_streams;
	memcpy(device->input_streams, streams, device->num_input_streams * sizeof(obe_int_input_stream_t**));
	device->device_type = INPUT_DEVICE_DEKTEC;
	memcpy(&device->user_opts, user_opts, sizeof(*user_opts));

	/* add device */
	add_device(h, device);

finish:
	if (opts) {
		opts->probe = 0;
		free(opts);
	}

	free(probe_ctx);

	return NULL;
}

static void *dektec_open_input(void *ptr)
{
	obe_input_params_t *input = (obe_input_params_t*)ptr;
	obe_t *h = input->h;
	obe_device_t *device = input->device;
	obe_input_t *user_opts = &device->user_opts;
	dektec_ctx_t *ctx;

	dektec_opts_t *opts = (dektec_opts_t *)calloc(1, sizeof(*opts));
	if (!opts) {
		fprintf(stderr, MODULE_PREFIX "Unable to alloc context\n");
		return NULL;
	}

	pthread_cleanup_push(close_thread, (void *)opts);

	opts->num_channels = 16;
	opts->card_idx = user_opts->card_idx;
	opts->video_format = user_opts->video_format;

	ctx = &opts->ctx;

	ctx->device = device;
	ctx->h = h;
	ctx->v_counter = 0;

	if (open_device(opts, 0) < 0)
		return NULL;

	sleep(INT_MAX);

	pthread_cleanup_pop(1);

	return NULL;
}

const obe_input_func_t dektec_input = { dektec_probe_stream, dektec_open_input };

#endif /* #if HAVE_DTAPI_H */
