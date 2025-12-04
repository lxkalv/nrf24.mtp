#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <zlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <wiringPi.h> // UI Hardware Control
#include <mntent.h>   // USB Detection
#include <dirent.h>   // Directory listing

// Include your local libraries
#include "libs/nrf24.h"
#include "libs/logger.h"
#include "libs/app_layer.h"

// ==========================================
// --- UI HARDWARE DEFINITIONS ---
// ==========================================
#define LED_INSERT_USB    25 
#define LED_EXTRACT_USB   26
#define LED_DEVICE_CONFIG 23
#define LED_RXTX_STATUS   16

#define BTN_INTERACT      19
#define BTN_STOP          6
#define SWITCH_MODE       27 // TX vs RX
#define SWITCH_SCENARIO   17 // P2P vs Network

// --- GLOBAL CONTROL FLAGS ---
volatile bool global_stop_flag = false;

// --- EXISTING RADIO CONSTANTS ---
#define MAX_PAYLOAD         32
#define CONTROL_PREFIX      0xFF
#define DATA_PREFIX         0x00
#define DEFAULT_SPI_DEVICE  "/dev/spidev0.0"
// ... (Your other define constants remain here) ...
#define MSG_STREAM_INFO     0x01
#define MSG_STREAM_FINISH   0x02
#define MSG_CHECKSUM        0x03
#define MSG_STREAM_READY    0x04
#define CHECKSUM_SIZE       8
#define CHECKSUM_SEND_WINDOW_MS 2000
#define READY_TIMEOUT_MS    2000
#define CONTROL_TIMEOUT_MS  100
#define DATA_TIMEOUT_MS     20
#define CHECKSUM_TIMEOUT_MS 1000
#define STREAM_INFO_SIZE    16
#define STREAM_READY_SIZE   11
#define STREAM_READY_MAX_ATTEMPTS 400
#define STREAM_READY_WINDOW_MS    2000
#define FNV64_OFFSET_BASIS  1469598103934665603ULL
#define FNV64_PRIME         1099511628211ULL

// ==========================================
// --- UI HELPER FUNCTIONS ---
// ==========================================

// Interrupt Service Routine for STOP button
void trigger_reset(void) {
    if (!global_stop_flag) {
        // printf inside ISR is risky but okay for simple debugging
        // global_stop_flag = true causes loops to break
        global_stop_flag = true; 
    }
}

// Check if we should abort current operation
bool should_reset() {
    return global_stop_flag;
}

// Blink Status LED to indicate activity (called inside radio loops)
void toggle_activity_led() {
    static unsigned long last_toggle = 0;
    static bool state = false;
    unsigned long now = millis();
    // Blink fast (every 50ms) during transfer
    if (now - last_toggle > 50) { 
        state = !state;
        digitalWrite(LED_RXTX_STATUS, state);
        last_toggle = now;
    }
}

// Helper: Check for mounted USB
bool check_usb_connected(char *mount_point_buffer) {
    struct mntent *ent;
    FILE *aFile;

    aFile = setmntent("/proc/mounts", "r");
    if (aFile == NULL) return false;

    while (NULL != (ent = getmntent(aFile))) {
        if (strncmp(ent->mnt_dir, "/media", 6) == 0) {
            if (mount_point_buffer) strcpy(mount_point_buffer, ent->mnt_dir);
            endmntent(aFile);
            return true;
        }
    }
    endmntent(aFile);
    return false;
}

// Helper: Find first file in directory (for TX)
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

// UI Wait Loop (Blinks LED while waiting for button or condition)
bool wait_with_blink(int led_pin, int button_pin, bool (*condition_func)(char*), char* cond_arg, float blink_interval_sec) {
    unsigned long last_blink = millis();
    bool led_state = false;
    int interval_ms = (int)(blink_interval_sec * 1000);

    while (1) {
        if (should_reset()) return true;

        if (button_pin != -1) {
            if (digitalRead(button_pin) == 0) return false; 
        }
        if (condition_func != NULL) {
            if (condition_func(cond_arg)) return false;
        }

        if (led_pin != -1 && interval_ms > 0) {
            unsigned long current_time = millis();
            if (current_time - last_blink >= interval_ms) {
                led_state = !led_state;
                digitalWrite(led_pin, led_state);
                last_blink = current_time;
            }
        }
        delay(50);
    }
}

// ==========================================
// --- RADIO LOGIC (Modified for Integration) ---
// ==========================================

