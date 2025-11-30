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

/* ---- Protocol constants ---- */

#define P2P_CHANNEL          90

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

#define CHECKSUM_SIZE        8      /* 64-bit FNV-1a checksum */

/* Control message IDs */
#define MSG_INFO             0xFF
#define MSG_BURST_INFO       0xF0
#define MSG_TRANSFER_FINISH  0x0F
#define P2P_MSG_STREAM_INFO  0xE0  /* per-page layout info */

/* Paging */
#define P2P_NUM_PAGES        10
#define MAX_BURSTS_PER_PAGE  255   /* burst_id is 8-bit on the air */
#define MAX_PAGES            16    /* safety margin for page_finished array */

/* ---- Time helper ---- */

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- Little-endian helpers ---- */

static void encode_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t decode_u16_le(const uint8_t *src)
{
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static void encode_u64_le(uint8_t *dst, uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        dst[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
}

/* ---- 64-bit FNV-1a checksum ---- */

#define FNV64_OFFSET_BASIS  1469598103934665603ULL
#define FNV64_PRIME         1099511628211ULL

static void checksum_init(uint64_t *state)
{
    *state = FNV64_OFFSET_BASIS;
}

static void checksum_update(uint64_t *state, const uint8_t *data, size_t len)
{
    uint64_t h = *state;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)data[i];
        h *= FNV64_PRIME;
    }
    *state = h;
}

static void checksum_final(uint64_t state, uint8_t out[CHECKSUM_SIZE])
{
    encode_u64_le(out, state);
}

/* ---- nRF24 convenience wrappers ---- */

static int configure_radio_from_app(nrf24_t *radio, const app_config_t *cfg)
{
    if (!radio || !cfg) {
        logger_error("configure_radio_from_app: invalid argument");
        return -1;
    }

    unsigned int data_rate_kbps;
    switch (cfg->data_rate) {
    case APP_DATA_RATE_250KBPS: data_rate_kbps = 250;  break;
    case APP_DATA_RATE_2MBPS:   data_rate_kbps = 2000; break;
    case APP_DATA_RATE_1MBPS:
    default:
        data_rate_kbps = 1000;
        break;
    }

    int pa_level_dbm;
    switch (cfg->pa_level) {
    case APP_PA_LOW:  pa_level_dbm = -12; break;
    case APP_PA_HIGH: pa_level_dbm = -6;  break;
    case APP_PA_MAX:  pa_level_dbm = 0;   break;
    case APP_PA_MIN:
    default:
        pa_level_dbm = -18;
        break;
    }

    unsigned int crc_bytes  = (unsigned int)cfg->crc_bytes;
    unsigned int retr_delay = (unsigned int)cfg->retransmission_delay;
    unsigned int retr_tries = (unsigned int)cfg->retransmission_tries;

    if (nrf24_configure_advanced(radio,
                                 (uint8_t)cfg->channel,
                                 data_rate_kbps,
                                 pa_level_dbm,
                                 crc_bytes,
                                 retr_delay,
                                 retr_tries) < 0) {
        logger_error("nrf24_configure_advanced failed");
        return -1;
    }

    return 0;
}


/* Counts every on-air attempt (including retries) into rf_bytes_total / rf_frames_total. */

static int send_with_retries(nrf24_t *radio,
                             const uint8_t *buf,
                             uint8_t len,
                             unsigned int timeout_ms,
                             const char *what,
                             uint64_t *rf_bytes_total,
                             uint64_t *rf_frames_total)
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
            (void)nrf24_configure_quick(radio, P2P_CHANNEL);
        }
    }
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

