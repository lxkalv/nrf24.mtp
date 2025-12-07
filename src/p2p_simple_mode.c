#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <zlib.h>
#include <unistd.h>

#include "libs/nrf24.h"
#include "libs/logger.h"
#include "libs/app_layer.h"
#include "libs/transport_layer.h"

#define MAX_PAYLOAD                                32
#define CONTROL_PREFIX                           0xFF
#define DATA_PREFIX                              0x00
#define DEFAULT_SPI_DEVICE           "/dev/spidev0.0"

#define MSG_STREAM_INFO                          0x01
#define MSG_STREAM_FINISH                        0x02
#define MSG_CHECKSUM                             0x03
#define MSG_STREAM_READY                         0x04

#define CHECKSUM_SIZE                               8
#define CHECKSUM_SEND_WINDOW_MS                  2000
#define READY_TIMEOUT_MS                         2000
#define CONTROL_TIMEOUT_MS                        200
#define DATA_TIMEOUT_MS                            60
#define CHECKSUM_TIMEOUT_MS                      1000
#define PAGE_CHECKSUM_TIMEOUT_MS                 1500
#define FINISH_ACK_TIMEOUT_MS                    2000

#define STREAM_INFO_SIZE                           32
#define STREAM_READY_SIZE                          11
#define STREAM_READY_MAX_ATTEMPTS                 400
#define STREAM_READY_WINDOW_MS                   2000
#define PAGE_CHECKSUM_MSG_SIZE   (2 + 1 + CHECKSUM_SIZE)
#define MODE_SWITCH_TX_DELAY_MS                    50
#define MODE_SWITCH_RX_DELAY_MS                    10

#define FNV64_OFFSET_BASIS        1469598103934665603ULL
#define FNV64_PRIME                     1099511628211ULL

typedef struct {
    uint8_t  channel;
    unsigned data_rate_kbps;
    int      pa_level_dbm;
    unsigned crc_bytes;
    unsigned retr_delay;
    unsigned retr_tries;
} p2p_radio_params_t;

static p2p_radio_params_t g_radio_params = {
    .channel        = 90,
    .data_rate_kbps = 250,
    .pa_level_dbm   = 0,
    .crc_bytes      = 2,
    .retr_delay     = 6,
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

static void sleep_ms_posix(unsigned int ms)
{
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&req, NULL);
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

static void update_radio_params_from_config(const app_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    g_radio_params.channel        = (uint8_t)cfg->channel;
    g_radio_params.data_rate_kbps = map_data_rate_kbps(cfg->data_rate);
    g_radio_params.pa_level_dbm   = map_pa_level_dbm(cfg->pa_level);
    g_radio_params.crc_bytes      = map_crc_bytes(cfg->crc_bytes);
    g_radio_params.retr_delay     = (unsigned)cfg->retransmission_delay;
    g_radio_params.retr_tries     = (unsigned)cfg->retransmission_tries;
}

static int configure_radio_runtime(nrf24_t *radio)
{
    return nrf24_configure_advanced(radio,
                                    g_radio_params.channel,
                                    g_radio_params.data_rate_kbps,
                                    g_radio_params.pa_level_dbm,
                                    g_radio_params.crc_bytes,
                                    g_radio_params.retr_delay,
                                    g_radio_params.retr_tries);
}

static int maybe_verify_radio_config(const app_config_t *cfg,
                                     nrf24_t *radio,
                                     const char *phase)
{
    if (!cfg || !cfg->verify_config) {
        return 0;
    }

    if (phase) {
        logger_info("Verifying radio configuration (%s phase) via module readback", phase);
    } else {
        logger_info("Verifying radio configuration via module readback");
    }

    if (nrf24_dump_config(radio) < 0) {
        logger_error("nrf24_dump_config failed: %s", strerror(errno));
        return -1;
    }
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

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
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
    return (uint32_t)(src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24));
}

static void encode_u64_le(uint8_t *dst, uint64_t v)
{
    for (size_t i = 0; i < 8; ++i) {
        dst[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
}

static uint64_t decode_u64_le(const uint8_t *src)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | src[i];
    }
    return v;
}

static int compress_buffer(const uint8_t *in,
                           size_t in_len,
                           uint8_t **out_buf,
                           size_t *out_len)
{
    if (!out_buf || !out_len) {
        logger_error("compress_buffer: invalid outputs");
        return -1;
    }

    uLong src_len = (uLong)in_len;
    uLong bound = compressBound(src_len == 0 ? 1 : src_len);
    if (bound == 0) {
        bound = 1;
    }

    uint8_t *buf = (uint8_t *)malloc(bound);
    if (!buf) {
        logger_error("compress_buffer: malloc(%lu) failed", (unsigned long)bound);
        return -1;
    }

    uLong dest_len = bound;
    int zret = compress2(buf, &dest_len, in ? in : (const Bytef *)"", src_len, 6);
    if (zret != Z_OK) {
        logger_error("compress2 failed (zret=%d)", zret);
        free(buf);
        return -1;
    }

    *out_buf = buf;
    *out_len = (size_t)dest_len;
    return 0;
}

static int decompress_buffer(const uint8_t *in,
                             size_t in_len,
                             uint8_t **out_buf,
                             size_t out_len)
{
    if ((!in && in_len > 0) || !out_buf) {
        logger_error("decompress_buffer: invalid arguments");
        return -1;
    }

    uint8_t *buf = NULL;
    if (out_len > 0) {
        buf = (uint8_t *)malloc(out_len);
        if (!buf) {
            logger_error("malloc(%zu) failed for decompression", out_len);
            return -1;
        }
    }

    uLongf dest_len = (uLongf)out_len;
    int zret = uncompress(buf,
                          &dest_len,
                          in ? in : (const Bytef *)"",
                          (uLong)in_len);
    if (zret != Z_OK || dest_len != out_len) {
        logger_error("uncompress failed (zret=%d, dest=%lu expected=%zu)",
                     zret,
                     (unsigned long)dest_len,
                     out_len);
        free(buf);
        return -1;
    }

    *out_buf = buf;
    return 0;
}

typedef struct {
    uint8_t        *comp_data;
    size_t          comp_len;
    uint32_t        orig_len;
    trans_burst_t  *bursts;
    size_t          burst_count;
    uint64_t        checksum;
} tx_page_t;

