
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wiringPi.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "p2p_mode.h"

//
// ---- Configuration (BCM numbers)
//
#define LED_INSERT_USB     25
#define LED_EXTRACT_USB    26
#define LED_DEVICE_CONFIG  23
#define LED_RXTX_STATUS    16

#define BTN_INTERACT       19
#define BTN_STOP           6
#define SWITCH_MODE        27 
#define SWITCH_SCENARIO    17   

#define USB_MOUNT_PREFIX   "/media"
#define NM_SCRIPT_PATH     "/home/nm.py"    
#define P2P_SCRIPT_PATH    "/home/p2p_mode.c"
#define SPI_DEVICE         "/dev/spidev0.0"
#define CE_GPIO            25

// prototypes for existing C functions (must be linked)
extern int run_tx(const char *spi_dev, int ce_bcm, const char *input_path);
extern int run_rx(const char *spi_dev, int ce_bcm, const char *output_path);

// Utility logging macros
#define INFO(fmt, ...)  do { printf("[INFO] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while(0)
#define ERROR(fmt, ...) do { fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__); fflush(stderr); } while(0)

// Global control flags
volatile sig_atomic_t quit_requested = 0;       // for SIGINT
volatile int global_stop_flag = 0;              // set when STOP pressed -> soft reset

// LED blink control structures
typedef struct {
    int pin;
    pthread_t thread;
    volatile int active;    // 0 = not blinking, 1 = blink
    volatile int stop;      // request thread to stop
    double on_time;
    double off_time;
    pthread_mutex_t mtx;
} blink_t;

blink_t led_insert = { .pin = LED_INSERT_USB, .active = 0, .stop = 0, .on_time = 0.5, .off_time = 0.5, .mtx = PTHREAD_MUTEX_INITIALIZER };
blink_t led_extract = { .pin = LED_EXTRACT_USB, .active = 0, .stop = 0, .on_time = 0.5, .off_time = 0.5, .mtx = PTHREAD_MUTEX_INITIALIZER };
blink_t led_device_config = { .pin = LED_DEVICE_CONFIG, .active = 0, .stop = 0, .on_time = 0.5, .off_time = 0.5, .mtx = PTHREAD_MUTEX_INITIALIZER };
// RX/TX status LED rarely blinks, but we will control it directly
int led_rxtx_pin = LED_RXTX_STATUS;

// forward declarations
void *blink_thread_fn(void *arg);
int start_blink(blink_t *b, double on_time, double off_time);
int stop_blink(blink_t *b);
int usb_connected(void);
void trigger_reset_from_isr(void);
void sighandler(int signo);
void cleanup_and_exit(int code);
void gpio_setup(void);
void set_led(int pin, int value);
int read_button_debounced(int pin, int stable_ms);

// =========================
// Thread-safe blink control
// =========================
void *blink_thread_fn(void *arg)
{
    blink_t *b = (blink_t *)arg;

    while (!b->stop) {
        pthread_mutex_lock(&b->mtx);
        int active = b->active;
        double on_t = b->on_time;
        double off_t = b->off_time;
        pthread_mutex_unlock(&b->mtx);

        if (!active) {
            // sleep short while waiting to be reactivated or stop
            usleep(100000);
            continue;
        }

        // turn on
        digitalWrite(b->pin, HIGH);
        // respect stop check in chunks
        for (int i = 0; i < (int)(on_t * 10) && !b->stop && b->active; ++i) usleep(100000);
        // turn off
        digitalWrite(b->pin, LOW);
        for (int i = 0; i < (int)(off_t * 10) && !b->stop && b->active; ++i) usleep(100000);
    }

    // ensure LED off when thread exits
    digitalWrite(b->pin, LOW);
    return NULL;
}

