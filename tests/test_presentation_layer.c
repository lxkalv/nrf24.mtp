#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "presentation_layer.h"

/* Create some test data with a repeated text pattern. */
static uint8_t *make_test_data(size_t *out_len)
{
    const char *text =
        "En un lugar de la Mancha, de cuyo nombre no quiero acordarme...\n";
    size_t text_len = strlen(text);

    size_t repeat = 200; /* ~12–13 KiB */
    size_t total  = text_len * repeat;

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        return NULL;
    }

    for (size_t i = 0; i < repeat; ++i) {
        memcpy(buf + i * text_len, text, text_len);
    }

    *out_len = total;
    return buf;
}

int main(void)
{
    if (logger_init("test_presentation_layer.log") != 0) {
        fprintf(stderr, "Failed to init logger\n");
        return 1;
    }

    logger_info("Starting Presentation Layer test...");

    size_t original_len = 0;
    uint8_t *original = make_test_data(&original_len);
    if (!original) {
        logger_error("Could not allocate test data");
        logger_close();
        return 1;
    }

    logger_info("Original length: %zu bytes", original_len);

    /* 1) Split into default pages. */
    pres_page_t *pages = NULL;
    size_t page_count = 0;
    if (pres_split_into_pages_default(original, original_len,
                                      &pages, &page_count) != 0) {
        logger_error("pres_split_into_pages_default failed");
        free(original);
        logger_close();
        return 1;
    }

    logger_info("Split into %zu pages", page_count);

    /* 2) Compress pages. */
    pres_page_t *compressed = NULL;
    size_t compressed_count = 0;
    if (pres_compress_pages(pages, page_count,
                            &compressed, &compressed_count) != 0) {
        logger_error("pres_compress_pages failed");
        pres_free_pages(pages, page_count);
        free(original);
        logger_close();
        return 1;
    }

    for (size_t i = 0; i < compressed_count; i++) {
        logger_info("Page %i with size %zu", i, (&compressed[i])->size);
    }

    /* 3) Decompress pages. */
    pres_page_t *decompressed = NULL;
    size_t decompressed_count = 0;
    if (pres_decompress_pages(compressed, compressed_count,
                              &decompressed, &decompressed_count) != 0) {
        logger_error("pres_decompress_pages failed");
        pres_free_pages(compressed, compressed_count);
        pres_free_pages(pages, page_count);
        free(original);
        logger_close();
        return 1;
    }

    /* 4) Merge decompressed pages. */
    uint8_t *merged = NULL;
    size_t merged_len = 0;
    if (pres_merge_pages(decompressed, decompressed_count,
                         &merged, &merged_len) != 0) {
        logger_error("pres_merge_pages failed");
        pres_free_pages(decompressed, decompressed_count);
        pres_free_pages(compressed, compressed_count);
        pres_free_pages(pages, page_count);
        free(original);
        logger_close();
        return 1;
    }

    /* 5) Compare with original. */
    if (merged_len != original_len ||
        memcmp(original, merged, original_len) != 0) {
        logger_error("Presentation Layer round-trip FAILED "
                     "(len %zu vs %zu or data mismatch)",
                     merged_len, original_len);
    } else {
        logger_succ("Presentation Layer round-trip OK: %zu bytes", merged_len);
    }

    /* Cleanup */
    free(merged);
    pres_free_pages(decompressed, decompressed_count);
    pres_free_pages(compressed, compressed_count);
    pres_free_pages(pages, page_count);
    free(original);
    logger_close();

    return 0;
}
