// =============================================================
// p3p_mode.c
// High-level file transfer mode using all protocol layers
// =============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <sys/stat.h> 

#include "libs/logger.h"
#include "libs/app_layer.h"
#include "libs/presentation_layer.h"
#include "libs/transport_layer.h"
#include "libs/link_layer.h"
#include "libs/nrf24.h"

// =============================================================
// General constants
// =============================================================

#define LOGS_DIR               "logs"
#define LOG_FILE_TX_PREFIX     "p3p_tx_"
#define LOG_FILE_RX_PREFIX     "p3p_rx_"
#define LOG_FILE_EXT           ".log"

#define SPI_DEVICE_DEFAULT     "/dev/spidev0.0"
#define SPI_SPEED_HZ_DEFAULT   8000000U
#define BURST_ACK_TIMEOUT_MS   1000U   // Wait for checksum ACK (ms)
#define BURST_MAX_RETRIES      5

// =============================================================
// NRF24 <-> Link layer glue
// =============================================================

typedef struct {
    nrf24_t dev;
} radio_ctx_t;

static int radio_send(void *ctx, const uint8_t *payload, size_t len)
{
    if (!ctx || !payload || len == 0) return -1;
    radio_ctx_t *r = (radio_ctx_t *)ctx;
    return nrf24_send_blocking(&r->dev, payload, len, 500);
}

static int radio_wait_until_sent(void *ctx)
{
    (void)ctx;
    // Blocking send already waits, so just return 0
    return 0;
}

/*
static void radio_reset_packages_lost(void *ctx)
{
    if (!ctx) return;
    radio_ctx_t *r = (radio_ctx_t *)ctx;
    nrf24_reset_packages_lost(&r->dev);
}
*/

static void radio_reset_packages_lost(void *ctx)
{
    (void)ctx;
    /* No-op for now: we rely on nRF24 auto-ack internally. */
}

/*
static int radio_get_packages_lost(void *ctx)
{
    if (!ctx) return -1;
    radio_ctx_t *r = (radio_ctx_t *)ctx;
    return nrf24_get_packages_lost(&r->dev);
}
*/

static int radio_get_packages_lost(void *ctx)
{
    (void)ctx;
    /* No per-frame lost counter exposed -> assume 0 for link_layer. */
    return 0;
}

/*
static int radio_data_ready(void *ctx)
{
    if (!ctx) return -1;
    radio_ctx_t *r = (radio_ctx_t *)ctx;
    return nrf24_data_ready(&r->dev);
}
*/

static int radio_data_ready(void *ctx)
{
    (void)ctx;
    /* We use blocking receive in radio_read_payload, so always "ready". */
    return 1;
}


static int radio_read_payload(void *ctx, uint8_t *buf, size_t *len)
{
    if (!ctx || !buf || !len || *len == 0) return -1;
    radio_ctx_t *r = (radio_ctx_t *)ctx;

    if (*len > NRF24_MAX_PAYLOAD_SIZE) {
        *len = NRF24_MAX_PAYLOAD_SIZE;
    }

    uint8_t l = (uint8_t)*len;
    /* 1000 ms timeout for any frame */
    int rc = nrf24_recv_blocking(&r->dev, buf, &l, 1000);
    if (rc != 0) {
        return -1;
    }

    *len = l;
    return 0;
}

// =============================================================
// Helper: build timestamped log filename
// =============================================================

static void make_log_filename(const char *prefix, char *out_path, size_t out_sz)
{
    char ts[32];
    logger_timestamp(ts, sizeof(ts));
    snprintf(out_path, out_sz, "%s/%s%s%s", LOGS_DIR, prefix, ts, LOG_FILE_EXT);
}

// =============================================================
// Function declarations (defined later)
// =============================================================

static int run_tx_mode(const app_config_t *cfg, link_radio_iface_t *iface);
static int run_rx_mode(const app_config_t *cfg, link_radio_iface_t *iface);

