# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from nrf24 import (
    NRF24,

    RF24_DATA_RATE,
    RF24_PA,
    RF24_RX_ADDR,
    RF24_PAYLOAD,
    RF24_CRC,
)

from pathlib import Path
import pigpio
import struct
import shutil
import time
import sys
import os

os.system("cls" if os.name == "nt" else "clear")

from enum import Enum
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
CE_PIN = 22

RECEIVER_TIMEOUT_S = 20

USB_MOUNT_PATH = Path("/media")

spinner     = "⣾⣽⣻⢿⡿⣟⣯⣷"
IDX_SPINNER = [0]

DATA_SIZE = 32
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: HELPER FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
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



def reset_line() -> None:
    """
    Delete the last CMD line
    """

    print("\x1b[2K\r", end = "")
    
    return



def progress_bar(active_msg: str, finished_msg: str, current_status: int, max_status: int) -> None:
    """
    Create a progress bar that gets updated everytime this function is called
    """
    
    terminal_width  = shutil.get_terminal_size().columns
    IDX_SPINNER[0] += 1
    spin            = spinner[IDX_SPINNER[0] % len(spinner)]
    progress        = f"({current_status}/{max_status}) {spin}"

    if current_status < max_status:
        progress = f"({current_status}/{max_status}) {spin}"

        reset_line()
        INFO(f"{active_msg} {progress.rjust(terminal_width - 8 - len(active_msg) - 2)}", end = "")
        sys.stdout.flush()
        
    
    else:
        progress = f"({current_status}/{max_status}) █"

        reset_line()
        SUCC(f"{finished_msg} {progress.rjust(terminal_width - 8 - len(finished_msg) - 2)}")
    
    return



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

    if usb_mount_point is None:
        return Path("lorem.txt")
    

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



def find_usb_mount_point() -> Path | None:
    usb_mount_point: Path | None = None

    # analyze the subtree of the USB mount point
    for path, _, _ in USB_MOUNT_PATH.walk():
        if path.is_mount():
            INFO(f"Found mount path: {path}")
            usb_mount_point = path
            break
    
    return usb_mount_point
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: NODE CONFIG  :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
class Role(Enum):
    TRANSMITTER = "TRANSMITTER"
    RECEIVER    = "RECEIVER"
    CARRIER     = "CARRIER"
    QUIT        = "QUIT"

    def __str__(self: "Role") -> str:
        return self.value



def choose_node_role() -> Role:
    """
    Function to choose the role of the current node
    """

    while True:
        val = input(f"{YELLOW('[>>>>]:')} Please choose a role for this device [T]ransmitter, [R]eceiver, [C]arrier, [Q]uit: ")
        
        try:
            val = val.upper()
        except:
            continue

        if val == "T":
            INFO(f"Device set to {Role.TRANSMITTER} role")
            return Role.TRANSMITTER
            
        elif val == "R":
            INFO(f"Device set to {Role.RECEIVER} role")
            return Role.RECEIVER

        elif val == "C":
            INFO(f"Device set to {Role.CARRIER} role")
            return Role.CARRIER
        
        elif val == "Q":
            INFO("Quitting program...")
            return Role.QUIT
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
nrf = NRF24(pi = pi, ce = CE_PIN, spi_speed = 10_000_000)


# radio channel
nrf.set_channel(76)


# data rate
nrf.set_data_rate(RF24_DATA_RATE.RATE_2MBPS)


# Tx/Rx power
nrf.set_pa_level(RF24_PA.MIN)


# CRC
nrf.enable_crc()
nrf.set_crc_bytes(RF24_CRC.BYTES_2)


# global payload 
nrf.set_payload_size(RF24_PAYLOAD.DYNAMIC) # [1 - 32] Bytes
PAYLOAD:list[bytes] = []


# auto-retries
nrf.set_retransmission(1, 15)


# Tx/Rx addresses
nrf.set_address_bytes(3) # [2 - 5] Bytes


# status visualization
INFO(f"Radio details:")
nrf.show_registers()



