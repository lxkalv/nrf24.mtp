#define _GNU_SOURCE
#include "nrf24.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <dirent.h>

/* ---------- small helpers ---------- */
/* Find the lowest /sys/class/gpio/gpiochip base value.
 * On Raspberry Pi this is the main SoC GPIO base (e.g. 512).
 */
static int get_gpio_base(void)
{
    DIR *d = opendir("/sys/class/gpio");
    if (!d) return -1;

    struct dirent *de;
    int best = -1;

    while ((de = readdir(d)) != NULL) {
        if (strncmp(de->d_name, "gpiochip", 8) != 0)
            continue;

        char path[128];
        snprintf(path, sizeof(path), "/sys/class/gpio/%s/base", de->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;

        char buf[32];
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (r <= 0)
            continue;

        buf[r] = '\0';
        int val = atoi(buf);
        if (val < 0)
            continue;

        if (best < 0 || val < best)
            best = val;
    }

    closedir(d);
    return best;
}

static int gpio_write_str(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t w = write(fd, value, strlen(value));
    close(fd);
    return (w == (ssize_t)strlen(value)) ? 0 : -1;
}

static int gpio_export(unsigned int pin)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u\n", pin);
    return gpio_write_str("/sys/class/gpio/export", buf);
}

static int gpio_unexport(unsigned int pin)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u\n", pin);
    return gpio_write_str("/sys/class/gpio/unexport", buf);
}


static int gpio_set_direction(uint8_t pin, const char *dir)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/direction", pin);
    return gpio_write_str(path, dir);
}

static int gpio_open_value(uint8_t pin)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", pin);
    return open(path, O_WRONLY);
}

static int gpio_write_value_fd(int fd, int value)
{
    const char ch = value ? '1' : '0';
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    ssize_t w = write(fd, &ch, 1);
    return (w == 1) ? 0 : -1;
}

/* CE control */
static int ce_set(nrf24_t *dev, int level)
{
    return gpio_write_value_fd(dev->ce_fd, level);
}

/* SPI transfer: tx_len bytes sent, rx_len bytes read (same length typically) */
static int spi_transfer(int fd,
                        const uint8_t *tx, uint8_t *rx,
                        size_t len)
{
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = len,
        .speed_hz = 0,       /* use fd's configured speed */
        .delay_usecs = 0,
        .bits_per_word = 0,  /* use fd's configured bits */
        .cs_change = 0
    };

    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    return (ret < 1) ? -1 : 0;
}

/* Read STATUS by sending NOP */
static int nrf24_read_status(nrf24_t *dev, uint8_t *status)
{
    uint8_t tx = NRF24_CMD_NOP;
    uint8_t rx = 0;
    if (spi_transfer(dev->spi_fd, &tx, &rx, 1) < 0)
        return -1;
    if (status) *status = rx;
    return 0;
}

