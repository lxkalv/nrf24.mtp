/* p3p.c
 *
 * Reliable point-to-point file transfer over nRF24L01 + Raspberry Pi.
 * This is essentially the "p2p_mode" we had working, but:
 *   - uses logger.[ch] instead of utils.[ch]
 *   - writes logs into logs/p3p_TX_<timestamp>.log or logs/p3p_RX_<timestamp>.log
 *
 * CLI is intentionally simple (same as old C p2p):
 *
 *   p3p tx <spi_device> <ce_gpio> <input_file>
 *   p3p rx <spi_device> <ce_gpio> <output_file>
 *
 * Example:
 *   p3p tx /dev/spidev0.0 22 test_files/quijote.txt
 *   p3p rx /dev/spidev0.0 22 received_quijote.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <zlib.h>

#include "nrf24.h"
#include "logger.h"

/* Map old-style macros onto logger */
#define ERROR(...) logger_error(__VA_ARGS__)
#define WARN(...)  logger_warn(__VA_ARGS__)
#define INFO(...)  logger_info(__VA_ARGS__)
#define SUCC(...)  logger_succ(__VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Constants (same spirit as previous p2p)                                   */
/* ------------------------------------------------------------------------- */

#define P2P_CHANNEL          76     /* same channel as before */

#define MAX_PAGES            10     /* number of pages we compress separately */

/* Within a burst we send multiple frames, each with:
 *   [ChunkID (1B)] + Data (<=31B)
 * so max on-air payload is 32 bytes.
 */
#define MAX_PAYLOAD          32
#define CHUNK_DATA_MAX       31

/* For compressed bytes per burst (roughly what we used before) */
#define BURST_DATA_MAX       7905   /* 255 frames * 31 bytes = 7905 */
#define MAX_CHUNKS_PER_BURST 255

#define CHECKSUM_TIMEOUT_MS  1000   /* wait up to 1 s for checksum from RX */
#define CONTROL_TIMEOUT_MS   100    /* per-attempt timeout when sending control frames */
#define DATA_TIMEOUT_MS      20     /* per-attempt timeout when sending data frames */

#define CHECKSUM_SIZE        8      /* 64-bit FNV-1a checksum */

/* Message types */
#define MSG_INFO             0xFF
#define P2P_MSG_STREAM_INFO  0xE0
#define MSG_BURST_INFO       0xF0
#define MSG_TRANSFER_FINISH  0x0F

/* ------------------------------------------------------------------------- */
/* Time helper                                                               */
/* ------------------------------------------------------------------------- */

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------------------------------- */
/* Little-endian helpers                                                     */
/* ------------------------------------------------------------------------- */

static void encode_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t decode_u16_le(const uint8_t *src)
{
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

/* ------------------------------------------------------------------------- */
/* 64-bit FNV-1a checksum                                                    */
/* ------------------------------------------------------------------------- */

#define FNV64_OFFSET_BASIS  1469598103934665603ULL
#define FNV64_PRIME         1099511628211ULL

static void checksum_init(uint64_t *state)
{
    *state = FNV64_OFFSET_BASIS;
}

static void checksum_update(uint64_t *state, const uint8_t *data, size_t len)
{
    uint64_t h = *state;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)data[i];
        h *= FNV64_PRIME;
    }
    *state = h;
}

static void checksum_final(uint64_t state, uint8_t out[CHECKSUM_SIZE])
{
    for (int i = 0; i < 8; ++i) {
        out[i] = (uint8_t)(state & 0xFFu);
        state >>= 8;
    }
}

/* ------------------------------------------------------------------------- */
/* nRF24 convenience wrappers (account RF usage)                             */
/* ------------------------------------------------------------------------- */

