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
#include <stdlib.h>

/* --------- nRF24L01+ command / register definitions --------- */

#define NRF24_CMD_R_REGISTER       0x00
#define NRF24_CMD_W_REGISTER       0x20
#define NRF24_CMD_REGISTER_MASK    0x1F
#define NRF24_CMD_R_RX_PAYLOAD     0x61
#define NRF24_CMD_W_TX_PAYLOAD     0xA0
#define NRF24_CMD_FLUSH_TX         0xE1
#define NRF24_CMD_FLUSH_RX         0xE2
#define NRF24_CMD_REUSE_TX_PL      0xE3
#define NRF24_CMD_R_RX_PL_WID      0x60
#define NRF24_CMD_W_ACK_PAYLOAD    0xA8
#define NRF24_CMD_NOP              0xFF
#define NRF24_CMD_ACTIVATE         0x50

#define NRF24_REG_CONFIG           0x00
#define NRF24_REG_EN_AA            0x01
#define NRF24_REG_EN_RXADDR        0x02
#define NRF24_REG_SETUP_AW         0x03
#define NRF24_REG_SETUP_RETR       0x04
#define NRF24_REG_RF_CH            0x05
#define NRF24_REG_RF_SETUP         0x06
#define NRF24_REG_STATUS           0x07
#define NRF24_REG_OBSERVE_TX       0x08
#define NRF24_REG_RX_ADDR_P0       0x0A
#define NRF24_REG_TX_ADDR          0x10
#define NRF24_REG_RX_PW_P0         0x11
#define NRF24_REG_FIFO_STATUS      0x17
#define NRF24_REG_DYNPD            0x1C
#define NRF24_REG_FEATURE          0x1D

#define NRF24_FEATURE_EN_DPL       (1 << 2)
#define NRF24_FEATURE_EN_ACK_PAY   (1 << 1)
#define NRF24_FEATURE_EN_DYN_ACK   (1 << 0)

#define NRF24_DYNPD_DPL_P0         (1 << 0)

#define NRF24_CONFIG_PRIM_RX       (1 << 0)
#define NRF24_CONFIG_PWR_UP        (1 << 1)
#define NRF24_CONFIG_CRCO          (1 << 2)
#define NRF24_CONFIG_EN_CRC        (1 << 3)

#define NRF24_STATUS_MAX_RT        (1 << 4)
#define NRF24_STATUS_TX_DS         (1 << 5)
#define NRF24_STATUS_RX_DR         (1 << 6)

#define NRF24_RF_SETUP_RF_DR_LOW   (1 << 5)
#define NRF24_RF_SETUP_RF_DR_HIGH  (1 << 3)
#define NRF24_RF_SETUP_RF_PWR_0DBM (3 << 1)

/* Shared address used on pipe0 and TX for quick_mode */
static const uint8_t QUICK_ADDR[5] = { 'M', 'T', 'P', '0', '1' };

/* --------- helpers: GPIO sysfs --------- */

static int gpio_write_str(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t w = write(fd, value, strlen(value));
    close(fd);
    return (w == (ssize_t)strlen(value)) ? 0 : -1;
}

/* Find the lowest /sys/class/gpio/gpiochipX/base value (main SoC GPIO base). */
static int get_gpio_base(void)
{
    DIR *d = opendir("/sys/class/gpio");
    if (!d)
        return -1;

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

static int gpio_set_direction(unsigned int pin, const char *dir)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/direction", pin);
    return gpio_write_str(path, dir);
}

static int gpio_open_value(unsigned int pin)
{
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", pin);
    return open(path, O_WRONLY);
}

static int gpio_write_value_fd(int fd, int value)
{
    const char ch = value ? '1' : '0';
    if (lseek(fd, 0, SEEK_SET) < 0)
        return -1;
    ssize_t w = write(fd, &ch, 1);
    return (w == 1) ? 0 : -1;
}

/* CE control */
static int ce_set(nrf24_t *dev, int level)
{
    return gpio_write_value_fd(dev->ce_fd, level);
}

/* --------- helpers: SPI / timing --------- */

