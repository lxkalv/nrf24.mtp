#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <zlib.h>

#include "libs/nrf24.h"
#include "libs/logger.h"
#include "libs/app_layer.h"

#ifdef _WIN32
#error "p2p_mode is Linux-only; build on Raspberry Pi / POSIX targets"
#endif

static void radio_clear_irq_flags(nrf24_t *radio) {
    if (!radio) return;

    (void)nrf24_clear_interrupts(radio);
}

static void radio_flush_tx_fifo(nrf24_t *radio) {
    if (!radio) return;

    (void)nrf24_flush_tx(radio);
}

static void radio_flush_rx_fifo(nrf24_t *radio) {
    if (!radio) return;

    (void)nrf24_flush_rx(radio);
}

static void radio_prepare_for_tx(nrf24_t *radio) {
    radio_clear_irq_flags(radio);
    radio_flush_tx_fifo(radio);
}

static void radio_prepare_for_rx(nrf24_t *radio) {
    radio_clear_irq_flags(radio);
    radio_flush_rx_fifo(radio);
}

#define DEFAULT_SPI_DEVICE "/dev/spidev0.0"
#define P2P_CHANNEL        90

static const char *get_spi_device_path(void) {
    const char *env = getenv("NRF24_SPI_DEVICE");
    
    if (env && *env) return env;
    return DEFAULT_SPI_DEVICE;
}

static unsigned data_rate_to_kbps(app_data_rate_t rate) {
    switch (rate) {
        case APP_DATA_RATE_250KBPS: return  250;
        case APP_DATA_RATE_2MBPS:   return 2000;
        case APP_DATA_RATE_1MBPS:
        default:                    return 1000;
    }
}

static int pa_level_to_dbm(app_pa_level_t level) {
    switch (level) {
        case APP_PA_LOW:  return -12;
        case APP_PA_HIGH: return - 6;
        case APP_PA_MAX:  return   0;
        case APP_PA_MIN:
        default:          return -18;
    }
}

static unsigned crc_bytes_from_cfg(app_crc_bytes_t crc_opt) {
    switch (crc_opt) {
        case APP_CRC_OFF: return 0;
        case APP_CRC_8:   return 1;
        case APP_CRC_16:
        default:          return 2;
    }
}

static int configure_radio_from_app(nrf24_t *radio, const app_config_t *cfg) {
    if (!radio) {
        errno = EINVAL;
        return -1;
    }

    const unsigned data_rate_kbps = data_rate_to_kbps(cfg ? cfg->data_rate : APP_DATA_RATE_1MBPS);
    const int       pa_dbm        = pa_level_to_dbm(cfg ? cfg->pa_level : APP_PA_MIN);
    const unsigned  crc_bytes     = crc_bytes_from_cfg(cfg ? cfg->crc_bytes : APP_CRC_16);
    const unsigned  retr_delay    = cfg ? (unsigned)cfg->retransmission_delay : 0u;
    const unsigned  retr_tries    = cfg ? (unsigned)cfg->retransmission_tries : 15u;
    const uint8_t   channel       = cfg ? (uint8_t)(cfg->channel & 0x7Fu) : (uint8_t)P2P_CHANNEL;

    if (nrf24_configure_advanced(radio, channel, data_rate_kbps, pa_dbm, crc_bytes, retr_delay, retr_tries) < 0) {
        logger_error("Failed to configure nRF24 (channel=%u, rate=%u kbps, PA=%d dBm)",
                     (unsigned)channel, data_rate_kbps, pa_dbm);
        return -1;
    }

    return 0;
}

static int maybe_verify_radio_config(const app_config_t *cfg, nrf24_t *radio, const char *phase) {
    if (!cfg || !cfg->verify_config) return 0;

    if (phase) {
        logger_info("Verifying radio configuration (phase: %s) via register readback",
                    phase);
    } else {
        logger_info("Verifying radio configuration via register readback");
    }

    if (nrf24_dump_config(radio) < 0) {
        logger_error("nrf24_dump_config failed while verifying radio config: %s",
                     strerror(errno));
        return -1;
    }
    return 0;
}

/* ---- Protocol constants ---- */
#define BURST_DATA_MAX       7905   /* bytes of DATA (excluding ChunkID) per burst */
#define CHUNK_DATA_MAX       31     /* bytes of DATA per frame (<=31 so payload<=32) */
#define MAX_PAYLOAD          32     /* max nRF24 payload size */
#define MAX_CHUNKS_PER_BURST 255    /* 7905 / 31 = 255 */
#define CHUNK_DATA_BYTES     CHUNK_DATA_MAX
#define MAX_FRAMES_PER_BURST MAX_CHUNKS_PER_BURST
#define BURST_DATA_BYTES     (CHUNK_DATA_BYTES * MAX_FRAMES_PER_BURST) /* 7905 */
#define CHECKSUM_TIMEOUT_MS  1000   /* wait up to 1 s for checksum */
#define CONTROL_TIMEOUT_MS   100    /* per-attempt timeout when sending control frames */
#define DATA_TIMEOUT_MS      20     /* per-attempt timeout when sending data frames */

#define STREAM_INFO_ECHO_WAIT_MS  500   /* TX waits up to 0.5 s for RX echo */
#define STREAM_INFO_ECHO_SEND_MS  300   /* RX retries echo for 0.3 s */

#define CHECKSUM_SIZE        8      /* 64-bit FNV-1a checksum */
/* Control message IDs */
#define MSG_INFO             0xFF
#define MSG_BURST_INFO       0xF0
#define MSG_TRANSFER_FINISH  0x0F
#define P2P_MSG_STREAM_INFO  0xE0  /* per-page layout info */
#define P2P_STREAM_INFO_SIZE 16
/* Paging */
#define P2P_NUM_PAGES        10
#define MAX_BURSTS_PER_PAGE  255   /* burst_id is 8-bit on the air */
#define MAX_PAGES            16    /* safety margin for page_finished array */

