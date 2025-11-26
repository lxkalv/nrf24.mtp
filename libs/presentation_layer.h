#ifndef PRESENTATION_LAYER_H
#define PRESENTATION_LAYER_H

#include <stddef.h>
#include <stdint.h>

/* One page of data (raw or compressed). */
typedef struct {
    uint8_t *data;     /* owned buffer (malloc/free) */
    size_t   size;     /* length of data[] in bytes */
    size_t   orig_size;/* uncompressed size.
                          For raw pages: orig_size == size */
} pres_page_t;

/* Default number of pages when splitting a file. */
#define PRES_DEFAULT_PAGE_COUNT 10

/* Split file_data[0..file_len) into page_count pages of similar size.
 *
 * On success:
 *   - *out_pages points to an array of page_count pres_page_t,
 *   - each page's data pointer is malloc()'d and must be freed via pres_free_pages().
 *   - *out_page_count = actual page count (may be smaller if file_len < page_count).
 *
 * Returns 0 on success, non-zero on failure.
 */
int pres_split_into_pages_by_count(const uint8_t *file_data,
                                   size_t file_len,
                                   size_t page_count,
                                   pres_page_t **out_pages,
                                   size_t *out_page_count);

/* Convenience: split into PRES_DEFAULT_PAGE_COUNT pages. */
int pres_split_into_pages_default(const uint8_t *file_data,
                                  size_t file_len,
                                  pres_page_t **out_pages,
                                  size_t *out_page_count);

/* Compress each page independently using zlib (level 6).
 *
 * in_pages / in_count: raw pages (orig_size must be set).
 * On success:
 *   - *out_pages points to an array of in_count pages,
 *   - each page's data is compressed,
 *   - orig_size is preserved (raw size),
 *   - *out_count == in_count.
 */
int pres_compress_pages(const pres_page_t *in_pages,
                        size_t in_count,
                        pres_page_t **out_pages,
                        size_t *out_count);

/* Decompress each page independently using zlib.
 *
 * in_pages / in_count: compressed pages, each with orig_size set.
 * On success:
 *   - *out_pages: array of in_count pages with raw data,
 *   - *out_count == in_count.
 */
int pres_decompress_pages(const pres_page_t *in_pages,
                          size_t in_count,
                          pres_page_t **out_pages,
                          size_t *out_count);

/* Merge pages[0..count) into a single buffer (in order).
 *
 * Typically called on decompressed pages to reconstruct the original file.
 *
 * On success:
 *   - *out_data is malloc()'d buffer containing the concatenated bytes,
 *   - *out_len is its length.
 */
int pres_merge_pages(const pres_page_t *pages,
                     size_t count,
                     uint8_t **out_data,
                     size_t *out_len);

/* Free an array of pages previously returned by any pres_* function. */
void pres_free_pages(pres_page_t *pages, size_t count);

#endif /* PRESENTATION_LAYER_H */
