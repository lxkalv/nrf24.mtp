from gpiozero import LED, Button, DigitalInputDevice
import time
import sys
import threading
import os

# --- CUSTOM EXCEPTION ---
class SoftReset(Exception):
    """
    A custom error we raise intentionally when STOP is pressed.
    """
    pass

# --- PIN CONFIGURATION ---
# LEDs
led_insert_usb = LED(16)
led_extract_usb = LED(25)
led_device_config = LED(23)
led_rxtx_status = LED(26)

# Inputs
btn_interact = Button(6)
btn_stop = Button(19)
switch_mode = DigitalInputDevice(17, pull_up=True)     
switch_scenario = DigitalInputDevice(27, pull_up=True) 

# --- CONSTANTS ---
USB_MOUNT_DIR = "/media/pi"
global_stop_flag = threading.Event()

def check_usb_connected():
    """Checks if USB is mounted."""
    if os.path.exists(USB_MOUNT_DIR):
        return any(os.scandir(USB_MOUNT_DIR))
    return False

def trigger_reset():
    """Runs in background when STOP is pressed."""
    if not global_stop_flag.is_set():
        print("\n[Interrupt] STOP Pressed! Resetting to Start...")
        global_stop_flag.set()

def check_stop():
    """Checks flag and raises exception to restart loop."""
    if global_stop_flag.is_set():
        raise SoftReset

def main():
    btn_stop.when_pressed = trigger_reset

    print("--- SYSTEM ONLINE ---")

    while True:
        try:
            # 0. RESET STATE
            global_stop_flag.clear()
            led_insert_usb.off()
            led_extract_usb.off()
            led_rxtx_status.off()
            led_device_config.off()
            
            # --- 1. INITIALIZATION & CONFIG ---
            print("\n[State] Device Configuration")
            
            # Start Blinking instead of Solid On <<<
            # This indicates "Waiting for Configuration Confirmation"
            led_device_config.blink(on_time=0.5, off_time=0.5)
            
            # Read Hardware Switches
            mode = "TX" if switch_mode.is_active else "RX"
            scenario = "Network" if switch_scenario.is_active else "P2P"
            print(f"[Info] Current Settings: Mode={mode}, Scenario={scenario}")
            
            print("[User] Press INTERACT to confirm. Press STOP to reset.")
            
            # Wait for button press
            while not btn_interact.is_pressed:
                check_stop()
                time.sleep(0.05)
            
            # Button Pressed -> LED turns Solid ON <<<
            # Calling .on() automatically stops the background blinking thread
            print("[Config] Configuration Accepted.")
            led_device_config.on()
            
            # --- 2. USB INSERTION PHASE ---
            check_stop()
            if not check_usb_connected():
                print("[State] Waiting for USB...")
                
                # Device Config is SOLID here. Insert USB starts BLINKING here.
                led_insert_usb.blink(on_time=0.5, off_time=0.5)
                
                while not check_usb_connected():
                    check_stop()
                    time.sleep(0.1)
                
                # USB Detected: LED goes Solid
                led_insert_usb.on()
            else:
                 # If USB was already there, ensure led is ON
                 led_insert_usb.on()

            # Ensure Device Config is still ON (Redundant but safe)
            led_device_config.on() 

            # --- 3. INTERACT PHASE ---
            check_stop()
            print("[State] Ready. Press INTERACT to Execute Task.")
            
            while not btn_interact.is_pressed:
                check_stop()
                time.sleep(0.05)
            
            # --- 4. TX/RX PROCESS PHASE ---
            check_stop()
            print(f"[State] Performing {mode} Task...")
            
            total_steps = 10
            for i in range(total_steps):
                check_stop()
                
                # LED ON
                led_rxtx_status.on()
                
                # Simulate work
                time.sleep(0.3)
                print(f"   Processing packet {i+1}/{total_steps}")
                
                # Blink logic
                if i < total_steps - 1:
                    led_rxtx_status.off()
                    time.sleep(0.3)
                else:
                    led_rxtx_status.on()

            print("[State] Task Finished.")
            time.sleep(1)

            # --- 5. COMPLETION & EXTRACTION ---
            check_stop()
            print("[State] Please Remove USB.")
            
            led_extract_usb.blink(on_time=0.5, off_time=0.5)
            
            while check_usb_connected():
                check_stop()
                time.sleep(0.1)
            
            # Cleanup
            led_insert_usb.off() 
            led_extract_usb.off()
            led_device_config.off()
            led_rxtx_status.off()
            
            print("[Success] Cycle Complete. Restarting in 3 seconds...")
            
            start_wait = time.time()
            while time.time() - start_wait < 3:
                check_stop()
                time.sleep(0.1)

        except SoftReset:
            # Loop restarts immediately
            print("!!! IMMEDIATE RESET TRIGGERED !!!")
            continue

        except KeyboardInterrupt:
            print("\nProgram Terminated by Keyboard.")
            sys.exit(0)

if __name__ == "__main__":
    main()