/* ---- Time helper ---- */
static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- Little-endian helpers ---- */
static void encode_u16_le(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t decode_u16_le(const uint8_t *src) {
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static void encode_u32_le(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t decode_u32_le(const uint8_t *src) {
    return (uint32_t)(src[0] |
                      (src[1] << 8) |
                      (src[2] << 16) |
                      (src[3] << 24));
}

static void encode_u64_le(uint8_t *dst, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
}
/* ---- 64-bit FNV-1a checksum ---- */

#define FNV64_OFFSET_BASIS  1469598103934665603ULL
#define FNV64_PRIME               1099511628211ULL


static void checksum_init(uint64_t *state) {
    *state = FNV64_OFFSET_BASIS;
}


static void checksum_update(uint64_t *state, const uint8_t *data, size_t len) {
    uint64_t h = *state;

    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)data[i];
        h *= FNV64_PRIME;
    }

    *state = h;
}


static void checksum_final(uint64_t state, uint8_t out[CHECKSUM_SIZE]) {
    encode_u64_le(out, state);
}


/* ---- nRF24 convenience wrappers ---- */
/* Counts every on-air attempt (including retries) into rf_bytes_total / rf_frames_total. */


static int send_with_retries(nrf24_t *radio,
                             const uint8_t *buf,
                             uint8_t len,
                             unsigned int timeout_ms,
                             const char *what,
                             uint64_t *rf_bytes_total,
                             uint64_t *rf_frames_total,
                             const app_config_t *cfg)
{
    unsigned int attempt = 0;


    for (;;) {
        /* Account RF usage for this attempt */
        if (rf_bytes_total)  *rf_bytes_total  += len;
        if (rf_frames_total) *rf_frames_total += 1;


        int ret = nrf24_send_blocking(radio, buf, len, timeout_ms);
        if (ret == 0) {
            return 0;  /* success */
        }


        if (errno != ETIMEDOUT) {
            logger_error("nrf24_send_blocking(%s) failed: %s", what, strerror(errno));
            return -1;
        }


        ++attempt;
        if (attempt == 1 || (attempt % 50) == 0) {
            logger_warn("%s: timeout (no ACK) on attempt %u for %u-byte frame",
                 what, attempt, (unsigned)len);
        }


        /* Keep retrying forever, but every so often we reconfigure the radio */
        if (attempt % 500 == 0) {
            logger_warn("%s: %u consecutive timeouts, reconfiguring radio",
                        what, attempt);
            if (configure_radio_from_app(radio, cfg) < 0) {
                logger_error("%s: failed to reconfigure radio during retries", what);
                return -1;
            }
        }
    }
}

/* Similar to send_with_retries but bounded by deadline_ms. Returns:
 *   0  -> frame delivered and ACKed
 *   1  -> deadline elapsed (treated as soft timeout)
 *  -1  -> fatal error (non-timeout failure)
 */
static int send_with_deadline(nrf24_t *radio,
                              const uint8_t *buf,
                              uint8_t len,
                              unsigned int timeout_ms,
                              unsigned int deadline_ms,
                              const char *what,
                              uint64_t *rf_bytes_total,
                              uint64_t *rf_frames_total,
                              const app_config_t *cfg)
{
    if (deadline_ms == 0) {
        deadline_ms = timeout_ms;
    }

    double deadline_start = now_seconds();
    unsigned int attempt = 0;

    while ((now_seconds() - deadline_start) * 1000.0 < (double)deadline_ms) {
        if (rf_bytes_total)  *rf_bytes_total  += len;
        if (rf_frames_total) *rf_frames_total += 1;

        int ret = nrf24_send_blocking(radio, buf, len, timeout_ms);
        if (ret == 0) {
            return 0;
        }

        if (errno != ETIMEDOUT) {
            logger_error("nrf24_send_blocking(%s) failed: %s", what, strerror(errno));
            return -1;
        }

        ++attempt;
        if (attempt == 1 || (attempt % 25) == 0) {
            logger_warn("%s: timeout (no ACK) on attempt %u within deadline", what, attempt);
        }

        if (attempt % 200 == 0) {
            logger_warn("%s: %u consecutive timeouts during bounded send, reconfiguring radio",
                        what, attempt);
            if (configure_radio_from_app(radio, cfg) < 0) {
                logger_error("%s: failed to reconfigure radio during bounded retries", what);
                return -1;
            }
        }
    }

    logger_warn("%s: deadline of %u ms elapsed after %u attempt(s)",
                what, deadline_ms, attempt);
    errno = ETIMEDOUT;
    return 1;
}

static int wait_for_stream_info_echo(nrf24_t *radio,
                                     const uint8_t expected[P2P_STREAM_INFO_SIZE],
                                     unsigned int wait_ms,
                                     uint64_t *rf_bytes_total,
                                     uint64_t *rf_frames_total)
{
    double start = now_seconds();
    while ((now_seconds() - start) * 1000.0 < (double)wait_ms) {
        uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
        uint8_t len = sizeof(buf);
        int ret = nrf24_recv_blocking(radio, buf, &len, 50);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            logger_error("P2P TX: nrf24_recv_blocking while waiting for STREAM_INFO echo failed: %s",
                         strerror(errno));
            return -1;
        }

        if (len > 0) {
            if (rf_bytes_total)  *rf_bytes_total  += len;
            if (rf_frames_total) *rf_frames_total += 1;
        }

        if (len == P2P_STREAM_INFO_SIZE &&
            buf[0] == MSG_INFO &&
            buf[1] == P2P_MSG_STREAM_INFO &&
            memcmp(buf, expected, P2P_STREAM_INFO_SIZE) == 0) {
            logger_succ("P2P TX: STREAM_INFO echo received and verified");
            return 0;
        }

        logger_warn("P2P TX: ignoring frame (len=%u) while waiting for STREAM_INFO echo", len);
    }

    return 1;
}


/* ---- RX-side in-memory burst storage (one page at a time) ---- */


typedef struct {
    unsigned frames_in_burst;
    uint8_t *frame_data[MAX_CHUNKS_PER_BURST];
    uint8_t  frame_len[MAX_CHUNKS_PER_BURST];
} Burst;


typedef struct {
    Burst  *bursts;
    size_t  count;
    size_t  capacity;
} PageStream;


/* Initialise an empty page stream */
static void page_stream_init(PageStream *ps)
{
    ps->bursts   = NULL;
    ps->count    = 0;
    ps->capacity = 0;
}


static void free_burst(Burst *b)
{
    if (!b) return;
    for (unsigned i = 0; i < b->frames_in_burst; ++i) {
        free(b->frame_data[i]);
        b->frame_data[i] = NULL;
        b->frame_len[i]  = 0;
    }
    b->frames_in_burst = 0;
}


static void page_stream_reset_contents(PageStream *ps)
{
    if (!ps || !ps->bursts) {
        return;
    }
    for (size_t i = 0; i < ps->capacity; ++i) {
        free_burst(&ps->bursts[i]);
    }
    ps->count = 0;
}


static void page_stream_free(PageStream *ps)
{
    if (!ps || !ps->bursts) {
        return;
    }
    page_stream_reset_contents(ps);
    free(ps->bursts);
    ps->bursts   = NULL;
    ps->count    = 0;
    ps->capacity = 0;
}


static int page_stream_reserve(PageStream *ps, size_t burst_capacity)
{
    if (!ps) {
        errno = EINVAL;
        return -1;
    }

    if (ps->bursts && ps->capacity == burst_capacity) {
        page_stream_reset_contents(ps);
        return 0;
    }

    page_stream_free(ps);

    ps->bursts = (Burst *)calloc(burst_capacity, sizeof(Burst));
    if (!ps->bursts) {
        return -1;
    }

    ps->capacity = burst_capacity;
    ps->count    = 0;
    return 0;
}


