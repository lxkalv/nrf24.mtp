#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <wiringPi.h>
#include <mntent.h> // For checking mounts
#include <string.h>
#include <time.h>

// --- PIN CONFIGURATION (BCM Numbering) ---
#define LED_INSERT_USB    25 // este esta mal
#define LED_EXTRACT_USB   26
#define LED_DEVICE_CONFIG 23 // este bien
#define LED_RXTX_STATUS   16

#define BTN_INTERACT      19
#define BTN_STOP          6
#define SWITCH_MODE       27
#define SWITCH_SCENARIO   17

// --- GLOBAL FLAGS ---
// volatile is needed because this is changed by an Interrupt (ISR)
volatile bool global_stop_flag = false;

// --- INTERRUPT SERVICE ROUTINE ---
void trigger_reset(void) {
    if (!global_stop_flag) {
        printf("\n[Interrupt] STOP Pressed! Resetting to Start...\n");
        global_stop_flag = true;
    }
}

// --- HELPER FUNCTIONS ---

// Replaces Python's raise SoftReset check
// Returns true if we should stop/reset
bool should_reset() {
    return global_stop_flag;
}

// Check if a USB is mounted at /media
bool check_usb_connected() {
    struct mntent *ent;
    FILE *aFile;

    aFile = setmntent("/proc/mounts", "r");
    if (aFile == NULL) {
        return false;
    }

    while (NULL != (ent = getmntent(aFile))) {
        // Check if the mount directory starts with "/media"
        if (strncmp(ent->mnt_dir, "/media", 6) == 0) {
            endmntent(aFile);
            return true;
        }
    }

    endmntent(aFile);
    return false;
}

// Helper to handle blinking while waiting for button or condition
// This simulates Python's background blink + sleep
// Returns true if Reset triggered, false if Condition met
bool wait_with_blink(int led_pin, int button_pin, bool (*condition_func)(), float blink_interval_sec) {
    unsigned long last_blink = millis();
    bool led_state = false;
    int interval_ms = (int)(blink_interval_sec * 1000);

    while (1) {
        // 1. Check Stop
        if (should_reset()) return true;

        // 2. Check Exit Condition (Button press or Custom Function)
        if (button_pin != -1) {
            if (digitalRead(button_pin) == 0) return false; // 0 means pressed (with PullUp)
        }
        if (condition_func != NULL) {
            if (condition_func()) return false;
        }

        // 3. Blink Logic
        if (led_pin != -1) {
            unsigned long current_time = millis();
            if (current_time - last_blink >= interval_ms) {
                led_state = !led_state;
                digitalWrite(led_pin, led_state);
                last_blink = current_time;
            }
        }
        
        delay(50); // Small delay to prevent 100% CPU usage
    }
}