def choose_address_based_on_role(role: Role, nrf: NRF24) -> None:
    """
    Choose the address of the current node based on the role that it has been
    assigned
    """
    
    if role is Role.TRANSMITTER:
        nrf.open_writing_pipe(b"TA1")
        nrf.open_reading_pipe(RF24_RX_ADDR.P1, b"TA0")
        INFO("Writing @: TA1 | Reading @; TA0")
    
    elif role is Role.RECEIVER:
        nrf.open_writing_pipe(b"TA0")
        nrf.open_reading_pipe(RF24_RX_ADDR.P1, b"TA1")
        INFO("Writing @: TA0 | Reading @; TA1")

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: FLOW FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def BEGIN_TRANSMITTER_MODE() -> None:
    """
    Transmits the first txt file found in the mounted USB, the flow of the TX MODE
    is the following:
    
    1. An appropiate file is selected from all the candidate files found in the
    mounted USB. The content of the file is extracted as raw bytes

    2. The bytes are splitted into chunks of size `payload_size` and then packed
    for future transmission

    3. An information message is sent containing the number of frames that the
    receiver should expect

    4. The rest of the frames are sent in a stop & wait fashion
    """

    INFO("Starting transmission")

    try:
        file_path = find_usb_txt_file()
        
        # open the file to read
        with open(file_path, "rb") as file:
            content = file.read()

        content_len = len(content)
        INFO(f"Read {content_len} raw bytes read from {file_path}")


        # split the contents into chunks
        chunks = [
            content[i:i+DATA_SIZE]
            for i in range(0, content_len, DATA_SIZE)
        ]
        chunks_len = len(chunks)


        # store the encoded bytes
        packets = []
        for chunk in chunks:
            packets.append(struct.pack(f"<{len(chunk)}s", chunk))


        # send and information message containing the expected number of frames
        frame = struct.pack("i", chunks_len)

        nrf.reset_packages_lost()
        nrf.send(frame)

        # TODO: maybe we should wrap this in an infinite loop
        try:
            nrf.wait_until_sent()
            SUCC("Header sent successfully")
        except TimeoutError:
            ERROR("Timeout while sending header")


        # send the rest of the frames
        for idx in range(chunks_len):

            num_retries = 0
            
            # NOTE: we try to send the same frame until it gets sent correctly
            while True:

                if idx % 100 == 0 or idx == chunks_len - 1:
                    progress_bar(
                        active_msg     = f"Sending frame {idx}, retries {num_retries}",
                        finished_msg   = f"All frames sent",
                        current_status = idx + 1,
                        max_status     = chunks_len,
                    )

                nrf.reset_packages_lost()
                nrf.send(packets[idx])

                try:
                    nrf.wait_until_sent()
                    
                except TimeoutError:
                    ERROR("Timeout while transmitting")

                if nrf.get_packages_lost() == 0:
                    break

                else:
                    ERROR(f"Lost packet {idx}, retrying...")
                    num_retries += nrf.get_retries()

    except KeyboardInterrupt:
        ERROR("Process interrupted by user")

    finally:
        nrf.power_down()
        pi.stop()
    
    return










def BEGIN_RECEIVER_MODE() -> None:
    """
    Receives multiple frames from a transmitter and reassembles the blocks into a
    `txt` file, the location of the `txt` depends on if there is a mounted USB or
    not. The flow of the RX MODE is the following:

    1. Start the timer that will interrupt the receiving process if there has not
    been any frame for `timeout` seconds

    2. Start listening the channel for frames. The first frame is treated
    differently as it contains the number of frames that the receiver will expect

    3. Start listening for the regular data frames

    4. After all the frames has been received (or connection has timed-out), we
    merge the payloads into one chunk of data and store it in the mounted USB. If
    there is no mounted USB then the file is stored in memory
    """

    INFO(f"Starting reception: {RECEIVER_TIMEOUT_S} seconds time-out")

    try:
        # list that will contain all the received chunks
        chunks:list[str] = []


        # wait for the first frame of the communication containing the expected number
        # of frames and extract its contents
        INFO("Waiting for header packet...")
        while not nrf.data_ready():
            pass

        header_packet = nrf.get_payload()
        total_chunks  = struct.unpack("i", header_packet[:4])[0] # NOTE: the default size of an int is 4 bytes
        SUCC(f"Header received: expecting {total_chunks} chunks")


        # start listening for frames
        received_chunks   = 0 # NOTE: not the ID
        timer_has_started = False

        tic = time.monotonic()
        tac = time.monotonic()
        while received_chunks < total_chunks and (tac - tic) < RECEIVER_TIMEOUT_S:
            tac = time.monotonic()

            # check if there are frames
            while nrf.data_ready():

                if not timer_has_started:
                    throughput_tic = time.monotonic()
                    timer_has_started = True

                packet = nrf.get_payload()

                chunk = struct.unpack(f"<{len(packet)}s", packet)[0] # NOTE: the struct.unpack method returs more things than just the data
                chunks.append(chunk)
                

                # display the progress of the transmission
                received_chunks += 1

                if received_chunks % 100 == 0 or received_chunks == total_chunks:
                    progress_bar(
                        active_msg     = f"Receiving chunks",
                        finished_msg   = f"All chunks received",
                        current_status = received_chunks,
                        max_status     = total_chunks,
                    )
            
                tic = time.monotonic()
            
        throughput_tac = time.monotonic()
        chunks_len     = len(chunks)


        if received_chunks != total_chunks:
            total_time = throughput_tac - throughput_tic - RECEIVER_TIMEOUT_S
            WARN("Connection timed-out")
        
        else:
            total_time = throughput_tac - throughput_tic
        

        if chunks_len == 0:
            ERROR("Did not receive anything")
            return
        
        
        # check if there is a mounted USB. If not, store the file in memory
        usb_mount_point = find_usb_mount_point()

        if usb_mount_point:
            file_path = usb_mount_point / "received_file.txt"
        else:
            file_path = "received_file.txt"
        

        # store the file
        content = b"".join(chunks)
        with open(file_path, "wb") as f:
            f.write(content)
        content_len = len(content)
        INFO(f"Saved {content_len} bytes to: {file_path}")
        

        # show a last information message with the througput
        INFO(f"Process finished in {total_time:.2f} seconds | Computed throughput: {((content_len / 1024) / total_time):.2f} KBps")
    
    except KeyboardInterrupt:
        ERROR("Process interrupted by user")

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
    """
    Main flow of the application
    """

    role = choose_node_role()
    choose_address_based_on_role(role, nrf)

    if role is Role.TRANSMITTER:
        BEGIN_TRANSMITTER_MODE()
    
    elif role is Role.RECEIVER:
        BEGIN_RECEIVER_MODE()
        
    elif role is Role.CARRIER:
        BEGIN_CONSTANT_CARRIER_MODE()

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




if __name__ == "__main__":
    main()
