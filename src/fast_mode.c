#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#include "nrf24.h"

#define FAST_MODE_CHANNEL       76

/* Header: "FMF1" + total_bytes (u64) + chunk_size (u16, LE) */
#define FM_HEADER_MAGIC         "FMF1"
#define FM_HEADER_MAGIC_LEN     4
#define FM_HEADER_TOTAL_LEN     (FM_HEADER_MAGIC_LEN + 8 + 2)  /* 4 + 8 + 2 */

/* Per-packet data header: 16-bit sequence number (LE) */
#define FM_DATA_HDR_BYTES       2
#define FM_MAX_CHUNK_DATA       (NRF24_MAX_PAYLOAD_SIZE - FM_DATA_HDR_BYTES)

/* ---------- time helper ---------- */

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---------- endian helpers ---------- */

static void encode_u64_le(uint8_t *dst, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        dst[i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
}

static uint64_t decode_u64_le(const uint8_t *src)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value <<= 8;
        value |= src[i];
    }
    return value;
}

static void encode_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t decode_u16_le(const uint8_t *src)
{
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

/* ---------- CLI helper ---------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s tx <spi_device> <ce_bcm_gpio> <input_file>\n"
            "  %s rx <spi_device> <ce_bcm_gpio> <output_file>\n"
            "Example:\n"
            "  %s tx /dev/spidev0.0 22 input.bin\n"
            "  %s rx /dev/spidev0.0 22 output.bin\n",
            prog, prog, prog, prog);
}

/* ---------- simple retry wrapper (same idea as quick_mode) ---------- */

static int send_with_retries(nrf24_t *radio,
                             const uint8_t *buf, uint8_t len,
                             unsigned int hw_timeout_ms,
                             unsigned int max_tries)
{
    unsigned int attempt = 0;

    for (;;) {
        int ret = nrf24_send_blocking(radio, buf, len, hw_timeout_ms);
        if (ret == 0) {
            return 0;   /* got ACK */
        }

        if (errno != ETIMEDOUT) {
            perror("nrf24_send_blocking");
            return -1;
        }

        attempt++;
        if (attempt == 1 || (attempt % 50) == 0) {
            fprintf(stderr,
                    "TX: timeout (no ACK) on attempt %u for %u-byte frame\n",
                    attempt, (unsigned)len);
        }
        fflush(stderr);

        /* Optional: reconfigure radio after many timeouts */
        if (attempt % 200 == 0) {
            fprintf(stderr,
                    "TX: %u consecutive timeouts, reconfiguring radio...\n",
                    attempt);
            (void)nrf24_configure_quick(radio, FAST_MODE_CHANNEL);
            (void)nrf24_set_mode_tx(radio);
        }

        if (max_tries && attempt >= max_tries) {
            errno = ETIMEDOUT;
            return -1;
        }

        /* else: loop and re-send the same frame */
    }
}

/* ---------- TX side: header + sequenced chunks ---------- */

static int run_tx(const char *spi_dev, int ce_bcm, const char *input_path)
{
    nrf24_t radio;
    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)ce_bcm
    };

    if (nrf24_init(&radio, &cfg) < 0) {
        perror("nrf24_init");
        return 1;
    }

    if (nrf24_configure_quick(&radio, FAST_MODE_CHANNEL) < 0) {
        perror("nrf24_configure_quick");
        nrf24_deinit(&radio);
        return 1;
    }

    if (nrf24_set_mode_tx(&radio) < 0) {
        perror("nrf24_set_mode_tx");
        nrf24_deinit(&radio);
        return 1;
    }

    /* Get file size */
    struct stat st;
    if (stat(input_path, &st) < 0) {
        perror("stat(input_file)");
        nrf24_deinit(&radio);
        return 1;
    }

    uint64_t total_bytes = (uint64_t)st.st_size;
    uint16_t chunk_size  = FM_MAX_CHUNK_DATA;

    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        perror("fopen(input_file)");
        nrf24_deinit(&radio);
        return 1;
    }

    printf("FAST TX: sending file '%s' (%llu bytes) with chunk size %u\n",
           input_path,
           (unsigned long long)total_bytes,
           (unsigned)chunk_size);

    /* Build header: "FMF1" + total_bytes + chunk_size */
    uint8_t header[FM_HEADER_TOTAL_LEN];
    memcpy(header, FM_HEADER_MAGIC, FM_HEADER_MAGIC_LEN);
    encode_u64_le(header + FM_HEADER_MAGIC_LEN, total_bytes);
    encode_u16_le(header + FM_HEADER_MAGIC_LEN + 8, chunk_size);

    if (send_with_retries(&radio,
                          header,
                          (uint8_t)FM_HEADER_TOTAL_LEN,
                          50,
                          0) < 0) {
        fprintf(stderr, "FAST TX: failed to send header (fatal)\n");
        fclose(fin);
        nrf24_deinit(&radio);
        return 1;
    }

    printf("FAST TX: header sent. Starting data transfer...\n");

    uint8_t frame[NRF24_MAX_PAYLOAD_SIZE];
    uint64_t sent = 0;
    uint16_t seq  = 0;
    double t_start = now_seconds();

    while (sent < total_bytes) {
        size_t to_read = chunk_size;
        if (total_bytes - sent < to_read)
            to_read = (size_t)(total_bytes - sent);

        size_t n = fread(frame + FM_DATA_HDR_BYTES, 1, to_read, fin);
        if (n == 0) {
            if (ferror(fin)) {
                perror("fread");
                fclose(fin);
                nrf24_deinit(&radio);
                return 1;
            }
            fprintf(stderr,
                    "FAST TX: unexpected EOF (sent=%llu, total=%llu)\n",
                    (unsigned long long)sent,
                    (unsigned long long)total_bytes);
            break;
        }

        /* Insert sequence header (LE) */
        encode_u16_le(frame, seq);

        uint8_t payload_len = (uint8_t)(FM_DATA_HDR_BYTES + n);

        if (send_with_retries(&radio,
                              frame,
                              payload_len,
                              20,
                              0) < 0) {
            fprintf(stderr, "FAST TX: fatal error sending data frame\n");
            fclose(fin);
            nrf24_deinit(&radio);
            return 1;
        }

        sent += n;
        seq++;

        if ((sent % (64 * 1024)) < FM_MAX_CHUNK_DATA) {
            printf("FAST TX: %llu / %llu bytes (%.1f%%)\n",
                   (unsigned long long)sent,
                   (unsigned long long)total_bytes,
                   (total_bytes ? (100.0 * (double)sent / (double)total_bytes) : 100.0));
            fflush(stdout);
        }
    }

    double t_end = now_seconds();
    double dt = t_end - t_start;
    double kibps = (dt > 0.0) ? ((double)sent / 1024.0 / dt) : 0.0;

    printf("FAST TX: done. Sent %llu bytes in %.3f s (%.1f KiB/s)\n",
           (unsigned long long)sent, dt, kibps);

    fclose(fin);
    nrf24_deinit(&radio);
    return 0;
}

