#ifndef LINK_LAYER_H
#define LINK_LAYER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes returned by the Link layer */
typedef enum {
    LINK_STATUS_OK          = 0,
    LINK_STATUS_INVALID_ARG = -1,
    LINK_STATUS_IO_ERROR    = -2
} link_status_t;

/*
 * Abstract radio interface used by the Link layer.
 *
 * This is deliberately generic so that:
 *  - On the Raspberry Pi you can plug this into your nRF24 driver.
 *  - On Windows you can plug in a fake radio for tests.
 */
typedef struct link_radio_iface {
    void *user_ctx;

    /* TX-related callbacks */

    /*
     * send(ctx, payload, len)
     *  - Should initiate sending 'len' bytes from 'payload'.
     *  - Return 0 on success; non-zero on fatal error.
     */
    int  (*send)(void *ctx, const uint8_t *payload, size_t len);

    /*
     * wait_until_sent(ctx)
     *  - Should block until the previous send operation has finished
     *    (either ACKed or considered failed).
     *  - Return 0 on success; non-zero on timeout or error.
     *
     * IMPORTANT:
     *  - The Link layer treats ANY non-zero return as "retryable":
     *      - it will WARN and retry sending the frame.
     *  - If you want a truly fatal condition, you should instead make
     *    send() return non-zero, which will cause link_send_frame()
     *    to return LINK_STATUS_IO_ERROR.
     */
    int  (*wait_until_sent)(void *ctx);

    /*
     * reset_packages_lost(ctx)
     *  - Should reset the internal lost-packet counter in the radio.
     */
    void (*reset_packages_lost)(void *ctx);

    /*
     * get_packages_lost(ctx)
     *  - Should return the number of lost packets reported by the radio
     *    for the last send.
     *  - The Link layer will retry if this value is > 0.
     */
    int  (*get_packages_lost)(void *ctx);

    /* RX-related callbacks */

    /*
     * data_ready(ctx)
     *  - Should return:
     *      0  if no frame is ready yet
     *      >0 if at least one frame is ready
     *      <0 on fatal error
     */
    int  (*data_ready)(void *ctx);

    /*
     * read_payload(ctx, buf, *inout_len)
     *  - On entry, *inout_len is the size of 'buf' in bytes.
     *  - On success:
     *      - copy up to *inout_len bytes into 'buf'
     *      - set *inout_len to the actual number of bytes copied
     *      - return 0
     *  - On error:
     *      - return non-zero (Link layer will treat it as fatal)
     */
    int  (*read_payload)(void *ctx, uint8_t *buf, size_t *inout_len);

} link_radio_iface_t;

/*
 * Blocking function that sends a single frame of bytes using the given
 * radio interface until the ACK is effectively received (no lost packets).
 *
 * Semantics:
 *  - If send() fails     -> logs ERROR and returns LINK_STATUS_IO_ERROR.
 *  - If wait_until_sent() returns non-zero -> logs WARN and retries.
 *  - If get_packages_lost() > 0           -> logs WARN and retries.
 *  - Otherwise                            -> returns LINK_STATUS_OK.
 */
link_status_t link_send_frame(const link_radio_iface_t *iface,
                              const uint8_t *frame,
                              size_t len);

/*
 * Blocking function that waits until a frame is received using the given
 * radio interface and stores it into 'buf'.
 *
 * On entry:
 *  - *inout_len is the size of 'buf' in bytes (capacity).
 *
 * On success:
 *  - fills 'buf' with the received bytes,
 *  - sets *inout_len to the number of bytes received,
 *  - returns LINK_STATUS_OK.
 *
 * On error:
 *  - returns LINK_STATUS_INVALID_ARG or LINK_STATUS_IO_ERROR.
 *
 * Semantics:
 *  - Busy-waits while data_ready() == 0 (like the Python version).
 *  - If data_ready() < 0 -> logs ERROR and returns LINK_STATUS_IO_ERROR.
 *  - If read_payload() != 0 -> logs ERROR and returns LINK_STATUS_IO_ERROR.
 */
link_status_t link_read_frame(const link_radio_iface_t *iface,
                              uint8_t *buf,
                              size_t *inout_len);

#ifdef __cplusplus
}
#endif

#endif /* LINK_LAYER_H */
