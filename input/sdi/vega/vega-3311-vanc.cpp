#if HAVE_VEGA3311_CAP_TYPES_H

/*****************************************************************************
 * vega-vanc.cpp: VANC processing from Vega encoding cards
 *****************************************************************************
 * Copyright (C) 2023 LTN
 *
 * Authors: Steven Toth <steven.toth@ltnglobal.com>
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
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <iostream>
#include <limits.h>
#include <fcntl.h>
#include <semaphore.h>
#include <time.h>

#include "vega-3311.h"

#define LOCAL_DEBUG 1

extern "C"
{
#include <libklvanc/vanc.h>
#include <libklscte35/scte35.h>
}

#define MODULE_PREFIX "[vega-vanc]: "

typedef struct
{
    volatile uint32_t pkt_continuity_counter;
    volatile uint32_t block_number: 16;
    volatile uint32_t f_bit: 1;
    volatile uint32_t o_bit: 1;
    volatile uint32_t reserved0: 14;
    volatile uint32_t stc_lsb;
    volatile uint32_t stc_msb;
    volatile API_VEGA3311_CAP_PCR_FORMAT_T ancd_pcr;
} AncdPHD; /* 24 bytes */

typedef struct
{
    volatile uint32_t user_data0: 10;
    volatile uint32_t user_data1: 10;
    volatile uint32_t checksum: 10;
    volatile uint32_t padding2: 2;
} anc_user_data_format; /* 4 bytes */

typedef struct
{
    /* 32 bits */
    volatile uint32_t header_flag: 22;
    volatile uint32_t h_not_v_flag: 1;
    volatile uint32_t post_pre_flag: 1;
    volatile uint32_t block_index: 8;

    /* 32 bits */
    volatile uint32_t header_pattern: 6;
    volatile uint32_t c_not_y_flag: 1;
    volatile uint32_t line_number: 11;
    volatile uint32_t horizontal_offset: 12;
    volatile uint32_t padding0: 2;

    /* 32 bits */
    volatile uint32_t did: 10;
    volatile uint32_t sdid: 10;
    volatile uint32_t data_count: 10;
    volatile uint32_t padding1: 2;
} block_data_t; /* 12 bytes */

/* VANC Callbacks */
static int cb_EIA_708B(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_eia_708b_s *pkt)
{
        vega_opts_t *opts = (vega_opts_t *)callback_context;
        vega_ctx_t *ctx = &opts->ctx;

	if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		klvanc_dump_EIA_708B(vanchdl, pkt); /* vanc lib helper */
	}

        /* Make a vega full formed SEI message and queue it.
         * Convert the klvanc_packet_eia_708b_s into a vega SEI frame.
         * During the next video frame callback, it will be assied to that frame and passed to the compression codec.
         * * See https://en.wikipedia.org/wiki/CTA-708
         */
        API_VEGA_BQB_SEI_PARAM_T s;
        memset(&s, 0, sizeof(s));

        s.ePayloadLoc   = API_VEGA_BQB_SEI_PAYLOAD_LOC_PICTURE;
        s.ePayloadType  = API_VEGA_BQB_SEI_PAYLOAD_TYPE_USER_DATA_REGISTERED_ITU_T_T35;
        s.u8PayloadSize = 0; //bytecount;
        uint8_t *p = &s.u8PayloadData[0];

        /* Preamble */
        *(p++) = 0x00;
        *(p++) = 0x00;
        *(p++) = 0x01;

        if (1) {
                /* HEVC Specific. PREFIX_NAL_UNIT */
                *(p++) = 0x4e;
                *(p++) = 0x01;
                *(p++) = 0x04;
                *(p++) = 0x47; // itu_t_t35_provider_code
                *(p++) = 0xb5; // itu_t_t35_country_code 
                *(p++) = 0x00; // ?
                *(p++) = 0x31; // ? 
                *(p++) = 'G';
                *(p++) = 'A';
                *(p++) = '9';
                *(p++) = '4';
                *(p++) = 0x03; // ATSC1_data_user_data_type_code 
                *(p++) = (1 << 7 /* reserved */) |
                         (1 << 6 /* process_cc_data_flag */) | (pkt->ccdata.cc_count & 0x1f);
                *(p++) = 0xff; /* Marker */
        } else {
                /* H264 */
                return -1;
        }

        for (int i = 0; i < pkt->ccdata.cc_count; i++) {
                *(p)    = 0xf8; /* Marker bits */
                *(p)   |= (pkt->ccdata.cc[i].cc_valid << 2);
                *(p++) |= pkt->ccdata.cc[i].cc_type;
                *(p++)  = pkt->ccdata.cc[i].cc_data[0];
                *(p++)  = pkt->ccdata.cc[i].cc_data[1];
        }

        *(p++) = 0xff; /* Marker */

        /* Wrap up the struct */
        s.u8PayloadSize = p - &s.u8PayloadData[0];

        if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
                printf(MODULE_PREFIX "Adding SEI: ");
                for (int i = 0; i < s.u8PayloadSize; i++) {
                        printf("%02x ", s.u8PayloadData[i]);
                }
                printf("\n");
        }

        if (vega_sei_append(ctx, &s) < 0) {
                fprintf(stderr, MODULE_PREFIX "Unable to create new SEI 708 entry, skipping\n");
        }

	return 0;
}

