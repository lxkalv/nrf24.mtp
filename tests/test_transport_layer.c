#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
#include "transport_layer.h"

/* Create some test data with a repeated text pattern. */
static uint8_t *make_test_data(size_t *out_len)
{
    const char *text =
        "En un lugar de la Mancha, de cuyo nombre no quiero acordarme...\n";
    size_t text_len = strlen(text);

    size_t repeat = 500; /* ~30 KiB */
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
    if (logger_init("test_transport_layer.log") != 0) {
        fprintf(stderr, "Failed to initialise logger\n");
        return 1;
    }

    logger_info("=== Transport Layer test started ===");

    /* 1) Generate test page data. */
    size_t page_len = 0;
    uint8_t *page = make_test_data(&page_len);
    if (!page) {
        logger_error("Could not allocate test data");
        logger_close();
        return 1;
    }

    logger_info("Test page length: %zu bytes", page_len);

    /* 2) Split into bursts. */
    trans_burst_t *bursts = NULL;
    size_t burst_count = 0;

    if (trans_split_page_into_bursts(page, page_len,
                                     &bursts, &burst_count) != 0) {
        logger_error("trans_split_page_into_bursts failed");
        free(page);
        logger_close();
        return 1;
    }

    logger_info("Split into %zu bursts", burst_count);

    /* 3) For each burst: test BurstSize, BURST_INFO, layout and checksum. */
    for (size_t b = 0; b < burst_count; ++b) {
        const trans_burst_t *burst = &bursts[b];

        uint16_t burst_size = trans_compute_burst_size(burst);
        logger_info("Burst %zu: frame_count=%zu, BurstSize=%u bytes",
                    b, burst->frame_count, burst_size);

        /* Build+parse BURST_INFO. */
        uint8_t bi[6];
        trans_build_burst_info(0 /*page_id*/, (uint8_t)b, burst, bi);

        uint8_t page_id2 = 0, bid2 = 0;
        uint16_t burst_size2 = 0;
        if (trans_parse_burst_info(bi, sizeof(bi),
                                   &page_id2, &bid2, &burst_size2) != 0) {
            logger_error("Burst %zu: trans_parse_burst_info failed", b);
            trans_free_bursts(bursts, burst_count);
            free(page);
            logger_close();
            return 1;
        }

        logger_info("Burst %zu: parsed BURST_INFO -> PageID=%u, BurstID=%u, BurstSize=%u",
                    b, (unsigned)page_id2, (unsigned)bid2, (unsigned)burst_size2);

        if (burst_size2 != burst_size || bid2 != (uint8_t)b) {
            logger_error("Burst %zu: BURST_INFO round-trip mismatch (size %u vs %u, id %u vs %u)",
                         b, burst_size, burst_size2, (unsigned)bid2, (unsigned)b);
            trans_free_bursts(bursts, burst_count);
            free(page);
            logger_close();
            return 1;
        }

        /* Derive frame layout and compare lengths. */
        uint8_t layout_count = 0;
        uint8_t layout_lens[TRANS_MAX_FRAMES_PER_BURST];
        if (trans_derive_frame_layout(burst_size, &layout_count, layout_lens) != 0) {
            logger_error("Burst %zu: trans_derive_frame_layout failed", b);
            trans_free_bursts(bursts, burst_count);
            free(page);
            logger_close();
            return 1;
        }

        logger_info("Burst %zu: derived frame_count=%u from BurstSize",
                    b, (unsigned)layout_count);

        if ((size_t)layout_count != burst->frame_count) {
            logger_error("Burst %zu: frame_count mismatch (%zu vs %u)",
                         b, burst->frame_count, (unsigned)layout_count);
            trans_free_bursts(bursts, burst_count);
            free(page);
            logger_close();
            return 1;
        }

        /* Log a small summary of frame lengths (first few only). */
        size_t log_frames = burst->frame_count;
        if (log_frames > 8) {
            log_frames = 8; /* Avoid huge logs. */
        }

        for (size_t i = 0; i < log_frames; ++i) {
            logger_info("Burst %zu: frame %zu len=%u (layout=%u)",
                        b, i,
                        (unsigned)burst->frames[i].len,
                        (unsigned)layout_lens[i]);
        }
        if (burst->frame_count > log_frames) {
            logger_info("Burst %zu: ... %zu more frames not shown",
                        b, burst->frame_count - log_frames);
        }

        for (size_t i = 0; i < burst->frame_count; ++i) {
            if (burst->frames[i].len != layout_lens[i]) {
                logger_error("Burst %zu: frame %zu len mismatch (%u vs %u)",
                             b, i,
                             (unsigned)burst->frames[i].len,
                             (unsigned)layout_lens[i]);
                trans_free_bursts(bursts, burst_count);
                free(page);
                logger_close();
                return 1;
            }
        }

        /* Checksum test. */
        uint64_t chk = trans_compute_burst_checksum(burst);
        uint8_t chk_bytes[8];
        trans_checksum_to_bytes(chk, chk_bytes);
        uint64_t chk2 = trans_checksum_from_bytes(chk_bytes);

        logger_info("Burst %zu: checksum=0x%016llX",
                    b, (unsigned long long)chk);

        if (chk != chk2) {
            logger_error("Burst %zu: checksum encode/decode mismatch", b);
            trans_free_bursts(bursts, burst_count);
            free(page);
            logger_close();
            return 1;
        }
    }

    logger_succ("Burst framing and checksum tests passed");

    /* 4) Merge bursts back into a page and compare. */
    uint8_t *merged = NULL;
    size_t merged_len = 0;
    if (trans_merge_bursts_into_page(bursts, burst_count,
                                     &merged, &merged_len) != 0) {
        logger_error("trans_merge_bursts_into_page failed");
        trans_free_bursts(bursts, burst_count);
        free(page);
        logger_close();
        return 1;
    }

    logger_info("Merged page length: %zu bytes", merged_len);

    if (merged_len != page_len ||
        memcmp(page, merged, page_len) != 0) {
        logger_error("Merge round-trip FAILED (len %zu vs %zu or data mismatch)",
                     merged_len, page_len);
        free(merged);
        trans_free_bursts(bursts, burst_count);
        free(page);
        logger_close();
        return 1;
    } else {
        logger_succ("Merge round-trip OK: %zu bytes", merged_len);
    }

    /* 5) STREAM_INFO test. */
    uint32_t total_orig = (uint32_t)page_len;
    uint32_t comp_sizes[TRANS_NUM_PAGES] = {0};
    uint32_t parsed_comp[TRANS_NUM_PAGES] = {0};

    /* Fake some per-page compressed sizes that sum roughly to page_len. */
    uint32_t base = (uint32_t)(page_len / TRANS_NUM_PAGES);
    uint32_t rem  = (uint32_t)(page_len % TRANS_NUM_PAGES);
    for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
        comp_sizes[i] = base + (i < rem ? 1u : 0u);
        if (comp_sizes[i] > TRANS_STREAM_MAX_PAGE_COMP_SIZE) {
            comp_sizes[i] = TRANS_STREAM_MAX_PAGE_COMP_SIZE;
        }
    }

    logger_info("STREAM_INFO: total_orig_size=%u", total_orig);
    for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
        logger_info("STREAM_INFO: page %zu comp_size=%u", i, comp_sizes[i]);
    }

    uint8_t si[32];
    if (trans_build_stream_info(total_orig, comp_sizes, si) != 0) {
        logger_error("trans_build_stream_info failed");
        free(merged);
        trans_free_bursts(bursts, burst_count);
        free(page);
        logger_close();
        return 1;
    }

    uint32_t total_orig2 = 0;
    if (trans_parse_stream_info(si, &total_orig2, parsed_comp) != 0) {
        logger_error("trans_parse_stream_info failed");
        free(merged);
        trans_free_bursts(bursts, burst_count);
        free(page);
        logger_close();
        return 1;
    }

    logger_info("STREAM_INFO parsed: total_orig_size=%u", total_orig2);
    for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
        logger_info("STREAM_INFO parsed: page %zu comp_size=%u",
                    i, parsed_comp[i]);
    }

    if (total_orig2 != total_orig) {
        logger_error("STREAM_INFO total_orig mismatch (%u vs %u)",
                     total_orig, total_orig2);
        free(merged);
        trans_free_bursts(bursts, burst_count);
        free(page);
        logger_close();
        return 1;
    }

    for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
        if (parsed_comp[i] != comp_sizes[i]) {
            logger_error("STREAM_INFO page %zu comp_size mismatch (%u vs %u)",
                         i, comp_sizes[i], parsed_comp[i]);
            free(merged);
            trans_free_bursts(bursts, burst_count);
            free(page);
            logger_close();
            return 1;
        }
    }

    logger_succ("STREAM_INFO pack/unpack test passed");

    /* 6) TRANSFER_FINISH test. */
    uint8_t tf[2];
    trans_build_transfer_finish(tf);
    logger_info("TRANSFER_FINISH built: bytes=[0x%02X, 0x%02X]",
                tf[0], tf[1]);

    if (trans_parse_transfer_finish(tf, sizeof(tf)) != 0) {
        logger_error("TRANSFER_FINISH parse failed");
        free(merged);
        trans_free_bursts(bursts, burst_count);
        free(page);
        logger_close();
        return 1;
    }
    logger_succ("TRANSFER_FINISH build/parse test passed");

    /* Cleanup */
    free(merged);
    trans_free_bursts(bursts, burst_count);
    free(page);

    logger_succ("=== Transport Layer test completed successfully ===");
    logger_close();

    return 0;
}
