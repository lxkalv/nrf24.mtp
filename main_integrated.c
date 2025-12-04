#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include <zlib.h>
#include <math.h> 

// --- HARDWARE LIBRARIES ---
#include <wiringPi.h>
#include <mntent.h>
#include <dirent.h>

// --- RADIO LIBRARIES ---
#include "libs/nrf24.h"
#include "libs/utils.h"

// ==========================================
// --- UI HARDWARE DEFINITIONS ---
// ==========================================
#define LED_INSERT_USB    25 // este esta mal (Keep comment as requested)
#define LED_EXTRACT_USB   26
#define LED_DEVICE_CONFIG 23
#define LED_RXTX_STATUS   16

#define BTN_INTERACT      19
#define BTN_STOP          6
#define SWITCH_MODE       27 // LOW=TX, HIGH=RX
#define SWITCH_SCENARIO   17 // LOW=Network, HIGH=P2P

// --- GLOBAL CONTROL FLAGS ---
volatile bool global_stop_flag = false;

// ==========================================
// --- RADIO PROTOCOL CONSTANTS ---
// ==========================================
#define P2P_CHANNEL          90
#define BURST_DATA_MAX       7905   
#define CHUNK_DATA_MAX       31     
#define MAX_PAYLOAD          32     
#define MAX_CHUNKS_PER_BURST 255    

#define CHUNK_DATA_BYTES     CHUNK_DATA_MAX
#define MAX_FRAMES_PER_BURST MAX_CHUNKS_PER_BURST
#define BURST_DATA_BYTES     (CHUNK_DATA_BYTES * MAX_FRAMES_PER_BURST)

#define CHECKSUM_TIMEOUT_MS  1000   
#define CONTROL_TIMEOUT_MS   100    
#define DATA_TIMEOUT_MS      20     
#define CHECKSUM_SIZE        8      

#define MSG_INFO             0xFF
#define MSG_BURST_INFO       0xF0
#define MSG_TRANSFER_FINISH  0x0F
#define P2P_MSG_STREAM_INFO  0xE0   

#define P2P_NUM_PAGES        10
#define MAX_BURSTS_PER_PAGE  255    
#define MAX_PAGES            16     

#define FNV64_OFFSET_BASIS   1469598103934665603ULL
#define FNV64_PRIME          1099511628211ULL

// ==========================================
// --- UI HELPER FUNCTIONS ---
// ==========================================

// Interrupt for STOP button
void trigger_reset(void) {
    if (!global_stop_flag) {
        global_stop_flag = true;
    }
}

// Check if we should abort (Stop pressed)
bool should_reset() {
    return global_stop_flag;
}

// Blink Status LED during transfer (Visual Feedback)
void toggle_activity_led() {
    static unsigned long last_toggle = 0;
    static bool state = false;
    unsigned long now = millis();
    if (now - last_toggle > 50) { // Blink every 50ms
        state = !state;
        digitalWrite(LED_RXTX_STATUS, state);
        last_toggle = now;
    }
}

// Check for USB Mount
bool check_usb_connected(char *mount_point_buffer) {
    struct mntent *ent;
    FILE *aFile;
    aFile = setmntent("/proc/mounts", "r");
    if (aFile == NULL) return false;

    while (NULL != (ent = getmntent(aFile))) {
        // Checks if mount point starts with /media
        if (strncmp(ent->mnt_dir, "/media", 6) == 0) {
            if (mount_point_buffer) strcpy(mount_point_buffer, ent->mnt_dir);
            endmntent(aFile);
            return true;
        }
    }
    endmntent(aFile);
    return false;
}