static int cb_EIA_608(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_eia_608_s *pkt)
{
        vega_opts_t *opts = (vega_opts_t *)callback_context;
        vega_ctx_t *ctx = &opts->ctx;

        printf(MODULE_PREFIX "%s\n", __func__);
	if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		klvanc_dump_EIA_608(vanchdl, pkt); /* vanc library helper */
	}

	return 0;
}

static int findOutputStreamIdByFormat(vega_ctx_t *ctx, enum stream_type_e stype, enum stream_formats_e fmt)
{
	if (ctx && ctx->device == NULL)
		return -1;

	for(int i = 0; i < ctx->device->num_input_streams; i++) {
		if ((ctx->device->input_streams[i]->stream_type == stype) &&
			(ctx->device->input_streams[i]->stream_format == fmt))
			return i;
        }

	return -1;
}

static int transmit_smpte2038_pes_to_muxer(vega_ctx_t *ctx, uint8_t *buf, uint32_t byteCount)
{
	int streamId = findOutputStreamIdByFormat(ctx, STREAM_TYPE_MISC, SMPTE2038);
	if (streamId < 0)
		return 0;

	/* Now send the constructed frame to the mux */
	obe_coded_frame_t *coded_frame = new_coded_frame(streamId, byteCount);
	if (!coded_frame) {
		syslog(LOG_ERR, "Malloc failed during %s, needed %d bytes\n", __func__, byteCount);
		return -1;
	}
	coded_frame->pts = ctx->lastcorrectedPicPTS;
	coded_frame->random_access = 1; /* ? */
	memcpy(coded_frame->data, buf, byteCount);
	add_to_queue(&ctx->h->mux_queue, coded_frame);

	return 0;
}

static int transmit_scte35_section_to_muxer(vega_ctx_t *ctx, uint8_t *buf, uint32_t byteCount)
{      
        /* TODO: This only works if we have one SCTE35 stream. */
	int streamId = findOutputStreamIdByFormat(ctx, STREAM_TYPE_MISC, DVB_TABLE_SECTION);
	if (streamId < 0) {
                fprintf(stderr, MODULE_PREFIX "Unable to find SCTE35 stream\n");
		return 0;
        }

	/* Now send the constructed frame to the mux */
	obe_coded_frame_t *coded_frame = new_coded_frame(streamId, byteCount);
	if (!coded_frame) {
		syslog(LOG_ERR, "Malloc failed during %s, needed %d bytes\n", __func__, byteCount);
		return -1;
	}
	coded_frame->pts           = ctx->lastcorrectedPicPTS;
	coded_frame->real_pts      = ctx->lastcorrectedPicPTS;
	coded_frame->real_dts      = ctx->lastcorrectedPicPTS;
	coded_frame->random_access = 1; /* ? */

	memcpy(coded_frame->data, buf, byteCount);
	add_to_queue(&ctx->h->mux_queue, coded_frame);

	return 0;
}

