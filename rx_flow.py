# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
import hashlib
import zlib

from radio import CustomNRF24

from utils import (
    get_usb_mount_path,

    status_bar,
    progress_bar,

    SUCC,
    WARN,
    INFO,
)

from nrf24 import (
    RF24_RX_ADDR
)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: PROTOCOL LAYERS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def generate_STREAM_structure_based_on_TR_INFO_message(TR_INFO: bytes, STREAM: list[list[list[bytes]]]) -> tuple[list[int], list[int], list[int]]:
    """
    Allocate all the slots of the STREAM structure to fill them up later with DATA
    messages. We use the information contained in the TR_INFO message to do so
    """
    # NOTE: The structure of our DATA payload looks like this (for now):
    #
    # ┌────────────────────┬───────────────┬────────────────────┬────────────────────┬─────┬────────────────┬─────────────────────┬─────────────────────┐
    # │ MessageID + InfoID │ Page0 N Burst │ Page0 L Last Burst │ Page0 L Last Chunk | ... | Page10 N Burst │ Page10 L Last Burst │ Page10 L Last Chunk │
    # └────────────────────┴───────────────┴────────────────────┴────────────────────┴─────┴────────────────┴─────────────────────┴─────────────────────┘
    #   ↑           ↑        ↑               ↑                    ↑
    #   │           │        │               │                    1B: The length of the last Chunk of the last Burst: [0 - 255]
    #   │           │        │               1B: The number of Chunks in the last Burst: [0 - 255]
    #   │           │        1B: The number of Bursts in the page: [0 - 255]
    #   │           4b: Identifies the type of INFO message that we are sending: [0 - 15], for TR_INFO is set to 0000
    #   4b: Identifies the kind of message that we are sending, for INFO payload is set to 1111
    MESSAGE           = TR_INFO[1:]
    number_of_pages   = len(MESSAGE) // 3
    burst_in_page     = [byte for byte in MESSAGE[0::3]]
    length_last_burst = [byte for byte in MESSAGE[1::3]]
    length_last_chunk = [byte for byte in MESSAGE[2::3]]

    for PageID in range(number_of_pages):
        INFO(f"Page {PageID}: {burst_in_page[PageID]} Bursts | {length_last_burst[PageID]} CLB | {length_last_chunk[PageID]} BLC")

        STREAM.append(list())
        for BurstID in range(burst_in_page[PageID]):
            STREAM[PageID].append(list())

            if BurstID == burst_in_page[PageID] - 1:
                chunks_count = length_last_burst[PageID]
            else:
                chunks_count = 256

            for ChunkID in range(chunks_count):
                STREAM[PageID][BurstID].append(bytes())
    return (burst_in_page, length_last_burst, length_last_chunk)

