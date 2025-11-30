#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "libs/nrf24.h"
#include "libs/logger.h"
#include "libs/app_layer.h"
#include "libs/presentation_layer.h"
#include "libs/transport_layer.h"

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

/* Paging */
#define MAX_BURSTS_PER_PAGE  255   /* burst_id is 8-bit on the air */

/* Default SPI device can be overridden via NRF24_SPI_DEVICE env var */
#if defined(_WIN32)
#define DEFAULT_SPI_DEVICE    "SPI0"
#else
#define DEFAULT_SPI_DEVICE    "/dev/spidev0.0"
#endif

static uint8_t g_radio_channel = P2P_CHANNEL;

typedef struct {
    uint8_t  channel;
    unsigned data_rate_kbps;
    int      pa_level_dbm;
    unsigned crc_bytes;
    unsigned retr_delay;
    unsigned retr_tries;
} RadioRuntimeConfig;

static RadioRuntimeConfig g_radio_runtime = {
    .channel        = P2P_CHANNEL,
    .data_rate_kbps = 1000,
    .pa_level_dbm   = -18,
    .crc_bytes      = 2,
    .retr_delay     = 2,
    .retr_tries     = 15
};

static unsigned map_data_rate_kbps(app_data_rate_t rate)
{
    switch (rate) {
    case APP_DATA_RATE_250KBPS: return 250;
    case APP_DATA_RATE_2MBPS:   return 2000;
    case APP_DATA_RATE_1MBPS:
    default:                    return 1000;
    }
}

static int map_pa_level_dbm(app_pa_level_t level)
{
    switch (level) {
    case APP_PA_MAX:  return 0;
    case APP_PA_HIGH: return -6;
    case APP_PA_LOW:  return -12;
    case APP_PA_MIN:
    default:          return -18;
    }
}

static unsigned map_crc_bytes(app_crc_bytes_t crc)
{
    switch (crc) {
    case APP_CRC_OFF: return 0;
    case APP_CRC_8:   return 1;
    case APP_CRC_16:
    default:          return 2;
    }
}

static void update_radio_runtime_from_config(const app_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    g_radio_channel = (uint8_t)cfg->channel;
    g_radio_runtime.channel        = g_radio_channel;
    g_radio_runtime.data_rate_kbps = map_data_rate_kbps(cfg->data_rate);
    g_radio_runtime.pa_level_dbm   = map_pa_level_dbm(cfg->pa_level);
    g_radio_runtime.crc_bytes      = map_crc_bytes(cfg->crc_bytes);
    g_radio_runtime.retr_delay     = (unsigned)cfg->retransmission_delay;
    g_radio_runtime.retr_tries     = (unsigned)cfg->retransmission_tries;
}

static int configure_radio_runtime(nrf24_t *radio)
{
    return nrf24_configure_advanced(radio,
                                    g_radio_runtime.channel,
                                    g_radio_runtime.data_rate_kbps,
                                    g_radio_runtime.pa_level_dbm,
                                    g_radio_runtime.crc_bytes,
                                    g_radio_runtime.retr_delay,
                                    g_radio_runtime.retr_tries);
}