static int spi_transfer(int fd, const uint8_t *tx, uint8_t *rx, size_t len)
{
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx,
        .rx_buf = (unsigned long)rx,
        .len = len,
        .speed_hz = 0,
        .delay_usecs = 0,
        .bits_per_word = 0,
        .cs_change = 0
    };

    int ret = ioctl(fd, SPI_IOC_MESSAGE(1), &tr);
    return (ret < 1) ? -1 : 0;
}

static void sleep_ms(unsigned int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void sleep_us(unsigned int us)
{
    struct timespec ts;
    ts.tv_sec  = us / 1000000U;
    ts.tv_nsec = (long)(us % 1000000U) * 1000L;
    nanosleep(&ts, NULL);
}


/* --------- low-level register access --------- */

int nrf24_write_reg(nrf24_t *dev, uint8_t reg, uint8_t value)
{
    uint8_t buf[2];
    buf[0] = NRF24_CMD_W_REGISTER | (NRF24_CMD_REGISTER_MASK & reg);
    buf[1] = value;
    return spi_transfer(dev->spi_fd, buf, NULL, 2);
}

int nrf24_read_reg(nrf24_t *dev, uint8_t reg, uint8_t *value)
{
    uint8_t tx[2];
    uint8_t rx[2];

    tx[0] = NRF24_CMD_R_REGISTER | (NRF24_CMD_REGISTER_MASK & reg);
    tx[1] = NRF24_CMD_NOP;

    if (spi_transfer(dev->spi_fd, tx, rx, 2) < 0)
        return -1;
    if (value)
        *value = rx[1];
    return 0;
}

int nrf24_write_buf(nrf24_t *dev, uint8_t cmd, const uint8_t *data, uint8_t len)
{
    if (len > 32)
        len = 32;
    uint8_t buf[1 + 32];
    buf[0] = cmd;
    if (len && data)
        memcpy(&buf[1], data, len);
    return spi_transfer(dev->spi_fd, buf, NULL, 1 + len);
}

int nrf24_read_buf(nrf24_t *dev, uint8_t cmd, uint8_t *data, uint8_t len)
{
    if (len > 32)
        len = 32;
    uint8_t tx[1 + 32];
    uint8_t rx[1 + 32];
    tx[0] = cmd;
    memset(&tx[1], NRF24_CMD_NOP, len);
    if (spi_transfer(dev->spi_fd, tx, rx, 1 + len) < 0)
        return -1;
    if (data && len)
        memcpy(data, &rx[1], len);
    return 0;
}

/* --------- small helpers around registers --------- */

static int nrf24_flush_tx(nrf24_t *dev)
{
    uint8_t cmd = NRF24_CMD_FLUSH_TX;
    return spi_transfer(dev->spi_fd, &cmd, NULL, 1);
}

static int nrf24_flush_rx(nrf24_t *dev)
{
    uint8_t cmd = NRF24_CMD_FLUSH_RX;
    return spi_transfer(dev->spi_fd, &cmd, NULL, 1);
}

static int nrf24_clear_interrupts(nrf24_t *dev)
{
    return nrf24_write_reg(dev, NRF24_REG_STATUS,
                           NRF24_STATUS_RX_DR |
                           NRF24_STATUS_TX_DS |
                           NRF24_STATUS_MAX_RT);
}

static int nrf24_set_address(nrf24_t *dev, uint8_t reg, const uint8_t addr[5])
{
    return nrf24_write_buf(dev,
                           NRF24_CMD_W_REGISTER | (NRF24_CMD_REGISTER_MASK & reg),
                           addr, 5);
}

/* --------- init / deinit --------- */

int nrf24_init(nrf24_t *dev, const nrf24_config_t *cfg)
{
    if (!dev || !cfg) {
        errno = EINVAL;
        return -1;
    }

    memset(dev, 0, sizeof(*dev));
    dev->cfg = *cfg;
    dev->spi_fd = -1;
    dev->ce_fd  = -1;

    /* open SPI device */
    int fd = open(cfg->spi_device, O_RDWR);
    if (fd < 0) {
        perror("open spi");
        return -1;
    }

    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = cfg->spi_speed_hz ? cfg->spi_speed_hz : 8000000;

    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("spi ioctl");
        close(fd);
        return -1;
    }

    dev->spi_fd = fd;

    /* map BCM CE -> global GPIO index */
    int base = get_gpio_base();
    if (base < 0) {
        perror("get_gpio_base");
        close(fd);
        dev->spi_fd = -1;
        return -1;
    }

    unsigned int ce_global = (unsigned int)(base + cfg->ce_gpio);

    (void)gpio_unexport(ce_global);
    if (gpio_export(ce_global) < 0) {
        perror("gpio_export");
        close(fd);
        dev->spi_fd = -1;
        return -1;
    }

    usleep(100000);

    if (gpio_set_direction(ce_global, "out") < 0) {
        perror("gpio_direction");
        close(fd);
        dev->spi_fd = -1;
        return -1;
    }

    dev->ce_fd = gpio_open_value(ce_global);
    if (dev->ce_fd < 0) {
        perror("gpio_open_value");
        close(fd);
        dev->spi_fd = -1;
        return -1;
    }

    /* CE low initially */
    if (gpio_write_value_fd(dev->ce_fd, 0) < 0) {
        perror("gpio_write_value_fd");
        close(dev->ce_fd);
        dev->ce_fd = -1;
        close(fd);
        dev->spi_fd = -1;
        return -1;
    }

    /* Power-down, CRC enabled, PRIM_RX=0 for now */
    if (nrf24_write_reg(dev, NRF24_REG_CONFIG,
                        NRF24_CONFIG_EN_CRC) < 0) {
        perror("nrf24_write_reg(CONFIG)");
        nrf24_deinit(dev);
        return -1;
    }

    return 0;
}

