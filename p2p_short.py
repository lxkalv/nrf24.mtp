# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from nrf24 import (
    NRF24,

    RF24_DATA_RATE,
    RF24_PA,
    RF24_RX_ADDR,
    RF24_PAYLOAD,
    RF24_CRC,
)

from hashlib import shake_256
from pathlib import Path
from math import ceil
import argparse
import pigpio
import zlib
import time
import os

from typing import NoReturn

os.system("clear")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
CE_PIN = 22

BYTES_IN_FRAME              = 30

CHANNEL_READ_TIMEOUT        = 200e-3
NUMBER_OF_CYCLES            = 5

FILE_TO_TX_STR              = "MTP-F25-SRI-A-TX.txt"
FILE_TO_RX_STR              = "MTP-F25-SRI-A-RX.txt"
RECEIVED_FILE_NAME          = "received_file.txt"
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



# :::: NODE CONFIG ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def get_id() -> str:
    """
    Return the contents of the node_id file in the user folder
    """
    return Path("~/node_id").expanduser().resolve().read_text().strip()



def disable_auto_ack(nrf: NRF24) -> None:
    #nrf.unset_ce()
    #nrf._nrf_write_reg(nrf.EN_AA, 0x00)   # <<< disable auto-ack for all pipes
    #nrf.set_ce()

    nrf.set_retransmission(1, 15)  # <<< disable auto-retransmissions (x+1) * 250 µs
    return



def create_radio_object(ce_pin: int) -> NRF24 | None:
    """
    Generate an instance to control the NRF24 radio module
    """
    hostname = "localhost"
    port     = 8888

    pi = pigpio.pi(hostname, port)
    if not pi.connected:
        ERROR("Not connected to Raspberry Pi, exiting")
        return None

    nrf = NRF24(
        pi            = pi,
        ce            = ce_pin,
        spi_speed     = 10e6,
        data_rate     = RF24_DATA_RATE.RATE_1MBPS, # NOTE: The lowest possible to increase range and reduce BER
        payload_size  = RF24_PAYLOAD.DYNAMIC,
        address_bytes = 4,
        crc_bytes     = RF24_CRC.BYTES_2,
        pa_level      = RF24_PA.MAX,                # NOTE: Maybe increase this to MAX
    )

    # Shared address across all network nodes to simulate broadcast
    address = b"NMNA"
    nrf.open_writing_pipe(address)
    nrf.open_reading_pipe(RF24_RX_ADDR.P1, address)

    # Disable the autoacks, there is no response in this network protocol
    disable_auto_ack(nrf)
    
    INFO(f"NRF24 Radio configuration:")
    nrf.show_registers()

    return nrf



def get_node_config() -> tuple[NRF24 | None, str, bool]:
    """
    Get a fully configured node based on user input and NODE_ID
    """
    parser = argparse.ArgumentParser(description = "NRF24 Network Mode")
    parser.add_argument(
        "--first",
        action = "store_true",                  # Si se pone la flag vale True, si no False
        help   = "Select this node as TX node",
    )
    args = parser.parse_args()
    
    if args.first:
        INFO("Node initialized as primary TX")
    else:
        INFO("Node initialized as primary RX")

    node_id = get_id()
    INFO(f"Detected NODE_ID: {node_id}")

    ce_pin = CE_PIN
    INFO(f"Selected CE PIN: {ce_pin}")

    nrf = create_radio_object(ce_pin)
    return nrf, node_id, args.first
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: FILE IO ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def get_file_to_transmit() -> Path | None:
    """
    Get the path to the file to transmit
    """
    
    file_path = Path(FILE_TO_TX_STR).resolve()
    
    if file_path.exists():
        SUCC(f"Valid file detected inside USB: {file_path}")
        return file_path
    
    else:
        ERROR(f"No valid file was found inside the USB, stopping")
        return None



def compress_file(content: bytes) -> bytes:
    """
    Compress the file content using zlib
    """
    compressed_content = zlib.compress(content, level = 6)
    INFO(f"Compressed file from {len(content)} B to {len(compressed_content)} B")
    return compressed_content



def decompress_file(content: bytes) -> bytes:
    """
    Decompress the file content using zlib
    """
    decompressed_content = zlib.decompress(content)
    INFO(f"Decompressed file from {len(content)} B to {len(decompressed_content)} B")
    return decompressed_content
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# :::: CHANNELS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def get_channels_based_on_node_id(all_channels: list[int], node_id: str) -> tuple[list[int], list[int]]:
    """
    Return a list with the own channels and a list with the channels assigned to the other nodes
    """
    if   node_id == "tan0":
        offset = 0
    elif node_id == "tan1":
        offset = 1
    elif node_id == "tbn0":
        offset = 2
    elif node_id == "tbn1":
        offset = 3

    INFO(f"Selected offset: {offset}")
    own_channels   = all_channels[offset : -1 : 4]
    other_channels = all_channels.copy()

    for channel in own_channels:
        other_channels.remove(channel)

    return own_channels, other_channels



def is_channel_free(nrf: NRF24) -> int:
    """
    Check if a channel has a power >= -65dBm
    """
    return nrf._nrf_read_reg(NRF24.RPD, 1)[0] & 1



def choose_free_channel(nrf: NRF24, own_channels: list[int]) -> int:
    """
    Listen to TX channels to select one for transmission
    """

    INFO("Listening TX channels to determine occupancy")
    
    channel_occupancy = [
        0 for _ in own_channels
    ]

    for _ in range(NUMBER_OF_CYCLES):
        for idx, channel in enumerate(own_channels):
            nrf.set_channel(channel)
            time.sleep(.1) # Wait for 200 ms
            channel_occupancy[idx] += is_channel_free(nrf)
    SUCC("Channel scan completed")

    selected = own_channels[0]
    min_occ  = NUMBER_OF_CYCLES + 1
    for occ, channel in zip(channel_occupancy, own_channels):
        INFO(f"    Channel {channel} occupancy: {occ}")

        if occ < min_occ:
            min_occ  = occ
            selected = channel

    SUCC(f"Selected channel {selected} to transmit")

    return selected



