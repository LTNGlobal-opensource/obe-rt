/*****************************************************************************
 * ip.c : IP output functions
 *****************************************************************************
 * Copyright (C) 2010 Open Broadcast Systems Ltd.
 * Copyright (c) 2025 LiveTimeNet Inc.
 *
 * Large Portions of this code originate from FFmpeg
 * Authors: Kieran Kunhya <kieran@kunhya.com>
 * Authors: Steven Toth <steven.toth@ltnglobal.com>
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

#include <libavutil/random_seed.h>
#include <libavutil/intreadwrite.h>
#include <sys/time.h>
#include <srt/srt.h>
#include <libltntstools/ltntstools.h>
#include <encoders/video/sei-timestamp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "common/common.h"
#include "common/network/network.h"
#include "common/network/udp/udp.h"
#include "output/output.h"
#include "common/bitstream.h"

#define RTP_VERSION 2
#define MPEG_TS_PAYLOAD_TYPE 33
#define RTP_HEADER_SIZE 12

#define RTCP_SR_PACKET_TYPE 200
#define RTCP_PACKET_SIZE 28

#define NTP_OFFSET 2208988800ULL
#define NTP_OFFSET_US (NTP_OFFSET * 1000000ULL)

#define MAX_TS_PACKETS_SIZE (16 * 188)

extern int g_udp_output_bps;
extern void klsyslog_and_stdout(int level, const char *format, ...);
static int _srt_get_connected(hnd_t handle);
static void _srt_set_connected(hnd_t handle, int value);

typedef struct
{
    hnd_t udp_handle;

    uint16_t seq;
    uint32_t ssrc;

    uint32_t pkt_cnt;
    uint32_t octet_cnt;
} obe_rtp_ctx;

typedef struct
{
    SRTSOCKET skt;
    time_t bps_last;
    int is_connected;

    void *throughputHandle;
    struct sockaddr_in sa;

    SRT_TRACEBSTATS stats;
} obe_srt_ctx;

struct ip_status
{
    obe_output_t *output;
    hnd_t *ip_handle;
};

#if DO_SET_VARIABLE
int g_udp_output_drop_next_video_packet = 0;
int g_udp_output_drop_next_audio_packet = 0;
int g_udp_output_drop_next_pat_packet = 0;
int g_udp_output_drop_next_pmt_packet = 0;
int g_udp_output_drop_next_packet = 0;
int g_udp_output_mangle_next_pmt_packet = 0;
int g_udp_output_scramble_next_video_packet = 0;
int g_udp_output_stall_packet_ms = 0;
int g_udp_output_latency_alert_ms = 0;
int g_udp_output_tei_next_packet = 0;
int g_udp_output_bad_sync_next_packet = 0;
uint64_t g_srt_output_stats = 0;
uint64_t g_srt_packets_ps = 0;
uint64_t g_srt_packets_lost_count = 0; /* Accumator */
uint64_t g_srt_packets_retransmitted_count = 0; /* Accumator */
uint64_t g_srt_disconnect_count = 0; /* Accumator */
uint64_t g_srt_connected = 0; /* true / false */
int g_srt_latency_ms = 250;
#endif