/* Ensure we have at least (burst_id+1) bursts allocated. */
static Burst *page_get_burst(PageStream *ps, unsigned burst_id)
{
    if (burst_id >= ps->capacity) {
        size_t new_cap = ps->capacity ? ps->capacity * 2 : 8;
        while (burst_id >= new_cap) new_cap *= 2;


        Burst *new_bursts = (Burst *)calloc(new_cap, sizeof(Burst));
        if (!new_bursts) {
            return NULL;
        }
        /* copy existing bursts */
        for (size_t i = 0; i < ps->count; ++i) {
            new_bursts[i] = ps->bursts[i];
        }
        free(ps->bursts);
        ps->bursts   = new_bursts;
        ps->capacity = new_cap;
    }


    if (burst_id >= ps->count) {
        /* initialise new bursts as empty */
        for (size_t i = ps->count; i <= burst_id; ++i) {
            ps->bursts[i].frames_in_burst = 0;
            for (unsigned j = 0; j < MAX_CHUNKS_PER_BURST; ++j) {
                ps->bursts[i].frame_data[j] = NULL;
                ps->bursts[i].frame_len[j]  = 0;
            }
        }
        ps->count = burst_id + 1;
    }


    return &ps->bursts[burst_id];
}


/* Store a fully-received burst into PageStream[burst_id]. Overwrites old one if present. */
static int store_burst(PageStream *ps,
                       unsigned burst_id,
                       unsigned frames_in_burst,
                       uint8_t current_burst[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD],
                       const uint8_t sizes[MAX_CHUNKS_PER_BURST])
{
    Burst *b = page_get_burst(ps, burst_id);
    if (!b) {
        logger_error("store_burst: out of memory for burst %u", burst_id);
        return -1;
    }


    /* free any existing contents */
    free_burst(b);


    b->frames_in_burst = frames_in_burst;


    for (unsigned i = 0; i < frames_in_burst; ++i) {
        size_t len = sizes[i];
        b->frame_data[i] = (uint8_t *)malloc(len);
        if (!b->frame_data[i]) {
            logger_error("store_burst: malloc failed for frame %u of burst %u", i, burst_id);
            b->frames_in_burst = i;
            return -1;
        }
        memcpy(b->frame_data[i], current_burst[i], len);
        b->frame_len[i] = (uint8_t)len;
    }


    return 0;
}


/* ---- Decompress a single page (PageStream) and append to fout ---- */


static int decompress_page_to_file(PageStream *ps,
                                   FILE *fout,
                                   uint64_t *compressed_total,
                                   uint64_t *uncompressed_total)
{
    if (!ps->bursts || ps->count == 0) {
        return 0; /* nothing in this page */
    }


    z_stream zs;
    memset(&zs, 0, sizeof(zs));


    int zret = inflateInit(&zs);
    if (zret != Z_OK) {
        logger_error("P2P RX: inflateInit failed (zret=%d); writing compressed page as-is", zret);


        /* Fallback: write raw compressed bytes if we cannot inflate at all */
        for (size_t bid = 0; bid < ps->count; ++bid) {
            Burst *b = &ps->bursts[bid];
            if (b->frames_in_burst == 0) continue;
            for (unsigned i = 0; i < b->frames_in_burst; ++i) {
                uint8_t *frame = b->frame_data[i];
                uint8_t  flen  = b->frame_len[i];
                if (!frame || flen <= 1) continue;
                size_t data_len = flen - 1;
                if (fwrite(frame + 1, 1, data_len, fout) != data_len) {
                    logger_error("P2P RX: fwrite failed in fallback");
                    return -1;
                }
                *compressed_total += data_len;
            }
        }
        return -1;
    }


    uint8_t outbuf[4096];
    int end_reached = 0;


    for (size_t bid = 0; bid < ps->count && !end_reached; ++bid) {
        Burst *b = &ps->bursts[bid];
        if (b->frames_in_burst == 0) continue;


        for (unsigned i = 0; i < b->frames_in_burst && !end_reached; ++i) {
            uint8_t *frame = b->frame_data[i];
            uint8_t  flen  = b->frame_len[i];
            if (!frame || flen <= 1) continue;


            const uint8_t *in     = frame + 1;   /* skip ChunkID */
            size_t         in_len = flen - 1;


            *compressed_total += in_len;


            zs.next_in  = (Bytef *)in;
            zs.avail_in = (uInt)in_len;


            while (zs.avail_in > 0) {
                zs.next_out  = outbuf;
                zs.avail_out = sizeof(outbuf);


                zret = inflate(&zs, Z_NO_FLUSH);


                if (zret == Z_STREAM_END) {
                    end_reached = 1;
                } else if (zret != Z_OK) {
                    logger_warn("P2P RX: decompression error (zret=%d) in page; partial page written", zret);
                    end_reached = 1;
                }


                size_t have = sizeof(outbuf) - zs.avail_out;
                if (have > 0) {
                    if (fwrite(outbuf, 1, have, fout) != have) {
                        logger_error("P2P RX: fwrite failed during decompression");
                        inflateEnd(&zs);
                        return -1;
                    }
                    *uncompressed_total += have;
                }


                if (zret == Z_STREAM_END ||
                    zret == Z_DATA_ERROR ||
                    zret == Z_MEM_ERROR ||
                    zret == Z_STREAM_ERROR) {
                    break;
                }
            }
        }
    }


    inflateEnd(&zs);
    return 0;
}


static void reset_page_buffer(PageStream *ps,
                              int *page_has_data,
                              unsigned *bursts_completed,
                              uint8_t burst_received[MAX_BURSTS_PER_PAGE])
{
    page_stream_reset_contents(ps);
    if (page_has_data) {
        *page_has_data = 0;
    }
    if (bursts_completed) {
        *bursts_completed = 0;
    }
    if (burst_received) {
        memset(burst_received, 0, MAX_BURSTS_PER_PAGE);
    }
}


static int prepare_rx_stream_buffers(PageStream *ps,
                                     int *page_has_data,
                                     unsigned *bursts_completed,
                                     uint8_t burst_received[MAX_BURSTS_PER_PAGE])
{
    if (page_stream_reserve(ps, MAX_BURSTS_PER_PAGE) < 0) {
        return -1;
    }

    if (page_has_data) {
        *page_has_data = 0;
    }
    if (bursts_completed) {
        *bursts_completed = 0;
    }
    if (burst_received) {
        memset(burst_received, 0, MAX_BURSTS_PER_PAGE);
    }
    return 0;
}


