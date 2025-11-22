#ifndef NRF24_H
#define NRF24_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* nRF24L01+ command set (datasheet table 19) */
#define NRF24_CMD_R_REGISTER         0x00
#define NRF24_CMD_W_REGISTER         0x20
#define NRF24_CMD_R_RX_PAYLOAD       0x61
#define NRF24_CMD_W_TX_PAYLOAD       0xA0
#define NRF24_CMD_FLUSH_TX           0xE1
#define NRF24_CMD_FLUSH_RX           0xE2
#define NRF24_CMD_REUSE_TX_PL        0xE3
#define NRF24_CMD_R_RX_PL_WID        0x60
#define NRF24_CMD_W_ACK_PAYLOAD      0xA8  /* OR with pipe[2:0] */
#define NRF24_CMD_W_TX_PAYLOAD_NOACK 0xB0
#define NRF24_CMD_NOP                0xFF

/* nRF24L01+ registers (datasheet register map) */
#define NRF24_REG_CONFIG            0x00
#define NRF24_REG_EN_AA             0x01
#define NRF24_REG_EN_RXADDR         0x02
#define NRF24_REG_SETUP_AW          0x03
#define NRF24_REG_SETUP_RETR        0x04
#define NRF24_REG_RF_CH             0x05
#define NRF24_REG_RF_SETUP          0x06
#define NRF24_REG_STATUS            0x07
#define NRF24_REG_OBSERVE_TX        0x08
#define NRF24_REG_RPD               0x09
#define NRF24_REG_RX_ADDR_P0        0x0A
#define NRF24_REG_RX_ADDR_P1        0x0B
#define NRF24_REG_RX_ADDR_P2        0x0C
#define NRF24_REG_RX_ADDR_P3        0x0D
#define NRF24_REG_RX_ADDR_P4        0x0E
#define NRF24_REG_RX_ADDR_P5        0x0F
#define NRF24_REG_TX_ADDR           0x10
#define NRF24_REG_RX_PW_P0          0x11
#define NRF24_REG_RX_PW_P1          0x12
#define NRF24_REG_RX_PW_P2          0x13
#define NRF24_REG_RX_PW_P3          0x14
#define NRF24_REG_RX_PW_P4          0x15
#define NRF24_REG_RX_PW_P5          0x16
#define NRF24_REG_FIFO_STATUS       0x17
#define NRF24_REG_DYNPD             0x1C
#define NRF24_REG_FEATURE           0x1D

/* CONFIG bits */
#define NRF24_CONFIG_MASK_RX_DR     (1 << 6)
#define NRF24_CONFIG_MASK_TX_DS     (1 << 5)
#define NRF24_CONFIG_MASK_MAX_RT    (1 << 4)
#define NRF24_CONFIG_EN_CRC         (1 << 3)
#define NRF24_CONFIG_CRCO           (1 << 2)
#define NRF24_CONFIG_PWR_UP         (1 << 1)
#define NRF24_CONFIG_PRIM_RX        (1 << 0)

/* STATUS bits */
#define NRF24_STATUS_RX_DR          (1 << 6)
#define NRF24_STATUS_TX_DS          (1 << 5)
#define NRF24_STATUS_MAX_RT         (1 << 4)
#define NRF24_STATUS_RX_P_NO_MASK   0x0E
#define NRF24_STATUS_TX_FULL        (1 << 0)

/* FIFO_STATUS bits */
#define NRF24_FIFO_STATUS_TX_REUSE  (1 << 6)
#define NRF24_FIFO_STATUS_TX_FULL   (1 << 5)
#define NRF24_FIFO_STATUS_TX_EMPTY  (1 << 4)
#define NRF24_FIFO_STATUS_RX_FULL   (1 << 1)
#define NRF24_FIFO_STATUS_RX_EMPTY  (1 << 0)

/* FEATURE bits */
#define NRF24_FEATURE_EN_DPL        (1 << 2)
#define NRF24_FEATURE_EN_ACK_PAY    (1 << 1)
#define NRF24_FEATURE_EN_DYN_ACK    (1 << 0)