static int _srt_open(hnd_t *p_handle, obe_udp_opts_t *udp_opts)
{
    obe_srt_ctx *p_srt = calloc(1, sizeof(*p_srt));
    if (!p_srt) {
        fprintf( stderr, "[srt] malloc failed");
        return -1;
    }

    *p_handle = p_srt;

    /* Allocate a hires throughput timer for measuring accurate bitrates.
     * 4000 writes per second as an upper ballpark, beyond this the solution
     * self adapts by allocating more slots. Calculate for 40mbps, a reasonable
     * default.
     */
    throughput_hires_alloc(&p_srt->throughputHandle, ((40 * 1e6) / 8 ) / 1316);

    srt_startup();

    p_srt->skt = srt_create_socket();
    if (p_srt->skt == SRT_INVALID_SOCK) {
        fprintf(stderr, "Error creating SRT socket: %s\n", srt_getlasterror_str());
        return -1;
    }

    if (srt_setsockopt(p_srt->skt, 0, SRTO_LATENCY, &g_srt_latency_ms, sizeof(g_srt_latency_ms)) == SRT_ERROR) {
        fprintf(stderr, "[srt] Failed to set latency to %d: %s\n", g_srt_latency_ms, srt_getlasterror_str());
        srt_close(p_srt->skt);
        return -1;
    }

    memset(&p_srt->sa, 0, sizeof(p_srt->sa));
    p_srt->sa.sin_family = AF_INET;
    p_srt->sa.sin_port = htons(udp_opts->port);
    if (inet_pton(AF_INET, udp_opts->hostname, &p_srt->sa.sin_addr) != 1) {
        perror("inet_pton");
        return -1;
    }

    if (srt_connect(p_srt->skt, (struct sockaddr*)&p_srt->sa, sizeof(p_srt->sa)) == SRT_ERROR) {
        fprintf(stderr, "[srt] failed to connect: %s\n", srt_getlasterror_str());
        return 0; /* Success, weird, I know, we'll try again later */
    }

    char t[64];
    time_t now = time(NULL);
    sprintf(t, "%s", ctime(&now));
    t[ strlen(t) - 1] = 0;
    klsyslog_and_stdout(LOG_ERR, "[srt] SRT connection opened to %s:%d @ %s\n",
        udp_opts->hostname, udp_opts->port, t);

    _srt_set_connected(p_srt, 1);

    return 0;
}

static int rtp_open( hnd_t *p_handle, obe_udp_opts_t *udp_opts )
{
    obe_rtp_ctx *p_rtp = calloc( 1, sizeof(*p_rtp) );
    if( !p_rtp )
    {
        fprintf( stderr, "[rtp] malloc failed" );
        return -1;
    }

    if( udp_open( &p_rtp->udp_handle, udp_opts ) < 0 )
    {
        fprintf( stderr, "[rtp] Could not create udp output" );
        return -1;
    }

    p_rtp->ssrc = av_get_random_seed();

    *p_handle = p_rtp;

    return 0;
}
#if 0
static int64_t obe_gettime(void)
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static uint64_t obe_ntp_time(void)
{
  return (obe_gettime() / 1000) * 1000 + NTP_OFFSET_US;
}

static int write_rtcp_pkt( hnd_t handle )
{
    obe_rtp_ctx *p_rtp = handle;
    uint64_t ntp_time = obe_ntp_time();
    uint8_t pkt[100];
    bs_t s;
    bs_init( &s, pkt, RTCP_PACKET_SIZE );

    bs_write( &s, 2, RTP_VERSION ); // version
    bs_write1( &s, 0 );             // padding
    bs_write( &s, 5, 0 );           // reception report count
    bs_write( &s, 8, RTCP_SR_PACKET_TYPE ); // packet type
    bs_write( &s, 8, 6 );           // length (length in words - 1)
    bs_write32( &s, p_rtp->ssrc );  // ssrc
    bs_write32( &s, ntp_time / 1000000 ); // NTP timestamp, most significant word
    bs_write32( &s, ((ntp_time % 1000000) << 32) / 1000000 ); // NTP timestamp, least significant word
    bs_write32( &s, 0 );            // RTP timestamp FIXME
    bs_write32( &s, p_rtp->pkt_cnt ); // sender's packet count
    bs_write32( &s, p_rtp->octet_cnt ); // sender's octet count
    bs_flush( &s );

    if( udp_write( p_rtp->udp_handle, pkt, RTCP_PACKET_SIZE ) < 0 )
        return -1;

    return 0;
}
#endif
static int write_rtp_pkt( hnd_t handle, uint8_t *data, int len, int64_t timestamp )
{
    obe_rtp_ctx *p_rtp = handle;
    uint8_t pkt[RTP_HEADER_SIZE + MAX_TS_PACKETS_SIZE];
    bs_t s;
    bs_init( &s, pkt, RTP_HEADER_SIZE + MAX_TS_PACKETS_SIZE );

    bs_write( &s, 2, RTP_VERSION ); // version
    bs_write1( &s, 0 );             // padding
    bs_write1( &s, 0 );             // extension
    bs_write( &s, 4, 0 );           // CSRC count
    bs_write1( &s, 0 );             // marker
    bs_write( &s, 7, MPEG_TS_PAYLOAD_TYPE ); // payload type
    bs_write( &s, 16, p_rtp->seq++ ); // sequence number
    bs_write32( &s, timestamp / 300 ); // timestamp
    bs_write32( &s, p_rtp->ssrc );    // ssrc
    bs_flush( &s );

    memcpy( &pkt[RTP_HEADER_SIZE], data, len );

    if( udp_write( p_rtp->udp_handle, pkt, RTP_HEADER_SIZE + obe_core_get_payload_size() ) < 0 )
        return -1;

    p_rtp->pkt_cnt++;
    p_rtp->octet_cnt += len;

    return 0;
}