void nrf24_deinit(nrf24_t *dev)
{
    if (!dev)
        return;

    if (dev->spi_fd >= 0) {
        uint8_t cfg;
        if (nrf24_read_reg(dev, NRF24_REG_CONFIG, &cfg) == 0) {
            cfg &= ~NRF24_CONFIG_PWR_UP;
            (void)nrf24_write_reg(dev, NRF24_REG_CONFIG, cfg);
        }
    }

    if (dev->ce_fd >= 0) {
        close(dev->ce_fd);
        dev->ce_fd = -1;

        int base = get_gpio_base();
        if (base >= 0) {
            unsigned int ce_global = (unsigned int)(base + dev->cfg.ce_gpio);
            (void)gpio_unexport(ce_global);
        }
    }

    if (dev->spi_fd >= 0) {
        close(dev->spi_fd);
        dev->spi_fd = -1;
    }
}

/* Send ACTIVATE 0x73 to toggle the features (needed on many chips). */
static int nrf24_toggle_features(nrf24_t *dev)
{
    uint8_t buf[2] = { NRF24_CMD_ACTIVATE, 0x73 };
    return spi_transfer(dev->spi_fd, buf, NULL, 2);
}

/* Turn on Dynamic Payload Length on pipe 0. */
static int nrf24_enable_dynamic_payloads(nrf24_t *dev)
{
    uint8_t feature;

    /* Try to set EN_DPL bit */
    if (nrf24_read_reg(dev, NRF24_REG_FEATURE, &feature) < 0)
        return -1;

    feature |= NRF24_FEATURE_EN_DPL;
    if (nrf24_write_reg(dev, NRF24_REG_FEATURE, feature) < 0)
        return -1;

    /* Read back; if still zero, features are disabled → need ACTIVATE */
    if (nrf24_read_reg(dev, NRF24_REG_FEATURE, &feature) < 0)
        return -1;

    if (!(feature & NRF24_FEATURE_EN_DPL)) {
        /* Enable feature register */
        if (nrf24_toggle_features(dev) < 0)
            return -1;

        if (nrf24_read_reg(dev, NRF24_REG_FEATURE, &feature) < 0)
            return -1;

        feature |= NRF24_FEATURE_EN_DPL;
        if (nrf24_write_reg(dev, NRF24_REG_FEATURE, feature) < 0)
            return -1;
    }

    /* Enable DPL on pipe 0 in DYNPD */
    uint8_t dynpd;
    if (nrf24_read_reg(dev, NRF24_REG_DYNPD, &dynpd) < 0)
        return -1;

    dynpd |= NRF24_DYNPD_DPL_P0;
    if (nrf24_write_reg(dev, NRF24_REG_DYNPD, dynpd) < 0)
        return -1;

    return 0;
}

