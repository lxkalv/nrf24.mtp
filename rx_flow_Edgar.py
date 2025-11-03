# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
from math import ceil
import hashlib
import zlib

from radio import CustomNRF24

from utils import (
    SUCC,
    WARN,
    INFO,

    progress_bar,
)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: PROTOCOL LAYERS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def RX_PRESENTATION_LAYER(compressed_pages: list[bytes]) -> None:
    """
    This layer is responsible for for the following things:
    - Receiving the pages from the previous layer
    - Decompress each page 
    - Join all the pages into an unique txt file.
    - Storing the generated txt file inside a USB. 
    """

    #Decompress each page
    decompressed_pages: list[bytes] = []
    decompressor = zlib.compressobj(level = 6)
    for idx, compressed_page in enumerate(compressed_pages, 1):
        decompressed_page = decompressor.decompress(compressed_page)
        decompressed_pages.append(decompressed_page)
        decompressed_len = sum(len(decompressed_page) for decompressed_page in decompressed_pages)
        progress_bar(
            pending_msg     = "Decompressing pages...",
            finished_msg    = f"Pages decompressed successfully, | Compression ratio: ~{decompressed_len / len(compressed_page) * 100:.2f}% | {len(compressed_page)} B -> {decompressed_len} B",
            current_status  = idx,
            finished_status = len(compressed_pages)
        )


def RX_LINK_LAYER(prx: CustomNRF24) -> None:
    """
    This layer is responsible for the following things:
    - Reading a TX_INFO message and set up the expected number of pages
    - Reading a PAGE_INFO message
    - Reading a BURST_INFO message
    - Receiving each frame
    - Compute and send the checksum at the end of the burst
    """
    
    # Wait for a TX_INFO message
    # NOTE: As of now, the TX_INFO message only contains the number of pages that will
    # be sent in the communication and the total ammount of bytes that are expected to
    # be transfered, but it can be changed to include more information
    #
    # | TxLength (4B) | TxWidth (4B) | = 8 Bytes
    #
    # TxLength: The number of pages that will be sent in the communication       [0..4_294_967_295]
    # TxWidth:  The total number of bytes that will be sent in the communication [0..4_294_967_295]
    INFO("Waiting for TX_INFO message")
    while not prx.data_ready():
        pass

    TX_INFO  = prx.get_payload()
    TxLength = int.from_bytes(TX_INFO[0:4])
    TxWidth  = int.from_bytes(TX_INFO[4:8])
    SUCC(f"Received TX_INFO message: TxLength = {TxLength} | TxWidth = {TxWidth}")

    # Iterate over all the pages of the communication
    PageID = 0
    while PageID < TxLength - 1:
        # Wait of a PAGE_INFO message
        # NOTE: everytime we start a page, we send a PAGE_INFO message containing the
        # PageID, the ammount of bytes in the page (page_width) and the number of bursts
        # in the page (page length). The PAGE_INFO message payload has the following
        # structure:
        #
        # | PageID (1B) | PageLength (3B) | = 4 Bytes
        # 
        # PageID:     The identifier of the page           [0..255]
        # PageLength: The number of bursts inside the page [0..16_777_215]
        INFO("Waiting for PAGE_INFO message")
        while not prx.data_ready():
            pass

        PAGE_INFO  = prx.get_payload()
        PageID     = PAGE_INFO[0]
        PageLength = int.from_bytes(PAGE_INFO[1:4])
        SUCC(f"Received PAGE_INFO message: PageID = {PageID} | PageLength = {PageLength}")

# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_RX_MODE(prx: CustomNRF24) -> None:
    RX_LINK_LAYER(prx)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
