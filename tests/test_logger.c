#include <stdio.h>
#include "../libs/logger.h"

int main(void)
{
    /* Try to open a log file; not fatal if it fails */
    if (logger_init("test_logger.log") != 0) {
        fprintf(stderr,
                "[MAIN ]: logger_init() could not open log file, "
                "continuing with console-only logging.\n");
    }

    char ts[32];
    logger_timestamp(ts, sizeof(ts));

    logger_info("%s Starting logger test...", ts);
    logger_warn("This is a warning with code %d", 123);
    logger_error("Something went wrong: error code = %d", -42);
    logger_succ("All tests finished successfully!");

    logger_close();
    return 0;
}