static void free_tx_page(tx_page_t *page);
static void free_tx_pages(tx_page_t pages[TRANS_NUM_PAGES]);
static int build_tx_pages(const uint8_t *file_data,
                          size_t file_len,
                          tx_page_t pages[TRANS_NUM_PAGES],
                          uint32_t page_comp_sizes[TRANS_NUM_PAGES],
                          uint64_t *total_comp_len,
                          uint32_t *total_frames_planned);
static int wait_for_page_checksum(nrf24_t *radio,
                                  uint8_t expected_page_id,
                                  uint64_t expected_checksum);
static int send_page_checksum(nrf24_t *radio,
                              uint8_t page_id,
                              uint64_t checksum,
                              uint64_t *rf_bytes_total,
                              uint64_t *rf_frames_total);
static void compute_page_orig_sizes(uint32_t total_orig_len,
                                    uint32_t out_sizes[TRANS_NUM_PAGES]);
static uint32_t estimate_total_frames(const uint32_t page_comp_sizes[TRANS_NUM_PAGES]);

static int send_with_retries(nrf24_t *radio,
                             const uint8_t *buf,
                             uint8_t len,
                             unsigned timeout_ms,
                             const char *label,
                             uint64_t *rf_bytes_total,
                             uint64_t *rf_frames_total)
{
    if (!radio || !buf || len == 0) {
        logger_error("send_with_retries(%s): invalid args", label ? label : "frame");
        return -1;
    }

    unsigned attempt = 0;
    while (1) {
        if (rf_bytes_total) {
            *rf_bytes_total += len;
        }
        if (rf_frames_total) {
            *rf_frames_total += 1;
        }

        int ret = nrf24_send_blocking(radio, buf, len, timeout_ms);
        if (ret == 0) {
            return 0;
        }

        if (errno != ETIMEDOUT) {
            logger_error("nrf24_send_blocking(%s) failed: %s",
                         label ? label : "frame",
                         strerror(errno));
            return -1;
        }

        ++attempt;
        if (attempt == 1 || (attempt % 50) == 0) {
            logger_warn("%s: timeout waiting for ACK (attempt %u)",
                        label ? label : "frame",
                        attempt);
        }

        if ((attempt % 200) == 0) {
            logger_warn("%s: %u consecutive timeouts, reconfiguring radio",
                        label ? label : "frame",
                        attempt);
            if (configure_radio_runtime(radio) != 0) {
                logger_error("Failed to reconfigure radio during retries: %s",
                             strerror(errno));
                return -1;
            }
        }
    }
}

static int ensure_mode_tx(nrf24_t *radio)
{
    if (nrf24_set_mode_tx(radio) < 0) {
        logger_error("nrf24_set_mode_tx failed: %s", strerror(errno));
        return -1;
    }
    sleep_ms_posix(MODE_SWITCH_TX_DELAY_MS);
    return 0;
}

static int ensure_mode_rx(nrf24_t *radio)
{
    if (nrf24_set_mode_rx(radio) < 0) {
        logger_error("nrf24_set_mode_rx failed: %s", strerror(errno));
        return -1;
    }
    sleep_ms_posix(MODE_SWITCH_RX_DELAY_MS);
    return 0;
}

static int send_stream_info(nrf24_t *radio,
                            uint32_t total_orig_len,
                            const uint32_t page_comp_sizes[TRANS_NUM_PAGES],
                            uint64_t total_comp_len,
                            uint32_t total_frames,
                            uint64_t *rf_bytes_total,
                            uint64_t *rf_frames_total)
{
    if (!page_comp_sizes) {
        logger_error("send_stream_info: missing page sizes");
        return -1;
    }

    uint8_t payload[32];
    if (trans_build_stream_info(total_orig_len, page_comp_sizes, payload) != 0) {
        return -1;
    }

    int ret = send_with_retries(radio,
                                payload,
                                (uint8_t)sizeof(payload),
                                CONTROL_TIMEOUT_MS,
                                "STREAM_INFO",
                                rf_bytes_total,
                                rf_frames_total);
    if (ret == 0) {
        logger_succ("p2p TX: STREAM_INFO acknowledged (orig=%u, comp=%llu, frames=%u)",
                    total_orig_len,
                    (unsigned long long)total_comp_len,
                    total_frames);
    }
    return ret;
}

static int send_stream_finish(nrf24_t *radio,
                              uint64_t *rf_bytes_total,
                              uint64_t *rf_frames_total)
{
    uint8_t msg[2] = { CONTROL_PREFIX, MSG_STREAM_FINISH };
    int ret = send_with_retries(radio,
                                msg,
                                sizeof(msg),
                                CONTROL_TIMEOUT_MS,
                                "STREAM_FINISH",
                                rf_bytes_total,
                                rf_frames_total);
    if (ret == 0) {
        logger_succ("p2p TX: STREAM_FINISH acknowledged by RX");
    }
    return ret;
}

static int send_stream_finish_with_timeout_rx(nrf24_t *radio,
                                              uint64_t *rf_bytes_total,
                                              uint64_t *rf_frames_total)
{
    uint8_t msg[2] = { CONTROL_PREFIX, MSG_STREAM_FINISH };
    double start = now_seconds();
    unsigned attempt = 0;

    while ((now_seconds() - start) * 1000.0 < FINISH_ACK_TIMEOUT_MS) {
        if (rf_bytes_total) {
            *rf_bytes_total += sizeof(msg);
        }
        if (rf_frames_total) {
            *rf_frames_total += 1;
        }

        if (nrf24_send_blocking(radio,
                                 msg,
                                 sizeof(msg),
                                 CONTROL_TIMEOUT_MS) == 0) {
            logger_succ("p2p RX: STREAM_FINISH confirmation delivered to TX");
            return 0;
        }

        if (errno != ETIMEDOUT) {
            logger_error("p2p RX: failed to send STREAM_FINISH confirmation: %s",
                         strerror(errno));
            return -1;
        }

        ++attempt;
        if (attempt == 1 || (attempt % 50) == 0) {
            logger_warn("p2p RX: STREAM_FINISH confirmation timeout (attempt %u)", attempt);
        }

        if ((attempt % 200) == 0) {
            logger_warn("p2p RX: %u STREAM_FINISH confirmation timeouts, reconfiguring radio",
                        attempt);
            if (configure_radio_runtime(radio) != 0) {
                logger_error("p2p RX: radio reconfigure failed while acknowledging STREAM_FINISH");
                return -1;
            }
            if (ensure_mode_tx(radio) != 0) {
                return -1;
            }
        }
    }

    logger_warn("p2p RX: STREAM_FINISH confirmation window elapsed; finishing without TX ack");
    errno = ETIMEDOUT;
    return 1;
}