// =============================================================
// Main entry
// =============================================================

int main(int argc, char **argv)
{
    int ret = EXIT_SUCCESS;
    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    // 1. Parse CLI
    if (app_parse_arguments(argc, argv, &cfg) != 0) {
        app_print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    // 2. Prepare logs
    char log_path[256];
    
    #if defined(_WIN32)
        mkdir(LOGS_DIR);
    #else
        mkdir(LOGS_DIR, 0777);
    #endif

    make_log_filename(cfg.mode == APP_MODE_TX ? LOG_FILE_TX_PREFIX : LOG_FILE_RX_PREFIX,
                      log_path, sizeof(log_path));

    logger_init(log_path);
    logger_set_level(LOGGER_LEVEL_INFO);

    logger_info("=== P3P MODE STARTED ===");

    if (cfg.print_config) {
        app_print_config(&cfg);
    }

    // 3. Init radio
    radio_ctx_t rctx;
    memset(&rctx, 0, sizeof(rctx));

    nrf24_config_t ncfg = {
        .spi_device   = SPI_DEVICE_DEFAULT,
        .spi_speed_hz = SPI_SPEED_HZ_DEFAULT,
        .ce_gpio      = cfg.ce_pin
    };

    if (nrf24_init(&rctx.dev, &ncfg) != 0) {
        logger_error("Failed to initialize nRF24");
        return EXIT_FAILURE;
    }

        /* ---- Map app_config_t → nRF24 parameters ---- */

    unsigned int data_rate_kbps;
    switch (cfg.data_rate) {
    case APP_DATA_RATE_250KBPS:
        data_rate_kbps = 250;
        break;
    case APP_DATA_RATE_1MBPS:
        data_rate_kbps = 1000;
        break;
    case APP_DATA_RATE_2MBPS:
        data_rate_kbps = 2000;
        break;
    default:
        ERROR("Invalid data rate in app config: %d", (int)cfg.data_rate);
        goto out;
    }

    int pa_level_dbm;
    switch (cfg.pa_level) {
    case APP_PA_MIN:
        pa_level_dbm = -18;
        break;
    case APP_PA_LOW:
        pa_level_dbm = -12;
        break;
    case APP_PA_HIGH:
        pa_level_dbm = -6;
        break;
    case APP_PA_MAX:
        pa_level_dbm = 0;
        break;
    default:
        ERROR("Invalid PA level in app config: %d", (int)cfg.pa_level);
        goto out;
    }

    unsigned int crc_bytes;
    switch (cfg.crc_bytes) {
    case APP_CRC_OFF:
        crc_bytes = 0;
        break;
    case APP_CRC_8:
        crc_bytes = 1;
        break;
    case APP_CRC_16:
        crc_bytes = 2;
        break;
    default:
        ERROR("Invalid CRC bytes in app config: %d", (int)cfg.crc_bytes);
        goto out;
    }

    /* retransmission_delay and retransmission_tries are already 0..15 */
    if (nrf24_configure_advanced(&rctx.dev,
                                 cfg.channel,
                                 data_rate_kbps,
                                 pa_level_dbm,
                                 crc_bytes,
                                 cfg.retransmission_delay,
                                 cfg.retransmission_tries) < 0) {
        ERROR("Failed to configure nRF24 radio: %s", strerror(errno));
        goto out;
    }

    INFO("Radio configured on channel %u", cfg.channel);

    if (cfg.print_config) {
        (void)nrf24_dump_config(&rctx.dev);
    }

    logger_info("Radio configured on channel %u", cfg.channel);

    if (cfg.print_config) {
        nrf24_dump_config(&rctx.dev);
    }

    // 4. Build link interface
    link_radio_iface_t iface = {
        .send               = radio_send,
        .wait_until_sent    = radio_wait_until_sent,
        .reset_packages_lost = radio_reset_packages_lost,
        .get_packages_lost   = radio_get_packages_lost,
        .data_ready         = radio_data_ready,
        .read_payload       = radio_read_payload,
        .user_ctx           = &rctx
    };

    // 5. Dispatch mode
    if (cfg.mode == APP_MODE_TX) {
        nrf24_set_mode_tx(&rctx.dev);
        ret = run_tx_mode(&cfg, &iface);
    } else if (cfg.mode == APP_MODE_RX) {
        nrf24_set_mode_rx(&rctx.dev);
        ret = run_rx_mode(&cfg, &iface);
    } else {
        logger_error("Unknown mode");
        ret = EXIT_FAILURE;
    }

    logger_info("P3P finished with code %d", ret);
    logger_close();
    nrf24_deinit(&rctx.dev);
    return ret;
}

// =============================================================
// Helper types for RX
// =============================================================

typedef struct {
    uint8_t  *data;           /* Compressed page bytes */
    uint32_t expected_size;   /* Compressed size (from STREAM_INFO) */
    uint32_t received_size;   /* How many bytes we have stored */
    uint16_t max_bursts;      /* Max bursts this page may use */
    bool    *burst_received;  /* Flags per burst_id */
    bool     done;            /* True when received_size == expected_size */
} page_rx_t;

// =============================================================
// Helper: compute original page sizes from total size
// (must match pres_split_into_pages_default logic)
// =============================================================

static void compute_page_orig_sizes(uint32_t total_orig_size,
                                    size_t page_count,
                                    size_t *out_sizes)
{
    if (!out_sizes || page_count == 0) {
        return;
    }

    size_t total   = (size_t)total_orig_size;
    size_t base    = total / page_count;
    size_t rem     = total % page_count;

    for (size_t i = 0; i < page_count; ++i) {
        out_sizes[i] = base + ((i < rem) ? 1 : 0);
    }
}

// =============================================================
// Helper: check if all pages with expected_size > 0 are done
// =============================================================

static bool all_pages_completed(const page_rx_t *pages,
                                const uint32_t comp_sizes[],
                                size_t page_count)
{
    if (!pages || !comp_sizes) return false;

    for (size_t i = 0; i < page_count; ++i) {
        if (comp_sizes[i] == 0) {
            continue;
        }
        if (!pages[i].done) {
            return false;
        }
    }
    return true;
}

// =============================================================
// Helper: TX – send one burst with checksum handshake
// =============================================================

static int tx_send_burst_with_retry(const app_config_t *cfg,
                                    const link_radio_iface_t *iface,
                                    uint8_t page_id,
                                    uint8_t burst_id,
                                    const trans_burst_t *burst)
{
    (void)cfg; /* unused for now, kept for future policy tuning */

    if (!iface || !burst) {
        logger_error("tx_send_burst_with_retry: invalid arguments");
        return -1;
    }

    const radio_ctx_t *rctx = (const radio_ctx_t *)iface->user_ctx;

    uint64_t expected_checksum = trans_compute_burst_checksum(burst);
    uint8_t  checksum_bytes[8];
    trans_checksum_to_bytes(expected_checksum, checksum_bytes);

    for (unsigned int attempt = 0; attempt < BURST_MAX_RETRIES; ++attempt) {
        logger_info("TX: Page %u Burst %u attempt %u",
                    (unsigned)page_id, (unsigned)burst_id, attempt + 1);

        uint8_t info[6];
        trans_build_burst_info(page_id, burst_id, burst, info);

        if (link_send_frame(iface, info, sizeof(info)) != LINK_STATUS_OK) {
            logger_error("TX: failed to send BURST_INFO (page %u, burst %u)",
                         (unsigned)page_id, (unsigned)burst_id);
            return -1;
        }

        /* Send all frames in this burst */
        for (size_t f = 0; f < burst->frame_count; ++f) {
            const trans_frame_t *fr = &burst->frames[f];

            if (fr->len == 0 || fr->len > TRANS_FRAME_MAX_LEN) {
                logger_error("TX: invalid frame length %u in burst", fr->len);
                return -1;
            }

            if (link_send_frame(iface, fr->data, fr->len) != LINK_STATUS_OK) {
                logger_error("TX: failed to send frame %zu in burst", f);
                return -1;
            }
        }

        /* Switch to RX to wait for checksum ACK */
        nrf24_set_mode_rx(&((radio_ctx_t *)rctx)->dev);

        uint8_t ack_buf[32];
        size_t  ack_len = sizeof(ack_buf);

        link_status_t st = link_read_frame(iface, ack_buf, &ack_len);
        if (st != LINK_STATUS_OK) {
            logger_warn("TX: did not receive checksum ACK (attempt %u)", attempt + 1);
            nrf24_set_mode_tx(&((radio_ctx_t *)rctx)->dev);
            continue;
        }

        if (ack_len != 8) {
            logger_warn("TX: checksum ACK has wrong length %zu (expected 8)", ack_len);
            nrf24_set_mode_tx(&((radio_ctx_t *)rctx)->dev);
            continue;
        }

        uint64_t recv_cs = trans_checksum_from_bytes(ack_buf);
        if (recv_cs == expected_checksum) {
            logger_info("TX: checksum OK for Page %u Burst %u",
                        (unsigned)page_id, (unsigned)burst_id);
            nrf24_set_mode_tx(&((radio_ctx_t *)rctx)->dev);
            return 0;
        }

        logger_warn("TX: checksum mismatch for Page %u Burst %u "
                    "(expected 0x%016llX, got 0x%016llX)",
                    (unsigned)page_id, (unsigned)burst_id,
                    (unsigned long long)expected_checksum,
                    (unsigned long long)recv_cs);

        nrf24_set_mode_tx(&((radio_ctx_t *)rctx)->dev);
    }

    logger_error("TX: giving up on Page %u Burst %u after %u attempts",
                 (unsigned)page_id, (unsigned)burst_id, BURST_MAX_RETRIES);
    return -1;
}

// =============================================================
// TX mode implementation
// =============================================================

static int run_tx_mode(const app_config_t *cfg, link_radio_iface_t *iface)
{
    if (!cfg || !iface) {
        logger_error("run_tx_mode: invalid arguments");
        return EXIT_FAILURE;
    }

    uint8_t *file_data = NULL;
    size_t   file_len  = 0;

    if (app_load_file_bytes(cfg->file_path_tx, &file_data, &file_len) != 0) {
        logger_error("TX: failed to load input file");
        return EXIT_FAILURE;
    }

    logger_info("TX: loaded file with %zu bytes", file_len);

    /* Split into pages and compress */
    pres_page_t *pages_raw  = NULL;
    pres_page_t *pages_comp = NULL;
    size_t       page_count = 0;
    size_t       comp_count = 0;

    if (pres_split_into_pages_default(file_data, file_len,
                                      &pages_raw, &page_count) != 0) {
        logger_error("TX: pres_split_into_pages_default failed");
        free(file_data);
        return EXIT_FAILURE;
    }

    if (page_count > TRANS_NUM_PAGES) {
        logger_error("TX: page_count=%zu exceeds TRANS_NUM_PAGES=%u",
                     page_count, (unsigned)TRANS_NUM_PAGES);
        pres_free_pages(pages_raw, page_count);
        free(file_data);
        return EXIT_FAILURE;
    }

    if (pres_compress_pages(pages_raw, page_count,
                            &pages_comp, &comp_count) != 0) {
        logger_error("TX: pres_compress_pages failed");
        pres_free_pages(pages_raw, page_count);
        free(file_data);
        return EXIT_FAILURE;
    }

    if (comp_count != page_count) {
        logger_warn("TX: compressed page count (%zu) differs from raw (%zu)",
                    comp_count, page_count);
    }

    /* Build compressed sizes array for STREAM_INFO */
    uint32_t comp_sizes[TRANS_NUM_PAGES] = {0};

    for (size_t i = 0; i < comp_count && i < TRANS_NUM_PAGES; ++i) {
        if (pages_comp[i].size > 0x1FFFFFu) { /* 21-bit max */
            logger_error("TX: page %zu compressed size %zu exceeds 21-bit field",
                         i, pages_comp[i].size);
            pres_free_pages(pages_comp, comp_count);
            pres_free_pages(pages_raw, page_count);
            free(file_data);
            return EXIT_FAILURE;
        }
        comp_sizes[i] = (uint32_t)pages_comp[i].size;
    }

    /* Build and send STREAM_INFO */
    uint8_t stream_info[32];
    if (trans_build_stream_info((uint32_t)file_len, comp_sizes, stream_info) != 0) {
        logger_error("TX: trans_build_stream_info failed");
        pres_free_pages(pages_comp, comp_count);
        pres_free_pages(pages_raw, page_count);
        free(file_data);
        return EXIT_FAILURE;
    }

    logger_info("TX: sending STREAM_INFO");
    if (link_send_frame(iface, stream_info, sizeof(stream_info)) != LINK_STATUS_OK) {
        logger_error("TX: failed to send STREAM_INFO");
        pres_free_pages(pages_comp, comp_count);
        pres_free_pages(pages_raw, page_count);
        free(file_data);
        return EXIT_FAILURE;
    }

    /* Send all pages, burst by burst */
    for (size_t p = 0; p < comp_count; ++p) {
        const pres_page_t *pg = &pages_comp[p];

        if (!pg->data || pg->size == 0) {
            logger_info("TX: skipping empty page %zu", p);
            continue;
        }

        logger_info("TX: splitting Page %zu (%zu bytes compressed) into bursts",
                    p, pg->size);

        trans_burst_t *bursts     = NULL;
        size_t         burst_count = 0;

        if (trans_split_page_into_bursts(pg->data, pg->size,
                                         &bursts, &burst_count) != 0) {
            logger_error("TX: trans_split_page_into_bursts failed for page %zu", p);
            pres_free_pages(pages_comp, comp_count);
            pres_free_pages(pages_raw, page_count);
            free(file_data);
            return EXIT_FAILURE;
        }

        for (size_t b = 0; b < burst_count; ++b) {
            if (tx_send_burst_with_retry(cfg, iface,
                                         (uint8_t)p, (uint8_t)b,
                                         &bursts[b]) != 0) {
                logger_error("TX: failed to transmit Page %zu Burst %zu", p, b);
                trans_free_bursts(bursts, burst_count);
                pres_free_pages(pages_comp, comp_count);
                pres_free_pages(pages_raw, page_count);
                free(file_data);
                return EXIT_FAILURE;
            }
        }

        trans_free_bursts(bursts, burst_count);
        logger_info("TX: finished Page %zu", p);
    }

    /* Send TRANSFER_FINISH */
    uint8_t finish_msg[2];
    trans_build_transfer_finish(finish_msg);
    logger_info("TX: sending TRANSFER_FINISH");
    if (link_send_frame(iface, finish_msg, sizeof(finish_msg)) != LINK_STATUS_OK) {
        logger_warn("TX: failed to send TRANSFER_FINISH");
    }

    /* Cleanup */
    pres_free_pages(pages_comp, comp_count);
    pres_free_pages(pages_raw, page_count);
    free(file_data);

    logger_succ("TX: file transfer completed");
    return EXIT_SUCCESS;
}

// =============================================================
// Helper: RX – receive one burst and send checksum back
// =============================================================

static int rx_receive_burst_and_ack(const app_config_t *cfg,
                                    const link_radio_iface_t *iface,
                                    uint8_t page_id,
                                    uint8_t burst_id,
                                    uint16_t burst_size,
                                    page_rx_t *page)
{
    (void)cfg;

    if (!iface || !page) {
        logger_error("rx_receive_burst_and_ack: invalid arguments");
        return -1;
    }

    radio_ctx_t *rctx = (radio_ctx_t *)iface->user_ctx;

    uint8_t frame_count = 0;
    uint8_t frame_lengths[TRANS_MAX_FRAMES_PER_BURST];

    if (trans_derive_frame_layout(burst_size, &frame_count, frame_lengths) != 0) {
        logger_error("RX: trans_derive_frame_layout failed (burst_size=%u)",
                     (unsigned)burst_size);
        return -1;
    }

    bool already_received = false;
    if (burst_id < page->max_bursts && page->burst_received) {
        already_received = page->burst_received[burst_id];
    }

    uint64_t cs_state = 0;
    trans_checksum_init(&cs_state);

    logger_info("RX: receiving Page %u Burst %u (%u frames)",
                (unsigned)page_id, (unsigned)burst_id, (unsigned)frame_count);

    for (uint8_t f = 0; f < frame_count; ++f) {
        uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
        size_t  len = sizeof(buf);

        link_status_t st = link_read_frame(iface, buf, &len);
        if (st != LINK_STATUS_OK) {
            logger_error("RX: failed to read frame %u in burst", (unsigned)f);
            return -1;
        }

        if (len != frame_lengths[f]) {
            logger_warn("RX: frame %u length mismatch (got %zu, expected %u)",
                        (unsigned)f, len, (unsigned)frame_lengths[f]);
        }

        /* Update checksum over whole frame (including FrameID) */
        trans_checksum_update(&cs_state, buf, len);

        /* Store payload (without FrameID) only if this is a new burst and page not done */
        if (!already_received && !page->done) {
            if (len > 0) {
                size_t payload_len = len - 1;
                if (page->received_size + payload_len > page->expected_size) {
                    logger_error("RX: page buffer overflow (page_id=%u)", (unsigned)page_id);
                    return -1;
                }

                memcpy(page->data + page->received_size,
                       buf + 1, payload_len);
                page->received_size += (uint32_t)payload_len;
            }
        }
    }

    /* Finalize checksum and send ACK */
    uint64_t checksum = trans_checksum_final(cs_state);
    uint8_t  checksum_bytes[8];
    trans_checksum_to_bytes(checksum, checksum_bytes);

    nrf24_set_mode_tx(&rctx->dev);
    if (link_send_frame(iface, checksum_bytes, sizeof(checksum_bytes)) != LINK_STATUS_OK) {
        logger_warn("RX: failed to send checksum ACK for Page %u Burst %u",
                    (unsigned)page_id, (unsigned)burst_id);
        nrf24_set_mode_rx(&rctx->dev);
        return -1;
    }
    nrf24_set_mode_rx(&rctx->dev);

    /* Mark burst as received if it was new */
    if (!already_received && burst_id < page->max_bursts && page->burst_received) {
        page->burst_received[burst_id] = true;
    }

    /* Mark page as done if we filled it */
    if (!page->done && page->received_size == page->expected_size) {
        page->done = true;
        logger_info("RX: Page %u fully received (%u bytes)",
                    (unsigned)page_id, (unsigned)page->expected_size);
    }

    return 0;
}

// =============================================================
// RX mode implementation
// =============================================================

static int run_rx_mode(const app_config_t *cfg, link_radio_iface_t *iface)
{
    if (!cfg || !iface) {
        logger_error("run_rx_mode: invalid arguments");
        return EXIT_FAILURE;
    }

    radio_ctx_t *rctx = (radio_ctx_t *)iface->user_ctx;
    nrf24_set_mode_rx(&rctx->dev);

    /* 1) Receive STREAM_INFO */
    uint8_t stream_info[32];
    size_t  info_len = sizeof(stream_info);
    uint32_t total_orig_size = 0;
    uint32_t comp_sizes[TRANS_NUM_PAGES] = {0};

    logger_info("RX: waiting for STREAM_INFO");

    for (;;) {
        info_len = sizeof(stream_info);
        link_status_t st = link_read_frame(iface, stream_info, &info_len);
        if (st != LINK_STATUS_OK) {
            logger_error("RX: failed to read STREAM_INFO");
            return EXIT_FAILURE;
        }

        if (info_len == sizeof(stream_info) &&
            trans_parse_stream_info(stream_info,
                                    &total_orig_size,
                                    comp_sizes) == 0) {
            logger_info("RX: STREAM_INFO received. Total original size = %u bytes",
                        (unsigned)total_orig_size);
            break;
        }

        logger_warn("RX: ignoring non-STREAM_INFO frame (len=%zu)", info_len);
    }

    /* 2) Prepare per-page RX state */
    size_t page_count = TRANS_NUM_PAGES; /* matches PRES_DEFAULT_PAGE_COUNT */
    size_t page_orig_sizes[TRANS_NUM_PAGES];
    compute_page_orig_sizes(total_orig_size, page_count, page_orig_sizes);

    page_rx_t pages[TRANS_NUM_PAGES];
    memset(pages, 0, sizeof(pages));

    for (size_t i = 0; i < page_count; ++i) {
        if (comp_sizes[i] == 0) {
            continue;
        }

        pages[i].expected_size = comp_sizes[i];
        pages[i].received_size = 0;
        pages[i].done          = false;

        pages[i].data = (uint8_t *)malloc(pages[i].expected_size);
        if (!pages[i].data) {
            logger_error("RX: malloc failed for compressed page %zu", i);
            for (size_t j = 0; j < i; ++j) {
                free(pages[j].data);
                free(pages[j].burst_received);
            }
            return EXIT_FAILURE;
        }

        pages[i].max_bursts =
            (uint16_t)((pages[i].expected_size + TRANS_DATA_BYTES_PER_BURST - 1) /
                       TRANS_DATA_BYTES_PER_BURST);
        if (pages[i].max_bursts == 0) {
            pages[i].max_bursts = 1;
        }

        pages[i].burst_received =
            (bool *)calloc(pages[i].max_bursts, sizeof(bool));
        if (!pages[i].burst_received) {
            logger_error("RX: calloc failed for burst_received of page %zu", i);
            for (size_t j = 0; j <= i; ++j) {
                free(pages[j].data);
                free(pages[j].burst_received);
            }
            return EXIT_FAILURE;
        }

        logger_info("RX: prepared Page %zu (compressed %u bytes, orig %zu bytes, max_bursts=%u)",
                    i, (unsigned)pages[i].expected_size,
                    page_orig_sizes[i],
                    (unsigned)pages[i].max_bursts);
    }

    /* 3) Main loop: receive BURST_INFO / TRANSFER_FINISH */
    bool got_finish = false;

    logger_info("RX: starting to receive bursts");

    while (!got_finish) {
        uint8_t buf[32];
        size_t  len = sizeof(buf);

        link_status_t st = link_read_frame(iface, buf, &len);
        if (st != LINK_STATUS_OK) {
            logger_error("RX: link_read_frame failed in main loop");
            goto rx_cleanup_error;
        }

        /* Try BURST_INFO first */
        uint8_t  page_id = 0;
        uint8_t  burst_id = 0;
        uint16_t burst_size = 0;

        if (trans_parse_burst_info(buf, len,
                                   &page_id, &burst_id, &burst_size) == 0) {
            if (page_id >= page_count) {
                logger_warn("RX: received BURST_INFO with invalid page_id=%u",
                            (unsigned)page_id);
                /* Still need to drain the burst frames, but we have no buffer.
                 * For now, just continue; TX will time out and retry.
                 */
                continue;
            }

            page_rx_t *page = &pages[page_id];

            if (page->expected_size == 0) {
                logger_warn("RX: BURST_INFO for page_id=%u with expected_size=0",
                            (unsigned)page_id);
                continue;
            }

            if (burst_id >= page->max_bursts) {
                logger_warn("RX: BURST_INFO with burst_id=%u exceeding max_bursts=%u",
                            (unsigned)burst_id, (unsigned)page->max_bursts);
                continue;
            }

            if (rx_receive_burst_and_ack(cfg, iface,
                                         page_id, burst_id, burst_size,
                                         page) != 0) {
                logger_warn("RX: error while receiving Page %u Burst %u",
                            (unsigned)page_id, (unsigned)burst_id);
                /* We keep going; TX may retry. */
                continue;
            }

            if (all_pages_completed(pages, comp_sizes, page_count)) {
                logger_info("RX: all pages completed; waiting for TRANSFER_FINISH");
            }

            continue;
        }

        /* Not BURST_INFO – maybe TRANSFER_FINISH? */
        if (trans_parse_transfer_finish(buf, len) == 0) {
            logger_info("RX: TRANSFER_FINISH received");
            got_finish = true;
            break;
        }

        logger_warn("RX: ignoring unknown control frame (len=%zu)", len);
    }

    /* 4) Build compressed pages array and decompress */
    {
        pres_page_t *comp_pages = (pres_page_t *)calloc(page_count, sizeof(pres_page_t));
        if (!comp_pages) {
            logger_error("RX: calloc failed for comp_pages");
            goto rx_cleanup_error;
        }

        for (size_t i = 0; i < page_count; ++i) {
            comp_pages[i].data      = pages[i].data;
            comp_pages[i].size      = pages[i].received_size;
            comp_pages[i].orig_size = page_orig_sizes[i];
        }

        pres_page_t *raw_pages = NULL;
        size_t       raw_count = 0;

        if (pres_decompress_pages(comp_pages, page_count,
                                  &raw_pages, &raw_count) != 0) {
            logger_error("RX: pres_decompress_pages failed");
            /* comp_pages owns 'data'; free with pres_free_pages */
            pres_free_pages(comp_pages, page_count);
            goto rx_cleanup_error_no_free_pages; /* pages[].data already freed */
        }

        /* comp_pages owns the compressed buffers, free them now */
        pres_free_pages(comp_pages, page_count);

        uint8_t *file_data = NULL;
        size_t   file_len  = 0;

        if (pres_merge_pages(raw_pages, raw_count,
                             &file_data, &file_len) != 0) {
            logger_error("RX: pres_merge_pages failed");
            pres_free_pages(raw_pages, raw_count);
            goto rx_cleanup_error_no_free_pages;
        }

        pres_free_pages(raw_pages, raw_count);

        if (file_len != (size_t)total_orig_size) {
            logger_warn("RX: reconstructed file length %zu differs from advertised %u",
                        file_len, (unsigned)total_orig_size);
        }

        if (app_store_file_bytes(cfg->file_path_rx, file_data, file_len) != 0) {
            logger_error("RX: app_store_file_bytes failed");
            free(file_data);
            goto rx_cleanup_error_no_free_pages;
        }

        free(file_data);
        logger_succ("RX: file stored successfully");
    }

    /* Cleanup success path */
    for (size_t i = 0; i < page_count; ++i) {
        free(pages[i].burst_received);
        /* pages[i].data already freed via pres_free_pages(comp_pages, ...) */
    }

    return EXIT_SUCCESS;

rx_cleanup_error:
    /* Free per-page buffers on error */
    for (size_t i = 0; i < page_count; ++i) {
        free(pages[i].data);
        free(pages[i].burst_received);
    }
    return EXIT_FAILURE;

rx_cleanup_error_no_free_pages:
    for (size_t i = 0; i < page_count; ++i) {
        free(pages[i].burst_received);
    }
    return EXIT_FAILURE;
}
