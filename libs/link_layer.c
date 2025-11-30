#include "link_layer.h"
#include "logger.h"

/* ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */
/* Internal helpers                                                         */
/* ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */

static int link_check_tx_iface(const link_radio_iface_t *iface)
{
    if (!iface) {
        return 0;
    }

    if (!iface->send ||
        !iface->wait_until_sent ||
        !iface->reset_packages_lost ||
        !iface->get_packages_lost) {
        return 0;
    }

    return 1;
}

static int link_check_rx_iface(const link_radio_iface_t *iface)
{
    if (!iface) {
        return 0;
    }

    if (!iface->data_ready ||
        !iface->read_payload) {
        return 0;
    }

    return 1;
}

/* ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */
/* Public API                                                               */
/* ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::: */

link_status_t link_send_frame(const link_radio_iface_t *iface,
                              const uint8_t *frame,
                              size_t len)
{
    if (!iface || !frame || len == 0) {
        logger_error("link_send_frame: invalid arguments (iface=%p, frame=%p, len=%zu)",
                     (void *)iface, (const void *)frame, len);
        return LINK_STATUS_INVALID_ARG;
    }

    if (!link_check_tx_iface(iface)) {
        logger_error("link_send_frame: radio interface missing TX callbacks");
        return LINK_STATUS_INVALID_ARG;
    }

    /* Behaviour mirrors the Python LinkLayer.send_frame logic:
     *
     *  while True:
     *      nrf.reset_packages_lost()
     *      nrf.send(frame)
     *      try:
     *          nrf.wait_until_sent()
     *      except TimeoutError:
     *          WARN("Frame sending timed out, retrying...")
     *          continue
     *
     *      if nrf.get_packages_lost() > 0:
     *          WARN("Frame sending failed, retrying...")
     *          continue
     *
     *      return
     */

    for (;;) {
        iface->reset_packages_lost(iface->user_ctx);

        if (iface->send(iface->user_ctx, frame, len) != 0) {
            /* Treat send() failures (timeouts, MAX_RT, etc.) as retryable. */
            logger_warn("link_send_frame: send() failed, retrying...");
            continue;
        }

        /* Any non-zero from wait_until_sent() is treated as retryable. */
        int wret = iface->wait_until_sent(iface->user_ctx);
        if (wret != 0) {
            logger_warn("link_send_frame: wait_until_sent() returned %d, retrying...", wret);
            continue;
        }

        int lost = iface->get_packages_lost(iface->user_ctx);
        if (lost > 0) {
            logger_warn("link_send_frame: %d lost packet(s) reported, retrying...", lost);
            continue;
        }

        /* Success: frame sent and ACKed with no lost packets. */
        return LINK_STATUS_OK;
    }
}

link_status_t link_read_frame(const link_radio_iface_t *iface,
                              uint8_t *buf,
                              size_t *inout_len)
{
    if (!iface || !buf || !inout_len || *inout_len == 0) {
        logger_error("link_read_frame: invalid arguments (iface=%p, buf=%p, len=%p)",
                     (void *)iface, (void *)buf, (void *)inout_len);
        return LINK_STATUS_INVALID_ARG;
    }

    if (!link_check_rx_iface(iface)) {
        logger_error("link_read_frame: radio interface missing RX callbacks");
        return LINK_STATUS_INVALID_ARG;
    }

    /* Behaviour mirrors LinkLayer.read_frame in Python:
     *
     *  while not nrf.data_ready():
     *      pass
     *  return nrf.get_payload()
     */
    for (;;) {
        for (;;) {
            int ready = iface->data_ready(iface->user_ctx);

            if (ready < 0) {
                logger_error("link_read_frame: data_ready() returned %d (fatal)", ready);
                return LINK_STATUS_IO_ERROR;
            }

            if (ready > 0) {
                break;
            }

            /* Busy-wait (like Python). If you want to be nicer to the CPU,
            * you can add a small sleep here in your own fork.
            */
        }

        if (iface->read_payload(iface->user_ctx, buf, inout_len) != 0) {
            /* Typically a timeout inside nrf24_recv_blocking: retry. */
            logger_warn("link_read_frame: read_payload() failed, retrying...");
            continue;
        }

        return LINK_STATUS_OK;
    }
}