// ... (Keep struct robust_radio_params_t, map_data_rate_kbps, etc. UNCHANGED) ...
// ... (Keep sleep_ms_posix, map_pa_level_dbm, map_crc_bytes, update_radio_params UNCHANGED) ...
// ... (Keep configure_radio_runtime, maybe_verify_radio_config, get_spi_device_path UNCHANGED) ...
// ... (Keep now_seconds, checksum functions, encode/decode/compress/decompress/derive_frame_layout UNCHANGED) ...

// *** MODIFIED FUNCTION: send_with_retries ***
// Added: should_reset() check and toggle_activity_led()
static int send_with_retries(nrf24_t *radio,
                             const uint8_t *buf,
                             uint8_t len,
                             unsigned timeout_ms,
                             const char *label,
                             uint64_t *rf_bytes_total,
                             uint64_t *rf_frames_total)
{
    // ... (Keep validation checks) ...

    unsigned attempt = 0;
    while (1) {
        // --- INTEGRATION: CHECK STOP BUTTON ---
        if (should_reset()) return -1;
        // --- INTEGRATION: BLINK LED ---
        toggle_activity_led();

        if (rf_bytes_total) *rf_bytes_total += len;
        if (rf_frames_total) *rf_frames_total += 1;

        int ret = nrf24_send_blocking(radio, buf, len, timeout_ms);
        if (ret == 0) return 0;

        // ... (Keep error handling and reconfig logic UNCHANGED) ...
        
        if (errno != ETIMEDOUT) {
             // log error
             return -1;
        }
        
        ++attempt;
        // ... (Keep logging logic) ...
        if ((attempt % 200) == 0) {
            // ... (Keep reconfig logic) ...
            if (configure_radio_runtime(radio) != 0) return -1;
        }
    }
}

// ... (Keep ensure_mode_tx, ensure_mode_rx UNCHANGED) ...
// ... (Keep send_stream_info, send_stream_finish UNCHANGED - they call send_with_retries so they are safe) ...

// *** MODIFIED FUNCTION: send_checksum_with_timeout ***
static int send_checksum_with_timeout(nrf24_t *radio,
                                      uint64_t checksum,
                                      uint64_t *rf_bytes_total,
                                      uint64_t *rf_frames_total)
{
    // ... (Keep setup logic) ...
    uint8_t msg[2 + CHECKSUM_SIZE];
    // ... (Fill msg) ...

    double start = now_seconds();
    unsigned attempt = 0;

    while ((now_seconds() - start) * 1000.0 < CHECKSUM_SEND_WINDOW_MS) {
        if (should_reset()) return -1; // <--- ADDED
        toggle_activity_led();         // <--- ADDED

        // ... (Keep existing while loop logic) ...
        
        // Ensure sleep uses checkable delay or short sleep
        sleep_ms_posix(50); 
    }
    errno = ETIMEDOUT;
    return -1;
}

// *** MODIFIED FUNCTION: wait_for_stream_ready ***
static int wait_for_stream_ready(nrf24_t *radio,
                                 uint8_t expected_id_bytes,
                                 uint32_t expected_frames,
                                 uint32_t expected_comp_len)
{
    if (ensure_mode_rx(radio) != 0) return -1;

    double wait_start = now_seconds();
    while ((now_seconds() - wait_start) * 1000.0 < READY_TIMEOUT_MS) {
        if (should_reset()) return -1; // <--- ADDED
        toggle_activity_led();         // <--- ADDED

        // ... (Rest of logic UNCHANGED) ...
        uint8_t buf[MAX_PAYLOAD];
        uint8_t len = sizeof(buf);
        int ret = nrf24_recv_blocking(radio, buf, &len, 200);
        // ...
    }
    // ...
    return 1;
}

// *** MODIFIED FUNCTION: send_stream_ready ***
// Needs to call should_reset in loop
static int send_stream_ready(nrf24_t *radio, uint8_t id_bytes, uint32_t expected_frames, uint32_t compressed_len, uint64_t *rf_bytes_total, uint64_t *rf_frames_total) {
    // ... setup ...
    double start = now_seconds();
    unsigned attempt = 0;
    int sent = 0;
    while (attempt < STREAM_READY_MAX_ATTEMPTS && (now_seconds() - start) * 1000.0 < STREAM_READY_WINDOW_MS) {
        if (should_reset()) return -1; // <--- ADDED
        toggle_activity_led();
        // ... (Rest of logic UNCHANGED) ...
    }
    // ...
    return sent ? 0 : 1;
}

