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










# :::: CONSTANTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
NUMBER_OF_PAGES  = 10
BURST_WIDTH      = 7905
CHUNK_WIDTH      = 31
CHECKSUM_TIMEOUT = 1
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
Inv_checksums = [0]









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
    PAGE_len    = ceil(content_len / NUMBER_OF_PAGES)
    PAGES       = [
        content[i : i + PAGE_len]
        for i in range(0, content_len, PAGE_len)
    ]
    INFO(f"Splitted file into {NUMBER_OF_PAGES} PAGES of {PAGE_len} B (last page may be shorter)")


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





def TX_TRANSPORT_LAYER(PAGES: list[bytes]) -> tuple[list[list[list[bytes]]], list[list[bytes]]]:
    # Generate the organized structure containing the DATA to be transmitted organized
    # by PageID, BurstID and ChunkID.
    #
    # NOTE: The STREAM structure looks like this:
    #
    # PAGE 0:
    #     BURST 0:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B31]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B31]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B31]
    #         ...
    #         CHUNK 254: bytes[B0, B1, B2, ..., BXX]
    #     BURST 1:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B31]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B31]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B31]
    #         ...
    #         CHUNK 254: bytes[B0, B1, B2, ..., BXX]
    #     ...
    #     BURST Y:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B31]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B31]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B31]
    #         ...
    #         CHUNK 254: bytes[B0, B1, B2, ..., BXX]
    # PAGE 1:
    #     BURST 0:
    #         ...
    #     BURST 1:
    #         ...
    #     ...
    #     BURST Y:
    #         ...
    # ...
    # PAGE Z:
    #     BURST 0:
    #         ...
    #     BURST 1:
    #         ...
    #     ...
    #     BURST Y:
    #         ...
    #
    #       PageID    BurstID   ChunkID    Payload(MessageID + ChunkID + DATA)
    #       ↓         ↓         ↓          ↓
    STREAM: list[     list[     list[      bytes]]] = list()

    # Generate the organized structure containing the CHECKSUMS of each BURST
    # NOTE: The CHECKSUMS structure looks like this:
    #
    # PAGE 0:
    #     BURST 0: [0B, 1B, 2B, ..., 31B]
    #     BURST 1: [0B, 1B, 2B, ..., 31B]
    #     BURST 2: [0B, 1B, 2B, ..., 31B]
    #     ...
    #     BURST Y: [0B, 1B, 2B, ..., 31B]
    # PAGE 1:
    #     BURST 0: ...
    #     BURST 1: ...
    #     ...
    #     BURST Y: ...
    # ...
    # PAGE Z:
    #     BURST 0: ...
    #     BURST 1: ...
    #     ...
    #     BURST Y: ...
    #
    # NOTE: The CHECKSUM of each BURST is computed INCLUDING the headers
    #
    #          PageID    BurstID    CHECKSUM
    #          ↓         ↓          ↓
    CHECKSUMS: list[     list[      bytes]] = list()

    
    # Build the STREAM and CHECKSUMS structures
    for PageID, PAGE in enumerate(PAGES):

        STREAM.append(list())
        CHECKSUMS.append(list())
        
        # Split each compressed PAGE into BURSTS of 7905 B
        BURSTS = [
            PAGE[i : i + BURST_WIDTH]
            for i in range(0, len(PAGE), BURST_WIDTH)
        ]

        for BurstID, BURST in enumerate(BURSTS):

            STREAM[PageID].append(list())
            CHECKSUMS[PageID].append(bytes())

            # Split the BURST into CHUNKS of 31 B
            CHUNKS = [
                BURST[i : i + CHUNK_WIDTH]
                for i in range(0, len(BURST), CHUNK_WIDTH)
            ]

            BURST_hasher = hashlib.sha256()
            for ChunkID, CHUNK in enumerate(CHUNKS):

                STREAM[PageID][BurstID].append(bytes())
                STREAM[PageID][BurstID][ChunkID] += ChunkID.to_bytes(1)
                STREAM[PageID][BurstID][ChunkID] += CHUNK
                
                BURST_hasher.update(STREAM[PageID][BurstID][ChunkID])

            CHECKSUMS[PageID][BurstID] = BURST_hasher.digest()

    return (STREAM, CHECKSUMS)





def TX_LINK_LAYER(PTX: CustomNRF24, STREAM: list[list[list[bytes]]], CHECKSUMS: list[list[str]]) -> None:
    PageID = 0
    while PageID < len(STREAM):
        BurstID = 0
        while BurstID < len(STREAM[PageID]):
            ChunkID = 0
            INFO(f"Sending BURST {BurstID} expected CHECKSUM: {CHECKSUMS[PageID][BurstID].hex()}")

            BURST_INFO  = bytes()
            BURST_INFO += 0xFF.to_bytes(1)                                                   # INFO message header
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

            while (tac - tic) < CHECKSUM_TIMEOUT:
                tac = time.time()

                if not PTX.data_ready():
                    continue

                received = PTX.get_payload()
                if received == CHECKSUMS[PageID][BurstID]:
                    SUCC(f"BURST {BurstID} transmitted successfully")
                    BurstID += 1
                    break

                else:
                    Inv_checksums[0] += 1
                    WARN(f"Invalid CHECKSUM received for BURST {BurstID}: {received.hex()}")
                    break
            
            if (tac - tic) >= CHECKSUM_TIMEOUT:
                ERROR(f"CHECKSUM timeout for BURST {BurstID}")
            
        PageID += 1
                    
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_TX_MODE(ptx: CustomNRF24) -> None:
    compressed_pages  = TX_PRESENTATION_LAYER()
    STREAM, CHECKSUMS = TX_TRANSPORT_LAYER(compressed_pages)
    TX_LINK_LAYER(ptx, STREAM, CHECKSUMS)

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
