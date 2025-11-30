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

#define ROBUST_CHANNEL      90
#define MAX_PAYLOAD         32
#define CONTROL_PREFIX      0xFF
#define DATA_PREFIX         0x00

#define MSG_STREAM_INFO     0x01
#define MSG_STREAM_FINISH   0x02
#define MSG_CHECKSUM        0x03
#define MSG_STREAM_READY    0x04

#define CHECKSUM_SIZE       8
#define CHECKSUM_SEND_WINDOW_MS 500
#define READY_TIMEOUT_MS    2000
#define CONTROL_TIMEOUT_MS  100
#define DATA_TIMEOUT_MS     20
#define CHECKSUM_TIMEOUT_MS 1000

#define STREAM_INFO_SIZE    16
#define STREAM_READY_SIZE   11

#define FNV64_OFFSET_BASIS  1469598103934665603ULL
#define FNV64_PRIME         1099511628211ULL

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

static int read_file_bytes(const char *path, uint8_t **out_buf, size_t *out_len)
{
    if (!path || !out_buf || !out_len) {
        logger_error("read_file_bytes: invalid arguments");
        return -1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        logger_error("Failed to open '%s': %s", path, strerror(errno));
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        logger_error("fseek failed on '%s'", path);
        fclose(f);
        return -1;
    }

    long sz = ftell(f);
    if (sz < 0) sz = 0;
    rewind(f);

    uint8_t *buf = NULL;
    size_t len = (size_t)sz;
    if (len > 0) {
        buf = (uint8_t *)malloc(len);
        if (!buf) {
            logger_error("malloc(%zu) failed", len);
            fclose(f);
            return -1;
        }
        if (fread(buf, 1, len, f) != len) {
            logger_error("fread failed for '%s'", path);
            free(buf);
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    *out_buf = buf;
    *out_len = len;
    return 0;
}

static int write_file_bytes(const char *path, const uint8_t *data, size_t len)
{
    if (!path) {
        logger_error("write_file_bytes: invalid path");
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        logger_error("Failed to open '%s' for writing: %s", path, strerror(errno));
        return -1;
    }

    if (len > 0 && fwrite(data, 1, len, f) != len) {
        logger_error("fwrite failed for '%s'", path);
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
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
                             const char *label)
{
    if (!radio || !buf || len == 0) {
        logger_error("send_with_retries(%s): invalid args", label ? label : "frame");
        return -1;
    }

    unsigned attempt = 0;
    while (1) {
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
            (void)nrf24_configure_quick(radio, ROBUST_CHANNEL);
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
                            uint32_t orig_len)
{
    uint8_t msg[STREAM_INFO_SIZE] = {0};
    msg[0] = CONTROL_PREFIX;
    msg[1] = MSG_STREAM_INFO;
    msg[2] = id_bytes;
    msg[3] = 0;
    encode_u32_le(&msg[4], comp_len);
    encode_u32_le(&msg[8], total_frames);
    encode_u32_le(&msg[12], orig_len);
    return send_with_retries(radio, msg, sizeof(msg), CONTROL_TIMEOUT_MS, "STREAM_INFO");
}

static int send_stream_finish(nrf24_t *radio)
{
    uint8_t msg[2] = { CONTROL_PREFIX, MSG_STREAM_FINISH };
    return send_with_retries(radio, msg, sizeof(msg), CONTROL_TIMEOUT_MS, "STREAM_FINISH");
}

static int send_checksum_with_timeout(nrf24_t *radio, uint64_t checksum)
{
    uint8_t msg[2 + CHECKSUM_SIZE];
    msg[0] = CONTROL_PREFIX;
    msg[1] = MSG_CHECKSUM;
    encode_u64_le(&msg[2], checksum);

    double start = now_seconds();
    unsigned attempt = 0;

    while ((now_seconds() - start) * 1000.0 < CHECKSUM_SEND_WINDOW_MS) {
        int ret = nrf24_send_blocking(radio,
                                      msg,
                                      sizeof(msg),
                                      CONTROL_TIMEOUT_MS);
        if (ret == 0) {
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
            (void)nrf24_configure_quick(radio, ROBUST_CHANNEL);
            if (ensure_mode_tx(radio) != 0) {
                return -1;
            }
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

    logger_error("robust TX: timeout waiting for STREAM_READY");
    return -1;
}

static int send_stream_ready(nrf24_t *radio,
                             uint8_t id_bytes,
                             uint32_t expected_frames,
                             uint32_t compressed_len)
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

    if (send_with_retries(radio, msg, sizeof(msg), CONTROL_TIMEOUT_MS, "STREAM_READY") != 0) {
        logger_error("robust RX: failed to send STREAM_READY");
        return -1;
    }

    logger_info("robust RX: sent STREAM_READY (frames=%u, comp=%u)",
                expected_frames,
                compressed_len);

    if (ensure_mode_rx(radio) != 0) {
        return -1;
    }
    return 0;
}

static int run_tx(const char *spi_dev, int ce_gpio, const char *input_path)
{
    uint8_t *file_data = NULL;
    size_t file_len = 0;
    uint8_t *compressed = NULL;
    size_t compressed_len = 0;
    int exit_code = 1;

    if (read_file_bytes(input_path, &file_data, &file_len) != 0) {
        goto cleanup;
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

    logger_info("robust TX: '%s' -> %zu bytes compressed (%u frames, %u ID bytes)",
                input_path,
                compressed_len,
                (unsigned)total_frames,
                id_bytes);

    nrf24_t radio;
    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)ce_gpio
    };

    if (nrf24_init(&radio, &cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        goto cleanup;
    }
    if (nrf24_configure_quick(&radio, ROBUST_CHANNEL) < 0) {
        logger_error("nrf24_configure_quick failed");
        nrf24_deinit(&radio);
        goto cleanup;
    }
    if (ensure_mode_tx(&radio) != 0) {
        nrf24_deinit(&radio);
        goto cleanup;
    }

    if (send_stream_info(&radio,
                         (uint8_t)id_bytes,
                         (uint32_t)compressed_len,
                         total_frames,
                         (uint32_t)file_len) != 0) {
        logger_error("Failed to send STREAM_INFO");
        nrf24_deinit(&radio);
        goto cleanup;
    }

    if (wait_for_stream_ready(&radio,
                              (uint8_t)id_bytes,
                              total_frames,
                              (uint32_t)compressed_len) != 0) {
        logger_error("robust TX: RX failed to signal readiness");
        nrf24_deinit(&radio);
        goto cleanup;
    }

    uint64_t checksum_state;
    checksum_init(&checksum_state);
    checksum_update(&checksum_state, compressed, compressed_len);
    uint64_t tx_checksum = checksum_final(checksum_state);

    int transfer_complete = 0;
    unsigned resend_round = 0;

    while (!transfer_complete) {
        logger_info("robust TX: sending data (round %u)", resend_round + 1);
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
                                  "DATA") != 0) {
                logger_error("Failed to send DATA frame %u", frame);
                nrf24_deinit(&radio);
                goto cleanup;
            }

            offset += chunk;
        }

        if (ensure_mode_rx(&radio) != 0) {
            nrf24_deinit(&radio);
            goto cleanup;
        }

        logger_info("robust TX: waiting for checksum reply");
        int checksum_ok = 0;
        double wait_start = now_seconds();
        while (!checksum_ok && (now_seconds() - wait_start) * 1000.0 < CHECKSUM_TIMEOUT_MS * 10) {
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

        if (send_stream_finish(&radio) != 0) {
            logger_warn("robust TX: failed to send STREAM_FINISH");
        }

        transfer_complete = 1;
    }

    logger_succ("robust TX: transfer complete (%zu bytes -> %zu bytes)",
                file_len,
                compressed_len);
    nrf24_deinit(&radio);
    exit_code = 0;

cleanup:
    free(file_data);
    free(compressed);
    return exit_code;
}

static int run_rx(const char *spi_dev, int ce_gpio, const char *output_path)
{
    nrf24_t radio;
    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)ce_gpio
    };

    if (nrf24_init(&radio, &cfg) < 0) {
        logger_error("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    if (nrf24_configure_quick(&radio, ROBUST_CHANNEL) < 0) {
        logger_error("nrf24_configure_quick failed");
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

                id_bytes = buf[2];
                compressed_len = decode_u32_le(&buf[4]);
                expected_frames = decode_u32_le(&buf[8]);
                original_len = decode_u32_le(&buf[12]);

                if (id_bytes == 0 || id_bytes > 4) {
                    logger_error("robust RX: invalid FrameID length %u", id_bytes);
                    goto cleanup;
                }

                payload_bytes = MAX_PAYLOAD - 1 - id_bytes;
                if (payload_bytes == 0) {
                    logger_error("robust RX: payload too small for FrameIDs");
                    goto cleanup;
                }

                if (compressed_len > UINT32_MAX) {
                    logger_error("robust RX: compressed length too large");
                    goto cleanup;
                }

                if (expected_frames == 0 && compressed_len > 0) {
                    expected_frames = (uint32_t)((compressed_len + payload_bytes - 1) / payload_bytes);
                }

                if (compressed_len > compressed_cap) {
                    uint8_t *tmp = (uint8_t *)realloc(compressed, compressed_len);
                    if (!tmp && compressed_len > 0) {
                        logger_error("robust RX: realloc(%zu) failed", compressed_len);
                        goto cleanup;
                    }
                    compressed = tmp;
                    compressed_cap = compressed_len;
                }

                free(frame_received);
                frame_received = NULL;
                if (expected_frames > 0) {
                    frame_received = (uint8_t *)calloc(expected_frames, 1);
                    if (!frame_received) {
                        logger_error("robust RX: calloc failed for frame tracking");
                        goto cleanup;
                    }
                }

                if (compressed_len > 0 && compressed) {
                    memset(compressed, 0, compressed_len);
                }
                frames_received = 0;
                checksum_sent = 0;
                have_info = 1;

                logger_info("robust RX: STREAM_INFO -> comp=%zu bytes, orig=%u bytes, frames=%u, id_bytes=%u",
                            compressed_len,
                            original_len,
                            expected_frames,
                            id_bytes);

                if (send_stream_ready(&radio,
                                      id_bytes,
                                      expected_frames,
                                      (uint32_t)compressed_len) != 0) {
                    goto cleanup;
                }
                continue;
            }

            if (type == MSG_STREAM_FINISH) {
                if (!checksum_sent) {
                    logger_warn("robust RX: STREAM_FINISH received before checksum sent");
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
        }

        if (expected_frames == 0) {
            continue;
        }

        if (frames_received >= expected_frames && !checksum_sent) {
            logger_info("robust RX: all %u frames received, sending checksum", expected_frames);

            uint64_t checksum_state;
            checksum_init(&checksum_state);
            checksum_update(&checksum_state, compressed, compressed_len);
            uint64_t rx_checksum = checksum_final(checksum_state);

            if (ensure_mode_tx(&radio) != 0) {
                goto cleanup;
            }
            if (send_checksum_with_timeout(&radio, rx_checksum) != 0) {
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

    if (compressed_len != 0 || original_len == 0) {
        uint8_t *output = NULL;
        if (decompress_buffer(compressed, compressed_len, &output, original_len) != 0) {
            goto cleanup;
        }

        if (write_file_bytes(output_path, output, original_len) != 0) {
            free(output);
            goto cleanup;
        }
        free(output);
        logger_succ("robust RX: stored '%s' (%u bytes)", output_path, original_len);
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

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s tx <spi_dev> <ce_gpio> <input_file>\n"
            "  %s rx <spi_dev> <ce_gpio> <output_file>\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        usage(argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *spi_dev = argv[2];
    int ce_gpio = atoi(argv[3]);
    const char *file_path = argv[4];

    if (ce_gpio < 0 || ce_gpio > 255) {
        logger_error("Invalid CE GPIO: %d", ce_gpio);
        return 1;
    }

    char log_path[32];
    if (strcmp(mode, "tx") == 0) {
        snprintf(log_path, sizeof(log_path), "robust_tx.log");
    } else if (strcmp(mode, "rx") == 0) {
        snprintf(log_path, sizeof(log_path), "robust_rx.log");
    } else {
        usage(argv[0]);
        return 1;
    }

    if (logger_init(log_path) != 0) {
        logger_warn("Could not open log file '%s'", log_path);
    } else {
        logger_info("Logging to file '%s'", log_path);
    }

    int ret;
    if (strcmp(mode, "tx") == 0) {
        ret = run_tx(spi_dev, ce_gpio, file_path);
    } else {
        ret = run_rx(spi_dev, ce_gpio, file_path);
    }

    logger_close();
    return ret;
}