static int send_with_retries(nrf24_t *radio,
                             const uint8_t *buf,
                             uint8_t len,
                             unsigned int timeout_ms,
                             const char *what,
                             uint64_t *rf_bytes_total,
                             uint64_t *rf_frames_total)
{
    unsigned int attempt = 0;

    for (;;) {
        /* Count this attempt as on-air RF usage */
        if (rf_bytes_total)  *rf_bytes_total  += len;
        if (rf_frames_total) *rf_frames_total += 1;

        int ret = nrf24_send_blocking(radio, buf, len, timeout_ms);
        if (ret == 0) {
            return 0;  /* success */
        }

        if (errno != ETIMEDOUT) {
            ERROR("nrf24_send_blocking(%s) failed: %s", what, strerror(errno));
            return -1;
        }

        ++attempt;
        if (attempt == 1 || (attempt % 50) == 0) {
            WARN("%s: timeout (no ACK) on attempt %u for %u-byte frame",
                 what, attempt, (unsigned)len);
        }

        /* Keep retrying forever, but periodically re-configure the radio */
        if (attempt % 500 == 0) {
            WARN("%s: %u consecutive timeouts, reconfiguring radio",
                 what, attempt);
            (void)nrf24_configure_quick(radio, P2P_CHANNEL);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* RX-side in-memory storage for a single page                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    unsigned frames_in_burst;
    uint8_t *frame_data[MAX_CHUNKS_PER_BURST];
    uint8_t  frame_len[MAX_CHUNKS_PER_BURST];
} Burst;

typedef struct {
    Burst  *bursts;
    size_t  count;
    size_t  capacity;
} PageStream;

static void page_stream_init(PageStream *ps)
{
    ps->bursts   = NULL;
    ps->count    = 0;
    ps->capacity = 0;
}

static void free_burst(Burst *b)
{
    if (!b) return;
    for (unsigned i = 0; i < b->frames_in_burst; ++i) {
        free(b->frame_data[i]);
        b->frame_data[i] = NULL;
        b->frame_len[i]  = 0;
    }
    b->frames_in_burst = 0;
}

static void page_stream_free(PageStream *ps)
{
    if (!ps->bursts) return;
    for (size_t i = 0; i < ps->count; ++i) {
        free_burst(&ps->bursts[i]);
    }
    free(ps->bursts);
    ps->bursts   = NULL;
    ps->count    = 0;
    ps->capacity = 0;
}

static Burst *page_stream_get_burst(PageStream *ps, unsigned burst_id)
{
    if (burst_id >= ps->capacity) {
        size_t new_cap = ps->capacity ? ps->capacity * 2 : 8;
        while (burst_id >= new_cap) new_cap *= 2;

        Burst *new_bursts = calloc(new_cap, sizeof(Burst));
        if (!new_bursts) {
            return NULL;
        }

        for (size_t i = 0; i < ps->count; ++i) {
            new_bursts[i] = ps->bursts[i];
        }
        free(ps->bursts);
        ps->bursts   = new_bursts;
        ps->capacity = new_cap;
    }

    if (burst_id >= ps->count) {
        for (size_t i = ps->count; i <= burst_id; ++i) {
            ps->bursts[i].frames_in_burst = 0;
            for (unsigned j = 0; j < MAX_CHUNKS_PER_BURST; ++j) {
                ps->bursts[i].frame_data[j] = NULL;
                ps->bursts[i].frame_len[j]  = 0;
            }
        }
        ps->count = burst_id + 1;
    }

    return &ps->bursts[burst_id];
}

static int store_burst(PageStream *ps,
                       unsigned burst_id,
                       unsigned frames_in_burst,
                       uint8_t current_burst[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD],
                       const uint8_t sizes[MAX_CHUNKS_PER_BURST])
{
    Burst *b = page_stream_get_burst(ps, burst_id);
    if (!b) {
        ERROR("store_burst: out of memory for burst %u", burst_id);
        return -1;
    }

    free_burst(b);
    b->frames_in_burst = frames_in_burst;

    for (unsigned i = 0; i < frames_in_burst; ++i) {
        size_t len = sizes[i];
        b->frame_data[i] = malloc(len);
        if (!b->frame_data[i]) {
            ERROR("store_burst: malloc failed for frame %u of burst %u", i, burst_id);
            b->frames_in_burst = i;
            return -1;
        }
        memcpy(b->frame_data[i], current_burst[i], len);
        b->frame_len[i] = (uint8_t)len;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Compression helpers: split into pages, compress page, decompress page     */
/* ------------------------------------------------------------------------- */

static int compress_page(const uint8_t *data, size_t len,
                         uint8_t **out_buf, size_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    if (len == 0) {
        *out_buf = NULL;
        *out_len = 0;
        return 0;
    }

    /* Worst-case bound for zlib */
    uLongf dest_len = compressBound((uLong)len);
    uint8_t *dest = (uint8_t *)malloc(dest_len);
    if (!dest) {
        ERROR("compress_page: malloc(%lu) failed", (unsigned long)dest_len);
        return -1;
    }

    int zret = compress2(dest, &dest_len, data, (uLong)len, Z_BEST_SPEED);
    if (zret != Z_OK) {
        ERROR("compress_page: compress2 failed with code %d", zret);
        free(dest);
        return -1;
    }

    *out_buf = dest;
    *out_len = (size_t)dest_len;
    return 0;
}

static int decompress_page_to_file(PageStream *ps,
                                   FILE *fout,
                                   uint64_t *compressed_total,
                                   uint64_t *uncompressed_total)
{
    /* First reconstruct compressed page bytes from bursts/frames */
    size_t comp_buf_cap = 0;
    size_t comp_buf_len = 0;
    uint8_t *comp_buf = NULL;

    for (size_t bid = 0; bid < ps->count; ++bid) {
        Burst *b = &ps->bursts[bid];
        if (b->frames_in_burst == 0) continue;

        for (unsigned i = 0; i < b->frames_in_burst; ++i) {
            uint8_t *frame = b->frame_data[i];
            uint8_t  flen  = b->frame_len[i];
            if (!frame || flen <= 1) continue;

            size_t data_len = flen - 1; /* skip ChunkID byte */

            if (comp_buf_len + data_len > comp_buf_cap) {
                size_t new_cap = (comp_buf_cap == 0) ? 4096 : comp_buf_cap * 2;
                while (comp_buf_len + data_len > new_cap) {
                    new_cap *= 2;
                }
                uint8_t *tmp = realloc(comp_buf, new_cap);
                if (!tmp) {
                    ERROR("decompress_page_to_file: realloc failed");
                    free(comp_buf);
                    return -1;
                }
                comp_buf = tmp;
                comp_buf_cap = new_cap;
            }

            memcpy(&comp_buf[comp_buf_len], frame + 1, data_len);
            comp_buf_len += data_len;
        }
    }

    if (comp_buf_len == 0) {
        WARN("decompress_page_to_file: no compressed data in page");
        free(comp_buf);
        return 0;
    }

    if (compressed_total) *compressed_total += comp_buf_len;

    /* Guess an upper bound for uncompressed size; we can grow dynamically */
    size_t decomp_cap = comp_buf_len * 5 + 1024;
    uint8_t *decomp_buf = (uint8_t *)malloc(decomp_cap);
    if (!decomp_buf) {
        ERROR("decompress_page_to_file: malloc(%zu) failed", decomp_cap);
        free(comp_buf);
        return -1;
    }

    uLongf dest_len = (uLongf)decomp_cap;
    int zret = uncompress(decomp_buf, &dest_len, comp_buf, (uLong)comp_buf_len);
    if (zret == Z_BUF_ERROR || zret == Z_MEM_ERROR) {
        /* Try once more with bigger buffer */
        size_t new_cap = decomp_cap * 2 + 16384;
        uint8_t *tmp = (uint8_t *)realloc(decomp_buf, new_cap);
        if (!tmp) {
            ERROR("decompress_page_to_file: realloc(%zu) failed", new_cap);
            free(comp_buf);
            free(decomp_buf);
            return -1;
        }
        decomp_buf = tmp;
        decomp_cap = new_cap;
        dest_len   = (uLongf)decomp_cap;
        zret = uncompress(decomp_buf, &dest_len, comp_buf, (uLong)comp_buf_len);
    }

    if (zret != Z_OK) {
        ERROR("decompress_page_to_file: uncompress failed with code %d", zret);
        free(comp_buf);
        free(decomp_buf);
        return -1;
    }

    /* Write decompressed bytes to output file */
    if (fwrite(decomp_buf, 1, dest_len, fout) != dest_len) {
        ERROR("decompress_page_to_file: fwrite failed");
        free(comp_buf);
        free(decomp_buf);
        return -1;
    }

    if (uncompressed_total) *uncompressed_total += dest_len;

    free(comp_buf);
    free(decomp_buf);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* TX path                                                                   */
/* ------------------------------------------------------------------------- */

static int run_tx(const char *spi_dev, int ce_bcm, const char *input_path)
{
    nrf24_t radio;
    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)ce_bcm
    };

    if (nrf24_init(&radio, &cfg) < 0) {
        ERROR("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    if (nrf24_configure_quick(&radio, P2P_CHANNEL) < 0) {
        ERROR("nrf24_configure_quick failed");
        nrf24_deinit(&radio);
        return 1;
    }
    if (nrf24_set_mode_tx(&radio) < 0) {
        ERROR("nrf24_set_mode_tx failed");
        nrf24_deinit(&radio);
        return 1;
    }

    /* Load entire input file */
    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        ERROR("Cannot open input file '%s': %s", input_path, strerror(errno));
        nrf24_deinit(&radio);
        return 1;
    }

    if (fseek(fin, 0, SEEK_END) != 0) {
        ERROR("fseek failed on input");
        fclose(fin);
        nrf24_deinit(&radio);
        return 1;
    }
    long fsize = ftell(fin);
    if (fsize < 0) fsize = 0;
    rewind(fin);

    uint64_t orig_len = (uint64_t)fsize;
    uint8_t *orig_buf = NULL;

    if (orig_len > 0) {
        orig_buf = (uint8_t *)malloc((size_t)orig_len);
        if (!orig_buf) {
            ERROR("malloc failed for input");
            fclose(fin);
            nrf24_deinit(&radio);
            return 1;
        }
        size_t r = fread(orig_buf, 1, (size_t)orig_len, fin);
        if (r != (size_t)orig_len) {
            ERROR("fread failed (%zu of %llu)", r, (unsigned long long)orig_len);
            free(orig_buf);
            fclose(fin);
            nrf24_deinit(&radio);
            return 1;
        }
    }

    fclose(fin);

    INFO("P3P TX: sending file '%s' (%llu bytes)",
         input_path, (unsigned long long)orig_len);

    /* Split into up to MAX_PAGES equal-ish pages */
    uint64_t bytes_per_page = (orig_len + MAX_PAGES - 1) / MAX_PAGES;
    if (bytes_per_page == 0) bytes_per_page = 1;

    double t_start = now_seconds();
    uint64_t tx_rf_bytes_total  = 0;
    uint64_t tx_rf_frames_total = 0;

    uint8_t total_pages = (orig_len == 0) ? 0 :
                          (uint8_t)((orig_len + bytes_per_page - 1) / bytes_per_page);
    if (total_pages > MAX_PAGES) total_pages = MAX_PAGES;

    uint64_t offset = 0;

    for (uint8_t page_id = 0; page_id < total_pages; ++page_id) {
        uint64_t remaining = orig_len - offset;
        uint64_t this_page_len = (remaining > bytes_per_page) ? bytes_per_page : remaining;

        uint8_t *comp_page = NULL;
        size_t   comp_len  = 0;

        if (compress_page(&orig_buf[offset], (size_t)this_page_len,
                          &comp_page, &comp_len) != 0) {
            ERROR("P3P TX: compress_page failed for Page %u", page_id);
            free(orig_buf);
            nrf24_deinit(&radio);
            return 1;
        }

        INFO("P3P TX: Page %u/%u: %llu B -> %zu B compressed",
             (unsigned)page_id + 1, (unsigned)total_pages,
             (unsigned long long)this_page_len, comp_len);

        /* Compute bursts for this page */
        uint16_t expected_bursts = (uint16_t)(comp_len / BURST_DATA_MAX);
        if ((comp_len % BURST_DATA_MAX) != 0) expected_bursts++;

        if (expected_bursts == 0) {
            free(comp_page);
            offset += this_page_len;
            continue;
        }

        /* Precompute last burst sizes */
        size_t last_burst_bytes = comp_len % BURST_DATA_MAX;
        if (last_burst_bytes == 0) last_burst_bytes = BURST_DATA_MAX;

        uint8_t last_burst_frames = (uint8_t)((last_burst_bytes + CHUNK_DATA_MAX - 1) / CHUNK_DATA_MAX);
        uint8_t last_frame_bytes  = (uint8_t)(last_burst_bytes -
                (size_t)(last_burst_frames - 1) * CHUNK_DATA_MAX);
        if (last_frame_bytes == 0) last_frame_bytes = CHUNK_DATA_MAX;

        /* STREAM_INFO for this page */
        uint8_t stream_info[8];
        stream_info[0] = MSG_INFO;
        stream_info[1] = P2P_MSG_STREAM_INFO;
        stream_info[2] = page_id;
        stream_info[3] = total_pages;
        encode_u16_le(&stream_info[4], expected_bursts);
        stream_info[6] = last_burst_frames;
        stream_info[7] = last_frame_bytes;

        if (send_with_retries(&radio,
                              stream_info,
                              sizeof(stream_info),
                              CONTROL_TIMEOUT_MS,
                              "STREAM_INFO",
                              &tx_rf_bytes_total,
                              &tx_rf_frames_total) < 0) {
            ERROR("P3P TX: failed to send STREAM_INFO");
            free(comp_page);
            free(orig_buf);
            nrf24_deinit(&radio);
            return 1;
        }

        /* Send each burst of this page with checksum/ack logic */
        size_t page_offset = 0;
        for (uint16_t burst_id = 0; burst_id < expected_bursts; ++burst_id) {

            size_t burst_bytes = BURST_DATA_MAX;
            if (burst_id == expected_bursts - 1) {
                burst_bytes = last_burst_bytes;
            }
            if (page_offset + burst_bytes > comp_len) {
                burst_bytes = comp_len - page_offset;
            }

            /* Build burst frames in memory */
            uint8_t burst_payloads[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD];
            uint8_t burst_sizes   [MAX_CHUNKS_PER_BURST];
            unsigned frames_in_burst = 0;
            uint16_t burst_onair_bytes = 0;

            uint64_t chk_state;
            checksum_init(&chk_state);

            size_t consumed = 0;
            while (consumed < burst_bytes && frames_in_burst < MAX_CHUNKS_PER_BURST) {
                size_t max_data = CHUNK_DATA_MAX;
                size_t remaining = burst_bytes - consumed;
                if (remaining < max_data) max_data = remaining;

                uint8_t chunk_id = (uint8_t)frames_in_burst;
                burst_payloads[frames_in_burst][0] = chunk_id;
                memcpy(&burst_payloads[frames_in_burst][1],
                       &comp_page[page_offset + consumed],
                       max_data);

                uint8_t payload_len = (uint8_t)(1 + max_data);
                burst_sizes[frames_in_burst] = payload_len;
                burst_onair_bytes += payload_len;

                checksum_update(&chk_state,
                                burst_payloads[frames_in_burst],
                                payload_len);

                consumed += max_data;
                frames_in_burst++;
            }

            if (frames_in_burst == 0) {
                WARN("P3P TX: burst %u of page %u had 0 frames, skipping",
                     (unsigned)burst_id, (unsigned)page_id);
                continue;
            }

            uint8_t checksum_bytes[CHECKSUM_SIZE];
            checksum_final(chk_state, checksum_bytes);

            INFO("P3P TX: Page %u, BURST %u -> %zu B, %u frames, checksum ready",
                 (unsigned)page_id, (unsigned)burst_id,
                 burst_bytes, frames_in_burst);

            int burst_done = 0;
            while (!burst_done) {
                /* BURST_INFO */
                uint8_t burst_info[6];
                burst_info[0] = MSG_INFO;
                burst_info[1] = MSG_BURST_INFO;
                burst_info[2] = page_id;
                burst_info[3] = (uint8_t)burst_id;
                encode_u16_le(&burst_info[4], burst_onair_bytes);

                if (send_with_retries(&radio,
                                      burst_info,
                                      sizeof(burst_info),
                                      CONTROL_TIMEOUT_MS,
                                      "BURST_INFO",
                                      &tx_rf_bytes_total,
                                      &tx_rf_frames_total) < 0) {
                    ERROR("P3P TX: failed to send BURST_INFO");
                    free(comp_page);
                    free(orig_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }

                /* All DATA frames */
                for (unsigned i = 0; i < frames_in_burst; ++i) {
                    if (send_with_retries(&radio,
                                          burst_payloads[i],
                                          burst_sizes[i],
                                          DATA_TIMEOUT_MS,
                                          "DATA",
                                          &tx_rf_bytes_total,
                                          &tx_rf_frames_total) < 0) {
                        ERROR("P3P TX: failed to send DATA frame");
                        free(comp_page);
                        free(orig_buf);
                        nrf24_deinit(&radio);
                        return 1;
                    }
                }

                /* Wait for checksum from RX */
                if (nrf24_set_mode_rx(&radio) < 0) {
                    ERROR("P3P TX: nrf24_set_mode_rx failed");
                    free(comp_page);
                    free(orig_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }

                double wait_start = now_seconds();
                int got_valid_checksum = 0;

                while (!got_valid_checksum &&
                       (now_seconds() - wait_start) * 1000.0 < CHECKSUM_TIMEOUT_MS) {
                    uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
                    uint8_t len = sizeof(buf);

                    int ret = nrf24_recv_blocking(&radio, buf, &len, 50);
                    if (ret < 0) {
                        if (errno == ETIMEDOUT) {
                            continue;
                        }
                        ERROR("P3P TX: nrf24_recv_blocking (checksum) failed: %s",
                              strerror(errno));
                        free(comp_page);
                        free(orig_buf);
                        nrf24_deinit(&radio);
                        return 1;
                    }

                    if (len != CHECKSUM_SIZE) {
                        WARN("P3P TX: received non-checksum frame (%u B) while waiting",
                             len);
                        continue;
                    }

                    if (memcmp(buf, checksum_bytes, CHECKSUM_SIZE) == 0) {
                        SUCC("P3P TX: Page %u, BURST %u checksum confirmed by RX",
                             (unsigned)page_id, (unsigned)burst_id);
                        got_valid_checksum = 1;
                    } else {
                        WARN("P3P TX: invalid checksum received for Page %u, BURST %u",
                             (unsigned)page_id, (unsigned)burst_id);
                    }
                }

                if (!got_valid_checksum) {
                    WARN("P3P TX: checksum timeout for Page %u, BURST %u, resending",
                         (unsigned)page_id, (unsigned)burst_id);
                    if (nrf24_set_mode_tx(&radio) < 0) {
                        ERROR("P3P TX: nrf24_set_mode_tx failed");
                        free(comp_page);
                        free(orig_buf);
                        nrf24_deinit(&radio);
                        return 1;
                    }
                    continue; /* resend same burst */
                }

                burst_done = 1;

                if (nrf24_set_mode_tx(&radio) < 0) {
                    ERROR("P3P TX: nrf24_set_mode_tx failed");
                    free(comp_page);
                    free(orig_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }
            }

            page_offset += burst_bytes;
        }

        free(comp_page);
        offset += this_page_len;
    }

    /* TRANSFER_FINISH */
    uint8_t fin_msg[2];
    fin_msg[0] = MSG_INFO;
    fin_msg[1] = MSG_TRANSFER_FINISH;

    (void)send_with_retries(&radio,
                            fin_msg,
                            sizeof(fin_msg),
                            CONTROL_TIMEOUT_MS,
                            "TRANSFER_FINISH",
                            &tx_rf_bytes_total,
                            &tx_rf_frames_total);

    double t_end = now_seconds();
    double dt    = t_end - t_start;

    double user_kibps = (dt > 0.0)
        ? ((double)orig_len / 1024.0 / dt)
        : 0.0;

    double rf_kibps = (dt > 0.0)
        ? ((double)tx_rf_bytes_total / 1024.0 / dt)
        : 0.0;

    SUCC("P3P TX: done. User: %llu bytes in %.3f s (%.1f KiB/s). "
         "RF on-air: %llu bytes in %.3f s (%.1f KiB/s, %llu frames).",
         (unsigned long long)orig_len, dt, user_kibps,
         (unsigned long long)tx_rf_bytes_total, dt, rf_kibps,
         (unsigned long long)tx_rf_frames_total);

    free(orig_buf);
    nrf24_deinit(&radio);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* RX path                                                                   */
/* ------------------------------------------------------------------------- */

static int run_rx(const char *spi_dev, int ce_bcm, const char *output_path)
{
    nrf24_t radio;
    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = 8000000,
        .ce_gpio      = (uint8_t)ce_bcm
    };

    if (nrf24_init(&radio, &cfg) < 0) {
        ERROR("nrf24_init failed: %s", strerror(errno));
        return 1;
    }
    if (nrf24_configure_quick(&radio, P2P_CHANNEL) < 0) {
        ERROR("nrf24_configure_quick failed");
        nrf24_deinit(&radio);
        return 1;
    }
    if (nrf24_set_mode_rx(&radio) < 0) {
        ERROR("nrf24_set_mode_rx failed");
        nrf24_deinit(&radio);
        return 1;
    }

    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        ERROR("Cannot open output file '%s': %s", output_path, strerror(errno));
        nrf24_deinit(&radio);
        return 1;
    }

    PageStream stream;
    page_stream_init(&stream);

    INFO("P3P RX: waiting on channel %d...", P2P_CHANNEL);

    int transfer_finished  = 0;
    int tx_started         = 0;
    double t_start         = 0.0;

    uint64_t rf_bytes_total  = 0;
    uint64_t rf_frames_total = 0;
    uint64_t compressed_total   = 0;
    uint64_t uncompressed_total = 0;

    uint8_t current_page_id     = 0;
    uint8_t total_pages         = 0;
    uint16_t expected_bursts    = 0;
    uint8_t last_burst_frames   = 0;
    uint8_t last_frame_bytes    = 0;

    int have_page_info = 0;
    int page_has_data  = 0;

    uint16_t bursts_completed = 0;
    uint8_t burst_received[65536]; /* overkill but safe */

    int in_burst = 0;
    uint8_t frames_in_burst = 0;
    uint8_t sizes[MAX_CHUNKS_PER_BURST];
    uint8_t current_burst[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD];

    while (!transfer_finished) {
        uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
        uint8_t len = sizeof(buf);

        int ret = nrf24_recv_blocking(&radio, buf, &len, 0);
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                continue;
            }
            ERROR("P3P RX: nrf24_recv_blocking failed (errno=%d: %s). "
                  "Stopping RX loop and assembling what we have.",
                  errno, strerror(errno));
            break;
        }

        if (len > 0) {
            rf_bytes_total  += len;
            rf_frames_total += 1;
        }

        if (!tx_started) {
            t_start   = now_seconds();
            tx_started = 1;
        }

        /* STREAM_INFO */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == P2P_MSG_STREAM_INFO) {
            if (len < 8) {
                WARN("P3P RX: malformed STREAM_INFO (len=%u)", len);
                continue;
            }

            /* If we had an unfinished page with data, decompress what we have */
            if (have_page_info && page_has_data) {
                WARN("P3P RX: new STREAM_INFO while previous page %u has data; "
                     "decompressing previous page as-is",
                     (unsigned)current_page_id);

                if (decompress_page_to_file(&stream, fout,
                                            &compressed_total,
                                            &uncompressed_total) != 0) {
                    WARN("P3P RX: decompress_page_to_file() failed for partial page");
                }
                page_stream_free(&stream);
                page_stream_init(&stream);
                page_has_data    = 0;
                bursts_completed = 0;
                memset(burst_received, 0, sizeof(burst_received));
            }

            current_page_id    = buf[2];
            total_pages        = buf[3];
            expected_bursts    = decode_u16_le(&buf[4]);
            last_burst_frames  = buf[6];
            last_frame_bytes   = buf[7];

            memset(burst_received, 0, sizeof(burst_received));
            bursts_completed = 0;
            page_has_data    = 0;
            in_burst         = 0;
            have_page_info   = 1;

            INFO("P3P RX: STREAM_INFO Page=%u/%u -> bursts=%u, last_frames=%u, last_frame_bytes=%u",
                 (unsigned)current_page_id, (unsigned)total_pages,
                 (unsigned)expected_bursts,
                 (unsigned)last_burst_frames, (unsigned)last_frame_bytes);
            continue;
        }

        /* TRANSFER_FINISH */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == MSG_TRANSFER_FINISH) {
            INFO("P3P RX: received TRANSFER_FINISH; finishing assembly");
            transfer_finished = 1;
            break;
        }

        /* BURST_INFO */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == MSG_BURST_INFO) {
            if (len < 6) {
                WARN("P3P RX: malformed BURST_INFO");
                continue;
            }

            uint8_t page_id  = buf[2];
            uint8_t burst_id = buf[3];
            uint16_t size_of_burst = decode_u16_le(&buf[4]);

            if (!have_page_info || page_id != current_page_id) {
                WARN("P3P RX: BURST_INFO for Page %u while expecting Page %u",
                     (unsigned)page_id, (unsigned)current_page_id);
                continue;
            }

            if (burst_id >= expected_bursts) {
                WARN("P3P RX: BURST_INFO burst_id=%u >= expected_bursts=%u",
                     (unsigned)burst_id, (unsigned)expected_bursts);
                continue;
            }

            /* Compute frames_in_burst from size_of_burst */
            frames_in_burst = (uint8_t)((size_of_burst + MAX_PAYLOAD - 1) / MAX_PAYLOAD);
            if (frames_in_burst == 0 || frames_in_burst > MAX_CHUNKS_PER_BURST) {
                WARN("P3P RX: invalid frames_in_burst=%u", frames_in_burst);
                continue;
            }

            uint8_t last_len = (uint8_t)(size_of_burst % MAX_PAYLOAD);
            if (last_len == 0) last_len = MAX_PAYLOAD;

            for (unsigned i = 0; i < frames_in_burst; ++i) {
                sizes[i] = (i == frames_in_burst - 1) ? last_len : MAX_PAYLOAD;
                memset(current_burst[i], 0, MAX_PAYLOAD);
            }

            in_burst = 1;
            continue;
        }

        /* DATA frame */
        if (!in_burst) {
            WARN("P3P RX: DATA frame received before BURST_INFO, ignoring");
            continue;
        }

        if (len == 0) {
            WARN("P3P RX: empty DATA frame");
            continue;
        }

        uint8_t frame_id = buf[0];
        if (frame_id >= frames_in_burst) {
            WARN("P3P RX: invalid frame_id=%u (frames_in_burst=%u)",
                 frame_id, frames_in_burst);
            continue;
        }

        if (len != sizes[frame_id]) {
            WARN("P3P RX: frame len=%u does not match expected=%u for frame %u",
                 len, sizes[frame_id], frame_id);
            continue;
        }

        memcpy(current_burst[frame_id], buf, len);

        /* If this is the last frame, compute checksum, store burst, send checksum */
        if (frame_id == frames_in_burst - 1) {
            uint64_t chk_state;
            checksum_init(&chk_state);
            for (unsigned i = 0; i < frames_in_burst; ++i) {
                checksum_update(&chk_state, current_burst[i], sizes[i]);
            }
            uint8_t checksum_bytes[CHECKSUM_SIZE];
            checksum_final(chk_state, checksum_bytes);

            SUCC("P3P RX: completed BURST for Page %u, checksum=0x%016llX",
                 (unsigned)current_page_id,
                 (unsigned long long)chk_state);

            /* STORE burst immediately; this survives lost ACKs */
            static uint16_t this_burst_id = 0;
            /* We do not have burst_id from BURST_INFO stored here; to keep it
               simple, we just append bursts in arrival order. For robustness
               with reordering we would carry burst_id around. */
            uint16_t bid = bursts_completed; /* monotone per page */

            if (store_burst(&stream,
                            bid,
                            frames_in_burst,
                            current_burst,
                            sizes) != 0) {
                ERROR("P3P RX: store_burst failed");
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            bursts_completed++;
            page_has_data = 1;

            /* Send checksum back to TX (bounded attempts) */
            if (nrf24_set_mode_tx(&radio) < 0) {
                ERROR("P3P RX: nrf24_set_mode_tx failed");
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            const double send_deadline_ms = 500.0;
            double send_start = now_seconds();
            unsigned attempt = 0;
            int checksum_sent_ok = 0;

            while (!checksum_sent_ok &&
                   (now_seconds() - send_start) * 1000.0 < send_deadline_ms) {
                int ret2 = nrf24_send_blocking(&radio,
                                               checksum_bytes,
                                               CHECKSUM_SIZE,
                                               CONTROL_TIMEOUT_MS);
                if (ret2 == 0) {
                    checksum_sent_ok = 1;
                    break;
                }

                if (errno != ETIMEDOUT) {
                    ERROR("P3P RX: nrf24_send_blocking(CHECKSUM) failed: %s",
                          strerror(errno));
                    page_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }

                attempt++;
                if (attempt == 1 || (attempt % 50) == 0) {
                    WARN("P3P RX: CHECKSUM timeout (no ACK) attempt %u", attempt);
                }

                if (attempt % 200 == 0) {
                    WARN("P3P RX: %u checksum timeouts, reconfiguring radio", attempt);
                    (void)nrf24_configure_quick(&radio, P2P_CHANNEL);
                    (void)nrf24_set_mode_tx(&radio);
                }
            }

            if (!checksum_sent_ok) {
                WARN("P3P RX: checksum timeout; returning to RX (TX may resend)");
            } else {
                SUCC("P3P RX: checksum sent successfully for last burst");
            }

            in_burst = 0;

            if (nrf24_set_mode_rx(&radio) < 0) {
                ERROR("P3P RX: nrf24_set_mode_rx failed");
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            /* If we think this page is complete, decompress it */
            if (have_page_info && bursts_completed >= expected_bursts) {
                SUCC("P3P RX: page %u seems complete, decompressing",
                     (unsigned)current_page_id);

                if (decompress_page_to_file(&stream, fout,
                                            &compressed_total,
                                            &uncompressed_total) != 0) {
                    ERROR("P3P RX: decompress_page_to_file failed for page %u",
                          (unsigned)current_page_id);
                }

                page_stream_free(&stream);
                page_stream_init(&stream);
                bursts_completed = 0;
                memset(burst_received, 0, sizeof(burst_received));
                page_has_data  = 0;
                have_page_info = 0;
            }
        }
    }

    /* If we exit without TRANSFER_FINISH but still have a partial page, try to decompress */
    if (have_page_info && page_has_data) {
        WARN("P3P RX: transfer ended unexpectedly; decompressing last partial page %u",
             (unsigned)current_page_id);
        (void)decompress_page_to_file(&stream, fout,
                                      &compressed_total,
                                      &uncompressed_total);
    }

    double t_end = now_seconds();
    double dt    = (tx_started ? (t_end - t_start) : 0.0);

    double user_kibps = (dt > 0.0)
        ? ((double)uncompressed_total / 1024.0 / dt)
        : 0.0;

    double rf_kibps = (dt > 0.0)
        ? ((double)rf_bytes_total / 1024.0 / dt)
        : 0.0;

    SUCC("P3P RX: done. Compressed %llu B -> %llu B uncompressed in %.3f s "
         "(%.1f KiB/s user data, %.1f KiB/s RF on-air, %llu RF frames).",
         (unsigned long long)compressed_total,
         (unsigned long long)uncompressed_total,
         dt, user_kibps, rf_kibps,
         (unsigned long long)rf_frames_total);

    page_stream_free(&stream);
    fclose(fout);
    nrf24_deinit(&radio);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* CLI + main                                                                */
/* ------------------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s tx <spi_device> <ce_gpio> <input_file>\n"
            "  %s rx <spi_device> <ce_gpio> <output_file>\n"
            "Example:\n"
            "  %s tx /dev/spidev0.0 22 test_files/quijote.txt\n"
            "  %s rx /dev/spidev0.0 22 received_file.txt\n",
            prog, prog, prog, prog);
}

static void ensure_logs_dir(void)
{
    /* Best-effort create logs/ */
#if defined(_WIN32)
    _mkdir("logs");
#else
    mkdir("logs", 0777);
#endif
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        usage(argv[0]);
        return 1;
    }

    const char *mode    = argv[1];
    const char *spi_dev = argv[2];
    int ce_bcm          = atoi(argv[3]);
    const char *path    = argv[4];

    if (ce_bcm < 0 || ce_bcm > 255) {
        fprintf(stderr, "Invalid CE GPIO: %d\n", ce_bcm);
        return 1;
    }

    ensure_logs_dir();

    char ts[32];
    logger_timestamp(ts, sizeof(ts));

    char log_path[128];
    if (strcmp(mode, "tx") == 0) {
        snprintf(log_path, sizeof(log_path), "logs/p3p_TX_%s.log", ts);
    } else if (strcmp(mode, "rx") == 0) {
        snprintf(log_path, sizeof(log_path), "logs/p3p_RX_%s.log", ts);
    } else {
        usage(argv[0]);
        return 1;
    }

    if (logger_init(log_path) != 0) {
        fprintf(stderr, "[LOGGER WARN]: could not open '%s', continuing console-only\n",
                log_path);
    } else {
        logger_info("Logging to file '%s'", log_path);
    }

    int ret;
    if (strcmp(mode, "tx") == 0) {
        ret = run_tx(spi_dev, ce_bcm, path);
    } else {
        ret = run_rx(spi_dev, ce_bcm, path);
    }

    logger_close();
    return ret;
}