def choose_occupied_channel(nrf: NRF24, other_channels: list[int]) -> int:
    """
    Listen to RX channels to detect a frame in any channel
    """
    INFO("Listening to RX channels to look for transmitters")
    
    channel_idx = 0
    while True:
        channel = other_channels[channel_idx % len(other_channels)]
        INFO(f"Listening on channel: {channel}")
        
        tic = time.time()
        tac = time.time()
        while (tac - tic) < CHANNEL_READ_TIMEOUT:
            tac = time.time()
            nrf.set_channel(channel)
            time.sleep(.05) # Wait for 50 ms

            if not nrf.data_ready(): continue
            
            INFO(f"Detected a transmitter on channel {channel}")
            return channel
        channel_idx += 1
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# :::: FLOW FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def ACT_AS_TX(nrf: NRF24, content: bytes, own_channels: list[int]) -> NoReturn:
    """
    Put the node in TX mode and start transmitting indefinetly until the process is terminated
    """

    nrf.power_up_rx()
    channel = choose_free_channel(nrf, own_channels)
    nrf.set_channel(channel)
    
    # split the bytes into frames with a FrameID
    frames = [
        FrameID.to_bytes(2) + content[i : i + BYTES_IN_FRAME]
        for FrameID, i in enumerate(range(0, len(content), BYTES_IN_FRAME))
    ]

    header_message  = bytes()
    header_message += 0xFFFF.to_bytes(2)            # Header reserved to control messages NOTE: Up to 65.535 Data Frames
    header_message += len(content).to_bytes(3)      # Ammount of data to transmit         NOTE: Up to 16.777.216 Bytes
    header_message += shake_256(content).digest(27) # Checksum of the file
    INFO(f"Generated header message: HEADER: {header_message[0:2]} | File length: {int.from_bytes(header_message[2:5])} B | Checksum: {header_message[5:].hex()}")
    
    cycle = []
    cycle.append(header_message)
    cycle.extend(frames)
    cycle_len = len(cycle)

    idx = 0
    while True:
        message = cycle[idx % cycle_len]
        nrf.send(message)
        idx += 1





def ACT_AS_RX(nrf: NRF24, other_channels: list[int]) -> tuple[bytes, float]:
    nrf.power_up_rx()
    channel = choose_occupied_channel(nrf, other_channels)
    nrf.set_channel(channel)

    tic_started        = False

    checksum           = None
    is_reading_frames  = False
    slots              = []

    prev_len           = -1
    prev_checksum      = -1

    while True:
        if not nrf.data_ready():
            continue

        if not tic_started:
            tic = time.time()
            tic_started = True

        frame: bytes = nrf.get_payload()
        FrameID = int.from_bytes(frame[0:2])

        if FrameID == 0xFFFF:
            data_len = int.from_bytes(frame[2:5])
            checksum = frame[5:]
            
            if (
                data_len != prev_len
            or  checksum != prev_checksum
            ):
                prev_len      = data_len
                prev_checksum = checksum
                
                num_of_frames = ceil(data_len / BYTES_IN_FRAME)
                
                slots = [
                    bytes()
                    for _ in range(num_of_frames)
                ]

                is_reading_frames = True
                INFO(f"Parsed header message: File length: {data_len} B | Checksum: {checksum.hex()}")


        if is_reading_frames and (FrameID < 0xFFFF):
            slots[FrameID] = frame[2:]
    

        if is_reading_frames and (FrameID == num_of_frames - 1):
            computed_checksum = shake_256(b"".join(slots)).digest(27)

            if computed_checksum == checksum:
                tac = time.time()
                elapsed = tac - tic
                SUCC("The checksum is correct")
                INFO(f"RF throughput: {len(b''.join(slots)) / elapsed / 1024:.2f} KiBps | Elapsed time: {elapsed:.2f} s | Received bytes: {len(b''.join(slots))} B")
                return b"".join(slots), elapsed

            else:
                WARN("The checksum is incorrect, retrying")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: MAIN :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def main(nrf: NRF24, node_id: str, is_first_node: bool) -> None:
    """
    Main flow of the application
    """
    all_channels                 = [15,45,76,90]
    own_channels, other_channels = get_channels_based_on_node_id(all_channels, node_id)

    INFO(f"TX channels: {own_channels}")
    INFO(f"RX channels: {other_channels}")

    if is_first_node:
        file_path = get_file_to_transmit()
        if not file_path: return
        
        content = file_path.read_bytes()
        compressed_content = compress_file(content)
        ACT_AS_TX(nrf, compressed_content, own_channels)

    else:
        content, elapsed = ACT_AS_RX(nrf, other_channels)
        content          = decompress_file(content)
        INFO(f"Data throughput: {len(content) / elapsed / 1024:.2f} KiBps | Elapsed time: {elapsed:.2f} s | Received bytes: {len(content)} B")
        file_path        = Path(FILE_TO_RX_STR).resolve()
        file_path.write_bytes(content)
        SUCC(f"File successfully saved to: {file_path}")
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




if __name__ == "__main__":
    try:
        nrf, node_id, first = get_node_config()
        if nrf is not None:
            main(nrf = nrf, node_id = node_id, is_first_node = first)

    except KeyboardInterrupt:
        ERROR("Process interrupted by the user")

    finally:
        if nrf is not None: nrf.power_down()