static void flush_active_page(uint8_t page_id,
                              int page_finished[MAX_PAGES],
                              PageStream *ps,
                              FILE *fout,
                              uint64_t *compressed_total,
                              uint64_t *uncompressed_total,
                              int *page_has_data,
                              unsigned *bursts_completed,
                              uint8_t burst_received[MAX_BURSTS_PER_PAGE])
{
    if (!page_has_data || !*page_has_data) {
        reset_page_buffer(ps, page_has_data, bursts_completed, burst_received);
        return;
    }


    if (page_id >= MAX_PAGES) {
        logger_warn("P2P RX: page ID %u exceeds MAX_PAGES=%u; discarding buffered data",
                    (unsigned)page_id, (unsigned)MAX_PAGES);
    } else if (!page_finished[page_id]) {
        unsigned stored_bursts = bursts_completed ? *bursts_completed : 0u;
        logger_succ("P2P RX: flushing buffered Page %u (%u bursts)",
                    (unsigned)page_id,
                    stored_bursts);
        (void)decompress_page_to_file(ps, fout, compressed_total, uncompressed_total);
        page_finished[page_id] = 1;
    }


    reset_page_buffer(ps, page_has_data, bursts_completed, burst_received);
}


/* ---- TX: read whole file and send in 10 compressed pages ---- */


