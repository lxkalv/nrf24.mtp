// flow2.c
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <wiringPi.h>  // sudo apt install wiringpi (o pigpio si prefieres, habría que adaptar)

#include "libs/logger.h"
#include "libs/app_layer.h"
#include "robust_mode_iface.h"   // run_tx, run_rx, get_spi_device_path, update_radio_params_from_config

// --- PINES EN BCM (igual que en gpiozero) ---
#define LED_INSERT_USB    25   // ojo: comentabas que este “está mal”
#define LED_EXTRACT_USB   26
#define LED_DEVICE_CONFIG 23
#define LED_RXTX_STATUS   16

#define BTN_INTERACT      19
#define BTN_STOP          6
#define SWITCH_MODE       27
#define SWITCH_SCENARIO   17

// --- STOP / SoftReset global ---
static volatile sig_atomic_t g_stop_flag = 0;

// --- Handler de STOP (ISR) ---
static void stop_isr(void)
{
    if (!g_stop_flag) {
        logger_info("\n[Interrupt] STOP Pressed! Resetting to Start...");
        g_stop_flag = 1;
    }
}

static int check_stop(void)
{
    return g_stop_flag != 0;
}

// --- helper para dormir en ms ---
static void sleep_ms(unsigned int ms)
{
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

// --- detectar si hay algo montado en /media (simple) ---
static int check_usb_connected(void)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) {
        logger_warn("Could not open /proc/mounts: %s", strerror(errno));
        return 0;
    }

    char dev[256], mount[256], rest[256];
    int found = 0;
    while (fscanf(f, "%255s %255s %255s\n", dev, mount, rest) == 3) {
        if (strncmp(mount, "/media", 6) == 0) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

// --- LEDs a OFF ---
static void reset_leds(void)
{
    digitalWrite(LED_INSERT_USB,    LOW);
    digitalWrite(LED_EXTRACT_USB,   LOW);
    digitalWrite(LED_DEVICE_CONFIG, LOW);
    digitalWrite(LED_RXTX_STATUS,   LOW);
}

int main(int argc, char **argv)
{
    // --- CONFIG DE APP (igual que robust_mode) ---
    app_config_t cfg;
    if (app_parse_arguments(argc, argv, &cfg) != 0) {
        app_print_usage(argv[0]);
        return 1;
    }

    if (logger_init("flow2.log") != 0) {
        logger_warn("Could not open log file 'flow2.log' (continuing without file log)");
    } else {
        logger_info("Logging to file 'flow2.log'");
    }

    if (cfg.print_config) {
        app_print_config(&cfg);
    }

    const char *spi_dev = get_spi_device_path();
    update_radio_params_from_config(&cfg);

    logger_info("Using SPI device: %s", spi_dev);

    // --- INIT wiringPi en modo BCM ---
    if (wiringPiSetupGpio() < 0) {
        logger_error("wiringPiSetupGpio failed");
        logger_close();
        return 1;
    }

    pinMode(LED_INSERT_USB,    OUTPUT);
    pinMode(LED_EXTRACT_USB,   OUTPUT);
    pinMode(LED_DEVICE_CONFIG, OUTPUT);
    pinMode(LED_RXTX_STATUS,   OUTPUT);

    pinMode(BTN_INTERACT, INPUT);
    pinMode(BTN_STOP,     INPUT);
    pinMode(SWITCH_MODE,  INPUT);
    pinMode(SWITCH_SCENARIO, INPUT);

    // pull-ups (equivalente a pull_up=True de gpiozero)
    pullUpDnControl(BTN_INTERACT, PUD_UP);
    pullUpDnControl(BTN_STOP,     PUD_UP);
    pullUpDnControl(SWITCH_MODE,  PUD_UP);
    pullUpDnControl(SWITCH_SCENARIO, PUD_UP);

    // ISR en STOP
    if (wiringPiISR(BTN_STOP, INT_EDGE_FALLING, &stop_isr) < 0) {
        logger_error("wiringPiISR failed: %s", strerror(errno));
        logger_close();
        return 1;
    }

    logger_info("--- SYSTEM ONLINE ---");

    for (;;) {
        // etiqueta para SoftReset
    soft_reset:
        g_stop_flag = 0;
        reset_leds();

        // --- 1. INITIALIZATION & CONFIG ---
        logger_info("\n[State] Device Configuration");
        logger_info("[User] Introduce the configuration and press INTERACT to confirm. Press STOP to reset.");

        // parpadeo manual de LED_DEVICE_CONFIG
        unsigned long last_blink_ms = millis();
        int led_cfg_state = 0;

        // Espera a que se pulse INTERACT
        while (digitalRead(BTN_INTERACT) == HIGH) { // HIGH = sin pulsar (pull-up)
            if (check_stop()) {
                logger_info("!!! IMMEDIATE RESET TRIGGERED !!!");
                goto soft_reset;
            }

            unsigned long now = millis();
            if (now - last_blink_ms >= 500) {
                led_cfg_state = !led_cfg_state;
                digitalWrite(LED_DEVICE_CONFIG, led_cfg_state ? HIGH : LOW);
                last_blink_ms = now;
            }
            delay(50);
        }

        // Leemos los switches físicos (mismo sentido que gpiozero)
        int mode_is_tx       = (digitalRead(SWITCH_MODE)     == HIGH);  // HIGH => TX
        int scenario_network = (digitalRead(SWITCH_SCENARIO) == HIGH);  // HIGH => Network

        const char *mode_str     = mode_is_tx ? "TX" : "RX";
        const char *scenario_str = scenario_network ? "Network" : "P2P";

        logger_info("[Info] Current Settings: Mode=%s, Scenario=%s", mode_str, scenario_str);
        logger_info("[Config] Configuration Accepted.");

        // LED config sólido ON
        digitalWrite(LED_DEVICE_CONFIG, HIGH);

        // --- 2. USB INSERTION PHASE ---
        if (check_stop()) {
            logger_info("!!! IMMEDIATE RESET TRIGGERED !!!");
            goto soft_reset;
        }

        logger_info("[State] Waiting for USB or checking USB...");

        // parpadeo de LED_INSERT_USB mientras esperamos
        unsigned long last_blink_usb = millis();
        int led_usb_state = 0;
        while (!check_usb_connected()) {
            if (check_stop()) {
                logger_info("!!! IMMEDIATE RESET TRIGGERED !!!");
                goto soft_reset;
            }

            unsigned long now = millis();
            if (now - last_blink_usb >= 500) {
                led_usb_state = !led_usb_state;
                digitalWrite(LED_INSERT_USB, led_usb_state ? HIGH : LOW);
                last_blink_usb = now;
            }
            delay(100);
        }

        logger_info("USB connected");
        sleep_ms(1000);
        digitalWrite(LED_INSERT_USB, HIGH); // sólido

        // aseguramos config ON
        digitalWrite(LED_DEVICE_CONFIG, HIGH);

        // --- 3. INTERACT PHASE ---
        if (check_stop()) {
            logger_info("!!! IMMEDIATE RESET TRIGGERED !!!");
            goto soft_reset;
        }

        logger_info("[State] Ready. Press INTERACT to Execute Task.");

        while (digitalRead(BTN_INTERACT) == HIGH) {
            if (check_stop()) {
                logger_info("!!! IMMEDIATE RESET TRIGGERED !!!");
                goto soft_reset;
            }
            delay(50);
        }

        // --- 4. TX/RX PROCESS PHASE ---
        if (check_stop()) {
            logger_info("!!! IMMEDIATE RESET TRIGGERED !!!");
            goto soft_reset;
        }

        logger_info("[State] Performing %s Task...", mode_str);
        digitalWrite(LED_RXTX_STATUS, HIGH);

        if (!scenario_network) {
            // P2P
            logger_info("[State] Simple mode (P2P). Launching robust_mode flow...");

            if (mode_is_tx) {
                logger_info("[P2P] TX -> run_tx()");

                uint8_t *data = NULL;
                size_t   len  = 0;
                if (app_load_file_bytes(cfg.file_path_tx, &data, &len) != 0) {
                    logger_error("flow2 TX: failed to load input bytes from '%s'", cfg.file_path_tx);
                } else {
                    int ret = run_tx(spi_dev, &cfg, data, len);
                    if (ret != 0) {
                        logger_error("flow2 TX: run_tx() failed with code %d", ret);
                    }
                    free(data);
                }
            } else {
                logger_info("[P2P] RX -> run_rx()");
                int ret = run_rx(spi_dev, &cfg);
                if (ret != 0) {
                    logger_error("flow2 RX: run_rx() failed with code %d", ret);
                }
            }
        } else {
            // Network mode (a rellenar más adelante)
            logger_info("[State] Network mode selected. (TODO: implement network logic here)");
        }

        digitalWrite(LED_RXTX_STATUS, LOW);
        logger_info("[State] Task Finished. Press INTERACT please");
        sleep_ms(1000);

        while (digitalRead(BTN_INTERACT) == HIGH) {
            if (check_stop()) {
                logger_info("!!! IMMEDIATE RESET TRIGGERED !!!");
                goto soft_reset;
            }
            delay(50);
        }

        // --- 5. COMPLETION & EXTRACTION ---
        if (check_stop()) {
            logger_info("!!! IMMEDIATE RESET TRIGGERED !!!");
            goto soft_reset;
        }

        logger_info("[State] Please Remove USB.");

        unsigned long last_blink_extract = millis();
        int led_extract_state = 0;

        while (check_usb_connected()) {
            if (check_stop()) {
                logger_info("!!! IMMEDIATE RESET TRIGGERED !!!");
                goto soft_reset;
            }

            unsigned long now = millis();
            if (now - last_blink_extract >= 500) {
                led_extract_state = !led_extract_state;
                digitalWrite(LED_EXTRACT_USB, led_extract_state ? HIGH : LOW);
                last_blink_extract = now;
            }
            delay(100);
        }

        // Cleanup
        reset_leds();

        logger_info("[Success] Cycle Complete. Restarting in 3 seconds...");
        double start_wait = (double)millis() / 1000.0;
        while (((double)millis() / 1000.0) - start_wait < 3.0) {
            if (check_stop()) {
                logger_info("!!! IMMEDIATE RESET TRIGGERED !!!");
                goto soft_reset;
            }
            delay(100);
        }
    }

    logger_close();
    return 0;
}
