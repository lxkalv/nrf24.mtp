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
#define MAX_BURSTS_PER_PAGE  255   /* burst_id is 8-bit on the air */
#define MAX_PAGES            16    /* safety margin for page_finished array */

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

typedef struct Burst Burst;
typedef struct PageStream PageStream;

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
        Burst *b = &ps->bursts[bid];
        if (b->frames_in_burst == 0) continue;

        for (unsigned i = 0; i < b->frames_in_burst; ++i) {
            uint8_t *frame = b->frame_data[i];
            uint8_t  flen  = b->frame_len[i];
            if (!frame || flen <= 1) continue;

            size_t payload_len = flen - 1;
            if (rx_output_append(&tmp, frame + 1, payload_len) != 0) {
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

struct Burst {
    unsigned frames_in_burst;
    uint8_t *frame_data[MAX_CHUNKS_PER_BURST];
    uint8_t  frame_len[MAX_CHUNKS_PER_BURST];
};

struct PageStream {
    Burst  *bursts;
    size_t  count;
    size_t  capacity;
};

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

    uint64_t orig_len = (uint64_t)input_len;
    const char *input_label = cfg->file_path_tx ? cfg->file_path_tx : "(auto-selected)";

    size_t total_pages = compressed_page_count;
    if (total_pages > 0xFF) {
        logger_warn("P2P TX: total_pages=%zu exceeds 255, truncating", total_pages);
        total_pages = 0xFF;
    }

    logger_info("P2P TX: sending '%s' (%llu bytes, %zu presentation pages)",
         input_label, (unsigned long long)orig_len, total_pages);

    double t_start = now_seconds();

    uint64_t tx_rf_bytes_total  = 0;  /* all on-air bytes (incl. retransmissions) */
    uint64_t tx_rf_frames_total = 0;  /* all frames actually sent */

    for (size_t page_id = 0; page_id < total_pages; ++page_id) {
        const pres_page_t *page = &compressed_pages[page_id];
        const uint8_t *comp_page = page->data;
        size_t comp_len = page->size;
        size_t page_len = page->orig_size;

        double ratio = (page_len == 0)
                     ? 0.0
                     : (100.0 * (double)comp_len / (double)page_len);

        logger_info("P2P TX: Page %zu compressed %zu -> %zu bytes (~%.2f%%)",
             page_id, page_len, comp_len, ratio);

        uint16_t bursts_in_page     = 0;
        uint8_t  last_burst_frames  = 0;
        uint8_t  last_frame_bytes   = 0;

        if (comp_len > 0) {
            bursts_in_page = (uint16_t)((comp_len + BURST_DATA_BYTES - 1) / BURST_DATA_BYTES);
            if (bursts_in_page > MAX_BURSTS_PER_PAGE) {
                logger_warn("P2P TX: bursts_in_page=%u > MAX_BURSTS_PER_PAGE=%u; truncating",
                     bursts_in_page, (unsigned)MAX_BURSTS_PER_PAGE);
                bursts_in_page = MAX_BURSTS_PER_PAGE;
            }

            size_t last_burst_bytes =
                comp_len - (size_t)(bursts_in_page - 1) * BURST_DATA_BYTES;

            last_burst_frames = (uint8_t)((last_burst_bytes + CHUNK_DATA_BYTES - 1) /
                                          CHUNK_DATA_BYTES);

            size_t used_by_prev_frames =
                (size_t)(last_burst_frames - 1) * CHUNK_DATA_BYTES;
            last_frame_bytes = (uint8_t)(last_burst_bytes - used_by_prev_frames);
            if (last_frame_bytes == 0) {
                last_frame_bytes = CHUNK_DATA_BYTES;
            }
        }

        uint8_t stream_info[12];
        stream_info[0] = MSG_INFO;
        stream_info[1] = P2P_MSG_STREAM_INFO;
        stream_info[2] = (uint8_t)page_id;
        stream_info[3] = (uint8_t)total_pages;
        encode_u16_le(&stream_info[4], bursts_in_page);
        stream_info[6] = last_burst_frames;
        stream_info[7] = last_frame_bytes;
        encode_u32_le(&stream_info[8], (uint32_t)page_len);

        logger_info("P2P TX: STREAM_INFO Page=%zu/%zu -> bursts=%u, last_frames=%u, last_frame_bytes=%u, orig=%zu",
             page_id, total_pages, (unsigned)bursts_in_page,
             (unsigned)last_burst_frames, (unsigned)last_frame_bytes, page_len);

        (void)send_with_retries(&radio,
                                stream_info,
                                sizeof(stream_info),
                                CONTROL_TIMEOUT_MS,
                                "STREAM_INFO",
                                &tx_rf_bytes_total,
                                &tx_rf_frames_total);

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

            logger_info("P2P TX: Page %zu, BURST %u -> %u compressed bytes in %u frames, checksum 0x%016llX",
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
                                      &tx_rf_frames_total) < 0) {
                    logger_error("Failed to send BURST_INFO (page %zu, burst %u), aborting",
                          page_id, burst_id);
                    goto cleanup;
                }

                for (unsigned i = 0; i < num_chunks; ++i) {
                    if (send_with_retries(&radio,
                                          burst_payloads[i],
                                          burst_sizes[i],
                                          DATA_TIMEOUT_MS,
                                          "DATA",
                                          &tx_rf_bytes_total,
                                          &tx_rf_frames_total) < 0) {
                        logger_error("Failed to send DATA frame (page %zu, burst %u), aborting",
                              page_id, burst_id);
                        goto cleanup;
                    }
                }

                if (nrf24_set_mode_rx(&radio) < 0) {
                    logger_error("nrf24_set_mode_rx failed");
                    goto cleanup;
                }

                double wait_start         = now_seconds();
                int    got_valid_checksum = 0;

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
                        goto cleanup;
                    }

                    if (len2 != CHECKSUM_SIZE) {
                        logger_warn("P2P TX: received non-checksum frame of %u bytes while waiting", len2);
                        continue;
                    }

                    if (memcmp(buf2, checksum_bytes, CHECKSUM_SIZE) == 0) {
                        logger_succ("P2P TX: Page %zu, BURST %u checksum confirmed by RX",
                             page_id, burst_id);
                        got_valid_checksum = 1;
                    } else {
                        logger_warn("P2P TX: invalid checksum received for Page %zu, BURST %u",
                             page_id, burst_id);
                    }
                }

                if (!got_valid_checksum) {
                    logger_warn("P2P TX: checksum timeout for Page %zu, BURST %u, resending burst",
                         page_id, burst_id);
                    if (nrf24_set_mode_tx(&radio) < 0) {
                        logger_error("nrf24_set_mode_tx failed");
                        goto cleanup;
                    }
                    continue;
                }

                burst_done = 1;

                if (nrf24_set_mode_tx(&radio) < 0) {
                    logger_error("nrf24_set_mode_tx failed");
                    goto cleanup;
                }
            }

            burst_id++;
        }
    }

    uint8_t fin_msg[2];
    fin_msg[0] = MSG_INFO;
    fin_msg[1] = MSG_TRANSFER_FINISH;

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

    logger_succ("P2P TX: done. User: %llu bytes in %.3f s (%.1f KiB/s). "
         "RF on-air: %llu bytes in %.3f s (%.1f KiB/s, %llu frames).",
         (unsigned long long)orig_len, dt, user_kibps,
         (unsigned long long)tx_rf_bytes_total, dt, rf_kibps,
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

    logger_info("P2P RX: waiting for STREAM_INFO / BURST_INFO on channel %u...",
         (unsigned)g_radio_runtime.channel);

    int transfer_finished = 0;
    int tx_started        = 0;
    double t_start        = 0.0;

    /* Per-page "finished" flags: once a page is appended, we won't append again,
       but we will still answer duplicate bursts with checksums. */
    int page_finished[MAX_PAGES];
    memset(page_finished, 0, sizeof(page_finished));
     size_t page_orig_sizes[MAX_PAGES];
     memset(page_orig_sizes, 0, sizeof(page_orig_sizes));

    /* Per-page state (current page in progress) */
    int      have_page_info      = 0;
    uint8_t  current_page_id     = 0;
    uint8_t  total_pages         = 0;
    uint16_t expected_bursts     = 0;
    uint8_t  last_burst_frames   = 0;
    uint8_t  last_frame_bytes    = 0;
     size_t   current_page_orig_size = 0;
    uint8_t  burst_received[MAX_BURSTS_PER_PAGE];
    unsigned bursts_completed    = 0;
    int      page_has_data       = 0;  /* any stored bursts? */

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

        /* STREAM_INFO: per-page layout */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == P2P_MSG_STREAM_INFO) {
            if (len < 12) {
                logger_warn("P2P RX: malformed STREAM_INFO frame (len=%u)", len);
                continue;
            }

            /* If we had an unfinished page with data, decompress it before moving on */
            if (have_page_info && page_has_data) {
                logger_warn("P2P RX: new STREAM_INFO while previous page %u has data; "
                     "decompressing previous page as-is",
                     (unsigned)current_page_id);

                if (current_page_id < MAX_PAGES && !page_finished[current_page_id]) {
                    size_t prev_orig = page_orig_sizes[current_page_id];
                    (void)decompress_page_to_buffer(&stream,
                                                    prev_orig,
                                                    out_buf,
                                                    &compressed_total,
                                                    &uncompressed_total);
                    page_finished[current_page_id] = 1;
                }
                page_stream_free(&stream);
                page_stream_init(&stream);
                page_has_data    = 0;
                bursts_completed = 0;
                memset(burst_received, 0, sizeof(burst_received));
            }

            current_page_id = buf[2];
            total_pages     = buf[3];
            expected_bursts = decode_u16_le(&buf[4]);
            last_burst_frames = buf[6];
            last_frame_bytes  = buf[7];
            current_page_orig_size = decode_u32_le(&buf[8]);

            if (current_page_id < MAX_PAGES) {
                page_orig_sizes[current_page_id] = current_page_orig_size;
            }

            if (expected_bursts > MAX_BURSTS_PER_PAGE) {
                logger_warn("P2P RX: expected_bursts=%u > MAX_BURSTS_PER_PAGE=%u, truncating",
                     (unsigned)expected_bursts, (unsigned)MAX_BURSTS_PER_PAGE);
                expected_bursts = MAX_BURSTS_PER_PAGE;
            }

            memset(burst_received, 0, sizeof(burst_received));
            bursts_completed = 0;
            page_has_data    = 0;
            in_burst         = 0;
            have_page_info   = 1;

                logger_info("P2P RX: STREAM_INFO Page=%u/%u -> bursts=%u, last_frames=%u, last_frame_bytes=%u, orig=%zu",
                                (unsigned)current_page_id, (unsigned)total_pages,
                                (unsigned)expected_bursts,
                                (unsigned)last_burst_frames, (unsigned)last_frame_bytes,
                                current_page_orig_size);
            continue;
        }

        /* BURST_INFO */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == MSG_BURST_INFO) {
            if (len < 6) {
                logger_warn("P2P RX: malformed BURST_INFO frame");
                continue;
            }

            uint8_t page_id  = buf[2];
            uint8_t burst_id = buf[3];
            uint16_t size_of_burst = decode_u16_le(&buf[4]);

            /* Accept BURST_INFO if:
             *  - it matches the current page (have_page_info && page_id == current_page_id), OR
             *  - it's for a page we've already finished (we'll just re-ACK checksums).
             */
            int is_current_page = (have_page_info && page_id == current_page_id);
            int is_finished_page = (page_id < MAX_PAGES && page_finished[page_id]);

            if (!is_current_page && !is_finished_page) {
                logger_warn("P2P RX: BURST_INFO for unexpected page=%u (current=%u), ignoring",
                     (unsigned)page_id, (unsigned)current_page_id);
                continue;
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
        page_has_data = 1;

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
                    rx_output_free(out_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }

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
                rx_output_free(out_buf);
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
                    rx_output_free(out_buf);
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
                    if (configure_radio_runtime(&radio) < 0) {
                        logger_error("P2P RX: radio reconfigure failed: %s", strerror(errno));
                        page_stream_free(&stream);
                        rx_output_free(out_buf);
                        nrf24_deinit(&radio);
                        return 1;
                    }
                    (void)nrf24_set_mode_tx(&radio);
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
                    rx_output_free(out_buf);
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
                rx_output_free(out_buf);
                nrf24_deinit(&radio);
                return 1;
            }

            /* 4) If this is the "active" page and we know how many bursts it
             * should have, and we've seen them all at least once, decompress
             * this page ONCE and mark it as finished.
             */
            if (!is_finished_page &&
                have_page_info &&
                cur_burst_page_id == current_page_id &&
                expected_bursts > 0 &&
                bursts_completed >= expected_bursts &&
                current_page_id < MAX_PAGES &&
                !page_finished[current_page_id]) {

                logger_succ("P2P RX: all %u bursts received for Page %u; decompressing page",
                     (unsigned)expected_bursts, (unsigned)current_page_id);

                size_t finished_orig =
                    (current_page_id < MAX_PAGES)
                        ? page_orig_sizes[current_page_id]
                        : current_page_orig_size;

                (void)decompress_page_to_buffer(&stream,
                                                finished_orig,
                                                out_buf,
                                                &compressed_total,
                                                &uncompressed_total);

                page_finished[current_page_id] = 1;

                page_stream_free(&stream);
                page_stream_init(&stream);
                page_has_data    = 0;

                /* Keep have_page_info = 1 so that if TX still resends bursts
                 * for this page, we will recompute and send checksums, but the
                 * page won't be appended again thanks to page_finished[].
                 */
                bursts_completed = expected_bursts;
                memset(burst_received, 0, sizeof(burst_received));
            }
        }
    }

    /* If we exit the loop without TRANSFER_FINISH, we may still have a partial page
     * buffered. Try to decompress what we have (once).
     */
    if (have_page_info && page_has_data &&
        current_page_id < MAX_PAGES && !page_finished[current_page_id]) {
        logger_warn("P2P RX: transfer ended unexpectedly; decompressing partial Page %u",
             (unsigned)current_page_id);
           size_t partial_orig = page_orig_sizes[current_page_id];
           (void)decompress_page_to_buffer(&stream,
                                    partial_orig,
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

    logger_succ("P2P RX: done. Compressed %llu B -> %llu B uncompressed in %.3f s "
         "(%.1f KiB/s user data, %.1f KiB/s RF on-air, %llu RF frames).",
         (unsigned long long)compressed_total,
         (unsigned long long)uncompressed_total,
         dt, user_kibps, rf_kibps,
         (unsigned long long)rf_frames_total);

    page_stream_free(&stream);
    nrf24_deinit(&radio);
    return 0;
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