/* Busy-wait sleep helper */
static void sleep_ms(unsigned int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---------- public API implementation ---------- */

int nrf24_init(nrf24_t *dev, const nrf24_config_t *cfg)
{
    if (!dev || !cfg) {
        errno = EINVAL;
        return -1;
    }

    memset(dev, 0, sizeof(*dev));
    dev->cfg = *cfg;

    /* ---- open & configure SPI ---- */

    int fd = open(cfg->spi_device, O_RDWR);
    if (fd < 0) {
        perror("open spi");
        return -1;
    }

    uint8_t  mode  = SPI_MODE_0;
    uint8_t  bits  = 8;
    uint32_t speed = cfg->spi_speed_hz ? cfg->spi_speed_hz : 8000000;

    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("spi ioctl");
        close(fd);
        return -1;
    }

    dev->spi_fd = fd;

    /* ---- map BCM CE pin -> global GPIO index ---- */
    /* e.g. on your Pi: gpiochip512/base = 512, BCM22 -> 512+22 = 534 */

    int base = get_gpio_base();
    if (base < 0) {
        perror("get_gpio_base");
        close(fd);
        return -1;
    }

    unsigned int ce_global = (unsigned int)(base + cfg->ce_gpio);

    /* ---- configure CE GPIO via sysfs ---- */

    (void)gpio_unexport(ce_global);  /* ignore error if not exported */

    if (gpio_export(ce_global) < 0) {
        perror("gpio_export");
        close(fd);
        return -1;
    }

    /* allow sysfs to create gpioN directory */
    usleep(100000);

    if (gpio_set_direction(ce_global, "out") < 0) {
        perror("gpio_direction");
        close(fd);
        return -1;
    }

    dev->ce_fd = gpio_open_value(ce_global);
    if (dev->ce_fd < 0) {
        perror("gpio_open_value");
        close(fd);
        return -1;
    }

    /* CE low initially (standby) */
    if (gpio_write_value_fd(dev->ce_fd, 0) < 0) {
        perror("gpio_write_value_fd");
        close(dev->ce_fd);
        close(fd);
        return -1;
    }

    /* ---- power up the nRF24 ---- */

    /* Power up: PWR_UP=1, EN_CRC=1 (1-byte CRC for now, PRIM_RX=0) */
    if (nrf24_write_reg(dev, NRF24_REG_CONFIG,
                        NRF24_CONFIG_PWR_UP | NRF24_CONFIG_EN_CRC) < 0) {
        perror("nrf24_write_reg(CONFIG)");
        nrf24_deinit(dev);
        return -1;
    }

    /* tpd2stby ~1.5 ms */
    sleep_ms(2);

    return 0;
}


void nrf24_deinit(nrf24_t *dev)
{
    if (!dev) return;

    /* power down radio */
    uint8_t cfg_val;
    if (nrf24_read_reg(dev, NRF24_REG_CONFIG, &cfg_val) == 0) {
        cfg_val &= ~NRF24_CONFIG_PWR_UP;
        (void)nrf24_write_reg(dev, NRF24_REG_CONFIG, cfg_val);
    }

    if (dev->ce_fd >= 0) {
        close(dev->ce_fd);
        gpio_unexport(dev->cfg.ce_gpio);
    }

    if (dev->spi_fd >= 0)
        close(dev->spi_fd);

    memset(dev, 0, sizeof(*dev));
}

/* R/W one-byte register */
int nrf24_read_reg(nrf24_t *dev, uint8_t reg, uint8_t *value)
{
    uint8_t tx[2] = { NRF24_CMD_R_REGISTER | (reg & 0x1F), 0xFF };
    uint8_t rx[2] = { 0 };
    if (spi_transfer(dev->spi_fd, tx, rx, 2) < 0)
        return -1;
    if (value) *value = rx[1];
    return 0;
}

int nrf24_write_reg(nrf24_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { NRF24_CMD_W_REGISTER | (reg & 0x1F), value };
    uint8_t rx[2];
    return spi_transfer(dev->spi_fd, tx, rx, 2);
}

/* multi-byte registers (addresses etc.) */
int nrf24_read_buf(nrf24_t *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!buf || !len) {
        errno = EINVAL;
        return -1;
    }
    uint8_t tx[len + 1];
    uint8_t rx[len + 1];
    tx[0] = NRF24_CMD_R_REGISTER | (reg & 0x1F);
    memset(tx + 1, 0xFF, len);

    if (spi_transfer(dev->spi_fd, tx, rx, len + 1) < 0)
        return -1;

    memcpy(buf, rx + 1, len);
    return 0;
}

int nrf24_write_buf(nrf24_t *dev, uint8_t reg, const uint8_t *buf, size_t len)
{
    if (!buf || !len) {
        errno = EINVAL;
        return -1;
    }
    uint8_t tx[len + 1];
    uint8_t rx[len + 1];
    tx[0] = NRF24_CMD_W_REGISTER | (reg & 0x1F);
    memcpy(tx + 1, buf, len);

    return spi_transfer(dev->spi_fd, tx, rx, len + 1);
}

