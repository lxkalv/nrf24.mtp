# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
from math import ceil
import hashlib
import zlib
import time

from radio import CustomNRF24

from utils import (
    ERROR,
    SUCC,
    WARN,
    INFO,

    progress_bar,
    status_bar,

    get_usb_mount_path,
    find_valid_txt_file_in_usb,
)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: CONSTANTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
PAGE_WIDTH       = 126_945
CHUNK_WIDTH      = 30
CHECKSUM_TIMEOUT = 1
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: PROTOCOL LAYERS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def TX_PRESENTATION_LAYER() -> list[bytes]:
    # Find a valid .txt file from a USB mount point.
    # NOTE: If no USB mount point is found or if no valid file is found inside the
    # USB, then a fallback file is taken from the "test_files" directory
    fallback_dir   = Path("test_files")
    usb_mount_path = get_usb_mount_path()
    file_path      = None

    if usb_mount_path:
        file_path = find_valid_txt_file_in_usb(usb_mount_path)

    if not file_path:
        file_path = fallback_dir / "lorem.txt"  
        WARN(f"File candidate not found, using fallback file: {file_path}")
    
    else:
        INFO(f"Selected file candidate: {file_path}")


    # Read the raw bytes from the file and split them into PAGES
    # NOTE: The size and number of PAGES is completely arbitrary for now, we will
    # split the file into NUMBER_OF_PAGES PAGES of undefined size
    content = file_path.read_bytes()
    
    content_len = len(content)
    PAGES       = [
        content[i : i + PAGE_WIDTH]
        for i in range(0, content_len, PAGE_WIDTH)
    ]
    INFO(f"Splitted file into {len(PAGES)} PAGES of {PAGE_WIDTH} B (last page may be shorter)")


    # Compress each PAGE
    # TODO: Find the best compression mechanism that we can. Compression is a very
    # critical aspect in this project given that the achieved throughput is very low
    # and that we expect interferences in the link
    compressed_PAGES: list[bytes] = []
    compressor = zlib.compressobj(level = 6)
    for idx, PAGE in enumerate(PAGES, 1):
        compressed_PAGE  = compressor.compress(PAGE)
        compressed_PAGE += compressor.flush(zlib.Z_SYNC_FLUSH)
        compressed_PAGES.append(compressed_PAGE)

        compressed_len = sum(len(compressed_PAGE) for compressed_PAGE in compressed_PAGES)
        progress_bar(
            pending_msg     = "Compressing PAGES...",
            finished_msg    = f"PAGES compressed successfully | Compression ratio: ~{compressed_len / content_len * 100:.2f}% | {content_len} B -> {compressed_len} B",
            current_status  = idx,
            finished_status = len(PAGES)
        )
    
    # Provide the PAGES to the next layer
    return compressed_PAGES





def TX_TRANSPORT_LAYER(PAGES: list[bytes]) -> tuple[list[list[bytes]], list[bytes]]:
    #       PageID  ChunkID  Payload(MessageID | ChunkID + DATA)
    #       ↓       ↓        ↓
    STREAM: list[   list[    bytes]] = list()

    #          PageID  CHECKSUM
    #          ↓       ↓
    CHECKSUMS: list[   bytes] = list()

    # Build the STREAM and CHECKSUMS structures
    for PageID, PAGE in enumerate(PAGES):

        STREAM.append(list())
        
        # Split the PAGE into CHUNKS of 30 B
        CHUNKS = [
            PAGE[i : i + CHUNK_WIDTH]
            for i in range(0, len(PAGE), CHUNK_WIDTH)
        ]

        PAGE_hasher = hashlib.sha256()
        for ChunkID, CHUNK in enumerate(CHUNKS):

            STREAM[PageID].append(bytes())
            STREAM[PageID][ChunkID] += ChunkID.to_bytes(2) # The first 4 bites should always be 0
            STREAM[PageID][ChunkID] += CHUNK
                
            PAGE_hasher.update(STREAM[PageID][ChunkID])

            CHECKSUMS.append(PAGE_hasher.digest())

    return (STREAM, CHECKSUMS)





def TX_LINK_LAYER(PTX: CustomNRF24, STREAM: list[list[bytes]], CHECKSUMS: list[bytes]) -> None:
    PageID = 0
    while PageID < len(STREAM):
        BurstID = 0
        while BurstID < len(STREAM[PageID]):
            ChunkID = 0
            INFO(f"Sending BURST {BurstID} expected CHECKSUM: {CHECKSUMS[PageID][BurstID].hex()}")

            BURST_INFO  = bytes()
            BURST_INFO += 0xFF.to_bytes(1)                                                   # INFO message header
            BURST_INFO += 0xF0.to_bytes(1)                                                   # BURST_INFO sub-message header
            BURST_INFO += PageID.to_bytes(1)                                                 # PageID header
            BURST_INFO += BurstID.to_bytes(1)                                                # BurstID header (should always fit)
            BURST_INFO += (sum(len(chunk) for chunk in STREAM[PageID][BurstID])).to_bytes(2) # ammount of bytes in the BURST
            PTX.send_CONTROL_message(BURST_INFO, "BURST_INFO")

            while ChunkID < len(STREAM[PageID][BurstID]):
                PTX.send_DATA_message(STREAM[PageID][BurstID][ChunkID], PageID, BurstID, ChunkID)
                ChunkID += 1

            PTX.power_up_rx()
            tic = time.time()
            tac = time.time()

            checksum_received = False
            while (tac - tic) < CHECKSUM_TIMEOUT and not checksum_received:
                tac = time.time()

                if not PTX.data_ready():
                    continue
                checksum_received = True
                received = PTX.get_payload()

                if received == CHECKSUMS[PageID][BurstID]:
                    SUCC(f"BURST {BurstID} transmitted successfully")
                    BurstID += 1

                else:
                    WARN(f"Invalid CHECKSUM received for BURST {BurstID}: {received.hex()}")
            
            if (tac - tic) >= CHECKSUM_TIMEOUT:
                ERROR(f"CHECKSUM timeout for BURST {BurstID}")
            
        PageID += 1

    TRANSFER_FINISH  = bytes()
    TRANSFER_FINISH += 0xFF.to_bytes(1)  # INFO message header
    TRANSFER_FINISH += 0x0F.to_bytes(1)  # TRANSFER_FINISH sub-message header
    PTX.send_CONTROL_message(TRANSFER_FINISH, "TRANSFER_FINISH")        
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_TX_MODE(ptx: CustomNRF24) -> None:
    compressed_pages  = TX_PRESENTATION_LAYER()
    STREAM, CHECKSUMS = TX_TRANSPORT_LAYER(compressed_pages)
    TX_LINK_LAYER(ptx, STREAM, CHECKSUMS)

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
