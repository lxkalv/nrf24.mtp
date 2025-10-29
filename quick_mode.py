# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from nrf24 import (
    NRF24,

    RF24_DATA_RATE,
    RF24_PA,
    RF24_RX_ADDR,
)

from pathlib import Path
import pigpio
import struct
import time
import sys
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
CE_PIN  = 22

PROCESS_START: float | None = None
PROCESS_STOP: float | None = None
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
    print(f"\033[32m[SUCC]:\033[0m {message}")



def ERROR(message: str) -> None:
    """
    Prints a message to the console with the red prefix `[~ERR]:`
    """
    print(f"\033[31m[~ERR]:\033[0m {message}")




USB_MOUNT_PATH = Path("/media")
def find_usb_txt_file() -> Path:
    """
    Searchs for all the txt files in the USB mount location and returs the path to
    first one
    """

    possible_files: list[str] = []
    usb_mount_point: Path | None = None

    # analyze the subtree of the USB mount point
    for path, dirs, files in USB_MOUNT_PATH.walk():
        if path.is_mount():
            INFO(f"""Found mount path: {path}
        Directories: {", ".join(dirs)}
        Files: {", ".join(files)}""")
            possible_files  = files
            usb_mount_point = path
    

    # filter out invalid files
    possible_files = [
        file
        for file in possible_files
        if not file.startswith(".")
    and file.endswith(".txt")
    ]
    INFO(f"Detected valid files: {", ".join(possible_files)}")


    # TODO: ask the teacher if the USB will only contain one file
    # choose the first file
    file = possible_files[0]
    INFO(f"Selected file: {file}")

    return usb_mount_point / file



def find_usb_mount_point() -> Path:
    usb_mount_point: Path | None = None

    # analyze the subtree of the USB mount point
    for path, _, _ in USB_MOUNT_PATH.walk():
        if path.is_mount():
            INFO(f"Found mount path: {path}")
            usb_mount_point = path
            break
    
    return usb_mount_point
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: ROLE CONFIG  :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
role = ""

option_is_valid = False
while not option_is_valid:
    val = input("\033[33m[>>>>]:\033[0m Please choose a role for this device [T]ransmitter, [R]eceiver, [C]arrier, [Q]uit: ")
    try:
        val = val.upper()

        if val == "T":
            INFO('Device set to TRANSMITTER role')
            role = "T"
            option_is_valid = True
        
        elif val == "R":
            INFO('Device set to RECEIVER role')
            role = "R"
            option_is_valid = True

        elif val == "C":
            INFO('Device set to CONSTANT CARRIER role')
            role = "C"
            option_is_valid = True
        
        elif val == "Q":
            INFO('Quitting program...')
            role = "Q"
            option_is_valid = True
        
        else:
            continue

    except:
        continue
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: RADIO CONFIG :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

# pigpio
hostname = "localhost"
port     = 8888

pi = pigpio.pi(hostname, port)
if not pi.connected:
    ERROR("Not connected to Raspberry Pi, exiting")
    sys.exit(1)

# radio object
nrf = NRF24(pi, ce = CE_PIN)


# radio channel
nrf.set_channel(76)


# data rate
nrf.set_data_rate(RF24_DATA_RATE.RATE_1MBPS)


# Tx/Rx power
nrf.set_pa_level(RF24_PA.MIN)


# CRC
nrf.enable_crc()
nrf.set_crc_bytes(2)


# global payload 
nrf.set_payload_size(32) # [1 - 32] Bytes
payload:list[bytes] = []


# auto-retries
nrf.set_retransmission(1, 15)


# Tx/Rx addresses
nrf.set_address_bytes(4) # [2 - 5] Bytes
possible_addreses = [b"TAN1", b"TAN2"] # Team A Node X 
address = ""


address_is_valid = False
while not address_is_valid:
    val = input("\033[33m[>>>>]:\033[0m Please choose a value for the address [0: TAN1, 1: TAN2]: ")
    try:
        val = int(val)

        if val == 0:
            INFO(f'Address set to {possible_addreses[0]}')
            address_is_valid = True

            if role == "T":
                nrf.open_writing_pipe(possible_addreses[1])
            
            elif role == "R":
                nrf.open_reading_pipe(RF24_RX_ADDR.P1, possible_addreses[0])

        if val == 1:
            INFO(f'Address set to {possible_addreses[1]}')
            address_is_valid = True

            if role == "T":
                nrf.open_writing_pipe(possible_addreses[0])
            
            elif role == "R":
                nrf.open_reading_pipe(RF24_RX_ADDR.P1, possible_addreses[1])
        
        else:
            continue

    except:
        continue