int start_blink(blink_t *b, double on_time, double off_time)
{
    pthread_mutex_lock(&b->mtx);
    b->on_time = on_time;
    b->off_time = off_time;
    b->active = 1;
    b->stop = 0;
    pthread_mutex_unlock(&b->mtx);

    // if thread not started yet, create it
    if (b->thread == 0) {
        if (pthread_create(&b->thread, NULL, blink_thread_fn, b) != 0) {
            ERROR("Failed to create blink thread for pin %d", b->pin);
            b->thread = 0;
            return -1;
        }
    }
    return 0;
}

int stop_blink(blink_t *b)
{
    pthread_mutex_lock(&b->mtx);
    b->active = 0;
    pthread_mutex_unlock(&b->mtx);
    // leave thread alive (it will idly wait). To actually stop and join thread, we'd set stop=1.
    return 0;
}

void shutdown_blink(blink_t *b)
{
    if (b->thread) {
        pthread_mutex_lock(&b->mtx);
        b->stop = 1;
        b->active = 0;
        pthread_mutex_unlock(&b->mtx);
        pthread_join(b->thread, NULL);
        b->thread = 0;
    }
}

// =========================
// USB detection (check /proc/mounts for /media/*)
// =========================
int usb_connected(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        // line format: <device> <mountpoint> <fstype> ...
        char device[256], mnt[512], fstype[128];
        if (sscanf(line, "%255s %511s %127s", device, mnt, fstype) == 3) {
            if (strncmp(mnt, USB_MOUNT_PREFIX, strlen(USB_MOUNT_PREFIX)) == 0) {
                found = 1;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

// =========================
// Button STOP polling helper
// =========================
void *stop_monitor_thread(void *arg)
{
    (void)arg;
    while (!quit_requested) {
        int v = digitalRead(BTN_STOP);
        if (v == LOW) { // with pull-up: pressed -> LOW
            // Debounce small
            delay(20);
            if (digitalRead(BTN_STOP) == LOW) {
                if (!global_stop_flag) {
                    INFO("[Interrupt] STOP Pressed! Resetting to Start...");
                    global_stop_flag = 1;
                }
                // wait until released to avoid toggling repeatedly
                while (digitalRead(BTN_STOP) == LOW && !quit_requested) delay(50);
            }
        }
        delay(50);
    }
    return NULL;
}

// =========================
// Utilities
// =========================
void set_led(int pin, int value)
{
    digitalWrite(pin, value ? HIGH : LOW);
}

int read_button_debounced(int pin, int stable_ms)
{
    int last = digitalRead(pin);
    int stable_count = 0;
    int iterations = stable_ms / 10;
    for (int i = 0; i < iterations; ++i) {
        delay(10);
        int now = digitalRead(pin);
        if (now == last) stable_count++;
        else { last = now; stable_count = 0; }
    }
    return last;
}

// =========================
// Signal handling & cleanup
// =========================
void sighandler(int signo)
{
    (void)signo;
    quit_requested = 1;
}

void cleanup_and_exit(int code)
{
    // stop blink threads
    shutdown_blink(&led_insert);
    shutdown_blink(&led_extract);
    shutdown_blink(&led_device_config);

    // turn off LEDs
    set_led(LED_INSERT_USB, 0);
    set_led(LED_EXTRACT_USB, 0);
    set_led(LED_DEVICE_CONFIG, 0);
    set_led(LED_RXTX_STATUS, 0);

    INFO("Exiting.");
    exit(code);
}

// =========================
// GPIO setup
// =========================
void gpio_setup(void)
{
    if (wiringPiSetupGpio() == -1) {
        ERROR("wiringPiSetupGpio failed (are you on Raspberry Pi?).");
        exit(1);
    }

    // LEDs outputs
    pinMode(LED_INSERT_USB, OUTPUT);
    pinMode(LED_EXTRACT_USB, OUTPUT);
    pinMode(LED_DEVICE_CONFIG, OUTPUT);
    pinMode(LED_RXTX_STATUS, OUTPUT);

    // Buttons and switches as inputs with pull-up (matches gpiozero defaults)
    pinMode(BTN_INTERACT, INPUT);
    pullUpDnControl(BTN_INTERACT, PUD_UP);

    pinMode(BTN_STOP, INPUT);
    pullUpDnControl(BTN_STOP, PUD_UP);

    pinMode(SWITCH_MODE, INPUT);
    pullUpDnControl(SWITCH_MODE, PUD_UP);

    pinMode(SWITCH_SCENARIO, INPUT);
    pullUpDnControl(SWITCH_SCENARIO, PUD_UP);

    // Ensure LEDs off initially
    digitalWrite(LED_INSERT_USB, LOW);
    digitalWrite(LED_EXTRACT_USB, LOW);
    digitalWrite(LED_DEVICE_CONFIG, LOW);
    digitalWrite(LED_RXTX_STATUS, LOW);
}

// =========================
// Main state machine (faithful to your Python code)
// =========================
int main(void)
{
    gpio_setup();

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    INFO("--- SYSTEM ONLINE ---");

    // start separate monitor thread for STOP button to set global_stop_flag quickly
    pthread_t stop_thread;
    pthread_create(&stop_thread, NULL, stop_monitor_thread, NULL);

    while (!quit_requested) {
        // Reset state
        global_stop_flag = 0;
        stop_blink(&led_insert);
        stop_blink(&led_extract);
        stop_blink(&led_device_config);
        digitalWrite(LED_INSERT_USB, LOW);
        digitalWrite(LED_EXTRACT_USB, LOW);
        digitalWrite(LED_RXTX_STATUS, LOW);
        digitalWrite(LED_DEVICE_CONFIG, LOW);

        // --- 1. INITIALIZATION & CONFIG ---
        INFO("\n[State] Device Configuration");
        // Start blinking device_config LED (indicates waiting for config)
        start_blink(&led_device_config, 0.5, 0.5);

        INFO("[User] Introduce the configuration and press INTERACT to confirm. Press STOP to reset.");

        // Wait for INTERACT pressed, checking for STOP
        while (!quit_requested) {
            int interact_val = read_button_debounced(BTN_INTERACT, 50); // pressed -> LOW
            if (global_stop_flag) break; // soft reset requested
            if (interact_val == LOW) break; // pressed
            delay(50);
        }
        if (quit_requested) break;
        if (global_stop_flag) { INFO("!!! IMMEDIATE RESET TRIGGERED !!!"); continue; }

        // Read hardware switches
        int switch_mode_val = digitalRead(SWITCH_MODE);       // pull-up: HIGH when open
        int switch_scenario_val = digitalRead(SWITCH_SCENARIO); // pull-up: HIGH when open

        const char *mode = (switch_mode_val ? "TX" : "RX");      // matches original: "TX" if is_active else "RX"
        const char *scenario = (switch_scenario_val ? "Network" : "P2P");

        INFO("[Info] Current Settings: Mode=%s, Scenario=%s", mode, scenario);

        // Button Pressed -> LED turns solid on
        stop_blink(&led_device_config);
        digitalWrite(LED_DEVICE_CONFIG, HIGH);
        INFO("[Config] Configuration Accepted.");

        // --- 2. USB INSERTION PHASE ---
        if (global_stop_flag) { INFO("!!! IMMEDIATE RESET TRIGGERED !!!"); continue; }

        INFO("[State] Waiting for USB or checking USB...");
        // Device Config is solid here. Insert USB starts blinking.
        start_blink(&led_insert, 0.5, 0.5);

        while (!quit_requested) {
            if (global_stop_flag) break;
            if (usb_connected()) break;
            delay(100);
        }
        if (quit_requested) break;
        if (global_stop_flag) { INFO("!!! IMMEDIATE RESET TRIGGERED !!!"); continue; }

        INFO("USB connected");
        sleep(1); // allow OS to finish mounts
        // USB Detected: LED goes solid
        stop_blink(&led_insert);
        digitalWrite(LED_INSERT_USB, HIGH);

        // Ensure Device Config is still on
        digitalWrite(LED_DEVICE_CONFIG, HIGH);

        // --- 3. INTERACT PHASE ---
        if (global_stop_flag) { INFO("!!! IMMEDIATE RESET TRIGGERED !!!"); continue; }

        INFO("[State] Ready. Press INTERACT to Execute Task.");
        while (!quit_requested) {
            int v = read_button_debounced(BTN_INTERACT, 50);
            if (global_stop_flag) break;
            if (v == LOW) break;
            delay(50);
        }
        if (quit_requested) break;
        if (global_stop_flag) { INFO("!!! IMMEDIATE RESET TRIGGERED !!!"); continue; }

        // --- 4. TX/RX PROCESS PHASE ---
        if (global_stop_flag) { INFO("!!! IMMEDIATE RESET TRIGGERED !!!"); continue; }
        INFO("[State] Performing %s Task...", mode);
        digitalWrite(LED_RXTX_STATUS, HIGH);

        if (strcmp(scenario, "P2P") == 0) {
            INFO("[State] Simple mode (P2P). Launching point-to-point flow...");
            if (strcmp(mode, "TX") == 0) {
                INFO("[P2P] BEGIN_TRANSMITTER_MODE()");
                int rc = run_tx(SPI_DEVICE, CE_GPIO, P2P_SCRIPT_PATH);
                if (rc != 0) ERROR("run_tx returned %d", rc);
            } else {
                INFO("[P2P] BEGIN_RECEIVER_MODE()");
                int rc = run_rx(SPI_DEVICE, CE_GPIO, P2P_SCRIPT_PATH);
                if (rc != 0) ERROR("run_rx returned %d", rc);
            }
        } else { // Network
            INFO("[State] Network mode selected.");
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "python3 %s", NM_SCRIPT_PATH);
            INFO("[Network] Launching: %s", cmd);
            int rc = system(cmd);
            if (rc != 0) {
                ERROR("Network script failed with exit code %d", rc);
            }
        }

        // End Tx/Rx
        digitalWrite(LED_RXTX_STATUS, LOW);

        INFO("[State] Task Finished. Press interact please");
        sleep(1);

        // Wait for INTERACT press to continue
        while (!quit_requested) {
            int v = read_button_debounced(BTN_INTERACT, 50);
            if (global_stop_flag) break;
            if (v == LOW) break;
            delay(50);
        }
        if (quit_requested) break;
        if (global_stop_flag) { INFO("!!! IMMEDIATE RESET TRIGGERED !!!"); continue; }

        // --- 5. COMPLETION & EXTRACTION ---
        if (global_stop_flag) { INFO("!!! IMMEDIATE RESET TRIGGERED !!!"); continue; }

        INFO("[State] Please Remove USB.");
        start_blink(&led_extract, 0.5, 0.5);

        while (!quit_requested) {
            if (global_stop_flag) break;
            if (!usb_connected()) break;
            delay(100);
        }
        if (quit_requested) break;
        if (global_stop_flag) { INFO("!!! IMMEDIATE RESET TRIGGERED !!!"); continue; }

        // Cleanup
        stop_blink(&led_insert);
        stop_blink(&led_extract);
        stop_blink(&led_device_config);

        digitalWrite(LED_INSERT_USB, LOW);
        digitalWrite(LED_EXTRACT_USB, LOW);
        digitalWrite(LED_DEVICE_CONFIG, LOW);
        digitalWrite(LED_RXTX_STATUS, LOW);

        INFO("[Success] Cycle Complete. Restarting in 3 seconds...");
        // wait with stop check
        for (int i = 0; i < 30 && !quit_requested; ++i) {
            if (global_stop_flag) break;
            delay(100);
        }
        if (global_stop_flag) { INFO("!!! IMMEDIATE RESET TRIGGERED !!!"); continue; }
    }

    // shutdown
    quit_requested = 1;
    pthread_join(stop_thread, NULL);

    cleanup_and_exit(0);
    return 0;
}
