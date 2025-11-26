#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "logger.h"
#include "link_layer.h"

/* ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */
/* Fake radio implementation (for tests only)                               */
/* ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */

typedef struct {
    uint8_t  buffer[64];
    size_t   buffer_len;
    int      has_frame;
    int      packages_lost;
} fake_radio_t;

static int fake_send(void *ctx, const uint8_t *payload, size_t len)
{
    fake_radio_t *r = (fake_radio_t *)ctx;

    if (len > sizeof(r->buffer)) {
        /* Simulate a fatal error if the frame is too large */
        return -1;
    }

    memcpy(r->buffer, payload, len);
    r->buffer_len = len;
    r->has_frame  = 1;

    return 0;
}

static int fake_wait_until_sent(void *ctx)
{
    /* In this simple fake radio, sending always "completes" immediately. */
    (void)ctx;
    return 0;
}

static void fake_reset_packages_lost(void *ctx)
{
    fake_radio_t *r = (fake_radio_t *)ctx;
    r->packages_lost = 0;
}

static int fake_get_packages_lost(void *ctx)
{
    fake_radio_t *r = (fake_radio_t *)ctx;
    return r->packages_lost;
}

static int fake_data_ready(void *ctx)
{
    fake_radio_t *r = (fake_radio_t *)ctx;
    return r->has_frame ? 1 : 0;
}

static int fake_read_payload(void *ctx, uint8_t *buf, size_t *inout_len)
{
    fake_radio_t *r = (fake_radio_t *)ctx;

    if (!r->has_frame) {
        return -1;
    }

    size_t to_copy = r->buffer_len;
    if (to_copy > *inout_len) {
        to_copy = *inout_len;
    }

    memcpy(buf, r->buffer, to_copy);
    *inout_len = to_copy;

    r->has_frame  = 0;
    r->buffer_len = 0;

    return 0;
}

/* ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */
/* Main test                                                                */
/* ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */

int main(void)
{
    if (logger_init("test_link_layer.log") != 0) {
        logger_warn("Could not open log file 'test_link_layer.log', continuing without file log");
    }

    logger_info("Starting Link layer test...");

    fake_radio_t radio;
    memset(&radio, 0, sizeof(radio));

    link_radio_iface_t iface = {
        .user_ctx            = &radio,
        .send                = fake_send,
        .wait_until_sent     = fake_wait_until_sent,
        .reset_packages_lost = fake_reset_packages_lost,
        .get_packages_lost   = fake_get_packages_lost,
        .data_ready          = fake_data_ready,
        .read_payload        = fake_read_payload
    };

    const uint8_t tx_frame[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    size_t tx_len = sizeof(tx_frame);

    logger_info("Sending test frame via link_send_frame()...");
    if (link_send_frame(&iface, tx_frame, tx_len) != LINK_STATUS_OK) {
        logger_error("link_send_frame() failed in test");
        logger_close();
        return 1;
    }

    logger_info("Frame reported as sent, now reading it back via link_read_frame()...");

    uint8_t rx_buf[16];
    size_t  rx_len = sizeof(rx_buf);

    if (link_read_frame(&iface, rx_buf, &rx_len) != LINK_STATUS_OK) {
        logger_error("link_read_frame() failed in test");
        logger_close();
        return 1;
    }

    if (rx_len != tx_len || memcmp(tx_frame, rx_buf, tx_len) != 0) {
        logger_error("Received frame does not match sent frame (rx_len=%zu, tx_len=%zu)",
                     rx_len, tx_len);
        logger_close();
        return 1;
    }

    logger_succ("Link layer test passed: %zu bytes round-trip OK", tx_len);

    logger_close();
    return 0;
}