/* FIFO helpers */
int nrf24_flush_tx(nrf24_t *dev)
{
    uint8_t tx = NRF24_CMD_FLUSH_TX;
    uint8_t rx = 0;
    return spi_transfer(dev->spi_fd, &tx, &rx, 1);
}

int nrf24_flush_rx(nrf24_t *dev)
{
    uint8_t tx = NRF24_CMD_FLUSH_RX;
    uint8_t rx = 0;
    return spi_transfer(dev->spi_fd, &tx, &rx, 1);
}

/* High-level config */

int nrf24_set_channel(nrf24_t *dev, uint8_t channel)
{
    /* 7-bit field: 0..125 typical */
    return nrf24_write_reg(dev, NRF24_REG_RF_CH, channel & 0x7F);
}

int nrf24_set_datarate(nrf24_t *dev, nrf24_datarate_t dr)
{
    uint8_t rf;
    if (nrf24_read_reg(dev, NRF24_REG_RF_SETUP, &rf) < 0)
        return -1;

    rf &= ~(NRF24_RF_DR_LOW | NRF24_RF_DR_HIGH);

    switch (dr) {
    case NRF24_DR_250KBPS:
        rf |= NRF24_RF_DR_LOW;
        break;
    case NRF24_DR_1MBPS:
        /* both bits 0 */
        break;
    case NRF24_DR_2MBPS:
        rf |= NRF24_RF_DR_HIGH;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    return nrf24_write_reg(dev, NRF24_REG_RF_SETUP, rf);
}

int nrf24_set_power(nrf24_t *dev, nrf24_power_t pwr)
{
    uint8_t rf;
    if (nrf24_read_reg(dev, NRF24_REG_RF_SETUP, &rf) < 0)
        return -1;

    rf &= ~NRF24_RF_PWR_MASK;
    rf |= ((uint8_t)pwr & 0x03) << 1;

    return nrf24_write_reg(dev, NRF24_REG_RF_SETUP, rf);
}

int nrf24_set_retries(nrf24_t *dev, uint8_t ard, uint8_t arc)
{
    /* ARD: upper 4 bits (0..15) => delay = (ard + 1) * 250us
       ARC: lower 4 bits (0..15) => max retransmits */
    uint8_t val = ((ard & 0x0F) << 4) | (arc & 0x0F);
    return nrf24_write_reg(dev, NRF24_REG_SETUP_RETR, val);
}

int nrf24_set_auto_ack(nrf24_t *dev, bool enable_p0)
{
    uint8_t en_aa = 0;
    if (enable_p0)
        en_aa |= 0x01; /* ENAA_P0 */
    return nrf24_write_reg(dev, NRF24_REG_EN_AA, en_aa);
}

int nrf24_enable_dynamic_payloads(nrf24_t *dev, bool enable_p0)
{
    uint8_t feature;
    if (nrf24_read_reg(dev, NRF24_REG_FEATURE, &feature) < 0)
        return -1;

    if (enable_p0)
        feature |= NRF24_FEATURE_EN_DPL;
    else
        feature &= ~NRF24_FEATURE_EN_DPL;

    if (nrf24_write_reg(dev, NRF24_REG_FEATURE, feature) < 0)
        return -1;

    uint8_t dynpd = 0;
    if (enable_p0)
        dynpd |= NRF24_DYNPD_DPL_P0;

    return nrf24_write_reg(dev, NRF24_REG_DYNPD, dynpd);
}

int nrf24_set_crc(nrf24_t *dev, bool enable, bool two_bytes)
{
    uint8_t cfg;
    if (nrf24_read_reg(dev, NRF24_REG_CONFIG, &cfg) < 0)
        return -1;

    if (enable) {
        cfg |= NRF24_CONFIG_EN_CRC;
        if (two_bytes)
            cfg |= NRF24_CONFIG_CRCO;
        else
            cfg &= ~NRF24_CONFIG_CRCO;
    } else {
        cfg &= ~NRF24_CONFIG_EN_CRC;
    }

    return nrf24_write_reg(dev, NRF24_REG_CONFIG, cfg);
}

int nrf24_set_address_width_5bytes(nrf24_t *dev)
{
    /* AW = 0b11 => 5 bytes */
    return nrf24_write_reg(dev, NRF24_REG_SETUP_AW, 0x03);
}

int nrf24_set_rx_address_p0(nrf24_t *dev, const uint8_t *addr, size_t len)
{
    if (!addr || len < 3 || len > 5) {
        errno = EINVAL;
        return -1;
    }
    if (nrf24_write_buf(dev, NRF24_REG_RX_ADDR_P0, addr, len) < 0)
        return -1;

    /* enable pipe 0 */
    uint8_t en_rx;
    if (nrf24_read_reg(dev, NRF24_REG_EN_RXADDR, &en_rx) < 0)
        return -1;
    en_rx |= 0x01;
    return nrf24_write_reg(dev, NRF24_REG_EN_RXADDR, en_rx);
}

int nrf24_set_tx_address(nrf24_t *dev, const uint8_t *addr, size_t len)
{
    if (!addr || len < 3 || len > 5) {
        errno = EINVAL;
        return -1;
    }
    return nrf24_write_buf(dev, NRF24_REG_TX_ADDR, addr, len);
}

/* Mode control */

int nrf24_set_rx_mode(nrf24_t *dev)
{
    uint8_t cfg;
    if (nrf24_read_reg(dev, NRF24_REG_CONFIG, &cfg) < 0)
        return -1;
    cfg |= NRF24_CONFIG_PRIM_RX;
    if (nrf24_write_reg(dev, NRF24_REG_CONFIG, cfg) < 0)
        return -1;

    /* CE high -> RX mode after ~130us */
    ce_set(dev, 1);
    sleep_ms(1);
    return 0;
}

int nrf24_set_tx_mode(nrf24_t *dev)
{
    uint8_t cfg;
    if (nrf24_read_reg(dev, NRF24_REG_CONFIG, &cfg) < 0)
        return -1;
    cfg &= ~NRF24_CONFIG_PRIM_RX;
    if (nrf24_write_reg(dev, NRF24_REG_CONFIG, cfg) < 0)
        return -1;

    /* Standby-I with CE low */
    ce_set(dev, 0);
    sleep_ms(1);
    return 0;
}

/* Blocking send:
 * - assumes we are in PTX (PRIM_RX=0)
 * - loads payload
 * - pulses CE high >= 10us
 * - polls STATUS until TX_DS or MAX_RT or timeout
 */
int nrf24_send_blocking(nrf24_t *dev,
                        const uint8_t *payload, size_t len,
                        unsigned int timeout_ms)
{
    if (!payload || len == 0 || len > 32) {
        errno = EINVAL;
        return -1;
    }

    /* ensure TX FIFO clean-ish */
    if (nrf24_flush_tx(dev) < 0)
        return -1;

    /* write payload */
    uint8_t tx[len + 1];
    uint8_t rx[len + 1];
    tx[0] = NRF24_CMD_W_TX_PAYLOAD;
    memcpy(tx + 1, payload, len);

    if (spi_transfer(dev->spi_fd, tx, rx, len + 1) < 0)
        return -1;

    /* pulse CE for >=10us */
    ce_set(dev, 1);
    usleep(20); /* 20us just to be safe */
    ce_set(dev, 0);

    /* poll STATUS */
    unsigned int waited = 0;
    const unsigned int step_ms = 1;
    uint8_t status = 0;

    while (waited < timeout_ms) {
        if (nrf24_read_status(dev, &status) < 0)
            return -1;

        if (status & NRF24_STATUS_TX_DS) {
            /* clear TX_DS */
            nrf24_write_reg(dev, NRF24_REG_STATUS, NRF24_STATUS_TX_DS);
            return 0; /* success */
        }
        if (status & NRF24_STATUS_MAX_RT) {
            /* clear MAX_RT and flush TX */
            nrf24_write_reg(dev, NRF24_REG_STATUS, NRF24_STATUS_MAX_RT);
            nrf24_flush_tx(dev);
            errno = ETIMEDOUT;
            return -1;
        }

        sleep_ms(step_ms);
        waited += step_ms;
    }

    errno = ETIMEDOUT;
    return -1;
}

/* Blocking receive:
 * - assumes RX mode (PRIM_RX=1, CE=1)
 * - waits for RX_DR or timeout
 * - reads payload width (if DPL) and payload
 */
int nrf24_recv_blocking(nrf24_t *dev,
                        uint8_t *payload, size_t max_len,
                        size_t *out_len,
                        unsigned int timeout_ms)
{
    if (!payload || max_len == 0) {
        errno = EINVAL;
        return -1;
    }

    unsigned int waited = 0;
    const unsigned int step_ms = 1;
    uint8_t status = 0;

    while (waited < timeout_ms) {
        if (nrf24_read_status(dev, &status) < 0)
            return -1;

        if (status & NRF24_STATUS_RX_DR)
            break;

        sleep_ms(step_ms);
        waited += step_ms;
    }

    if (!(status & NRF24_STATUS_RX_DR)) {
        errno = ETIMEDOUT;
        return -1;
    }

    /* read payload width (for DPL) */
    uint8_t tx_wid = NRF24_CMD_R_RX_PL_WID;
    uint8_t rx_wid = 0;
    if (spi_transfer(dev->spi_fd, &tx_wid, &rx_wid, 1) < 0)
        return -1;

    uint8_t pl_len = rx_wid;
    if (pl_len == 0 || pl_len > 32 || pl_len > max_len) {
        /* something is wrong; flush RX FIFO */
        nrf24_flush_rx(dev);
        errno = EIO;
        return -1;
    }

    uint8_t tx[33] = { NRF24_CMD_R_RX_PAYLOAD };
    uint8_t rx[33] = { 0 };
    if (spi_transfer(dev->spi_fd, tx, rx, (size_t)pl_len + 1) < 0)
        return -1;

    memcpy(payload, rx + 1, pl_len);
    if (out_len)
        *out_len = pl_len;

    /* clear RX_DR */
    nrf24_write_reg(dev, NRF24_REG_STATUS, NRF24_STATUS_RX_DR);

    return 0;
}

/* Basic "quick-mode-like" config */
int nrf24_configure_basic(nrf24_t *dev,
                          uint8_t channel,
                          const uint8_t *tx_addr,
                          const uint8_t *rx_addr_p0)
{
    if (!tx_addr || !rx_addr_p0) {
        errno = EINVAL;
        return -1;
    }

    if (nrf24_set_address_width_5bytes(dev) < 0)
        return -1;

    if (nrf24_set_tx_address(dev, tx_addr, 5) < 0)
        return -1;

    if (nrf24_set_rx_address_p0(dev, rx_addr_p0, 5) < 0)
        return -1;

    if (nrf24_set_channel(dev, channel) < 0)
        return -1;

    if (nrf24_set_datarate(dev, NRF24_DR_2MBPS) < 0)
        return -1;

    if (nrf24_set_power(dev, NRF24_PWR_0DBM) < 0)
        return -1;

    /* ARD = 500us (0b0001 => 500us), ARC = 15 (0b1111) */
    if (nrf24_set_retries(dev, 1, 15) < 0)
        return -1;

    if (nrf24_set_auto_ack(dev, true) < 0)
        return -1;

    if (nrf24_enable_dynamic_payloads(dev, true) < 0)
        return -1;

    if (nrf24_set_crc(dev, true, true) < 0)
        return -1;

    return 0;
}
