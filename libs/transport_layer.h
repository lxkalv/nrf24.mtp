#ifndef TRANSPORT_LAYER_H
#define TRANSPORT_LAYER_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

#define TRANS_DATA_BYTES_PER_FRAME       31u
#define TRANS_FRAME_MAX_LEN              32u   /* 1 byte FrameID + 31 data */
#define TRANS_MAX_FRAMES_PER_BURST       255u
#define TRANS_DATA_BYTES_PER_BURST       (TRANS_DATA_BYTES_PER_FRAME * TRANS_MAX_FRAMES_PER_BURST)

#define TRANS_NUM_PAGES                  10u

/* STREAM_INFO limits */
#define TRANS_STREAM_MAX_TOTAL_SIZE      0xFFFFFFu  /* 24-bit (≈16 MiB) */
#define TRANS_STREAM_MAX_PAGE_COMP_SIZE  0x1FFFFFu  /* 21-bit (≈2 MiB/page) */

/* Control message IDs (same style as previous C p2p) */
#define TRANS_MSG_INFO                   0xFFu
#define TRANS_MSG_STREAM_INFO            0xE0u
#define TRANS_MSG_BURST_INFO             0xF0u
#define TRANS_MSG_TRANSFER_FINISH        0x0Fu

/* ------------------------------------------------------------------------- */
/* Types                                                                     */
/* ------------------------------------------------------------------------- */

/* One on-air frame (NRF payload) organised by Transport. */
typedef struct {
    uint8_t data[TRANS_FRAME_MAX_LEN]; /* data[0] = FrameID, rest: payload */
    uint8_t len;                       /* 1..TRANS_FRAME_MAX_LEN */
} trans_frame_t;

/* One burst = consecutive frames for a given page. */
typedef struct {
    trans_frame_t *frames;   /* malloc'd array */
    size_t         frame_count;
} trans_burst_t;

/* ------------------------------------------------------------------------- */
/* STREAM_INFO: pack/unpack                                                  */
/* ------------------------------------------------------------------------- */

/* Build a 32-byte STREAM_INFO frame.
 *
 * Layout (all little-endian, bit-packed):
 *   [0]   = 0xFF (TRANS_MSG_INFO)
 *   [1]   = 0xE0 (TRANS_MSG_STREAM_INFO)
 *   [2..4]  = TotalOrigSize (24-bit, uint32_t <= 0xFFFFFF)
 *   [5..31] = bit-field of 10 per-page compressed sizes, each 21-bit,
 *             packed consecutively (page 0 at bit 0, page 9 at bit 189).
 *
 * Constraints:
 *   total_orig_size <= TRANS_STREAM_MAX_TOTAL_SIZE
 *   comp_sizes[i]   <= TRANS_STREAM_MAX_PAGE_COMP_SIZE
 *
 * Returns 0 on success, non-zero on range error.
 */
int trans_build_stream_info(uint32_t total_orig_size,
                            const uint32_t comp_sizes[TRANS_NUM_PAGES],
                            uint8_t out_payload[32]);

/* Parse a 32-byte STREAM_INFO frame.
 *
 * On success:
 *   - *total_orig_size in [0, TRANS_STREAM_MAX_TOTAL_SIZE]
 *   - comp_sizes[i]     in [0, TRANS_STREAM_MAX_PAGE_COMP_SIZE]
 *
 * Returns 0 on success, non-zero on malformed header.
 */
int trans_parse_stream_info(const uint8_t in_payload[32],
                            uint32_t *total_orig_size,
                            uint32_t comp_sizes[TRANS_NUM_PAGES]);

/* ------------------------------------------------------------------------- */
/* Burst split/merge                                                         */
/* ------------------------------------------------------------------------- */

/* Split a single (compressed) page into bursts and frames.
 *
 * page_data/page_len: input bytes.
 *
 * Each burst has at most TRANS_DATA_BYTES_PER_BURST data bytes.
 * Each frame:
 *   - data[0] = FrameID (0..frame_count-1),
 *   - data[1..] = up to 31 bytes of page data,
 *   - len = 1 + data_payload_len.
 * All frames except possibly the last in each burst are full 32-byte frames.
 *
 * On success:
 *   - *out_bursts is a malloc'd array of trans_burst_t,
 *   - each burst's frames[] is malloc'd,
 *   - *out_burst_count is number of bursts.
 *
 * For page_len == 0:
 *   - *out_bursts = NULL, *out_burst_count = 0, return 0.
 *
 * Returns 0 on success, non-zero on allocation error.
 */