// *** MODIFIED FUNCTION: run_tx ***
static int run_tx(const char *spi_dev,
                  const app_config_t *cfg,
                  const uint8_t *file_data,
                  size_t file_len)
{
    // ... (Keep compression and setup logic UNCHANGED) ...
    // ... (Keep radio init UNCHANGED) ...

    // INSIDE MAIN LOOPS, ensure should_reset is checked
    // Fortunately, most checks are handled inside send_with_retries and wait_for_stream_ready.
    
    // Just ensure top-level loops check it:
    while (1) {
        if (should_reset()) goto cleanup; // <--- ADDED
        int ready_ret = wait_for_stream_ready(&radio, (uint8_t)id_bytes, total_frames, (uint32_t)compressed_len);
        if (ready_ret == 0) break;
        if (ready_ret < 0) goto cleanup;
        // ... resend info logic ...
    }

    // ...
    while (!transfer_complete) {
        if (should_reset()) goto cleanup; // <--- ADDED
        // ... (Sending chunks logic calls send_with_retries, which checks reset) ...
        // ...
        
        // Wait for checksum
        while (!checksum_ok && (now_seconds() - wait_start) * 1000.0 < CHECKSUM_TIMEOUT_MS) {
             if (should_reset()) goto cleanup;
             toggle_activity_led();
             // ... recv logic ...
        }
        // ...
    }
    
    // ... Cleanup and Return ...
cleanup:
    free(compressed);
    // If we aborted via reset, ensure we return a fail code or just exit
    if (should_reset()) return -1;
    return exit_code;
}

// *** MODIFIED FUNCTION: run_rx ***
static int run_rx(const char *spi_dev, const app_config_t *cfg)
{
    // ... (Radio Init UNCHANGED) ...
    
    int done = 0;
    while (!done) {
        if (should_reset()) goto cleanup; // <--- ADDED
        toggle_activity_led();            // <--- ADDED

        uint8_t buf[MAX_PAYLOAD];
        uint8_t len = sizeof(buf);
        int ret = nrf24_recv_blocking(&radio, buf, &len, 500);
        
        // ... (Rest of RX Logic UNCHANGED) ...
        // The RX logic is one giant loop, so adding the check at the top is usually sufficient.
        
        // Ensure nested loops (like send_checksum_with_timeout) also have the check (which we added above).
    }

    // ... (Decompression and File Save UNCHANGED) ...

cleanup:
    nrf24_deinit(&radio);
    free(frame_received);
    free(compressed);
    if (should_reset()) return -1;
    return 1;
}

// ==========================================
// --- NEW MAIN FUNCTION (UI Integration) ---
// ==========================================

// Dummy structs for Radio params
robust_radio_params_t g_radio_params = {
    .channel = 76, .data_rate_kbps = 2000, .pa_level_dbm = 0, .crc_bytes = 2, .retr_delay = 2, .retr_tries = 15
};

