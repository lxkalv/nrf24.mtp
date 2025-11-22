#ifndef NRF24_H
#define NRF24_H

#include <stdint.h>

#define NRF24_MAX_PAYLOAD_SIZE 32

typedef struct {
    const char *spi_device;   /* e.g. "/dev/spidev0.0" */
    uint32_t    spi_speed_hz; /* e.g. 8000000 */
    uint8_t     ce_gpio;      /* BCM number, e.g. 22 */
} nrf24_config_t;

typedef struct {
    int             spi_fd;
    int             ce_fd;
    nrf24_config_t  cfg;
} nrf24_t;

/* ----- low-level register / buffer API (handy for debugging) ----- */

int  nrf24_write_reg(nrf24_t *dev, uint8_t reg, uint8_t value);
int  nrf24_read_reg(nrf24_t *dev, uint8_t reg, uint8_t *value);
int  nrf24_write_buf(nrf24_t *dev, uint8_t cmd, const uint8_t *data, uint8_t len);
int  nrf24_read_buf(nrf24_t *dev, uint8_t cmd, uint8_t *data, uint8_t len);

/* ----- high-level API used by quick_mode ----- */

int  nrf24_init(nrf24_t *dev, const nrf24_config_t *cfg);
void nrf24_deinit(nrf24_t *dev);

/* Configure channel, data rate, auto-ack, addresses, payload size, etc. */
int  nrf24_configure_quick(nrf24_t *dev, uint8_t channel);

/* Put radio into RX or TX mode (CE handled internally) */
int  nrf24_set_mode_rx(nrf24_t *dev);
int  nrf24_set_mode_tx(nrf24_t *dev);

/* Blocking send / receive.
 * timeout_ms == 0 means "wait forever".
 * Returns 0 on success, -1 on error, sets errno.
 */
int  nrf24_send_blocking(nrf24_t *dev,
                         const void *payload, uint8_t length,
                         unsigned int timeout_ms);

int  nrf24_recv_blocking(nrf24_t *dev,
                         void *payload, uint8_t *length_inout,
                         unsigned int timeout_ms);

#endif /* NRF24_H */
