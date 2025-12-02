#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h> /* usleep */
#include <zlib.h>

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
#define CHECKSUM_SEND_WINDOW_MS 500
#define READY_TIMEOUT_MS    2000
#define CONTROL_TIMEOUT_MS  100
#define DATA_TIMEOUT_MS     50    /* Aumentado para dar margen en Linux */
#define CHECKSUM_TIMEOUT_MS 1000

#define STREAM_INFO_SIZE    16
#define STREAM_READY_SIZE   11
#define STREAM_READY_MAX_ATTEMPTS 400
#define STREAM_READY_WINDOW_MS    2000

#define FNV64_OFFSET_BASIS  1469598103934665603ULL
#define FNV64_PRIME         1099511628211ULL

/* --- FIX 1: Configuración más robusta para evitar timeouts por latencia del OS --- */
typedef struct {
    uint8_t  channel;
    unsigned data_rate_kbps;
    int      pa_level_dbm;
    unsigned crc_bytes;
    unsigned retr_delay;
    unsigned retr_tries;
} robust_radio_params_t;

static robust_radio_params_t g_radio_params = {
    .channel        = 76,
    .data_rate_kbps = 1000,
    .pa_level_dbm   = -18,
    .crc_bytes      = 2,
    /* Cambiado de 2 a 15 (Max 4000us). 
       Esto es vital para que el HW espere si el RX está ocupado logueando/escribiendo disco */
    .retr_delay     = 15, 
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

static void update_radio_params_from_config(const app_config_t *cfg)
{
    if (!cfg) {
        return;
    }

    g_radio_params.channel        = (uint8_t)cfg->channel;
    g_radio_params.data_rate_kbps = map_data_rate_kbps(cfg->data_rate);
    g_radio_params.pa_level_dbm   = map_pa_level_dbm(cfg->pa_level);
    g_radio_params.crc_bytes      = map_crc_bytes(cfg->crc_bytes);
    /* Forzamos delay alto aunque la config diga otra cosa, por estabilidad */
    // g_radio_params.retr_delay     = (unsigned)cfg->retransmission_delay; 
    // g_radio_params.retr_tries     = (unsigned)cfg->retransmission_tries;
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
            logger_error("malloc(%zu) failed", out_len);
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

/* Helper para hacer flush de ambos FIFOs */
static void flush_radio_buffers(nrf24_t *radio) {
    uint8_t cmd_flush_tx = 0xE1;
    uint8_t cmd_flush_rx = 0xE2;
    nrf24_write_buf(radio, cmd_flush_tx, NULL, 0);
    nrf24_write_buf(radio, cmd_flush_rx, NULL, 0);
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
        /* Logs menos frecuentes para no saturar consola */
        if (attempt == 1 || (attempt % 100) == 0) {
            logger_warn("%s: timeout waiting for ACK (attempt %u)",
                        label ? label : "frame", attempt);
        }

        if ((attempt % 200) == 0) {
            logger_warn("%s: %u consecutive timeouts, reconfiguring radio",
                        label ? label : "frame", attempt);
            
            /* FIX: Flush buffers before reconfig to clear stuck packets */
            flush_radio_buffers(radio);

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
    
    /* FIX: Flush before control message */
    flush_radio_buffers(radio);

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
        if ((attempt % 10) == 0) {
             /* Logging ligero para no frenar al RX */
            // logger_warn("robust RX: checksum timeout (attempt %u)", attempt);
        }
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
            continue;
        }

        if (buf[1] == MSG_STREAM_READY) {
            uint32_t rx_frames  = (len >= 7) ? decode_u32_le(&buf[3]) : 0;
            uint32_t rx_comp    = (len >= 11) ? decode_u32_le(&buf[7]) : 0;

            logger_info("robust TX: RX ready (frames=%u, comp=%u)", rx_frames, rx_comp);

            if (ensure_mode_tx(radio) != 0) {
                return -1;
            }
            return 0;
        }
    }

    if (ensure_mode_tx(radio) != 0) {
        return -1;
    }

    return 1; /* timeout */
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

    /* FIX: Flush buffers before sending critical control packet */
    flush_radio_buffers(radio);

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
    }

    if (!sent) {
        logger_warn("robust RX: STREAM_READY not acknowledged after %u attempts", attempt);
        if (ensure_mode_rx(radio) != 0) {
            return -1;
        }
        errno = ETIMEDOUT;
        return 1;
    }

    logger_succ("robust RX: STREAM_READY acknowledged");

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

    if (!spi_dev) {
        logger_error("run_tx: missing SPI device");
        return 1;
    }

    if (compress_buffer(file_data, file_len, &compressed, &compressed_len) != 0) {
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
                input_label, compressed_len, (unsigned)total_frames, id_bytes);

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
    
    /* Limpiamos buffers antes de empezar */
    flush_radio_buffers(&radio);
    ensure_mode_tx(&radio);

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
            nrf24_deinit(&radio);
            goto cleanup;
        }
    }

    /* FIX 2: Espera crítica para sincronización RX.
       RX acaba de enviar ACK a STREAM_READY, necesita tiempo para volver a modo RX.
       Sin esto, el frame 0 se pierde casi siempre. */
    sleep(20000); // 20ms

    uint64_t checksum_state;
    checksum_init(&checksum_state);
    checksum_update(&checksum_state, compressed, compressed_len);
    uint64_t tx_checksum = checksum_final(checksum_state);

    int transfer_complete = 0;
    unsigned resend_round = 0;

    while (!transfer_complete) {
        if (resend_round > 0) {
            logger_info("robust TX: sending data (round %u)", resend_round + 1);
        } else {
            logger_info("robust TX: sending data...");
        }

        /* Flush para evitar que basura previa cause timeouts */
        flush_radio_buffers(&radio);

        size_t offset = 0;
        unsigned next_tx_progress_pct = 10;

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

            if (send_with_retries(&radio,
                                  payload,
                                  (uint8_t)(1 + id_bytes + chunk),
                                  DATA_TIMEOUT_MS,
                                  "DATA",
                                  &tx_rf_bytes,
                                  &tx_rf_frames) != 0) {
                logger_error("Failed to send DATA frame %u", frame);
                nrf24_deinit(&radio);
                goto cleanup;
            }

            offset += chunk;

            if (total_frames > 0 && resend_round == 0) {
                uint32_t pct = (uint32_t)(((uint64_t)(frame + 1) * 100u) / total_frames);
                if (frame + 1 == total_frames) pct = 100;
                if (pct >= next_tx_progress_pct) {
                    logger_info("robust TX: progress %u%% (%u/%u)", pct, frame + 1, total_frames);
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
        
        /* Limpiar buffer RX antes de esperar checksum */
        flush_radio_buffers(&radio);

        while (!checksum_ok && (now_seconds() - wait_start) * 1000.0 < CHECKSUM_TIMEOUT_MS) {
            uint8_t buf[MAX_PAYLOAD];
            uint8_t len = sizeof(buf);
            int ret = nrf24_recv_blocking(&radio, buf, &len, CHECKSUM_TIMEOUT_MS);
            if (ret < 0) {
                if (errno == ETIMEDOUT) continue;
                logger_error("robust TX: nrf24_recv_blocking failed: %s", strerror(errno));
                nrf24_deinit(&radio);
                goto cleanup;
            }

            if (len < 2 || buf[0] != CONTROL_PREFIX) continue;

            if (buf[1] == MSG_CHECKSUM) {
                uint64_t rx_checksum = decode_u64_le(&buf[2]);
                if (rx_checksum == tx_checksum) {
                    checksum_ok = 1;
                    break;
                }
                logger_warn("robust TX: checksum mismatch (exp 0x%016llX, got 0x%016llX)",
                            (unsigned long long)tx_checksum,
                            (unsigned long long)rx_checksum);
                break;
            }

            if (buf[1] == MSG_STREAM_FINISH) {
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
            /* Esperar un momento antes de empezar a reenviar para dejar al RX acomodarse */
            sleep(100000); 
            ++resend_round;
            continue;
        }

        if (ensure_mode_tx(&radio) != 0) {
            nrf24_deinit(&radio);
            goto cleanup;
        }

        if (send_stream_finish(&radio, &tx_rf_bytes, &tx_rf_frames) != 0) {
            logger_warn("robust TX: failed to send STREAM_FINISH");
        }

        transfer_complete = 1;
    }

    if (tx_start > 0.0) {
        double tx_end = now_seconds();
        double elapsed = tx_end - tx_start;
        if (elapsed <= 0.0) elapsed = 1e-9;
        double user_rate_kib = ((double)file_len / 1024.0) / elapsed;
        double rf_rate_kib   = ((double)tx_rf_bytes / 1024.0) / elapsed;
        logger_info("robust TX throughput: user=%.2f KiB/s, rf=%.2f KiB/s",
                    user_rate_kib, rf_rate_kib);
    }

    logger_succ("robust TX: transfer complete");
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
    
    flush_radio_buffers(&radio);
    if (ensure_mode_rx(&radio) != 0) {
        nrf24_deinit(&radio);
        return 1;
    }

    uint8_t *compressed = NULL;
    size_t compressed_len = 0;
    uint8_t *frame_received = NULL;
    unsigned id_bytes = 0;
    size_t payload_bytes = 0;
    uint32_t expected_frames = 0;
    uint32_t original_len = 0;
    int checksum_sent = 0;
    int frames_received=0;
    int have_info = 0;
    uint64_t rf_rx_bytes = 0;
    uint64_t rf_rx_frames = 0;
    uint64_t rf_tx_bytes = 0;
    uint64_t rf_tx_frames = 0;
    unsigned next_rx_progress_pct = 10;
    int32_t highest_frame_seen = -1;
    double rx_start = now_seconds();

    logger_info("robust RX: waiting for STREAM_INFO");

    int done = 0;
    while (!done) {
        uint8_t buf[MAX_PAYLOAD];
        uint8_t len = sizeof(buf);
        int ret = nrf24_recv_blocking(&radio, buf, &len, 500);
        if (ret < 0) {
            if (errno == ETIMEDOUT) continue;
            logger_error("robust RX: error %s", strerror(errno));
            goto cleanup;
        }

        rf_rx_bytes += len;
        rf_rx_frames += 1;

        if (len < 1) continue;

        if (buf[0] == CONTROL_PREFIX) {
            if (len < 2) continue;
            uint8_t type = buf[1];

            if (type == MSG_STREAM_INFO) {
                if (len < STREAM_INFO_SIZE) continue;

                uint8_t new_id_bytes        = buf[2];
                uint32_t new_compressed_len = decode_u32_le(&buf[4]);
                uint32_t new_expected_frames= decode_u32_le(&buf[8]);
                uint32_t new_original_len   = decode_u32_le(&buf[12]);

                size_t new_payload_bytes = MAX_PAYLOAD - 1 - new_id_bytes;

                int same_stream = have_info &&
                                  new_id_bytes        == id_bytes &&
                                  new_compressed_len  == compressed_len &&
                                  new_expected_frames == expected_frames;

                if (!same_stream) {
                    free(compressed);
                    compressed = NULL;
                    if (new_compressed_len > 0) {
                        compressed = (uint8_t *)malloc(new_compressed_len);
                        if (!compressed) goto cleanup;
                        memset(compressed, 0, new_compressed_len);
                    }

                    free(frame_received);
                    frame_received = NULL;
                    if (new_expected_frames > 0) {
                        frame_received = (uint8_t *)calloc(new_expected_frames, 1);
                        if (!frame_received) goto cleanup;
                    }

                    frames_received = 0;
                    checksum_sent = 0;
                    next_rx_progress_pct = 10;
                    highest_frame_seen = -1;
                }

                id_bytes = new_id_bytes;
                compressed_len = new_compressed_len;
                expected_frames = new_expected_frames;
                original_len = new_original_len;
                payload_bytes = new_payload_bytes;
                have_info = 1;

                logger_info("robust RX: STREAM_INFO (frames=%u)", expected_frames);

                /* Flush antes de enviar READY */
                flush_radio_buffers(&radio);
                int ready_send = send_stream_ready(&radio,
                                                   id_bytes,
                                                   expected_frames,
                                                   (uint32_t)compressed_len,
                                                   &rf_tx_bytes,
                                                   &rf_tx_frames);
                if (ready_send < 0) goto cleanup;
                continue;
            }

            if (type == MSG_STREAM_FINISH) {
                done = 1;
                break;
            }
            continue;
        }

        if (!have_info) continue;
        if (buf[0] != DATA_PREFIX) continue;
        if (len <= 1 + id_bytes) continue;

        /* Si llega DATA después de haber enviado checksum, es que TX no recibió el checksum.
           Reiniciamos flags para permitir reenvío del checksum al final. */
        if (checksum_sent) {
            logger_warn("robust RX: data received after checksum sent (TX retry detected)");
            checksum_sent = 0;
            /* No reseteamos highest_frame_seen para evitar logs de progreso duplicados */
        }

        uint32_t frame_id = 0;
        for (unsigned b = 0; b < id_bytes; ++b) {
            frame_id |= ((uint32_t)buf[1 + b]) << (8 * b);
        }

        size_t chunk_len = len - 1 - id_bytes;
        size_t offset = (size_t)frame_id * payload_bytes;
        
        if (offset + chunk_len <= compressed_len) {
            memcpy(compressed + offset, &buf[1 + id_bytes], chunk_len);
            
            if (frame_received && !frame_received[frame_id]) {
                frame_received[frame_id] = 1;

                if (expected_frames > 0 && (int32_t)frame_id > highest_frame_seen) {
                    highest_frame_seen = (int32_t)frame_id;
                    uint32_t pct = (uint32_t)(((uint64_t)(frame_id + 1) * 100u) / expected_frames);
                    if (pct >= next_rx_progress_pct) {
                        logger_info("robust RX: progress %u%%", pct);
                        while (next_rx_progress_pct <= pct && next_rx_progress_pct < 100) {
                            next_rx_progress_pct += 10;
                        }
                    }
                }
            }
        }

        /* Lógica de fin de transmisión (Check Last Frame) */
        if (!checksum_sent && expected_frames > 0 && frame_id == expected_frames - 1) {
            
            /* Contar paquetes perdidos solo para informar */
            int missing_total = 0;
            if (frame_received) {
                for (uint32_t i = 0; i < expected_frames; ++i) {
                    if (!frame_received[i]) missing_total++;
                }
            }

            if (missing_total > 0) {
                logger_warn("robust RX: missing %d frames before checksum", missing_total);
            } else {
                logger_info("robust RX: all frames received, calculating checksum...");
            }

            uint64_t checksum_state;
            checksum_init(&checksum_state);
            checksum_update(&checksum_state, compressed, compressed_len);
            uint64_t rx_checksum = checksum_final(checksum_state);

            if (ensure_mode_tx(&radio) != 0) goto cleanup;
            
            /* FIX: Flush antes de enviar checksum */
            flush_radio_buffers(&radio);
            
            if (send_checksum_with_timeout(&radio,
                                           rx_checksum,
                                           &rf_tx_bytes,
                                           &rf_tx_frames) != 0) {
                logger_warn("robust RX: checksum send timed out");
                if (ensure_mode_rx(&radio) != 0) goto cleanup;
                continue;
            }

            checksum_sent = 1;
            if (ensure_mode_rx(&radio) != 0) goto cleanup;
        }
    }

    if (compressed_len != 0 || original_len == 0) {
        uint8_t *output = NULL;
        if (decompress_buffer(compressed, compressed_len, &output, original_len) != 0) {
            goto cleanup;
        }
        app_store_file_bytes(cfg ? cfg->file_path_rx : NULL, output, original_len);
        free(output);
    }

    if (rx_start > 0.0) {
        double elapsed = now_seconds() - rx_start;
        if (elapsed <= 0.0) elapsed = 1e-9;
        logger_info("robust RX throughput: %.2f KiB/s", ((double)original_len / 1024.0) / elapsed);
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

    logger_init(log_path);
    if (cfg.print_config) app_print_config(&cfg);

    const char *spi_dev = get_spi_device_path();
    update_radio_params_from_config(&cfg);

    if (cfg.mode == APP_MODE_TX) {
        uint8_t *data = NULL;
        size_t   len  = 0;
        if (app_load_file_bytes(cfg.file_path_tx, &data, &len) != 0) return 1;
        int ret = run_tx(spi_dev, &cfg, data, len);
        free(data);
        logger_close();
        return ret;
    }

    if (cfg.mode == APP_MODE_RX) {
        int ret = run_rx(spi_dev, &cfg);
        logger_close();
        return ret;
    }

    return 1;
}