#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "nrf24.h"

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <tx|rx> <spi_device> <ce_bcm_gpio>\n"
            "Example: %s tx /dev/spidev0.0 22\n",
            prog, prog);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    const char *mode = argv[1];
    const char *spi_dev = argv[2];
    int ce_bcm = atoi(argv[3]);

    if (ce_bcm < 0 || ce_bcm > 255) {
        fprintf(stderr, "Invalid CE GPIO: %d\n", ce_bcm);
        return 1;
    }

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

    int is_tx = 0;
    if (strcmp(mode, "tx") == 0) {
        is_tx = 1;
    } else if (strcmp(mode, "rx") == 0) {
        is_tx = 0;
    } else {
        usage(argv[0]);
        nrf24_deinit(&radio);
        return 1;
    }

    if (is_tx) {
        if (nrf24_set_mode_tx(&radio) < 0) {
            perror("nrf24_set_mode_tx");
            nrf24_deinit(&radio);
            return 1;
        }

        printf("TX: enter message to send (<=%d bytes). Ctrl+D to exit.\n",
               NRF24_MAX_PAYLOAD_SIZE);

        char line[256];
        while (fgets(line, sizeof(line), stdin)) {
            size_t len = strlen(line);
            if (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = '\0';

            if (len == 0)
                continue;

            if (len > NRF24_MAX_PAYLOAD_SIZE)
                len = NRF24_MAX_PAYLOAD_SIZE;

            uint8_t payload[NRF24_MAX_PAYLOAD_SIZE];
            memcpy(payload, line, len);

            int ret = nrf24_send_blocking(&radio, payload, (uint8_t)len, 500);
            if (ret < 0) {
                if (errno == ETIMEDOUT) {
                    fprintf(stderr,
                            "nrf24_send_blocking: Connection timed out\n");
                } else {
                    perror("nrf24_send_blocking");
                }
            } else {
                printf("TX: sent %zu bytes\n", len);
            }
        }
    } else {
        if (nrf24_set_mode_rx(&radio) < 0) {
            perror("nrf24_set_mode_rx");
            nrf24_deinit(&radio);
            return 1;
        }

        printf("RX: listening on channel 76...\n");

        for (;;) {
            uint8_t buf[NRF24_MAX_PAYLOAD_SIZE + 1];
            uint8_t len = NRF24_MAX_PAYLOAD_SIZE;

            int ret = nrf24_recv_blocking(&radio, buf, &len, 5000);
            if (ret < 0) {
                if (errno == ETIMEDOUT) {
                    fprintf(stderr,
                            "nrf24_recv_blocking: Connection timed out\n");
                    continue;
                } else {
                    perror("nrf24_recv_blocking");
                    continue;
                }
            }

            buf[len] = '\0';
            printf("RX: got %u bytes: '%s'\n", len, buf);
            fflush(stdout);
        }
    }

    nrf24_deinit(&radio);
    return 0;
}
