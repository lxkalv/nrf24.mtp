#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <limits.h>

#include "nrf24.h"
#include "logger.h"
#include "lz4hc.h"

#define MAX_PAYLOAD         32
#define CONTROL_PREFIX      0xA5
#define DATA_PREFIX         0x5A
#define MSG_STREAM_INFO     0x01
#define MSG_STREAM_READY    0x02
#define MSG_STREAM_FINISH   0x03
#define MSG_CHECKSUM        0x04
#define MSG_RESEND_REQUEST  0x05

#define CHECKSUM_SIZE       8
#define STREAM_INFO_SIZE    16

#define DATA_TIMEOUT_MS         200
#define STREAM_INFO_TIMEOUT_MS  500
#define STREAM_READY_TIMEOUT_MS 1000
#define CHECKSUM_TIMEOUT_MS     1000
#define STREAM_FINISH_TIMEOUT_MS 500

#define MAX_RESEND_ROUNDS  10

typedef struct {
    const char *file_path_tx;
    const char *file_path_rx;
    int ce_pin;
    int dry_run;
    int verbose;
    int compress_level;
    int crc_mode;
    int stats_only;
} app_config_t;

typedef enum {
    APP_CRC_NONE = 0,
    APP_CRC_64,
    APP_CRC_32,
    APP_CRC_16,
    APP_CRC_8,
    APP_CRC_1
} app_crc_mode_t;

static uint64_t checksum64(const uint8_t *data, size_t len);
static uint32_t checksum32(const uint8_t *data, size_t len);
static uint16_t checksum16(const uint8_t *data, size_t len);
static uint8_t  checksum8(const uint8_t *data, size_t len);

static uint64_t now_tick(void);
static double now_seconds(void);
static int ensure_mode_rx(nrf24_t *radio);
static int ensure_mode_tx(nrf24_t *radio);

static uint64_t encode_u64_le(uint64_t v, uint8_t *buf);
static uint64_t decode_u64_le(const uint8_t *buf);
static uint32_t encode_u32_le(uint32_t v, uint8_t *buf);
static uint32_t decode_u32_le(const uint8_t *buf);

static int send_with_retries(nrf24_t *radio,
                             const uint8_t *payload,
                             uint8_t len,
                             unsigned timeout_ms,
                             const char *tag,
                             uint64_t *out_rf_bytes,
                             uint64_t *out_rf_frames);

static int send_stream_info(nrf24_t *radio,
                            uint8_t id_bytes,
                            uint32_t compressed_len,
                            uint32_t total_frames,
                            uint32_t orig_len,
                            uint64_t *out_rf_bytes,
                            uint64_t *out_rf_frames);

static int wait_for_stream_ready(nrf24_t *radio,
                                 uint32_t expected_frames,
                                 unsigned timeout_ms);

static void checksum_init(uint64_t *state);
static void checksum_update(uint64_t *state, const uint8_t *data, size_t len);
static uint64_t checksum_final(uint64_t state);

static int run_tx(const char *spi_dev,
                  const app_config_t *cfg,
                  const uint8_t *file_data,
                  size_t file_len);

static int run_rx(const char *spi_dev,
                  const app_config_t *cfg);

static int derive_frame_layout(size_t compressed_len,
                               unsigned *id_bytes,
                               size_t *payload_bytes,
                               uint32_t *total_frames);

static void print_usage(const char *prog);

