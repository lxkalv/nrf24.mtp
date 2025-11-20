"""
##### Transport Layer
Prepares the bursts to be send and makes sure that the information at the other
node arrives correctly.

##### Diagram
```
IO
                      ┌────────────────────┐                      ╮
                      │                    │ <─> CONTROL MESSAGES │
 COMPRESSED PAGES <─> │ PRESENTATION LAYER │ <─> DATA BURSTS      ├ FRAME BYTES
                      │                    │ <─> 256HASH          │
                      └────────────────────┘                      ╯
```

##### Bidirectional
- COMPRESSED PAGES: The compressed pages to be sent over the NRF24 link
- CONTROL MESSAGES: Control messages to ensure reliable transmission and inform the other node in case it is necessary
- DATA BURSTS: Blocks of data sent reliably over the NRF24 link
- 256HASH: Hashes used to verify the integrity of the received data
"""

# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from hashlib import sha256

import nrf24_mtp.utils.Logger as Logger
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: BURST HANDLING :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
BYTES_PER_BURST = 7_905
BYTES_PER_FRAME = 31

def split_page_into_bursts(compressed_page: bytes) -> list[list[bytes]]:
    """
    Splits a given page into a list of bursts, each frame within the burst has a
    FrameID that goes from 0 up to 254.
    """

    # Split the compressed page into bursts of BYTES_PER_BURST size.
    # NOTE: This size is selected in order to generate 255 frames of 31 bytes of
    # width. This prevents the FrameID to get the value of 0xFF, which is reserved for
    # control messages.
    bursts = [
        compressed_page[i : i + BYTES_PER_BURST]
        for i in range(0, len(compressed_page), BYTES_PER_BURST)
    ]

    for BurstID, burst in enumerate(bursts):
        # Split each burst into frames of BYTES_PER_FRAME size.
        frames = [
            burst[i : i + BYTES_PER_FRAME]
            for i in range(0, len(burst), BYTES_PER_FRAME)
        ]

        for FrameID, frame in enumerate(frames):
            frames[FrameID] = FrameID.to_bytes(1) + frame
        
        bursts[BurstID] = frames

    return bursts


def merge_bursts_into_page(bursts: list[list[bytes]]) -> bytes:
    """
    Merges a list of bursts back into a single compressed page. It removes the
    FrameID automatically
    """
    compressed_page = b""

    for burst in bursts:
        for frame in burst:
            compressed_page += frame[1:]  # Remove FrameID byte

    return compressed_page
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: HASH HANDLING ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def compute_burst_hash(burst: list[list[bytes]]) -> bytes:
    """
    Computes a 256-bit (32 bytes) hash for the given burst. The hash is computed
    taking into account the FrameID, not only the data
    """
    burst_data = b"".join(frame for frame in burst)
    burst_hash = sha256(burst_data).digest()
    return burst_hash
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONTROL MESSAGES :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def generate_burst_info_control_message(PageID: int, BurstID: int, burst: list[list[bytes]]) -> bytes:
    """
    Generates a control message indicating the PageID and BurstID being sent, as
    well as the burst size.
    """
    burst_info  = bytes()
    burst_info += 0xFF.to_bytes(1)                                 # Control Message ID
    burst_info += 0xF0.to_bytes(1)                                 # Burst Info Message Type
    burst_info += PageID.to_bytes(1)                               # PageID
    burst_info += BurstID.to_bytes(1)                              # BurstID
    burst_info += (sum(len(frame) for frame in burst)).to_bytes(2) # Burst Size in bytes
    return burst_info



def read_burst_info_control_message(message: bytes) -> tuple[int, int, int] | None:
    """
    Reads a burst info control message and extracts the PageID, the BurstID and the Burst Size
    """
    if len(message) != 6:
        Logger.ERROR("Invalid burst info control message length.")
        return None
    _         = message[0]  # Control Message ID
    _         = message[1]  # Burst Info Message Type
    PageID    = message[2]
    BurstID   = message[3]
    BurstSize = int.from_bytes(message[4:6])
    return PageID, BurstID, BurstSize



def generate_transfer_finish_control_message() -> bytes:
    """
    Generates a control message indicating that the transfer has finished.
    """
    finish_message  = bytes()
    finish_message += 0xFF.to_bytes(1)  # Control Message ID
    finish_message += 0x0F.to_bytes(1)  # Transfer Finish Message Type
    return finish_message


def read_transfer_finish_control_message(message: bytes) -> bool:
    """
    Reads a transfer finish control message and returns True if it is valid.
    """
    if len(message) != 2:
        Logger.ERROR("Invalid transfer finish control message length.")
        return False

    return True
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::