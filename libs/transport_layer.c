#include "transport_layer.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "logger.h"

/* ------------------------------------------------------------------------- */
/* Internal helpers                                                          */
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
/* STREAM_INFO pack/unpack                                                   */
/* ------------------------------------------------------------------------- */

int trans_build_stream_info(uint32_t total_orig_size,
                            const uint32_t comp_sizes[TRANS_NUM_PAGES],
                            uint8_t out_payload[32])
{
    if (!out_payload || !comp_sizes) {
        logger_error("trans_build_stream_info: NULL pointer");
        return -1;
    }

    if (total_orig_size > TRANS_STREAM_MAX_TOTAL_SIZE) {
        logger_error("STREAM_INFO: total size %u exceeds 24-bit limit (%u)",
                     total_orig_size, TRANS_STREAM_MAX_TOTAL_SIZE);
        return -1;
    }

    /* Check per-page compressed sizes. */
    for (size_t i = 0; i < TRANS_NUM_PAGES; ++i) {
        if (comp_sizes[i] > TRANS_STREAM_MAX_PAGE_COMP_SIZE) {
            logger_error("STREAM_INFO: page %zu compressed size %u exceeds 21-bit limit (%u)",
                         i, comp_sizes[i], TRANS_STREAM_MAX_PAGE_COMP_SIZE);
            return -1;
        }
    }

    memset(out_payload, 0, 32);

    out_payload[0] = TRANS_MSG_INFO;
    out_payload[1] = TRANS_MSG_STREAM_INFO;

    /* TotalOrigSize as 24-bit LE at bytes [2..4]. */
    out_payload[2] = (uint8_t)( total_orig_size        & 0xFFu);
    out_payload[3] = (uint8_t)((total_orig_size >> 8)  & 0xFFu);
    out_payload[4] = (uint8_t)((total_orig_size >> 16) & 0xFFu);

    /* Bit-pack 10×21-bit compressed sizes into bytes [5..31]. */
    for (size_t page = 0; page < TRANS_NUM_PAGES; ++page) {
        uint32_t v = comp_sizes[page]; /* already range-checked (21 bits max). */

        unsigned bit_offset = (unsigned)(21u * page);
        for (unsigned j = 0; j < 21u; ++j) {
            unsigned bit_index = bit_offset + j;
            unsigned byte_index = 5u + (bit_index / 8u);
            unsigned bit_in_byte = bit_index % 8u;

            if (byte_index >= 32u) {
                logger_error("STREAM_INFO: internal bit-pack overflow");
                return -1;
            }

            if ((v >> j) & 1u) {
                out_payload[byte_index] |= (uint8_t)(1u << bit_in_byte);
            }
        }
    }

    return 0;
}