static int cb_SCTE_104(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_scte_104_s *pkt)
{
        vega_opts_t *opts = (vega_opts_t *)callback_context;
        vega_ctx_t *ctx = &opts->ctx;

	if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s:%s()\n", __FILE__, __func__);
		klvanc_dump_SCTE_104(vanchdl, pkt); /* vanc library helper */
	}

	if (klvanc_packetType1(&pkt->hdr)) {
		/* Silently discard type 1 SCTE104 packets, as per SMPTE 291 section 6.3 */
		return 0;
	}

	struct klvanc_single_operation_message *m = &pkt->so_msg;

	if (m->opID == 0xFFFF /* Multiple Operation Message */) {
		struct splice_entries results;
		/* Take the PTS from the coded frame, output from our codec, build a SCTE35 message
		 * for that timestamp. Send it to the muxer.
		 */

                /* TODO: WE're technically guessing that the callback happened close to the time
                 * associated with the last video frame. It's going to get us close.
                 * We really need to collect the PCR and propagate it, but we can't easily do that - yet
                 */
		/* Note, we add 10 second to the PTS to compensate for TS_START added by libmpegts */
		int r = scte35_generate_from_scte104(pkt, &results, ctx->lastcorrectedPicPTS + (10 * 90000));
		if (r != 0) {
			fprintf(stderr, "Generation of SCTE-35 sections failed\n");
		}

		for (size_t i = 0; i < results.num_splices; i++) {
                        /* Send the message right to the muxer, which isn't frame accurate. */
			transmit_scte35_section_to_muxer(ctx, results.splice_entry[i],
							 results.splice_size[i]);
			free(results.splice_entry[i]);
		}
	} else {
		/* Unsupported single_operation_message type */
		fprintf(stderr, MODULE_PREFIX "Unsupported single_operation SCTE35 message type, ignoring.\n");
	}

	if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_SCTE104) {
		static time_t lastErrTime = 0;
		time_t now = time(0);
		if (lastErrTime != now) {
			lastErrTime = now;

			char t[64];
			snprintf(t, sizeof(t), "%s", ctime(&now));
			t[ strlen(t) - 1] = 0;
			syslog(LOG_INFO, MODULE_PREFIX "SCTE104 frames present on SDI");
			fprintf(stdout, MODULE_PREFIX "SCTE104 frames present on SDI  @ %s", t);
			printf("\n");
			fflush(stdout);

		}
	}

	return 0;
}

static int cb_all(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_header_s *pkt)
{
        vega_opts_t *opts = (vega_opts_t *)callback_context;
        vega_ctx_t *ctx = &opts->ctx;

	if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s()\n", __func__);
	}

        //klvanc_packet_header_dump_console(pkt); /* Local project func, see vega3311-misc.cpp */

	/* We've been called with a VANC frame. Pass it to the SMPTE2038 packetizer.
	 * We'll be called here from the thread handing the advantech VANC callback (vega3311-vanc.cpp)
	 * callback, which calls vanc_packet_parse for each ANC line.
	 * Push the pkt into the SMPTE2038 layer, its collecting VANC data.
	 */
	if (ctx->smpte2038_ctx) {
		if (klvanc_smpte2038_packetizer_append(ctx->smpte2038_ctx, pkt) < 0) {
                        fprintf(stderr, MODULE_PREFIX "Failed to add message to SMPTE2038 framework\n");
		}
	}

#if 0
/* This patch should not be required for VEGA based deployments. */
	if (OPTION_ENABLED(patch1) && ctx->vanchdl && pkt->did == 0x52 && pkt->dbnsdid == 0x01) {

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
#endif

	return 0;
}

static int cb_VANC_TYPE_KL_UINT64_COUNTER(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_kl_u64le_counter_s *pkt)
{
        //vega_opts_t *opts = (vega_opts_t *)callback_context;
        //vega_ctx_t *ctx = &opts->ctx;

        printf(MODULE_PREFIX "%s\n", __func__);

        /* Have the library display some debug */
	static uint64_t lastGoodKLFrameCounter = 0;
        if (lastGoodKLFrameCounter && lastGoodKLFrameCounter + 1 != pkt->counter) {
                char ts[64];
                obe_getTimestamp(ts, NULL);

                fprintf(stderr, MODULE_PREFIX "%s: KL VANC frame counter discontinuity was %" PRIu64 " now %" PRIu64 "\n",
                        ts,
                        lastGoodKLFrameCounter, pkt->counter);
        }
        lastGoodKLFrameCounter = pkt->counter;

        return 0;
}

