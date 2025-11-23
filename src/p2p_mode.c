#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <zlib.h>

#include "libs/nrf24.h"
#include "libs/utils.h"

/* ---- Protocol constants (mirroring the Python logic, no compression) ---- */

#define P2P_CHANNEL          76

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

#define MSG_INFO             0xFF
#define MSG_BURST_INFO       0xF0
#define MSG_TRANSFER_FINISH  0x0F

/* New control subtype for stream layout */
#define P2P_MSG_STREAM_INFO  0xE0

/* radio config */
#define RADIO_CONFIG_SPI_SPEED 8000000

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

static int send_with_retries(nrf24_t *radio,
                             const uint8_t *buf,
                             uint8_t len,
                             unsigned int timeout_ms,
                             const char *what)
{
    unsigned int attempt = 0;

    for (;;) {
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
            WARN("%s: %u consecutive timeouts, reconfiguring radio", what, attempt);
            (void)nrf24_configure_quick(radio, P2P_CHANNEL);
        }
    }
}

/* ---- RX-side in-memory burst storage (similar to STREAM in Python) ---- */

typedef struct {
    unsigned frames_in_burst;
    uint8_t *frame_data[MAX_CHUNKS_PER_BURST];
    uint8_t  frame_len[MAX_CHUNKS_PER_BURST];
} Burst;

typedef struct {
    Burst  *bursts;
    size_t  count;
    size_t  capacity;
} Page0Stream;

static void page0_stream_init(Page0Stream *ps)
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

static void page0_stream_free(Page0Stream *ps)
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

