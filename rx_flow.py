# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
import hashlib
import zlib
import time

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
def generate_STREAM_structure_based_on_TR_INFO_message(TR_INFO: bytes, STREAM: list[list[list[bytes]]]) -> None:
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
    
    return

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

    CHECKSUM     = 0
    BURST_HASHER = hashlib.sha256()
    
    prx.ack_payload(RF24_RX_ADDR.P1, b"")
    while not TRANSFER_HAS_ENDED:
        # If we have not received anything we do nothing
        while not prx.data_ready(): continue

        # Pull the received frame from the FIFO
        frame = prx.get_payload()

        # NOTE: If the first Byte has the format 11110000 then it is a TR_INFO message.
        # After we have received this type of message we generate the emtpy STREAM
        # structure with all the allocated slots where we will store each reaceived DATA
        # message. We only generate the structure once, meaning we discard any other
        # TR_INFO that we may get by error
        if frame[0] == 0xF0:
            if STREAM_HAS_BEEN_GENERATED: continue

            generate_STREAM_structure_based_on_TR_INFO_message(frame, STREAM)
            STREAM_HAS_BEEN_GENERATED = True
        

        # NOTE: If the first Byte has the format 0000XXXX then it is a DATA message. We
        # check if it was a retransmision by comparing the PageID, BurstID and ChunkID
        # with the last received ones. We do not expect the TRX to send the frames
        # unordered even if there are lost ACKs, because the TRX will send the next frame
        # only if the ACK has been received.
        # TODO: Add some guard checking in case there are errors in the header uncatched
        # by the CRC
        elif not frame[0] & 0xF0:
            # NOTE: We set the ACK payload to be emtpy to maximize throughput
            prx.ack_payload(RF24_RX_ADDR.P1, b"")

            PageID  = frame[0]
            BurstID = frame[1]
            ChunkID = frame[2]

            # If the header information is invalid we discard the frame
            if (
                PageID  >= len(STREAM)
            or  BurstID >= len(STREAM[PageID])
            or  ChunkID >= len(STREAM[PageID][BurstID])
            ):
                WARN(f"Invalid header information received: ({PageID}/{BurstID}/{ChunkID})")
                continue

            # If it is a retransmission we ignore the frame
            if (
                PageID  == LAST_PAGEID
            and BurstID == LAST_BURSTID
            and ChunkID == LAST_CHUNKID    
            ): continue
            
            # If this is the start of a new Burst, we reset the hasher
            if ChunkID == 0:
                BURST_HASHER = hashlib.sha256()

            
            status_bar(f"Receiving DATA ({PageID}/{BurstID}/{ChunkID})", "INFO")
            STREAM[PageID][BurstID][ChunkID] = frame

            BURST_HASHER.update(frame)

            LAST_PAGEID  = PageID
            LAST_BURSTID = BurstID
            LAST_CHUNKID = ChunkID
        

        # NOTE: If the first Byte has the format 11110011 then it is an EMPTY message.
        # This message is used to notify the PRX that the current Burst has finished and
        # to set the ACK payload to be the checksum of the Burst. The TRX will decide
        # which Burst to send after receiving the checksum
        elif frame == b"\xF2" * 32 or frame == b"\xF3" * 32:
            CHECKSUM = BURST_HASHER.digest()
            prx.ack_payload(RF24_RX_ADDR.P1, CHECKSUM)
            status_bar(f"Sending checksum ({LAST_PAGEID}/{LAST_BURSTID}): {CHECKSUM.hex()}", "SUCC")
        
        # NOTE: If the first Byte has the format 11111010 then it is a TR_FINISH message.
        # This message notifies the PRX that the transmission has ended and no more data
        # will be sent.
        elif frame == b"\xFA" * 32:
            TRANSFER_HAS_ENDED = True
            SUCC(f"Transfer has finished successfully")

            with open("STREAM.txt", "w") as f:
                for PageID, page in enumerate(STREAM):
                    f.write(f"PAGE {PageID}:\n")
                    for BurstID, burst in enumerate(page):
                        f.write(f"    BURST {BurstID}:\n")
                        for ChunkID, chunk in enumerate(burst):
                            f.write(f"        CHUNK {ChunkID:03d}: {chunk.hex()}\n")
                    f.write("\n")     

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
                compressed_page += STREAM[PageID][BurstID][ChunkID][2:] # NOTE: We ignore the first 3 Bytes as they are the headers
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
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
