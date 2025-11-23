#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#include "nrf24.h"

#define QM_HEADER_MAGIC      "QMF1"
#define QM_HEADER_MAGIC_LEN  4
#define QM_HEADER_TOTAL_LEN  (QM_HEADER_MAGIC_LEN + 8)  /* 4 magic + 8 size */

/* Little-endian encode/decode of uint64_t */
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

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---------- TX path: send header + file ---------- */

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

    if (nrf24_configure_quick(&radio, 76) < 0) {
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

    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        perror("fopen(input_file)");
        nrf24_deinit(&radio);
        return 1;
    }

    printf("TX: sending file '%s' (%llu bytes)\n",
           input_path, (unsigned long long)total_bytes);

    /* Build and send header packet */
    uint8_t header[QM_HEADER_TOTAL_LEN];
    memcpy(header, QM_HEADER_MAGIC, QM_HEADER_MAGIC_LEN);
    encode_u64_le(header + QM_HEADER_MAGIC_LEN, total_bytes);

    if (nrf24_send_blocking(&radio,
                            header,
                            (uint8_t)QM_HEADER_TOTAL_LEN,
                            1000) < 0) {
        if (errno == ETIMEDOUT) {
            fprintf(stderr,
                    "TX: header send timed out (no ACK)\n");
        } else {
            perror("TX: nrf24_send_blocking(header)");
        }
        fclose(fin);
        nrf24_deinit(&radio);
        return 1;
    }

    printf("TX: header sent. Starting data transfer...\n");

    uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
    uint64_t sent = 0;
    double t_start = now_seconds();

    while (sent < total_bytes) {
        size_t to_read = NRF24_MAX_PAYLOAD_SIZE;
        if (total_bytes - sent < to_read)
            to_read = (size_t)(total_bytes - sent);

        size_t n = fread(buf, 1, to_read, fin);
        if (n == 0) {
            if (ferror(fin)) {
                perror("fread");
                fclose(fin);
                nrf24_deinit(&radio);
                return 1;
            }
            /* EOF earlier than expected -> inconsistent; break */
            fprintf(stderr,
                    "TX: unexpected EOF (sent=%llu, total=%llu)\n",
                    (unsigned long long)sent,
                    (unsigned long long)total_bytes);
            break;
        }

        if (nrf24_send_blocking(&radio, buf, (uint8_t)n, 500) < 0) {
            if (errno == ETIMEDOUT) {
                fprintf(stderr,
                        "TX: data frame timed out (no ACK)\n");
            } else {
                perror("TX: nrf24_send_blocking(data)");
            }
            fclose(fin);
            nrf24_deinit(&radio);
            return 1;
        }

        sent += n;

        /* Simple progress every ~64 KB */
        if ((sent % (64 * 1024)) < NRF24_MAX_PAYLOAD_SIZE) {
            printf("TX: %llu / %llu bytes (%.1f%%)\n",
                   (unsigned long long)sent,
                   (unsigned long long)total_bytes,
                   (total_bytes ? (100.0 * (double)sent / (double)total_bytes) : 100.0));
            fflush(stdout);
        }
    }

    double t_end = now_seconds();
    double dt = t_end - t_start;
    double kbps = (dt > 0.0) ? (8.0 * (double)sent / (1000.0 * dt)) : 0.0;

    printf("TX: done. Sent %llu bytes in %.3f s (%.1f kbps)\n",
           (unsigned long long)sent, dt, kbps);

    fclose(fin);
    nrf24_deinit(&radio);
    return 0;
}

/* ---------- RX path: wait for header, then receive file ---------- */

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

    if (nrf24_configure_quick(&radio, 76) < 0) {
        perror("nrf24_configure_quick");
        nrf24_deinit(&radio);
        return 1;
    }

    if (nrf24_set_mode_rx(&radio) < 0) {
        perror("nrf24_set_mode_rx");
        nrf24_deinit(&radio);
        return 1;
    }

    printf("RX: waiting for header on channel 76...\n");

    uint8_t hdr_buf[NRF24_MAX_PAYLOAD_SIZE];
    uint8_t hdr_len = NRF24_MAX_PAYLOAD_SIZE;
    uint64_t total_bytes = 0;

    /* Wait indefinitely for a valid header (magic + length) */
    for (;;) {
        hdr_len = NRF24_MAX_PAYLOAD_SIZE;
        if (nrf24_recv_blocking(&radio, hdr_buf, &hdr_len, 0) < 0) {
            perror("RX: nrf24_recv_blocking(header)");
            nrf24_deinit(&radio);
            return 1;
        }

        if (hdr_len >= QM_HEADER_TOTAL_LEN &&
            memcmp(hdr_buf, QM_HEADER_MAGIC, QM_HEADER_MAGIC_LEN) == 0) {
            total_bytes = decode_u64_le(hdr_buf + QM_HEADER_MAGIC_LEN);
            printf("RX: got header. Expecting %llu bytes\n",
                   (unsigned long long)total_bytes);
            break;
        } else {
            fprintf(stderr,
                    "RX: received non-header packet (len=%u), ignoring\n",
                    hdr_len);
        }
    }

    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        perror("fopen(output_file)");
        nrf24_deinit(&radio);
        return 1;
    }

    printf("RX: receiving file into '%s'...\n", output_path);

    uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
    uint64_t received = 0;
    double t_start = now_seconds();

    while (received < total_bytes) {
        uint8_t len = NRF24_MAX_PAYLOAD_SIZE;
        int ret = nrf24_recv_blocking(&radio, buf, &len, 5000);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                fprintf(stderr,
                        "RX: timeout waiting for data (received=%llu / %llu)\n",
                        (unsigned long long)received,
                        (unsigned long long)total_bytes);
                continue; /* keep waiting */
            } else {
                perror("RX: nrf24_recv_blocking(data)");
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }
        }

        if (len == 0) {
            /* Shouldn't happen with DPL */
            continue;
        }

        size_t to_write = len;
        if (received + to_write > total_bytes)
            to_write = (size_t)(total_bytes - received);

        size_t w = fwrite(buf, 1, to_write, fout);
        if (w != to_write) {
            perror("fwrite");
            fclose(fout);
            nrf24_deinit(&radio);
            return 1;
        }

        received += w;

        if ((received % (64 * 1024)) < NRF24_MAX_PAYLOAD_SIZE) {
            printf("RX: %llu / %llu bytes (%.1f%%)\n",
                   (unsigned long long)received,
                   (unsigned long long)total_bytes,
                   (total_bytes ? (100.0 * (double)received / (double)total_bytes) : 100.0));
            fflush(stdout);
        }
    }

    double t_end = now_seconds();
    double dt = t_end - t_start;
    double kbps = (dt > 0.0) ? (8.0 * (double)received / (1000.0 * dt)) : 0.0;

    printf("RX: done. Received %llu bytes in %.3f s (%.1f kbps)\n",
           (unsigned long long)received, dt, kbps);

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