static int wait_for_stream_finish_ack(nrf24_t *radio)
{
    if (ensure_mode_rx(radio) != 0) {
        return -1;
    }

    double wait_start = now_seconds();
    while ((now_seconds() - wait_start) * 1000.0 < FINISH_ACK_TIMEOUT_MS) {
        uint8_t buf[MAX_PAYLOAD];
        uint8_t len = sizeof(buf);
        int ret = nrf24_recv_blocking(radio, buf, &len, CONTROL_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            logger_error("p2p TX: waiting for STREAM_FINISH ack failed: %s",
                         strerror(errno));
            (void)ensure_mode_tx(radio);
            return -1;
        }

        if (len >= 2 && buf[0] == CONTROL_PREFIX) {
            if (buf[1] == MSG_STREAM_FINISH) {
                logger_succ("p2p TX: STREAM_FINISH acknowledged by RX");
                if (ensure_mode_tx(radio) != 0) {
                    return -1;
                }
                return 0;
            }

            logger_warn("p2p TX: control 0x%02X while waiting for STREAM_FINISH ack",
                        buf[1]);
            continue;
        }

        logger_warn("p2p TX: ignoring non-control frame while waiting for STREAM_FINISH ack");
    }

    logger_warn("p2p TX: timeout waiting for STREAM_FINISH ack");
    if (ensure_mode_tx(radio) != 0) {
        return -1;
    }
    return 1;
}

static int wait_for_stream_ready(nrf24_t *radio,
                                 uint8_t expected_id_bytes,
                                 uint32_t expected_frames,
                                 uint32_t expected_comp_len)
{
    if (ensure_mode_rx(radio) != 0) {
        return -1;
    }

    double wait_start = now_seconds();
    while ((now_seconds() - wait_start) * 1000.0 < READY_TIMEOUT_MS) {
        uint8_t buf[MAX_PAYLOAD];
        uint8_t len = sizeof(buf);
        int ret = nrf24_recv_blocking(radio, buf, &len, 200);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            logger_error("p2p TX: waiting for STREAM_READY failed: %s", strerror(errno));
            return -1;
        }

        if (len < 2 || buf[0] != CONTROL_PREFIX) {
            logger_warn("p2p TX: ignoring unexpected frame while waiting for READY");
            continue;
        }

        if (buf[1] == MSG_STREAM_READY) {
            uint8_t rx_id_bytes = (len >= 3) ? buf[2] : 0;
            uint32_t rx_frames  = (len >= 7) ? decode_u32_le(&buf[3]) : 0;
            uint32_t rx_comp    = (len >= 11) ? decode_u32_le(&buf[7]) : 0;

            if (rx_id_bytes != expected_id_bytes) {
                logger_warn("p2p TX: RX id_bytes=%u differs from TX=%u",
                            rx_id_bytes,
                            expected_id_bytes);
            }
            if (expected_frames && rx_frames && rx_frames != expected_frames) {
                logger_warn("p2p TX: RX expects %u frames but TX planned %u",
                            rx_frames,
                            expected_frames);
            }
            if (expected_comp_len && rx_comp && rx_comp != expected_comp_len) {
                logger_warn("p2p TX: RX reported comp_len=%u but TX has %u",
                            rx_comp,
                            expected_comp_len);
            }

            logger_info("p2p TX: RX ready (frames=%u, comp=%u)", rx_frames, rx_comp);

            if (ensure_mode_tx(radio) != 0) {
                return -1;
            }
            return 0;
        }

        logger_warn("p2p TX: control 0x%02X while waiting for READY", buf[1]);
    }

    if (ensure_mode_tx(radio) != 0) {
        return -1;
    }

    return 1; /* timeout, caller may retry */
}

static int send_stream_ready(nrf24_t *radio,
                             uint8_t id_bytes,
                             uint32_t expected_frames,
                             uint32_t compressed_len,
                             uint64_t *rf_bytes_total,
                             uint64_t *rf_frames_total)
{
    if (ensure_mode_tx(radio) != 0) {
        return -1;
    }

    uint8_t msg[STREAM_READY_SIZE] = {0};
    msg[0] = CONTROL_PREFIX;
    msg[1] = MSG_STREAM_READY;
    msg[2] = id_bytes;
    encode_u32_le(&msg[3], expected_frames);
    encode_u32_le(&msg[7], compressed_len);

    double start = now_seconds();
    unsigned attempt = 0;
    int sent = 0;

    while (attempt < STREAM_READY_MAX_ATTEMPTS &&
           (now_seconds() - start) * 1000.0 < STREAM_READY_WINDOW_MS) {
        if (rf_bytes_total) {
            *rf_bytes_total += sizeof(msg);
        }
        if (rf_frames_total) {
            *rf_frames_total += 1;
        }

        if (nrf24_send_blocking(radio,
                                 msg,
                                 sizeof(msg),
                                 CONTROL_TIMEOUT_MS) == 0) {
            sent = 1;
            break;
        }

        if (errno != ETIMEDOUT) {
            logger_error("p2p RX: STREAM_READY send failed: %s", strerror(errno));
            if (ensure_mode_rx(radio) != 0) {
                return -1;
            }
            return -1;
        }

        ++attempt;
        if (attempt == 1 || (attempt % 50) == 0) {
            logger_warn("STREAM_READY: timeout waiting for ACK (attempt %u)", attempt);
        }

        if ((attempt % 200) == 0) {
            logger_warn("STREAM_READY: %u consecutive timeouts, reconfiguring radio", attempt);
            if (configure_radio_runtime(radio) != 0) {
                logger_error("p2p RX: failed to reconfigure radio while sending STREAM_READY");
                if (ensure_mode_rx(radio) != 0) {
                    return -1;
                }
                return -1;
            }
            if (ensure_mode_tx(radio) != 0) {
                return -1;
            }
        }
    }

    if (!sent) {
        logger_warn("p2p RX: STREAM_READY not acknowledged after %u attempts", attempt);
        if (ensure_mode_rx(radio) != 0) {
            return -1;
        }
        errno = ETIMEDOUT;
        return 1; /* indicate timeout */
    }

    logger_succ("p2p RX: STREAM_READY acknowledged (frames=%u, comp=%u)",
                expected_frames,
                compressed_len);

    if (ensure_mode_rx(radio) != 0) {
        return -1;
    }
    return 0;
}