/* Ensure we have at least (burst_id+1) bursts allocated in page 0. */
static Burst *page0_get_burst(Page0Stream *ps, unsigned burst_id)
{
    if (burst_id >= ps->capacity) {
        size_t new_cap = ps->capacity ? ps->capacity * 2 : 8;
        while (burst_id >= new_cap) new_cap *= 2;

        Burst *new_bursts = calloc(new_cap, sizeof(Burst));
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

/* Store a fully-received burst into STREAM[Page0][BurstID]. Overwrites old one if present. */
static int store_burst(Page0Stream *ps,
                       unsigned burst_id,
                       unsigned frames_in_burst,
                       uint8_t current_burst[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD],
                       const uint8_t sizes[MAX_CHUNKS_PER_BURST])
{
    Burst *b = page0_get_burst(ps, burst_id);
    if (!b) {
        ERROR("store_burst: out of memory for burst %u", burst_id);
        return -1;
    }

    /* free any existing contents */
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

/* Helper: check if we have all bursts 0..(total_bursts-1) stored */
static int have_all_bursts(const Page0Stream *ps, uint16_t total_bursts)
{
    if (total_bursts == 0) return 0;
    if (ps->count < total_bursts) return 0;

    for (uint16_t i = 0; i < total_bursts; ++i) {
        if (i >= ps->count) return 0;
        const Burst *b = &ps->bursts[i];
        if (b->frames_in_burst == 0) return 0;
    }
    return 1;
}


/* ---- Simple zlib compression helper (whole file -> compressed buffer) ---- */

static int compress_file_zlib(FILE *fin,
                              uint8_t **out_buf,
                              size_t   *out_len,
                              uint64_t *out_uncomp_len)
{
    *out_buf        = NULL;
    *out_len        = 0;
    *out_uncomp_len = 0;

    if (fseek(fin, 0, SEEK_END) != 0) {
        ERROR("compress_file_zlib: fseek failed");
        return -1;
    }
    long fsize = ftell(fin);
    if (fsize < 0) {
        ERROR("compress_file_zlib: ftell failed");
        return -1;
    }
    rewind(fin);

    *out_uncomp_len = (uint64_t)fsize;

    if (fsize == 0) {
        INFO("compress_file_zlib: input file is empty");
        return 0;
    }

    uint8_t *input = (uint8_t *)malloc((size_t)fsize);
    if (!input) {
        ERROR("compress_file_zlib: malloc(%ld) failed", fsize);
        return -1;
    }

    size_t nread = fread(input, 1, (size_t)fsize, fin);
    if (nread != (size_t)fsize) {
        ERROR("compress_file_zlib: fread() got %zu / %ld bytes", nread, fsize);
        free(input);
        return -1;
    }

    uLong src_len  = (uLong)fsize;
    uLong dest_len = compressBound(src_len);
    uint8_t *comp  = (uint8_t *)malloc(dest_len);
    if (!comp) {
        ERROR("compress_file_zlib: malloc(%lu) for compressed buffer failed", (unsigned long)dest_len);
        free(input);
        return -1;
    }

    int zret = compress2(comp, &dest_len, input, src_len, 6);  /* level 6 = same as Python */
    free(input);

    if (zret != Z_OK) {
        ERROR("compress_file_zlib: compress2 failed (zret=%d)", zret);
        free(comp);
        return -1;
    }

    *out_buf = comp;
    *out_len = (size_t)dest_len;

    double ratio = (*out_uncomp_len == 0)
                 ? 0.0
                 : (100.0 * (double)*out_len / (double)*out_uncomp_len);

    INFO("P2P TX: compressed %llu bytes to %zu bytes (~%.2f%%)",
         (unsigned long long)*out_uncomp_len,
         *out_len,
         ratio);

    return 0;
}

/* ---- TX: send file with burst-level reliability ---- */

static int run_tx(const char *spi_dev, int ce_bcm, const char *input_path)
{
    nrf24_t radio;
    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = RADIO_CONFIG_SPI_SPEED,
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

    uint8_t *comp_buf      = NULL;
    size_t   comp_len      = 0;
    uint64_t uncomp_len    = 0;

    if (compress_file_zlib(fin, &comp_buf, &comp_len, &uncomp_len) < 0) {
        ERROR("P2P TX: compression step failed");
        fclose(fin);
        nrf24_deinit(&radio);
        return 1;
    }
    fclose(fin);

    INFO("P2P TX: sending file '%s' (original %llu bytes, compressed %zu bytes)",
         input_path,
         (unsigned long long)uncomp_len,
         comp_len);

    /* ---- STREAM_INFO based on compressed length (for RX safety net) ---- */
    uint16_t total_bursts = 0;
    uint8_t  last_burst_frames = 0;
    uint8_t  last_frame_bytes  = 0;

    if (comp_len > 0) {
        total_bursts = (uint16_t)((comp_len + BURST_DATA_BYTES - 1) / BURST_DATA_BYTES);

        size_t last_burst_bytes =
            comp_len - (size_t)(total_bursts - 1) * BURST_DATA_BYTES;

        last_burst_frames = (uint8_t)((last_burst_bytes + CHUNK_DATA_BYTES - 1) /
                                      CHUNK_DATA_BYTES);

        size_t used_by_prev_frames =
            (size_t)(last_burst_frames - 1) * CHUNK_DATA_BYTES;
        last_frame_bytes = (uint8_t)(last_burst_bytes - used_by_prev_frames);
        if (last_frame_bytes == 0) {
            last_frame_bytes = CHUNK_DATA_BYTES;
        }
    }

    uint8_t stream_info[6];
    stream_info[0] = MSG_INFO;
    stream_info[1] = P2P_MSG_STREAM_INFO;
    encode_u16_le(&stream_info[2], total_bursts);
    stream_info[4] = last_burst_frames;
    stream_info[5] = last_frame_bytes;

    INFO("P2P TX: STREAM_INFO -> bursts=%u, last_frames=%u, last_frame_bytes=%u",
         (unsigned)total_bursts,
         (unsigned)last_burst_frames,
         (unsigned)last_frame_bytes);

    if (send_with_retries(&radio,
                          stream_info,
                          sizeof(stream_info),
                          CONTROL_TIMEOUT_MS,
                          "STREAM_INFO") < 0) {
        WARN("P2P TX: failed to send STREAM_INFO (continuing anyway)");
    }

    double t_start = now_seconds();

    unsigned page_id  = 0; /* still using a single logical page */
    unsigned burst_id = 0;

    size_t comp_pos = 0;

    while (comp_pos < comp_len) {
        /* Build one burst from compressed buffer */
        uint8_t burst_payloads[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD];
        uint8_t burst_sizes  [MAX_CHUNKS_PER_BURST];
        unsigned num_chunks   = 0;

        size_t   burst_data_bytes   = 0;   /* compressed bytes (no headers) */
        uint16_t burst_onair_bytes  = 0;   /* sum of payload lengths */

        uint64_t chk_state;
        checksum_init(&chk_state);

        while (comp_pos < comp_len &&
               burst_data_bytes < BURST_DATA_MAX &&
               num_chunks < MAX_CHUNKS_PER_BURST) {

            size_t max_data   = CHUNK_DATA_BYTES;
            size_t remaining  = comp_len - comp_pos;
            if (remaining < max_data) {
                max_data = remaining;
            }

            if (burst_data_bytes + max_data > BURST_DATA_MAX) {
                max_data = BURST_DATA_MAX - burst_data_bytes;
            }

            if (max_data == 0) {
                break;
            }

            /* Take max_data bytes from compressed buffer */
            uint8_t chunk_data[CHUNK_DATA_BYTES];
            memcpy(chunk_data, comp_buf + comp_pos, max_data);
            comp_pos += max_data;

            uint8_t chunk_id = (uint8_t)num_chunks;
            burst_payloads[num_chunks][0] = chunk_id;
            memcpy(&burst_payloads[num_chunks][1], chunk_data, max_data);

            uint8_t payload_len       = (uint8_t)(1 + max_data);
            burst_sizes[num_chunks]   = payload_len;

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

        INFO("P2P TX: BURST %u -> %u compressed bytes in %u frames, checksum 0x%016llX",
             burst_id, (unsigned)burst_data_bytes, num_chunks,
             (unsigned long long)chk_state);

        /* Outer loop: send this burst until RX confirms checksum */
        int burst_done = 0;
        while (!burst_done) {
            /* 1) send BURST_INFO control frame */
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
                                  "BURST_INFO") < 0) {
                ERROR("Failed to send BURST_INFO, aborting");
                free(comp_buf);
                nrf24_deinit(&radio);
                return 1;
            }

            /* 2) send all data frames for this burst */
            for (unsigned i = 0; i < num_chunks; ++i) {
                if (send_with_retries(&radio,
                                      burst_payloads[i],
                                      burst_sizes[i],
                                      DATA_TIMEOUT_MS,
                                      "DATA") < 0) {
                    ERROR("Failed to send DATA frame, aborting");
                    free(comp_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }
            }

            /* 3) listen for checksum from RX */
            if (nrf24_set_mode_rx(&radio) < 0) {
                ERROR("nrf24_set_mode_rx failed");
                free(comp_buf);
                nrf24_deinit(&radio);
                return 1;
            }

            double wait_start         = now_seconds();
            int    got_valid_checksum = 0;

            while (!got_valid_checksum &&
                   (now_seconds() - wait_start) * 1000.0 < CHECKSUM_TIMEOUT_MS) {

                uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
                uint8_t len = sizeof(buf);
                int ret = nrf24_recv_blocking(&radio, buf, &len, 50);
                if (ret < 0) {
                    if (errno == ETIMEDOUT) {
                        continue; /* try again */
                    }
                    ERROR("nrf24_recv_blocking (checksum) failed: %s", strerror(errno));
                    free(comp_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }

                if (len != CHECKSUM_SIZE) {
                    WARN("P2P TX: received non-checksum frame of %u bytes while waiting", len);
                    continue;
                }

                if (memcmp(buf, checksum_bytes, CHECKSUM_SIZE) == 0) {
                    SUCC("P2P TX: BURST %u checksum confirmed by RX", burst_id);
                    got_valid_checksum = 1;
                } else {
                    WARN("P2P TX: invalid checksum received for BURST %u", burst_id);
                }
            }

            if (!got_valid_checksum) {
                WARN("P2P TX: checksum timeout for BURST %u, resending burst", burst_id);
                if (nrf24_set_mode_tx(&radio) < 0) {
                    ERROR("nrf24_set_mode_tx failed");
                    free(comp_buf);
                    nrf24_deinit(&radio);
                    return 1;
                }
                continue;  /* resend same burst */
            }

            /* checksum OK, move on to next burst */
            burst_done = 1;
            burst_id++;

            if (nrf24_set_mode_tx(&radio) < 0) {
                ERROR("nrf24_set_mode_tx failed");
                free(comp_buf);
                nrf24_deinit(&radio);
                return 1;
            }
        }
    }

    /* Send TRANSFER_FINISH control message */
    uint8_t fin_msg[2];
    fin_msg[0] = MSG_INFO;
    fin_msg[1] = MSG_TRANSFER_FINISH;

    if (send_with_retries(&radio,
                          fin_msg,
                          sizeof(fin_msg),
                          CONTROL_TIMEOUT_MS,
                          "TRANSFER_FINISH") < 0) {
        WARN("P2P TX: failed to send TRANSFER_FINISH (continuing anyway)");
    }

    double t_end   = now_seconds();
    double dt      = t_end - t_start;
    double kibps   = dt > 0.0 ? ((double)uncomp_len / 1024.0 / dt) : 0.0;

    SUCC("P2P TX: done. Original %llu bytes (compressed %zu), in %.3f s (%.1f KiB/s user data)",
         (unsigned long long)uncomp_len,
         comp_len,
         dt,
         kibps);

    free(comp_buf);
    nrf24_deinit(&radio);
    return 0;
}


/* ---- RX: receive file with burst-level reliability ---- */

static int run_rx(const char *spi_dev, int ce_bcm, const char *output_path)
{
    nrf24_t radio;
    nrf24_config_t cfg = {
        .spi_device   = spi_dev,
        .spi_speed_hz = RADIO_CONFIG_SPI_SPEED,
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

    Page0Stream stream;
    page0_stream_init(&stream);

    INFO("P2P RX: waiting for BURST_INFO on channel %d...", P2P_CHANNEL);

    int transfer_finished = 0;
    int tx_started        = 0;
    double t_start = 0.0;

    /* STREAM_INFO state */
    int have_stream_info          = 0;
    uint16_t expected_total_bursts = 0;
    uint8_t  expected_last_frames  = 0;
    uint8_t  expected_last_frame_bytes = 0;

    int waiting_for_finish = 0;
    double finish_deadline = 0.0;

    /* current burst state */
    int in_burst = 0;
    unsigned cur_page_id = 0;
    unsigned cur_burst_id = 0;
    unsigned frames_in_burst = 0;
    uint8_t sizes[MAX_CHUNKS_PER_BURST];
    uint8_t current_burst[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD];

    while (!transfer_finished) {

        /* If we know we have all bursts but never get TRANSFER_FINISH,
         * bail out after a short grace period.
         */
        if (waiting_for_finish) {
            double now = now_seconds();
            if (now >= finish_deadline) {
                WARN("P2P RX: finish timeout after receiving all bursts; "
                     "assuming TRANSFER_FINISH was lost and closing transfer.");
                transfer_finished = 1;
                break;
            }
        }

        uint8_t buf[NRF24_MAX_PAYLOAD_SIZE];
        uint8_t len = sizeof(buf);

        int ret = nrf24_recv_blocking(&radio, buf, &len, 0); /* block */
        if (ret < 0) {
            if (errno == ETIMEDOUT) {
                /* should not happen with timeout=0, but handle just in case */
                continue;
            }
            ERROR("nrf24_recv_blocking failed: %s", strerror(errno));
            page0_stream_free(&stream);
            fclose(fout);
            nrf24_deinit(&radio);
            return 1;
        }

        if (!tx_started) {
            t_start   = now_seconds();
            tx_started = 1;
        }

        /* STREAM_INFO control message */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == P2P_MSG_STREAM_INFO) {
            if (len < 6) {
                WARN("P2P RX: malformed STREAM_INFO frame (len=%u)", len);
                continue;
            }
            expected_total_bursts = decode_u16_le(&buf[2]);
            expected_last_frames  = buf[4];
            expected_last_frame_bytes = buf[5];
            have_stream_info      = 1;
            waiting_for_finish    = 0; /* reset in case of re-send */

            INFO("P2P RX: STREAM_INFO -> bursts=%u, last_frames=%u, last_frame_bytes=%u",
                 (unsigned)expected_total_bursts,
                 (unsigned)expected_last_frames,
                 (unsigned)expected_last_frame_bytes);
            continue;
        }

        /* BURST_INFO control message */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == MSG_BURST_INFO) {
            if (len < 6) {
                WARN("P2P RX: malformed BURST_INFO frame");
                continue;
            }

            cur_page_id  = buf[2];
            cur_burst_id = buf[3];
            uint16_t size_of_burst = decode_u16_le(&buf[4]);

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
                 cur_page_id, cur_burst_id,
                 (unsigned)size_of_burst, frames_in_burst);

            in_burst = 1;
            continue;
        }

        /* TRANSFER_FINISH control message */
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == MSG_TRANSFER_FINISH) {
            transfer_finished = 1;
            INFO("P2P RX: received TRANSFER_FINISH");
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

        /* When we receive the last frame, compute checksum & respond */
        if (frame_id == frames_in_burst - 1) {
            /* ---- 1) Compute checksum over the whole burst ---- */
            uint64_t chk_state;
            checksum_init(&chk_state);
            for (unsigned i = 0; i < frames_in_burst; ++i) {
                checksum_update(&chk_state, current_burst[i], sizes[i]);
            }
            uint8_t checksum_bytes[CHECKSUM_SIZE];
            checksum_final(chk_state, checksum_bytes);

            SUCC("P2P RX: completed BURST [P%u|B%u], checksum 0x%016llX",
                 cur_page_id, cur_burst_id,
                 (unsigned long long)chk_state);

            /* ---- 2) STORE THE BURST IMMEDIATELY ---- */
            if (store_burst(&stream,
                            cur_burst_id,
                            frames_in_burst,
                            current_burst,
                            sizes) < 0) {
                ERROR("P2P RX: failed to store burst %u", cur_burst_id);
                page0_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }

            /* If we know the expected total bursts, check if we already
             * have them all; if so, start a 'finish wait' timer so the
             * transfer can complete even if TRANSFER_FINISH is lost.
             */
            if (have_stream_info &&
                !waiting_for_finish &&
                have_all_bursts(&stream, expected_total_bursts)) {

                WARN("P2P RX: all %u bursts received; waiting for TRANSFER_FINISH or timeout...",
                     (unsigned)expected_total_bursts);
                waiting_for_finish = 1;
                finish_deadline = now_seconds() + 2.0; /* 2 s grace */
            }

            /* ---- 3) Try to send checksum back to TX (bounded time) ---- */
            if (nrf24_set_mode_tx(&radio) < 0) {
                ERROR("nrf24_set_mode_tx failed");
                page0_stream_free(&stream);
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
                    page0_stream_free(&stream);
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
                WARN("P2P RX: checksum timeout for BURST %u, returning to RX "
                     "(TX may resend it)", cur_burst_id);

                in_burst = 0;

                if (nrf24_set_mode_rx(&radio) < 0) {
                    ERROR("nrf24_set_mode_rx failed");
                    page0_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }
                continue;  /* back to main receive loop */
            }

            /* Checksum definitely got through at least once */
            SUCC("P2P RX: checksum for BURST %u sent successfully", cur_burst_id);

            in_burst = 0;

            if (nrf24_set_mode_rx(&radio) < 0) {
                ERROR("nrf24_set_mode_rx failed");
                page0_stream_free(&stream);
                fclose(fout);
                nrf24_deinit(&radio);
                return 1;
            }
        }
    }

    /* Reassemble + DECOMPRESS from STREAM[Page0] */
    uint64_t compressed_total   = 0;
    uint64_t uncompressed_total = 0;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    int zret = inflateInit(&zs);
    if (zret != Z_OK) {
        ERROR("P2P RX: inflateInit failed (zret=%d), writing compressed data instead", zret);

        /* Fallback: write raw compressed stream if inflateInit fails */
        for (size_t bid = 0; bid < stream.count; ++bid) {
            Burst *b = &stream.bursts[bid];
            if (b->frames_in_burst == 0) continue;

            for (unsigned i = 0; i < b->frames_in_burst; ++i) {
                uint8_t *frame = b->frame_data[i];
                uint8_t  flen  = b->frame_len[i];
                if (!frame || flen <= 1) continue;
                size_t data_len = flen - 1;
                if (fwrite(frame + 1, 1, data_len, fout) != data_len) {
                    ERROR("P2P RX: fwrite failed in fallback");
                    page0_stream_free(&stream);
                    fclose(fout);
                    nrf24_deinit(&radio);
                    return 1;
                }
                compressed_total += data_len;
            }
        }
    } else {
        uint8_t outbuf[4096];

        for (size_t bid = 0; bid < stream.count; ++bid) {
            Burst *b = &stream.bursts[bid];
            if (b->frames_in_burst == 0) continue;

            for (unsigned i = 0; i < b->frames_in_burst; ++i) {
                uint8_t *frame = b->frame_data[i];
                uint8_t  flen  = b->frame_len[i];
                if (!frame || flen <= 1) continue;

                const uint8_t *in     = frame + 1;   /* skip ChunkID */
                size_t         in_len = flen - 1;

                compressed_total += in_len;

                zs.next_in  = (Bytef *)in;
                zs.avail_in = (uInt)in_len;

                while (zs.avail_in > 0) {
                    zs.next_out  = outbuf;
                    zs.avail_out = sizeof(outbuf);

                    zret = inflate(&zs, Z_NO_FLUSH);
                    if (zret == Z_STREAM_ERROR || zret == Z_DATA_ERROR || zret == Z_MEM_ERROR) {
                        WARN("P2P RX: decompression error (zret=%d); stopping, partial file written", zret);
                        zs.avail_in = 0;  /* drop rest */
                        break;
                    }

                    size_t have = sizeof(outbuf) - zs.avail_out;
                    if (have > 0) {
                        if (fwrite(outbuf, 1, have, fout) != have) {
                            ERROR("P2P RX: fwrite failed during decompression");
                            inflateEnd(&zs);
                            page0_stream_free(&stream);
                            fclose(fout);
                            nrf24_deinit(&radio);
                            return 1;
                        }
                        uncompressed_total += have;
                    }

                    if (zret == Z_STREAM_END) {
                        /* End of compressed stream; ignore any trailing bytes */
                        break;
                    }
                }

                if (zret == Z_STREAM_END) {
                    break;
                }
            }

            if (zret == Z_STREAM_END) {
                break;
            }
        }

        inflateEnd(&zs);
    }

    double t_end = now_seconds();
    double dt    = (tx_started ? (t_end - t_start) : 0.0);
    double kibps = dt > 0.0 ? ((double)uncompressed_total / 1024.0 / dt) : 0.0;

    SUCC("P2P RX: done. Compressed %llu B -> %llu B uncompressed in %.3f s (%.1f KiB/s user data)",
         (unsigned long long)compressed_total,
         (unsigned long long)uncompressed_total,
         dt,
         kibps);

    page0_stream_free(&stream);
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
        /* Not fatal: just tell the user and continue */
        WARN("Could not open log file '%s' (continuing without file log)", log_path);
    } else {
        INFO("Logging to file '%s'", log_path);
    }

    int ret;
    if (strcmp(mode, "tx") == 0) {
        ret = run_tx(spi_dev, ce_bcm, path);
    } else { /* mode == "rx" already checked above */
        ret = run_rx(spi_dev, ce_bcm, path);
    }

    log_close();
    return ret;
}