static void page_stream_free(PageStream *ps)
{
    if (!ps->bursts) return;
    for (size_t i = 0; i < ps->count; ++i) {
        free_burst(&ps->bursts[i]);
    }
    free(ps->bursts);
    ps->bursts   = NULL;
    ps->count    = 0;
    ps->capacity = 0;
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

/* ---- TX: read whole file and send in 10 compressed pages ---- */

static int run_p3p_tx(const app_config_t *app_cfg)
{
    if (!app_cfg) {
        logger_error("run_p3p_tx: app_cfg is NULL");
        return 1;
    }

    const char *spi_dev = "/dev/spidev0.0"; /* TODO: make configurable via app_layer if needed */

    nrf24_t radio;
    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)app_cfg->ce_pin
    };

    if (nrf24_init(&radio, &cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    if (configure_radio_from_app(&radio, app_cfg) < 0) {
        nrf24_deinit(&radio);
        return 1;
    }
    if (nrf24_set_mode_tx(&radio) < 0) {
        logger_error("nrf24_set_mode_tx failed");
        nrf24_deinit(&radio);
        return 1;
    }

    /* Load the entire input file via app_layer (USB / fallback handled there) */
    uint8_t *orig_buf  = NULL;
    size_t   orig_size = 0;
    if (app_load_file_bytes(app_cfg->file_path_tx, &orig_buf, &orig_size) != 0) {
        logger_error("P3P TX: failed to load input file");
        nrf24_deinit(&radio);
        return 1;
    }

    uint64_t orig_len = (uint64_t)orig_size;

    logger_info("P3P TX: sending %llu bytes (split into %d pages)",
                (unsigned long long)orig_len, P2P_NUM_PAGES);

    /* ---- BEGIN: original P2P TX body (unchanged, except logging funcs) ---- */

    double t_start = now_seconds();

    uint64_t total_compressed   = 0;
    uint64_t tx_rf_bytes_total  = 0;  /* counts radio bytes, including retries */
    uint64_t tx_rf_frames_total = 0;  /* counts frames, including retries */

    uint64_t page_offsets[P2P_NUM_PAGES + 1];
    page_offsets[0] = 0;

    uint64_t base_page_size = orig_len / P2P_NUM_PAGES;
    uint64_t remainder      = orig_len % P2P_NUM_PAGES;

    for (unsigned page_id = 0; page_id < P2P_NUM_PAGES; ++page_id) {
        uint64_t extra = (page_id < remainder) ? 1 : 0;
        page_offsets[page_id + 1] = page_offsets[page_id] + base_page_size + extra;
    }

    for (unsigned page_id = 0; page_id < P2P_NUM_PAGES; ++page_id) {
        uint64_t page_start = page_offsets[page_id];
        uint64_t page_end   = page_offsets[page_id + 1];
        uint64_t page_len   = page_end - page_start;

        const uint8_t *page_data = orig_buf + page_start;

        uint64_t uncompressed_offset = page_start; /* for logging */

        uint32_t burst_count = 0;

        size_t bursts_for_page = (page_len + BURST_DATA_MAX - 1) / BURST_DATA_MAX;
        if (bursts_for_page == 0) bursts_for_page = 1;

        logger_info("P2P TX: Page %u: %llu bytes -> %zu bursts",
                    page_id,
                    (unsigned long long)page_len,
                    bursts_for_page);

        uint64_t comp_offset   = 0;
        uint64_t comp_capacity = (page_len > 0)
                               ? (page_len + (page_len / 10) + 64)
                               : 64;

        uint8_t *comp_page = (uint8_t *)malloc((size_t)comp_capacity);
        if (!comp_page) {
            logger_error("P2P TX: malloc(%llu) for compressed page failed",
                         (unsigned long long)comp_capacity);
            free(orig_buf);
            nrf24_deinit(&radio);
            return 1;
        }

        unsigned long dest_len = (unsigned long)comp_capacity;
        int zret = compress2(comp_page, &dest_len,
                             page_data, (unsigned long)page_len,
                             Z_BEST_SPEED);

        if (zret != Z_OK) {
            logger_error("P2P TX: compress2 failed for page %u (zret=%d)", page_id, zret);
            free(comp_page);
            free(orig_buf);
            nrf24_deinit(&radio);
            return 1;
        }

        size_t comp_len = (size_t)dest_len;
        total_compressed += comp_len;

        double ratio = (page_len == 0)
                     ? 0.0
                     : (100.0 * (double)comp_len / (double)page_len);

        logger_info("P2P TX: Page %u compressed %llu -> %zu bytes (~%.2f%%)",
                    page_id,
                    (unsigned long long)page_len,
                    comp_len,
                    ratio);

        uint64_t chk;
        checksum_init(&chk);
        checksum_update(&chk, page_data, (size_t)page_len);
        uint8_t checksum[CHECKSUM_SIZE];
        checksum_final(chk, checksum);

        logger_info("P2P TX: Page %u checksum = 0x%02X%02X%02X%02X%02X%02X%02X%02X",
                    page_id,
                    checksum[0], checksum[1], checksum[2], checksum[3],
                    checksum[4], checksum[5], checksum[6], checksum[7]);

        tx_rf_bytes_total  += 4 + comp_len + CHECKSUM_SIZE;
        tx_rf_frames_total += bursts_for_page + 2;

        for (size_t burst_idx = 0; burst_idx < bursts_for_page; ++burst_idx) {
            uint64_t remaining     = comp_len - comp_offset;
            uint64_t burst_payload = (remaining > BURST_DATA_MAX) ? BURST_DATA_MAX : remaining;

            uint8_t info[4];
            info[0] = MSG_BURST_INFO;
            info[1] = (uint8_t)page_id;
            info[2] = (uint8_t)burst_idx;
            info[3] = (uint8_t)bursts_for_page;

            if (send_with_retries(&radio,
                                  info, sizeof(info),
                                  CONTROL_TIMEOUT_MS,
                                  "BURST_INFO",
                                  &tx_rf_bytes_total,
                                  &tx_rf_frames_total) < 0) {
                logger_error("P2P TX: failed to send BURST_INFO for page %u, burst %zu",
                             page_id, burst_idx);
                free(comp_page);
                free(orig_buf);
                nrf24_deinit(&radio);
                return 1;
            }

            uint64_t chunk_offset = comp_offset;
            uint64_t burst_end    = comp_offset + burst_payload;

            unsigned chunk_id = 0;

            while (chunk_offset < burst_end) {
                uint64_t chunk_data_len = burst_end - chunk_offset;
                if (chunk_data_len > CHUNK_DATA_MAX) {
                    chunk_data_len = CHUNK_DATA_MAX;
                }

                uint8_t frame[MAX_PAYLOAD];
                frame[0] = (uint8_t)chunk_id;
                memcpy(&frame[1], comp_page + chunk_offset, (size_t)chunk_data_len);

                uint8_t frame_len = (uint8_t)(1 + chunk_data_len);

                if (send_with_retries(&radio,
                                      frame,
                                      frame_len,
                                      DATA_TIMEOUT_MS,
                                      "DATA chunk",
                                      &tx_rf_bytes_total,
                                      &tx_rf_frames_total) < 0) {
                    logger_error("P2P TX: failed to send DATA chunk for page %u, burst %zu, chunk %u",
                                 page_id, burst_idx, chunk_id);
                    free(comp_page);
                    free(orig_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }

                chunk_offset += chunk_data_len;
                ++chunk_id;
            }

            comp_offset += burst_payload;
            ++burst_count;
        }

        if (burst_count != bursts_for_page) {
            logger_error("P2P TX: internal error: burst_count (%u) != bursts_for_page (%zu)",
                         burst_count, bursts_for_page);
        }

        uint8_t info[1 + CHECKSUM_SIZE];
        info[0] = MSG_TRANSFER_FINISH;
        memcpy(&info[1], checksum, CHECKSUM_SIZE);

        if (send_with_retries(&radio,
                              info, sizeof(info),
                              CHECKSUM_TIMEOUT_MS,
                              "TRANSFER_FINISH",
                              &tx_rf_bytes_total,
                              &tx_rf_frames_total) < 0) {
            logger_error("P2P TX: failed to send TRANSFER_FINISH for page %u", page_id);
            free(comp_page);
            free(orig_buf);
            nrf24_deinit(&radio);
            return 1;
        }

        free(comp_page);

        uncompressed_offset += page_len;
        logger_info("P2P TX: Page %u completed; uncompressed offset now %llu bytes",
                    page_id, (unsigned long long)uncompressed_offset);
    }

    double t_end = now_seconds();
    double dt    = t_end - t_start;

    double user_kibps = (dt > 0.0)
        ? ((double)orig_len / 1024.0 / dt)
        : 0.0;

    double rf_kibps = (dt > 0.0)
        ? ((double)tx_rf_bytes_total / 1024.0 / dt)
        : 0.0;

    logger_info("P2P TX: compressed total %llu bytes out of %llu bytes (ratio ~%.2f%%)",
                (unsigned long long)total_compressed,
                (unsigned long long)orig_len,
                (orig_len == 0)
                    ? 0.0
                    : (100.0 * (double)total_compressed / (double)orig_len));

    logger_succ("P2P TX: done. Sent %llu B in %.3f s (%.1f KiB/s user data, "
                "%llu RF bytes in %.3f s (~%.1f KiB/s RF on-air), %llu RF frames).",
                (unsigned long long)orig_len,
                dt, user_kibps,
                (unsigned long long)tx_rf_bytes_total, dt, rf_kibps,
                (unsigned long long)tx_rf_frames_total);

    free(orig_buf);
    nrf24_deinit(&radio);
    return 0;
}


/* ---- RX: receive in pages, decompress each page independently ---- */

static int run_p3p_rx(const app_config_t *app_cfg)
{
    if (!app_cfg) {
        logger_error("run_p3p_rx: app_cfg is NULL");
        return 1;
    }

    const char *spi_dev = "/dev/spidev0.0"; /* TODO: make configurable via app_layer if needed */

    nrf24_t radio;
    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)app_cfg->ce_pin
    };

    if (nrf24_init(&radio, &cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    if (configure_radio_from_app(&radio, app_cfg) < 0) {
        nrf24_deinit(&radio);
        return 1;
    }
    if (nrf24_set_mode_rx(&radio) < 0) {
        logger_error("nrf24_set_mode_rx failed");
        nrf24_deinit(&radio);
        return 1;
    }

    char path_buf[512];
    const char *output_path = app_cfg->file_path_rx;

    if (!output_path || output_path[0] == '\0') {
        char ts[32];
        logger_timestamp(ts, sizeof(ts));
        snprintf(path_buf, sizeof(path_buf), "rx_files/%s.txt", ts);
        output_path = path_buf;
    }

    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        logger_error("Cannot open output file '%s': %s", output_path, strerror(errno));
        nrf24_deinit(&radio);
        return 1;
    }

    logger_info("P3P RX: writing received data to '%s'", output_path);

    /* ---- BEGIN: original P2P RX body (unchanged) ---- */

    PageStream stream;
    page_stream_init(&stream);

    logger_info("P2P RX: waiting for STREAM_INFO / BURST_INFO on channel %d...", P2P_CHANNEL);

    int transfer_finished = 0;
    int tx_started        = 0;
    double t_start        = 0.0;

    int page_finished[P2P_NUM_PAGES];
    for (unsigned i = 0; i < P2P_NUM_PAGES; ++i) {
        page_finished[i] = 0;
    }

    uint64_t rf_bytes_total  = 0;
    uint64_t rf_frames_total = 0;

    uint8_t rx_buf[MAX_PAYLOAD];
    uint8_t rx_len;

    uint64_t compressed_total   = 0;
    uint64_t uncompressed_total = 0;

    while (!transfer_finished) {
        rx_len = sizeof(rx_buf);
        int r = nrf24_recv_blocking(&radio,
                                    rx_buf,
                                    &rx_len,
                                    1000);

        if (r < 0) {
            logger_error("P2P RX: nrf24_recv_blocking failed: %s", strerror(errno));
            page_stream_free(&stream);
            fclose(fout);
            nrf24_deinit(&radio);
            return 1;
        }

        if (rx_len == 0) {
            continue;
        }

        rf_bytes_total  += rx_len;
        rf_frames_total += 1;

        uint8_t msg_id = rx_buf[0];

        if (msg_id == MSG_BURST_INFO) {
            if (rx_len < 4) {
                logger_warn("P2P RX: invalid BURST_INFO length: %u", rx_len);
                continue;
            }

            uint8_t page_id      = rx_buf[1];
            uint8_t burst_idx    = rx_buf[2];
            uint8_t bursts_total = rx_buf[3];

            if (page_id >= P2P_NUM_PAGES) {
                logger_warn("P2P RX: invalid page_id %u in BURST_INFO", page_id);
                continue;
            }

            logger_info("P2P RX: BURST_INFO for page %u, burst %u / %u",
                        page_id, burst_idx, bursts_total);

            if (!tx_started) {
                tx_started = 1;
                t_start    = now_seconds();
            }

            uint8_t expected_chunk_id = 0;
            uint8_t burst_buf[1 + BURST_DATA_MAX];
            uint64_t burst_data_len = 0;

            while (1) {
                rx_len = sizeof(rx_buf);
                r = nrf24_recv_blocking(&radio,
                                        rx_buf,
                                        &rx_len,
                                        1000);

                if (r < 0) {
                    logger_error("P2P RX: nrf24_recv_blocking failed (DATA): %s", strerror(errno));
                    page_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }

                if (rx_len == 0) {
                    continue;
                }

                rf_bytes_total  += rx_len;
                rf_frames_total += 1;

                if (rx_buf[0] == MSG_BURST_INFO || rx_buf[0] == MSG_TRANSFER_FINISH) {
                    compressed_total   += burst_data_len;
                    burst_data_len      = 0;
                    expected_chunk_id   = 0;

                    logger_warn("P2P RX: unexpected control frame while receiving DATA; restarting burst");
                    break;
                }

                uint8_t chunk_id = rx_buf[0];
                if (chunk_id != expected_chunk_id) {
                    logger_warn("P2P RX: out-of-order chunk: got %u, expected %u",
                                chunk_id, expected_chunk_id);
                    continue;
                }

                uint8_t chunk_len = rx_len - 1;
                if (chunk_len > CHUNK_DATA_MAX) {
                    logger_warn("P2P RX: invalid chunk_len %u (max %d)", chunk_len, CHUNK_DATA_MAX);
                    continue;
                }

                if (burst_data_len + chunk_len > BURST_DATA_MAX) {
                    logger_warn("P2P RX: burst_data_len overflow (current %llu, chunk %u)",
                                (unsigned long long)burst_data_len, chunk_len);
                    continue;
                }

                memcpy(burst_buf + burst_data_len, &rx_buf[1], chunk_len);
                burst_data_len += chunk_len;
                ++expected_chunk_id;

                if (expected_chunk_id >= 255) {
                    logger_warn("P2P RX: expected_chunk_id overflow");
                    break;
                }

                if (burst_idx == bursts_total - 1 &&
                    burst_data_len >= BURST_DATA_MAX) {
                    break;
                }

                if (rx_len < MAX_PAYLOAD) {
                    break;
                }
            }

            compressed_total += burst_data_len;

            if (page_finished[page_id]) {
                logger_info("P2P RX: page %u already finished; skipping append", page_id);
            } else {
                if (page_stream_append_burst(&stream,
                                             page_id,
                                             burst_idx,
                                             bursts_total,
                                             burst_buf,
                                             (size_t)burst_data_len) < 0) {
                    logger_error("P2P RX: page_stream_append_burst failed for page %u, burst %u",
                                 page_id, burst_idx);
                    page_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }
            }
        } else if (msg_id == MSG_TRANSFER_FINISH) {
            if (rx_len != 1 + CHECKSUM_SIZE) {
                logger_warn("P2P RX: invalid TRANSFER_FINISH length: %u", rx_len);
                continue;
            }

            uint8_t page_id = rx_buf[1];
            if (page_id >= P2P_NUM_PAGES) {
                logger_warn("P2P RX: invalid page_id %u in TRANSFER_FINISH", page_id);
                continue;
            }

            uint8_t checksum[CHECKSUM_SIZE];
            memcpy(checksum, &rx_buf[1], CHECKSUM_SIZE);

            logger_info("P2P RX: TRANSFER_FINISH for page %u; checksum = "
                        "0x%02X%02X%02X%02X%02X%02X%02X%02X",
                        page_id,
                        checksum[0], checksum[1], checksum[2], checksum[3],
                        checksum[4], checksum[5], checksum[6], checksum[7]);

            if (page_finished[page_id]) {
                logger_info("P2P RX: page %u already finished; ignoring TRANSFER_FINISH", page_id);
                continue;
            }

            size_t page_len = 0;
            uint8_t *page_data = page_stream_get_page(&stream, page_id, &page_len);
            if (!page_data) {
                logger_error("P2P RX: page_stream_get_page failed for page %u", page_id);
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            uint64_t chk;
            checksum_init(&chk);
            checksum_update(&chk, page_data, page_len);
            uint8_t computed[CHECKSUM_SIZE];
            checksum_final(chk, computed);

            if (memcmp(computed, checksum, CHECKSUM_SIZE) != 0) {
                logger_error("P2P RX: checksum mismatch for page %u", page_id);
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            if (decompress_page_to_file(page_data, page_len, fout) < 0) {
                logger_error("P2P RX: decompress_page_to_file failed for page %u", page_id);
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            uncompressed_total += page_stream_page_uncompressed_size(&stream, page_id);
            page_finished[page_id] = 1;

            logger_info("P2P RX: page %u finished; uncompressed_total now %llu bytes",
                        page_id,
                        (unsigned long long)uncompressed_total);

            transfer_finished = 1;
            for (unsigned i = 0; i < P2P_NUM_PAGES; ++i) {
                if (!page_finished[i]) {
                    transfer_finished = 0;
                    break;
                }
            }
        } else {
            logger_warn("P2P RX: unknown message ID 0x%02X", msg_id);
        }
    }

    double t_end = now_seconds();
    double dt    = t_end - t_start;

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

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s tx <spi_device> <ce_gpio> <input_file>\n"
            "  %s rx <spi_device> <ce_gpio> <output_file>\n"
            "Example:\n"
            "  %s tx /dev/spidev0.0 22 test_files/lorem.txt\n"
            "  %s rx /dev/spidev0.0 22 received_file.txt\n",
            prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
    app_config_t cfg;

    app_set_default_config(&cfg);

    if (app_parse_arguments(argc, argv, &cfg) != 0) {
        app_print_usage(argv[0]);
        return 1;
    }

    if (cfg.print_config) {
        app_print_config(&cfg);
    }

    /* Choose logfile name based on mode */
    char log_path[64];
    if (cfg.mode == APP_MODE_TX) {
        snprintf(log_path, sizeof(log_path), "p3p_tx.log");
    } else if (cfg.mode == APP_MODE_RX) {
        snprintf(log_path, sizeof(log_path), "p3p_rx.log");
    } else {
        logger_error("Mode not set; please provide --mode TX or --mode RX");
        return 1;
    }

    if (logger_init(log_path) != 0) {
        logger_warn("Could not open log file '%s' (continuing without file log)", log_path);
    } else {
        logger_info("Logging to file '%s'", log_path);
    }

    int ret = 0;
    if (cfg.mode == APP_MODE_TX) {
        ret = run_p3p_tx(&cfg);
    } else {
        ret = run_p3p_rx(&cfg);
    }

    logger_close();
    return ret;
}