int trans_split_page_into_bursts(const uint8_t *page_data,
                                 size_t page_len,
                                 trans_burst_t **out_bursts,
                                 size_t *out_burst_count);

/* Merge bursts back into a single page.
 *
 * Bursts are assumed to be in correct order and contain frames seq 0..N-1.
 * For each frame we drop the first byte (FrameID) and concatenate data bytes.
 *
 * On success:
 *   - *out_page is malloc'd and must be free()'d by caller,
 *   - *out_page_len is total data length.
 *
 * Returns 0 on success, non-zero on allocation or malformed frame length.
 */
int trans_merge_bursts_into_page(const trans_burst_t *bursts,
                                 size_t burst_count,
                                 uint8_t **out_page,
                                 size_t *out_page_len);

/* ------------------------------------------------------------------------- */
/* Burst helpers                                                             */
/* ------------------------------------------------------------------------- */

/* Free a single burst's frames (but not the burst struct itself). */
void trans_free_burst(trans_burst_t *burst);

/* Free an array of bursts (and all their frames). */
void trans_free_bursts(trans_burst_t *bursts, size_t burst_count);

/* Compute on-air BurstSize = sum of frame lengths (1..32 each).
 * The result always fits into uint16_t (max 255 * 32 = 8160).
 */
uint16_t trans_compute_burst_size(const trans_burst_t *burst);

/* Given BurstSize, derive the number of frames and their lengths
 * under the rule “all frames are full TRANS_FRAME_MAX_LEN except
 * possibly the last”.
 *
 * On success:
 *   - *out_frame_count in [1, TRANS_MAX_FRAMES_PER_BURST],
 *   - out_frame_lengths[i] (i=0..out_frame_count-1) is each frame len.
 *
 * Returns 0 on success, non-zero if burst_size is invalid.
 */
int trans_derive_frame_layout(uint16_t burst_size,
                              uint8_t *out_frame_count,
                              uint8_t out_frame_lengths[TRANS_MAX_FRAMES_PER_BURST]);

/* ------------------------------------------------------------------------- */
/* FNV-1a 64-bit checksum                                                    */
/* ------------------------------------------------------------------------- */

#define TRANS_FNV64_OFFSET_BASIS  1469598103934665603ULL
#define TRANS_FNV64_PRIME         1099511628211ULL

/* Stateless helpers: init/update/final. */
static inline void trans_checksum_init(uint64_t *state)
{
    *state = TRANS_FNV64_OFFSET_BASIS;
}

void trans_checksum_update(uint64_t *state, const uint8_t *data, size_t len);

/* Final just returns the current state. */
static inline uint64_t trans_checksum_final(uint64_t state)
{
    return state;
}

/* Convert checksum to 8 bytes (little-endian). */
void trans_checksum_to_bytes(uint64_t value, uint8_t out[8]);

/* Convert 8 bytes (little-endian) back to uint64_t. */
uint64_t trans_checksum_from_bytes(const uint8_t in[8]);

/* Compute FNV-1a checksum of an entire burst (all frames, including FrameID). */
uint64_t trans_compute_burst_checksum(const trans_burst_t *burst);

/* ------------------------------------------------------------------------- */
/* Control messages: BURST_INFO / TRANSFER_FINISH                            */
/* ------------------------------------------------------------------------- */

/* Build a 6-byte BURST_INFO message:
 *
 *   [0] = 0xFF (TRANS_MSG_INFO)
 *   [1] = 0xF0 (TRANS_MSG_BURST_INFO)
 *   [2] = PageID
 *   [3] = BurstID
 *   [4..5] = BurstSize (uint16_t, little-endian)
 */
void trans_build_burst_info(uint8_t page_id,
                            uint8_t burst_id,
                            const trans_burst_t *burst,
                            uint8_t out_msg[6]);

/* Parse a BURST_INFO message.
 *
 * Returns 0 on success, non-zero if malformed or wrong header.
 */
int trans_parse_burst_info(const uint8_t *msg,
                           size_t msg_len,
                           uint8_t *out_page_id,
                           uint8_t *out_burst_id,
                           uint16_t *out_burst_size);

/* Build a 2-byte TRANSFER_FINISH message:
 *
 *   [0] = 0xFF
 *   [1] = 0x0F
 */
void trans_build_transfer_finish(uint8_t out_msg[2]);

/* Parse TRANSFER_FINISH message.
 *
 * Returns 0 on success, non-zero if malformed or wrong header.
 */
int trans_parse_transfer_finish(const uint8_t *msg,
                                size_t msg_len);

#endif /* TRANSPORT_LAYER_H */
