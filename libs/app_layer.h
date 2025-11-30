#ifndef APP_LAYER_H
#define APP_LAYER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Application mode: Transmitter or Receiver */
typedef enum {
    APP_MODE_UNSET = 0,
    APP_MODE_TX,
    APP_MODE_RX
} app_mode_t;

/* Data rate options (match Python strings 250KBPS / 1MBPS / 2MBPS) */
typedef enum {
    APP_DATA_RATE_250KBPS = 0,
    APP_DATA_RATE_1MBPS,
    APP_DATA_RATE_2MBPS
} app_data_rate_t;

/* PA (power amplifier) level */
typedef enum {
    APP_PA_MIN = 0,
    APP_PA_LOW,
    APP_PA_HIGH,
    APP_PA_MAX
} app_pa_level_t;

/* CRC configuration: number of CRC bytes */
typedef enum {
    APP_CRC_OFF = 0,
    APP_CRC_8   = 1,
    APP_CRC_16  = 2
} app_crc_bytes_t;

/* High-level configuration collected from the CLI / user. */
typedef struct {
    app_mode_t      mode;                  /* TX or RX */

    const char     *file_path_tx;          /* Path to file to transmit (may be NULL) */
    const char     *file_path_rx;          /* Path to store received bytes (may be NULL) */

    int             ce_pin;                /* 0..31 (GPIO number for CE) */
    int             channel;               /* 0..125 (RF channel) */
    app_data_rate_t data_rate;             /* Data rate selection */
    app_pa_level_t  pa_level;              /* Power amplifier level */
    app_crc_bytes_t crc_bytes;             /* 0..2 bytes of CRC */
    int             retransmission_tries;  /* 0..15 */
    int             retransmission_delay;  /* 0..15 */

    bool            autostart;             /* If true, start transfer immediately */
    bool            print_config;          /* If true, print config before starting */
    bool            verify_config;         /* If true, dump radio config after setup */
} app_config_t;

/* Fill cfg with default configuration values. */
void app_set_default_config(app_config_t *cfg);

/* Parse CLI arguments into an app_config_t.
 *
 * Supported options (all double-dash):
 *   --mode TX|RX                    (required)
 *   --file-path-tx PATH             (optional)
 *   --file-path-rx PATH             (optional)
 *   --ce-pin N                      (0..31, default 22)
 *   --channel N                     (0..125, default 76)
 *   --data-rate 250KBPS|1MBPS|2MBPS (default 1MBPS)
 *   --pa-level MIN|LOW|HIGH|MAX     (default MIN)
 *   --crc-bytes 0|1|2               (default 2)
 *   --retransmission-tries N        (0..15, default 15)
 *   --retransmission-delay N        (0..15, default 2)
 *   --autostart                     (flag)
 *   --print-config                  (flag)
 *   --verify-config                 (flag)
 *
 * Returns 0 on success, -1 on error.
 * On error, cfg contents are undefined.
 */
int app_parse_arguments(int argc, char **argv, app_config_t *cfg);

/* Print the configuration using logger_info(). */
void app_print_config(const app_config_t *cfg);

/* Print usage / help text to stderr. */
void app_print_usage(const char *prog_name);

/* Convenience string helpers for human-readable printing. */
const char *app_mode_str(app_mode_t m);
const char *app_data_rate_str(app_data_rate_t r);
const char *app_pa_level_str(app_pa_level_t p);
const char *app_crc_bytes_str(app_crc_bytes_t c);

/* File I/O helpers ------------------------------------------------------- */

/* Load an entire file into memory.
 *
 * If file_path is NULL, this function tries (in order):
 *   1) to find a valid .txt file on a USB mount (currently unimplemented → skipped),
 *   2) to load "test_files/quijote.txt".
 *
 * On success:
 *   - *out_data is malloc()'d and must be freed by the caller,
 *   - *out_len holds the number of bytes.
 * Returns 0 on success, non-zero on failure.
 */
int app_load_file_bytes(const char *file_path,
                        uint8_t **out_data,
                        size_t  *out_len);

/* Store bytes into a file.
 *
 * If output_path is non-NULL, the data is written exactly to that path
 * (overwriting if it already exists).
 *
 * If output_path is NULL, this function:
 *   1) attempts to use a USB mount directory (currently unimplemented → skipped),
 *   2) otherwise uses "received_files/<timestamp>.txt".
 *
 * Returns 0 on success, non-zero on failure.
 */
int app_store_file_bytes(const char *output_path,
                         const uint8_t *data,
                         size_t len);

#endif /* APP_LAYER_H */
