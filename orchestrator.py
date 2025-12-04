from gpiozero import LED, Button, DigitalInputDevice
import time
import sys
import threading
import os
from pathlib import Path

# :::: COLORING FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def RED(message: str) -> str:
    """
    Returns a copy of the string wrapped in ANSI scape sequences to make it red
    """
    return f"\033[31m{message}\033[0m"

def GREEN(message: str) -> str:
    """
    Returns a copy of the string wrapped in ANSI scape sequences to make it green
    """
    return f"\033[32m{message}\033[0m"

def YELLOW(message: str) -> str:
    """
    Returns a copy of the string wrapped in ANSI scape sequences to make it yellow
    """
    return f"\033[33m{message}\033[0m"

def BLUE(message: str) -> str:
    """
    Returns a copy of the string wrapped in ANSI scape sequences to make it blue
    """
    return f"\033[34m{message}\033[0m"
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: MESSAGING FUNCTIONS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def ERROR(message: str, end = "\n") -> None:
    """
    Prints a message to the console with the red prefix `[~ERR]:`
    """
    print(f"{RED('[~ERR]:')} {message}", end = end)

def SUCC(message: str, end = "\n") -> None:
    """
    Prints a message to the console with the green prefix `[SUCC]:`
    """
    print(f"{GREEN('[SUCC]:')} {message}", end = end)

def WARN(message: str, end = "\n") -> None:
    """
    Prints a message to the console with the yellow prefix `[WARN]:`
    """
    print(f"{YELLOW('[WARN]:')} {message}", end = end)

def INFO(message: str, end = "\n") -> None:
    """
    Prints a message to the console with the blue prefix `[INFO]:`
    """
    print(f"{BLUE('[INFO]:')} {message}", end = end)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

# --- CUSTOM EXCEPTION ---
class SoftReset(Exception):
    """
    A custom error we raise intentionally when STOP is pressed.
    """
    pass

# --- PIN CONFIGURATION ---
# LEDs
led_insert_usb = LED(25) #este esta mal
led_extract_usb = LED(26)
led_device_config = LED(23) #este bien 
led_rxtx_status = LED(16)

# Inputs
btn_interact = Button(19)
btn_stop = Button(6)
switch_mode = DigitalInputDevice(27, pull_up=True)     
switch_scenario = DigitalInputDevice(17, pull_up=True) 

# --- CONSTANTS ---
USB_MOUNT_PATH = Path("/media")
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

    file = [
        file
        for file in usb_mount_path.iterdir()
        if file.is_file()
        and file.suffix == ".txt"
        and not str(file).startswith(".")
    ]

    file = sorted(file)

    if not file:
        return None

    return file[0].resolve()

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
        time.sleep(0.1)

    INFO("USB connected")
    time.sleep(1)
    # USB Detected: LED goes Solid
    led_insert_usb.on()

    path_archivo_usb=find_valid_txt_file_in_usb(path)
    content = path_archivo_usb.read_bytes()
    path_file_to_transmit = Path("file_to_transmit.txt").resolve()
    path_file_to_transmit.write_bytes(content)

    led_insert_usb.off()
    check_stop()
    INFO("[State] Please Remove USB.")
            
    led_extract_usb.blink(on_time=0.5, off_time=0.5)
            
    while check_usb_connected() is not None:
        check_stop()
        time.sleep(0.1)
    
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
        ok = os.system("./bin/robust --help")
        INFO(f"Finished process with exit code {ok}")
        




    






def main():
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
            return
                
            # Ensure Device Config is still ON (Redundant but safe)
            

           
            
            # --- 4. TX/RX PROCESS PHASE ---
           
            if scenario == "Network":
                # NETWORK MODE 
                INFO("[State] Network mode selected.")
                pass

            
            led_rxtx_status.off()

            INFO("[State] Task Finished. Press interact please")
            time.sleep(1)

            while not btn_interact.is_pressed:
                check_stop()
                time.sleep(0.05)

            # --- 5. COMPLETION & EXTRACTION ---

            
            # Cleanup
            led_insert_usb.off() 
            led_extract_usb.off()
            led_device_config.off()
            led_rxtx_status.off()
            
            INFO("[Success] Cycle Complete. Restarting in 3 seconds...")
            
            start_wait = time.time()
            while time.time() - start_wait < 3:
                check_stop()
                time.sleep(0.1)

        except SoftReset:
            # Loop restarts immediately
            INFO("!!! IMMEDIATE RESET TRIGGERED !!!")
            continue

        except KeyboardInterrupt:
            INFO("\nProgram Terminated by Keyboard.")
            sys.exit(0)

if __name__ == "__main__":
    main()