// Find first file in directory
int find_first_file(const char *dir_path, char *out_path, size_t max_len) {
    DIR *d;
    struct dirent *dir;
    d = opendir(dir_path);
    if (!d) return -1;
    while ((dir = readdir(d)) != NULL) {
        if (dir->d_type == DT_REG) { // Regular file
            snprintf(out_path, max_len, "%s/%s", dir_path, dir->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

// --- IMPROVED WAIT FUNCTION (With Debouncing) ---
bool wait_with_blink(int led_pin, int button_pin, bool (*condition_func)(char*), char* cond_arg, float blink_interval_sec) {
    unsigned long last_blink = millis();
    bool led_state = false;
    int interval_ms = (int)(blink_interval_sec * 1000);

    while (1) {
        // 1. Check Stop Priority
        if (should_reset()) return true;

        // 2. Check Button with Debounce
        if (button_pin != -1) {
            if (digitalRead(button_pin) == 0) { // Active LOW
                delay(50); // Debounce wait
                if (digitalRead(button_pin) == 0) { // Still pressed?
                    // Wait for release so we don't skip the next step
                    while(digitalRead(button_pin) == 0) {
                        delay(10);
                        if (should_reset()) return true;
                    }
                    return false; // Condition Met (Button Pressed)
                }
            }
        }

        // 3. Check Custom Condition (USB)
        if (condition_func != NULL) {
            if (condition_func(cond_arg)) return false; // Condition Met
        }

        // 4. Blink Logic
        if (led_pin != -1 && interval_ms > 0) {
            unsigned long current_time = millis();
            if (current_time - last_blink >= interval_ms) {
                led_state = !led_state;
                digitalWrite(led_pin, led_state);
                last_blink = current_time;
            }
        }
        delay(20); // Save CPU
    }
}

// ==========================================
// --- RADIO LOGIC (Time, Checksum, Utils) ---
// ==========================================

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void encode_u16_le(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)(v & 0xFFu);
    dst[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static uint16_t decode_u16_le(const uint8_t *src) {
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static void checksum_init(uint64_t *state) { *state = FNV64_OFFSET_BASIS; }

static void checksum_update(uint64_t *state, const uint8_t *data, size_t len) {
    uint64_t h = *state;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)data[i];
        h *= FNV64_PRIME;
    }
    *state = h;
}

static void checksum_final(uint64_t state, uint8_t out[CHECKSUM_SIZE]) {
    for (int i = 0; i < 8; ++i) {
        out[i] = (uint8_t)(state & 0xFFu);
        state >>= 8;
    }
}

// --- RADIO WRAPPER WITH UI HOOKS ---
static int send_with_retries(nrf24_t *radio, const uint8_t *buf, uint8_t len, unsigned int timeout_ms, const char *what, uint64_t *rf_bytes_total, uint64_t *rf_frames_total) {
    unsigned int attempt = 0;
    for (;;) {
        // [INTEGRATION] UI Checks
        if (should_reset()) return -1;
        toggle_activity_led();

        if (rf_bytes_total)  *rf_bytes_total  += len;
        if (rf_frames_total) *rf_frames_total += 1;

        int ret = nrf24_send_blocking(radio, buf, len, timeout_ms);
        if (ret == 0) return 0;

        if (errno != ETIMEDOUT) {
            ERROR("nrf24_send_blocking(%s) failed: %s", what, strerror(errno));
            return -1;
        }

        ++attempt;
        if (attempt == 1 || (attempt % 50) == 0) {
            WARN("%s: timeout (no ACK) on attempt %u", what, attempt);
        }
        if (attempt % 500 == 0) {
            WARN("%s: %u timeouts, reconfiguring radio", what, attempt);
            (void)nrf24_configure_quick(radio, P2P_CHANNEL);
        }
    }
}

// --- BURST & PAGE STREAM STRUCTURES ---
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

static void page_stream_init(PageStream *ps) {
    ps->bursts = NULL; ps->count = 0; ps->capacity = 0;
}

static void free_burst(Burst *b) {
    if (!b) return;
    for (unsigned i = 0; i < b->frames_in_burst; ++i) {
        free(b->frame_data[i]);
    }
    b->frames_in_burst = 0;
}

static void page_stream_free(PageStream *ps) {
    if (!ps->bursts) return;
    for (size_t i = 0; i < ps->count; ++i) free_burst(&ps->bursts[i]);
    free(ps->bursts);
    ps->bursts = NULL; ps->count = 0; ps->capacity = 0;
}

static Burst *page_get_burst(PageStream *ps, unsigned burst_id) {
    if (burst_id >= ps->capacity) {
        size_t new_cap = ps->capacity ? ps->capacity * 2 : 8;
        while (burst_id >= new_cap) new_cap *= 2;
        Burst *new_bursts = (Burst *)calloc(new_cap, sizeof(Burst));
        if (!new_bursts) return NULL;
        for (size_t i = 0; i < ps->count; ++i) new_bursts[i] = ps->bursts[i];
        free(ps->bursts);
        ps->bursts = new_bursts;
        ps->capacity = new_cap;
    }
    if (burst_id >= ps->count) {
        for (size_t i = ps->count; i <= burst_id; ++i) {
            ps->bursts[i].frames_in_burst = 0;
            for (unsigned j = 0; j < MAX_CHUNKS_PER_BURST; ++j) {
                ps->bursts[i].frame_data[j] = NULL;
            }
        }
        ps->count = burst_id + 1;
    }
    return &ps->bursts[burst_id];
}

static int store_burst(PageStream *ps, unsigned burst_id, unsigned frames_in_burst, uint8_t current_burst[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD], const uint8_t sizes[MAX_CHUNKS_PER_BURST]) {
    Burst *b = page_get_burst(ps, burst_id);
    if (!b) return -1;
    free_burst(b);
    b->frames_in_burst = frames_in_burst;
    for (unsigned i = 0; i < frames_in_burst; ++i) {
        size_t len = sizes[i];
        b->frame_data[i] = (uint8_t *)malloc(len);
        if (!b->frame_data[i]) return -1;
        memcpy(b->frame_data[i], current_burst[i], len);
        b->frame_len[i] = (uint8_t)len;
    }
    return 0;
}

static int decompress_page_to_file(PageStream *ps, FILE *fout, uint64_t *compressed_total, uint64_t *uncompressed_total) {
    if (!ps->bursts || ps->count == 0) return 0;
    z_stream zs; memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK) return -1;

    uint8_t outbuf[4096];
    int end_reached = 0;

    for (size_t bid = 0; bid < ps->count && !end_reached; ++bid) {
        Burst *b = &ps->bursts[bid];
        if (b->frames_in_burst == 0) continue;
        for (unsigned i = 0; i < b->frames_in_burst && !end_reached; ++i) {
            uint8_t *frame = b->frame_data[i];
            if (!frame || b->frame_len[i] <= 1) continue;
            const uint8_t *in = frame + 1; 
            size_t in_len = b->frame_len[i] - 1;
            *compressed_total += in_len;

            zs.next_in = (Bytef *)in;
            zs.avail_in = (uInt)in_len;
            while (zs.avail_in > 0) {
                zs.next_out = outbuf;
                zs.avail_out = sizeof(outbuf);
                int zret = inflate(&zs, Z_NO_FLUSH);
                if (zret == Z_STREAM_END) end_reached = 1;
                else if (zret != Z_OK) { end_reached = 1; }

                size_t have = sizeof(outbuf) - zs.avail_out;
                if (have > 0) {
                    fwrite(outbuf, 1, have, fout);
                    *uncompressed_total += have;
                }
                if (zret != Z_OK && zret != Z_BUF_ERROR) break;
            }
        }
    }
    inflateEnd(&zs);
    return 0;
}

// ==========================================
// --- INTEGRATED TX LOGIC ---
// ==========================================

static int run_tx(const char *spi_dev, int ce_bcm, const char *input_path) {
    nrf24_t radio;
    nrf24_config_t cfg = { .spi_device = spi_dev, .spi_speed_hz = 8000000, .ce_gpio = (uint8_t)ce_bcm };
    if (nrf24_init(&radio, &cfg) < 0) return 1;
    if (nrf24_configure_quick(&radio, P2P_CHANNEL) < 0) { nrf24_deinit(&radio); return 1; }
    if (nrf24_set_mode_tx(&radio) < 0) { nrf24_deinit(&radio); return 1; }

    FILE *fin = fopen(input_path, "rb");
    if (!fin) { ERROR("Open failed: %s", input_path); nrf24_deinit(&radio); return 1; }
    fseek(fin, 0, SEEK_END); long fsize = ftell(fin); rewind(fin);
    uint8_t *orig_buf = (uint8_t *)malloc((size_t)fsize);
    if (fread(orig_buf, 1, fsize, fin) != fsize) { free(orig_buf); fclose(fin); nrf24_deinit(&radio); return 1; }
    fclose(fin);

    INFO("TX Start: %s (%ld bytes)", input_path, fsize);
    uint64_t orig_len = (uint64_t)fsize;
    uint64_t tx_rf_bytes = 0; uint64_t tx_rf_frames = 0;

    for (unsigned page_id = 0; page_id < P2P_NUM_PAGES; ++page_id) {
        // [INTEGRATION] Check Reset
        if (should_reset()) { free(orig_buf); goto cleanup; }

        uint64_t page_start = (orig_len * page_id) / P2P_NUM_PAGES;
        uint64_t page_end   = (orig_len * (page_id+1)) / P2P_NUM_PAGES;
        if (page_start >= orig_len) break;
        if (page_end > orig_len) page_end = orig_len;
        uint64_t page_len = page_end - page_start;
        if (page_len == 0) continue;

        // Compress
        uLong dest_len = compressBound((uLong)page_len);
        uint8_t *comp_page = (uint8_t *)malloc(dest_len);
        compress2(comp_page, &dest_len, orig_buf + page_start, (uLong)page_len, 6);
        size_t comp_len = (size_t)dest_len;

        // Calculate Bursts
        uint16_t bursts_in_page = (uint16_t)((comp_len + BURST_DATA_BYTES - 1) / BURST_DATA_BYTES);
        if (bursts_in_page > MAX_BURSTS_PER_PAGE) bursts_in_page = MAX_BURSTS_PER_PAGE;
        size_t last_burst_bytes = comp_len - (size_t)(bursts_in_page - 1) * BURST_DATA_BYTES;
        uint8_t last_burst_frames = (uint8_t)((last_burst_bytes + CHUNK_DATA_BYTES - 1) / CHUNK_DATA_BYTES);
        size_t used_prev = (size_t)(last_burst_frames - 1) * CHUNK_DATA_BYTES;
        uint8_t last_frame_bytes = (uint8_t)(last_burst_bytes - used_prev);
        if (last_frame_bytes == 0) last_frame_bytes = CHUNK_DATA_BYTES;

        // Send STREAM_INFO
        uint8_t s_info[8] = {MSG_INFO, P2P_MSG_STREAM_INFO, (uint8_t)page_id, (uint8_t)P2P_NUM_PAGES, 0, 0, last_burst_frames, last_frame_bytes};
        encode_u16_le(&s_info[4], bursts_in_page);
        if (send_with_retries(&radio, s_info, 8, CONTROL_TIMEOUT_MS, "STREAM_INFO", &tx_rf_bytes, &tx_rf_frames) < 0) {
            free(comp_page); goto cleanup;
        }

        // Send Bursts
        size_t comp_pos = 0;
        uint8_t burst_id = 0;
        while (comp_pos < comp_len) {
            if (should_reset()) { free(comp_page); free(orig_buf); goto cleanup; }

            uint8_t b_payloads[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD];
            uint8_t b_sizes[MAX_CHUNKS_PER_BURST];
            unsigned num_chunks = 0;
            size_t b_data = 0; uint16_t b_onair = 0;
            uint64_t chk_state; checksum_init(&chk_state);

            while (comp_pos < comp_len && b_data < BURST_DATA_MAX && num_chunks < MAX_CHUNKS_PER_BURST) {
                size_t max_data = CHUNK_DATA_BYTES;
                size_t rem = comp_len - comp_pos;
                if (rem < max_data) max_data = rem;
                if (b_data + max_data > BURST_DATA_MAX) max_data = BURST_DATA_MAX - b_data;
                if (max_data == 0) break;

                b_payloads[num_chunks][0] = (uint8_t)num_chunks;
                memcpy(&b_payloads[num_chunks][1], comp_page + comp_pos, max_data);
                comp_pos += max_data;
                b_sizes[num_chunks] = (uint8_t)(1 + max_data);
                checksum_update(&chk_state, b_payloads[num_chunks], b_sizes[num_chunks]);
                b_data += max_data; b_onair += b_sizes[num_chunks];
                num_chunks++;
            }
            if (num_chunks == 0) break;

            uint8_t chk_bytes[CHECKSUM_SIZE]; checksum_final(chk_state, chk_bytes);

            // Send Loop for this Burst
            int burst_done = 0;
            while (!burst_done) {
                if (should_reset()) { free(comp_page); free(orig_buf); goto cleanup; }

                uint8_t b_info[6] = {MSG_INFO, MSG_BURST_INFO, (uint8_t)page_id, burst_id, 0, 0};
                encode_u16_le(&b_info[4], b_onair);
                if (send_with_retries(&radio, b_info, 6, CONTROL_TIMEOUT_MS, "BURST_INFO", &tx_rf_bytes, &tx_rf_frames) < 0) {
                    free(comp_page); free(orig_buf); goto cleanup;
                }

                for (unsigned i = 0; i < num_chunks; ++i) {
                     if (send_with_retries(&radio, b_payloads[i], b_sizes[i], DATA_TIMEOUT_MS, "DATA", &tx_rf_bytes, &tx_rf_frames) < 0) {
                        free(comp_page); free(orig_buf); goto cleanup;
                     }
                }

                // Checksum Wait
                nrf24_set_mode_rx(&radio);
                double w_start = now_seconds();
                int got_chk = 0;
                while (!got_chk && (now_seconds() - w_start)*1000.0 < CHECKSUM_TIMEOUT_MS) {
                     // [INTEGRATION] Check Reset/Blink while waiting
                     if (should_reset()) { free(comp_page); free(orig_buf); goto cleanup; }
                     toggle_activity_led(); 

                     uint8_t buf[32]; uint8_t len=32;
                     if (nrf24_recv_blocking(&radio, buf, &len, 50) >= 0) {
                         if (len == CHECKSUM_SIZE && memcmp(buf, chk_bytes, CHECKSUM_SIZE) == 0) got_chk = 1;
                     }
                }
                
                if (got_chk) burst_done = 1;
                else {
                    WARN("Checksum timeout, resending burst");
                    nrf24_set_mode_tx(&radio);
                }
            }
            nrf24_set_mode_tx(&radio);
            burst_id++;
        }
        free(comp_page);
    }

    uint8_t fin_msg[2] = {MSG_INFO, MSG_TRANSFER_FINISH};
    send_with_retries(&radio, fin_msg, 2, CONTROL_TIMEOUT_MS, "FINISH", &tx_rf_bytes, &tx_rf_frames);

    free(orig_buf); nrf24_deinit(&radio);
    return 0;

cleanup:
    // Close handles and free memory on stop
    if (orig_buf) free(orig_buf);
    nrf24_deinit(&radio);
    return 1;
}

// ==========================================
// --- INTEGRATED RX LOGIC ---
// ==========================================

static int run_rx(const char *spi_dev, int ce_bcm, const char *output_path) {
    nrf24_t radio;
    nrf24_config_t cfg = { .spi_device = spi_dev, .spi_speed_hz = 8000000, .ce_gpio = (uint8_t)ce_bcm };
    if (nrf24_init(&radio, &cfg) < 0) return 1;
    if (nrf24_configure_quick(&radio, P2P_CHANNEL) < 0) { nrf24_deinit(&radio); return 1; }
    if (nrf24_set_mode_rx(&radio) < 0) { nrf24_deinit(&radio); return 1; }

    FILE *fout = fopen(output_path, "wb");
    if (!fout) { ERROR("Open failed: %s", output_path); nrf24_deinit(&radio); return 1; }

    PageStream stream; page_stream_init(&stream);
    INFO("RX Start. Output: %s", output_path);

    int transfer_finished = 0;
    // RX Logic Vars
    int page_finished[MAX_PAGES] = {0};
    int have_page_info = 0; uint8_t cur_page = 0; uint16_t exp_bursts = 0;
    uint8_t bursts_recv[MAX_BURSTS_PER_PAGE] = {0};
    unsigned bursts_done = 0; int in_burst = 0;
    unsigned cur_burst = 0; unsigned frames_in_burst = 0;
    uint8_t sizes[MAX_CHUNKS_PER_BURST];
    uint8_t cur_burst_data[MAX_CHUNKS_PER_BURST][MAX_PAYLOAD];
    uint64_t comp_total = 0, uncomp_total = 0;

    while (!transfer_finished) {
        // [INTEGRATION] UI Hooks
        if (should_reset()) goto cleanup; 
        toggle_activity_led();            

        uint8_t buf[32]; uint8_t len = 32;
        int ret = nrf24_recv_blocking(&radio, buf, &len, 20); // Short timeout to allow blinking
        if (ret < 0 && errno != ETIMEDOUT) break;
        if (len == 0) continue;

        // STREAM_INFO
        if (len >= 8 && buf[0] == MSG_INFO && buf[1] == P2P_MSG_STREAM_INFO) {
            if (have_page_info && bursts_done > 0 && !page_finished[cur_page]) {
                decompress_page_to_file(&stream, fout, &comp_total, &uncomp_total);
                page_finished[cur_page] = 1;
            }
            page_stream_free(&stream); page_stream_init(&stream);
            cur_page = buf[2]; exp_bursts = decode_u16_le(&buf[4]);
            if (exp_bursts > MAX_BURSTS_PER_PAGE) exp_bursts = MAX_BURSTS_PER_PAGE;
            memset(bursts_recv, 0, sizeof(bursts_recv));
            bursts_done = 0; in_burst = 0; have_page_info = 1;
            continue;
        }

        // BURST_INFO
        if (len >= 6 && buf[0] == MSG_INFO && buf[1] == MSG_BURST_INFO) {
            cur_burst = buf[3];
            uint16_t sz = decode_u16_le(&buf[4]);
            frames_in_burst = (sz + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
            uint8_t last_len = (sz % MAX_PAYLOAD) ? (sz % MAX_PAYLOAD) : MAX_PAYLOAD;
            for(unsigned i=0; i<frames_in_burst; ++i) sizes[i] = (i==frames_in_burst-1)?last_len:MAX_PAYLOAD;
            in_burst = 1;
            continue;
        }

        // FINISH
        if (len >= 2 && buf[0] == MSG_INFO && buf[1] == MSG_TRANSFER_FINISH) {
            transfer_finished = 1; break;
        }

        // DATA
        if (in_burst && len > 0) {
            uint8_t fid = buf[0];
            if (fid < frames_in_burst && len == sizes[fid]) {
                memcpy(cur_burst_data[fid], buf, len);
                
                if (fid == frames_in_burst - 1) { // End of Burst
                     uint64_t chk; checksum_init(&chk);
                     for(unsigned i=0; i<frames_in_burst; ++i) checksum_update(&chk, cur_burst_data[i], sizes[i]);
                     uint8_t chk_b[CHECKSUM_SIZE]; checksum_final(chk, chk_b);

                     if (!page_finished[cur_page]) {
                         store_burst(&stream, cur_burst, frames_in_burst, cur_burst_data, sizes);
                         if (cur_burst < MAX_BURSTS_PER_PAGE && !bursts_recv[cur_burst]) {
                             bursts_recv[cur_burst] = 1; bursts_done++;
                         }
                     }

                     // Send Checksum
                     nrf24_set_mode_tx(&radio);
                     unsigned att = 0; int sent = 0;
                     while (!sent && att < 10) {
                         if (nrf24_send_blocking(&radio, chk_b, CHECKSUM_SIZE, CONTROL_TIMEOUT_MS) == 0) sent = 1;
                         att++;
                     }
                     nrf24_set_mode_rx(&radio);

                     if (!page_finished[cur_page] && bursts_done >= exp_bursts) {
                         decompress_page_to_file(&stream, fout, &comp_total, &uncomp_total);
                         page_finished[cur_page] = 1;
                     }
                }
            }
        }
    }
    
    // Final cleanup
    if (have_page_info && !page_finished[cur_page]) decompress_page_to_file(&stream, fout, &comp_total, &uncomp_total);

    page_stream_free(&stream); fclose(fout); nrf24_deinit(&radio);
    return 0;

cleanup:
    page_stream_free(&stream); fclose(fout); nrf24_deinit(&radio);
    return 1;
}


// ==========================================
// --- MAIN STATE MACHINE ---
// ==========================================
int main(void) {
    if (wiringPiSetupGpio() == -1) { fprintf(stderr, "WiringPi Fail\n"); return 1; }

    pinMode(LED_INSERT_USB, OUTPUT);    digitalWrite(LED_INSERT_USB, LOW);
    pinMode(LED_EXTRACT_USB, OUTPUT);   digitalWrite(LED_EXTRACT_USB, LOW);
    pinMode(LED_DEVICE_CONFIG, OUTPUT); digitalWrite(LED_DEVICE_CONFIG, LOW);
    pinMode(LED_RXTX_STATUS, OUTPUT);   digitalWrite(LED_RXTX_STATUS, LOW);

    pinMode(BTN_INTERACT, INPUT);       pullUpDnControl(BTN_INTERACT, PUD_UP);
    pinMode(BTN_STOP, INPUT);           pullUpDnControl(BTN_STOP, PUD_UP);
    pinMode(SWITCH_MODE, INPUT);        pullUpDnControl(SWITCH_MODE, PUD_UP);
    pinMode(SWITCH_SCENARIO, INPUT);    pullUpDnControl(SWITCH_SCENARIO, PUD_UP);

    wiringPiISR(BTN_STOP, INT_EDGE_FALLING, &trigger_reset);

    log_init("radio_integrated.log");
    INFO("--- SYSTEM ONLINE ---");

    char usb_path[256];
    char file_path[512];

    start_of_loop:
    while(1) {
        // 0. RESET
        global_stop_flag = false;
        digitalWrite(LED_INSERT_USB, LOW); digitalWrite(LED_EXTRACT_USB, LOW);
        digitalWrite(LED_RXTX_STATUS, LOW); digitalWrite(LED_DEVICE_CONFIG, LOW);

        // 1. CONFIG
        printf("\n[State] Config (Set Switches, Press Interact)\n");
        if (wait_with_blink(LED_DEVICE_CONFIG, BTN_INTERACT, NULL, NULL, 0.5)) goto start_of_loop;
        
        bool is_tx = (digitalRead(SWITCH_MODE) == 0);
        printf("[Info] Mode: %s\n", is_tx ? "TX" : "RX");
        digitalWrite(LED_DEVICE_CONFIG, HIGH);

        // 2. USB WAIT
        if (should_reset()) goto start_of_loop;
        printf("[State] Waiting for USB...\n");
        if (wait_with_blink(LED_INSERT_USB, -1, (bool (*)(char*))check_usb_connected, usb_path, 0.5)) goto start_of_loop;
        
        printf("[Info] USB: %s\n", usb_path);
        delay(1000); digitalWrite(LED_INSERT_USB, HIGH);

        // Prepare Files
        if (is_tx) {
            if (find_first_file(usb_path, file_path, sizeof(file_path)) != 0) {
                ERROR("No files found!");
                while(!should_reset()) { digitalWrite(LED_INSERT_USB, !digitalRead(LED_INSERT_USB)); delay(100); }
                goto start_of_loop;
            }
            printf("[Info] File to send: %s\n", file_path);
        } else {
            snprintf(file_path, sizeof(file_path), "%s/rx_%ld.dat", usb_path, time(NULL));
            printf("[Info] Saving to: %s\n", file_path);
        }

        // 3. READY TO START
        if (should_reset()) goto start_of_loop;
        printf("[State] Ready. Press Interact.\n");
        if (wait_with_blink(-1, BTN_INTERACT, NULL, NULL, 0)) goto start_of_loop;

        // 4. RUNNING
        if (should_reset()) goto start_of_loop;
        printf("[State] Running...\n");

        if (is_tx) {
            run_tx("/dev/spidev0.0", 22, file_path);
        } else {
            run_rx("/dev/spidev0.0", 22, file_path);
        }
        
        digitalWrite(LED_RXTX_STATUS, LOW); // Ensure OFF
        
        if (should_reset()) goto start_of_loop;

        // 5. FINISH
        printf("[State] Done. Press Interact.\n");
        if (wait_with_blink(-1, BTN_INTERACT, NULL, NULL, 0)) goto start_of_loop;

        // 6. REMOVE USB
        if (should_reset()) goto start_of_loop;
        printf("[State] Remove USB.\n");
        while(check_usb_connected(NULL)) {
            if (should_reset()) goto start_of_loop;
            digitalWrite(LED_EXTRACT_USB, !digitalRead(LED_EXTRACT_USB));
            delay(500);
        }
        
        printf("[Success] Restarting...\n");
        delay(2000);
    }
    return 0;
}
//No ability to type commands in the terminal (replaced by physical switches).
//Make sure you have your libs folder in the same directory. Compile command BASH:
//gcc -o radio_app main_integrated.c libs/nrf24.c libs/utils.c -lwiringPi -lz -lm