static int cb_SMPTE2108_1(void *callback_context, struct klvanc_context_s *vanchdl, struct klvanc_packet_smpte_2108_1_s *pkt)
{
        vega_opts_t *opts = (vega_opts_t *)callback_context;
        vega_ctx_t *ctx = &opts->ctx;

	if (ctx->h->verbose_bitmask & INPUTSOURCE__SDI_VANC_DISCOVERY_DISPLAY) {
		printf("%s()\n", __func__);
                klvanc_dump_SMPTE_2108_1(vanchdl, pkt);
	}

        /* Create SEI frames for the 2108 frame types we understand. */
        for (int i = 0; i < pkt->num_frames; i++) {

                API_VEGA_BQB_SEI_PARAM_T s;
                memset(&s, 0, sizeof(s));

                switch(pkt->frames[i].frame_type) {
                case KLVANC_HDR_STATIC1:
                        VEGA_BQB_ENC_MakeMasteringDisplayColourVolumeSei(
                                &s,
                                pkt->frames[i].static1.display_primaries_x[0],
                                pkt->frames[i].static1.display_primaries_y[0],
                                pkt->frames[i].static1.display_primaries_x[1],
                                pkt->frames[i].static1.display_primaries_y[1],
                                pkt->frames[i].static1.display_primaries_x[2],
                                pkt->frames[i].static1.display_primaries_y[2],
                                pkt->frames[i].static1.white_point_x,
                                pkt->frames[i].static1.white_point_y,
                                pkt->frames[i].static1.max_display_mastering_luminance,
                                pkt->frames[i].static1.min_display_mastering_luminance
                        );
                        break;
                case KLVANC_HDR_STATIC2:
                        VEGA_BQB_ENC_MakeContentLightInfoSei(
                                &s,
                                API_VEGA_BQB_SEI_PAYLOAD_LOC_GOP,
                                pkt->frames[i].static2.max_content_light_level,
                                pkt->frames[i].static2.max_pic_average_light_level
                        );
                        break;
                default:
                        continue;
                }

                if (vega_sei_append(ctx, &s) < 0) {
                        fprintf(stderr, MODULE_PREFIX "Unable to create new SEI HDR entry, skipping\n");
                }

        } // for each frame...

        return 0;
}

struct klvanc_callbacks_s vega3311_vanc_callbacks = 
{
	.afd			= NULL,
	.eia_708b		= cb_EIA_708B,
	.eia_608		= cb_EIA_608,
	.scte_104		= cb_SCTE_104,
	.all			= cb_all,
	.kl_i64le_counter       = cb_VANC_TYPE_KL_UINT64_COUNTER,
	.sdp			= NULL,
        .smpte_12_2             = NULL,
        .smpte_2108_1           = cb_SMPTE2108_1,
};
/* End: VANC Callbacks */


/* Convert the Advantech oddly frameswpacket VANC into a standardized version we can 
 * push into libKL vanc, for parsing and calback handling.
 */