static int run_tx(const char *spi_dev,
                  const app_config_t *cfg,
                  const uint8_t *file_data,
                  size_t file_len)
{
    tx_page_t pages[TRANS_NUM_PAGES];
    uint32_t page_comp_sizes[TRANS_NUM_PAGES];
    uint64_t total_comp_len = 0;
    uint32_t total_frames = 0;
    uint64_t tx_rf_bytes = 0;
    uint64_t tx_rf_frames = 0;
    double tx_start = 0.0;
    unsigned finish_ack_attempts = 0;
    int exit_code = 1;
    int radio_ready = 0;

    if (!spi_dev) {
        logger_error("run_tx: missing SPI device");
        return 1;
    }
    if (!file_data && file_len > 0) {
        logger_error("run_tx: input buffer is NULL but length > 0");
        return 1;
    }

    if (file_len > TRANS_STREAM_MAX_TOTAL_SIZE) {
        logger_error("run_tx: file too large for STREAM_INFO format (%zu > %u)",
                     file_len,
                     TRANS_STREAM_MAX_TOTAL_SIZE);
        return 1;
    }

    memset(pages, 0, sizeof(pages));
    if (build_tx_pages(file_data,
                       file_len,
                       pages,
                       page_comp_sizes,
                       &total_comp_len,
                       &total_frames) != 0) {
        free_tx_pages(pages);
        return 1;
    }

    const char *input_label = (cfg && cfg->file_path_tx && cfg->file_path_tx[0])
                            ? cfg->file_path_tx
                            : "(auto)";

    logger_info("p2p TX: '%s' split into %u pages (total comp=%llu bytes, frames=%u)",
                input_label,
                TRANS_NUM_PAGES,
                (unsigned long long)total_comp_len,
                total_frames);

    nrf24_t radio;
    uint8_t ce_pin = (uint8_t)((cfg && cfg->ce_pin >= 0 && cfg->ce_pin <= 255)
                               ? cfg->ce_pin : 22);
    nrf24_config_t radio_cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = ce_pin
    };

    if (nrf24_init(&radio, &radio_cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        goto cleanup;
    }
    radio_ready = 1;

    if (configure_radio_runtime(&radio) < 0) {
        logger_error("configure_radio_runtime failed: %s", strerror(errno));
        goto cleanup;
    }
    if (maybe_verify_radio_config(cfg, &radio, "TX init") != 0) {
        goto cleanup;
    }
    if (ensure_mode_tx(&radio) != 0) {
        goto cleanup;
    }

    uint32_t total_orig_len = (uint32_t)file_len;
    if (send_stream_info(&radio,
                         total_orig_len,
                         page_comp_sizes,
                         total_comp_len,
                         total_frames,
                         &tx_rf_bytes,
                         &tx_rf_frames) != 0) {
        logger_error("Failed to send STREAM_INFO");
        goto cleanup;
    }

    while (1) {
        int ready_ret = wait_for_stream_ready(&radio,
                                              1,
                                              total_frames,
                                              (uint32_t)total_comp_len);
        if (ready_ret == 0) {
            break;
        }
        if (ready_ret < 0) {
            logger_error("p2p TX: failed while waiting for STREAM_READY");
            goto cleanup;
        }

        logger_warn("p2p TX: RX not ready yet, resending STREAM_INFO");
        if (send_stream_info(&radio,
                             total_orig_len,
                             page_comp_sizes,
                             total_comp_len,
                             total_frames,
                             &tx_rf_bytes,
                             &tx_rf_frames) != 0) {
            logger_error("Failed to resend STREAM_INFO");
            goto cleanup;
        }
    }

    tx_start = now_seconds();

    for (unsigned page_id = 0; page_id < TRANS_NUM_PAGES; ++page_id) {
        tx_page_t *page = &pages[page_id];
        if (!page->comp_data && page->comp_len == 0) {
            logger_info("p2p TX: Page %u empty, requesting checksum ack", page_id);
        } else {
            logger_info("p2p TX: Page %u -> %u bytes compressed in %zu burst(s)",
                        page_id,
                        page->comp_len,
                        page->burst_count);
        }

        int page_done = 0;
        unsigned resend_round = 0;

        while (!page_done) {
            if (page->burst_count == 0 && page->comp_len == 0) {
                uint8_t empty_info[6] = {
                    TRANS_MSG_INFO,
                    TRANS_MSG_BURST_INFO,
                    (uint8_t)page_id,
                    0,
                    0,
                    0
                };
                if (send_with_retries(&radio,
                                      empty_info,
                                      sizeof(empty_info),
                                      CONTROL_TIMEOUT_MS,
                                      "BURST_INFO(empty)",
                                      &tx_rf_bytes,
                                      &tx_rf_frames) != 0) {
                    logger_error("Failed to signal empty page %u", page_id);
                    goto cleanup;
                }
            }

            for (size_t burst_idx = 0; burst_idx < page->burst_count; ++burst_idx) {
                const trans_burst_t *burst = &page->bursts[burst_idx];
                uint8_t info[6];
                trans_build_burst_info((uint8_t)page_id,
                                       (uint8_t)burst_idx,
                                       burst,
                                       info);

                if (send_with_retries(&radio,
                                      info,
                                      sizeof(info),
                                      CONTROL_TIMEOUT_MS,
                                      "BURST_INFO",
                                      &tx_rf_bytes,
                                      &tx_rf_frames) != 0) {
                    logger_error("Failed to send BURST_INFO (page %u, burst %zu)",
                                 page_id,
                                 burst_idx);
                    goto cleanup;
                }

                for (size_t frame_idx = 0; frame_idx < burst->frame_count; ++frame_idx) {
                    const trans_frame_t *frame = &burst->frames[frame_idx];
                    char label[48];
                    snprintf(label,
                             sizeof(label),
                             "DATA[p%u-b%zu-f%zu]",
                             page_id,
                             burst_idx,
                             frame_idx);
                    if (send_with_retries(&radio,
                                          frame->data,
                                          frame->len,
                                          DATA_TIMEOUT_MS,
                                          label,
                                          &tx_rf_bytes,
                                          &tx_rf_frames) != 0) {
                        logger_error("Failed to send frame %zu of burst %zu (page %u)",
                                     frame_idx,
                                     burst_idx,
                                     page_id);
                        goto cleanup;
                    }
                }
            }

            int checksum_rc = wait_for_page_checksum(&radio,
                                                      (uint8_t)page_id,
                                                      page->checksum);
            if (checksum_rc == 0) {
                page_done = 1;
                break;
            }
            if (checksum_rc < 0) {
                goto cleanup;
            }

            ++resend_round;
            logger_warn("p2p TX: resending Page %u (attempt %u)",
                        page_id,
                        resend_round + 1);
        }
    }

    while (1) {
        if (send_stream_finish(&radio, &tx_rf_bytes, &tx_rf_frames) != 0) {
            logger_error("p2p TX: failed to send STREAM_FINISH");
            goto cleanup;
        }

        int finish_ack = wait_for_stream_finish_ack(&radio);
        if (finish_ack == 0) {
            break;
        }

        if (finish_ack > 0) {
            ++finish_ack_attempts;
            logger_warn("p2p TX: STREAM_FINISH ack timeout, retrying (attempt %u)",
                        finish_ack_attempts);
            continue;
        }

        logger_error("p2p TX: failed while waiting for STREAM_FINISH ack");
        goto cleanup;
    }

    if (tx_start > 0.0) {
        double tx_end = now_seconds();
        double elapsed = tx_end - tx_start;
        if (elapsed <= 0.0) {
            elapsed = 1e-9;
        }
        double user_rate_kib = ((double)file_len / 1024.0) / elapsed;
        double rf_rate_kib   = ((double)tx_rf_bytes / 1024.0) / elapsed;
        logger_info("p2p TX throughput: user=%.2f KiB/s (%zu bytes in %.2fs), rf=%.2f KiB/s (%llu bytes, %llu frames)",
                    user_rate_kib,
                    file_len,
                    elapsed,
                    rf_rate_kib,
                    (unsigned long long)tx_rf_bytes,
                    (unsigned long long)tx_rf_frames);
    }

    logger_succ("p2p TX: transfer complete (%zu bytes -> %llu bytes)",
                file_len,
                (unsigned long long)total_comp_len);
    exit_code = 0;

cleanup:
    if (radio_ready) {
        nrf24_deinit(&radio);
    }
    free_tx_pages(pages);
    return exit_code;
}

