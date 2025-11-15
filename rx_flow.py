# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
import hashlib
import zlib
import time
import math

from radio import CustomNRF24

from utils import (
    ERROR,
    get_usb_mount_path,

    progress_bar,

    SUCC,
    WARN,
    INFO,
)

from nrf24 import (
    RF24_RX_ADDR
)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

CHECKSUM_TIMEOUT = 0.5
MAX_PAYLOAD = 32






# :::: PROTOCOL LAYERS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def generate_STREAM_section_based_on_BURST_INFO(frame: bytes, STREAM: list[list[list[bytes]]]) -> tuple[int, int, list[bytes]]:
    _                 = frame[0]
    PageID            = frame[1]
    BurstID           = frame[2]
    size_of_burst     = int.from_bytes(frame[3:5])

    chunks_in_burst   = math.ceil(size_of_burst / MAX_PAYLOAD)
    length_last_chunk = size_of_burst % MAX_PAYLOAD if (size_of_burst % MAX_PAYLOAD) != 0 else MAX_PAYLOAD

    sizes = list()

    if len(STREAM) <= PageID:
        STREAM.append(list())
    if len(STREAM[PageID]) <= BurstID:
        STREAM[PageID].append(list())
    for ChunkID in range(chunks_in_burst):
        STREAM[PageID][BurstID].append(bytes())
        if ChunkID == chunks_in_burst - 1:
            sizes.append(bytes(length_last_chunk))
        else:
            sizes[PageID][BurstID].append(bytes(32))

    return (PageID, BurstID, sizes)

def RX_LINK_LAYER(PRX: CustomNRF24) -> None:
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
    #         CHUNK 255: bytes[B0, B1, B2, ..., BXX]
    #     BURST 1:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B31]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B31]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B31]
    #         ...
    #         CHUNK 255: bytes[B0, B1, B2, ..., BXX]
    #     ...
    #     BURST Y:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B31]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B31]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B31]
    #         ...
    #         CHUNK 255: bytes[B0, B1, B2, ..., BXX]
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
    #       PageID    BurstID   ChunkID    Payload(MessageID + PageID + BurstID + ChunkID + DATA)
    #       ↓         ↓         ↓          ↓
    STREAM: list[     list[     list[      bytes]]] = list()

    # NOTE: The flow of the PRX is as follows, we keep receiving frames until the
    # transmission has finished. We do not care about the order of the frames as we
    # assume that the PTX will take care of that. We treat each frame differently
    # depending on the information inside the frame itself. After each received frame,
    # we evaluate if the transmission has ended or not
    TRANSFER_HAS_ENDED = False
    TX_HAS_STARTED     = False

    THROUGHPUT_TIC = 0
    THROUGHPUT_TAC = 0

    BURST_HASHER = hashlib.sha256()
    while not TRANSFER_HAS_ENDED:
        # If we have not received anything we do nothing
        while not PRX.data_ready(): continue

        # Pull the received frame from the FIFO
        frame: bytes = PRX.get_payload()

        # Burst INFO
        if frame[0] == 0xFF:
            PageID, BurstID, sizes = generate_STREAM_section_based_on_BURST_INFO(frame, STREAM)

            if not TX_HAS_STARTED:
                THROUGHPUT_TIC = time.time()
            TX_HAS_STARTED = True
            
        else:
            ChunkID = frame[0]

            # If the header information is invalid we discard the frame
            if (
               ChunkID > len(sizes) - 1
            or len(frame) != len(sizes[ChunkID])
            ):
                WARN(f"Invalid message received: {ChunkID:03d} -> {len(frame)} B")
                continue

            STREAM[PageID][BurstID][ChunkID] = frame

            if ChunkID == len(sizes) - 1:
                SUCC(f"Completed BURST: {PageID:02d}|{BurstID:03d}")
                BURST_HASHER = hashlib.sha256()
                for ChunkID, chunk in enumerate(STREAM[PageID][BurstID]):
                    if not chunk: WARN(f"Missing {ChunkID:03d} in BURST")
                    BURST_HASHER.update(chunk)
                CHECKSUM = BURST_HASHER.digest()
                
                tic = time.time()
                tac = time.time()

                while (tac - tic) < CHECKSUM_TIMEOUT:
                    tac = time.time()

                    PRX.reset_packages_lost()
                    PRX.send(CHECKSUM)

                    try:
                        PRX.wait_until_sent()
                    except TimeoutError:
                        ERROR(f"Time-out while sending CHECKSUM for BURST {PageID:02d}|{BurstID:03d}, retrying")
                        continue

                    if PRX.get_packages_lost() > 0:
                        ERROR(f"Packages lost while sending CHECKSUM for BURST {PageID:02d}|{BurstID:03d}, retrying")
                        continue

                    SUCC(f"CHECKSUM for BURST {PageID:02d}|{BurstID:03d} sent successfully: {CHECKSUM.hex()}")
                    break

                if (tac - tic) >= CHECKSUM_TIMEOUT:
                    ERROR(f"CHECKSUM timeout for BURST {PageID:02d}|{BurstID:03d}, retrying") 


    # INFO(f"Computed throughput: {tx_data / tx_time / 1024:.2f} KBps over {tx_time:.2f} seconds | {tx_data / 1024:.2f} KB transferred")
    return STREAM