static void rtp_close( hnd_t handle )
{
    obe_rtp_ctx *p_rtp = handle;

    udp_close( p_rtp->udp_handle );
    free( p_rtp );
}

int _srt_write(hnd_t handle, uint8_t *buf, int size)
{
    obe_srt_ctx *s = handle;
    int ret;

    /* Measure throughput in bits per second. Store this bitrate with now (NULL). */
    throughput_hires_write_i64(s->throughputHandle, 0, size * 8, NULL);

    /* If its been a second size we last ran the bitrate calculate, run it again. */
    time_t now;
    time(&now);
    if (now != s->bps_last) {
        s->bps_last = now;

        g_udp_output_bps = throughput_hires_sumtotal_i64(s->throughputHandle, 0, NULL, NULL);

        /* Purge data older than 2 seconds */
        throughput_hires_expire(s->throughputHandle, NULL);
    }

    extern int g_sei_timestamping;
    if (g_sei_timestamping > 1) {
        while (size == 1316) {
            //printf("%s() %d bytes\n", __func__, size);
            int offset = ltn_uuid_find(buf, 1316);
            if (offset < 0)
                break;

            struct timeval now;
            gettimeofday(&now, NULL);

            if (((offset % 188) + SEI_TIMESTAMP_PAYLOAD_LENGTH) < 188) {
                /* Ensure we don't pass over the end of the TS packet into the beginning of
                    * the next packet.
                    */
                if (sei_timestamp_field_set(buf + offset, size - offset, 8, now.tv_sec) >= 0) {
                    sei_timestamp_field_set(buf +  offset, size - offset, 9, now.tv_usec);
                }
            }

            if (g_sei_timestamping > 2) {
                sei_timestamp_hexdump(buf + offset, size - offset);
            }
            break;
        }
    } /* (g_sei_timestamping) */

    ret = srt_send(s->skt, (const char *)buf, size);
    if (ret == SRT_ERROR) {
        return -1;
    }

    return size;
}

static void _srt_stats(hnd_t handle)
{
    obe_srt_ctx *p_srt = handle;

    uint64_t old_pktSndLoss = p_srt->stats.pktSndLoss;
    uint64_t old_pktRetrans = p_srt->stats.pktRetrans;

    memset(&p_srt->stats, 0, sizeof(p_srt->stats));

    srt_bstats(p_srt->skt, &p_srt->stats, 1); // Reset stats on each call

    double bitrate = (double)g_udp_output_bps;
    bitrate /= 1000000;

#if 0
    /* Control test */
    FILE *fh = fopen("/tmp/rloss.dat", "rb");
    if (fh) {
        fclose(fh);
        p_srt->stats.pktRetrans += (rand() % 16);
    }
    fh = fopen("/tmp/ploss.dat", "rb");
    if (fh) {
        fclose(fh);
        p_srt->stats.pktSndLoss += (rand() % 16);
    }
#endif

    int flag_lost_packets = 0;
    int flag_retrans_packets = 0;

    if (p_srt->stats.pktSndLoss && p_srt->stats.pktSndLoss != old_pktSndLoss) {
        flag_lost_packets = 1;
    }
    if (p_srt->stats.pktRetrans && p_srt->stats.pktRetrans != old_pktRetrans) {
        flag_retrans_packets = 1;
    }

    g_srt_packets_ps = p_srt->stats.pktSent;
    g_srt_packets_lost_count += p_srt->stats.pktSndLoss;
    g_srt_packets_retransmitted_count += p_srt->stats.pktRetrans;
    g_srt_connected = _srt_get_connected(p_srt);

    if (flag_lost_packets) {
        klsyslog_and_stdout(LOG_ERR, "[srt] Lost packets changed, now %" PRIi64 "\n",
            g_srt_packets_lost_count + p_srt->stats.pktSndLoss);
    }
    if (flag_retrans_packets) {
        klsyslog_and_stdout(LOG_ERR, "[srt] Retrans packets changed, now %" PRIi64 "\n",
            g_srt_packets_retransmitted_count + p_srt->stats.pktRetrans);
    }

    if (g_srt_output_stats) {
        printf("[srt] Sent: %" PRId64 " pkts, Lost: %" PRId32 ", Retrans: %" PRId32 ", Bitrate: %.2f Mbps\n",
            p_srt->stats.pktSent,
            p_srt->stats.pktSndLoss,
            p_srt->stats.pktRetrans,
            bitrate);
    }

}

