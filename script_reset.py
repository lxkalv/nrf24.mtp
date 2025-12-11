#!/usr/bin/env python3
import os
import time

import pigpio  # needs pigpiod installed & available

# --- EDIT THESE IF NEEDED ---

# Hardware SPI0 pins on Raspberry Pi
SPI_PINS = [
    7,   # CE1 (if used)
    8,   # CE0 / CSN for nRF24
    9,   # MISO
    10,  # MOSI
    11,  # SCLK
]

# nRF24 control pins (adapt to your wiring)
NRF24_PINS = [
    22,  # CE
    # 25,  # IRQ (uncomment if you use it)
]

ALL_PINS = sorted(set(SPI_PINS + NRF24_PINS))


def restart_pigpiod():
    """
    Restart the pigpio daemon so we start from a clean state.
    Requires sudo privileges.
    """
    print("[INFO] Restarting pigpiod...")
    # Kill any existing pigpiod
    os.system("sudo killall pigpiod 2>/dev/null")
    time.sleep(0.2)
    # Start a fresh daemon
    os.system("sudo pigpiod")
    time.sleep(0.2)
    print("[INFO] pigpiod restarted.")


def reset_gpio_pins():
    """
    Put all relevant GPIOs into a neutral state:
    - Mode: INPUT
    - Pull-ups/downs: OFF
    """
    print("[INFO] Connecting to pigpio...")
    pi = pigpio.pi()
    if not pi.connected:
        print("[ERRO] Could not connect to pigpiod. Is it running?")
        return

    print(f"[INFO] Resetting pins: {ALL_PINS}")
    for gpio in ALL_PINS:
        # Set as input
        pi.set_mode(gpio, pigpio.INPUT)
        # Disable pull-up/down
        pi.set_pull_up_down(gpio, pigpio.PUD_OFF)

    pi.stop()
    print("[SUCC] GPIO pins reset to INPUT with no pulls.")


def main():
    restart_pigpiod()
    reset_gpio_pins()
    print("[SUCC] SPI + nRF24 pins and pigpiod are now in a clean state.")


if __name__ == "__main__":
    main()