static int run_tx(const char *spi_dev,
                  int ce_bcm,
                  const char *input_path,
                  const app_config_t *cfg)
{
    nrf24_t radio;
    nrf24_config_t radio_cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)ce_bcm
    };


    if (nrf24_init(&radio, &radio_cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    if (configure_radio_from_app(&radio, cfg) < 0) {
        logger_error("Failed to configure radio for TX");
        nrf24_deinit(&radio);
        return 1;
    }
    if (maybe_verify_radio_config(cfg, &radio, "tx-init") < 0) {
        nrf24_deinit(&radio);
        return 1;
    }
    if (nrf24_set_mode_tx(&radio) < 0) {
        logger_error("nrf24_set_mode_tx failed");
        nrf24_deinit(&radio);
        return 1;
    }


    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        logger_error("Cannot open input file '%s': %s", input_path, strerror(errno));
        nrf24_deinit(&radio);
        return 1;
    }


    if (fseek(fin, 0, SEEK_END) != 0) {
        logger_error("fseek failed on input");
        fclose(fin);
        nrf24_deinit(&radio);
        return 1;
    }
    long fsize = ftell(fin);
    if (fsize < 0) fsize = 0;
    rewind(fin);


    uint64_t orig_len = (uint64_t)fsize;
    uint8_t *orig_buf = NULL;
    if (orig_len > 0) {
        orig_buf = (uint8_t *)malloc((size_t)orig_len);
        if (!orig_buf) {
            logger_error("P2P TX: malloc(%ld) for input buffer failed", fsize);
            fclose(fin);
            nrf24_deinit(&radio);
            return 1;
        }
        size_t nread = fread(orig_buf, 1, (size_t)orig_len, fin);
        if (nread != (size_t)orig_len) {
            logger_error("P2P TX: fread() got %zu / %llu bytes", nread, (unsigned long long)orig_len);
            free(orig_buf);
            fclose(fin);
            nrf24_deinit(&radio);
            return 1;
        }
    }
    fclose(fin);


    logger_info("P2P TX: sending file '%s' (%llu bytes, split into %d pages)",
                input_path, (unsigned long long)orig_len, P2P_NUM_PAGES);


    typedef struct {
        uint8_t *comp_data;
        size_t   comp_len;
        uint64_t orig_len;
    } TxPageInfo;


    TxPageInfo pages[P2P_NUM_PAGES];
    memset(pages, 0, sizeof(pages));


    uint64_t total_compressed    = 0;
    uint64_t total_frames_planned = 0;
    uint64_t tx_rf_bytes_total   = 0;
    uint64_t tx_rf_frames_total  = 0;


    for (unsigned page_id = 0; page_id < P2P_NUM_PAGES; ++page_id) {
        uint64_t page_start = (orig_len * page_id) / P2P_NUM_PAGES;
        uint64_t page_end   = (orig_len * (page_id + 1)) / P2P_NUM_PAGES;
        if (page_start >= orig_len) {
            break;
        }
        if (page_end > orig_len) page_end = orig_len;


        uint64_t page_len = page_end - page_start;
        pages[page_id].orig_len = page_len;
        if (page_len == 0) {
            continue;
        }


        const uint8_t *page_src = orig_buf + page_start;
        uLong src_len  = (uLong)page_len;
        uLong dest_len = compressBound(src_len);
        uint8_t *comp_page = (uint8_t *)malloc(dest_len);
        if (!comp_page) {
            logger_error("P2P TX: malloc(%lu) for compressed page failed", (unsigned long)dest_len);
            goto tx_cleanup;
        }


        int zret = compress2(comp_page, &dest_len, page_src, src_len, 6);
        if (zret != Z_OK) {
            logger_error("P2P TX: compress2 failed for page %u (zret=%d)", page_id, zret);
            free(comp_page);
            goto tx_cleanup;
        }


        size_t comp_len = (size_t)dest_len;
        pages[page_id].comp_data = comp_page;
        pages[page_id].comp_len  = comp_len;


        double ratio = (page_len == 0)
            ? 0.0
            : (100.0 * (double)comp_len / (double)page_len);


        logger_info("P2P TX: Page %u compressed %llu -> %zu bytes (~%.2f%%)",
                    page_id, (unsigned long long)page_len, comp_len, ratio);


        total_compressed    += comp_len;
        total_frames_planned += (uint64_t)((comp_len + CHUNK_DATA_BYTES - 1) / CHUNK_DATA_BYTES);
    }


    if (total_compressed > UINT32_MAX || orig_len > UINT32_MAX ||
        total_frames_planned > UINT32_MAX) {
        logger_error("P2P TX: STREAM_INFO overflow (orig=%llu, comp=%llu, frames=%llu)",
                     (unsigned long long)orig_len,
                     (unsigned long long)total_compressed,
                     (unsigned long long)total_frames_planned);
        goto tx_cleanup;
    }


    uint8_t stream_info[P2P_STREAM_INFO_SIZE] = {0};
    stream_info[0] = MSG_INFO;
    stream_info[1] = P2P_MSG_STREAM_INFO;
    stream_info[2] = 1;  /* chunk IDs fit in one byte */
    stream_info[3] = 0;
    encode_u32_le(&stream_info[4],  (uint32_t)total_compressed);
    encode_u32_le(&stream_info[8],  (uint32_t)total_frames_planned);
    encode_u32_le(&stream_info[12], (uint32_t)orig_len);


    logger_info("P2P TX: STREAM_INFO -> comp=%llu B, orig=%llu B, frames=%llu",
                (unsigned long long)total_compressed,
                (unsigned long long)orig_len,
                (unsigned long long)total_frames_planned);

    int stream_info_confirmed = 0;
    while (!stream_info_confirmed) {
        if (send_with_retries(&radio,
                              stream_info,
                              sizeof(stream_info),
                              CONTROL_TIMEOUT_MS,
                              "STREAM_INFO",
                              &tx_rf_bytes_total,
                              &tx_rf_frames_total,
                              cfg) < 0) {
            logger_error("Failed to send STREAM_INFO");
            goto tx_cleanup;
        }

        radio_prepare_for_rx(&radio);
        if (nrf24_set_mode_rx(&radio) < 0) {
            logger_error("nrf24_set_mode_rx failed while waiting for STREAM_INFO echo");
            goto tx_cleanup;
        }

        int wait_rc = wait_for_stream_info_echo(&radio,
                                                stream_info,
                                                STREAM_INFO_ECHO_WAIT_MS,
                                                &tx_rf_bytes_total,
                                                &tx_rf_frames_total);

        if (wait_rc < 0) {
            goto tx_cleanup;
        }

        radio_prepare_for_tx(&radio);
        if (nrf24_set_mode_tx(&radio) < 0) {
            logger_error("nrf24_set_mode_tx failed after STREAM_INFO echo window");
            goto tx_cleanup;
        }

        if (wait_rc == 0) {
            stream_info_confirmed = 1;
        } else {
            logger_warn("P2P TX: STREAM_INFO echo window elapsed; resending STREAM_INFO");
        }
    }


    double t_start = now_seconds();


    for (unsigned page_id = 0; page_id < P2P_NUM_PAGES; ++page_id) {
        size_t      comp_len  = pages[page_id].comp_len;
        uint8_t    *comp_page = pages[page_id].comp_data;
        uint64_t    page_len  = pages[page_id].orig_len;
        if (!comp_page || comp_len == 0) {
            free(comp_page);
            pages[page_id].comp_data = NULL;
            continue;
        }


        size_t comp_pos = 0;
        uint8_t burst_id = 0;


        while (comp_pos < comp_len) {
            uint8_t burst_payloads[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD];
            uint8_t burst_sizes  [MAX_CHUNKS_PER_BURST];
            unsigned num_chunks   = 0;
            size_t   burst_data_bytes = 0;
            uint16_t burst_onair_bytes = 0;


            uint64_t chk_state;
            checksum_init(&chk_state);


            while (comp_pos < comp_len &&
                   burst_data_bytes < BURST_DATA_MAX &&
                   num_chunks < MAX_CHUNKS_PER_BURST) {


                size_t max_data  = CHUNK_DATA_BYTES;
                size_t remaining = comp_len - comp_pos;
                if (remaining < max_data) {
                    max_data = remaining;
                }


                if (burst_data_bytes + max_data > BURST_DATA_MAX) {
                    max_data = BURST_DATA_MAX - burst_data_bytes;
                }


                if (max_data == 0) {
                    break;
                }


                uint8_t chunk_data[CHUNK_DATA_BYTES];
                memcpy(chunk_data, comp_page + comp_pos, max_data);
                comp_pos += max_data;


                uint8_t chunk_id = (uint8_t)num_chunks;
                burst_payloads[num_chunks][0] = chunk_id;
                memcpy(&burst_payloads[num_chunks][1], chunk_data, max_data);


                uint8_t payload_len = (uint8_t)(1 + max_data);
                burst_sizes[num_chunks] = payload_len;
                checksum_update(&chk_state, burst_payloads[num_chunks], payload_len);


                burst_data_bytes  += max_data;
                burst_onair_bytes += payload_len;
                num_chunks++;
            }


            if (num_chunks == 0) {
                break;
            }


            uint8_t checksum_bytes[CHECKSUM_SIZE];
            checksum_final(chk_state, checksum_bytes);


            logger_info("P2P TX: Page %u, BURST %u -> %u compressed bytes in %u frames, checksum 0x%016llX",
                        page_id, burst_id, (unsigned)burst_data_bytes, num_chunks,
                        (unsigned long long)chk_state);


            int burst_done = 0;
            while (!burst_done) {
                uint8_t burst_info[6];
                burst_info[0] = MSG_INFO;
                burst_info[1] = MSG_BURST_INFO;
                burst_info[2] = (uint8_t)page_id;
                burst_info[3] = (uint8_t)burst_id;
                encode_u16_le(&burst_info[4], burst_onair_bytes);


                if (send_with_retries(&radio,
                                      burst_info,
                                      sizeof(burst_info),
                                      CONTROL_TIMEOUT_MS,
                                      "BURST_INFO",
                                      &tx_rf_bytes_total,
                                      &tx_rf_frames_total,
                                      cfg) < 0) {
                    logger_error("Failed to send BURST_INFO (page %u, burst %u)", page_id, burst_id);
                    goto tx_cleanup;
                }


                for (unsigned i = 0; i < num_chunks; ++i) {
                    if (send_with_retries(&radio,
                                          burst_payloads[i],
                                          burst_sizes[i],
                                          DATA_TIMEOUT_MS,
                                          "DATA",
                                          &tx_rf_bytes_total,
                                          &tx_rf_frames_total,
                                          cfg) < 0) {
                        logger_error("Failed to send DATA frame (page %u, burst %u)",
                                     page_id, burst_id);
                        goto tx_cleanup;
                    }
                }


                if (nrf24_set_mode_rx(&radio) < 0) {
                    logger_error("nrf24_set_mode_rx failed");
                    goto tx_cleanup;
                }


                double wait_start = now_seconds();
                int got_valid_checksum = 0;


                while (!got_valid_checksum &&
                       (now_seconds() - wait_start) * 1000.0 < CHECKSUM_TIMEOUT_MS) {


                    uint8_t buf2[NRF24_MAX_PAYLOAD_SIZE];
                    uint8_t len2 = sizeof(buf2);
                    int ret2 = nrf24_recv_blocking(&radio, buf2, &len2, 50);
                    if (ret2 < 0) {
                        if (errno == ETIMEDOUT) {
                            continue;
                        }
                        logger_error("nrf24_recv_blocking (checksum) failed: %s", strerror(errno));
                        goto tx_cleanup;
                    }


                    if (len2 != CHECKSUM_SIZE) {
                        logger_warn("P2P TX: received non-checksum frame of %u bytes while waiting", len2);
                        continue;
                    }


                    if (memcmp(buf2, checksum_bytes, CHECKSUM_SIZE) == 0) {
                        logger_succ("P2P TX: Page %u, BURST %u checksum confirmed by RX",
                                    page_id, burst_id);
                        got_valid_checksum = 1;
                    } else {
                        logger_warn("P2P TX: invalid checksum received for Page %u, BURST %u",
                                    page_id, burst_id);
                    }
                }


                if (!got_valid_checksum) {
                    logger_warn("P2P TX: checksum timeout for Page %u, Burst %u, resending",
                                page_id, burst_id);
                    if (nrf24_set_mode_tx(&radio) < 0) {
                        logger_error("nrf24_set_mode_tx failed");
                        goto tx_cleanup;
                    }
                    continue;
                }


                burst_done = 1;


                if (nrf24_set_mode_tx(&radio) < 0) {
                    logger_error("nrf24_set_mode_tx failed");
                    goto tx_cleanup;
                }
            }


            burst_id++;
        }


        free(comp_page);
        pages[page_id].comp_data = NULL;


        if (page_len > 0) {
            logger_info("P2P TX: Page %u fully transmitted", page_id);
        }
    }


    {
        uint8_t fin_msg[2] = { MSG_INFO, MSG_TRANSFER_FINISH };
        (void)send_with_retries(&radio,
                                fin_msg,
                                sizeof(fin_msg),
                                CONTROL_TIMEOUT_MS,
                                "TRANSFER_FINISH",
                                &tx_rf_bytes_total,
                                &tx_rf_frames_total,
                                cfg);
    }


    {
        double t_end = now_seconds();
        double dt = t_end - t_start;
        double user_kibps = (dt > 0.0) ? ((double)orig_len / 1024.0 / dt) : 0.0;
        double rf_kibps = (dt > 0.0) ? ((double)tx_rf_bytes_total / 1024.0 / dt) : 0.0;


        logger_succ("P2P TX: done. User: %llu bytes in %.3f s (%.1f KiB/s). "
                    "RF on-air: %llu bytes in %.3f s (%.1f KiB/s, %llu frames).",
                    (unsigned long long)orig_len,
                    dt,
                    user_kibps,
                    (unsigned long long)tx_rf_bytes_total,
                    dt,
                    rf_kibps,
                    (unsigned long long)tx_rf_frames_total);
    }


    free(orig_buf);
    nrf24_deinit(&radio);
    return 0;


tx_cleanup:
    for (unsigned i = 0; i < P2P_NUM_PAGES; ++i) {
        free(pages[i].comp_data);
        pages[i].comp_data = NULL;
    }
    free(orig_buf);
    nrf24_deinit(&radio);
    return 1;
}