static void parse_ancillary_data(vega_ctx_t *ctx, vega_opts_t *opts, uint32_t block_index, uint8_t *src_buf)
{
        block_data_t *block_data_ptr = (block_data_t *)src_buf;

        /* Pointer to vanc data */
        //uint8_t *udw_ptr_last = NULL;
        uint8_t *udw_ptr = (uint8_t *)block_data_ptr + sizeof(block_data_t);
        uint8_t word10b_count;

        /* Bit shifting and unpacking table we use to walk the payload and convert into uint16_t's */
        struct state_s
        {
                int offset;
                int b1mask;
                int b1rshift;
                int b2mask;
                int b2lshift;
        } states[4] = {
            { 0, 0xff, 0, 0x03, 8, },
            { 1, 0xfc, 2, 0x0f, 6, },
            { 2, 0xf0, 4, 0x3f, 4, },
            { 3, 0xc0, 0, 0xff, 0, },
        };

        /* For every VANC messages we're passed .... */
        for (uint32_t i = 0; i < block_index; i++) {

                word10b_count = (uint8_t)block_data_ptr->data_count;

                /* Convert this specifi9c VANC message into uint16_t payload */
                int payloadIdx = 0;
                uint16_t payload[256] = {0};
                payload[payloadIdx++] = 0;
                payload[payloadIdx++] = 0x3ff;
                payload[payloadIdx++] = 0x3ff;
                payload[payloadIdx++] = (uint32_t)block_data_ptr->did;
                payload[payloadIdx++] = (uint32_t)block_data_ptr->sdid;
                payload[payloadIdx++] = (uint32_t)block_data_ptr->data_count;
                //payload[payloadIdx++] = (uint32_t)block_data_ptr->horizontal_offset;

                int cstate = 0; /* We only go through state 0 once, start here then we iterate through 1..3 after this */
                
                /* Calculate and round up number of blocks containing words */
                /* Setup a uint8_t loop, walk thr loop using the state/shift machine rules to unpack the payload */
                uint16_t a, b, c, d;
                int count = ((word10b_count / 5) + 1) * 5;
                for (int j = 0; j < (count * 10) / 8; j += 4)
                {
                        a = 0xffff;
                        if (cstate == 0)
                        {
                                a = (*(udw_ptr + j + states[cstate].offset) & states[cstate].b1mask) >> states[cstate].b1rshift;
                                a |= ((*(udw_ptr + j + states[cstate].offset + 1) & states[cstate].b2mask) << states[cstate].b2lshift);
                                payload[payloadIdx++] = a;
                                cstate++;
                        }

                        b = (*(udw_ptr + j + states[cstate].offset) & states[cstate].b1mask) >> states[cstate].b1rshift;
                        b |= ((*(udw_ptr + j + states[cstate].offset + 1) & states[cstate].b2mask) << states[cstate].b2lshift);
                        payload[payloadIdx++] = b;

                        cstate++;
                        c = (*(udw_ptr + j + states[cstate].offset) & states[cstate].b1mask) >> states[cstate].b1rshift;
                        c |= ((*(udw_ptr + j + states[cstate].offset + 1) & states[cstate].b2mask) << states[cstate].b2lshift);
                        payload[payloadIdx++] = c;

                        cstate++;
                        d = (*(udw_ptr + j + states[cstate].offset) & states[cstate].b1mask) >> states[cstate].b1rshift;
                        d |= ((*(udw_ptr + j + states[cstate].offset + 1) & states[cstate].b2mask) << states[cstate].b2lshift);
                        payload[payloadIdx++] = d;

#if 0
                        if (a != 0xffff) {
                                printf("\toffset[%3d] %04x %04x %04x %04x\n", j, a, b, c, d);
                        } else {
                                printf("\toffset[%3d]      %04x %04x %04x\n", j,    b, c, d);
                        }
#endif
                        cstate = 1;
                }
                payload[payloadIdx++] = 0xdead;

#if 0
                /* Dump the payload in 8 bit format, so we can compare klvanc in cinterface mode output */
                printf("Reconstructed VANC Payload for parsing:\n");
                for (int j = 1; j < payloadIdx + 1; j++)
                {
                        printf("%04x ", payload[j - 1]);
                        if (j % 16 == 0)
                                printf("\n");
                }
                printf("\n");
#endif

                /* Start a new 2038 session */
                /* Calls to klvanc_packet_parse are returned to 2038 via the cb_all
                 * callback. We have 2038 code in that func to add 2038 payload to the 2038 context session. */
                if (ctx->smpte2038_ctx)
                        klvanc_smpte2038_packetizer_begin(ctx->smpte2038_ctx);

                if (ctx->vanchdl) {
        		int ret = klvanc_packet_parse(ctx->vanchdl,
                                (uint32_t)block_data_ptr->line_number,
                                payload,
                                sizeof(payload) / (sizeof(unsigned short)));
        		if (ret < 0) {
                                /* No VANC on this line */
                                fprintf(stderr, MODULE_PREFIX "No VANC on this line / parse failed or construction bug?\n");
		        }
	        }

                /* Finish the 2038 session and forward 2038 to muxer */
                if (ctx->smpte2038_ctx) {
                        if (klvanc_smpte2038_packetizer_end(ctx->smpte2038_ctx, ctx->lastcorrectedPicPTS + (10 * 90000)) == 0) {
                                if (transmit_smpte2038_pes_to_muxer(ctx, ctx->smpte2038_ctx->buf, ctx->smpte2038_ctx->bufused) < 0) {
                                        fprintf(stderr, MODULE_PREFIX "failed to xmit PES to muxer\n");
                                }
                        }
                }

#if 0
                /* Dump the raw payload, for debug purposes, in its advantech packed format */
                for (int i = 0; i < word10b_count; i++)
                {
                        printf("%02x ", *(udw_ptr + i));
                }
                printf("\n");
#endif

#if 0
                if (block_data_ptr->h_not_v_flag == 0)
                {
                        printf("\tBlock idx %u: %s-V, %s, line %u, H-offset=%u, DID 0x%X, SDID 0x%X, DC 0x%X\n",
                               i,
                               (block_data_ptr->post_pre_flag == 0) ? "POST" : "PRE",
                               (block_data_ptr->c_not_y_flag == 0) ? "Y" : "C",
                               (uint32_t)block_data_ptr->line_number,
                               (uint32_t)block_data_ptr->horizontal_offset,
                               (uint32_t)block_data_ptr->did,
                               (uint32_t)block_data_ptr->sdid,
                               (uint32_t)block_data_ptr->data_count);
                }
                else
                {
                        printf("\tBlock idx %u: POST-H, %s, line %u, H-offset=%u, DID 0x%X, SDID 0x%X, DC 0x%X\n",
                               i,
                               (block_data_ptr->c_not_y_flag == 0) ? "Y" : "C",
                               (uint32_t)block_data_ptr->line_number,
                               (uint32_t)block_data_ptr->horizontal_offset,
                               (uint32_t)block_data_ptr->did,
                               (uint32_t)block_data_ptr->sdid,
                               (uint32_t)block_data_ptr->data_count);
                }
#endif
                /* setup for next block data */
                /* sizeof(block_data_t) = 3x uint32_t = 12 bytes */
                block_data_ptr = (block_data_t *)(udw_ptr + ((word10b_count + 3) / 3) * sizeof(uint32_t));
                udw_ptr = (uint8_t *)block_data_ptr + sizeof(block_data_t);
#if 0
                if (udw_ptr_last)
                {
                        printf("%ld bytes\n", udw_ptr - udw_ptr_last);
                }
                udw_ptr_last = udw_ptr;
#endif
        } /* For every VANC messages we're passed.... */
}