int trans_parse_stream_info(const uint8_t in_payload[32],
                            uint32_t *total_orig_size,
                            uint32_t comp_sizes[TRANS_NUM_PAGES])
{
    if (!in_payload || !total_orig_size || !comp_sizes) {
        logger_error("trans_parse_stream_info: NULL pointer");
        return -1;
    }

    if (in_payload[0] != TRANS_MSG_INFO ||
        in_payload[1] != TRANS_MSG_STREAM_INFO) {
        logger_error("STREAM_INFO: invalid header (0x%02X,0x%02X)",
                     in_payload[0], in_payload[1]);
        return -1;
    }

    uint32_t total = 0;
    total |= (uint32_t)in_payload[2];
    total |= (uint32_t)in_payload[3] << 8;
    total |= (uint32_t)in_payload[4] << 16;

    if (total > TRANS_STREAM_MAX_TOTAL_SIZE) {
        logger_error("STREAM_INFO: parsed total size %u exceeds protocol limit", total);
        return -1;
    }

    *total_orig_size = total;

    /* Unpack 10×21-bit sizes from bytes [5..31]. */
    for (size_t page = 0; page < TRANS_NUM_PAGES; ++page) {
        uint32_t v = 0;
        unsigned bit_offset = (unsigned)(21u * page);

        for (unsigned j = 0; j < 21u; ++j) {
            unsigned bit_index = bit_offset + j;
            unsigned byte_index = 5u + (bit_index / 8u);
            unsigned bit_in_byte = bit_index % 8u;

            if (byte_index >= 32u) {
                logger_error("STREAM_INFO: internal bit-unpack overflow");
                return -1;
            }

            uint8_t byte = in_payload[byte_index];
            uint8_t b = (uint8_t)((byte >> bit_in_byte) & 0x01u);

            v |= ((uint32_t)b << j);
        }

        if (v > TRANS_STREAM_MAX_PAGE_COMP_SIZE) {
            logger_error("STREAM_INFO: parsed page %zu size %u exceeds protocol limit", page, v);
            return -1;
        }

        comp_sizes[page] = v;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Burst split/merge                                                         */
/* ------------------------------------------------------------------------- */

void trans_free_burst(trans_burst_t *burst)
{
    if (!burst) return;
    free(burst->frames);
    burst->frames = NULL;
    burst->frame_count = 0;
}

void trans_free_bursts(trans_burst_t *bursts, size_t burst_count)
{
    if (!bursts) return;
    for (size_t i = 0; i < burst_count; ++i) {
        trans_free_burst(&bursts[i]);
    }
    free(bursts);
}

/* Split page into bursts and frames. */
int trans_split_page_into_bursts(const uint8_t *page_data,
                                 size_t page_len,
                                 trans_burst_t **out_bursts,
                                 size_t *out_burst_count)
{
    if (!out_bursts || !out_burst_count) {
        logger_error("trans_split_page_into_bursts: output pointers are NULL");
        return -1;
    }

    *out_bursts = NULL;
    *out_burst_count = 0;

    if (!page_data || page_len == 0) {
        /* Empty page, nothing to do. */
        return 0;
    }

    size_t max_data_per_burst = TRANS_DATA_BYTES_PER_BURST;
    size_t burst_count = (page_len + max_data_per_burst - 1) / max_data_per_burst;

    trans_burst_t *bursts = (trans_burst_t *)calloc(burst_count, sizeof(trans_burst_t));
    if (!bursts) {
        logger_error("trans_split_page_into_bursts: calloc(bursts) failed");
        return -1;
    }

    size_t offset = 0;

    for (size_t b = 0; b < burst_count; ++b) {
        size_t remaining = page_len - offset;
        size_t this_data_len =
            (remaining < max_data_per_burst) ? remaining : max_data_per_burst;

        if (this_data_len == 0) {
            bursts[b].frames = NULL;
            bursts[b].frame_count = 0;
            continue;
        }

        size_t frame_count =
            (this_data_len + TRANS_DATA_BYTES_PER_FRAME - 1) / TRANS_DATA_BYTES_PER_FRAME;

        if (frame_count == 0 || frame_count > TRANS_MAX_FRAMES_PER_BURST) {
            logger_error("trans_split_page_into_bursts: invalid frame_count=%zu", frame_count);
            trans_free_bursts(bursts, burst_count);
            return -1;
        }

        trans_frame_t *frames =
            (trans_frame_t *)calloc(frame_count, sizeof(trans_frame_t));
        if (!frames) {
            logger_error("trans_split_page_into_bursts: calloc(frames) failed");
            trans_free_bursts(bursts, burst_count);
            return -1;
        }

        size_t data_in_this_burst = 0;

        for (size_t f = 0; f < frame_count; ++f) {
            size_t remaining_for_burst = this_data_len - data_in_this_burst;
            size_t data_len;

            if (f < frame_count - 1) {
                data_len = TRANS_DATA_BYTES_PER_FRAME;
            } else {
                data_len = remaining_for_burst;
            }

            if (data_len == 0 || data_len > TRANS_DATA_BYTES_PER_FRAME) {
                logger_error("trans_split_page_into_bursts: invalid data_len=%zu", data_len);
                free(frames);
                trans_free_bursts(bursts, burst_count);
                return -1;
            }

            frames[f].len = (uint8_t)(1u + data_len);
            frames[f].data[0] = (uint8_t)f; /* FrameID */

            memcpy(&frames[f].data[1],
                   page_data + offset + data_in_this_burst,
                   data_len);

            data_in_this_burst += data_len;
        }

        bursts[b].frames = frames;
        bursts[b].frame_count = frame_count;

        offset += this_data_len;
    }

    *out_bursts = bursts;
    *out_burst_count = burst_count;

    logger_info("Transport: split page of %zu bytes into %zu bursts", page_len, burst_count);
    return 0;
}

/* Merge bursts back into a single page (discard FrameID byte). */
int trans_merge_bursts_into_page(const trans_burst_t *bursts,
                                 size_t burst_count,
                                 uint8_t **out_page,
                                 size_t *out_page_len)
{
    if (!out_page || !out_page_len) {
        logger_error("trans_merge_bursts_into_page: output pointers are NULL");
        return -1;
    }

    *out_page = NULL;
    *out_page_len = 0;

    if (!bursts || burst_count == 0) {
        return 0;
    }

    /* First pass: compute total data length. */
    size_t total_data = 0;

    for (size_t b = 0; b < burst_count; ++b) {
        const trans_burst_t *burst = &bursts[b];

        for (size_t f = 0; f < burst->frame_count; ++f) {
            const trans_frame_t *fr = &burst->frames[f];
            if (fr->len == 0 || fr->len > TRANS_FRAME_MAX_LEN) {
                logger_error("trans_merge_bursts_into_page: invalid frame len=%u", fr->len);
                return -1;
            }
            if (fr->len <= 1) {
                /* No data payload, only FrameID. This is odd but not fatal. */
                continue;
            }
            total_data += (size_t)(fr->len - 1u);
        }
    }

    if (total_data == 0) {
        return 0;
    }

    uint8_t *buf = (uint8_t *)malloc(total_data);
    if (!buf) {
        logger_error("trans_merge_bursts_into_page: malloc(%zu) failed", total_data);
        return -1;
    }

    /* Second pass: copy data. */
    size_t offset = 0;

    for (size_t b = 0; b < burst_count; ++b) {
        const trans_burst_t *burst = &bursts[b];

        for (size_t f = 0; f < burst->frame_count; ++f) {
            const trans_frame_t *fr = &burst->frames[f];
            if (fr->len <= 1) {
                continue;
            }
            size_t data_len = (size_t)(fr->len - 1u);

            memcpy(buf + offset, &fr->data[1], data_len);
            offset += data_len;
        }
    }

    *out_page = buf;
    *out_page_len = total_data;

    logger_info("Transport: merged %zu bursts into page of %zu bytes",
                burst_count, total_data);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Burst helpers                                                             */
/* ------------------------------------------------------------------------- */

uint16_t trans_compute_burst_size(const trans_burst_t *burst)
{
    if (!burst || !burst->frames) return 0;

    size_t sum = 0;
    for (size_t i = 0; i < burst->frame_count; ++i) {
        sum += burst->frames[i].len;
    }
    if (sum > 0xFFFFu) {
        /* Should not happen with our constraints. */
        logger_warn("trans_compute_burst_size: sum=%zu exceeds uint16_t", sum);
        return 0xFFFFu;
    }
    return (uint16_t)sum;
}

int trans_derive_frame_layout(uint16_t burst_size,
                              uint8_t *out_frame_count,
                              uint8_t out_frame_lengths[TRANS_MAX_FRAMES_PER_BURST])
{
    if (!out_frame_count || !out_frame_lengths) {
        logger_error("trans_derive_frame_layout: NULL output pointer");
        return -1;
    }

    if (burst_size == 0) {
        logger_error("trans_derive_frame_layout: burst_size=0 not valid");
        return -1;
    }

    /* Max possible bytes: 255 * 32 = 8160. */
    if (burst_size > TRANS_MAX_FRAMES_PER_BURST * TRANS_FRAME_MAX_LEN) {
        logger_error("trans_derive_frame_layout: burst_size=%u too large", burst_size);
        return -1;
    }

    uint16_t max_len = (uint16_t)TRANS_FRAME_MAX_LEN;
    uint16_t fc = (uint16_t)((burst_size + max_len - 1u) / max_len);

    if (fc == 0 || fc > TRANS_MAX_FRAMES_PER_BURST) {
        logger_error("trans_derive_frame_layout: derived frame_count=%u invalid", fc);
        return -1;
    }

    uint16_t full_bytes = (uint16_t)((fc - 1u) * max_len);
    if (burst_size <= full_bytes) {
        logger_error("trans_derive_frame_layout: burst_size=%u too small for %u full frames",
                     burst_size, fc - 1u);
        return -1;
    }

    uint16_t last_len = (uint16_t)(burst_size - full_bytes);
    if (last_len == 0 || last_len > max_len) {
        logger_error("trans_derive_frame_layout: invalid last_len=%u", last_len);
        return -1;
    }

    for (uint16_t i = 0; i < fc - 1u; ++i) {
        out_frame_lengths[i] = (uint8_t)max_len;
    }
    out_frame_lengths[fc - 1u] = (uint8_t)last_len;

    *out_frame_count = (uint8_t)fc;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* FNV-1a 64-bit checksum                                                    */
/* ------------------------------------------------------------------------- */

void trans_checksum_update(uint64_t *state, const uint8_t *data, size_t len)
{
    if (!state || !data || len == 0) return;

    uint64_t h = *state;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)data[i];
        h *= TRANS_FNV64_PRIME;
    }
    *state = h;
}

void trans_checksum_to_bytes(uint64_t value, uint8_t out[8])
{
    if (!out) return;
    for (int i = 0; i < 8; ++i) {
        out[i] = (uint8_t)(value & 0xFFu);
        value >>= 8;
    }
}

uint64_t trans_checksum_from_bytes(const uint8_t in[8])
{
    if (!in) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v <<= 8;
        v |= (uint64_t)in[i];
    }
    return v;
}

uint64_t trans_compute_burst_checksum(const trans_burst_t *burst)
{
    uint64_t state;
    trans_checksum_init(&state);

    if (!burst || !burst->frames) {
        return trans_checksum_final(state);
    }

    for (size_t i = 0; i < burst->frame_count; ++i) {
        const trans_frame_t *fr = &burst->frames[i];
        if (fr->len == 0 || fr->len > TRANS_FRAME_MAX_LEN) {
            continue;
        }
        trans_checksum_update(&state, fr->data, fr->len);
    }

    return trans_checksum_final(state);
}

/* ------------------------------------------------------------------------- */
/* Control messages                                                          */
/* ------------------------------------------------------------------------- */

void trans_build_burst_info(uint8_t page_id,
                            uint8_t burst_id,
                            const trans_burst_t *burst,
                            uint8_t out_msg[6])
{
    if (!out_msg) return;

    out_msg[0] = TRANS_MSG_INFO;
    out_msg[1] = TRANS_MSG_BURST_INFO;
    out_msg[2] = page_id;
    out_msg[3] = burst_id;

    uint16_t burst_size = trans_compute_burst_size(burst);
    encode_u16_le(&out_msg[4], burst_size);
}

int trans_parse_burst_info(const uint8_t *msg,
                           size_t msg_len,
                           uint8_t *out_page_id,
                           uint8_t *out_burst_id,
                           uint16_t *out_burst_size)
{
    if (!msg || msg_len < 6 ||
        !out_page_id || !out_burst_id || !out_burst_size) {
        logger_error("trans_parse_burst_info: invalid arguments");
        return -1;
    }

    if (msg[0] != TRANS_MSG_INFO || msg[1] != TRANS_MSG_BURST_INFO) {
        logger_error("BURST_INFO: invalid header (0x%02X,0x%02X)", msg[0], msg[1]);
        return -1;
    }

    *out_page_id = msg[2];
    *out_burst_id = msg[3];
    *out_burst_size = decode_u16_le(&msg[4]);

    return 0;
}

void trans_build_transfer_finish(uint8_t out_msg[2])
{
    if (!out_msg) return;
    out_msg[0] = TRANS_MSG_INFO;
    out_msg[1] = TRANS_MSG_TRANSFER_FINISH;
}

int trans_parse_transfer_finish(const uint8_t *msg,
                                size_t msg_len)
{
    if (!msg || msg_len < 2) {
        logger_error("trans_parse_transfer_finish: invalid arguments");
        return -1;
    }

    if (msg[0] != TRANS_MSG_INFO || msg[1] != TRANS_MSG_TRANSFER_FINISH) {
        logger_error("TRANSFER_FINISH: invalid header (0x%02X,0x%02X)",
                     msg[0], msg[1]);
        return -1;
    }

    return 0;
}