/* ---- RX: receive in pages, decompress each page independently ---- */


static int run_rx(const char *spi_dev,
                  int ce_bcm,
                  const char *output_path,
                  const app_config_t *cfg)
{
    nrf24_t radio;
    nrf24_config_t radio_cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)ce_bcm
    };


    if (nrf24_init(&radio, &radio_cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    if (configure_radio_from_app(&radio, cfg) < 0) {
        logger_error("Failed to configure radio for RX");
        nrf24_deinit(&radio);
        return 1;
    }
    if (maybe_verify_radio_config(cfg, &radio, "rx-init") < 0) {
        nrf24_deinit(&radio);
        return 1;
    }
    if (nrf24_set_mode_rx(&radio) < 0) {
        logger_error("nrf24_set_mode_rx failed");
        nrf24_deinit(&radio);
        return 1;
    }


    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        logger_error("Cannot open output file '%s': %s", output_path, strerror(errno));
        nrf24_deinit(&radio);
        return 1;
    }


    PageStream stream;
    page_stream_init(&stream);


    const int listen_channel = cfg ? cfg->channel : P2P_CHANNEL;
    logger_info("P2P RX: waiting for STREAM_INFO / BURST_INFO on channel %d...",
                listen_channel);


     int transfer_finished = 0;
     int tx_started        = 0;
     double t_start        = 0.0;


    uint8_t  chunk_id_bytes           = 0;
    uint32_t expected_stream_compressed = 0;
    uint32_t expected_stream_frames     = 0;
    uint32_t expected_stream_orig       = 0;
    int      have_stream_info           = 0;
    int      stream_buffers_ready       = 0;


     /* Per-page "finished" flags: once a page is appended, we won't append again,
         but we will still answer duplicate bursts with checksums. */
     int page_finished[MAX_PAGES];
     memset(page_finished, 0, sizeof(page_finished));


     /* Per-page state (current page in progress) */
    uint8_t  active_page_id      = 0;
    int      active_page_valid   = 0;
    int      page_has_data       = 0;  /* any stored bursts? */
     uint8_t  burst_received[MAX_BURSTS_PER_PAGE];
     memset(burst_received, 0, sizeof(burst_received));
     unsigned bursts_completed    = 0;


    /* Per-burst state (for current burst) */
    int      in_burst            = 0;
    uint8_t  cur_burst_page_id   = 0;
    unsigned cur_burst_id        = 0;
    unsigned frames_in_burst     = 0;
    uint8_t  sizes[MAX_CHUNKS_PER_BURST];
    uint8_t  current_burst[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD];


    /* Throughput / counters */
    uint64_t compressed_total    = 0;
    uint64_t uncompressed_total  = 0;
    uint64_t rf_bytes_total      = 0;  /* all bytes received and sent (checksums) */
    uint64_t rf_frames_total     = 0;  /* frames RX+TX */


    while (!transfer_finished) {
        uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
        uint8_t len = sizeof(buf);


        int ret = nrf24_recv_blocking(&radio, buf, &len, 0); /* block */
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                /* with timeout=0 this should not happen, but ignore safely */
                continue;
            }
            logger_error("nrf24_recv_blocking failed (errno=%d: %s). "
                  "Stopping RX loop and assembling what we have.",
                  errno, strerror(errno));
            /* We will break and decompress what we have so far. */
            break;
        }


        if (len > 0) {
            rf_bytes_total  += len;
            rf_frames_total += 1;
        }


        if (!tx_started) {
            t_start    = now_seconds();
            tx_started = 1;
        }


        /* STREAM_INFO: one-shot global metadata */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == P2P_MSG_STREAM_INFO) {
            if (len < P2P_STREAM_INFO_SIZE) {
                logger_warn("P2P RX: malformed STREAM_INFO frame (len=%u)", len);
                continue;
            }


            uint8_t incoming_chunk_bytes = buf[2];
            uint32_t incoming_comp    = decode_u32_le(&buf[4]);
            uint32_t incoming_frames  = decode_u32_le(&buf[8]);
            uint32_t incoming_orig    = decode_u32_le(&buf[12]);


            int same_stream = have_stream_info &&
                              incoming_chunk_bytes == chunk_id_bytes &&
                              incoming_comp       == expected_stream_compressed &&
                              incoming_frames     == expected_stream_frames &&
                              incoming_orig       == expected_stream_orig;


            if (!same_stream) {
                if (active_page_valid) {
                    flush_active_page(active_page_id,
                                      page_finished,
                                      &stream,
                                      fout,
                                      &compressed_total,
                                      &uncompressed_total,
                                      &page_has_data,
                                      &bursts_completed,
                                      burst_received);
                    active_page_valid = 0;
                } else {
                    reset_page_buffer(&stream,
                                      &page_has_data,
                                      &bursts_completed,
                                      burst_received);
                }
                memset(page_finished, 0, sizeof(page_finished));
                stream_buffers_ready = 0;
            }

            if (!stream_buffers_ready) {
                if (prepare_rx_stream_buffers(&stream,
                                              &page_has_data,
                                              &bursts_completed,
                                              burst_received) < 0) {
                    logger_error("P2P RX: unable to allocate buffers for new STREAM_INFO");
                    page_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }
                stream_buffers_ready = 1;
            } else if (same_stream) {
                logger_warn("P2P RX: STREAM_INFO already prepared; replying immediately");
            }


            chunk_id_bytes             = incoming_chunk_bytes;
            expected_stream_compressed = incoming_comp;
            expected_stream_frames     = incoming_frames;
            expected_stream_orig       = incoming_orig;
            have_stream_info           = 1;


            if (chunk_id_bytes != 1) {
                logger_warn("P2P RX: unexpected chunk ID width %u (expected 1)", chunk_id_bytes);
            }


            logger_info("P2P RX: STREAM_INFO -> chunk_id_bytes=%u, comp=%u B, orig=%u B, frames=%u",
                        (unsigned)chunk_id_bytes,
                        expected_stream_compressed,
                        expected_stream_orig,
                        expected_stream_frames);

            uint8_t stream_info_reply[P2P_STREAM_INFO_SIZE];
            memcpy(stream_info_reply, buf, P2P_STREAM_INFO_SIZE);

            radio_prepare_for_tx(&radio);
            if (nrf24_set_mode_tx(&radio) < 0) {
                logger_error("nrf24_set_mode_tx failed before STREAM_INFO echo");
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            int echo_rc = send_with_deadline(&radio,
                                             stream_info_reply,
                                             (uint8_t)P2P_STREAM_INFO_SIZE,
                                             CONTROL_TIMEOUT_MS,
                                             STREAM_INFO_ECHO_SEND_MS,
                                             "STREAM_INFO_ECHO",
                                             &rf_bytes_total,
                                             &rf_frames_total,
                                             cfg);

            if (echo_rc < 0) {
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            if (echo_rc == 0) {
                logger_succ("P2P RX: STREAM_INFO echoed back to TX");
            } else {
                logger_warn("P2P RX: STREAM_INFO echo window elapsed; returning to RX");
            }

            radio_prepare_for_rx(&radio);
            if (nrf24_set_mode_rx(&radio) < 0) {
                logger_error("nrf24_set_mode_rx failed after STREAM_INFO echo");
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }
            continue;
        }


        /* BURST_INFO */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == MSG_BURST_INFO) {
            if (len < 6) {
                logger_warn("P2P RX: malformed BURST_INFO frame");
                continue;
            }


            if (!have_stream_info) {
                logger_warn("P2P RX: BURST_INFO received before STREAM_INFO, ignoring");
                continue;
            }


            uint8_t page_id  = buf[2];
            uint8_t burst_id = buf[3];
            uint16_t size_of_burst = decode_u16_le(&buf[4]);


            int is_finished_page = (page_id < MAX_PAGES && page_finished[page_id]);
            int page_changed = 0;

            if (!is_finished_page) {
                if (!active_page_valid) {
                    active_page_valid = 1;
                    active_page_id = page_id;
                    page_changed = 1;
                } else if (page_id != active_page_id) {
                    flush_active_page(active_page_id,
                                      page_finished,
                                      &stream,
                                      fout,
                                      &compressed_total,
                                      &uncompressed_total,
                                      &page_has_data,
                                      &bursts_completed,
                                      burst_received);
                    active_page_id = page_id;
                    page_changed = 1;
                }
            }

            cur_burst_page_id = page_id;
            cur_burst_id      = burst_id;


            frames_in_burst = (size_of_burst + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
            if (frames_in_burst == 0 || frames_in_burst > MAX_CHUNKS_PER_BURST) {
                logger_warn("P2P RX: invalid frames_in_burst=%u", frames_in_burst);
                continue;
            }


            uint8_t last_len = (uint8_t)(size_of_burst % MAX_PAYLOAD);
            if (last_len == 0) last_len = MAX_PAYLOAD;


            for (unsigned i = 0; i < frames_in_burst; ++i) {
                sizes[i] = (i == frames_in_burst - 1) ? last_len : MAX_PAYLOAD;
                memset(current_burst[i], 0, MAX_PAYLOAD);
            }


            logger_info("P2P RX: BURST_INFO Page=%u Burst=%u -> size %u B in %u frames",
                 (unsigned)page_id, (unsigned)burst_id,
                 (unsigned)size_of_burst, frames_in_burst);


            in_burst = 1;
            continue;
        }


        /* TRANSFER_FINISH */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == MSG_TRANSFER_FINISH) {
            logger_info("P2P RX: received TRANSFER_FINISH");


            if (active_page_valid && page_has_data) {
                flush_active_page(active_page_id,
                                  page_finished,
                                  &stream,
                                  fout,
                                  &compressed_total,
                                  &uncompressed_total,
                                  &page_has_data,
                                  &bursts_completed,
                                  burst_received);
                active_page_valid = 0;
            }


            transfer_finished = 1;
            break;
        }


        /* DATA frame */
        if (!in_burst) {
            logger_warn("P2P RX: DATA frame received before BURST_INFO, ignoring");
            continue;
        }


        if (len == 0) {
            logger_warn("P2P RX: empty DATA frame");
            continue;
        }


        uint8_t frame_id = buf[0];


        if (frame_id >= frames_in_burst) {
            logger_warn("P2P RX: invalid FrameID=%u (frames_in_burst=%u)",
                 frame_id, frames_in_burst);
            continue;
        }


        if (len != sizes[frame_id]) {
            logger_warn("P2P RX: frame len=%u does not match expected=%u for FrameID=%u",
                 len, sizes[frame_id], frame_id);
            continue;
        }


        memcpy(current_burst[frame_id], buf, len);


        /* When we receive the last frame, compute checksum, optionally store burst, send checksum */
        if (frame_id == frames_in_burst - 1) {
            /* 1) Compute checksum over the whole burst */
            uint64_t chk_state;
            checksum_init(&chk_state);
            for (unsigned i = 0; i < frames_in_burst; ++i) {
                checksum_update(&chk_state, current_burst[i], sizes[i]);
            }
            uint8_t checksum_bytes[CHECKSUM_SIZE];
            checksum_final(chk_state, checksum_bytes);


            logger_succ("P2P RX: completed BURST [P%u|B%u], checksum 0x%016llX",
                 (unsigned)cur_burst_page_id, (unsigned)cur_burst_id,
                 (unsigned long long)chk_state);


            /* 2) Store the burst in memory ONLY if this page has not been finished
             * yet. If it's a duplicate burst for an already-finished page, we don't
             * store or append data, we just send the checksum back.
             */
            int is_finished_page =
                (cur_burst_page_id < MAX_PAGES && page_finished[cur_burst_page_id]);


            if (!is_finished_page) {
                if (store_burst(&stream,
                                cur_burst_id,
                                frames_in_burst,
                                current_burst,
                                sizes) < 0) {
                    logger_error("P2P RX: failed to store burst %u", cur_burst_id);
                    page_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }


                page_has_data = 1;


                /* Track unique bursts for this page (only if it's a normal, not-yet-finished page) */
                if (cur_burst_id < MAX_BURSTS_PER_PAGE) {
                    if (!burst_received[cur_burst_id]) {
                        burst_received[cur_burst_id] = 1;
                        bursts_completed++;
                    }
                }
            } else {
                logger_info("P2P RX: duplicate BURST [P%u|B%u] for finished page, will re-send checksum only",
                     (unsigned)cur_burst_page_id, (unsigned)cur_burst_id);
            }


            /* 3) Try to send checksum back to TX (bounded time) */
            if (nrf24_set_mode_tx(&radio) < 0) {
                logger_error("nrf24_set_mode_tx failed");
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }


            const double send_deadline_ms = 500.0;  /* total window */
            double send_start = now_seconds();
            unsigned attempt = 0;
            int checksum_sent_ok = 0;


            while (!checksum_sent_ok &&
                   (now_seconds() - send_start) * 1000.0 < send_deadline_ms) {


                rf_bytes_total  += CHECKSUM_SIZE;
                rf_frames_total += 1;


                int ret2 = nrf24_send_blocking(&radio,
                                               checksum_bytes,
                                               CHECKSUM_SIZE,
                                               CONTROL_TIMEOUT_MS);


                if (ret2 == 0) {
                    checksum_sent_ok = 1;
                    break;
                }


                if (errno != ETIMEDOUT) {
                    logger_error("P2P RX: nrf24_send_blocking(CHECKSUM) failed: %s",
                          strerror(errno));
                    page_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }


                attempt++;
                if (attempt == 1 || (attempt % 50) == 0) {
                    logger_warn("CHECKSUM: timeout (no ACK) on attempt %u for %u-byte frame",
                         attempt, CHECKSUM_SIZE);
                }


                if (attempt % 200 == 0) {
                    logger_warn("P2P RX: %u checksum timeouts, reconfiguring radio", attempt);
                    if (configure_radio_from_app(&radio, cfg) == 0) {
                        (void)nrf24_set_mode_tx(&radio);
                    }
                }
            }


            if (!checksum_sent_ok) {
                logger_warn("P2P RX: checksum timeout for Page %u, BURST %u; returning to RX "
                     "(TX may resend it)",
                     (unsigned)cur_burst_page_id, (unsigned)cur_burst_id);


                in_burst = 0;


                if (nrf24_set_mode_rx(&radio) < 0) {
                    logger_error("nrf24_set_mode_rx failed");
                    page_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }
                continue;
            }


            logger_succ("P2P RX: checksum for Page %u, BURST %u sent successfully",
                 (unsigned)cur_burst_page_id, (unsigned)cur_burst_id);


            in_burst = 0;


            if (nrf24_set_mode_rx(&radio) < 0) {
                logger_error("nrf24_set_mode_rx failed");
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }


            /* 4) If this is the "active" page and we know how many bursts it
             * should have, and we've seen them all at least once, decompress
             * this page ONCE and mark it as finished.
             */
        }
    }


    /* If we exit the loop without TRANSFER_FINISH, we may still have a partial page
     * buffered. Try to decompress what we have (once).
     */
    if (active_page_valid && page_has_data) {
        logger_warn("P2P RX: transfer ended unexpectedly; flushing partial Page %u",
             (unsigned)active_page_id);
        flush_active_page(active_page_id,
                          page_finished,
                          &stream,
                          fout,
                          &compressed_total,
                          &uncompressed_total,
                          &page_has_data,
                          &bursts_completed,
                          burst_received);
        active_page_valid = 0;
    }


    double t_end = now_seconds();
    double dt    = (tx_started ? (t_end - t_start) : 0.0);


    double user_kibps = (dt > 0.0)
        ? ((double)uncompressed_total / 1024.0 / dt)
        : 0.0;


    double rf_kibps = (dt > 0.0)
        ? ((double)rf_bytes_total / 1024.0 / dt)
        : 0.0;


    logger_succ("P2P RX: done. Compressed %llu B -> %llu B uncompressed in %.3f s "
         "(%.1f KiB/s user data, %.1f KiB/s RF on-air, %llu RF frames).",
         (unsigned long long)compressed_total,
         (unsigned long long)uncompressed_total,
         dt, user_kibps, rf_kibps,
         (unsigned long long)rf_frames_total);


    page_stream_free(&stream);
    fclose(fout);
    nrf24_deinit(&radio);
    return 0;
}