void vega3311_vanc_callback(uint32_t u32DevId,
        API_VEGA3311_CAP_CHN_E eCh,
        API_VEGA3311_CAPTURE_FRAME_INFO_T* st_frame_info,
        API_VEGA3311_CAPTURE_FORMAT_T* st_input_info,
        void *pv_user_arg)
{
        vega_opts_t *opts = (vega_opts_t *)pv_user_arg;
        vega_ctx_t *ctx = &opts->ctx;

        if (st_frame_info->u32BufSize == 0) {
                if (st_input_info->eAncdState == API_VEGA3311_CAP_STATE_CAPTURING) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] anc state change to capturing, source signal recovery\n", u32DevId, eCh);
                } else if (st_input_info->eAncdState == API_VEGA3311_CAP_STATE_WAITING) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] anc state change to waiting, source signal loss\n", u32DevId, eCh);
                } else if (st_input_info->eAncdState == API_VEGA3311_CAP_STATE_STANDBY) {
                        printf(MODULE_PREFIX "[DEV%u:CH%d] anc state change to standby, callback function is terminated\n", u32DevId, eCh);
                }

                return;
        }

        AncdPHD *pkt_hdr = (AncdPHD *)st_frame_info->u8pDataBuf;
#if 0
        printf(MODULE_PREFIX "anc callback PCR %15" PRIi64 " size %d bytes\n", pkt_hdr->ancd_pcr.u64Dword, st_frame_info->u32BufSize);
#if 0
        printf(MODULE_PREFIX "Entire VANC callback data:\n");
        ltntstools_hexdump(st_frame_info->u8pDataBuf, st_frame_info->u32BufSize, 32);
#endif
#endif
        uint8_t *ancd_data_block_p = st_frame_info->u8pDataBuf + sizeof(AncdPHD);
        //int ancd_data_block_size = st_frame_info->u32BufSize - sizeof(AncdPHD);



        parse_ancillary_data(ctx, opts, (uint32_t)pkt_hdr->block_number, ancd_data_block_p);


}

#endif /* #if HAVE_VEGA3311_CAP_TYPES_H */