int main(int argc, char **argv)
{
    const char *spi_dev = "/dev/spidev0.0";
    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ce_pin = 22;
    cfg.compress_level = 9;
    cfg.crc_mode = APP_CRC_64;

    int tx_mode = 0;
    int rx_mode = 0;

    int opt;
    while ((opt = getopt(argc, argv, "t:r:s:c:l:C:vdh")) != -1) {
        switch (opt) {
        case 't':
            cfg.file_path_tx = optarg;
            tx_mode = 1;
            break;
        case 'r':
            cfg.file_path_rx = optarg;
            rx_mode = 1;
            break;
        case 's':
            spi_dev = optarg;
            break;
        case 'c':
            cfg.ce_pin = atoi(optarg);
            break;
        case 'l':
            cfg.compress_level = atoi(optarg);
            if (cfg.compress_level < 0) cfg.compress_level = 0;
            if (cfg.compress_level > 16) cfg.compress_level = 16;
            break;
        case 'C':
            cfg.crc_mode = atoi(optarg);
            if (cfg.crc_mode < APP_CRC_NONE) cfg.crc_mode = APP_CRC_NONE;
            if (cfg.crc_mode > APP_CRC_1) cfg.crc_mode = APP_CRC_1;
            break;
        case 'v':
            cfg.verbose = 1;
            break;
        case 'd':
            cfg.dry_run = 1;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (tx_mode && rx_mode) {
        logger_error("Cannot run in both TX and RX mode");
        return 1;
    }
    if (!tx_mode && !rx_mode) {
        logger_error("Must specify either TX (-t) or RX (-r) mode");
        print_usage(argv[0]);
        return 1;
    }

    if (tx_mode) {
        if (!cfg.file_path_tx) {
            logger_error("TX mode requires a file to send (-t)");
            return 1;
        }

        FILE *f = fopen(cfg.file_path_tx, "rb");
        if (!f) {
            logger_error("Failed to open input file '%s': %s",
                         cfg.file_path_tx,
                         strerror(errno));
            return 1;
        }

        if (fseek(f, 0, SEEK_END) != 0) {
            logger_error("Failed to seek in input file: %s", strerror(errno));
            fclose(f);
            return 1;
        }
        long fsize = ftell(f);
        if (fsize < 0) {
            logger_error("ftell failed on input file: %s", strerror(errno));
            fclose(f);
            return 1;
        }
        if (fseek(f, 0, SEEK_SET) != 0) {
            logger_error("Failed to rewind input file: %s", strerror(errno));
            fclose(f);
            return 1;
        }

        uint8_t *buf = malloc((size_t)fsize);
        if (!buf) {
            logger_error("Failed to allocate %ld bytes for input file", fsize);
            fclose(f);
            return 1;
        }

        size_t read_len = fread(buf, 1, (size_t)fsize, f);
        fclose(f);
        if ((long)read_len != fsize) {
            logger_error("Failed to read entire input file");
            free(buf);
            return 1;
        }

        int ret = run_tx(spi_dev, &cfg, buf, (size_t)fsize);
        free(buf);
        return ret;
    } else {
        return run_rx(spi_dev, &cfg);
    }
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -t input_file [-r output_file] [options]\n"
            "       %s -r output_file [options]\n"
            "\n"
            "Options:\n"
            "  -t file   TX mode: send this file\n"
            "  -r file   RX mode: receive to this file\n"
            "  -s dev    SPI device (default: /dev/spidev0.0)\n"
            "  -c pin    CE GPIO pin (default: 22)\n"
            "  -l level  LZ4HC compression level (0-16, default: 9)\n"
            "  -C mode   checksum mode (0=none,1=64-bit,2=32,3=16,4=8,5=1)\n"
            "  -v        verbose logging\n"
            "  -d        dry-run (no actual radio transmission)\n"
            "  -h        show this help\n",
            prog, prog);
}

static uint64_t now_tick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static double now_seconds(void)
{
    return (double)now_tick() / 1e9;
}

static uint64_t checksum64(const uint8_t *data, size_t len)
{
    uint64_t crc = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint64_t)data[i];
        crc *= 0x100000001b3ull;
    }
    return crc;
}

static uint32_t checksum32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0x811c9dc5u;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)data[i];
        crc *= 0x01000193u;
    }
    return crc;
}

static uint16_t checksum16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1u) {
                crc = (crc >> 1) ^ 0xA001u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static uint8_t checksum8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            if (crc & 1u) {
                crc = (uint8_t)((crc >> 1) ^ 0x8Cu);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static void checksum_init(uint64_t *state)
{
    *state = 0xcbf29ce484222325ull;
}

static void checksum_update(uint64_t *state, const uint8_t *data, size_t len)
{
    uint64_t crc = *state;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint64_t)data[i];
        crc *= 0x100000001b3ull;
    }
    *state = crc;
}

