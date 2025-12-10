# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from gpiozero import LED, Button, DigitalInputDevice
import time
import sys
import threading
import os
from pathlib import Path
from datetime import datetime
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: HELPER FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def RED(message: str)    -> str: return f"\033[31m{message}\033[0m"
def GREEN(message: str)  -> str: return f"\033[32m{message}\033[0m"
def YELLOW(message: str) -> str: return f"\033[33m{message}\033[0m"
def BLUE(message: str)   -> str: return f"\033[34m{message}\033[0m"

def ERROR(message: str) -> None: print(f"{RED('[ERRO]:')} {message}")
def SUCC(message: str)  -> None: print(f"{GREEN('[SUCC]:')} {message}")
def WARN(message: str)  -> None: print(f"{YELLOW('[WARN]:')} {message}")
def INFO(message: str)  -> None: print(f"{BLUE('[INFO]:')} {message}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




# --- CUSTOM EXCEPTION ---
class SoftReset(Exception):
    """
    A custom error we raise intentionally when STOP is pressed.
    """
    pass

# --- PIN CONFIGURATION ---
# LEDs
led_insert_usb    = LED(25)
led_extract_usb   = LED(26)
led_device_config = LED(23)
led_rxtx_status   = LED(16)

# Inputs
btn_interact    = Button(19)
btn_stop        = Button(6)
switch_mode     = DigitalInputDevice(27, pull_up = True)     
switch_scenario = DigitalInputDevice(17, pull_up = True) 

# --- CONSTANTS ---
USB_MOUNT_PATH   = Path("/media")
global_stop_flag = threading.Event()

def check_usb_connected() -> bool:
    """
    Try to find a valid USB device connected to the USB mount path
    """
    
    for path, _, _ in USB_MOUNT_PATH.walk():
        if path.is_mount():
            return path

    return None


def find_valid_txt_file_in_usb(usb_mount_path: Path) -> Path | None:
    """
    Searches for all the txt files in the first level of depth of the USB mount
    location and returns the path to first one ordered alphabetically
    """

    candidates = []
    for file in usb_mount_path.iterdir():
        if not file.is_file():
            continue

        name = file.name
        if name.startswith('.'):
            INFO(f"Skipping hidden file '{name}' on USB")
            continue
        if name.startswith('._'):
            INFO(f"Skipping Apple resource file '{name}' on USB")
            continue
        if file.suffix.lower() != ".txt":
            continue

        candidates.append(file)

    if not candidates:
        WARN(f"No .txt files found on USB mount '{usb_mount_path}'")
        return None

    selected = sorted(candidates)[0].resolve()
    INFO(f"Selected USB source file '{selected}'")
    return selected


def trigger_reset():
    """Runs in background when STOP is pressed."""
    if not global_stop_flag.is_set():
        INFO("\n[Interrupt] STOP Pressed! Resetting to Start...")
        global_stop_flag.set()


def check_stop():
    """Checks flag and raises exception to restart loop."""
    if global_stop_flag.is_set():
        raise SoftReset


def Tx_flow(scenario):
    INFO("we are in TX mode")
    INFO("[State] Waiting for USB or checking USB...")
                
    # Device Config is SOLID here. Insert USB starts BLINKING here.
    led_insert_usb.blink(on_time=0.5, off_time=0.5)
                
    path = None
    while path is None:
        check_stop()
        path = check_usb_connected()
      

    INFO(f"USB connected at '{path.resolve()}'")

    # USB Detected: LED goes Solid
    led_insert_usb.on()

    path_archivo_usb=find_valid_txt_file_in_usb(path)
    if path_archivo_usb is None:
        ERROR("No valid .txt file found on USB. Resetting flow.")
        raise SoftReset

    try:
        file_size = path_archivo_usb.stat().st_size
    except OSError:
        file_size = 0
    INFO(f"Grabbing '{path_archivo_usb}' ({file_size} bytes)")
    content = path_archivo_usb.read_bytes()
    path_file_to_transmit = Path("file_to_transmit.txt").resolve()
    path_file_to_transmit.write_bytes(content)
    SUCC(f"Cached '{path_file_to_transmit}' ({len(content)} bytes) for RF transmission")

    led_insert_usb.off()
    check_stop()
    INFO("[State] Please Remove USB.")
            
    led_extract_usb.blink(on_time=0.5, off_time=0.5)
            
    while check_usb_connected() is not None:
        check_stop()

    
    led_extract_usb.off()

    # --- 3. INTERACT PHASE ---
    check_stop()
    INFO("[State] Ready. Press INTERACT to Execute Task.")
            
    while not btn_interact.is_pressed:
        check_stop()
        time.sleep(0.05)
    check_stop()

    INFO(f"[State] Performing TX Task...")
    led_rxtx_status.blink()

    if scenario == "P2P":

        INFO("[State] Simple mode (P2P). Launching point-to-point flow...")
        cmd = "./bin/robust_mode --verify-config --mode TX --file-path-tx file_to_transmit.txt --pa-level MAX --channel 90 --data-rate 250KBPS"
        INFO(f"Executing: {cmd}")
        result = os.system(cmd)
        exit_code = result >> 8 if result >= 0 else result
        if exit_code == 0:
            SUCC("robust_mode TX completed successfully")
        else:
            ERROR(f"robust_mode TX failed (exit code {exit_code})")

        led_rxtx_status.off() 



def RX_flow(scenario) :
    INFO("We are in RX mode")
    led_rxtx_status.blink()

    if scenario == "P2P":
        INFO("[State] Simple mode (P2P). Recieving P2P...")
        cmd = "./bin/robust_mode --verify-config --mode RX --file-path-rx received_file.txt --pa-level MAX --channel 90 --data-rate 250KBPS"
        INFO(f"Executing: {cmd}")
        result = os.system(cmd)
        exit_code = result >> 8 if result >= 0 else result
        if exit_code == 0:
            SUCC("robust_mode RX completed successfully")
        else:
            ERROR(f"robust_mode RX failed (exit code {exit_code})")

        led_rxtx_status.off() 

        # Device Config is SOLID here. Insert USB starts BLINKING here.
        led_insert_usb.blink(on_time=0.5, off_time=0.5)
                    
        path = None
        while path is None:
            check_stop()
            path = check_usb_connected()

        path = path.resolve()
        INFO(f"USB connected at '{path}'")
        # USB Detected: LED goes Solid
        led_insert_usb.on()

        file_path = Path("received_file.txt").resolve()
        content = file_path.read_bytes()

        (path / "received_file.txt").write_bytes(content)
        SUCC(f"Wrote {(path / 'received_file.txt').resolve()} ({len(content)} bytes)")

        check_stop()
        INFO("[State] Please Remove USB.")
        led_extract_usb.blink(on_time=0.5, off_time=0.5)        
        while check_usb_connected() is not None:
            check_stop()

        INFO("[State] USB Removed.")
        SUCC("RX flow complete; file delivered to USB drive")







def main():
    logger_instance = get_logger()
    if logger_instance.log_path:
        INFO(f"Logging to file '{logger_instance.log_path}'")
    else:
        WARN("File logging disabled; falling back to console-only output")

    btn_stop.when_pressed = trigger_reset

    INFO("--- SYSTEM ONLINE ---")

    while True:
        try:
            # 0. RESET STATE
            global_stop_flag.clear()
            led_insert_usb.off()
            led_extract_usb.off()
            led_rxtx_status.off()
            led_device_config.off()
            
            # --- 1. INITIALIZATION & CONFIG ---
            INFO("\n[State] Device Configuration")
            
            # Start Blinking 
            # This indicates "Waiting for Configuration Confirmation"
            led_device_config.blink(on_time=0.5, off_time=0.5)
            
            INFO("[User] Introduce the configuration and press INTERACT to confirm. Press STOP to reset.")
            # Wait for button press
            while not btn_interact.is_pressed:
                check_stop()
                time.sleep(0.05)
            INFO("[User] INTERACT pressed; locking configuration switches")

            # Read Hardware Switches
            mode = "TX" if switch_mode.is_active else "RX"
            scenario = "Network" if switch_scenario.is_active else "P2P"
            INFO(f"[Info] Current Settings: Mode={mode}, Scenario={scenario}")
            
            # Button Pressed -> LED turns Solid ON
            # Calling .on() automatically stops the background blinking thread
            INFO("[Config] Configuration Accepted.")
            led_device_config.on()
            
            # --- 2. USB INSERTION PHASE ---
            check_stop()
            
            if mode=="TX":
                Tx_flow(scenario)
                led_rxtx_status.off()
                INFO("[State] Task Finished. Press interact please")
                time.sleep(1)
            elif mode=="RX":
                RX_flow(scenario)
                time.sleep(1)
            else:
                ERROR(f"Unsupported mode '{mode}' selected; restarting")
                continue
                
            # --- 4. TX/RX PROCESS PHASE ---
            if scenario == "Network":
                # NETWORK MODE 
                INFO("[State] Network mode selected.")
                WARN("Network mode functionality is not implemented yet; skipping workflow")

            # Cleanup
            led_insert_usb.off() 
            led_extract_usb.off()
            led_device_config.off()
            led_rxtx_status.off()
            
            INFO("[Success] Cycle Complete. Restarting in 3 seconds...")
            

        except SoftReset:
            # Loop restarts immediately
            INFO("!!! IMMEDIATE RESET TRIGGERED !!!")
            continue

        except KeyboardInterrupt:
            INFO("\nProgram Terminated by Keyboard.")
            sys.exit(0)

if __name__ == "__main__":
    main()