static int maybe_verify_radio_config(const app_config_t *cfg,
                                     nrf24_t *radio,
                                     const char *label)
{
    if (!cfg || !cfg->verify_config) {
        return 0;
    }

    if (label) {
        logger_info("Verifying radio configuration (phase: %s) via module readback",
                    label);
    } else {
        logger_info("Verifying radio configuration via module readback");
    }

    if (nrf24_dump_config(radio) < 0) {
        logger_error("nrf24_dump_config failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} RxOutputBuffer;

static void rx_output_init(RxOutputBuffer *buf)
{
    if (!buf) return;
    buf->data = NULL;
    buf->len  = 0;
    buf->cap  = 0;
}

static void rx_output_free(RxOutputBuffer *buf)
{
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len  = 0;
    buf->cap  = 0;
}

static int rx_output_append(RxOutputBuffer *buf, const uint8_t *chunk, size_t len)
{
    if (!buf || (!chunk && len > 0)) {
        logger_error("rx_output_append: invalid parameters");
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    size_t needed = buf->len + len;
    if (needed > buf->cap) {
        size_t new_cap = buf->cap ? buf->cap * 2 : 4096;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        uint8_t *tmp = (uint8_t *)realloc(buf->data, new_cap);
        if (!tmp) {
            logger_error("rx_output_append: realloc failed (need %zu bytes)", needed);
            return -1;
        }
        buf->data = tmp;
        buf->cap  = new_cap;
    }

    memcpy(buf->data + buf->len, chunk, len);
    buf->len = needed;
    return 0;
}

typedef struct {
    trans_burst_t *bursts;
    size_t         count;
    size_t         capacity;
} PageStream;

static int extract_page_compressed_data(PageStream *ps,
                                        uint8_t **out_data,
                                        size_t *out_len,
                                        uint64_t *compressed_total)
{
    if (!out_data || !out_len) {
        logger_error("extract_page_compressed_data: null output pointer");
        return -1;
    }

    *out_data = NULL;
    *out_len  = 0;

    if (!ps->bursts || ps->count == 0) {
        return 0;
    }

    RxOutputBuffer tmp;
    rx_output_init(&tmp);

    for (size_t bid = 0; bid < ps->count; ++bid) {
        trans_burst_t *b = &ps->bursts[bid];
        if (!b->frames || b->frame_count == 0) continue;

        for (size_t i = 0; i < b->frame_count; ++i) {
            trans_frame_t *frame = &b->frames[i];
            if (frame->len <= 1) continue;

            size_t payload_len = (size_t)frame->len - 1u;
            if (rx_output_append(&tmp, frame->data + 1, payload_len) != 0) {
                rx_output_free(&tmp);
                return -1;
            }
            if (compressed_total) {
                *compressed_total += payload_len;
            }
        }
    }

    *out_data = tmp.data;
    *out_len  = tmp.len;
    tmp.data  = NULL;
    tmp.len   = 0;
    tmp.cap   = 0;
    rx_output_free(&tmp);
    return 0;
}

static const char *get_spi_device_path(void)
{
    const char *env = getenv("NRF24_SPI_DEVICE");
    if (env && *env) {
        return env;
    }
    return DEFAULT_SPI_DEVICE;
}

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

static void encode_u32_le(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
    dst[2] = (uint8_t)((v >> 16) & 0xFFu);
    dst[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t decode_u32_le(const uint8_t *src)
{
    return (uint32_t)(src[0] |
                      ((uint32_t)src[1] << 8) |
                      ((uint32_t)src[2] << 16) |
                      ((uint32_t)src[3] << 24));
}

static size_t compute_page_orig_len(uint64_t total_len, size_t page_index)
{
    if (page_index >= TRANS_NUM_PAGES || total_len == 0) {
        return 0;
    }

    uint64_t page_count = (total_len < TRANS_NUM_PAGES) ? total_len : TRANS_NUM_PAGES;
    if (page_index >= page_count) {
        return 0;
    }

    uint64_t base = (page_count > 0) ? (total_len / page_count) : 0;
    uint64_t rem  = (page_count > 0) ? (total_len % page_count) : 0;
    uint64_t len  = base + ((uint64_t)page_index < rem ? 1ull : 0ull);
    return (size_t)len;
}

/* ---- nRF24 convenience wrappers ---- */
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
            if (configure_radio_runtime(radio) < 0) {
                logger_error("%s: radio reconfigure failed: %s", what, strerror(errno));
                return -1;
            }
        }
    }
}

/* ---- RX-side in-memory burst storage (one page at a time) ---- */

/* Initialise an empty page stream */
static void page_stream_init(PageStream *ps)
{
    if (!ps) return;
    ps->bursts   = NULL;
    ps->count    = 0;
    ps->capacity = 0;
}

static void page_stream_free(PageStream *ps)
{
    if (!ps || !ps->bursts) return;
    for (size_t i = 0; i < ps->count; ++i) {
        trans_free_burst(&ps->bursts[i]);
    }
    free(ps->bursts);
    ps->bursts   = NULL;
    ps->count    = 0;
    ps->capacity = 0;
}

/* Ensure we have at least (burst_id+1) bursts allocated. */
static trans_burst_t *page_get_burst(PageStream *ps, unsigned burst_id)
{
    if (!ps) return NULL;

    if (burst_id >= ps->capacity) {
        size_t new_cap = ps->capacity ? ps->capacity * 2 : 8;
        while (burst_id >= new_cap) new_cap *= 2;

        trans_burst_t *new_bursts = (trans_burst_t *)calloc(new_cap, sizeof(trans_burst_t));
        if (!new_bursts) {
            return NULL;
        }

        for (size_t i = 0; i < ps->count; ++i) {
            new_bursts[i] = ps->bursts[i];
        }
        free(ps->bursts);
        ps->bursts   = new_bursts;
        ps->capacity = new_cap;
    }

    if (burst_id >= ps->count) {
        for (size_t i = ps->count; i <= burst_id; ++i) {
            ps->bursts[i].frames = NULL;
            ps->bursts[i].frame_count = 0;
        }
        ps->count = burst_id + 1;
    }

    return &ps->bursts[burst_id];
}

/* Store a fully-received burst into PageStream[burst_id]. Overwrites old one if present. */
static int store_burst(PageStream *ps,
                       unsigned burst_id,
                       trans_burst_t *burst)
{
    trans_burst_t *slot = page_get_burst(ps, burst_id);
    if (!slot) {
        logger_error("store_burst: out of memory for burst %u", burst_id);
        return -1;
    }

    trans_free_burst(slot);
    *slot = *burst;
    burst->frames = NULL;
    burst->frame_count = 0;
    return 0;
}

/* ---- Decompress a single page (PageStream) and append to RX buffer ---- */

static int decompress_page_to_buffer(PageStream *ps,
                                     size_t expected_raw_len,
                                     RxOutputBuffer *out,
                                     uint64_t *compressed_total,
                                     uint64_t *uncompressed_total)
{
    uint8_t *compressed = NULL;
    size_t   compressed_len = 0;

    if (extract_page_compressed_data(ps,
                                     &compressed,
                                     &compressed_len,
                                     compressed_total) != 0) {
        return -1;
    }

    if (!compressed || compressed_len == 0) {
        if (compressed) {
            free(compressed);
        }
        if (expected_raw_len > 0) {
            logger_warn("P2P RX: page expected %zu raw bytes but no compressed payload was collected",
                        expected_raw_len);
        }
        return 0;
    }

    pres_page_t in_page;
    in_page.data = compressed;
    in_page.size = compressed_len;
    in_page.orig_size = expected_raw_len;

    pres_page_t *raw_pages = NULL;
    size_t raw_count = 0;

    int pret = pres_decompress_pages(&in_page, 1, &raw_pages, &raw_count);
    free(compressed);

    if (pret != 0) {
        logger_error("P2P RX: presentation-layer decompression failed for page (ret=%d)", pret);
        if (raw_pages) {
            pres_free_pages(raw_pages, raw_count);
        }
        return -1;
    }

    if (raw_count == 0) {
        return 0;
    }

    for (size_t i = 0; i < raw_count; ++i) {
        if (!raw_pages[i].data || raw_pages[i].size == 0) {
            continue;
        }
        if (rx_output_append(out, raw_pages[i].data, raw_pages[i].size) != 0) {
            pres_free_pages(raw_pages, raw_count);
            return -1;
        }
        if (uncompressed_total) {
            *uncompressed_total += raw_pages[i].size;
        }
    }

    pres_free_pages(raw_pages, raw_count);
    return 0;
}

/* ---- TX: send preloaded bytes split/compressed by presentation layer ---- */

static int run_tx(const char *spi_dev,
                  const app_config_t *cfg,
                  const uint8_t *input_data,
                  size_t input_len)
{
    if (!cfg) {
        logger_error("run_tx: cfg is NULL");
        return 1;
    }
    if (input_len > 0 && !input_data) {
        logger_error("run_tx: input buffer is NULL but length=%zu", input_len);
        return 1;
    }

    nrf24_t radio;
    nrf24_config_t hw_cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)cfg->ce_pin
    };

    if (nrf24_init(&radio, &hw_cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    if (configure_radio_runtime(&radio) < 0) {
        logger_error("nrf24_configure_advanced failed: %s", strerror(errno));
        nrf24_deinit(&radio);
        return 1;
    }
    if (nrf24_set_mode_tx(&radio) < 0) {
        logger_error("nrf24_set_mode_tx failed");
        nrf24_deinit(&radio);
        return 1;
    }
    if (maybe_verify_radio_config(cfg, &radio, "TX setup") < 0) {
        nrf24_deinit(&radio);
        return 1;
    }

    pres_page_t *raw_pages = NULL;
    size_t raw_page_count = 0;
    pres_page_t *compressed_pages = NULL;
    size_t compressed_page_count = 0;
    int exit_code = 1;

    if (pres_split_into_pages_default(input_data,
                                      input_len,
                                      &raw_pages,
                                      &raw_page_count) != 0) {
        logger_error("P2P TX: failed to split input into pages");
        goto cleanup;
    }

    if (raw_page_count < TRANS_NUM_PAGES) {
        pres_page_t *expanded = (pres_page_t *)calloc(TRANS_NUM_PAGES, sizeof(pres_page_t));
        if (!expanded) {
            logger_error("P2P TX: calloc failed while padding raw pages");
            goto cleanup;
        }
        for (size_t i = 0; i < raw_page_count; ++i) {
            expanded[i] = raw_pages[i];
        }
        free(raw_pages);
        raw_pages = expanded;
        raw_page_count = TRANS_NUM_PAGES;
    }

    if (raw_page_count != TRANS_NUM_PAGES) {
        logger_error("P2P TX: unexpected raw_page_count=%zu (expected %u)",
                     raw_page_count, (unsigned)TRANS_NUM_PAGES);
        goto cleanup;
    }

    if (pres_compress_pages(raw_pages,
                            raw_page_count,
                            &compressed_pages,
                            &compressed_page_count) != 0) {
        logger_error("P2P TX: failed to compress pages");
        goto cleanup;
    }

    pres_free_pages(raw_pages, raw_page_count);
    raw_pages = NULL;
    raw_page_count = 0;

    if (compressed_page_count != TRANS_NUM_PAGES) {
        logger_error("P2P TX: transport requires %u pages (got %zu)",
                     (unsigned)TRANS_NUM_PAGES, compressed_page_count);
        goto cleanup;
    }

    if (input_len > TRANS_STREAM_MAX_TOTAL_SIZE) {
        logger_error("P2P TX: file too large (%zu bytes > %u-byte limit)",
                     input_len, (unsigned)TRANS_STREAM_MAX_TOTAL_SIZE);
        goto cleanup;
    }

    uint64_t orig_len = (uint64_t)input_len;
    const char *input_label = cfg->file_path_tx ? cfg->file_path_tx : "(auto-selected)";

    uint32_t comp_sizes[TRANS_NUM_PAGES] = {0};
    uint64_t total_compressed = 0;

    for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
        size_t comp_len = compressed_pages[i].size;
        if (comp_len > TRANS_STREAM_MAX_PAGE_COMP_SIZE) {
            logger_error("P2P TX: page %zu compressed size %zu exceeds protocol limit", i, comp_len);
            goto cleanup;
        }
        comp_sizes[i] = (uint32_t)comp_len;
        total_compressed += comp_len;
    }

    uint8_t stream_info[32];
    if (trans_build_stream_info((uint32_t)orig_len, comp_sizes, stream_info) != 0) {
        logger_error("P2P TX: failed to build STREAM_INFO");
        goto cleanup;
    }

    logger_info("P2P TX: sending '%s' (%llu bytes, %u transport pages)",
                input_label, (unsigned long long)orig_len, (unsigned)TRANS_NUM_PAGES);

    double t_start = now_seconds();

    uint64_t tx_rf_bytes_total  = 0;
    uint64_t tx_rf_frames_total = 0;

    if (send_with_retries(&radio,
                          stream_info,
                          sizeof(stream_info),
                          CONTROL_TIMEOUT_MS,
                          "STREAM_INFO",
                          &tx_rf_bytes_total,
                          &tx_rf_frames_total) < 0) {
        logger_error("P2P TX: failed to send STREAM_INFO");
        goto cleanup;
    }

    for (size_t page_id = 0; page_id < TRANS_NUM_PAGES; ++page_id) {
        const pres_page_t *page = &compressed_pages[page_id];
        size_t comp_len = page->size;
        size_t page_len = page->orig_size;

        if (comp_len == 0) {
            if (page_len > 0) {
                logger_warn("P2P TX: page %zu has raw bytes but zero compressed data", page_id);
            }
            continue;
        }

        double ratio = (page_len == 0)
                     ? 0.0
                     : (100.0 * (double)comp_len / (double)page_len);
        logger_info("P2P TX: Page %zu compressed %zu -> %zu bytes (~%.2f%%)",
                    page_id, page_len, comp_len, ratio);

        trans_burst_t *bursts = NULL;
        size_t burst_count = 0;
        if (trans_split_page_into_bursts(page->data,
                                         comp_len,
                                         &bursts,
                                         &burst_count) != 0) {
            logger_error("P2P TX: failed to split page %zu into bursts", page_id);
            goto cleanup;
        }

        for (size_t burst_id = 0; burst_id < burst_count; ++burst_id) {
            trans_burst_t *burst = &bursts[burst_id];
            uint64_t chk = trans_compute_burst_checksum(burst);
            uint8_t chk_bytes[CHECKSUM_SIZE];
            trans_checksum_to_bytes(chk, chk_bytes);

            logger_info("P2P TX: Page %zu, BURST %zu -> %zu frames, checksum 0x%016llX",
                        page_id, burst_id, burst->frame_count,
                        (unsigned long long)chk);

            int burst_done = 0;
            while (!burst_done) {
                uint8_t burst_info[6];
                trans_build_burst_info((uint8_t)page_id,
                                       (uint8_t)burst_id,
                                       burst,
                                       burst_info);

                if (send_with_retries(&radio,
                                      burst_info,
                                      sizeof(burst_info),
                                      CONTROL_TIMEOUT_MS,
                                      "BURST_INFO",
                                      &tx_rf_bytes_total,
                                      &tx_rf_frames_total) < 0) {
                    logger_error("Failed to send BURST_INFO (page %zu, burst %zu)",
                                 page_id, burst_id);
                    trans_free_bursts(bursts, burst_count);
                    goto cleanup;
                }

                for (size_t frame_idx = 0; frame_idx < burst->frame_count; ++frame_idx) {
                    const trans_frame_t *fr = &burst->frames[frame_idx];
                    if (send_with_retries(&radio,
                                          fr->data,
                                          fr->len,
                                          DATA_TIMEOUT_MS,
                                          "DATA",
                                          &tx_rf_bytes_total,
                                          &tx_rf_frames_total) < 0) {
                        logger_error("Failed to send DATA frame (page %zu, burst %zu)",
                                     page_id, burst_id);
                        trans_free_bursts(bursts, burst_count);
                        goto cleanup;
                    }
                }

                if (nrf24_set_mode_rx(&radio) < 0) {
                    logger_error("nrf24_set_mode_rx failed");
                    trans_free_bursts(bursts, burst_count);
                    goto cleanup;
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
                        trans_free_bursts(bursts, burst_count);
                        goto cleanup;
                    }

                    if (len2 != CHECKSUM_SIZE) {
                        logger_warn("P2P TX: received non-checksum frame of %u bytes while waiting",
                                     len2);
                        continue;
                    }

                    if (memcmp(buf2, chk_bytes, CHECKSUM_SIZE) == 0) {
                        logger_succ("P2P TX: Page %zu, BURST %zu checksum confirmed by RX",
                                    page_id, burst_id);
                        got_valid_checksum = 1;
                    } else {
                        logger_warn("P2P TX: invalid checksum received for Page %zu, BURST %zu",
                                    page_id, burst_id);
                    }
                }

                if (!got_valid_checksum) {
                    logger_warn("P2P TX: checksum timeout for Page %zu, BURST %zu; resending",
                                page_id, burst_id);
                    if (nrf24_set_mode_tx(&radio) < 0) {
                        logger_error("nrf24_set_mode_tx failed");
                        trans_free_bursts(bursts, burst_count);
                        goto cleanup;
                    }
                    continue;
                }

                burst_done = 1;

                if (nrf24_set_mode_tx(&radio) < 0) {
                    logger_error("nrf24_set_mode_tx failed");
                    trans_free_bursts(bursts, burst_count);
                    goto cleanup;
                }
            }
        }

        trans_free_bursts(bursts, burst_count);
    }

    uint8_t fin_msg[2];
    trans_build_transfer_finish(fin_msg);

    (void)send_with_retries(&radio,
                            fin_msg,
                            sizeof(fin_msg),
                            CONTROL_TIMEOUT_MS,
                            "TRANSFER_FINISH",
                            &tx_rf_bytes_total,
                            &tx_rf_frames_total);

    double t_end = now_seconds();
    double dt    = t_end - t_start;

    double user_kibps = (dt > 0.0)
        ? ((double)orig_len / 1024.0 / dt)
        : 0.0;

    double rf_kibps = (dt > 0.0)
        ? ((double)tx_rf_bytes_total / 1024.0 / dt)
        : 0.0;

    logger_succ("P2P TX: done. User: %llu bytes (compressed %llu B) in %.3f s (%.1f KiB/s). "
                "RF on-air: %llu bytes in %.3f s (%.1f KiB/s, %llu frames).",
                (unsigned long long)orig_len,
                (unsigned long long)total_compressed,
                dt,
                user_kibps,
                (unsigned long long)tx_rf_bytes_total,
                dt,
                rf_kibps,
                (unsigned long long)tx_rf_frames_total);

    exit_code = 0;

cleanup:
    pres_free_pages(raw_pages, raw_page_count);
    pres_free_pages(compressed_pages, compressed_page_count);
    nrf24_deinit(&radio);
    return exit_code;
}

/* ---- RX: receive in pages, decompress each page independently ---- */

static int run_rx(const char *spi_dev,
                  const app_config_t *cfg,
                  RxOutputBuffer *out_buf)
{
    if (!cfg || !out_buf) {
        logger_error("run_rx: invalid arguments");
        return 1;
    }

    rx_output_init(out_buf);

    nrf24_t radio;
    nrf24_config_t hw_cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)cfg->ce_pin
    };

    if (nrf24_init(&radio, &hw_cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    if (configure_radio_runtime(&radio) < 0) {
        logger_error("nrf24_configure_advanced failed: %s", strerror(errno));
        nrf24_deinit(&radio);
        return 1;
    }
    if (nrf24_set_mode_rx(&radio) < 0) {
        logger_error("nrf24_set_mode_rx failed");
        nrf24_deinit(&radio);
        return 1;
    }
    if (maybe_verify_radio_config(cfg, &radio, "RX setup") < 0) {
        nrf24_deinit(&radio);
        return 1;
    }

    PageStream stream;
    page_stream_init(&stream);
    int exit_code = 1;

    logger_info("P2P RX: waiting for STREAM_INFO / bursts on channel %u...",
                (unsigned)g_radio_runtime.channel);

    int transfer_finished = 0;
    int tx_started        = 0;
    double t_start        = 0.0;

    int have_stream_info = 0;
    uint32_t stream_total_orig = 0;
    size_t   stream_page_orig_sizes[TRANS_NUM_PAGES] = {0};
    uint16_t stream_page_expected_bursts[TRANS_NUM_PAGES] = {0};
    uint8_t  page_finished[TRANS_NUM_PAGES] = {0};

    uint8_t current_page_id = 0;
    uint16_t current_expected_bursts = 0;
    uint16_t bursts_completed = 0;
    uint8_t  burst_seen[MAX_BURSTS_PER_PAGE];
    memset(burst_seen, 0, sizeof(burst_seen));

    trans_burst_t cur_burst = {0};
    uint8_t  cur_burst_page_id = 0;
    uint8_t  cur_burst_id = 0;
    uint8_t  frames_in_burst = 0;
    uint8_t  frame_lengths[TRANS_MAX_FRAMES_PER_BURST];
    uint8_t  frames_received = 0;
    uint8_t  frame_received_mask[TRANS_MAX_FRAMES_PER_BURST];
    int      in_burst = 0;

    uint64_t compressed_total    = 0;
    uint64_t uncompressed_total  = 0;
    uint64_t rf_bytes_total      = 0;
    uint64_t rf_frames_total     = 0;

    while (!transfer_finished) {
        uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
        uint8_t len = sizeof(buf);

        int ret = nrf24_recv_blocking(&radio, buf, &len, 0);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            logger_error("nrf24_recv_blocking failed (errno=%d: %s). Assembling buffered data.",
                         errno, strerror(errno));
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

        if (!have_stream_info) {
            if (len == 32 && buf[0] == TRANS_MSG_INFO && buf[1] == TRANS_MSG_STREAM_INFO) {
                uint32_t tmp_total = 0;
                uint32_t tmp_comp[TRANS_NUM_PAGES] = {0};
                if (trans_parse_stream_info(buf, &tmp_total, tmp_comp) != 0) {
                    logger_error("P2P RX: failed to parse STREAM_INFO");
                    continue;
                }

                stream_total_orig = tmp_total;
                uint64_t total_comp_bytes = 0;
                uint32_t total_expected_bursts = 0;
                uint32_t total_expected_frames = 0;
                size_t active_pages = 0;

                for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
                    uint32_t comp_bytes = tmp_comp[i];
                    stream_page_orig_sizes[i] = compute_page_orig_len(stream_total_orig, i);
                    stream_page_expected_bursts[i] =
                        (uint16_t)((comp_bytes + TRANS_DATA_BYTES_PER_BURST - 1) /
                                   TRANS_DATA_BYTES_PER_BURST);
                    if (comp_bytes == 0) {
                        page_finished[i] = 1;
                    } else {
                        active_pages++;
                    }

                    uint32_t frames_for_page =
                        (comp_bytes + TRANS_DATA_BYTES_PER_FRAME - 1) /
                        TRANS_DATA_BYTES_PER_FRAME;

                    total_comp_bytes     += comp_bytes;
                    total_expected_bursts += stream_page_expected_bursts[i];
                    total_expected_frames += frames_for_page;

                    logger_info("P2P RX: STREAM_INFO page %zu -> raw=%zu B, comp=%u B, bursts=%u, frames=%u",
                                i,
                                stream_page_orig_sizes[i],
                                comp_bytes,
                                (unsigned)stream_page_expected_bursts[i],
                                frames_for_page);
                }

                logger_info("P2P RX: STREAM_INFO parsed -> total_raw=%u B, total_comp=%llu B, active_pages=%zu, expected_bursts=%u, expected_frames=%u",
                            stream_total_orig,
                            (unsigned long long)total_comp_bytes,
                            active_pages,
                            total_expected_bursts,
                            total_expected_frames);
                have_stream_info = 1;
                continue;
            } else {
                logger_warn("P2P RX: discarding frame (len=%u) before STREAM_INFO", len);
                continue;
            }
        }

        if (len >= 2 && buf[0] == TRANS_MSG_INFO) {
            if (buf[1] == TRANS_MSG_STREAM_INFO) {
                logger_warn("P2P RX: received duplicate STREAM_INFO, ignoring");
                continue;
            }

            if (buf[1] == TRANS_MSG_BURST_INFO) {
                uint8_t page_id = 0;
                uint8_t burst_id = 0;
                uint16_t burst_size = 0;
                if (trans_parse_burst_info(buf, len, &page_id, &burst_id, &burst_size) != 0) {
                    logger_warn("P2P RX: malformed BURST_INFO");
                    continue;
                }

                if (page_id >= TRANS_NUM_PAGES) {
                    logger_warn("P2P RX: BURST_INFO page_id=%u out of range", page_id);
                    continue;
                }

                if (cur_burst.frames) {
                    trans_free_burst(&cur_burst);
                }
                memset(frame_lengths, 0, sizeof(frame_lengths));
                memset(frame_received_mask, 0, sizeof(frame_received_mask));

                uint8_t layout_count = 0;
                if (trans_derive_frame_layout(burst_size,
                                              &layout_count,
                                              frame_lengths) != 0) {
                    logger_warn("P2P RX: invalid BurstSize=%u", burst_size);
                    continue;
                }

                cur_burst.frames = (trans_frame_t *)calloc(layout_count, sizeof(trans_frame_t));
                if (!cur_burst.frames) {
                    logger_error("P2P RX: calloc frames failed for burst %u", burst_id);
                    goto cleanup_rx;
                }
                cur_burst.frame_count = layout_count;

                cur_burst_page_id = page_id;
                cur_burst_id = burst_id;
                frames_in_burst = layout_count;
                frames_received = 0;
                in_burst = 1;

                if (page_id != current_page_id) {
                    if (!page_finished[page_id]) {
                        if (stream.count > 0 && !page_finished[current_page_id]) {
                            size_t prev_raw = stream_page_orig_sizes[current_page_id];
                            logger_warn("P2P RX: switching to page %u while %u has buffered data; flushing",
                                        page_id, current_page_id);
                            (void)decompress_page_to_buffer(&stream,
                                                            prev_raw,
                                                            out_buf,
                                                            &compressed_total,
                                                            &uncompressed_total);
                            page_finished[current_page_id] = 1;
                        }
                        page_stream_free(&stream);
                        page_stream_init(&stream);
                        memset(burst_seen, 0, sizeof(burst_seen));
                        bursts_completed = 0;
                        current_page_id = page_id;
                        current_expected_bursts = stream_page_expected_bursts[current_page_id];
                    } else {
                        current_expected_bursts = stream_page_expected_bursts[current_page_id];
                    }
                } else {
                    current_expected_bursts = stream_page_expected_bursts[current_page_id];
                }

                logger_info("P2P RX: BURST_INFO Page=%u Burst=%u -> size=%u bytes, frames=%u",
                            page_id, burst_id, burst_size, layout_count);
                continue;
            }

            if (buf[1] == TRANS_MSG_TRANSFER_FINISH) {
                logger_info("P2P RX: received TRANSFER_FINISH");
                transfer_finished = 1;
                break;
            }
        }

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
            logger_warn("P2P RX: FrameID=%u out of range (%u)", frame_id, frames_in_burst);
            continue;
        }

        uint8_t expected_len = frame_lengths[frame_id];
        if (expected_len == 0 || len != expected_len) {
            logger_warn("P2P RX: FrameID=%u len mismatch (got=%u expected=%u)",
                        frame_id, len, expected_len);
            continue;
        }

        memcpy(cur_burst.frames[frame_id].data, buf, len);
        cur_burst.frames[frame_id].len = len;

        if (!frame_received_mask[frame_id]) {
            frame_received_mask[frame_id] = 1;
            frames_received++;
        }

        if (frames_received < frames_in_burst) {
            continue;
        }

        uint64_t chk = trans_compute_burst_checksum(&cur_burst);
        uint8_t checksum_bytes[CHECKSUM_SIZE];
        trans_checksum_to_bytes(chk, checksum_bytes);

        logger_succ("P2P RX: completed BURST [P%u|B%u], checksum 0x%016llX",
                    cur_burst_page_id, cur_burst_id, (unsigned long long)chk);

        if (nrf24_set_mode_tx(&radio) < 0) {
            logger_error("nrf24_set_mode_tx failed");
            goto cleanup_rx;
        }

        const double send_window_ms = 500.0;
        double send_start = now_seconds();
        unsigned attempt = 0;
        int checksum_sent_ok = 0;

        while (!checksum_sent_ok &&
               (now_seconds() - send_start) * 1000.0 < send_window_ms) {
            rf_bytes_total  += CHECKSUM_SIZE;
            rf_frames_total += 1;

            if (nrf24_send_blocking(&radio,
                                    checksum_bytes,
                                    CHECKSUM_SIZE,
                                    CONTROL_TIMEOUT_MS) == 0) {
                checksum_sent_ok = 1;
                break;
            }

            if (errno != ETIMEDOUT) {
                logger_error("P2P RX: nrf24_send_blocking(CHECKSUM) failed: %s",
                             strerror(errno));
                goto cleanup_rx;
            }

            attempt++;
            if (attempt == 1 || (attempt % 50) == 0) {
                logger_warn("CHECKSUM resend attempt %u", attempt);
            }

            if ((attempt % 200) == 0) {
                logger_warn("P2P RX: %u checksum timeouts, reconfiguring radio", attempt);
                if (configure_radio_runtime(&radio) < 0) {
                    logger_error("P2P RX: radio reconfigure failed during checksum retry");
                    goto cleanup_rx;
                }
                if (nrf24_set_mode_tx(&radio) < 0) {
                    logger_error("nrf24_set_mode_tx failed after reconfigure");
                    goto cleanup_rx;
                }
            }
        }

        if (!checksum_sent_ok) {
            logger_warn("P2P RX: checksum timeout for Page %u Burst %u", cur_burst_page_id, cur_burst_id);
        }

        if (nrf24_set_mode_rx(&radio) < 0) {
            logger_error("nrf24_set_mode_rx failed");
            goto cleanup_rx;
        }

        in_burst = 0;

        int finished_page = page_finished[cur_burst_page_id];
        if (!finished_page) {
            if (store_burst(&stream, cur_burst_id, &cur_burst) != 0) {
                logger_error("P2P RX: failed to store burst %u", cur_burst_id);
                goto cleanup_rx;
            }

            if (cur_burst_id < MAX_BURSTS_PER_PAGE && !burst_seen[cur_burst_id]) {
                burst_seen[cur_burst_id] = 1;
                bursts_completed++;
            }
        } else {
            trans_free_burst(&cur_burst);
        }

        cur_burst.frames = NULL;
        cur_burst.frame_count = 0;

        if (!finished_page &&
            current_expected_bursts > 0 &&
            bursts_completed >= current_expected_bursts &&
            !page_finished[current_page_id]) {

            size_t expected_raw = stream_page_orig_sizes[current_page_id];
            logger_succ("P2P RX: all %u bursts received for Page %u; decompressing",
                        current_expected_bursts, current_page_id);

            (void)decompress_page_to_buffer(&stream,
                                            expected_raw,
                                            out_buf,
                                            &compressed_total,
                                            &uncompressed_total);

            page_finished[current_page_id] = 1;
            page_stream_free(&stream);
            page_stream_init(&stream);
            memset(burst_seen, 0, sizeof(burst_seen));
            bursts_completed = current_expected_bursts;
        }
    }

    if (stream.count > 0 && current_page_id < TRANS_NUM_PAGES &&
        !page_finished[current_page_id]) {
        logger_warn("P2P RX: flushing partial page %u", current_page_id);
        size_t expected_raw = stream_page_orig_sizes[current_page_id];
        (void)decompress_page_to_buffer(&stream,
                                        expected_raw,
                                        out_buf,
                                        &compressed_total,
                                        &uncompressed_total);
    }

    double t_end = now_seconds();
    double dt    = (tx_started ? (t_end - t_start) : 0.0);

    double user_kibps = (dt > 0.0)
        ? ((double)uncompressed_total / 1024.0 / dt)
        : 0.0;

    double rf_kibps = (dt > 0.0)
        ? ((double)rf_bytes_total / 1024.0 / dt)
        : 0.0;

    logger_succ("P2P RX: done. Compressed %llu B -> %llu B in %.3f s (%.1f KiB/s user, %.1f KiB/s RF, %llu frames)",
                (unsigned long long)compressed_total,
                (unsigned long long)uncompressed_total,
                dt,
                user_kibps,
                rf_kibps,
                (unsigned long long)rf_frames_total);

    exit_code = 0;
    goto cleanup_rx;