/* --------- radio configuration for quick_mode --------- */

int nrf24_configure_quick(nrf24_t *dev, uint8_t channel)
{
    /* 5-byte addresses */
    if (nrf24_write_reg(dev, NRF24_REG_SETUP_AW, 0x03) < 0)
        return -1;

    /* auto-ack on pipe 0 */
    if (nrf24_write_reg(dev, NRF24_REG_EN_AA, 0x01) < 0)
        return -1;

    /* enable data pipe 0 only */
    if (nrf24_write_reg(dev, NRF24_REG_EN_RXADDR, 0x01) < 0)
        return -1;

    /* ARD=0 (250us), ARC=15 -> 0x0F.
    * 250us is fine since we don't use ACK payloads.
    */
    if (nrf24_write_reg(dev, NRF24_REG_SETUP_RETR, 0x0F) < 0)
        return -1;


    /* RF channel */
    if (nrf24_write_reg(dev, NRF24_REG_RF_CH, (uint8_t)(channel & 0x7F)) < 0)
        return -1;

    /* 2 Mbps, 0 dBm */
    if (nrf24_write_reg(dev, NRF24_REG_RF_SETUP,
                        NRF24_RF_SETUP_RF_DR_HIGH |
                        NRF24_RF_SETUP_RF_PWR_0DBM) < 0)
        return -1;

    /* RX_PW_P0 is ignored in DPL mode, but set to max anyway */
    if (nrf24_write_reg(dev, NRF24_REG_RX_PW_P0, NRF24_MAX_PAYLOAD_SIZE) < 0)
        return -1;

    /* Disable any old DPL/feature config first */
    if (nrf24_write_reg(dev, NRF24_REG_DYNPD, 0x00) < 0)
        return -1;
    if (nrf24_write_reg(dev, NRF24_REG_FEATURE, 0x00) < 0)
        return -1;

    /* Same address for TX and RX pipe 0 */
    if (nrf24_set_address(dev, NRF24_REG_RX_ADDR_P0, QUICK_ADDR) < 0)
        return -1;
    if (nrf24_set_address(dev, NRF24_REG_TX_ADDR, QUICK_ADDR) < 0)
        return -1;

    /* Clear interrupts and FIFOs */
    if (nrf24_clear_interrupts(dev) < 0)
        return -1;
    if (nrf24_flush_rx(dev) < 0)
        return -1;
    if (nrf24_flush_tx(dev) < 0)
        return -1;

    /* *** KEY STEP: turn on dynamic payloads *** */
    if (nrf24_enable_dynamic_payloads(dev) < 0)
        return -1;

    return 0;
}


int nrf24_set_mode_rx(nrf24_t *dev)
{
    if (ce_set(dev, 0) < 0)
        return -1;

    uint8_t cfg;
    if (nrf24_read_reg(dev, NRF24_REG_CONFIG, &cfg) < 0)
        return -1;

    cfg |= NRF24_CONFIG_PWR_UP | NRF24_CONFIG_EN_CRC | NRF24_CONFIG_PRIM_RX;

    if (nrf24_write_reg(dev, NRF24_REG_CONFIG, cfg) < 0)
        return -1;

    sleep_ms(2); /* tpd2stby ~1.5ms */

    if (nrf24_clear_interrupts(dev) < 0)
        return -1;
    if (nrf24_flush_rx(dev) < 0)
        return -1;

    return ce_set(dev, 1);  /* start listening */
}

int nrf24_set_mode_tx(nrf24_t *dev)
{
    /*
    if (ce_set(dev, 0) < 0)
        return -1;
    */
   /* Enter TX mode and keep CE high so FIFO drives transmissions */
    if (ce_set(dev, 1) < 0)
        return -1;

   

    uint8_t cfg;
    if (nrf24_read_reg(dev, NRF24_REG_CONFIG, &cfg) < 0)
        return -1;

    cfg |= NRF24_CONFIG_PWR_UP | NRF24_CONFIG_EN_CRC;
    cfg &= ~NRF24_CONFIG_PRIM_RX;

    if (nrf24_write_reg(dev, NRF24_REG_CONFIG, cfg) < 0)
        return -1;

    sleep_ms(2);

    if (nrf24_clear_interrupts(dev) < 0)
        return -1;
    if (nrf24_flush_tx(dev) < 0)
        return -1;

    return 0;
}

