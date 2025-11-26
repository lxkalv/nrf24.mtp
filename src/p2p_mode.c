#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <zlib.h>

#include "libs/nrf24.h"
#include "libs/utils.h"

/* ---- Protocol constants ---- */

#define P2P_CHANNEL          90

#define BURST_DATA_MAX       7905   /* bytes of DATA (excluding ChunkID) per burst */
#define CHUNK_DATA_MAX       31     /* bytes of DATA per frame (<=31 so payload<=32) */
#define MAX_PAYLOAD          32     /* max nRF24 payload size */
#define MAX_CHUNKS_PER_BURST 255    /* 7905 / 31 = 255 */

#define CHUNK_DATA_BYTES     CHUNK_DATA_MAX
#define MAX_FRAMES_PER_BURST MAX_CHUNKS_PER_BURST
#define BURST_DATA_BYTES     (CHUNK_DATA_BYTES * MAX_FRAMES_PER_BURST) /* 7905 */

#define CHECKSUM_TIMEOUT_MS  1000   /* wait up to 1 s for checksum */
#define CONTROL_TIMEOUT_MS   100    /* per-attempt timeout when sending control frames */
#define DATA_TIMEOUT_MS      20     /* per-attempt timeout when sending data frames */

#define CHECKSUM_SIZE        8      /* 64-bit FNV-1a checksum */

/* Control message IDs */
#define MSG_INFO             0xFF
#define MSG_BURST_INFO       0xF0
#define MSG_TRANSFER_FINISH  0x0F
#define P2P_MSG_STREAM_INFO  0xE0  /* per-page layout info */

/* Paging */
#define P2P_NUM_PAGES        10
#define MAX_BURSTS_PER_PAGE  255   /* burst_id is 8-bit on the air */
#define MAX_PAGES            16    /* safety margin for page_finished array */

/* ---- Time helper ---- */

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- Little-endian helpers ---- */

static void encode_u16_le(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t decode_u16_le(const uint8_t *src)
{
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static void encode_u64_le(uint8_t *dst, uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        dst[i] = (uint8_t)(v & 0xFFu);
        v >>= 8;
    }
}

/* ---- 64-bit FNV-1a checksum ---- */

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
    encode_u64_le(out, state);
}

/* ---- nRF24 convenience wrappers ---- */
/* Counts every on-air attempt (including retries) into rf_bytes_total / rf_frames_total. */

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
        /* Account RF usage for this attempt */
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

        /* Keep retrying forever, but every so often we reconfigure the radio */
        if (attempt % 500 == 0) {
            WARN("%s: %u consecutive timeouts, reconfiguring radio",
                 what, attempt);
            (void)nrf24_configure_quick(radio, P2P_CHANNEL);
        }
    }
}

/* ---- RX-side in-memory burst storage (one page at a time) ---- */

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

/* Initialise an empty page stream */
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

