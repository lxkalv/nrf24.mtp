#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "logger.h"
#include "app_layer.h"

/* Simple helper to create a small test input file. */
static int create_test_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        logger_error("Failed to create test file %s", path);
        return -1;
    }
    const char *msg = "Hello from Application Layer test!\n";
    size_t len = strlen(msg);
    if (fwrite(msg, 1, len, f) != len) {
        logger_error("Failed to write to test file %s", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

int main(void)
{
    if (logger_init("test_app_layer.log") != 0) {
        fprintf(stderr, "Failed to initialise logger. Exiting.\n");
        return 1;
    }

    logger_info("Starting Application Layer test...");

    const char *input_path  = "tests/test_input.txt";
    const char *output_path = "tests/test_output.txt";

    /* Ensure tests directory exists (best-effort). */
#if defined(_WIN32)
    _mkdir("tests");
#else
    mkdir("tests", 0777);
#endif

    if (create_test_file(input_path) != 0) {
        logger_error("Could not create input test file");
        logger_close();
        return 1;
    }

    /* Build a fake argv to exercise the parser. */
    char *argv[] = {
        "test_app_layer",
        "--mode", "TX",
        "--file-path-tx", (char *)input_path,
        "--file-path-rx", (char *)output_path,
        "--ce-pin", "22",
        "--channel", "76",
        "--data-rate", "2MBPS",
        "--pa-level", "MIN",
        "--crc-bytes", "2",
        "--retransmission-tries", "15",
        "--retransmission-delay", "2",
        "--autostart",
        "--print-config"
    };
    int argc = (int)(sizeof(argv) / sizeof(argv[0]));

    app_config_t cfg;
    if (app_parse_arguments(argc, argv, &cfg) != 0) {
        logger_error("app_parse_arguments failed");
        logger_close();
        return 1;
    }

    if (cfg.print_config) {
        app_print_config(&cfg);
    }

    /* Load bytes from the input file. */
    uint8_t *data = NULL;
    size_t   len  = 0;
    if (app_load_file_bytes(cfg.file_path_tx, &data, &len) != 0) {
        logger_error("app_load_file_bytes failed");
        logger_close();
        return 1;
    }

    logger_info("Loaded %zu bytes from %s", len, cfg.file_path_tx);

    /* Store them into the output path. */
    if (app_store_file_bytes(cfg.file_path_rx, data, len) != 0) {
        logger_error("app_store_file_bytes failed");
        free(data);
        logger_close();
        return 1;
    }

    free(data);

    logger_succ("Application Layer test finished successfully.");
    logger_close();
    return 0;
}
