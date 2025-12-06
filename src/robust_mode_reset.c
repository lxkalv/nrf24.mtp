#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <zlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "libs/nrf24.h"
#include "libs/logger.h"
#include "libs/app_layer.h"


#define MAX_PAYLOAD         32
#define CONTROL_PREFIX      0xFF
#define DATA_PREFIX         0x00
#define DEFAULT_SPI_DEVICE  "/dev/spidev0.0"

#define MSG_STREAM_INFO     0x01
#define MSG_STREAM_FINISH   0x02
#define MSG_CHECKSUM        0x03
#define MSG_STREAM_READY    0x04

#define CHECKSUM_SIZE       8
#define CHECKSUM_SEND_WINDOW_MS 2000
#define READY_TIMEOUT_MS    2000
#define CONTROL_TIMEOUT_MS  200
#define DATA_TIMEOUT_MS     60
#define CHECKSUM_TIMEOUT_MS 1000
#define FINISH_ACK_TIMEOUT_MS 2000

#define STREAM_INFO_SIZE    16
#define STREAM_READY_SIZE   11
#define STREAM_READY_MAX_ATTEMPTS 400
#define STREAM_READY_WINDOW_MS    2000

#define FNV64_OFFSET_BASIS  1469598103934665603ULL
#define FNV64_PRIME         1099511628211ULL

typedef struct {
    uint8_t  channel;
    unsigned data_rate_kbps;
    int      pa_level_dbm;
    unsigned crc_bytes;
    unsigned retr_delay;
    unsigned retr_tries;
} robust_radio_params_t;

static volatile int abort_requested = 0;

