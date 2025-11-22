#include "nrf24.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Example 5-byte addresses (LSB-first as usual for nRF24) */
static const uint8_t TX_ADDR[5] = { 'M', 'T', 'P', 'T', 'X' };
static const uint8_t RX_ADDR[5] = { 'M', 'T', 'P', 'R', 'X' };

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s tx <spi_dev> <ce_gpio>\n"
            "  %s rx <spi_dev> <ce_gpio>\n"
            "Example: %s tx /dev/spidev0.0 25\n",
            prog, prog, prog);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        usage(argv[0]);
        return 1;
    }

    const char *role = argv[1];
    const char *spi_dev = argv[2];
    int ce_gpio = atoi(argv[3]);

    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)ce_gpio
    };

    nrf24_t dev;
    if (nrf24_init(&dev, &cfg) < 0) {
        perror("nrf24_init");
        return 1;
    }

    /* For this demo we fix channel to 76 (2.476 GHz) */
    if (strcmp(role, "tx") == 0) {
        if (nrf24_configure_basic(&dev, 76, TX_ADDR, RX_ADDR) < 0) {
            perror("nrf24_configure_basic(tx)");
            nrf24_deinit(&dev);
            return 1;
        }

        if (nrf24_set_tx_mode(&dev) < 0) {
            perror("nrf24_set_tx_mode");
            nrf24_deinit(&dev);
            return 1;
        }

        printf("TX: enter message to send (<=32 bytes). Ctrl+D to exit.\n");

        char line[256];
        while (fgets(line, sizeof(line), stdin)) {
            size_t len = strlen(line);
            if (len && line[len - 1] == '\n')
                line[--len] = '\0';

            if (len > 32)
                len = 32;

            if (nrf24_send_blocking(&dev, (const uint8_t *)line, len, 100) < 0) {
                perror("nrf24_send_blocking");
            } else {
                printf("TX: sent %zu bytes: \"%.*s\"\n", len, (int)len, line);
            }
        }

    } else if (strcmp(role, "rx") == 0) {

        if (nrf24_configure_basic(&dev, 76, TX_ADDR, RX_ADDR) < 0) {
            perror("nrf24_configure_basic(rx)");
            nrf24_deinit(&dev);
            return 1;
        }

        if (nrf24_set_rx_mode(&dev) < 0) {
            perror("nrf24_set_rx_mode");
            nrf24_deinit(&dev);
            return 1;
        }

        printf("RX: listening on channel 76...\n");

        uint8_t buf[32];
        size_t  got = 0;

        for (;;) {
            int ret = nrf24_recv_blocking(&dev, buf, sizeof(buf), &got, 5000);
            if (ret < 0) {
                perror("nrf24_recv_blocking");
            } else {
                printf("RX: got %zu bytes: \"", got);
                for (size_t i = 0; i < got; ++i) {
                    uint8_t c = buf[i];
                    if (c >= 32 && c <= 126)
                        putchar((char)c);
                    else
                        printf("\\x%02X", c);
                }
                printf("\"\n");
            }
        }
    } else {
        usage(argv[0]);
        nrf24_deinit(&dev);
        return 1;
    }

    nrf24_deinit(&dev);
    return 0;
}