static int run_rx(const char *spi_dev, const app_config_t *cfg)
{
    if (!spi_dev) {
        logger_error("run_rx: missing SPI device");
        return 1;
    }

    nrf24_t radio;
    uint8_t ce_pin = (uint8_t)((cfg && cfg->ce_pin >= 0 && cfg->ce_pin <= 255)
                               ? cfg->ce_pin : 22);
    nrf24_config_t radio_cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = ce_pin
    };

    uint32_t page_expected_comp[TRANS_NUM_PAGES] = {0};
    uint32_t page_orig_sizes[TRANS_NUM_PAGES] = {0};
    uint32_t page_offsets[TRANS_NUM_PAGES] = {0};
    uint8_t *final_output = NULL;
    size_t   final_output_len = 0;
    uint8_t *page_buffer = NULL;
    size_t   page_buffer_cap = 0;
    uint8_t  active_page_id = 0;
    uint32_t active_page_expected = 0;
    uint32_t active_page_received = 0;
    uint64_t active_page_checksum_state = 0;
    uint8_t  burst_frame_lengths[TRANS_MAX_FRAMES_PER_BURST];
    uint8_t  burst_expected_frames = 0;
    uint8_t  burst_frame_index = 0;
    uint8_t  burst_page_id = 0;
    int      in_burst = 0;
    int      have_stream_info = 0;
    uint32_t total_orig_size = 0;
    uint64_t expected_total_comp = 0;
    uint32_t expected_total_frames = 0;
    uint64_t rf_rx_bytes = 0;
    uint64_t rf_rx_frames = 0;
    uint64_t rf_tx_bytes = 0;
    uint64_t rf_tx_frames = 0;
    uint32_t pages_completed = 0;
    double   rx_start = 0.0;
    int      transfer_finished = 0;
    int      output_stored = 0;
    int      radio_ready = 0;
    int      exit_code = 1;

    if (nrf24_init(&radio, &radio_cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    radio_ready = 1;

    if (configure_radio_runtime(&radio) < 0) {
        logger_error("configure_radio_runtime failed: %s", strerror(errno));
        goto cleanup_rx;
    }
    if (maybe_verify_radio_config(cfg, &radio, "RX init") != 0) {
        goto cleanup_rx;
    }
    if (ensure_mode_rx(&radio) != 0) {
        goto cleanup_rx;
    }

    logger_info("p2p RX: waiting for STREAM_INFO");

    while (!transfer_finished) {
        uint8_t buf[MAX_PAYLOAD];
        uint8_t len = sizeof(buf);
        int ret = nrf24_recv_blocking(&radio, buf, &len, 0);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            logger_error("p2p RX: nrf24_recv_blocking failed: %s", strerror(errno));
            goto cleanup_rx;
        }

        if (len > 0) {
            rf_rx_bytes += len;
            rf_rx_frames += 1;
        }

        if (rx_start == 0.0) {
            rx_start = now_seconds();
        }

        if (len == 0) {
            continue;
        }

        if (buf[0] == CONTROL_PREFIX) {
            if (len < 2) {
                logger_warn("p2p RX: short control frame");
                continue;
            }

            uint8_t msg_type = buf[1];
            if (msg_type == MSG_STREAM_INFO) {
                if (len < STREAM_INFO_SIZE) {
                    logger_warn("p2p RX: malformed STREAM_INFO (len=%u)", len);
                    continue;
                }

                if (trans_parse_stream_info(buf,
                                            &total_orig_size,
                                            page_expected_comp) != 0) {
                    logger_error("p2p RX: failed to parse STREAM_INFO payload");
                    goto cleanup_rx;
                }

                expected_total_comp = 0;
                for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
                    expected_total_comp += page_expected_comp[i];
                }
                expected_total_frames = estimate_total_frames(page_expected_comp);
                compute_page_orig_sizes(total_orig_size, page_orig_sizes);

                uint32_t offset = 0;
                for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
                    page_offsets[i] = offset;
                    offset += page_orig_sizes[i];
                }

                free(final_output);
                final_output = NULL;
                final_output_len = total_orig_size;
                if (final_output_len > 0) {
                    final_output = (uint8_t *)malloc(final_output_len);
                    if (!final_output) {
                        logger_error("p2p RX: malloc(%zu) failed for output", final_output_len);
                        goto cleanup_rx;
                    }
                }

                have_stream_info = 1;
                pages_completed = 0;
                active_page_id = 0;
                active_page_expected = page_expected_comp[0];
                active_page_received = 0;
                trans_checksum_init(&active_page_checksum_state);
                in_burst = 0;

                logger_info("p2p RX: STREAM_INFO -> total_orig=%u bytes, total_comp=%llu bytes",
                            total_orig_size,
                            (unsigned long long)expected_total_comp);

                int ready_rc = send_stream_ready(&radio,
                                                 1,
                                                 expected_total_frames,
                                                 (uint32_t)expected_total_comp,
                                                 &rf_tx_bytes,
                                                 &rf_tx_frames);
                if (ready_rc < 0) {
                    goto cleanup_rx;
                }
                if (ready_rc > 0) {
                    logger_warn("p2p RX: STREAM_READY timed out; awaiting STREAM_INFO resend");
                }
                continue;
            }

            if (msg_type == MSG_STREAM_FINISH) {
                if (!output_stored) {
                    const char *dest_label = (cfg && cfg->file_path_rx && cfg->file_path_rx[0])
                                           ? cfg->file_path_rx
                                           : "(auto)";
                    const uint8_t *out_ptr = final_output_len > 0 ? final_output : NULL;
                    if (app_store_file_bytes(cfg ? cfg->file_path_rx : NULL,
                                             out_ptr,
                                             final_output_len) != 0) {
                        logger_error("p2p RX: failed to store output file");
                        goto cleanup_rx;
                    }
                    output_stored = 1;
                    logger_succ("p2p RX: stored '%s' (%u bytes)", dest_label, total_orig_size);
                }

                if (ensure_mode_tx(&radio) != 0) {
                    goto cleanup_rx;
                }
                int ack_rc = send_stream_finish_with_timeout_rx(&radio,
                                                                 &rf_tx_bytes,
                                                                 &rf_tx_frames);
                if (ack_rc < 0) {
                    goto cleanup_rx;
                }
                if (ensure_mode_rx(&radio) != 0) {
                    goto cleanup_rx;
                }

                transfer_finished = 1;
                break;
            }

            if (msg_type == MSG_BURST_INFO) {
                if (!have_stream_info) {
                    logger_warn("p2p RX: BURST_INFO before STREAM_INFO");
                    continue;
                }

                uint8_t page_id = 0;
                uint8_t burst_id = 0;
                uint16_t burst_size = 0;
                if (trans_parse_burst_info(buf,
                                           len,
                                           &page_id,
                                           &burst_id,
                                           &burst_size) != 0) {
                    logger_warn("p2p RX: malformed BURST_INFO frame");
                    continue;
                }

                if (page_id >= TRANS_NUM_PAGES) {
                    logger_warn("p2p RX: page %u exceeds limit", page_id);
                    continue;
                }

                if (page_id != active_page_id) {
                    logger_info("p2p RX: switching to Page %u", page_id);
                    active_page_id = page_id;
                    active_page_expected = page_expected_comp[page_id];
                    active_page_received = 0;
                    trans_checksum_init(&active_page_checksum_state);
                    in_burst = 0;
                }

                if (burst_size == 0) {
                    logger_info("p2p RX: Page %u declared empty", page_id);
                    uint64_t checksum_state;
                    trans_checksum_init(&checksum_state);
                    uint64_t checksum_value = trans_checksum_final(checksum_state);
                    if (send_page_checksum(&radio,
                                           page_id,
                                           checksum_value,
                                           &rf_tx_bytes,
                                           &rf_tx_frames) < 0) {
                        goto cleanup_rx;
                    }
                    ++pages_completed;
                    continue;
                }

                if (trans_derive_frame_layout(burst_size,
                                              &burst_expected_frames,
                                              burst_frame_lengths) != 0) {
                    logger_warn("p2p RX: invalid burst layout (page=%u, size=%u)",
                                page_id,
                                burst_size);
                    continue;
                }

                if (burst_id == 0 && active_page_received > 0) {
                    logger_warn("p2p RX: restart detected for Page %u; discarding partial data", page_id);
                    active_page_received = 0;
                    trans_checksum_init(&active_page_checksum_state);
                }

                if (active_page_expected > page_buffer_cap) {
                    uint8_t *tmp = (uint8_t *)realloc(page_buffer,
                                                       active_page_expected > 0 ? active_page_expected : 1);
                    if (!tmp) {
                        logger_error("p2p RX: realloc(%u) failed for page buffer", active_page_expected);
                        goto cleanup_rx;
                    }
                    page_buffer = tmp;
                    page_buffer_cap = active_page_expected;
                }

                burst_page_id = page_id;
                burst_frame_index = 0;
                in_burst = 1;
                continue;
            }

            if (msg_type == MSG_CHECKSUM) {
                logger_info("p2p RX: checksum control ignored on RX side");
                continue;
            }

            logger_warn("p2p RX: unknown control message 0x%02X", msg_type);
            continue;
        }

        if (!have_stream_info) {
            logger_warn("p2p RX: data frame before STREAM_INFO");
            continue;
        }

        if (!in_burst) {
            logger_warn("p2p RX: data frame received outside of burst context");
            continue;
        }

        if (burst_page_id != active_page_id) {
            logger_warn("p2p RX: burst/page mismatch (%u vs %u)", burst_page_id, active_page_id);
            continue;
        }

        if (burst_frame_index >= burst_expected_frames) {
            logger_warn("p2p RX: burst frame index overflow (page %u)", burst_page_id);
            continue;
        }

        if (len != burst_frame_lengths[burst_frame_index]) {
            logger_warn("p2p RX: frame len mismatch (expected %u, got %u)",
                        burst_frame_lengths[burst_frame_index],
                        len);
            continue;
        }

        uint8_t frame_id = buf[0];
        if (frame_id != burst_frame_index) {
            logger_warn("p2p RX: frame order mismatch (expected %u, got %u)",
                        burst_frame_index,
                        frame_id);
            continue;
        }

        if (active_page_received + (len - 1) > active_page_expected) {
            logger_warn("p2p RX: page buffer overflow (page %u)", active_page_id);
            continue;
        }

        memcpy(page_buffer + active_page_received, &buf[1], len - 1);
        active_page_received += (len - 1);
        trans_checksum_update(&active_page_checksum_state, buf, len);

        ++burst_frame_index;
        if (burst_frame_index == burst_expected_frames) {
            in_burst = 0;
            burst_frame_index = 0;
        }

        if (active_page_received == active_page_expected) {
            uint64_t checksum_value = trans_checksum_final(active_page_checksum_state);
            if (send_page_checksum(&radio,
                                   active_page_id,
                                   checksum_value,
                                   &rf_tx_bytes,
                                   &rf_tx_frames) < 0) {
                goto cleanup_rx;
            }

            if (page_orig_sizes[active_page_id] > 0) {
                uint8_t *page_out = NULL;
                if (decompress_buffer(page_buffer,
                                       active_page_received,
                                       &page_out,
                                       page_orig_sizes[active_page_id]) != 0) {
                    logger_error("p2p RX: failed to decompress page %u", active_page_id);
                    free(page_out);
                    goto cleanup_rx;
                }
                memcpy(final_output + page_offsets[active_page_id],
                       page_out,
                       page_orig_sizes[active_page_id]);
                free(page_out);
            }

            ++pages_completed;

            active_page_id += 1;
            if (active_page_id < TRANS_NUM_PAGES) {
                active_page_expected = page_expected_comp[active_page_id];
            } else {
                active_page_expected = 0;
            }
            active_page_received = 0;
            trans_checksum_init(&active_page_checksum_state);
        }
    }

    if (!transfer_finished) {
        logger_warn("p2p RX: stream ended without STREAM_FINISH");
    }

    if (rx_start > 0.0) {
        double rx_end = now_seconds();
        double elapsed = rx_end - rx_start;
        if (elapsed <= 0.0) {
            elapsed = 1e-9;
        }
        double user_rate_kib = (elapsed > 0.0)
                             ? (((double)total_orig_size) / 1024.0) / elapsed
                             : 0.0;
        double rf_rate_kib   = (elapsed > 0.0)
                             ? (((double)rf_rx_bytes) / 1024.0) / elapsed
                             : 0.0;
        logger_info("p2p RX throughput: user=%.2f KiB/s (%u bytes in %.2fs), rf=%.2f KiB/s (%llu bytes, %llu frames)",
                    user_rate_kib,
                    total_orig_size,
                    elapsed,
                    rf_rate_kib,
                    (unsigned long long)rf_rx_bytes,
                    (unsigned long long)rf_rx_frames);
    }

    logger_info("p2p RX: completed %u/%u pages", pages_completed, TRANS_NUM_PAGES);
    exit_code = 0;