static int _srt_get_connected(hnd_t handle)
{
    obe_srt_ctx *p_srt = handle;
    return p_srt->is_connected;
}

static void _srt_set_connected(hnd_t handle, int value)
{
    obe_srt_ctx *p_srt = handle;

    /* Moving from connected to disconnected */
    if (p_srt->is_connected && value == 0) {
        g_srt_disconnect_count++;
    }

    p_srt->is_connected = value;
}

static int _srt_reopen(hnd_t handle, obe_udp_opts_t *udp_opts)
{
    obe_srt_ctx *p_srt = handle;

    char t[64];
    time_t now = time(NULL);
    sprintf(t, "%s", ctime(&now));
    t[ strlen(t) - 1] = 0;
    klsyslog_and_stdout(LOG_ERR, "[srt] SRT connection lost @ %s\n", t);

    _srt_set_connected(p_srt, 0);

    /* Re-establish connection */
    srt_close(p_srt->skt);

    p_srt->skt = srt_create_socket();
    if (p_srt->skt == SRT_INVALID_SOCK) {
        fprintf(stderr, "Error creating SRT socket: %s\n", srt_getlasterror_str());
        return -1;
    }

    if (srt_setsockopt(p_srt->skt, 0, SRTO_LATENCY, &g_srt_latency_ms, sizeof(g_srt_latency_ms)) == SRT_ERROR) {
        fprintf(stderr, "[srt] Failed to set latency to %d: %s\n", g_srt_latency_ms, srt_getlasterror_str());
        srt_close(p_srt->skt);
        return -1;
    }

    memset(&p_srt->sa, 0, sizeof(p_srt->sa));
    p_srt->sa.sin_family = AF_INET;
    p_srt->sa.sin_port = htons(udp_opts->port);
    if (inet_pton(AF_INET, udp_opts->hostname, &p_srt->sa.sin_addr) != 1) {
        perror("inet_pton");
        return -1;
    }

    klsyslog_and_stdout(LOG_ERR, "[srt] SRT attempting reconnect @ %s\n", t);

    // Connect to the destination (caller mode)
    if (srt_connect(p_srt->skt, (struct sockaddr*)&p_srt->sa, sizeof(p_srt->sa)) == SRT_ERROR) {
        fprintf(stderr, "[srt] error: %s\n", srt_getlasterror_str());
        srt_close(p_srt->skt);
        return -1;
    }

    klsyslog_and_stdout(LOG_ERR, "[srt] Success re-established, opened SRT connection to %s:%d @ %s\n",
        udp_opts->hostname, udp_opts->port, t);

    _srt_set_connected(p_srt, 1);

    return 0; /* Success */
}

static void _srt_close(hnd_t handle)
{
    obe_srt_ctx *p_srt = handle;

    srt_close(p_srt->skt);
    srt_cleanup();
    free(p_srt);
}