/* ---- CLI ---- */


int main(int argc, char **argv)
{
    // Parse argumets from the user
    app_config_t cfg;
    if (app_parse_arguments(argc, argv, &cfg) != 0) {
        app_print_usage(argv[0]);
        return 1;
    }


    // Determine TX file path
    // NOTE: If no TX path is provided then "~/nrf24.mtp/file_to_transmit.txt" will be used
    const char *tx_path = (cfg.file_path_tx && cfg.file_path_tx[0])
                        ? cfg.file_path_tx
                        : "file_to_transmit.txt";


    // Determine RX file path
    // NOTE: If no RX path is provided then "~/nrf24.mtp/file_to_store.txt" will be used
    const char *rx_path = (cfg.file_path_rx && cfg.file_path_rx[0])
                        ? cfg.file_path_rx
                        : "file_to_store.txt";


    if (cfg.print_config) {
        app_print_config(&cfg);
    }


    /* Choose logfile name based on mode */
    char log_path[64];
    if (cfg.mode == APP_MODE_TX) {
        snprintf(log_path, sizeof(log_path), "p2p_tx.log");
    } else if (cfg.mode == APP_MODE_RX) {
        snprintf(log_path, sizeof(log_path), "p2p_rx.log");
    } else {
        snprintf(log_path, sizeof(log_path), "p2p.log");
    }


    if (logger_init(log_path) != 0) {
        logger_warn("Could not open log file '%s' (continuing without file log)", log_path);
    } else {
        logger_info("Logging to file '%s'", log_path);
    }


    const char *spi_dev = get_spi_device_path();
    int ce_bcm = cfg.ce_pin;
    logger_info("Using SPI device: %s (CE pin %d)", spi_dev, ce_bcm);


    int ret = 1;
    if (cfg.mode == APP_MODE_TX) {
        ret = run_tx(spi_dev, ce_bcm, tx_path, &cfg);
    } else if (cfg.mode == APP_MODE_RX) {
        ret = run_rx(spi_dev, ce_bcm, rx_path, &cfg);
    } else {
        logger_error("Unsupported mode: %s", app_mode_str(cfg.mode));
    }


    logger_close();
    return ret;
}