static robust_radio_params_t g_radio_params = {
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

static void sleep_ms_posix(unsigned int ms) {
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

static uint64_t checksum_final(uint64_t state)
{
    return state;
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


static int compress_buffer(const uint8_t *in, size_t in_len, uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) {
        logger_error("compress_buffer: invalid outputs");
        return -1;
    }

    uLong src_len = (uLong)in_len;
    uLong bound = compressBound(src_len == 0 ? 1 : src_len);
    if (bound == 0) bound = 1;

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

static int decompress_buffer(const uint8_t *in, size_t in_len, uint8_t **out_buf, size_t out_len)
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
    int zret = uncompress(buf, &dest_len, in ? in : (const Bytef *)"", (uLong)in_len);
    if (zret != Z_OK || dest_len != out_len) {
        logger_error("uncompress failed (zret=%d, dest=%lu expected=%zu)",
                     zret, (unsigned long)dest_len, out_len);
        free(buf);
        return -1;
    }

    *out_buf = buf;
    return 0;
}

static int store_decompressed_output(const app_config_t *cfg,
                                     const uint8_t *compressed,
                                     size_t compressed_len,
                                     uint32_t original_len)
{
    uint8_t *output = NULL;

    if (compressed_len != 0 || original_len == 0) {
        if (decompress_buffer(compressed,
                              compressed_len,
                              &output,
                              (size_t)original_len) != 0) {
            logger_error("robust RX: failed to decompress payload (%zu bytes)",
                         compressed_len);
            return -1;
        }
    } else {
        logger_error("robust RX: non-empty output requested but compressed buffer is empty");
        return -1;
    }

    const char *dest_label = (cfg && cfg->file_path_rx && cfg->file_path_rx[0])
                           ? cfg->file_path_rx
                           : "(auto)";

    if (app_store_file_bytes(cfg ? cfg->file_path_rx : NULL,
                             output,
                             (size_t)original_len) != 0) {
        logger_error("robust RX: failed to store output bytes");
        free(output);
        return -1;
    }

    free(output);
    logger_succ("robust RX: stored '%s' (%u bytes)", dest_label, original_len);
    return 0;
}

static int derive_frame_layout(size_t data_len,
                               unsigned *out_id_bytes,
                               size_t *out_payload_bytes,
                               uint32_t *out_total_frames)
{
    if (!out_id_bytes || !out_payload_bytes || !out_total_frames) {
        logger_error("derive_frame_layout: invalid outputs");
        return -1;
    }

    for (unsigned id_bytes = 1; id_bytes <= 4; ++id_bytes) {
        size_t payload = MAX_PAYLOAD - 1 - id_bytes;
        if (payload == 0) {
            continue;
        }

        size_t frames = (data_len == 0)
                      ? 0
                      : (data_len + payload - 1) / payload;

        uint64_t max_frames = ((uint64_t)1 << (id_bytes * 8)) - 1;
        if (frames <= max_frames) {
            *out_id_bytes = id_bytes;
            *out_payload_bytes = payload;
            *out_total_frames = (uint32_t)frames;
            return 0;
        }
    }

    logger_error("derive_frame_layout: data too large for frame layout");
    return -1;
}

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
                        label ? label : "frame", attempt);
        }

        if ((attempt % 200) == 0) {
            logger_warn("%s: %u consecutive timeouts, reconfiguring radio",
                        label ? label : "frame", attempt);
            if (configure_radio_runtime(radio) != 0) {
                logger_error("Failed to reconfigure radio during retries: %s", strerror(errno));
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
    return 0;
}

static int ensure_mode_rx(nrf24_t *radio)
{
    if (nrf24_set_mode_rx(radio) < 0) {
        logger_error("nrf24_set_mode_rx failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int send_stream_info(nrf24_t *radio,
                            uint8_t id_bytes,
                            uint32_t comp_len,
                            uint32_t total_frames,
                            uint32_t orig_len,
                            uint64_t *rf_bytes_total,
                            uint64_t *rf_frames_total)
{
    uint8_t msg[STREAM_INFO_SIZE] = {0};
    msg[0] = CONTROL_PREFIX;
    msg[1] = MSG_STREAM_INFO;
    msg[2] = id_bytes;
    msg[3] = 0;
    encode_u32_le(&msg[4], comp_len);
    encode_u32_le(&msg[8], total_frames);
    encode_u32_le(&msg[12], orig_len);
    int ret = send_with_retries(radio,
                                msg,
                                sizeof(msg),
                                CONTROL_TIMEOUT_MS,
                                "STREAM_INFO",
                                rf_bytes_total,
                                rf_frames_total);
    if (ret == 0) {
        logger_succ("robust TX: STREAM_INFO acknowledged (frames=%u, comp=%u, orig=%u)",
                    total_frames,
                    comp_len,
                    orig_len);
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
        logger_succ("robust TX: STREAM_FINISH acknowledged by RX");
    }
    return ret;
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
            logger_error("robust TX: waiting for STREAM_FINISH ack failed: %s",
                         strerror(errno));
            (void)ensure_mode_tx(radio);
            return -1;
        }

        if (len >= 2 && buf[0] == CONTROL_PREFIX) {
            if (buf[1] == MSG_STREAM_FINISH) {
                logger_succ("robust TX: STREAM_FINISH acknowledged by RX");
                if (ensure_mode_tx(radio) != 0) {
                    return -1;
                }
                return 0;
            }

            logger_warn("robust TX: control 0x%02X while waiting for STREAM_FINISH ack",
                        buf[1]);
            continue;
        }

        logger_warn("robust TX: ignoring non-control frame while waiting for STREAM_FINISH ack");
    }

    logger_warn("robust TX: timeout waiting for STREAM_FINISH ack");
    if (ensure_mode_tx(radio) != 0) {
        return -1;
    }
    return 1;
}

static int send_checksum_with_timeout(nrf24_t *radio,
                                      uint64_t checksum,
                                      uint64_t *rf_bytes_total,
                                      uint64_t *rf_frames_total)
{
    uint8_t msg[2 + CHECKSUM_SIZE];
    msg[0] = CONTROL_PREFIX;
    msg[1] = MSG_CHECKSUM;
    encode_u64_le(&msg[2], checksum);

    double start = now_seconds();
    unsigned attempt = 0;

    while ((now_seconds() - start) * 1000.0 < CHECKSUM_SEND_WINDOW_MS) {
        if (rf_bytes_total) {
            *rf_bytes_total += sizeof(msg);
        }
        if (rf_frames_total) {
            *rf_frames_total += 1;
        }

        int ret = nrf24_send_blocking(radio,
                                      msg,
                                      sizeof(msg),
                                      CONTROL_TIMEOUT_MS);
        if (ret == 0) {
            logger_succ("robust RX: CHECKSUM acknowledged by TX");
            return 0;
        }

        if (errno != ETIMEDOUT) {
            logger_error("robust RX: nrf24_send_blocking(CHECKSUM) failed: %s",
                         strerror(errno));
            return -1;
        }

        ++attempt;
        if (attempt == 1 || (attempt % 50) == 0) {
            logger_warn("robust RX: checksum timeout (attempt %u)", attempt);
        }

        if ((attempt % 200) == 0) {
            logger_warn("robust RX: %u checksum timeouts, reconfiguring radio", attempt);
            if (configure_radio_runtime(radio) != 0) {
                logger_error("robust RX: failed to reconfigure radio during checksum retries");
                return -1;
            }
            if (ensure_mode_tx(radio) != 0) {
                return -1;
            }
        }
        sleep_ms_posix(50);
    }

    errno = ETIMEDOUT;
    return -1;
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
            logger_error("robust TX: waiting for STREAM_READY failed: %s", strerror(errno));
            return -1;
        }

        if (len < 2 || buf[0] != CONTROL_PREFIX) {
            logger_warn("robust TX: ignoring unexpected frame while waiting for READY");
            continue;
        }

        if (buf[1] == MSG_STREAM_READY) {
            uint8_t rx_id_bytes = (len >= 3) ? buf[2] : 0;
            uint32_t rx_frames  = (len >= 7) ? decode_u32_le(&buf[3]) : 0;
            uint32_t rx_comp    = (len >= 11) ? decode_u32_le(&buf[7]) : 0;

            if (rx_id_bytes != expected_id_bytes) {
                logger_warn("robust TX: RX id_bytes=%u differs from TX=%u",
                            rx_id_bytes,
                            expected_id_bytes);
            }
            if (expected_frames && rx_frames && rx_frames != expected_frames) {
                logger_warn("robust TX: RX expects %u frames but TX planned %u",
                            rx_frames,
                            expected_frames);
            }
            if (expected_comp_len && rx_comp && rx_comp != expected_comp_len) {
                logger_warn("robust TX: RX reported comp_len=%u but TX has %u",
                            rx_comp,
                            expected_comp_len);
            }

            logger_info("robust TX: RX ready (frames=%u, comp=%u)", rx_frames, rx_comp);

            if (ensure_mode_tx(radio) != 0) {
                return -1;
            }
            return 0;
        }

        logger_warn("robust TX: control 0x%02X while waiting for READY", buf[1]);
    }

    if (ensure_mode_tx(radio) != 0) {
        return -1;
    }

    return 1; /* timeout, caller may retry */
}