cleanup_rx:
    free(page_buffer);
    free(final_output);
    if (radio_ready) {
        nrf24_deinit(&radio);
    }
    return exit_code;
}

int main(int argc, char **argv)
{
    app_config_t cfg;
    if (app_parse_arguments(argc, argv, &cfg) != 0) {
        app_print_usage(argv[0]);
        return 1;
    }

    const char *log_folder = "p2p";
    char log_path[64];
    if (cfg.mode == APP_MODE_TX) {
        snprintf(log_path, sizeof(log_path), "%s/p2p_tx.log", log_folder);
    } else if (cfg.mode == APP_MODE_RX) {
        snprintf(log_path, sizeof(log_path), "%s/p2p_rx.log", log_folder);
    } else {
        snprintf(log_path, sizeof(log_path), "%s/p2p.log", log_folder);
    }

    if (logger_init(log_path) != 0) {
        logger_warn("Could not open log file '%s' (continuing without file log)", log_path);
    } else {
        logger_info("Logging to file '%s'", log_path);
    }

    if (cfg.print_config) {
        app_print_config(&cfg);
    }

    const char *spi_dev = get_spi_device_path();
    update_radio_params_from_config(&cfg);

    logger_info("Using SPI device: %s", spi_dev);

    int exit_code = 0;

    if (cfg.mode == APP_MODE_TX) {
        uint8_t *data = NULL;
        size_t   len  = 0;
        if (app_load_file_bytes(cfg.file_path_tx, &data, &len) != 0) {
            logger_error("p2p TX: failed to load input bytes");
            exit_code = 1;
            goto cleanup;
        }

        exit_code = run_tx(spi_dev, &cfg, data, len);
        free(data);
        goto cleanup;
    }

    if (cfg.mode == APP_MODE_RX) {
        exit_code = run_rx(spi_dev, &cfg);
        goto cleanup;
    }

    logger_error("Unsupported mode: %s", app_mode_str(cfg.mode));
    exit_code = 1;

cleanup:
    logger_close();
    return exit_code;
}