static void close_output( void *handle )
{
    struct ip_status *status = handle;

    if( status->output->output_dest.type == OUTPUT_RTP )
    {
        if( *status->ip_handle )
            rtp_close( *status->ip_handle );
    }
    else
    if (status->output->output_dest.type == OUTPUT_SRT)
    {
        if (*status->ip_handle) {
            _srt_close(*status->ip_handle);
        }
    }
    else
    {
        if( *status->ip_handle )
            udp_close( *status->ip_handle );
    }
    if( status->output->output_dest.target  )
        free( status->output->output_dest.target );

    pthread_mutex_unlock( &status->output->queue.mutex );
}

static void *open_output( void *ptr )
{
    obe_output_t *output = ptr;
    obe_output_dest_t *output_dest = &output->output_dest;
    struct ip_status status;
    hnd_t ip_handle = NULL;
    int num_muxed_data = 0;
    AVBufferRef **muxed_data;
    obe_udp_opts_t udp_opts;

    struct sched_param param = {0};
    param.sched_priority = 99;
    pthread_setschedparam( pthread_self(), SCHED_FIFO, &param );

    status.output = output;
    status.ip_handle = &ip_handle;
    pthread_cleanup_push( close_output, (void*)&status );

    udp_populate_opts( &udp_opts, output_dest->target );

    if( output_dest->type == OUTPUT_RTP )
    {
        if( rtp_open( &ip_handle, &udp_opts ) < 0 )
            return NULL;
    }
    else
    if (output_dest->type == OUTPUT_SRT)
    {
        if (_srt_open(&ip_handle, &udp_opts) < 0) {
            klsyslog_and_stdout(LOG_ERR, "[srt] SRT connected failed to %s:%d, will try again momentarily\n",
                udp_opts.hostname, udp_opts.port);
        }
    }
    else
    {
        if( udp_open( &ip_handle, &udp_opts ) < 0 )
        {
            fprintf( stderr, "[udp] Could not create udp output" );
            return NULL;
        }
    }

    while( 1 )
    {
        pthread_mutex_lock( &output->queue.mutex );
        while( !output->queue.size && !output->cancel_thread )
        {
            /* Often this cond_wait is not because of an underflow */
            pthread_cond_wait( &output->queue.in_cv, &output->queue.mutex );
        }

        if( output->cancel_thread )
        {
            pthread_mutex_unlock( &output->queue.mutex );
            break;
        }

        if (output_dest->type == OUTPUT_SRT) {
            static time_t lastReport = 0;
            time_t now = time(NULL);

            if (lastReport != now) {
                lastReport = now;
                _srt_stats(ip_handle);
            }
        }

        num_muxed_data = output->queue.size;

        muxed_data = malloc( num_muxed_data * sizeof(*muxed_data) );
        if( !muxed_data )
        {
            pthread_mutex_unlock( &output->queue.mutex );
            syslog( LOG_ERR, "Malloc failed\n" );
            return NULL;
        }
        memcpy( muxed_data, output->queue.queue, num_muxed_data * sizeof(*muxed_data) );
        pthread_mutex_unlock( &output->queue.mutex );

        if (output_dest->type == OUTPUT_SRT) {

            if (_srt_get_connected(ip_handle) == 0) {

                /* If our SRT connection is closed, we don't want to burst payload
                 * into the SRT connection once it opens again, so drop all packets.
                 * Also, if the connection is closed for hours, this avoids a potential
                 * massive memory over-allocation.
                 */
                num_muxed_data = output->queue.size;
                for (int i = 0; i < num_muxed_data; i++) {
                    remove_from_queue(&output->queue);
                    av_buffer_unref(&muxed_data[i]);
                }

                /* Every second,l attempt a reconnect */
                /* Try and re-open the connection every 1 second */
                time_t now = time(NULL);
                static time_t lastAttempt = 0;
                if (now != lastAttempt) {
                    lastAttempt = now;

                    /* Re-establish connection */
                    _srt_reopen(ip_handle, &udp_opts);

                }

                continue;
            }
        }

//        printf("\n START %i \n", num_muxed_data );

        for( int i = 0; i < num_muxed_data; i++ )
        {
            if (g_udp_output_latency_alert_ms) {
                static struct timeval lastPacketTime;
                struct timeval now, diff;
                gettimeofday(&now, NULL);
                obe_timeval_subtract(&diff, &now, &lastPacketTime);
                int64_t ms = obe_timediff_to_msecs(&diff);
                if (ms >= g_udp_output_latency_alert_ms) {
                    printf("udp inter-packet delay was %" PRIi64 "ms, too long.\n", ms);
                }
                lastPacketTime = now;
            }
            if (g_udp_output_stall_packet_ms) {
                printf("Stalling output pipeline for %d ms\n", g_udp_output_stall_packet_ms);
                usleep(g_udp_output_stall_packet_ms * 1000);
                g_udp_output_stall_packet_ms = 0;
            }

            if (g_udp_output_drop_next_packet) {
                printf("Dropping packet %d\n", g_udp_output_drop_next_packet);
                g_udp_output_drop_next_packet--;
                remove_from_queue( &output->queue );
                av_buffer_unref( &muxed_data[i] );
                continue;
            }
            if (g_udp_output_bad_sync_next_packet) {
                unsigned char *p = &muxed_data[i]->data[7*sizeof(int64_t)];
                for (int j = 0; j < 7; j++) {
                    unsigned char *q = p + (j * 188);
                    printf("Setting bad sync on packet %d\n", g_udp_output_bad_sync_next_packet--);
                    /* Mangle the header, flip the pid so the decoder can't decode it. */
                    *(q + 0) = 0x46;
                    if (g_udp_output_bad_sync_next_packet <= 0) {
                        g_udp_output_bad_sync_next_packet = 0;
                        break;
                    }
                }
            }
            if (g_udp_output_tei_next_packet) {
                unsigned char *p = &muxed_data[i]->data[7*sizeof(int64_t)];
                for (int j = 0; j < 7; j++) {
                    unsigned char *q = p + (j * 188);
                    printf("Setting TEI on packet %d\n", g_udp_output_tei_next_packet--);
                    /* Mangle the header, flip the pid so the decoder can't decode it. */
                    *(q + 1) |= 0x80;
                    if (g_udp_output_tei_next_packet <= 0) {
                        g_udp_output_tei_next_packet = 0;
                        break;
                    }
                }
            }
            if (g_udp_output_drop_next_pat_packet) {
                unsigned char *p = &muxed_data[i]->data[7*sizeof(int64_t)];
                for (int j = 0; j < 7; j++) {
                    unsigned char *q = p + (j * 188);
                    int packetpid = (*(q + 1) << 8 | *(q + 2)) & 0x1fff;
                    if (packetpid == 0) {
                        printf("Dropping pat packet %d, pid = 0x%04x\n", g_udp_output_drop_next_pat_packet, packetpid);
                        /* Mangle the header, flip the pid so the decoder can't decode it. */
                        *(q + 1) |= 0xc0;
                        *(q + 2) |= 0x40;
                        g_udp_output_drop_next_pat_packet--;
                    }
                }
            }
            if (g_udp_output_drop_next_pmt_packet) {
                unsigned char *p = &muxed_data[i]->data[7*sizeof(int64_t)];
                for (int j = 0; j < 7; j++) {
                    unsigned char *q = p + (j * 188);
                    int packetpid = (*(q + 1) << 8 | *(q + 2)) & 0x1fff;
                    if (packetpid == 0x30) {
                        printf("Dropping pmt packet %d, pid = 0x%04x\n", g_udp_output_drop_next_pmt_packet, packetpid);
                        /* Mangle the header, flip the pid so the decoder can't decode it. */
                        *(q + 1) |= 0xc0;
                        *(q + 2) |= 0x40;
                        g_udp_output_drop_next_pmt_packet--;
                    }
                }
            }
            if (g_udp_output_mangle_next_pmt_packet) {
                unsigned char *p = &muxed_data[i]->data[7*sizeof(int64_t)];
                for (int j = 0; j < 7; j++) {
                    unsigned char *q = p + (j * 188);
                    int packetpid = (*(q + 1) << 8 | *(q + 2)) & 0x1fff;
                    if (packetpid == 0x30) {
                        printf("Mangle pmt packet %d, pid = 0x%04x\n", g_udp_output_mangle_next_pmt_packet--, packetpid);
                        /* Mangle the header, flip the pid so the decoder can't decode it. */
                        *(q + 9) = 0xff;
                        if (g_udp_output_mangle_next_pmt_packet <= 0) {
                            g_udp_output_mangle_next_pmt_packet = 0;
                            break;
                        }
                    }
                }
            }
            if (g_udp_output_scramble_next_video_packet) {
                unsigned char *p = &muxed_data[i]->data[7*sizeof(int64_t)];
                for (int j = 0; j < 7; j++) {
                    unsigned char *q = p + (j * 188);
                    int packetpid = (*(q + 1) << 8 | *(q + 2)) & 0x1fff;
                    if (packetpid == 0x31) {
                        printf("Scramble video packet %d, pid = 0x%04x\n", g_udp_output_scramble_next_video_packet--, packetpid);
                        /* Mangle the header, flip the pid so the decoder can't decode it. */
                        *(q + 3) |= 0xc0; /* Set scramble to not-scrambled */
                        if (g_udp_output_scramble_next_video_packet <= 0) {
                            g_udp_output_scramble_next_video_packet = 0;
                            break;
                        }
                    }
                }
            }
            if (g_udp_output_drop_next_video_packet) {
                unsigned char *p = &muxed_data[i]->data[7*sizeof(int64_t)];
                for (int j = 0; j < 7; j++) {
                    unsigned char *q = p + (j * 188);
                    int packetpid = (*(q + 1) << 8 | *(q + 2)) & 0x1fff;
                    if (packetpid == 0x31) {
                        printf("Dropping video packet %d, pid = 0x%04x\n", g_udp_output_drop_next_video_packet--, packetpid);
                        /* Mangle the header, flip the pid so the decoder can't decode it. */
                        *(q + 1) |= 0xc0;
                        *(q + 2) |= 0x40;
                        if (g_udp_output_drop_next_video_packet <= 0) {
                            g_udp_output_drop_next_video_packet = 0;
                            break;
                        }
                    }
                }
            }
            if (g_udp_output_drop_next_audio_packet) {
                unsigned char *p = &muxed_data[i]->data[7*sizeof(int64_t)];
                for (int j = 0; j < 7; j++) {
                    unsigned char *q = p + (j * 188);
                    int packetpid = (*(q + 1) << 8 | *(q + 2)) & 0x1fff;
                    if (packetpid == 0x32) {
                        printf("Dropping audio packet %d, pid = 0x%04x\n", g_udp_output_drop_next_audio_packet--, packetpid);
                        /* Mangle the header, flip the pid so the decoder can't decode it. */
                        *(q + 1) |= 0xc0;
                        *(q + 2) |= 0x40;
                        if (g_udp_output_drop_next_audio_packet <= 0) {
                            g_udp_output_drop_next_audio_packet = 0;
                            break;
                        }
                    }
                }
            }

            /* Output specific processing */
            if( output_dest->type == OUTPUT_RTP )
            {
                if( write_rtp_pkt( ip_handle, &muxed_data[i]->data[ obe_core_get_payload_packets() * sizeof(int64_t)], obe_core_get_payload_size(), AV_RN64( muxed_data[i]->data ) ) < 0 )
                    syslog( LOG_ERR, "[rtp] Failed to write RTP packet\n" );
            }
            else
            if (output_dest->type == OUTPUT_SRT)
            {
                if (_srt_write(ip_handle,
                    &muxed_data[i]->data[obe_core_get_payload_packets() * sizeof(int64_t)], obe_core_get_payload_size()) < 0)
                {
                    _srt_set_connected(ip_handle, 0);
                }
            }
            else
            {

                if( udp_write( ip_handle, &muxed_data[i]->data[obe_core_get_payload_packets() * sizeof(int64_t)], obe_core_get_payload_size() ) < 0 )
                    syslog( LOG_ERR, "[udp] Failed to write UDP packet\n" );
            }

            remove_from_queue( &output->queue );
            av_buffer_unref( &muxed_data[i] );
        }

        free( muxed_data );
        muxed_data = NULL;
    }

    pthread_cleanup_pop( 1 );

    return NULL;
}

const obe_output_func_t ip_output = { open_output };
