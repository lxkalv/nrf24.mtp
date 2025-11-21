# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from nrf24 import (
    NRF24,

    RF24_DATA_RATE,
    RF24_PA,
    RF24_RX_ADDR,
)

import RPi.GPIO as GPIO
import os
import pigpio
import struct
import time
import sys
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: HELPER FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def INFO(message: str) -> None:
    """
    Prints a message to the console with the blue prefix `[INFO]:`
    """
    print(f"\033[34m[INFO]:\033[0m {message}")



def SUCC(message: str) -> None:
    """
    Prints a message to the console with the green prefix `[SUCC]:`
    """
    print(f"\033[33m[INFO]:\033[0m {message}")


def ERROR(message: str) -> None:
    """
    Prints a message to the console with the red prefix `[~ERR]:`
    """
    print(f"\033[31m[~ERR]:\033[0m {message}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# --- PIN CONFIGURATION ---
# Buttons
PIN_BTN_INTERACT = 6
PIN_BTN_STOP = 19

# New Switches (Toggle Switches recommended)
PIN_SWITCH_MODE = 17      # Determines Tx vs Rx
PIN_SWITCH_SCENARIO = 27  # Determines Network vs Point-to-Point

# LEDs
PIN_LED_INSERT_USB = 16
PIN_LED_EXTRACT_USB = 25
PIN_LED_DEVICE_CONFIG = 23
PIN_LED_RXTX_STATUS = 26

# --- CONSTANTS ---
# Define what the switch states mean
MODE_TX = "Transmission"
MODE_RX = "Reception"
SCENARIO_NET = "Network"
SCENARIO_P2P = "Point-to-Point"

def setup_gpio():
    """Activates the GPIO pins and sets up Input/Output directions."""
    GPIO.setmode(GPIO.BCM)
    GPIO.setwarnings(False)

    # Outputs (LEDs) - Start them all OFF
    GPIO.setup(PIN_LED_INSERT_USB, GPIO.OUT, initial=GPIO.LOW)
    GPIO.setup(PIN_LED_EXTRACT_USB, GPIO.OUT, initial=GPIO.LOW)
    GPIO.setup(PIN_LED_DEVICE_CONFIG, GPIO.OUT, initial=GPIO.LOW)
    GPIO.setup(PIN_LED_RXTX_STATUS, GPIO.OUT, initial=GPIO.LOW)

    # Inputs (Buttons & Switches)
    # Using Pull-Up: Default is HIGH (1). Connecting to GND makes it LOW (0).
    GPIO.setup(PIN_BTN_INTERACT, GPIO.IN, pull_up_down=GPIO.PUD_UP)
    GPIO.setup(PIN_BTN_STOP, GPIO.IN, pull_up_down=GPIO.PUD_UP)
    GPIO.setup(PIN_SWITCH_MODE, GPIO.IN, pull_up_down=GPIO.PUD_UP)
    GPIO.setup(PIN_SWITCH_SCENARIO, GPIO.IN, pull_up_down=GPIO.PUD_UP)

def check_usb_connected():
    """
    Checks if USB is physically inserted.
    Replace the return value with `os.path.exists('/media/pi/YOUR_USB_ID')` for real usage.
    """
    # For simulation: We assume USB is connected unless we want to test the loop.
    # You can toggle this logic or check a specific file path.
    return True 

def read_configuration():
    """Reads the physical switches to determine Mode and Scenario."""
    # If Switch is Open (HIGH) -> Mode A. If Closed to GND (LOW) -> Mode B.
    # Logic: Let's assume Open = Tx/Network, Closed = Rx/P2P.
    
    if GPIO.input(PIN_SWITCH_MODE) == GPIO.HIGH:
        mode = MODE_TX
    else:
        mode = MODE_RX

    if GPIO.input(PIN_SWITCH_SCENARIO) == GPIO.HIGH:
        scenario = SCENARIO_NET
    else:
        scenario = SCENARIO_P2P
        
    return mode, scenario

def perform_tx_rx_task(mode, scenario):
    """
    Simulates the transmission or reception process.
    Returns True if finished successfully, False if stopped.
    """
    print(f"[System] Performing {mode} in {scenario} scenario.")
    #::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

    # Configuración de los pines (usando numeración BCM)
    GPIO.setmode(GPIO.BCM)
    pin_pwm = 16
    GPIO.setup(pin_pwm, GPIO.OUT)
    # Iniciar PWM en el pin especificado con 50 Hz de frecuencia
    frecuencia_hz = 50
    pwm = GPIO.PWM(pin_pwm, frecuencia_hz)
    pwm.start(0)  # Iniciar con ciclo de trabajo del 0%
    try:
        while True:
        # Aumentar el ciclo de trabajo del 0% al 100%
            for dc in range(0, 101, 5):
                pwm.ChangeDutyCycle(dc)
                time.sleep(0.1)
        # Disminuir el ciclo de trabajo del 100% al 0%
            for dc in range(100, -1, -5):
                pwm.ChangeDutyCycle(dc)
                time.sleep(0.1)
    except KeyboardInterrupt:
        pass
        # Check STOP button
        if GPIO.input(PIN_BTN_STOP) == GPIO.LOW:
            print("[Urgent] STOP pressed during operation.")
            return False
# Limpiar los pines al finalizar
pwm.stop()
GPIO.cleanup()



def main():
    try:
        print("--- START ---")
        setup_gpio()

        # --- CHECK PORTS & CONFIG ---
        print("[Check] Verifying ports...")
        # Simple safety check: Ensure Stop button isn't stuck pressed at boot
        if GPIO.input(PIN_BTN_STOP) == GPIO.LOW:
            print("[Error] Stop button is pressed during startup.")
            return

        # "Device Config" led turns on
        GPIO.output(PIN_LED_DEVICE_CONFIG, GPIO.HIGH)
        time.sleep(1)

        # Select between tx/rx mode & Select scenario
        current_mode, current_scenario = read_configuration()
        print(f"[Config] Selected Mode: {current_mode}")
        print(f"[Config] Selected Scenario: {current_scenario}")

        # Decision: "Is it correctly set?"
        # In a headless Pi, we assume physical switches *are* the source of truth.
        # If you wanted a manual confirmation, we could wait for a button press here.
        # For now, we check if the user presses STOP immediately to signal "Incorrect".
        print("[User] Verify config. Press STOP within 3 seconds if incorrect.")
        start_wait = time.time()
        config_ok = True
        while time.time() - start_wait < 3:
            if GPIO.input(PIN_BTN_STOP) == GPIO.LOW:
                config_ok = False
                break
            time.sleep(0.1)

        if not config_ok:
            # Path: No -> "Device Config" led turns off -> Review Design -> Stop
            print("[Error] Configuration rejected by user.")
            GPIO.output(PIN_LED_DEVICE_CONFIG, GPIO.LOW)
            print("Review the design.")
            return # Stop

        # Path: Yes -> Continue
        print("[Config] Configuration Accepted.")

        # --- USB INSERTION PHASE ---
        print("[USB] Waiting for USB insertion...")
        
        # Loop: Add command to turn on "Insert USB" -> Led ON -> Inserted? -> No -> Led OFF -> Loop
        usb_detected = False
        while not usb_detected:
            # "Insert USB" led turns on
            GPIO.output(PIN_LED_INSERT_USB, GPIO.HIGH)
            time.sleep(0.5) # Visual blink duration

            if check_usb_connected():
                # Yes -> "Insert USB" led turns off -> Proceed
                usb_detected = True
                GPIO.output(PIN_LED_INSERT_USB, GPIO.LOW)
                print("[USB] USB Detected.")
            else:
                # No -> "Insert USB" led turns off (Blink logic)
                GPIO.output(PIN_LED_INSERT_USB, GPIO.LOW)
                time.sleep(0.5)

        # Flowchart: "Device Config" led turns on (It was already on, but we ensure it stays on)
        GPIO.output(PIN_LED_DEVICE_CONFIG, GPIO.HIGH)

        # --- INTERACT PHASE ---
        print("[System] Ready. Press INTERACT to start.")
        
        # Wait for Interact
        while True:
            if GPIO.input(PIN_BTN_STOP) == GPIO.LOW:
                print("STOP pressed.")
                GPIO.cleanup()
                return
            if GPIO.input(PIN_BTN_INTERACT) == GPIO.LOW:
                time.sleep(0.2) # Debounce
                break
            time.sleep(0.1)

        # --- TX/RX PHASE ---
        # "Rx/Tx Status" led turns on (Initial indicator)
        GPIO.output(PIN_LED_RXTX_STATUS, GPIO.HIGH)

        # Perform Task (Includes the Blinking Loop from flowchart)
        success = perform_tx_rx_task(current_mode, current_scenario)

        if success:
            # Finished? Yes -> "Rx/Tx Status" led turns on (Solid)
            GPIO.output(PIN_LED_RXTX_STATUS, GPIO.HIGH)
            print("[System] Task Finished Successfully.")
        else:
            GPIO.output(PIN_LED_RXTX_STATUS, GPIO.LOW)
            print("[System] Task Stopped/Failed.")
            return

        # --- COMPLETION / EXTRACT PHASE ---
        print("[File] Verifying file storage...")
        time.sleep(1) # Simulate verification
        
        # Add command to turn on "Extract USB" led
        print("[USB] Please Remove USB.")

        # Loop: "Extract USB" led turns on -> Removed? -> No -> Led OFF -> Loop
        while True:
            # "Extract USB" led turns on
            GPIO.output(PIN_LED_EXTRACT_USB, GPIO.HIGH)
            time.sleep(0.5)

            if not check_usb_connected():
                # Yes (Removed) -> All leds turn off
                print("[USB] USB Removed.")
                break
            else:
                # No -> "Extract USB" led turns off
                GPIO.output(PIN_LED_EXTRACT_USB, GPIO.LOW)
                time.sleep(0.5)

        # --- END ---
        # All leds turn off
        GPIO.output(PIN_LED_INSERT_USB, GPIO.LOW)
        GPIO.output(PIN_LED_EXTRACT_USB, GPIO.LOW)
        GPIO.output(PIN_LED_DEVICE_CONFIG, GPIO.LOW)
        GPIO.output(PIN_LED_RXTX_STATUS, GPIO.LOW)

        print("[Done] Read the file received.")

    except KeyboardInterrupt:
        print("\nProgram Interrupted.")
    finally:
        GPIO.cleanup()

if __name__ == "__main__":
    main()