static uint64_t checksum_final(uint64_t state)
{
    return state;
}

static uint64_t encode_u64_le(uint64_t v, uint8_t *buf)
{
    buf[0] = (uint8_t)(v & 0xFFu);
    buf[1] = (uint8_t)((v >> 8) & 0xFFu);
    buf[2] = (uint8_t)((v >> 16) & 0xFFu);
    buf[3] = (uint8_t)((v >> 24) & 0xFFu);
    buf[4] = (uint8_t)((v >> 32) & 0xFFu);
    buf[5] = (uint8_t)((v >> 40) & 0xFFu);
    buf[6] = (uint8_t)((v >> 48) & 0xFFu);
    buf[7] = (uint8_t)((v >> 56) & 0xFFu);
    return v;
}

static uint64_t decode_u64_le(const uint8_t *buf)
{
    uint64_t v = 0;
    v |= (uint64_t)buf[0];
    v |= (uint64_t)buf[1] << 8;
    v |= (uint64_t)buf[2] << 16;
    v |= (uint64_t)buf[3] << 24;
    v |= (uint64_t)buf[4] << 32;
    v |= (uint64_t)buf[5] << 40;
    v |= (uint64_t)buf[6] << 48;
    v |= (uint64_t)buf[7] << 56;
    return v;
}

static uint32_t encode_u32_le(uint32_t v, uint8_t *buf)
{
    buf[0] = (uint8_t)(v & 0xFFu);
    buf[1] = (uint8_t)((v >> 8) & 0xFFu);
    buf[2] = (uint8_t)((v >> 16) & 0xFFu);
    buf[3] = (uint8_t)((v >> 24) & 0xFFu);
    return v;
}

static uint32_t decode_u32_le(const uint8_t *buf)
{
    uint32_t v = 0;
    v |= (uint32_t)buf[0];
    v |= (uint32_t)buf[1] << 8;
    v |= (uint32_t)buf[2] << 16;
    v |= (uint32_t)buf[3] << 24;
    return v;
}