// --- MAIN ---
int main(void) {
    // 1. Setup WiringPi with BCM numbering
    if (wiringPiSetupGpio() == -1) {
        fprintf(stderr, "Failed to initialize GPIO\n");
        return 1;
    }

    // 2. Configure Pins
    pinMode(LED_INSERT_USB, OUTPUT);
    pinMode(LED_EXTRACT_USB, OUTPUT);
    pinMode(LED_DEVICE_CONFIG, OUTPUT);
    pinMode(LED_RXTX_STATUS, OUTPUT);

    pinMode(BTN_INTERACT, INPUT);
    pullUpDnControl(BTN_INTERACT, PUD_UP);

    pinMode(BTN_STOP, INPUT);
    pullUpDnControl(BTN_STOP, PUD_UP);
    
    // Attach Interrupt to Stop Button (Falling Edge = Pressed)
    wiringPiISR(BTN_STOP, INT_EDGE_FALLING, &trigger_reset);

    pinMode(SWITCH_MODE, INPUT);
    pullUpDnControl(SWITCH_MODE, PUD_UP);

    pinMode(SWITCH_SCENARIO, INPUT);
    pullUpDnControl(SWITCH_SCENARIO, PUD_UP);

    printf("--- SYSTEM ONLINE ---\n");

    // --- MAIN LOOP ---
    // The label 'start_of_loop' combined with 'goto' acts like the try/except/continue
    
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
        printf("[User] Introduce the configuration and press INTERACT.\n");

        // Wait for INTERACT button while Blinking Config LED
        if (wait_with_blink(LED_DEVICE_CONFIG, BTN_INTERACT, NULL, 0.5)) goto start_of_loop;

        // Read Switches (0 is Active because PullUp)
        const char* mode = (digitalRead(SWITCH_MODE) == 0) ? "TX" : "RX";
        const char* scenario = (digitalRead(SWITCH_SCENARIO) == 0) ? "Network" : "P2P";
        
        printf("[Info] Current Settings: Mode=%s, Scenario=%s\n", mode, scenario);
        printf("[Config] Configuration Accepted.\n");
        
        digitalWrite(LED_DEVICE_CONFIG, HIGH); // Solid ON

        // --- 2. USB INSERTION PHASE ---
        if (should_reset()) goto start_of_loop;
        
        printf("[State] Waiting for USB...\n");

        // Wait for check_usb_connected() to return true while blinking
        // We pass -1 as button_pin to ignore buttons here
        if (wait_with_blink(LED_INSERT_USB, -1, check_usb_connected, 0.5)) goto start_of_loop;

        printf("USB connected\n");
        delay(1000);
        digitalWrite(LED_INSERT_USB, HIGH); // Solid ON
        digitalWrite(LED_DEVICE_CONFIG, HIGH); // Ensure ON

        // --- 3. INTERACT PHASE ---
        if (should_reset()) goto start_of_loop;
        printf("[State] Ready. Press INTERACT to Execute Task.\n");

        // Wait for button, no blink (pass -1 as led)
        if (wait_with_blink(-1, BTN_INTERACT, NULL, 0)) goto start_of_loop;

        // --- 4. TX/RX PROCESS PHASE ---
        if (should_reset()) goto start_of_loop;
        printf("[State] Performing %s Task...\n", mode);

        int total_steps = 10;
        for (int i = 0; i < total_steps; i++) {
            if (should_reset()) goto start_of_loop;

            digitalWrite(LED_RXTX_STATUS, HIGH);
            
            // Simulate work
            delay(300); 
            printf("   Processing packet %d/%d\n", i+1, total_steps);

            // Blink logic
            if (i < total_steps - 1) {
                digitalWrite(LED_RXTX_STATUS, LOW);
                delay(300);
            } else {
                digitalWrite(LED_RXTX_STATUS, HIGH);
            }
        }

        printf("[State] Task Finished. Press interact please\n");
        delay(1000);

        if (wait_with_blink(-1, BTN_INTERACT, NULL, 0)) goto start_of_loop;

        // --- 5. COMPLETION & EXTRACTION ---
        if (should_reset()) goto start_of_loop;
        printf("[State] Please Remove USB.\n");

        // Create a lambda-like behavior for "USB Disconnected"
        // Since we don't have lambdas, we do the loop manually or invert check logic
        // Let's do a manual loop for clarity here as negation is tricky in helper
        unsigned long last_blink = millis();
        bool led_state = false;
        
        while(check_usb_connected()) {
            if (should_reset()) goto start_of_loop;
            
            // Blink Logic
            if (millis() - last_blink > 500) {
                led_state = !led_state;
                digitalWrite(LED_EXTRACT_USB, led_state);
                last_blink = millis();
            }
            delay(100);
        }

        // Cleanup
        digitalWrite(LED_INSERT_USB, LOW);
        digitalWrite(LED_EXTRACT_USB, LOW);
        digitalWrite(LED_DEVICE_CONFIG, LOW);
        digitalWrite(LED_RXTX_STATUS, LOW);

        printf("[Success] Cycle Complete. Restarting in 3 seconds...\n");

        // 3 Second Wait
        unsigned long start_wait = millis();
        while (millis() - start_wait < 3000) {
            if (should_reset()) goto start_of_loop;
            delay(100);
        }
    }

    return 0;
}

// RUN Bash: gcc -o controller controller.c -lwiringPi
//           sudo ./controller