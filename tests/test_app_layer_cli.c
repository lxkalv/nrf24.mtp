#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "app_layer.h"

int main(int argc, char **argv)
{
    /* Init logger first so parse errors also appear nicely. */
    if (logger_init("test_app_layer_cli.log") != 0) {
        fprintf(stderr, "Failed to initialise logger.\n");
        return 1;
    }

    /* Check for --help before any parsing. */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) {
            app_print_usage(argv[0]);
            logger_close();
            return 0;
        }
    }

    app_config_t cfg;
    if (app_parse_arguments(argc, argv, &cfg) != 0) {
        logger_error("Argument parsing failed.");
        app_print_usage(argv[0]);
        logger_close();
        return 1;
    }

    if (cfg.print_config) {
        app_print_config(&cfg);
    }

    /* Simulate how the real app will behave, at least for file I/O. */
    if (cfg.mode == APP_MODE_TX) {
        if (!cfg.file_path_tx) {
            logger_warn("TX mode selected but no --file-path-tx given.");
        } else {
            uint8_t *data = NULL;
            size_t   len  = 0;
            if (app_load_file_bytes(cfg.file_path_tx, &data, &len) != 0) {
                logger_error("Failed to load TX file '%s'", cfg.file_path_tx);
                logger_close();
                return 1;
            }
            logger_succ("TX test: loaded %zu bytes from '%s'", len, cfg.file_path_tx);
            free(data);
        }
    } else if (cfg.mode == APP_MODE_RX) {
        const char dummy[] = "Dummy RX payload for Application Layer test.\n";
        if (app_store_file_bytes(cfg.file_path_rx,
                                 (const uint8_t *)dummy,
                                 sizeof(dummy) - 1) != 0) {
            logger_error("RX test: failed to store dummy data");
            logger_close();
            return 1;
        }
        logger_succ("RX test: dummy payload stored successfully.");
    }

    logger_succ("Application Layer CLI-style test completed.");
    logger_close();
    return 0;
}