static void free_tx_page(tx_page_t *page)
{
    if (!page) {
        return;
    }

    if (page->comp_data) {
        free(page->comp_data);
        page->comp_data = NULL;
    }
    if (page->bursts) {
        trans_free_bursts(page->bursts, page->burst_count);
        page->bursts = NULL;
    }
    page->burst_count = 0;
    page->comp_len = 0;
    page->orig_len = 0;
    page->checksum = 0;
}

static void free_tx_pages(tx_page_t pages[TRANS_NUM_PAGES])
{
    if (!pages) {
        return;
    }
    for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
        free_tx_page(&pages[i]);
    }
}

static int build_tx_pages(const uint8_t *file_data,
                          size_t file_len,
                          tx_page_t pages[TRANS_NUM_PAGES],
                          uint32_t page_comp_sizes[TRANS_NUM_PAGES],
                          uint64_t *total_comp_len,
                          uint32_t *total_frames_planned)
{
    if (!pages || !page_comp_sizes || !total_comp_len || !total_frames_planned) {
        logger_error("build_tx_pages: invalid arguments");
        return -1;
    }

    *total_comp_len = 0;
    *total_frames_planned = 0;

    for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
        page_comp_sizes[i] = 0;
        pages[i].comp_data = NULL;
        pages[i].bursts = NULL;
        pages[i].burst_count = 0;
        pages[i].checksum = 0;
        pages[i].orig_len = 0;
        pages[i].comp_len = 0;
    }

    for (size_t page = 0; page < TRANS_NUM_PAGES; ++page) {
        uint64_t start = (file_len * page) / TRANS_NUM_PAGES;
        uint64_t end   = (file_len * (page + 1)) / TRANS_NUM_PAGES;
        if (end < start) {
            end = start;
        }
        size_t slice_len = (size_t)(end - start);
        pages[page].orig_len = (uint32_t)slice_len;

        if (slice_len == 0) {
            page_comp_sizes[page] = 0;
            uint64_t checksum_state;
            trans_checksum_init(&checksum_state);
            pages[page].checksum = trans_checksum_final(checksum_state);
            continue;
        }

        if (compress_buffer(file_data + start,
                            slice_len,
                            &pages[page].comp_data,
                            &pages[page].comp_len) != 0) {
            logger_error("Failed to compress page %zu", page);
            free_tx_pages(pages);
            return -1;
        }

        if (pages[page].comp_len > TRANS_STREAM_MAX_PAGE_COMP_SIZE) {
            logger_error("Page %zu compressed size %zu exceeds protocol limit (%u)",
                         page,
                         pages[page].comp_len,
                         TRANS_STREAM_MAX_PAGE_COMP_SIZE);
            free_tx_pages(pages);
            return -1;
        }

        page_comp_sizes[page] = (uint32_t)pages[page].comp_len;
        *total_comp_len += pages[page].comp_len;

        if (trans_split_page_into_bursts(pages[page].comp_data,
                                         pages[page].comp_len,
                                         &pages[page].bursts,
                                         &pages[page].burst_count) != 0) {
            logger_error("Failed to split page %zu into bursts", page);
            free_tx_pages(pages);
            return -1;
        }

        uint64_t checksum_state;
        trans_checksum_init(&checksum_state);
        trans_checksum_update(&checksum_state,
                              pages[page].comp_data,
                              pages[page].comp_len);
        pages[page].checksum = trans_checksum_final(checksum_state);

        if (pages[page].bursts) {
            for (size_t b = 0; b < pages[page].burst_count; ++b) {
                *total_frames_planned += (uint32_t)pages[page].bursts[b].frame_count;
            }
        }
    }

    if (*total_comp_len > UINT32_MAX || *total_frames_planned > UINT32_MAX) {
        logger_error("STREAM_INFO totals exceed 32-bit range (comp=%llu, frames=%u)",
                     (unsigned long long)*total_comp_len,
                     *total_frames_planned);
        free_tx_pages(pages);
        return -1;
    }

    return 0;
}