cleanup_rx:
    if (cur_burst.frames) {
        trans_free_burst(&cur_burst);
    }
    page_stream_free(&stream);
    nrf24_deinit(&radio);
    return exit_code;
}

/* ---- CLI ---- */

int main(int argc, char **argv)
{
    app_config_t cfg;
    if (app_parse_arguments(argc, argv, &cfg) != 0) {
        app_print_usage(argv[0]);
        return 1;
    }

    if (cfg.print_config) {
        app_print_config(&cfg);
    }

    char log_path[64];
    if (cfg.mode == APP_MODE_TX) {
        snprintf(log_path, sizeof(log_path), "p3p_tx.log");
    } else if (cfg.mode == APP_MODE_RX) {
        snprintf(log_path, sizeof(log_path), "p3p_rx.log");
    } else {
        snprintf(log_path, sizeof(log_path), "p3p.log");
    }

    if (logger_init(log_path) != 0) {
        logger_warn("Could not open log file '%s' (continuing without file log)", log_path);
    } else {
        logger_info("Logging to file '%s'", log_path);
    }

    const char *spi_dev = get_spi_device_path();
    update_radio_runtime_from_config(&cfg);

    logger_info("Using SPI device: %s", spi_dev);

    int exit_code = 0;

    if (cfg.mode == APP_MODE_TX) {
        uint8_t *data = NULL;
        size_t   len  = 0;
        if (app_load_file_bytes(cfg.file_path_tx, &data, &len) != 0) {
            logger_error("Failed to load TX file bytes");
            exit_code = 1;
            goto cleanup;
        }

        exit_code = run_tx(spi_dev, &cfg, data, len);
        free(data);
        goto cleanup;
    }

    if (cfg.mode == APP_MODE_RX) {
        RxOutputBuffer output;
        int ret = run_rx(spi_dev, &cfg, &output);
        if (ret != 0) {
            rx_output_free(&output);
            exit_code = ret;
            goto cleanup;
        }

        if (app_store_file_bytes(cfg.file_path_rx, output.data, output.len) != 0) {
            logger_error("Failed to store RX bytes");
            rx_output_free(&output);
            exit_code = 1;
            goto cleanup;
        }

        logger_succ("Stored RX payload (%zu bytes)", output.len);
        rx_output_free(&output);
        goto cleanup;
    }

    logger_error("Unsupported mode: %s", app_mode_str(cfg.mode));
    exit_code = 1;

cleanup:
    logger_close();
    return exit_code;
}