/**
 * Save the partially received compressed data to disk.
 *
 * Writes out the current contents of compressed up to compressed_len.
 *
 * Returns 0 on success, -1 on error.
 */
static int save_partial_file(const app_config_t *cfg,
                             const uint8_t *compressed,
                             size_t compressed_len)
{
    if (!cfg || !cfg->file_path_rx || !cfg->file_path_rx[0]) {
        logger_error("save_partial_file: destination path not set");
        return -1;
    }
    if (!compressed) {
        logger_error("save_partial_file: compressed buffer is NULL");
        return -1;
    }

    const char *path = cfg->file_path_rx;
    FILE *fp = fopen(path, "wb");  // overwrite each time we save a snapshot
    if (!fp) {
        logger_error("save_partial_file: fopen('%s') failed: %s",
                     path, strerror(errno));
        return -1;
    }

    size_t written = fwrite(compressed, 1, compressed_len, fp);
    if (written != compressed_len) {
        logger_error("save_partial_file: fwrite failed (written=%zu expected=%zu)",
                     written, compressed_len);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    logger_info("save_partial_file: wrote %zu bytes to '%s'", compressed_len, path);
    return 0;
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
            logger_error("robust RX: STREAM_READY send failed: %s", strerror(errno));
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
                logger_error("robust RX: failed to reconfigure radio while sending STREAM_READY");
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
        logger_warn("robust RX: STREAM_READY not acknowledged after %u attempts", attempt);
        if (ensure_mode_rx(radio) != 0) {
            return -1;
        }
        errno = ETIMEDOUT;
        return 1; /* indicate timeout */
    }

    logger_succ("robust RX: STREAM_READY acknowledged (frames=%u, comp=%u)",
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
    uint8_t *compressed = NULL;
    size_t compressed_len = 0;
    int exit_code = 1;
    uint64_t tx_rf_bytes = 0;
    uint64_t tx_rf_frames = 0;
    double tx_start = 0.0;
    unsigned finish_ack_attempts = 0;

    if (!spi_dev) {
        logger_error("run_tx: missing SPI device");
        return 1;
    }
    if (!file_data && file_len > 0) {
        logger_error("run_tx: input buffer is NULL but length > 0");
        return 1;
    }

    if (compress_buffer(file_data, file_len, &compressed, &compressed_len) != 0) {
        goto cleanup;
    }

    if (compressed_len > UINT32_MAX || file_len > UINT32_MAX) {
        logger_error("File too large for robust_mode (limit 4 GiB per stage)");
        goto cleanup;
    }

    unsigned id_bytes = 0;
    size_t payload_bytes = 0;
    uint32_t total_frames = 0;
    if (derive_frame_layout(compressed_len, &id_bytes, &payload_bytes, &total_frames) != 0) {
        goto cleanup;
    }

    const char *input_label = (cfg && cfg->file_path_tx && cfg->file_path_tx[0])
                            ? cfg->file_path_tx
                            : "(auto)";

    logger_info("robust TX: '%s' -> %zu bytes compressed (%u frames, %u ID bytes)",
                input_label,
                compressed_len,
                (unsigned)total_frames,
                id_bytes);

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
    if (configure_radio_runtime(&radio) < 0) {
        logger_error("configure_radio_runtime failed: %s", strerror(errno));
        nrf24_deinit(&radio);
        goto cleanup;
    }
    if (maybe_verify_radio_config(cfg, &radio, "TX init") != 0) {
        nrf24_deinit(&radio);
        goto cleanup;
    }
    if (ensure_mode_tx(&radio) != 0) {
        nrf24_deinit(&radio);
        goto cleanup;
    }

    tx_start = now_seconds();

    if (send_stream_info(&radio,
                         (uint8_t)id_bytes,
                         (uint32_t)compressed_len,
                         total_frames,
                         (uint32_t)file_len,
                         &tx_rf_bytes,
                         &tx_rf_frames) != 0) {
        logger_error("Failed to send STREAM_INFO");
        nrf24_deinit(&radio);
        goto cleanup;
    }

    while (1) {
        int ready_ret = wait_for_stream_ready(&radio,
                                              (uint8_t)id_bytes,
                                              total_frames,
                                              (uint32_t)compressed_len);
        if (ready_ret == 0) {
            break;
        }
        if (ready_ret < 0) {
            logger_error("robust TX: failed while waiting for STREAM_READY");
            nrf24_deinit(&radio);
            goto cleanup;
        }

        logger_warn("robust TX: RX not ready yet, resending STREAM_INFO");
        if (send_stream_info(&radio,
                             (uint8_t)id_bytes,
                             (uint32_t)compressed_len,
                             total_frames,
                             (uint32_t)file_len,
                             &tx_rf_bytes,
                             &tx_rf_frames) != 0) {
            logger_error("Failed to resend STREAM_INFO");
            nrf24_deinit(&radio);
            goto cleanup;
        }
    }

    sleep_ms_posix(20);
    uint64_t checksum_state;
    checksum_init(&checksum_state);
    checksum_update(&checksum_state, compressed, compressed_len);
    uint64_t tx_checksum = checksum_final(checksum_state);

    int transfer_complete = 0;
    unsigned resend_round = 0;
    //unsigned next_tx_progress_pct = 10;

    while (!transfer_complete) {
        unsigned next_tx_progress_pct = 10;

        logger_info("robust TX: sending data (round %u)", resend_round + 1);
        if (payload_bytes > 0) {
            for (unsigned warm = 0; warm < 3; ++warm) {
                uint8_t warm_payload[MAX_PAYLOAD];
                warm_payload[0] = DATA_PREFIX;

                /* Usamos un FrameID fuera de rango para que RX lo ignore */
                uint32_t warm_id = total_frames + 100 + warm;
                uint32_t tmp_id = warm_id;
                for (unsigned b = 0; b < id_bytes; ++b) {
                    warm_payload[1 + b] = (uint8_t)(tmp_id & 0xFFu);
                    tmp_id >>= 8;
                }

                /* Datos dummy (no importan) */
                memset(&warm_payload[1 + id_bytes], 0xAA, payload_bytes);

                /* No pasa nada si esto falla: es solo calentamiento */
                (void)send_with_retries(&radio,
                                        warm_payload,
                                        (uint8_t)(1 + id_bytes + payload_bytes),
                                        DATA_TIMEOUT_MS,
                                        "DATA-WARMUP",
                                        &tx_rf_bytes,
                                        &tx_rf_frames);
            }
        }
        size_t offset = 0;
        for (uint32_t frame = 0; frame < total_frames; ++frame) {
            size_t chunk = payload_bytes;
            if (offset + chunk > compressed_len) {
                chunk = compressed_len - offset;
            }

            uint8_t payload[MAX_PAYLOAD];
            payload[0] = DATA_PREFIX;
            uint32_t frame_id = frame;
            for (unsigned b = 0; b < id_bytes; ++b) {
                payload[1 + b] = (uint8_t)(frame_id & 0xFFu);
                frame_id >>= 8;
            }
            memcpy(&payload[1 + id_bytes], compressed + offset, chunk);

            char label[32];
            snprintf(label, sizeof(label), "DATA[%u]", frame);
            if (send_with_retries(&radio,
                                payload,
                                (uint8_t)(1 + id_bytes + chunk),
                                DATA_TIMEOUT_MS,
                                label,          // <-- usar label aquí
                                &tx_rf_bytes,
                                &tx_rf_frames) != 0) {
                logger_error("Failed to send DATA frame %u", frame);
                nrf24_deinit(&radio);
                goto cleanup;
            }


            offset += chunk;

            if (total_frames > 0) {
                uint32_t pct = (uint32_t)(((uint64_t)(frame + 1) * 100u) / total_frames);
                if (frame + 1 == total_frames) {
                    pct = 100;
                }
                if (pct >= next_tx_progress_pct) {
                    logger_info("robust TX: progress %u%% (%u/%u frames)",
                                pct,
                                frame + 1,
                                total_frames);
                    while (next_tx_progress_pct <= pct && next_tx_progress_pct < 100) {
                        next_tx_progress_pct += 10;
                    }
                }
            }
        }

        if (ensure_mode_rx(&radio) != 0) {
            nrf24_deinit(&radio);
            goto cleanup;
        }

        logger_info("robust TX: waiting for checksum reply");
        int checksum_ok = 0;
        double wait_start = now_seconds();
        while (!checksum_ok && (now_seconds() - wait_start) * 1000.0 < CHECKSUM_TIMEOUT_MS) {
            uint8_t buf[MAX_PAYLOAD];
            uint8_t len = sizeof(buf);
            int ret = nrf24_recv_blocking(&radio, buf, &len, CHECKSUM_TIMEOUT_MS);
            if (ret < 0) {
                if (errno == ETIMEDOUT) {
                    continue;
                }
                logger_error("robust TX: nrf24_recv_blocking failed: %s", strerror(errno));
                nrf24_deinit(&radio);
                goto cleanup;
            }

            if (len < 2) {
                logger_warn("robust TX: short frame len=%u while waiting for checksum", len);
                continue;
            }

            if (buf[0] != CONTROL_PREFIX) {
                logger_warn("robust TX: unexpected data frame during checksum wait");
                continue;
            }

            if (buf[1] == MSG_CHECKSUM) {
                if (len < 2 + CHECKSUM_SIZE) {
                    logger_warn("robust TX: malformed CHECKSUM frame");
                    continue;
                }
                uint64_t rx_checksum = decode_u64_le(&buf[2]);
                if (rx_checksum == tx_checksum) {
                    checksum_ok = 1;
                    break;
                }
                logger_warn("robust TX: checksum mismatch (expected 0x%016llX, got 0x%016llX)",
                            (unsigned long long)tx_checksum,
                            (unsigned long long)rx_checksum);
                break;
            }

            if (buf[1] == MSG_STREAM_INFO) {
                logger_warn("robust TX: RX resent STREAM_INFO ack; ignoring");
                continue;
            }

            if (buf[1] == MSG_STREAM_FINISH) {
                logger_info("robust TX: RX already finished transfer");
                checksum_ok = 1;
                break;
            }
        }

        if (!checksum_ok) {
            logger_warn("robust TX: checksum not confirmed, resending data");
            if (ensure_mode_tx(&radio) != 0) {
                nrf24_deinit(&radio);
                goto cleanup;
            }
            ++resend_round;
            continue;
        }

        if (ensure_mode_tx(&radio) != 0) {
            nrf24_deinit(&radio);
            goto cleanup;
        }

        while (!transfer_complete) {
            if (send_stream_finish(&radio, &tx_rf_bytes, &tx_rf_frames) != 0) {
                logger_error("robust TX: failed to send STREAM_FINISH");
                nrf24_deinit(&radio);
                goto cleanup;
            }

            int finish_ack = wait_for_stream_finish_ack(&radio);
            if (finish_ack == 0) {
                transfer_complete = 1;
                break;
            }

            if (finish_ack > 0) {
                ++finish_ack_attempts;
                logger_warn("robust TX: STREAM_FINISH ack timeout, retrying (attempt %u)",
                            finish_ack_attempts);
                continue;
            }

            logger_error("robust TX: failed while waiting for STREAM_FINISH ack");
            nrf24_deinit(&radio);
            goto cleanup;
        }
    }

    if (tx_start > 0.0) {
        double tx_end = now_seconds();
        double elapsed = tx_end - tx_start;
        if (elapsed <= 0.0) {
            elapsed = 1e-9;
        }
        double user_rate_kib = ((double)file_len / 1024.0) / elapsed;
        double rf_rate_kib   = ((double)tx_rf_bytes / 1024.0) / elapsed;
        logger_info("robust TX throughput: user=%.2f KiB/s (%zu bytes in %.2fs), rf=%.2f KiB/s (%llu bytes, %llu frames)",
                    user_rate_kib,
                    file_len,
                    elapsed,
                    rf_rate_kib,
                    (unsigned long long)tx_rf_bytes,
                    (unsigned long long)tx_rf_frames);
    }

    logger_succ("robust TX: transfer complete (%zu bytes -> %zu bytes)",
                file_len,
                compressed_len);
    nrf24_deinit(&radio);
    exit_code = 0;

cleanup:
    free(compressed);
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

    if (nrf24_init(&radio, &radio_cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    if (configure_radio_runtime(&radio) < 0) {
        logger_error("configure_radio_runtime failed: %s", strerror(errno));
        nrf24_deinit(&radio);
        return 1;
    }
    if (maybe_verify_radio_config(cfg, &radio, "RX init") != 0) {
        nrf24_deinit(&radio);
        return 1;
    }
    if (ensure_mode_rx(&radio) != 0) {
        nrf24_deinit(&radio);
        return 1;
    }

    uint8_t *compressed = NULL;
    size_t compressed_len = 0;
    size_t compressed_cap = 0;
    uint8_t *frame_received = NULL;
    unsigned id_bytes = 0;
    size_t payload_bytes = 0;
    uint32_t frames_received = 0;
    uint32_t expected_frames = 0;
    uint32_t original_len = 0;
    int checksum_sent = 0;
    int have_info = 0;
    int output_stored = 0;
    uint64_t rf_rx_bytes = 0;
    uint64_t rf_rx_frames = 0;
    uint64_t rf_tx_bytes = 0;
    uint64_t rf_tx_frames = 0;
    unsigned next_rx_progress_pct = 10;
    int32_t highest_frame_seen = -1;
    double rx_start = now_seconds();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);   

    logger_info("robust RX: waiting for STREAM_INFO");

    int done = 0;
    while (!done) {
        uint8_t buf[MAX_PAYLOAD];
        uint8_t len = sizeof(buf);
        int ret = nrf24_recv_blocking(&radio, buf, &len, 500);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            logger_error("robust RX: nrf24_recv_blocking failed: %s", strerror(errno));
            goto cleanup;
        }

        rf_rx_bytes += len;
        rf_rx_frames += 1;


        char cmd[16];
        ssize_t n = read(STDIN_FILENO, cmd, sizeof(cmd) - 1);
        if (n > 0) {
            cmd[n] = '\0';
            if (strstr(cmd, "STOP") != NULL) {
                logger_warn("RX: STOP command received from Python! Aborting...");
                abort_requested = 1;
            }
        }

        if (abort_requested) {
            if (save_partial_file(cfg, compressed, compressed_len) != 0) {
                logger_warn("robust RX: failed to save partial file");
            }
            abort();
        }

        if (len < 1) {
            logger_warn("robust RX: empty frame");
            continue;
        }

        if (buf[0] == CONTROL_PREFIX) {
            if (len < 2) {
                logger_warn("robust RX: short control frame");
                continue;
            }

            uint8_t type = buf[1];
            if (type == MSG_STREAM_INFO) {
                if (len < STREAM_INFO_SIZE) {
                    logger_warn("robust RX: malformed STREAM_INFO (len=%u)", len);
                    continue;
                }

                uint8_t new_id_bytes        = buf[2];
                uint32_t new_compressed_len = decode_u32_le(&buf[4]);
                uint32_t new_expected_frames= decode_u32_le(&buf[8]);
                uint32_t new_original_len   = decode_u32_le(&buf[12]);

                if (new_id_bytes == 0 || new_id_bytes > 4) {
                    logger_error("robust RX: invalid FrameID length %u", new_id_bytes);
                    goto cleanup;
                }

                size_t new_payload_bytes = MAX_PAYLOAD - 1 - new_id_bytes;
                if (new_payload_bytes == 0) {
                    logger_error("robust RX: payload too small for FrameIDs");
                    goto cleanup;
                }

                if (new_compressed_len > UINT32_MAX) {
                    logger_error("robust RX: compressed length too large");
                    goto cleanup;
                }

                if (new_expected_frames == 0 && new_compressed_len > 0) {
                    new_expected_frames = (uint32_t)((new_compressed_len + new_payload_bytes - 1) / new_payload_bytes);
                }

                int same_stream = have_info &&
                                  new_id_bytes        == id_bytes &&
                                  new_compressed_len  == compressed_len &&
                                  new_expected_frames == expected_frames &&
                                  new_original_len    == original_len;

                if (!same_stream) {
                    if (new_compressed_len > 0) {
                        uint8_t *tmp = (uint8_t *)realloc(compressed, new_compressed_len);
                        if (!tmp) {
                            logger_error("robust RX: realloc(%u) failed", new_compressed_len);
                            goto cleanup;
                        }
                        compressed = tmp;
                    } else {
                        free(compressed);
                        compressed = NULL;
                    }
                    compressed_cap = new_compressed_len;

                    free(frame_received);
                    frame_received = NULL;
                    if (new_expected_frames > 0) {
                        frame_received = (uint8_t *)calloc(new_expected_frames, 1);
                        if (!frame_received) {
                            logger_error("robust RX: calloc failed for frame tracking");
                            goto cleanup;
                        }
                    }

                    if (compressed && new_compressed_len > 0) {
                        memset(compressed, 0, new_compressed_len);
                    }
                    frames_received = 0;
                    checksum_sent = 0;
                    next_rx_progress_pct = 10;
                    highest_frame_seen = -1;
                } else {
                    logger_info("robust RX: STREAM_INFO matches current transfer; keeping buffered frames");
                }

                id_bytes = new_id_bytes;
                compressed_len = new_compressed_len;
                expected_frames = new_expected_frames;
                original_len = new_original_len;
                payload_bytes = new_payload_bytes;
                have_info = 1;

                logger_info("robust RX: STREAM_INFO -> comp=%zu bytes, orig=%u bytes, frames=%u, id_bytes=%u",
                            compressed_len,
                            original_len,
                            expected_frames,
                            id_bytes);

                int ready_send = send_stream_ready(&radio,
                                                   id_bytes,
                                                   expected_frames,
                                                   (uint32_t)compressed_len,
                                                   &rf_tx_bytes,
                                                   &rf_tx_frames);
                if (ready_send < 0) {
                    goto cleanup;
                }
                if (ready_send > 0) {
                    logger_warn("robust RX: STREAM_READY delivery timed out, waiting for retransmit");
                    continue;
                }
                continue;
            }

            if (type == MSG_STREAM_FINISH) {
                if (!checksum_sent) {
                    logger_warn("robust RX: STREAM_FINISH received before checksum sent");
                }
                if (!output_stored) {
                    if (store_decompressed_output(cfg,
                                                  compressed,
                                                  compressed_len,
                                                  original_len) != 0) {
                        logger_error("robust RX: failed to finalize output; aborting");
                        goto cleanup;
                    }
                    output_stored = 1;
                } else {
                    logger_info("robust RX: STREAM_FINISH already processed; resending ack");
                }

                if (ensure_mode_tx(&radio) != 0) {
                    goto cleanup;
                }
                if (send_stream_finish(&radio, &rf_tx_bytes, &rf_tx_frames) != 0) {
                    logger_error("robust RX: failed to acknowledge STREAM_FINISH");
                    goto cleanup;
                }
                if (ensure_mode_rx(&radio) != 0) {
                    goto cleanup;
                }

                done = 1;
                break;
            }

            if (type == MSG_CHECKSUM) {
                logger_info("robust RX: checksum echoed back, ignoring");
                continue;
            }

            logger_warn("robust RX: unknown control type %u", type);
            continue;
        }

        

        if (!have_info) {
            logger_warn("robust RX: data frame before STREAM_INFO");
            continue;
        }

        if (buf[0] != DATA_PREFIX) {
            logger_warn("robust RX: data frame has invalid prefix 0x%02X", buf[0]);
            continue;
        }

        if (len <= 1 + id_bytes) {
            logger_warn("robust RX: DATA frame too short (len=%u)", len);
            continue;
        }

        if (checksum_sent) {
            logger_warn("robust RX: data received after checksum sent; assuming TX resend");
            checksum_sent = 0;
            highest_frame_seen = -1;
            next_rx_progress_pct = 10;
        }

        uint32_t frame_id = 0;
        for (unsigned b = 0; b < id_bytes; ++b) {
            frame_id |= ((uint32_t)buf[1 + b]) << (8 * b);
        }

        if (frame_id >= expected_frames && expected_frames > 0) {
            logger_warn("robust RX: FrameID %u out of range (%u)", frame_id, expected_frames);
            continue;
        }

        size_t chunk_len = len - 1 - id_bytes;
        size_t offset = (size_t)frame_id * payload_bytes;
        if (offset + chunk_len > compressed_len) {
            logger_warn("robust RX: chunk exceeds buffer (%zu > %zu)", offset + chunk_len, compressed_len);
            continue;
        }

        memcpy(compressed + offset, &buf[1 + id_bytes], chunk_len);
        if (frame_received && !frame_received[frame_id]) {
            frame_received[frame_id] = 1;
            ++frames_received;

            if (expected_frames > 0 && (int32_t)frame_id > highest_frame_seen) {
                highest_frame_seen = (int32_t)frame_id;
                uint32_t pct = (uint32_t)(((uint64_t)(frame_id + 1) * 100u) / expected_frames);
                if (frame_id + 1 >= expected_frames) {
                    pct = 100;
                }
                if (pct >= next_rx_progress_pct) {
                    logger_info("robust RX: progress %u%% (frame_id=%u/%u)",
                                pct,
                                frame_id + 1,
                                expected_frames);
                    while (next_rx_progress_pct <= pct && next_rx_progress_pct < 100) {
                        next_rx_progress_pct += 10;
                    }
                }
            }
        }

        if (expected_frames == 0) {
            continue;
        }

        if (!checksum_sent && frame_id == expected_frames - 1) {
            int missing_total = 0;
            if (frame_received) {
                for (uint32_t missing = 0; missing < expected_frames; ++missing) {
                    if (!frame_received[missing]) {
                        logger_warn("robust RX: frame %u missing before checksum send", missing);
                        ++missing_total;
                    }
                }
            }
            if (missing_total == 0) {
                logger_info("robust RX: last frame (ID=%u) received, checksum covers all frames", frame_id);
            } else {
                logger_warn("robust RX: checksum computed with %d missing frame(s)", missing_total);
            }

            uint64_t checksum_state;
            checksum_init(&checksum_state);
            checksum_update(&checksum_state, compressed, compressed_len);
            uint64_t rx_checksum = checksum_final(checksum_state);

            if (ensure_mode_tx(&radio) != 0) {
                goto cleanup;
            }
            if (send_checksum_with_timeout(&radio,
                                           rx_checksum,
                                           &rf_tx_bytes,
                                           &rf_tx_frames) != 0) {
                logger_warn("robust RX: checksum send window elapsed, expecting resend");
                if (ensure_mode_rx(&radio) != 0) {
                    goto cleanup;
                }
                continue;
            }
            checksum_sent = 1;
            if (ensure_mode_rx(&radio) != 0) {
                goto cleanup;
            }
        }
    }

    if (!checksum_sent && expected_frames > 0) {
        logger_warn("robust RX: transfer finished without checksum phase");
    }

    if (!output_stored) {
        if (store_decompressed_output(cfg,
                                      compressed,
                                      compressed_len,
                                      original_len) != 0) {
            goto cleanup;
        }
        output_stored = 1;
    }

    if (rx_start > 0.0) {
        double rx_end = now_seconds();
        double elapsed = rx_end - rx_start;
        if (elapsed <= 0.0) {
            elapsed = 1e-9;
        }
        double user_rate_kib = ((double)original_len / 1024.0) / elapsed;
        double rf_rate_kib   = ((double)rf_rx_bytes / 1024.0) / elapsed;
        logger_info("robust RX throughput: user=%.2f KiB/s (%u bytes in %.2fs), rf=%.2f KiB/s (%llu bytes, %llu frames)",
                    user_rate_kib,
                    original_len,
                    elapsed,
                    rf_rate_kib,
                    (unsigned long long)rf_rx_bytes,
                    (unsigned long long)rf_rx_frames);
    }

    nrf24_deinit(&radio);
    free(frame_received);
    free(compressed);
    return 0;

cleanup:
    nrf24_deinit(&radio);
    free(frame_received);
    free(compressed);
    return 1;
}

int main(int argc, char **argv)
{
    app_config_t cfg;
    if (app_parse_arguments(argc, argv, &cfg) != 0) {
        app_print_usage(argv[0]);
        return 1;
    }

    char log_path[64];
    if (cfg.mode == APP_MODE_TX) {
        snprintf(log_path, sizeof(log_path), "robust_tx.log");
    } else if (cfg.mode == APP_MODE_RX) {
        snprintf(log_path, sizeof(log_path), "robust_rx.log");
    } else {
        snprintf(log_path, sizeof(log_path), "robust.log");
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
            logger_error("robust TX: failed to load input bytes");
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