static int wait_for_page_checksum(nrf24_t *radio,
                                  uint8_t expected_page_id,
                                  uint64_t expected_checksum)
{
    if (ensure_mode_rx(radio) != 0) {
        return -1;
    }

    double wait_start = now_seconds();
    while ((now_seconds() - wait_start) * 1000.0 < PAGE_CHECKSUM_TIMEOUT_MS) {
        uint8_t buf[MAX_PAYLOAD];
        uint8_t len = sizeof(buf);
        int ret = nrf24_recv_blocking(radio, buf, &len, CONTROL_TIMEOUT_MS);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            logger_error("p2p TX: waiting for page checksum failed: %s", strerror(errno));
            (void)ensure_mode_tx(radio);
            return -1;
        }

        if (len < 2 || buf[0] != CONTROL_PREFIX) {
            logger_warn("p2p TX: ignoring non-control frame while waiting for page checksum");
            continue;
        }

        if (buf[1] != MSG_CHECKSUM) {
            logger_warn("p2p TX: control 0x%02X while waiting for page checksum", buf[1]);
            continue;
        }

        if (len < PAGE_CHECKSUM_MSG_SIZE) {
            logger_warn("p2p TX: malformed page checksum frame (len=%u)", len);
            continue;
        }

        uint8_t page_id = buf[2];
        uint64_t rx_checksum = decode_u64_le(&buf[3]);

        if (page_id != expected_page_id) {
            logger_warn("p2p TX: checksum for unexpected page %u (expecting %u)",
                        page_id,
                        expected_page_id);
            continue;
        }

        if (ensure_mode_tx(radio) != 0) {
            return -1;
        }

        if (rx_checksum == expected_checksum) {
            logger_info("p2p TX: Page %u checksum confirmed", page_id);
            return 0;
        }

        logger_warn("p2p TX: checksum mismatch on page %u (expected 0x%016llX, got 0x%016llX)",
                    page_id,
                    (unsigned long long)expected_checksum,
                    (unsigned long long)rx_checksum);
        return 1;
    }

    logger_warn("p2p TX: timeout waiting for checksum of page %u", expected_page_id);
    if (ensure_mode_tx(radio) != 0) {
        return -1;
    }
    return 1;
}

static int send_page_checksum(nrf24_t *radio,
                              uint8_t page_id,
                              uint64_t checksum,
                              uint64_t *rf_bytes_total,
                              uint64_t *rf_frames_total)
{
    uint8_t msg[PAGE_CHECKSUM_MSG_SIZE];
    msg[0] = CONTROL_PREFIX;
    msg[1] = MSG_CHECKSUM;
    msg[2] = page_id;
    encode_u64_le(&msg[3], checksum);

    if (ensure_mode_tx(radio) != 0) {
        return -1;
    }

    int ret = send_with_retries(radio,
                                msg,
                                sizeof(msg),
                                CONTROL_TIMEOUT_MS,
                                "PAGE_CHECKSUM",
                                rf_bytes_total,
                                rf_frames_total);

    if (ensure_mode_rx(radio) != 0) {
        return -1;
    }

    if (ret == 0) {
        logger_info("p2p RX: checksum sent for Page %u", page_id);
    } else {
        logger_warn("p2p RX: failed to send checksum for Page %u", page_id);
    }
    return ret;
}

static void compute_page_orig_sizes(uint32_t total_orig_len,
                                    uint32_t out_sizes[TRANS_NUM_PAGES])
{
    if (!out_sizes) {
        return;
    }

    for (size_t page = 0; page < TRANS_NUM_PAGES; ++page) {
        uint64_t start = ((uint64_t)total_orig_len * page) / TRANS_NUM_PAGES;
        uint64_t end   = ((uint64_t)total_orig_len * (page + 1)) / TRANS_NUM_PAGES;
        if (end < start) {
            end = start;
        }
        out_sizes[page] = (uint32_t)(end - start);
    }
}

static uint32_t estimate_frames_from_comp(uint32_t comp_len)
{
    if (comp_len == 0) {
        return 0;
    }

    uint32_t full_bursts = comp_len / TRANS_DATA_BYTES_PER_BURST;
    uint32_t rem_bytes   = comp_len % TRANS_DATA_BYTES_PER_BURST;

    uint64_t frames = (uint64_t)full_bursts * TRANS_MAX_FRAMES_PER_BURST;
    if (rem_bytes > 0) {
        frames += (rem_bytes + TRANS_DATA_BYTES_PER_FRAME - 1u) / TRANS_DATA_BYTES_PER_FRAME;
    }

    if (frames > UINT32_MAX) {
        frames = UINT32_MAX;
    }
    return (uint32_t)frames;
}

static uint32_t estimate_total_frames(const uint32_t page_comp_sizes[TRANS_NUM_PAGES])
{
    uint64_t total = 0;
    for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
        total += estimate_frames_from_comp(page_comp_sizes ? page_comp_sizes[i] : 0);
    }
    if (total > UINT32_MAX) {
        return UINT32_MAX;
    }
    return (uint32_t)total;
}