# status visualization
INFO(f"Radio details:")
nrf.show_registers()
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: FLOW FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def BEGIN_TRANSMITTER_MODE() -> None:
    """
    Transmits the first txt file found in the mounted USB
    """

    INFO('Starting transmission')

    try:
        file_path = find_usb_txt_file()
        
        # open the file to read
        with open(file_path, "rb") as file:
            content = file.read()

        content_len = len(content)
        INFO(f'Read {content_len} raw bytes read from {file_path}: {content}')


        # split the contents into chunks
        chunks = [
            content[i:i+nrf.get_payload_size()]
            for i in range(0, content_len, nrf.get_payload_size())
        ]
        chunks_len = len(chunks)


        # store the encoded bytes
        packets = []
        for chunk in chunks:
            packets.append(struct.pack(f"<{nrf.get_payload_size()}s", chunk))


        for idx in range(chunks_len):
            INFO(f"Sending packet: {chunks[idx]} --> {packets[idx]}")

            # reset the packages that we have lost
            nrf.reset_packages_lost()

            
            nrf.send(packets[idx])


            try:
                tic = time.monotonic_ns()
                nrf.wait_until_sent()
                tac = time.monotonic_ns()
            except TimeoutError:
                ERROR("Timeout while transmitting")

            if nrf.get_packages_lost() == 0:
                SUCC(f"Frame sent in {(tac - tic)/1000:.2f} us and {nrf.get_retries()}")

            else:
                ERROR(f"Lost packet after {nrf.get_retries()} retries")

            # time.sleep(1) # wait for one second because why not
    
    finally:
        nrf.power_down()
        pi.stop()
    
    return










def BEGIN_RECEIVER_MODE() -> None:
    """
    Receives multiple frames from a transmitter and reassembles the blocks into a
    txt file
    """

    INFO('Starting reception')

    try:
        # start the timers
        tic     = time.monotonic()
        tac     = time.monotonic()
        timeout = 5
        INFO(f'Timeout set to {timeout} seconds')

        chunks = []

        
        started_timer = False
        while (tac - tic) < timeout:
            tac = time.monotonic()

            # check if there are frames
            while nrf.data_ready():
                if not started_timer:
                    PROCESS_START = time.monotonic()
                    started_timer = True

                payload_pipe = nrf.data_pipe()

                packet = nrf.get_payload()

                chunk: str = struct.unpack(f"<{nrf.get_payload_size()}s", packet)[0] # the struct.unpack method returs more things than just the data
                chunk = chunk.rstrip(b"\x00")
                chunks.append(chunk)
                
                SUCC(f"Received {len(chunk)} bytes on pipe {payload_pipe}: {packet} --> {chunk}")
            
                tic = time.monotonic()
            
        PROCESS_STOP = time.monotonic()

        INFO('Connection timed-out')
        
        
        INFO('Collected:')
        for chunk in chunks:
            print(f"    {chunk}")
        

        content = bytes()
        for chunk in chunks:
            content += chunk
        INFO(f'Merged data: {content}')
        

        if len(content) == 0:
            ERROR('Did not receive anything')
            return
        
        
        usb_mount_point = find_usb_mount_point()
        file_path = usb_mount_point / "received_file.txt"
        
        with open(file_path, "wb") as f:
            f.write(content)
        content_len = len(content)

        INFO(f'Saved {content_len} bytes to: {file_path}')
        INFO(f'Computed throughput: {((content_len*8/1024) / (PROCESS_STOP - PROCESS_START - timeout)):.2f} Kbps')

    finally:
        nrf.power_down()
        pi.stop()

    return










def BEGIN_CONSTANT_CARRIER_MODE() -> None:
    """
    Transmits a constant carrier until the user exits with CTRL+C
    """
    
    ERROR("TODO")
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: MAIN :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def main():
    match role:
        case "T":
            BEGIN_TRANSMITTER_MODE()
            return
        
        case "R":
            BEGIN_RECEIVER_MODE()
            return
        
        case "C":
            BEGIN_CONSTANT_CARRIER_MODE()
            return
        
        case "Q":
            return
        
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




if __name__ == "__main__":
    main()