/* DYNPD bits */
#define NRF24_DYNPD_DPL_P0          (1 << 0)
#define NRF24_DYNPD_DPL_P1          (1 << 1) /* etc. */

/* RF_SETUP bits (data rate & power) */
#define NRF24_RF_DR_LOW             (1 << 5)
#define NRF24_RF_DR_HIGH            (1 << 3)
#define NRF24_RF_PWR_MASK           (3 << 1)

/* Helper enums for config */
typedef enum {
    NRF24_DR_250KBPS,
    NRF24_DR_1MBPS,
    NRF24_DR_2MBPS
} nrf24_datarate_t;

typedef enum {
    NRF24_PWR_NEG_18DBM,
    NRF24_PWR_NEG_12DBM,
    NRF24_PWR_NEG_6DBM,
    NRF24_PWR_0DBM
} nrf24_power_t;

typedef struct {
    const char *spi_device;   /* e.g. "/dev/spidev0.0" */
    uint32_t    spi_speed_hz; /* e.g. 8000000 */
    uint8_t     ce_gpio;      /* BCM number for CE pin */
    /* You can add irq_gpio later if you wire IRQ */
} nrf24_config_t;

typedef struct {
    int         spi_fd;
    int         ce_fd;
    nrf24_config_t cfg;
} nrf24_t;

/* Public API */

/* Initialize SPI + CE GPIO and power up the radio in standby-I */
int  nrf24_init(nrf24_t *dev, const nrf24_config_t *cfg);

/* Power down radio and close descriptors */
void nrf24_deinit(nrf24_t *dev);

/* Basic register access */
int  nrf24_read_reg(nrf24_t *dev, uint8_t reg, uint8_t *value);
int  nrf24_write_reg(nrf24_t *dev, uint8_t reg, uint8_t value);
int  nrf24_read_buf(nrf24_t *dev, uint8_t reg, uint8_t *buf, size_t len);
int  nrf24_write_buf(nrf24_t *dev, uint8_t reg, const uint8_t *buf, size_t len);

/* FIFO helpers */
int  nrf24_flush_tx(nrf24_t *dev);
int  nrf24_flush_rx(nrf24_t *dev);

/* High-level configuration helpers */
int  nrf24_set_channel(nrf24_t *dev, uint8_t channel);
int  nrf24_set_datarate(nrf24_t *dev, nrf24_datarate_t dr);
int  nrf24_set_power(nrf24_t *dev, nrf24_power_t pwr);
int  nrf24_set_retries(nrf24_t *dev, uint8_t ard, uint8_t arc);
int  nrf24_set_auto_ack(nrf24_t *dev, bool enable_p0);
int  nrf24_enable_dynamic_payloads(nrf24_t *dev, bool enable_p0);
int  nrf24_set_crc(nrf24_t *dev, bool enable, bool two_bytes);
int  nrf24_set_address_width_5bytes(nrf24_t *dev);
int  nrf24_set_rx_address_p0(nrf24_t *dev, const uint8_t *addr, size_t len);
int  nrf24_set_tx_address(nrf24_t *dev, const uint8_t *addr, size_t len);

/* Mode control */
int  nrf24_set_rx_mode(nrf24_t *dev);
int  nrf24_set_tx_mode(nrf24_t *dev);

/* Blocking send/receive helpers */
int  nrf24_send_blocking(nrf24_t *dev,
                         const uint8_t *payload, size_t len,
                         unsigned int timeout_ms);

int  nrf24_recv_blocking(nrf24_t *dev,
                         uint8_t *payload, size_t max_len,
                         size_t *out_len,
                         unsigned int timeout_ms);

/* A convenience to configure a “quick_mode-like” baseline:
 * - 2 Mbps
 * - 0 dBm
 * - 5 byte addresses
 * - auto-ACK enabled on pipe 0
 * - dynamic payload on pipe 0
 * - CRC enabled, 2 bytes
 * - retries: ARD=500us, ARC=15
 */
int  nrf24_configure_basic(nrf24_t *dev,
                           uint8_t channel,
                           const uint8_t *tx_addr /* 5 bytes */,
                           const uint8_t *rx_addr_p0 /* 5 bytes */);

#endif /* NRF24_H */