def RX_TRANSPORT_LAYER(STREAM: list[list[list[bytes]]]) -> list[bytes]:
    """
    This layer is responsible for the following things:
    - Receiving the unified STREAM structure and decompose it into compressed
    pages
    - Provice the compressed pages to the next layer
    """

    compressed_pages = []
    for PageID in range(len(STREAM)):
        compressed_page = bytes()
        for BurstID in range(len(STREAM[PageID])):
            for ChunkID in range(len(STREAM[PageID][BurstID])):
                compressed_page += STREAM[PageID][BurstID][ChunkID][3:] # NOTE: We ignore the first 3 Bytes as they are the headers
        compressed_pages.append(compressed_page)
    
    return compressed_pages



def RX_PRESENTATION_LAYER(compressed_pages: list[bytes]) -> None:
    """
    This layer is responsible for the following things:
    - Receive the compressed pages from the previous layer
    - Uncompress each page
    - Join all the pages into a unique txt file
    - Store the file into a possible location
    """

    compressed_len = sum(len(compressed_page) for compressed_page in compressed_pages)

    pages: list[bytes] = []
    decompressor = zlib.decompressobj()
    for idx, compressed_page in enumerate(compressed_pages, 1):
        page = decompressor.decompress(compressed_page)
        pages.append(page)

        uncompressed_len = sum(len(page) for page in pages)

        progress_bar(
            pending_msg     = "Decompressing pages...",
            finished_msg    = f"Pages uncompressed successfully | Compression ratio: ~{compressed_len / uncompressed_len * 100:.2f}% | {compressed_len} B -> {uncompressed_len} B",
            current_status  = idx,
            finished_status = len(compressed_pages)
        )

    # Join all the content in the pages into a single structure
    content = b"".join(pages)

    # Find a valid path inside a USB to store the received file
    # NOTE: If no USB mount point is found then the file is stored in memory
    usb_mount_path = get_usb_mount_path()
    file_path      = None

    if usb_mount_path:
        file_path = usb_mount_path / "received_file.txt"

    if not file_path:
        file_path = Path("received_file.txt")
        WARN(f"File candidate not found, using fallback file: {file_path}")

    else:
        INFO(f"Stored received file in: {file_path}")

    file_path.write_bytes(content)

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::









# TODO: add new message PREPARE_CHECKSUM before sending EMTPY messages so that we do not send each burst two times :P
# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_RX_MODE(prx: CustomNRF24) -> None:
    STREAM = RX_LINK_LAYER(prx)
    compressed_pages = RX_TRANSPORT_LAYER(STREAM)
    RX_PRESENTATION_LAYER(compressed_pages)

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