/* Ensure we have at least (burst_id+1) bursts allocated. */
static Burst *page_get_burst(PageStream *ps, unsigned burst_id)
{
    if (burst_id >= ps->capacity) {
        size_t new_cap = ps->capacity ? ps->capacity * 2 : 8;
        while (burst_id >= new_cap) new_cap *= 2;

        Burst *new_bursts = (Burst *)calloc(new_cap, sizeof(Burst));
        if (!new_bursts) {
            return NULL;
        }
        /* copy existing bursts */
        for (size_t i = 0; i < ps->count; ++i) {
            new_bursts[i] = ps->bursts[i];
        }
        free(ps->bursts);
        ps->bursts   = new_bursts;
        ps->capacity = new_cap;
    }

    if (burst_id >= ps->count) {
        /* initialise new bursts as empty */
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

/* Store a fully-received burst into PageStream[burst_id]. Overwrites old one if present. */
static int store_burst(PageStream *ps,
                       unsigned burst_id,
                       unsigned frames_in_burst,
                       uint8_t current_burst[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD],
                       const uint8_t sizes[MAX_CHUNKS_PER_BURST])
{
    Burst *b = page_get_burst(ps, burst_id);
    if (!b) {
        ERROR("store_burst: out of memory for burst %u", burst_id);
        return -1;
    }

    /* free any existing contents */
    free_burst(b);

    b->frames_in_burst = frames_in_burst;

    for (unsigned i = 0; i < frames_in_burst; ++i) {
        size_t len = sizes[i];
        b->frame_data[i] = (uint8_t *)malloc(len);
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

/* ---- Decompress a single page (PageStream) and append to fout ---- */

static int decompress_page_to_file(PageStream *ps,
                                   FILE *fout,
                                   uint64_t *compressed_total,
                                   uint64_t *uncompressed_total)
{
    if (!ps->bursts || ps->count == 0) {
        return 0; /* nothing in this page */
    }

    z_stream zs;
    memset(&zs, 0, sizeof(zs));

    int zret = inflateInit(&zs);
    if (zret != Z_OK) {
        ERROR("P2P RX: inflateInit failed (zret=%d); writing compressed page as-is", zret);

        /* Fallback: write raw compressed bytes if we cannot inflate at all */
        for (size_t bid = 0; bid < ps->count; ++bid) {
            Burst *b = &ps->bursts[bid];
            if (b->frames_in_burst == 0) continue;
            for (unsigned i = 0; i < b->frames_in_burst; ++i) {
                uint8_t *frame = b->frame_data[i];
                uint8_t  flen  = b->frame_len[i];
                if (!frame || flen <= 1) continue;
                size_t data_len = flen - 1;
                if (fwrite(frame + 1, 1, data_len, fout) != data_len) {
                    ERROR("P2P RX: fwrite failed in fallback");
                    return -1;
                }
                *compressed_total += data_len;
            }
        }
        return -1;
    }

    uint8_t outbuf[4096];
    int end_reached = 0;

    for (size_t bid = 0; bid < ps->count && !end_reached; ++bid) {
        Burst *b = &ps->bursts[bid];
        if (b->frames_in_burst == 0) continue;

        for (unsigned i = 0; i < b->frames_in_burst && !end_reached; ++i) {
            uint8_t *frame = b->frame_data[i];
            uint8_t  flen  = b->frame_len[i];
            if (!frame || flen <= 1) continue;

            const uint8_t *in     = frame + 1;   /* skip ChunkID */
            size_t         in_len = flen - 1;

            *compressed_total += in_len;

            zs.next_in  = (Bytef *)in;
            zs.avail_in = (uInt)in_len;

            while (zs.avail_in > 0) {
                zs.next_out  = outbuf;
                zs.avail_out = sizeof(outbuf);

                zret = inflate(&zs, Z_NO_FLUSH);

                if (zret == Z_STREAM_END) {
                    end_reached = 1;
                } else if (zret != Z_OK) {
                    WARN("P2P RX: decompression error (zret=%d) in page; partial page written", zret);
                    end_reached = 1;
                }

                size_t have = sizeof(outbuf) - zs.avail_out;
                if (have > 0) {
                    if (fwrite(outbuf, 1, have, fout) != have) {
                        ERROR("P2P RX: fwrite failed during decompression");
                        inflateEnd(&zs);
                        return -1;
                    }
                    *uncompressed_total += have;
                }

                if (zret == Z_STREAM_END ||
                    zret == Z_DATA_ERROR ||
                    zret == Z_MEM_ERROR ||
                    zret == Z_STREAM_ERROR) {
                    break;
                }
            }
        }
    }

    inflateEnd(&zs);
    return 0;
}

/* ---- TX: read whole file and send in 10 compressed pages ---- */

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

    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        ERROR("Cannot open input file '%s': %s", input_path, strerror(errno));
        nrf24_deinit(&radio);
        return 1;
    }

    /* Read entire file into memory */
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
            ERROR("P2P TX: malloc(%ld) for input buffer failed", fsize);
            fclose(fin);
            nrf24_deinit(&radio);
            return 1;
        }
        size_t nread = fread(orig_buf, 1, (size_t)orig_len, fin);
        if (nread != (size_t)orig_len) {
            ERROR("P2P TX: fread() got %zu / %llu bytes", nread, (unsigned long long)orig_len);
            free(orig_buf);
            fclose(fin);
            nrf24_deinit(&radio);
            return 1;
        }
    }
    fclose(fin);

    INFO("P2P TX: sending file '%s' (%llu bytes, split into %d pages)",
         input_path, (unsigned long long)orig_len, P2P_NUM_PAGES);

    double t_start = now_seconds();

    uint64_t total_compressed   = 0;
    uint64_t tx_rf_bytes_total  = 0;  /* all on-air bytes (incl. retransmissions) */
    uint64_t tx_rf_frames_total = 0;  /* all frames actually sent */

    /* Partition into pages */
    for (unsigned page_id = 0; page_id < P2P_NUM_PAGES; ++page_id) {
        /* Compute [start, end) for this page using proportional split */
        uint64_t page_start = (orig_len * page_id)     / P2P_NUM_PAGES;
        uint64_t page_end   = (orig_len * (page_id+1)) / P2P_NUM_PAGES;
        if (page_start >= orig_len) {
            break;  /* no data for further pages */
        }
        if (page_end > orig_len) page_end = orig_len;

        uint64_t page_len = page_end - page_start;
        if (page_len == 0) {
            continue;
        }

        INFO("P2P TX: Page %u -> %llu bytes", page_id, (unsigned long long)page_len);

        /* Compress this page with zlib (level 6) */
        const uint8_t *page_src = orig_buf + page_start;
        uLong src_len  = (uLong)page_len;
        uLong dest_len = compressBound(src_len);
        uint8_t *comp_page = (uint8_t *)malloc(dest_len);
        if (!comp_page) {
            ERROR("P2P TX: malloc(%lu) for compressed page failed", (unsigned long)dest_len);
            free(orig_buf);
            nrf24_deinit(&radio);
            return 1;
        }

        int zret = compress2(comp_page, &dest_len, page_src, src_len, 6);
        if (zret != Z_OK) {
            ERROR("P2P TX: compress2 failed for page %u (zret=%d)", page_id, zret);
            free(comp_page);
            free(orig_buf);
            nrf24_deinit(&radio);
            return 1;
        }

        size_t comp_len = (size_t)dest_len;
        total_compressed += comp_len;

        double ratio = (page_len == 0)
                     ? 0.0
                     : (100.0 * (double)comp_len / (double)page_len);

        INFO("P2P TX: Page %u compressed %llu -> %zu bytes (~%.2f%%)",
             page_id, (unsigned long long)page_len, comp_len, ratio);

        /* Layout of this page in bursts */
        uint16_t bursts_in_page     = 0;
        uint8_t  last_burst_frames  = 0;
        uint8_t  last_frame_bytes   = 0;

        if (comp_len > 0) {
            bursts_in_page = (uint16_t)((comp_len + BURST_DATA_BYTES - 1) / BURST_DATA_BYTES);
            if (bursts_in_page > MAX_BURSTS_PER_PAGE) {
                WARN("P2P TX: bursts_in_page=%u > MAX_BURSTS_PER_PAGE=%u; truncating",
                     bursts_in_page, (unsigned)MAX_BURSTS_PER_PAGE);
                bursts_in_page = MAX_BURSTS_PER_PAGE;
            }

            size_t last_burst_bytes =
                comp_len - (size_t)(bursts_in_page - 1) * BURST_DATA_BYTES;

            last_burst_frames = (uint8_t)((last_burst_bytes + CHUNK_DATA_BYTES - 1) /
                                          CHUNK_DATA_BYTES);

            size_t used_by_prev_frames =
                (size_t)(last_burst_frames - 1) * CHUNK_DATA_BYTES;
            last_frame_bytes = (uint8_t)(last_burst_bytes - used_by_prev_frames);
            if (last_frame_bytes == 0) {
                last_frame_bytes = CHUNK_DATA_BYTES;
            }
        }

        /* STREAM_INFO for this page:
         * [0] MSG_INFO
         * [1] P2P_MSG_STREAM_INFO
         * [2] PageID
         * [3] Total pages
         * [4..5] bursts_in_page (u16 LE)
         * [6] last_burst_frames
         * [7] last_frame_bytes
         */
        uint8_t stream_info[8];
        stream_info[0] = MSG_INFO;
        stream_info[1] = P2P_MSG_STREAM_INFO;
        stream_info[2] = (uint8_t)page_id;
        stream_info[3] = (uint8_t)P2P_NUM_PAGES;
        encode_u16_le(&stream_info[4], bursts_in_page);
        stream_info[6] = last_burst_frames;
        stream_info[7] = last_frame_bytes;

        INFO("P2P TX: STREAM_INFO Page=%u/%u -> bursts=%u, last_frames=%u, last_frame_bytes=%u",
             page_id, P2P_NUM_PAGES, (unsigned)bursts_in_page,
             (unsigned)last_burst_frames, (unsigned)last_frame_bytes);

        (void)send_with_retries(&radio,
                                stream_info,
                                sizeof(stream_info),
                                CONTROL_TIMEOUT_MS,
                                "STREAM_INFO",
                                &tx_rf_bytes_total,
                                &tx_rf_frames_total);

        /* Now send all bursts for this page */
        size_t comp_pos = 0;
        uint8_t burst_id = 0;

        while (comp_pos < comp_len) {
            uint8_t burst_payloads[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD];
            uint8_t burst_sizes  [MAX_CHUNKS_PER_BURST];
            unsigned num_chunks   = 0;

            size_t   burst_data_bytes = 0;
            uint16_t burst_onair_bytes = 0;

            uint64_t chk_state;
            checksum_init(&chk_state);

            while (comp_pos < comp_len &&
                   burst_data_bytes < BURST_DATA_MAX &&
                   num_chunks < MAX_CHUNKS_PER_BURST) {

                size_t max_data  = CHUNK_DATA_BYTES;
                size_t remaining = comp_len - comp_pos;
                if (remaining < max_data) {
                    max_data = remaining;
                }

                if (burst_data_bytes + max_data > BURST_DATA_MAX) {
                    max_data = BURST_DATA_MAX - burst_data_bytes;
                }

                if (max_data == 0) {
                    break;
                }

                uint8_t chunk_data[CHUNK_DATA_BYTES];
                memcpy(chunk_data, comp_page + comp_pos, max_data);
                comp_pos += max_data;

                uint8_t chunk_id = (uint8_t)num_chunks;
                burst_payloads[num_chunks][0] = chunk_id;
                memcpy(&burst_payloads[num_chunks][1], chunk_data, max_data);

                uint8_t payload_len = (uint8_t)(1 + max_data);
                burst_sizes[num_chunks] = payload_len;

                checksum_update(&chk_state, burst_payloads[num_chunks], payload_len);

                burst_data_bytes  += max_data;
                burst_onair_bytes += payload_len;
                num_chunks++;
            }

            if (num_chunks == 0) {
                break;
            }

            uint8_t checksum_bytes[CHECKSUM_SIZE];
            checksum_final(chk_state, checksum_bytes);

            INFO("P2P TX: Page %u, BURST %u -> %u compressed bytes in %u frames, checksum 0x%016llX",
                 page_id, burst_id, (unsigned)burst_data_bytes, num_chunks,
                 (unsigned long long)chk_state);

            /* Outer loop: send this burst until RX confirms checksum */
            int burst_done = 0;
            while (!burst_done) {
                /* 1) send BURST_INFO */
                uint8_t burst_info[6];
                burst_info[0] = MSG_INFO;
                burst_info[1] = MSG_BURST_INFO;
                burst_info[2] = (uint8_t)page_id;
                burst_info[3] = (uint8_t)burst_id;
                encode_u16_le(&burst_info[4], burst_onair_bytes);

                if (send_with_retries(&radio,
                                      burst_info,
                                      sizeof(burst_info),
                                      CONTROL_TIMEOUT_MS,
                                      "BURST_INFO",
                                      &tx_rf_bytes_total,
                                      &tx_rf_frames_total) < 0) {
                    ERROR("Failed to send BURST_INFO (page %u, burst %u), aborting",
                          page_id, burst_id);
                    free(comp_page);
                    free(orig_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }

                /* 2) send all data frames for this burst */
                for (unsigned i = 0; i < num_chunks; ++i) {
                    if (send_with_retries(&radio,
                                          burst_payloads[i],
                                          burst_sizes[i],
                                          DATA_TIMEOUT_MS,
                                          "DATA",
                                          &tx_rf_bytes_total,
                                          &tx_rf_frames_total) < 0) {
                        ERROR("Failed to send DATA frame (page %u, burst %u), aborting",
                              page_id, burst_id);
                        free(comp_page);
                        free(orig_buf);
                        nrf24_deinit(&radio);
                        return 1;
                    }
                }

                /* 3) listen for checksum from RX */
                if (nrf24_set_mode_rx(&radio) < 0) {
                    ERROR("nrf24_set_mode_rx failed");
                    free(comp_page);
                    free(orig_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }

                double wait_start         = now_seconds();
                int    got_valid_checksum = 0;

                while (!got_valid_checksum &&
                       (now_seconds() - wait_start) * 1000.0 < CHECKSUM_TIMEOUT_MS) {

                    uint8_t buf2[NRF24_MAX_PAYLOAD_SIZE];
                    uint8_t len2 = sizeof(buf2);
                    int ret2 = nrf24_recv_blocking(&radio, buf2, &len2, 50);
                    if (ret2 < 0) {
                        if (errno == ETIMEDOUT) {
                            continue;
                        }
                        ERROR("nrf24_recv_blocking (checksum) failed: %s", strerror(errno));
                        free(comp_page);
                        free(orig_buf);
                        nrf24_deinit(&radio);
                        return 1;
                    }

                    if (len2 != CHECKSUM_SIZE) {
                        WARN("P2P TX: received non-checksum frame of %u bytes while waiting", len2);
                        continue;
                    }

                    if (memcmp(buf2, checksum_bytes, CHECKSUM_SIZE) == 0) {
                        SUCC("P2P TX: Page %u, BURST %u checksum confirmed by RX",
                             page_id, burst_id);
                        got_valid_checksum = 1;
                    } else {
                        WARN("P2P TX: invalid checksum received for Page %u, BURST %u",
                             page_id, burst_id);
                    }
                }

                if (!got_valid_checksum) {
                    WARN("P2P TX: checksum timeout for Page %u, BURST %u, resending burst",
                         page_id, burst_id);
                    if (nrf24_set_mode_tx(&radio) < 0) {
                        ERROR("nrf24_set_mode_tx failed");
                        free(comp_page);
                        free(orig_buf);
                        nrf24_deinit(&radio);
                        return 1;
                    }
                    continue;  /* resend same burst */
                }

                burst_done = 1;

                if (nrf24_set_mode_tx(&radio) < 0) {
                    ERROR("nrf24_set_mode_tx failed");
                    free(comp_page);
                    free(orig_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }
            }

            burst_id++;
        }

        free(comp_page);
    }

    /* Send TRANSFER_FINISH control message */
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

    SUCC("P2P TX: done. User: %llu bytes in %.3f s (%.1f KiB/s). "
         "RF on-air: %llu bytes in %.3f s (%.1f KiB/s, %llu frames).",
         (unsigned long long)orig_len, dt, user_kibps,
         (unsigned long long)tx_rf_bytes_total, dt, rf_kibps,
         (unsigned long long)tx_rf_frames_total);

    free(orig_buf);
    nrf24_deinit(&radio);
    return 0;
}

/* ---- RX: receive in pages, decompress each page independently ---- */

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

    INFO("P2P RX: waiting for STREAM_INFO / BURST_INFO on channel %d...", P2P_CHANNEL);

    int transfer_finished = 0;
    int tx_started        = 0;
    double t_start        = 0.0;

    /* Per-page "finished" flags: once a page is appended, we won't append again,
       but we will still answer duplicate bursts with checksums. */
    int page_finished[MAX_PAGES];
    memset(page_finished, 0, sizeof(page_finished));

    /* Per-page state (current page in progress) */
    int      have_page_info      = 0;
    uint8_t  current_page_id     = 0;
    uint8_t  total_pages         = 0;
    uint16_t expected_bursts     = 0;
    uint8_t  last_burst_frames   = 0;
    uint8_t  last_frame_bytes    = 0;
    uint8_t  burst_received[MAX_BURSTS_PER_PAGE];
    unsigned bursts_completed    = 0;
    int      page_has_data       = 0;  /* any stored bursts? */

    /* Per-burst state (for current burst) */
    int      in_burst            = 0;
    uint8_t  cur_burst_page_id   = 0;
    unsigned cur_burst_id        = 0;
    unsigned frames_in_burst     = 0;
    uint8_t  sizes[MAX_CHUNKS_PER_BURST];
    uint8_t  current_burst[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD];

    /* Throughput / counters */
    uint64_t compressed_total    = 0;
    uint64_t uncompressed_total  = 0;
    uint64_t rf_bytes_total      = 0;  /* all bytes received and sent (checksums) */
    uint64_t rf_frames_total     = 0;  /* frames RX+TX */

    while (!transfer_finished) {
        uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
        uint8_t len = sizeof(buf);

        int ret = nrf24_recv_blocking(&radio, buf, &len, 0); /* block */
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                /* with timeout=0 this should not happen, but ignore safely */
                continue;
            }
            ERROR("nrf24_recv_blocking failed (errno=%d: %s). "
                  "Stopping RX loop and assembling what we have.",
                  errno, strerror(errno));
            /* We will break and decompress what we have so far. */
            break;
        }

        if (len > 0) {
            rf_bytes_total  += len;
            rf_frames_total += 1;
        }

        if (!tx_started) {
            t_start    = now_seconds();
            tx_started = 1;
        }

        /* STREAM_INFO: per-page layout */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == P2P_MSG_STREAM_INFO) {
            if (len < 8) {
                WARN("P2P RX: malformed STREAM_INFO frame (len=%u)", len);
                continue;
            }

            /* If we had an unfinished page with data, decompress it before moving on */
            if (have_page_info && page_has_data) {
                WARN("P2P RX: new STREAM_INFO while previous page %u has data; "
                     "decompressing previous page as-is",
                     (unsigned)current_page_id);

                if (current_page_id < MAX_PAGES && !page_finished[current_page_id]) {
                    (void)decompress_page_to_file(&stream, fout,
                                                  &compressed_total, &uncompressed_total);
                    page_finished[current_page_id] = 1;
                }
                page_stream_free(&stream);
                page_stream_init(&stream);
                page_has_data    = 0;
                bursts_completed = 0;
                memset(burst_received, 0, sizeof(burst_received));
            }

            current_page_id = buf[2];
            total_pages     = buf[3];
            expected_bursts = decode_u16_le(&buf[4]);
            last_burst_frames = buf[6];
            last_frame_bytes  = buf[7];

            if (expected_bursts > MAX_BURSTS_PER_PAGE) {
                WARN("P2P RX: expected_bursts=%u > MAX_BURSTS_PER_PAGE=%u, truncating",
                     (unsigned)expected_bursts, (unsigned)MAX_BURSTS_PER_PAGE);
                expected_bursts = MAX_BURSTS_PER_PAGE;
            }

            memset(burst_received, 0, sizeof(burst_received));
            bursts_completed = 0;
            page_has_data    = 0;
            in_burst         = 0;
            have_page_info   = 1;

            INFO("P2P RX: STREAM_INFO Page=%u/%u -> bursts=%u, last_frames=%u, last_frame_bytes=%u",
                 (unsigned)current_page_id, (unsigned)total_pages,
                 (unsigned)expected_bursts,
                 (unsigned)last_burst_frames, (unsigned)last_frame_bytes);
            continue;
        }

        /* BURST_INFO */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == MSG_BURST_INFO) {
            if (len < 6) {
                WARN("P2P RX: malformed BURST_INFO frame");
                continue;
            }

            uint8_t  page_id       = buf[2];
            uint8_t  burst_id      = buf[3];
            uint16_t size_of_burst = decode_u16_le(&buf[4]);

            int is_current_page  = (have_page_info && page_id == current_page_id);
            int is_finished_page = (page_id < MAX_PAGES && page_finished[page_id]);

            if (!is_current_page && !is_finished_page) {
                /* --- NUEVO: intento de resincronización --- */

                /* Caso típico: hemos perdido el STREAM_INFO de una página nueva.
                * Aceptamos este BURST_INFO como inicio de la nueva página.
                * Solo lo hacemos si:
                *   - aún no teníamos página activa, o
                *   - la página es posterior a la actual (TX ha avanzado).
                */
                if (!have_page_info || page_id > current_page_id) {
                    WARN("P2P RX: BURST_INFO para page=%u sin STREAM_INFO previo "
                        "(current=%u, have_page_info=%d); resincronizando con esta página",
                        (unsigned)page_id, (unsigned)current_page_id, have_page_info);

                    /* Si la página anterior tenía datos y no estaba marcada como terminada,
                    * la descomprimimos tal y como hacemos cuando llega un STREAM_INFO nuevo.
                    */
                    if (have_page_info &&
                        page_has_data &&
                        current_page_id < MAX_PAGES &&
                        !page_finished[current_page_id]) {

                        WARN("P2P RX: la página anterior %u tenía datos; "
                            "descomprimiéndola antes de cambiar de página",
                            (unsigned)current_page_id);

                        (void)decompress_page_to_file(&stream, fout,
                                                    &compressed_total, &uncompressed_total);
                        page_finished[current_page_id] = 1;
                    }

                    /* Reiniciar estado de página */
                    page_stream_free(&stream);
                    page_stream_init(&stream);
                    memset(burst_received, 0, sizeof(burst_received));
                    bursts_completed = 0;
                    page_has_data    = 0;

                    current_page_id   = page_id;
                    expected_bursts   = 0;   /* desconocido, ya no dependemos de STREAM_INFO */
                    last_burst_frames = 0;
                    last_frame_bytes  = 0;
                    have_page_info    = 1;

                    /* A partir de aquí tratamos el BURST_INFO como de la página actual */
                    is_current_page = 1;
                } else {
                    /* Página extraña (anterior no terminada, etc.): la ignoramos como antes */
                    WARN("P2P RX: BURST_INFO for unexpected page=%u (current=%u), ignoring",
                        (unsigned)page_id, (unsigned)current_page_id);
                    continue;
                }
            }

            /* A partir de aquí, o bien era la página actual, o hemos hecho resync arriba */

            cur_burst_page_id = page_id;
            cur_burst_id      = burst_id;

            frames_in_burst = (size_of_burst + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
            if (frames_in_burst == 0 || frames_in_burst > MAX_CHUNKS_PER_BURST) {
                WARN("P2P RX: invalid frames_in_burst=%u", frames_in_burst);
                continue;
            }

            uint8_t last_len = (uint8_t)(size_of_burst % MAX_PAYLOAD);
            if (last_len == 0) last_len = MAX_PAYLOAD;

            for (unsigned i = 0; i < frames_in_burst; ++i) {
                sizes[i] = (i == frames_in_burst - 1) ? last_len : MAX_PAYLOAD;
                memset(current_burst[i], 0, MAX_PAYLOAD);
            }

            INFO("P2P RX: BURST_INFO Page=%u Burst=%u -> size %u B in %u frames",
                (unsigned)page_id, (unsigned)burst_id,
                (unsigned)size_of_burst, frames_in_burst);

            in_burst = 1;
            continue;
        }


        /* TRANSFER_FINISH */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == MSG_TRANSFER_FINISH) {
            INFO("P2P RX: received TRANSFER_FINISH");
            transfer_finished = 1;
            break;
        }

        /* DATA frame */
        if (!in_burst) {
            WARN("P2P RX: DATA frame received before BURST_INFO, ignoring");
            continue;
        }

        if (len == 0) {
            WARN("P2P RX: empty DATA frame");
            continue;
        }

        uint8_t frame_id = buf[0];

        if (frame_id >= frames_in_burst) {
            WARN("P2P RX: invalid FrameID=%u (frames_in_burst=%u)",
                 frame_id, frames_in_burst);
            continue;
        }

        if (len != sizes[frame_id]) {
            WARN("P2P RX: frame len=%u does not match expected=%u for FrameID=%u",
                 len, sizes[frame_id], frame_id);
            continue;
        }

        memcpy(current_burst[frame_id], buf, len);
        page_has_data = 1;

        /* When we receive the last frame, compute checksum, optionally store burst, send checksum */
        if (frame_id == frames_in_burst - 1) {
            /* 1) Compute checksum over the whole burst */
            uint64_t chk_state;
            checksum_init(&chk_state);
            for (unsigned i = 0; i < frames_in_burst; ++i) {
                checksum_update(&chk_state, current_burst[i], sizes[i]);
            }
            uint8_t checksum_bytes[CHECKSUM_SIZE];
            checksum_final(chk_state, checksum_bytes);

            SUCC("P2P RX: completed BURST [P%u|B%u], checksum 0x%016llX",
                 (unsigned)cur_burst_page_id, (unsigned)cur_burst_id,
                 (unsigned long long)chk_state);

            /* 2) Store the burst in memory ONLY if this page has not been finished
             * yet. If it's a duplicate burst for an already-finished page, we don't
             * store or append data, we just send the checksum back.
             */
            int is_finished_page =
                (cur_burst_page_id < MAX_PAGES && page_finished[cur_burst_page_id]);

            if (!is_finished_page) {
                if (store_burst(&stream,
                                cur_burst_id,
                                frames_in_burst,
                                current_burst,
                                sizes) < 0) {
                    ERROR("P2P RX: failed to store burst %u", cur_burst_id);
                    page_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }

                /* Track unique bursts for this page (only if it's a normal, not-yet-finished page) */
                if (cur_burst_id < MAX_BURSTS_PER_PAGE) {
                    if (!burst_received[cur_burst_id]) {
                        burst_received[cur_burst_id] = 1;
                        bursts_completed++;
                    }
                }
            } else {
                INFO("P2P RX: duplicate BURST [P%u|B%u] for finished page, will re-send checksum only",
                     (unsigned)cur_burst_page_id, (unsigned)cur_burst_id);
            }

            /* 3) Try to send checksum back to TX (bounded time) */
            if (nrf24_set_mode_tx(&radio) < 0) {
                ERROR("nrf24_set_mode_tx failed");
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            const double send_deadline_ms = 500.0;  /* total window */
            double send_start = now_seconds();
            unsigned attempt = 0;
            int checksum_sent_ok = 0;

            while (!checksum_sent_ok &&
                   (now_seconds() - send_start) * 1000.0 < send_deadline_ms) {

                rf_bytes_total  += CHECKSUM_SIZE;
                rf_frames_total += 1;

                int ret2 = nrf24_send_blocking(&radio,
                                               checksum_bytes,
                                               CHECKSUM_SIZE,
                                               CONTROL_TIMEOUT_MS);

                if (ret2 == 0) {
                    checksum_sent_ok = 1;
                    break;
                }

                if (errno != ETIMEDOUT) {
                    ERROR("P2P RX: nrf24_send_blocking(CHECKSUM) failed: %s",
                          strerror(errno));
                    page_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }

                attempt++;
                if (attempt == 1 || (attempt % 50) == 0) {
                    WARN("CHECKSUM: timeout (no ACK) on attempt %u for %u-byte frame",
                         attempt, CHECKSUM_SIZE);
                }

                if (attempt % 200 == 0) {
                    WARN("P2P RX: %u checksum timeouts, reconfiguring radio", attempt);
                    (void)nrf24_configure_quick(&radio, P2P_CHANNEL);
                    (void)nrf24_set_mode_tx(&radio);
                }
            }

            if (!checksum_sent_ok) {
                WARN("P2P RX: checksum timeout for Page %u, BURST %u; returning to RX "
                     "(TX may resend it)",
                     (unsigned)cur_burst_page_id, (unsigned)cur_burst_id);

                in_burst = 0;

                if (nrf24_set_mode_rx(&radio) < 0) {
                    ERROR("nrf24_set_mode_rx failed");
                    page_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }
                continue;
            }

            SUCC("P2P RX: checksum for Page %u, BURST %u sent successfully",
                 (unsigned)cur_burst_page_id, (unsigned)cur_burst_id);

            in_burst = 0;

            if (nrf24_set_mode_rx(&radio) < 0) {
                ERROR("nrf24_set_mode_rx failed");
                page_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            /* 4) If this is the "active" page and we know how many bursts it
             * should have, and we've seen them all at least once, decompress
             * this page ONCE and mark it as finished.
             */
            if (!is_finished_page &&
                have_page_info &&
                cur_burst_page_id == current_page_id &&
                expected_bursts > 0 &&
                bursts_completed >= expected_bursts &&
                current_page_id < MAX_PAGES &&
                !page_finished[current_page_id]) {

                SUCC("P2P RX: all %u bursts received for Page %u; decompressing page",
                     (unsigned)expected_bursts, (unsigned)current_page_id);

                (void)decompress_page_to_file(&stream, fout,
                                              &compressed_total, &uncompressed_total);

                page_finished[current_page_id] = 1;

                page_stream_free(&stream);
                page_stream_init(&stream);
                page_has_data    = 0;

                /* Keep have_page_info = 1 so that if TX still resends bursts
                 * for this page, we will recompute and send checksums, but the
                 * page won't be appended again thanks to page_finished[].
                 */
                bursts_completed = expected_bursts;
                memset(burst_received, 0, sizeof(burst_received));
            }
        }
    }

    /* If we exit the loop without TRANSFER_FINISH, we may still have a partial page
     * buffered. Try to decompress what we have (once).
     */
    if (have_page_info && page_has_data &&
        current_page_id < MAX_PAGES && !page_finished[current_page_id]) {
        WARN("P2P RX: transfer ended unexpectedly; decompressing partial Page %u",
             (unsigned)current_page_id);
        (void)decompress_page_to_file(&stream, fout,
                                      &compressed_total, &uncompressed_total);
    }

    double t_end = now_seconds();
    double dt    = (tx_started ? (t_end - t_start) : 0.0);

    double user_kibps = (dt > 0.0)
        ? ((double)uncompressed_total / 1024.0 / dt)
        : 0.0;

    double rf_kibps = (dt > 0.0)
        ? ((double)rf_bytes_total / 1024.0 / dt)
        : 0.0;

    SUCC("P2P RX: done. Compressed %llu B -> %llu B uncompressed in %.3f s "
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

/* ---- CLI ---- */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s tx <spi_device> <ce_gpio> <input_file>\n"
            "  %s rx <spi_device> <ce_gpio> <output_file>\n"
            "Example:\n"
            "  %s tx /dev/spidev0.0 22 test_files/lorem.txt\n"
            "  %s rx /dev/spidev0.0 22 received_file.txt\n",
            prog, prog, prog, prog);
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
        ERROR("Invalid CE GPIO: %d", ce_bcm);
        return 1;
    }

    /* Choose logfile name based on mode */
    char log_path[64];
    if (strcmp(mode, "tx") == 0) {
        snprintf(log_path, sizeof(log_path), "p2p_tx.log");
    } else if (strcmp(mode, "rx") == 0) {
        snprintf(log_path, sizeof(log_path), "p2p_rx.log");
    } else {
        usage(argv[0]);
        return 1;
    }

    if (log_init(log_path) != 0) {
        WARN("Could not open log file '%s' (continuing without file log)", log_path);
    } else {
        INFO("Logging to file '%s'", log_path);
    }

    int ret;
    if (strcmp(mode, "tx") == 0) {
        ret = run_tx(spi_dev, ce_bcm, path);
    } else {
        ret = run_rx(spi_dev, ce_bcm, path);
    }

    log_close();
    return ret;
}