int main(int argc, char **argv)
{
    // 1. SETUP WIRING PI
    if (wiringPiSetupGpio() == -1) {
        fprintf(stderr, "Failed to initialize GPIO\n");
        return 1;
    }

    // 2. CONFIGURE PINS
    pinMode(LED_INSERT_USB, OUTPUT);
    pinMode(LED_EXTRACT_USB, OUTPUT);
    pinMode(LED_DEVICE_CONFIG, OUTPUT);
    pinMode(LED_RXTX_STATUS, OUTPUT);
    pinMode(BTN_INTERACT, INPUT);    pullUpDnControl(BTN_INTERACT, PUD_UP);
    pinMode(BTN_STOP, INPUT);        pullUpDnControl(BTN_STOP, PUD_UP);
    pinMode(SWITCH_MODE, INPUT);     pullUpDnControl(SWITCH_MODE, PUD_UP);
    pinMode(SWITCH_SCENARIO, INPUT); pullUpDnControl(SWITCH_SCENARIO, PUD_UP);

    // 3. ATTACH STOP INTERRUPT
    wiringPiISR(BTN_STOP, INT_EDGE_FALLING, &trigger_reset);

    // 4. LOGGER SETUP
    logger_init("integrated_radio.log");
    logger_info("--- SYSTEM ONLINE ---");

    const char *spi_dev = DEFAULT_SPI_DEVICE;
    char usb_mount_path[256];
    char file_path[512];

    // --- MAIN STATE MACHINE ---
    start_of_loop:
    while (1) {
        // 0. RESET STATE
        global_stop_flag = false;
        digitalWrite(LED_INSERT_USB, LOW);
        digitalWrite(LED_EXTRACT_USB, LOW);
        digitalWrite(LED_RXTX_STATUS, LOW);
        digitalWrite(LED_DEVICE_CONFIG, LOW);

        // --- 1. INITIALIZATION & CONFIG ---
        printf("\n[State] Device Configuration\n");
        
        // Blink Config LED until Interact pressed
        if (wait_with_blink(LED_DEVICE_CONFIG, BTN_INTERACT, NULL, NULL, 0.5)) goto start_of_loop;

        // Read Hardware Switches
        bool is_tx = (digitalRead(SWITCH_MODE) == 0); // 0=Active=TX
        bool is_network = (digitalRead(SWITCH_SCENARIO) == 0);

        printf("[Info] Settings: Mode=%s, Scenario=%s\n", is_tx ? "TX" : "RX", is_network ? "Network" : "P2P");
        digitalWrite(LED_DEVICE_CONFIG, HIGH); // Solid ON

        // --- 2. USB INSERTION PHASE ---
        if (should_reset()) goto start_of_loop;
        printf("[State] Waiting for USB...\n");

        // Blink USB LED until check_usb_connected returns true
        if (wait_with_blink(LED_INSERT_USB, -1, (bool (*)(char*))check_usb_connected, usb_mount_path, 0.5)) goto start_of_loop;

        printf("USB connected at: %s\n", usb_mount_path);
        delay(1000);
        digitalWrite(LED_INSERT_USB, HIGH);

        // Prepare Files
        if (is_tx) {
            // Find first file in USB
            if (find_first_file(usb_mount_path, file_path, sizeof(file_path)) != 0) {
                logger_error("No files found on USB!");
                // Error state: fast blink USB LED forever or until reset
                while(!should_reset()) {
                    digitalWrite(LED_INSERT_USB, !digitalRead(LED_INSERT_USB));
                    delay(100);
                }
                goto start_of_loop;
            }
            printf("Selected file: %s\n", file_path);
        } else {
            // Generate output filename
            snprintf(file_path, sizeof(file_path), "%s/received_file_%ld.dat", usb_mount_path, time(NULL));
            printf("Save target: %s\n", file_path);
        }

        // --- 3. INTERACT PHASE ---
        if (should_reset()) goto start_of_loop;
        printf("[State] Ready. Press INTERACT to Execute.\n");
        if (wait_with_blink(-1, BTN_INTERACT, NULL, NULL, 0)) goto start_of_loop;

        // --- 4. EXECUTION PHASE ---
        if (should_reset()) goto start_of_loop;
        
        // Setup Config struct for Radio Logic
        app_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.ce_pin = 22; // Default
        cfg.verify_config = 1;
        
        if (is_tx) {
             cfg.mode = APP_MODE_TX;
             cfg.file_path_tx = file_path;
             
             // Load file data
             uint8_t *data = NULL;
             size_t len = 0;
             if (app_load_file_bytes(file_path, &data, &len) != 0) goto start_of_loop;
             
             // RUN TX
             printf("[State] Running TX...\n");
             run_tx(spi_dev, &cfg, data, len);
             free(data);
             
        } else {
             cfg.mode = APP_MODE_RX;
             cfg.file_path_rx = file_path;
             
             // RUN RX
             printf("[State] Running RX...\n");
             run_rx(spi_dev, &cfg);
        }

        digitalWrite(LED_RXTX_STATUS, LOW);
        
        // Check if we finished successfully or were reset
        if (should_reset()) goto start_of_loop;

        printf("[State] Task Finished. Press INTERACT.\n");
        delay(1000);
        if (wait_with_blink(-1, BTN_INTERACT, NULL, NULL, 0)) goto start_of_loop;

        // --- 5. COMPLETION & EXTRACTION ---
        if (should_reset()) goto start_of_loop;
        printf("[State] Please Remove USB.\n");
        
        // Blink Extract LED until USB gone
        while(check_usb_connected(NULL)) {
            if (should_reset()) goto start_of_loop;
            digitalWrite(LED_EXTRACT_USB, !digitalRead(LED_EXTRACT_USB));
            delay(500);
        }

        digitalWrite(LED_INSERT_USB, LOW); 
        digitalWrite(LED_EXTRACT_USB, LOW);
        digitalWrite(LED_DEVICE_CONFIG, LOW);
        
        printf("[Success] Restarting...\n");
        delay(3000);
    }

    return 0;
}