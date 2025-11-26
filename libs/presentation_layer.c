#include "presentation_layer.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zlib.h>

#include "logger.h"

/* ------------------------------------------------------------------------- */
/* Free helper                                                               */
/* ------------------------------------------------------------------------- */

void pres_free_pages(pres_page_t *pages, size_t count)
{
    if (!pages) return;
    for (size_t i = 0; i < count; ++i) {
        free(pages[i].data);
        pages[i].data = NULL;
        pages[i].size = 0;
        pages[i].orig_size = 0;
    }
    free(pages);
}

/* ------------------------------------------------------------------------- */
/* Splitting                                                                 */
/* ------------------------------------------------------------------------- */

int pres_split_into_pages_by_count(const uint8_t *file_data,
                                   size_t file_len,
                                   size_t page_count,
                                   pres_page_t **out_pages,
                                   size_t *out_page_count)
{
    if (!out_pages || !out_page_count) {
        logger_error("pres_split_into_pages_by_count: output pointers are NULL");
        return -1;
    }

    *out_pages = NULL;
    *out_page_count = 0;

    if (!file_data || file_len == 0 || page_count == 0) {
        /* Nothing to split. */
        return 0;
    }

    /* Avoid zero-length pages. */
    if (page_count > file_len) {
        page_count = file_len;
    }

    pres_page_t *pages = (pres_page_t *)calloc(page_count, sizeof(pres_page_t));
    if (!pages) {
        logger_error("pres_split_into_pages_by_count: calloc failed");
        return -1;
    }

    size_t base = file_len / page_count;
    size_t rem  = file_len % page_count;
    size_t offset = 0;

    for (size_t i = 0; i < page_count; ++i) {
        size_t this_len = base + ((i < rem) ? 1 : 0);

        pages[i].size = this_len;
        pages[i].orig_size = this_len;

        if (this_len == 0) {
            pages[i].data = NULL;
            continue;
        }

        pages[i].data = (uint8_t *)malloc(this_len);
        if (!pages[i].data) {
            logger_error("pres_split_into_pages_by_count: malloc failed for page %zu", i);
            pres_free_pages(pages, page_count);
            return -1;
        }

        memcpy(pages[i].data, file_data + offset, this_len);
        offset += this_len;
    }

    *out_pages = pages;
    *out_page_count = page_count;

    logger_info("Presentation: split %zu bytes into %zu pages",
                file_len, page_count);
    return 0;
}

int pres_split_into_pages_default(const uint8_t *file_data,
                                  size_t file_len,
                                  pres_page_t **out_pages,
                                  size_t *out_page_count)
{
    return pres_split_into_pages_by_count(file_data,
                                          file_len,
                                          PRES_DEFAULT_PAGE_COUNT,
                                          out_pages,
                                          out_page_count);
}

/* ------------------------------------------------------------------------- */
/* Compression                                                               */
/* ------------------------------------------------------------------------- */

int pres_compress_pages(const pres_page_t *in_pages,
                        size_t in_count,
                        pres_page_t **out_pages,
                        size_t *out_count)
{
    if (!out_pages || !out_count) {
        logger_error("pres_compress_pages: output pointers are NULL");
        return -1;
    }

    *out_pages = NULL;
    *out_count = 0;

    if (!in_pages || in_count == 0) {
        return 0;
    }

    pres_page_t *pages = (pres_page_t *)calloc(in_count, sizeof(pres_page_t));
    if (!pages) {
        logger_error("pres_compress_pages: calloc failed");
        return -1;
    }

    for (size_t i = 0; i < in_count; ++i) {
        const pres_page_t *src = &in_pages[i];
        pres_page_t *dst       = &pages[i];

        dst->orig_size = src->orig_size;

        if (!src->data || src->size == 0) {
            dst->data = NULL;
            dst->size = 0;
            continue;
        }

        uLongf dest_cap = compressBound((uLong)src->size);
        uint8_t *comp = (uint8_t *)malloc(dest_cap);
        if (!comp) {
            logger_error("pres_compress_pages: malloc failed for page %zu", i);
            pres_free_pages(pages, in_count);
            return -1;
        }

        uLongf dest_len = dest_cap;
        int zret = compress2(comp, &dest_len,
                             src->data, (uLong)src->size,
                             6); /* compression level 6 */

        if (zret != Z_OK) {
            logger_error("pres_compress_pages: zlib compress2 failed for page %zu: %d",
                         i, zret);
            free(comp);
            pres_free_pages(pages, in_count);
            return -1;
        }

        dst->data = comp;
        dst->size = (size_t)dest_len;
    }

    *out_pages = pages;
    *out_count = in_count;

    logger_info("Presentation: compressed %zu pages", in_count);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Decompression                                                             */
/* ------------------------------------------------------------------------- */

int pres_decompress_pages(const pres_page_t *in_pages,
                          size_t in_count,
                          pres_page_t **out_pages,
                          size_t *out_count)
{
    if (!out_pages || !out_count) {
        logger_error("pres_decompress_pages: output pointers are NULL");
        return -1;
    }

    *out_pages = NULL;
    *out_count = 0;

    if (!in_pages || in_count == 0) {
        return 0;
    }

    pres_page_t *pages = (pres_page_t *)calloc(in_count, sizeof(pres_page_t));
    if (!pages) {
        logger_error("pres_decompress_pages: calloc failed");
        return -1;
    }

    for (size_t i = 0; i < in_count; ++i) {
        const pres_page_t *src = &in_pages[i];
        pres_page_t *dst       = &pages[i];

        dst->orig_size = src->orig_size;

        if (!src->data || src->size == 0 || src->orig_size == 0) {
            dst->data = NULL;
            dst->size = 0;
            continue;
        }

        uLongf dest_len = (uLongf)src->orig_size;
        uint8_t *raw = (uint8_t *)malloc(dest_len);
        if (!raw) {
            logger_error("pres_decompress_pages: malloc failed for page %zu", i);
            pres_free_pages(pages, in_count);
            return -1;
        }

        int zret = uncompress(raw, &dest_len,
                              src->data, (uLong)src->size);

        if (zret != Z_OK || dest_len != (uLongf)src->orig_size) {
            logger_error("pres_decompress_pages: uncompress failed for page %zu "
                         "(z=%d, dest_len=%lu, expected=%zu)",
                         i, zret, (unsigned long)dest_len, src->orig_size);
            free(raw);
            pres_free_pages(pages, in_count);
            return -1;
        }

        dst->data = raw;
        dst->size = (size_t)dest_len;
    }

    *out_pages = pages;
    *out_count = in_count;

    logger_info("Presentation: decompressed %zu pages", in_count);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Merge                                                                    */
/* ------------------------------------------------------------------------- */

int pres_merge_pages(const pres_page_t *pages,
                     size_t count,
                     uint8_t **out_data,
                     size_t *out_len)
{
    if (!out_data || !out_len) {
        logger_error("pres_merge_pages: output pointers are NULL");
        return -1;
    }

    *out_data = NULL;
    *out_len  = 0;

    if (!pages || count == 0) {
        return 0;
    }

    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        total += pages[i].size;
    }

    if (total == 0) {
        return 0;
    }

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) {
        logger_error("pres_merge_pages: malloc failed for %zu bytes", total);
        return -1;
    }

    size_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!pages[i].data || pages[i].size == 0) continue;
        memcpy(buf + offset, pages[i].data, pages[i].size);
        offset += pages[i].size;
    }

    *out_data = buf;
    *out_len  = total;

    logger_info("Presentation: merged %zu pages into %zu bytes", count, total);
    return 0;
}