/* ---------- RX side: header + sequenced chunks ---------- */

static int run_rx(const char *spi_dev, int ce_bcm, const char *output_path)
{
    nrf24_t radio;
    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)ce_bcm
    };

    if (nrf24_init(&radio, &cfg) < 0) {
        perror("nrf24_init");
        return 1;
    }

    if (nrf24_configure_quick(&radio, FAST_MODE_CHANNEL) < 0) {
        perror("nrf24_configure_quick");
        nrf24_deinit(&radio);
        return 1;
    }

    if (nrf24_set_mode_rx(&radio) < 0) {
        perror("nrf24_set_mode_rx");
        nrf24_deinit(&radio);
        return 1;
    }

    printf("FAST RX: waiting for header on channel %d...\n", FAST_MODE_CHANNEL);

    uint8_t hdr_buf[NRF24_MAX_PAYLOAD_SIZE];
    uint8_t hdr_len = NRF24_MAX_PAYLOAD_SIZE;
    uint64_t total_bytes = 0;
    uint16_t chunk_size  = 0;

    /* Wait for a valid FM header */
    for (;;) {
        hdr_len = NRF24_MAX_PAYLOAD_SIZE;

        if (nrf24_recv_blocking(&radio, hdr_buf, &hdr_len, 0) < 0) {
            perror("FAST RX: nrf24_recv_blocking(header)");
            nrf24_deinit(&radio);
            return 1;
        }

        if (hdr_len >= FM_HEADER_TOTAL_LEN &&
            memcmp(hdr_buf, FM_HEADER_MAGIC, FM_HEADER_MAGIC_LEN) == 0) {
            total_bytes = decode_u64_le(hdr_buf + FM_HEADER_MAGIC_LEN);
            chunk_size  = decode_u16_le(hdr_buf + FM_HEADER_MAGIC_LEN + 8);
            if (chunk_size == 0 || chunk_size > FM_MAX_CHUNK_DATA)
                chunk_size = FM_MAX_CHUNK_DATA;

            printf("FAST RX: got header. Expecting %llu bytes, chunk size %u\n",
                   (unsigned long long)total_bytes,
                   (unsigned)chunk_size);
            break;
        } else {
            fprintf(stderr,
                    "FAST RX: received non-header packet (len=%u), ignoring\n",
                    hdr_len);
        }
    }

    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        perror("fopen(output_file)");
        nrf24_deinit(&radio);
        return 1;
    }

    printf("FAST RX: receiving file into '%s'...\n", output_path);

    uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
    uint64_t received = 0;
    uint16_t expected_seq = 0;
    double t_start = now_seconds();

    while (received < total_bytes) {
        uint8_t len = NRF24_MAX_PAYLOAD_SIZE;
        int ret = nrf24_recv_blocking(&radio, buf, &len, 1000);

        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                fprintf(stderr,
                        "FAST RX: timeout waiting for data (received=%llu / %llu)\n",
                        (unsigned long long)received,
                        (unsigned long long)total_bytes);
                continue;
            } else {
                perror("FAST RX: nrf24_recv_blocking(data)");
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }
        }

        if (len <= FM_DATA_HDR_BYTES) {
            /* malformed or empty, ignore */
            continue;
        }

        uint16_t seq = decode_u16_le(buf);
        size_t data_len = (size_t)(len - FM_DATA_HDR_BYTES);
        uint8_t *data = buf + FM_DATA_HDR_BYTES;

        if (seq == expected_seq) {
            size_t to_write = data_len;
            if (received + to_write > total_bytes)
                to_write = (size_t)(total_bytes - received);

            size_t w = fwrite(data, 1, to_write, fout);
            if (w != to_write) {
                perror("fwrite");
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            received += w;
            expected_seq++;

            if ((received % (64 * 1024)) < FM_MAX_CHUNK_DATA) {
                printf("FAST RX: %llu / %llu bytes (%.1f%%)\n",
                       (unsigned long long)received,
                       (unsigned long long)total_bytes,
                       (total_bytes ? (100.0 * (double)received / (double)total_bytes) : 100.0));
                fflush(stdout);
            }
        } else if (seq < expected_seq) {
            /* duplicate due to retries -> ignore silently */
            // printf("FAST RX: duplicate seq=%u (expected %u), dropping\n",
            //        seq, expected_seq);
        } else { /* seq > expected_seq */
            /* Out-of-order / missing chunk (shouldn't happen in stop-and-wait) */
            fprintf(stderr,
                    "FAST RX: out-of-order seq=%u (expected %u), dropping\n",
                    seq, expected_seq);
            /* We drop it; TX will retry because it didn't get our HW ACK. */
        }
    }

    double t_end = now_seconds();
    double dt = t_end - t_start;
    double kibps = (dt > 0.0) ? ((double)received / 1024.0 / dt) : 0.0;

    printf("FAST RX: done. Received %llu bytes in %.3f s (%.1f KiB/s)\n",
           (unsigned long long)received, dt, kibps);

    fclose(fout);
    nrf24_deinit(&radio);
    return 0;
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
    if (argc != 5) {
        usage(argv[0]);
        return 1;
    }

    const char *mode    = argv[1];
    const char *spi_dev = argv[2];
    int ce_bcm          = atoi(argv[3]);
    const char *path    = argv[4];

    if (ce_bcm < 0 || ce_bcm > 255) {
        fprintf(stderr, "Invalid CE GPIO: %d\n", ce_bcm);
        return 1;
    }

    if (strcmp(mode, "tx") == 0) {
        return run_tx(spi_dev, ce_bcm, path);
    } else if (strcmp(mode, "rx") == 0) {
        return run_rx(spi_dev, ce_bcm, path);
    } else {
        usage(argv[0]);
        return 1;
    }
}