static int ensure_mode_rx(nrf24_t *radio)
{
    if (nrf24_set_mode(radio, NRF24_MODE_RX) < 0) {
        logger_error("Failed to set RX mode: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int ensure_mode_tx(nrf24_t *radio)
{
    if (nrf24_set_mode(radio, NRF24_MODE_TX) < 0) {
        logger_error("Failed to set TX mode: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int send_with_retries(nrf24_t *radio,
                             const uint8_t *payload,
                             uint8_t len,
                             unsigned timeout_ms,
                             const char *tag,
                             uint64_t *out_rf_bytes,
                             uint64_t *out_rf_frames)
{
    const int max_retries = 3;
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        if (nrf24_send_blocking(radio, payload, len, timeout_ms) == 0) {
            if (out_rf_bytes) {
                *out_rf_bytes += len;
            }
            if (out_rf_frames) {
                *out_rf_frames += 1;
            }
            return 0;
        }
        if (errno != ETIMEDOUT) {
            logger_error("%s: nrf24_send_blocking failed: %s", tag, strerror(errno));
            return -1;
        }
        logger_warn("%s: timeout on attempt %d, retrying", tag, attempt + 1);
    }
    logger_error("%s: failed after %d attempts", tag, max_retries);
    return -1;
}

static int send_stream_info(nrf24_t *radio,
                            uint8_t id_bytes,
                            uint32_t compressed_len,
                            uint32_t total_frames,
                            uint32_t orig_len,
                            uint64_t *out_rf_bytes,
                            uint64_t *out_rf_frames)
{
    uint8_t payload[MAX_PAYLOAD];
    payload[0] = CONTROL_PREFIX;
    payload[1] = MSG_STREAM_INFO;

    encode_u32_le((uint32_t)compressed_len, &payload[2]);
    encode_u32_le((uint32_t)orig_len, &payload[6]);
    payload[10] = id_bytes;
    encode_u32_le(total_frames, &payload[11]);

    uint8_t len = 15;
    return send_with_retries(radio,
                             payload,
                             len,
                             STREAM_INFO_TIMEOUT_MS,
                             "STREAM_INFO",
                             out_rf_bytes,
                             out_rf_frames);
}

static int wait_for_stream_ready(nrf24_t *radio,
                                 uint32_t expected_frames,
                                 unsigned timeout_ms)
{
    uint8_t buf[MAX_PAYLOAD];
    uint8_t len;
    double start = now_seconds();
    while ((now_seconds() - start) * 1000.0 < timeout_ms) {
        len = sizeof(buf);
        int ret = nrf24_recv_blocking(radio, buf, &len, timeout_ms);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            logger_error("wait_for_stream_ready: nrf24_recv_blocking failed: %s",
                         strerror(errno));
            return -1;
        }
        if (len < 2) {
            continue;
        }
        if (buf[0] != CONTROL_PREFIX) {
            continue;
        }
        if (buf[1] == MSG_STREAM_READY) {
            if (len < 6) {
                logger_warn("STREAM_READY frame too short");
                continue;
            }
            uint32_t rx_frames = decode_u32_le(&buf[2]);
            if (rx_frames != expected_frames) {
                logger_warn("STREAM_READY: frame count mismatch (rx=%u, tx=%u)",
                            rx_frames,
                            expected_frames);
                continue;
            }
            return 0;
        }
    }
    return 1;
}

static int derive_frame_layout(size_t compressed_len,
                               unsigned *id_bytes,
                               size_t *payload_bytes,
                               uint32_t *total_frames)
{
    if (!id_bytes || !payload_bytes || !total_frames) {
        return -1;
    }

    uint32_t frames_16 = (uint32_t)((compressed_len + (MAX_PAYLOAD - 3) - 1) / (MAX_PAYLOAD - 3));
    uint32_t frames_24 = (uint32_t)((compressed_len + (MAX_PAYLOAD - 4) - 1) / (MAX_PAYLOAD - 4));
    uint32_t frames_32 = (uint32_t)((compressed_len + (MAX_PAYLOAD - 5) - 1) / (MAX_PAYLOAD - 5));

    if (frames_16 <= 0xFFFFu) {
        *id_bytes = 2;
        *payload_bytes = MAX_PAYLOAD - 3;
        *total_frames = frames_16;
    } else if (frames_24 <= 0xFFFFFFu) {
        *id_bytes = 3;
        *payload_bytes = MAX_PAYLOAD - 4;
        *total_frames = frames_24;
    } else {
        *id_bytes = 4;
        *payload_bytes = MAX_PAYLOAD - 5;
        *total_frames = frames_32;
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
    if (!file_data && file_len > 0) {
        logger_error("run_tx: input buffer is NULL but length > 0");
        return 1;
    }

    if (file_len == 0) {
        logger_warn("Input file is empty, nothing to send");
        return 0;
    }

    if (file_len > UINT32_MAX) {
        logger_error("File too large for robust_mode (limit 4 GiB per stage)");
        return 1;
    }

    int max_dst_size = LZ4_compressBound((int)file_len);
    compressed = malloc((size_t)max_dst_size);
    if (!compressed) {
        logger_error("Failed to allocate %d bytes for compression", max_dst_size);
        return 1;
    }

    int compressed_size = LZ4_compress_HC((const char *)file_data,
                                          (char *)compressed,
                                          (int)file_len,
                                          max_dst_size,
                                          (cfg ? cfg->compress_level : 9));
    if (compressed_size <= 0) {
        logger_error("LZ4_compress_HC failed");
        goto cleanup;
    }
    compressed_len = (size_t)compressed_size;

    unsigned id_bytes = 0;
    size_t payload_bytes = 0;
    uint32_t total_frames = 0;
    if (derive_frame_layout(compressed_len, &id_bytes, &payload_bytes, &total_frames) != 0) {
        goto cleanup;
    }

    const char *input_label = (cfg && cfg->file_path_tx && cfg->file_path_tx[0])
                            ? cfg->file_path_tx
                            : "(memory buffer)";

    logger_info("robust TX: '%s' -> %zu bytes compressed (%u frames, %u ID bytes)",
                input_label,
                compressed_len,
                total_frames,
                id_bytes);

    if (cfg && cfg->dry_run) {
        logger_info("Dry-run mode: not actually sending anything over radio");
        exit_code = 0;
        goto cleanup;
    }

    nrf24_t radio;
    memset(&radio, 0, sizeof(radio));

    int ce_pin = (cfg && cfg->ce_pin >= 0 && cfg->ce_pin <= 255)
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

    int ready_ret = wait_for_stream_ready(&radio, total_frames, STREAM_READY_TIMEOUT_MS);
    if (ready_ret != 0) {
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

    uint64_t checksum_state;
    checksum_init(&checksum_state);
    checksum_update(&checksum_state, compressed, compressed_len);
    uint64_t tx_checksum = checksum_final(checksum_state);

    int transfer_complete = 0;
    unsigned resend_round = 0;

    while (!transfer_complete) {
        unsigned next_tx_progress_pct = 10;

        logger_info("TX robusto: enviando datos (ronda %u)", resend_round + 1);
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

            if (total_frames > 0) {
                uint32_t pct = (uint32_t)(((uint64_t)(frame + 1) * 100u) / total_frames);
                if (frame + 1 == total_frames) {
                    pct = 100;
                }
                if (pct >= next_tx_progress_pct) {
                    logger_info("TX robusto: ronda %u progreso %u%% (%u/%u tramas)",
                                resend_round + 1,
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
            } else if (buf[1] == MSG_STREAM_READY) {
                logger_warn("robust TX: RX resent STREAM_INFO ack; ignoring");
            } else if (buf[1] == MSG_STREAM_FINISH) {
                logger_info("robust TX: RX already finished transfer");
                checksum_ok = 1;
                break;
            } else {
                logger_warn("robust TX: unexpected control message 0x%02X", buf[1]);
            }
        }

        if (!checksum_ok) {
            logger_warn("robust TX: checksum not confirmed, resending data");
            if (++resend_round >= MAX_RESEND_ROUNDS) {
                logger_error("robust TX: maximum resend rounds reached");
                nrf24_deinit(&radio);
                goto cleanup;
            }
            if (ensure_mode_tx(&radio) != 0) {
                nrf24_deinit(&radio);
                goto cleanup;
            }
            continue;
        }

        if (ensure_mode_tx(&radio) != 0) {
            nrf24_deinit(&radio);
            goto cleanup;
        }

        uint8_t finish_payload[2];
        finish_payload[0] = CONTROL_PREFIX;
        finish_payload[1] = MSG_STREAM_FINISH;
        if (send_with_retries(&radio,
                              finish_payload,
                              2,
                              STREAM_FINISH_TIMEOUT_MS,
                              "STREAM_FINISH",
                              &tx_rf_bytes,
                              &tx_rf_frames) != 0) {
            logger_warn("robust TX: failed to send STREAM_FINISH");
        }

        transfer_complete = 1;
    }

    if (tx_start > 0.0) {
        double tx_end = now_seconds();
        double elapsed = tx_end - tx_start;
        double user_rate = (elapsed > 0.0) ? ((double)file_len / 1024.0 / elapsed) : 0.0;
        double rf_rate = (elapsed > 0.0) ? ((double)tx_rf_bytes / 1024.0 / elapsed) : 0.0;
        logger_info("robust TX throughput: user=%.2f KiB/s (%zu bytes in %.2fs), rf=%.2f KiB/s (%llu bytes, %llu frames)",
                    user_rate,
                    file_len,
                    elapsed,
                    rf_rate,
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

static int run_rx(const char *spi_dev,
                  const app_config_t *cfg)
{
    (void)spi_dev;
    (void)cfg;
    /* RX implementation omitted for brevity in this example */
    logger_info("run_rx: not implemented in this snippet");
    return 0;
}

/* ... resto de funciones auxiliares si las hubiera ... */
