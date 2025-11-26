#include "app_layer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "logger.h"

/* ------------------------------------------------------------------------- */
/* String helpers                                                            */
/* ------------------------------------------------------------------------- */

const char *app_mode_str(app_mode_t m)
{
    switch (m) {
    case APP_MODE_TX:   return "TX";
    case APP_MODE_RX:   return "RX";
    default:            return "UNSET";
    }
}

const char *app_data_rate_str(app_data_rate_t r)
{
    switch (r) {
    case APP_DATA_RATE_250KBPS: return "250KBPS";
    case APP_DATA_RATE_1MBPS:   return "1MBPS";
    case APP_DATA_RATE_2MBPS:   return "2MBPS";
    default:                    return "UNKNOWN";
    }
}

const char *app_pa_level_str(app_pa_level_t p)
{
    switch (p) {
    case APP_PA_MIN: return "MIN";
    case APP_PA_LOW: return "LOW";
    case APP_PA_HIGH:return "HIGH";
    case APP_PA_MAX: return "MAX";
    default:         return "UNKNOWN";
    }
}

const char *app_crc_bytes_str(app_crc_bytes_t c)
{
    switch (c) {
    case APP_CRC_OFF: return "OFF";
    case APP_CRC_8:   return "1-byte";
    case APP_CRC_16:  return "2-byte";
    default:          return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------------- */
/* Default configuration                                                     */
/* ------------------------------------------------------------------------- */

void app_set_default_config(app_config_t *cfg)
{
    if (!cfg) return;

    cfg->mode       = APP_MODE_UNSET;

    cfg->file_path_tx = NULL;
    cfg->file_path_rx = NULL;

    cfg->ce_pin     = 22;          /* same default as Python */
    cfg->channel    = 76;          /* our usual channel */
    cfg->data_rate  = APP_DATA_RATE_1MBPS;
    cfg->pa_level   = APP_PA_MIN;
    cfg->crc_bytes  = APP_CRC_16;
    cfg->retransmission_tries  = 15;
    cfg->retransmission_delay  = 2;

    cfg->autostart  = false;
    cfg->print_config = false;
}

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static int app_parse_int(const char *opt, const char *val, int *out)
{
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (val == NULL || *val == '\0' || end == val || *end != '\0') {
        logger_error("Invalid integer for %s: '%s'", opt, val ? val : "(null)");
        return -1;
    }
    /* We intentionally do not clamp here; callers check narrower ranges. */
    *out = (int)v;
    return 0;
}

static int app_strcasecmp(const char *a, const char *b)
{
    if (!a || !b) return (a == b) ? 0 : (a ? 1 : -1);
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (*a - 'A' + 'a') : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (*b - 'A' + 'a') : *b;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        ++a; ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* ------------------------------------------------------------------------- */
/* CLI parsing                                                               */
/* ------------------------------------------------------------------------- */

int app_parse_arguments(int argc, char **argv, app_config_t *cfg)
{
    if (!cfg) {
        logger_error("app_parse_arguments: cfg is NULL");
        return -1;
    }

    app_set_default_config(cfg);

    /* Simple manual parsing of --key value options */
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];

        if (strcmp(arg, "--mode") == 0) {
            if (i + 1 >= argc) {
                logger_error("--mode requires an argument (TX or RX)");
                return -1;
            }
            const char *val = argv[++i];
            if (app_strcasecmp(val, "TX") == 0) {
                cfg->mode = APP_MODE_TX;
            } else if (app_strcasecmp(val, "RX") == 0) {
                cfg->mode = APP_MODE_RX;
            } else {
                logger_error("Invalid mode '%s'. Must be TX or RX", val);
                return -1;
            }
        }
        else if (strcmp(arg, "--file-path-tx") == 0) {
            if (i + 1 >= argc) {
                logger_error("--file-path-tx requires a path");
                return -1;
            }
            cfg->file_path_tx = argv[++i];
        }
        else if (strcmp(arg, "--file-path-rx") == 0) {
            if (i + 1 >= argc) {
                logger_error("--file-path-rx requires a path");
                return -1;
            }
            cfg->file_path_rx = argv[++i];
        }
        else if (strcmp(arg, "--ce-pin") == 0) {
            if (i + 1 >= argc) {
                logger_error("--ce-pin requires an integer value");
                return -1;
            }
            if (app_parse_int("--ce-pin", argv[++i], &cfg->ce_pin) != 0) {
                return -1;
            }
        }
        else if (strcmp(arg, "--channel") == 0) {
            if (i + 1 >= argc) {
                logger_error("--channel requires an integer value");
                return -1;
            }
            if (app_parse_int("--channel", argv[++i], &cfg->channel) != 0) {
                return -1;
            }
        }
        else if (strcmp(arg, "--data-rate") == 0) {
            if (i + 1 >= argc) {
                logger_error("--data-rate requires a value (250KBPS, 1MBPS, 2MBPS)");
                return -1;
            }
            const char *val = argv[++i];
            if (app_strcasecmp(val, "250KBPS") == 0) {
                cfg->data_rate = APP_DATA_RATE_250KBPS;
            } else if (app_strcasecmp(val, "1MBPS") == 0) {
                cfg->data_rate = APP_DATA_RATE_1MBPS;
            } else if (app_strcasecmp(val, "2MBPS") == 0) {
                cfg->data_rate = APP_DATA_RATE_2MBPS;
            } else {
                logger_error("Invalid data rate '%s'. Must be 250KBPS, 1MBPS or 2MBPS", val);
                return -1;
            }
        }
        else if (strcmp(arg, "--pa-level") == 0) {
            if (i + 1 >= argc) {
                logger_error("--pa-level requires a value (MIN, LOW, HIGH, MAX)");
                return -1;
            }
            const char *val = argv[++i];
            if (app_strcasecmp(val, "MIN") == 0) {
                cfg->pa_level = APP_PA_MIN;
            } else if (app_strcasecmp(val, "LOW") == 0) {
                cfg->pa_level = APP_PA_LOW;
            } else if (app_strcasecmp(val, "HIGH") == 0) {
                cfg->pa_level = APP_PA_HIGH;
            } else if (app_strcasecmp(val, "MAX") == 0) {
                cfg->pa_level = APP_PA_MAX;
            } else {
                logger_error("Invalid PA level '%s'. Must be MIN, LOW, HIGH or MAX", val);
                return -1;
            }
        }
        else if (strcmp(arg, "--crc-bytes") == 0) {
            if (i + 1 >= argc) {
                logger_error("--crc-bytes requires an integer (0..2)");
                return -1;
            }
            int v = 0;
            if (app_parse_int("--crc-bytes", argv[++i], &v) != 0) {
                return -1;
            }
            if (v < 0 || v > 2) {
                logger_error("Invalid crc-bytes %d. Must be 0..2", v);
                return -1;
            }
            if (v == 0)      cfg->crc_bytes = APP_CRC_OFF;
            else if (v == 1) cfg->crc_bytes = APP_CRC_8;
            else             cfg->crc_bytes = APP_CRC_16;
        }
        else if (strcmp(arg, "--retransmission-tries") == 0) {
            if (i + 1 >= argc) {
                logger_error("--retransmission-tries requires an integer (0..15)");
                return -1;
            }
            if (app_parse_int("--retransmission-tries", argv[++i],
                              &cfg->retransmission_tries) != 0) {
                return -1;
            }
        }
        else if (strcmp(arg, "--retransmission-delay") == 0) {
            if (i + 1 >= argc) {
                logger_error("--retransmission-delay requires an integer (0..15)");
                return -1;
            }
            if (app_parse_int("--retransmission-delay", argv[++i],
                              &cfg->retransmission_delay) != 0) {
                return -1;
            }
        }
        else if (strcmp(arg, "--autostart") == 0) {
            cfg->autostart = true;
        }
        else if (strcmp(arg, "--print-config") == 0) {
            cfg->print_config = true;
        }
        else if (strcmp(arg, "--help") == 0) {
            /* Let caller handle usage printing and exit code. */
            return -1;
        }
        else {
            logger_warn("Unknown option ignored: %s", arg);
        }
    }

    /* Validate ranges that depend on parsed values */
    if (cfg->mode == APP_MODE_UNSET) {
        logger_error("Missing required option: --mode (TX or RX)");
        return -1;
    }

    if (cfg->ce_pin < 0 || cfg->ce_pin > 31) {
        logger_error("Invalid CE pin %d. Must be in range 0..31", cfg->ce_pin);
        return -1;
    }

    if (cfg->channel < 0 || cfg->channel > 125) {
        logger_error("Invalid channel %d. Must be in range 0..125", cfg->channel);
        return -1;
    }

    if (cfg->retransmission_tries < 0 || cfg->retransmission_tries > 15) {
        logger_error("Invalid retransmission-tries %d. Must be 0..15",
                     cfg->retransmission_tries);
        return -1;
    }

    if (cfg->retransmission_delay < 0 || cfg->retransmission_delay > 15) {
        logger_error("Invalid retransmission-delay %d. Must be 0..15",
                     cfg->retransmission_delay);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Printing                                                                  */
/* ------------------------------------------------------------------------- */

void app_print_config(const app_config_t *cfg)
{
    if (!cfg) return;

    logger_info("Application configuration:");
    logger_info("  Mode:                 %s", app_mode_str(cfg->mode));
    logger_info("  File TX path:         %s",
                cfg->file_path_tx ? cfg->file_path_tx : "(none)");
    logger_info("  File RX path:         %s",
                cfg->file_path_rx ? cfg->file_path_rx : "(none)");
    logger_info("  CE pin:               %d", cfg->ce_pin);
    logger_info("  Channel:              %d", cfg->channel);
    logger_info("  Data rate:            %s", app_data_rate_str(cfg->data_rate));
    logger_info("  PA level:             %s", app_pa_level_str(cfg->pa_level));
    logger_info("  CRC bytes:            %s", app_crc_bytes_str(cfg->crc_bytes));
    logger_info("  Retransmission tries: %d", cfg->retransmission_tries);
    logger_info("  Retransmission delay: %d", cfg->retransmission_delay);
    logger_info("  Autostart:            %s", cfg->autostart ? "true" : "false");
    logger_info("  Print config:         %s", cfg->print_config ? "true" : "false");
}

void app_print_usage(const char *prog_name)
{
    const char *p = prog_name ? prog_name : "app";

    fprintf(stderr,
        "Usage:\n"
        "  %s --mode TX|RX [options]\n"
        "\n"
        "Options:\n"
        "  --mode TX|RX                    (required)\n"
        "  --file-path-tx PATH             File to transmit\n"
        "  --file-path-rx PATH             File to write received data\n"
        "                                  If omitted, a timestamped file in\n"
        "                                  'received_files/' will be used.\n"
        "  --ce-pin N                      GPIO for CE (0..31, default 22)\n"
        "  --channel N                     RF channel (0..125, default 76)\n"
        "  --data-rate 250KBPS|1MBPS|2MBPS (default 1MBPS)\n"
        "  --pa-level MIN|LOW|HIGH|MAX     PA level (default MIN)\n"
        "  --crc-bytes 0|1|2               CRC bytes (default 2)\n"
        "  --retransmission-tries N        Auto-retry count (0..15, default 15)\n"
        "  --retransmission-delay N        Auto-retry delay (0..15, default 2)\n"
        "  --autostart                     Start transmission automatically\n"
        "  --print-config                  Print parsed configuration\n"
        "  --help                          Show this help message\n"
        "\n",
        p);
}

/* ------------------------------------------------------------------------- */
/* File I/O helpers                                                          */
/* ------------------------------------------------------------------------- */

/* For now USB detection is not implemented (cross-platform issue). */
static char *app_get_usb_mount_path(void)
{
    (void)0; /* suppress unused warnings if any */
    return NULL;
}

int app_load_file_bytes(const char *file_path,
                        uint8_t **out_data,
                        size_t  *out_len)
{
    if (!out_data || !out_len) {
        logger_error("app_load_file_bytes: output pointers are NULL");
        return -1;
    }
    *out_data = NULL;
    *out_len  = 0;

    const char *path_to_use = file_path;

    if (!path_to_use || path_to_use[0] == '\0') {
        /* No explicit path: try USB, then fallback to test_files/quijote.txt */
        char *usb_path = app_get_usb_mount_path();
        if (usb_path) {
            path_to_use = usb_path;
        } else {
            path_to_use = "test_files/quijote.txt";
            logger_warn("USB file candidate not found, using fallback file: %s",
                        path_to_use);
        }
        /* usb_path, if allocated, would need free() here.
           Currently app_get_usb_mount_path() returns NULL. */
    }

    FILE *f = fopen(path_to_use, "rb");
    if (!f) {
        logger_error("File not found: %s (%s)", path_to_use, strerror(errno));
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        logger_error("fseek failed on file %s", path_to_use);
        fclose(f);
        return -1;
    }

    long sz = ftell(f);
    if (sz < 0) {
        logger_error("ftell failed on file %s", path_to_use);
        fclose(f);
        return -1;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        logger_error("fseek (rewind) failed on file %s", path_to_use);
        fclose(f);
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf && sz > 0) {
        logger_error("malloc failed while reading file %s", path_to_use);
        fclose(f);
        return -1;
    }

    size_t read_bytes = 0;
    if (sz > 0) {
        read_bytes = fread(buf, 1, (size_t)sz, f);
        if (read_bytes != (size_t)sz) {
            logger_error("fread failed on file %s (read %zu of %ld)",
                         path_to_use, read_bytes, sz);
            free(buf);
            fclose(f);
            return -1;
        }
    }

    fclose(f);

    *out_data = buf;
    *out_len  = read_bytes;

    logger_succ("Loaded %zu bytes from file: %s", read_bytes, path_to_use);
    return 0;
}

static int app_mkdir_if_needed(const char *dir)
{
    if (!dir || !dir[0]) return 0;

#if defined(_WIN32)
    if (_mkdir(dir) != 0 && errno != EEXIST) {
        return -1;
    }
#else
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        return -1;
    }
#endif
    return 0;
}

int app_store_file_bytes(const char *output_path,
                         const uint8_t *data,
                         size_t len)
{
    if (!data && len != 0) {
        logger_error("app_store_file_bytes: data is NULL but len=%zu", len);
        return -1;
    }

    char path_buf[512];

    const char *path_to_use = output_path;

    if (!path_to_use || path_to_use[0] == '\0') {
        /* Try USB mount (unimplemented) */
        char *usb_dir = app_get_usb_mount_path();
        if (usb_dir) {
            /* Compose usb_dir/<timestamp>.txt */
            char ts[32];
            logger_timestamp(ts, sizeof(ts));

            snprintf(path_buf, sizeof(path_buf), "%s/%s.txt", usb_dir, ts);
            path_to_use = path_buf;

            free(usb_dir); /* if app_get_usb_mount_path ever allocates */
        } else {
            /* Fallback to local received_files/<timestamp>.txt */
            if (app_mkdir_if_needed("received_files") != 0) {
                logger_warn("Could not create directory 'received_files' "
                            "(%s). Continuing anyway.", strerror(errno));
            }

            char ts[32];
            logger_timestamp(ts, sizeof(ts));
            snprintf(path_buf, sizeof(path_buf),
                     "received_files/%s.txt", ts);
            path_to_use = path_buf;
        }
    }

    FILE *f = fopen(path_to_use, "wb");
    if (!f) {
        logger_error("Failed to open output file %s: %s",
                     path_to_use, strerror(errno));
        return -1;
    }

    size_t written = 0;
    if (len > 0) {
        written = fwrite(data, 1, len, f);
    }

    if (written != len) {
        logger_error("Failed to write %zu bytes to %s (wrote %zu): %s",
                     len, path_to_use, written, strerror(errno));
        fclose(f);
        return -1;
    }

    if (fclose(f) != 0) {
        logger_warn("Error while closing file %s: %s",
                    path_to_use, strerror(errno));
    }

    logger_succ("Stored %zu bytes into file: %s", len, path_to_use);
    return 0;
}
