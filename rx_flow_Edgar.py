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

    get_usb_mount_path,
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
    
    #Join the raw bytes of all pages and generate the txt file
    content = b"".join(decompressed_pages)

    # Find a USB mount point. And create the txt file in its path
    test_files_dir = Path("received_test_files")
    usb_mount_path = get_usb_mount_path()
    file_path      = None

    if usb_mount_path:
        file_path = usb_mount_path / "received_file.txt"
        with open(file_path, "wb") as f:
            f.write(content)
        content_len = len(content)
        INFO(f"Saved {content_len} bytes to: {file_path}")

    if not file_path:
        file_path = test_files_dir / "received_file.txt"  
        WARN(f"File candidate not found, using fallback file: {file_path}")
        file_path = usb_mount_path / "received_file.txt"
        with open(file_path, "wb") as f:
            f.write(content)
        content_len = len(content)
        INFO(f"Saved {content_len} bytes to: {file_path}")
        
    else:
        INFO(f"Selected file candidate: {file_path}")
    
def RX_TRANSPORT_LAYER(BURSTS: dict[str, dict[str, bytes]], CHECKSUM: dict[str, str] , VERIFIED: dict[str, bool]) -> tuple[dict[str, bool], list[bytes]]:
    """
    This layer is responsible for the following things:
    - Compute the checksum of a received bursts
    - Verify the computed checksum of the bursts is equal to
    the received checksum by the transmitter
    - Identify if the received bursts are corrupted.
    - Decides if any burst needs retransmision.
    - In case that all the bursts were received correctly, it joins all the bursts in a page
    - The layer will only ccompute the checksum of the indicated bursts
    """
    # NOTE:The BURST unified structure that will be introduced in the Transport Layer is
    #
    #     BURST 0:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B32]
    #         ...
    #         CHUNK 255: bytes[B0, B1, B2, ..., B32]
    #     BURST 1:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B32]
    #         ...
    #         CHUNK 255: bytes[B0, B1, B2, ..., B32]
    #     ...
    #     BURST N:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B32]
    #         ...
    #         CHUNK 255: bytes[B0, B1, B2, ..., B32]
    #
    # NOTE: The STREAM-like structure introduced in the Transport Layer
    # that contains the checksums of each burst, is like this:
    #
    #     BURST 0: str[ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb]
    #     BURST 1: str[ea325d761f98c6b73320e442b67f2a3574d9924716d788ddc0dbbdcaca853fe7]
    #     BURST 2: str[d26cd84ddae9829c5a1053fce8e1c1d969086940e58c56d65d27989b6b46bba2]
    #     ...
    #     BURST N: str[1694f1a2500bf2aa881c461c92655b3621cae5bbf70b4177d02a2aa92c1aa903]
    #
    # Generate the unified structure where we are goint to store Burst ID and if the burst
    # needs a retransmission because it has been corrupted
    # NOTE: The structure is like this
    #
    #   BURST 0: False / True
    #   BURST 1: False / True
    #   BURST 2: False / True
    #   ...
    #   BURST N: False / True
    CHECKED_BURSTS: dict[str, bool] = dict()

    # Generate the Page that will be sent to the Presentation Layer.
    Page: list[bytes] = []

    # For each burst compute its checksum and compares it to the received 
    # checksum by the transmitter. If the checksum is correct the retransmision
    # of that burst is not needed, otherwise it will be needed.
    for idx_burst in BURSTS:
        burst_hasher = hashlib.sha256()
        if VERIFIED[f"BURST{idx_burst}"] == False:
            CHECKED_BURSTS[f"BURST{idx_burst}"] = dict()
            for idx_chunk in BURSTS[f"BURST{idx_burst}"]:
                burst_hasher.update(BURSTS[f"BURST{idx_burst}"][f"CHUNK{idx_chunk}"])

            computed_checksum = burst_hasher.hexdigest()
            Page.append(BURSTS[f"BURST{idx_burst}"])

            if computed_checksum == CHECKSUM[f"BURST{idx_burst}"]:
                CHECKED_BURSTS[f"BURST{idx_burst}"] = True
            else:
                CHECKED_BURSTS[f"BURST{idx_burst}"] = False
                Retrasnmision_needed = True
    # If retransmission needed do not create the page, otherwise create the page
    if Retrasnmision_needed:
        return (CHECKED_BURSTS, None)
    else:
        return (CHECKED_BURSTS, Page)

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