def RX_LINK_LAYER(prx: CustomNRF24) -> None:
    """
    This layer is responsible for the following things:
    - Generating a unified structure containing the data of the transmission orderer
    by page, burst and chunk
    - Reading a TX_INFO message and set up the expected number of pages
    - Reading a PAGE_INFO message
    - Reading a BURST_INFO message
    - Receiving each frame
    - Compute and send the checksum at the end of the burst
    """
    # Generate the unified structure where we are goint to store the bytes categorized
    # by Page, Burst and Chunk.
    #
    # NOTE: The structure of our DATA payload looks like this (for now):
    #
    # ┌────────────────────┬─────────┬─────────┬──────┐
    # │ MessageID + PageID │ BurstID │ ChunkID │ DATA │
    # └────────────────────┴─────────┴─────────┴──────┘
    #   ↑           ↑        ↑         ↑         ↑
    #   │           │        │         │         29B: The data to be sent, either if it is part of a file or data from an info message
    #   │           │        │         1B: Identifies a chunk inside a burst, starts from 0 at every Burst: [0 - 255]
    #   │           │        1B: Identifies a Burst inside a Page, starts from 0 at every Page: [0 - 255]
    #   │           4b: Identifies a Page inside a Transfer: [0 - 15]
    #   4b: Identifies the kind of message that we are sending, for DATA payload is set to 0000
    #
    # NOTE: The unified structure of the Transfer looks like this:
    # NOTE: N ≤ 255
    # NOTE: L ≤ 15
    #
    # PAGE 0:
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
    # PAGE 1:
    #     BURST 0:
    #         ...
    #     BURST 1:
    #         ...
    #     ...
    #     BURST N:
    #         ...
    # ...
    # PAGE L:
    #     BURST 0:
    #         ...
    #     BURST 1:
    #         ...
    #     ...
    #     BURST N:
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
    TRANSFER_HAS_ENDED        = False
    STREAM_HAS_BEEN_GENERATED = False
    
    LAST_PAGEID               = 0
    LAST_BURSTID              = 0
    LAST_CHUNKID              = 0

    BURSTS_PER_PAGE           = []
    LAST_BURST_WIDTH_PER_PAGE = []
    LAST_CHUNK_WIDTH_PER_PAGE = []

    CHECKSUM                  = 0
    burst_hasher              = hashlib.sha256()
    while not TRANSFER_HAS_ENDED:
        # If we have not received anything we do nothing
        while not prx.data_ready():
            continue

        prx.ack_payload(RF24_RX_ADDR.P1, b"")
        # If we have received something we pull it from the FIFO and analyze the first
        # Byte to check what type of message we have received
        #
        # NOTE: If the byte has the format 1111XXXX then it is an INFO message and we need
        # to read the next 4 bits to identify the type of INFO message:
        # 11110000: Corresponds to TR_INFO
        # 11110001: Corresponds to EMPTY
        #
        # NOTE: If the byte has the format 0000XXXX then it is a DATA message and the
        # first 3 Bytes correspond to the PageID, BurstID and ChunkID
        frame = prx.get_payload()

        # NOTE: INFO message (1111XXXX)
        if frame[0] & 0xF0:

           # NOTE: TR_INFO (11110000)
            if frame[0] == 0xF0:
                if STREAM_HAS_BEEN_GENERATED:
                    continue

                BURSTS_PER_PAGE, LAST_BURST_WIDTH_PER_PAGE, LAST_CHUNK_WIDTH_PER_PAGE = generate_STREAM_structure_based_on_TR_INFO_message(frame, STREAM)
                STREAM_HAS_BEEN_GENERATED = True
            
            elif frame[0] == 0xF3:
                CHECKSUM = burst_hasher.digest()
                status_bar(f"Sending checksum ({LAST_PAGEID}/{LAST_BURSTID}): {CHECKSUM.hex()}", "SUCC")
                prx.ack_payload(RF24_RX_ADDR.P1, CHECKSUM)
            
            # NOTE: TR_FINISH (11111010)
            elif frame[0] == 0xFA:
                TRANSFER_HAS_ENDED = True
                SUCC(f"Transfer has finished successfully")

        
        # NOTE: DATA message (0000XXXX)
        else:
            PageID  = frame[0]
            BurstID = frame[1]
            ChunkID = frame[2]

            if (
                PageID  == LAST_PAGEID
            and BurstID == LAST_BURSTID
            and ChunkID == LAST_CHUNKID    
            ):
                continue

            if (
                (PageID > 10 - 1)
            or  (BurstID > 256 - 1)
            or  (BurstID > BURSTS_PER_PAGE[PageID] - 1)
            or  (ChunkID > 256 - 1)
            or  (ChunkID > LAST_CHUNK_WIDTH_PER_PAGE[PageID] - 1) and (BurstID == BURSTS_PER_PAGE[PageID] - 1)
            ):
                continue
            
            if (
                PageID  != LAST_PAGEID
            or  BurstID != LAST_BURSTID
            or  ChunkID < LAST_CHUNKID
            ):
                burst_hasher = hashlib.sha256()

            
            status_bar(f"Receiving DATA ({PageID}/{BurstID}/{ChunkID})", "INFO")
            STREAM[PageID][BurstID][ChunkID] = frame
            burst_hasher.update(frame)

            LAST_PAGEID  = PageID
            LAST_BURSTID = BurstID
            LAST_CHUNKID = ChunkID

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
        compressed_page = []
        for BurstID in range(len(STREAM[PageID])):
            for ChunkID in range(len(STREAM[PageID][BurstID])):
                compressed_page.extend(STREAM[PageID][BurstID][ChunkID][2:]) # NOTE: We ignore the first 3 Bytes as they are the headers
        compressed_pages.append(compressed_page)
    
    return compressed_page



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
    decompressor = zlib.decompressobj(level = 6)
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










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_RX_MODE(prx: CustomNRF24) -> None:
    STREAM = RX_LINK_LAYER(prx)
    compressed_pages = RX_TRANSPORT_LAYER(STREAM)
    RX_PRESENTATION_LAYER(compressed_pages)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
