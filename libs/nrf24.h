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

/* Configure the radio with user-provided parameters.
 *
 *  channel            : RF channel (0..125)
 *  data_rate_kbps     : 250, 1000 or 2000
 *  pa_level_dbm       : -18, -12, -6 or 0
 *  crc_bytes          : 0 (off), 1 or 2
 *  retr_delay         : 0..15  (delay = (retr_delay + 1) * 250 us)
 *  retr_tries         : 0..15  (number of auto-retransmits)
 *
 * Returns 0 on success, -1 on error (errno set).
 */
int nrf24_configure_advanced(nrf24_t *dev,
                             uint8_t  channel,
                             unsigned int data_rate_kbps,
                             int      pa_level_dbm,
                             unsigned int crc_bytes,
                             unsigned int retr_delay,
                             unsigned int retr_tries);


/* Dump current radio configuration (read from registers) in human-readable form.
 *
 * This reads the main configuration registers from the chip and prints a
 * human-readable summary using the logger (logger_info()).
 *
 * Returns 0 on success, -1 on SPI/GPIO error (errno is set).
 */
int nrf24_dump_config(nrf24_t *dev);


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