/* --------- blocking send / recv --------- */

int nrf24_send_blocking(nrf24_t *dev,
                        const void *payload, uint8_t length,
                        unsigned int timeout_ms)
{
    if (!dev || !payload) {
        errno = EINVAL;
        return -1;
    }

    if (length == 0) {
        errno = EINVAL;
        return -1;
    }

    if (length > NRF24_MAX_PAYLOAD_SIZE)
        length = NRF24_MAX_PAYLOAD_SIZE;

    /* Write payload to TX FIFO (DPL, so we send exactly <length> bytes) */
    if (nrf24_write_buf(dev,
                        NRF24_CMD_W_TX_PAYLOAD,
                        (const uint8_t *)payload,
                        length) < 0) {
        return -1;
    }

    /* Short CE pulse: spec says >10us; we give it 20us */
    /*
    if (ce_set(dev, 1) < 0)
        return -1;
    sleep_us(20);
    if (ce_set(dev, 0) < 0)
        return -1;
    */

    /* Poll STATUS with microsecond sleeps instead of milliseconds */
    const unsigned int step_us    = 50;  /* poll every 50us */
    const unsigned int timeout_us = timeout_ms ? timeout_ms * 1000U : 0;

    unsigned int elapsed_us = 0;

    while (1) {
        uint8_t status;
        if (nrf24_read_reg(dev, NRF24_REG_STATUS, &status) < 0)
            return -1;

        if (status & (NRF24_STATUS_TX_DS | NRF24_STATUS_MAX_RT)) {
            /* clear both flags */
            if (nrf24_write_reg(dev, NRF24_REG_STATUS,
                                NRF24_STATUS_TX_DS | NRF24_STATUS_MAX_RT) < 0)
                return -1;

            if (status & NRF24_STATUS_MAX_RT) {
                (void)nrf24_flush_tx(dev);
                errno = ETIMEDOUT;
                return -1;
            }

            return 0;  /* success */
        }

        if (timeout_us && elapsed_us >= timeout_us) {
            errno = ETIMEDOUT;
            return -1;
        }

        sleep_us(step_us);
        elapsed_us += step_us;
    }
}



int nrf24_recv_blocking(nrf24_t *dev,
                        void *payload, uint8_t *length_inout,
                        unsigned int timeout_ms)
{
    if (!dev || !payload || !length_inout) {
        errno = EINVAL;
        return -1;
    }

    if (*length_inout == 0) {
        errno = EINVAL;
        return -1;
    }

    const unsigned int step_us    = 50;
    const unsigned int timeout_us = timeout_ms ? timeout_ms * 1000U : 0;
    unsigned int       elapsed_us = 0;

    while (1) {
        uint8_t status;
        if (nrf24_read_reg(dev, NRF24_REG_STATUS, &status) < 0)
            return -1;

        if (status & NRF24_STATUS_RX_DR) {
            uint8_t width = 0;

            /* DPL: read payload width first */
            if (nrf24_read_buf(dev,
                               NRF24_CMD_R_RX_PL_WID,
                               &width,
                               1) < 0)
                return -1;

            if (width == 0 || width > NRF24_MAX_PAYLOAD_SIZE) {
                (void)nrf24_flush_rx(dev);
                (void)nrf24_write_reg(dev, NRF24_REG_STATUS,
                                      NRF24_STATUS_RX_DR);
                errno = EIO;
                return -1;
            }

            if (width > *length_inout)
                width = *length_inout;

            if (nrf24_read_buf(dev,
                               NRF24_CMD_R_RX_PAYLOAD,
                               (uint8_t *)payload,
                               width) < 0)
                return -1;

            *length_inout = width;

            if (nrf24_write_reg(dev, NRF24_REG_STATUS,
                                NRF24_STATUS_RX_DR) < 0)
                return -1;

            return 0;
        }

        if (timeout_us && elapsed_us >= timeout_us) {
            errno = ETIMEDOUT;
            return -1;
        }

        sleep_us(step_us);
        elapsed_us += step_us;
    }
}

