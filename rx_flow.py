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


MAX_PAYLOAD = 32






# :::: PROTOCOL LAYERS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def generate_STREAM_based_on_TRANSFER_INFO(TRANSFER_INFO: bytes, STREAM: list[list[list[bytes]]], SIZES: list[list[list[bytes]]]) -> None:
    """
    Allocate all the slots of the STREAM structure to fill them up later with DATA
    messages. We use the information contained in the TRANSFER_INFO message to do so
    """
    # Allocate the slots in the STREAM structure based on the information contained in
    # the TRANSFER_INFO message
    #
    # NOTE: The structure of our TRANSFER_INFO message looks like this:
    #
    # ┌───────────────────────┬───────────────┬────────────────────┬────────────────────┬─────┬────────────────┬─────────────────────┬─────────────────────┐
    # │ MessageID + ControlID │ Page0 N Burst │ Page0 L Last Burst │ Page0 L Last Chunk | ... | Page10 N Burst │ Page10 L Last Burst │ Page10 L Last Chunk │ = 1B + (N Pages * 3B)
    # └───────────────────────┴───────────────┴────────────────────┴────────────────────┴─────┴────────────────┴─────────────────────┴─────────────────────┘
    #   ↑           ↑           ↑               ↑                    ↑
    #   │           │           │               │                    1B: The length of the last Chunk of the last Burst: [0 - 255]
    #   │           │           │               1B: The number of Chunks in the last Burst: [0 - 255]
    #   │           │           1B: The number of Bursts in the page: [0 - 255]
    #   │           4b: Identifies the type of CONTROL message that we are sending: "0000" for TRANSFER_INFO
    #   4b: Identifies the kind of message that we are sending: "1111" for CONTROL message
    CONTENT           = TRANSFER_INFO[1:]
    number_of_pages   = len(CONTENT) // 3
    burst_in_page     = [byte for byte in CONTENT[0::3]]
    length_last_burst = [byte for byte in CONTENT[1::3]]
    length_last_chunk = [byte for byte in CONTENT[2::3]]
    for PageID in range(number_of_pages):
        INFO(f"PAGE {PageID}: {burst_in_page[PageID]} BURSTS | {length_last_burst[PageID]} CLB | {length_last_chunk[PageID]} BLC")

        STREAM.append(list())
        SIZES.append(list())
        for BurstID in range(burst_in_page[PageID]):
            STREAM[PageID].append(list())
            SIZES[PageID].append(list())

            if BurstID == burst_in_page[PageID] - 1:
                chunks_count = length_last_burst[PageID]
            else:
                chunks_count = 256

            for ChunkID in range(chunks_count):
                STREAM[PageID][BurstID].append(bytes())
                if (
                    BurstID == burst_in_page[PageID] - 1
                and ChunkID == chunks_count - 1
                ):
                    SIZES[PageID][BurstID].append(bytes(length_last_chunk[PageID]))
                else:
                    SIZES[PageID][BurstID].append(bytes(32))
    
    return

def RX_LINK_LAYER(PRX: CustomNRF24) -> None:
    """
    This layer is responsible for the following things:
    - Generate the STREAM structure containing all the DATA of the communication
    ordered by PAGE, BURST and CHUNK
    - Compute and send the CHECKSUM at the end of each BURST
    """
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
    SIZES:  list[     list[     list[      bytes]]] = list()

    # NOTE: The flow of the PRX is as follows, we keep receiving frames until the
    # transmission has finished. We do not care about the order of the frames as we
    # assume that the PTX will take care of that. We treat each frame differently
    # depending on the information inside the frame itself. After each received frame,
    # we evaluate if the transmission has ended or not
    TRANSFER_HAS_ENDED        = False
    STREAM_HAS_BEEN_GENERATED = False
    
    LAST_PAGEID  = None
    LAST_BURSTID = None
    LAST_CHUNKID = None

    THROUGHPUT_TIC = 0
    THROUGHPUT_TAC = 0

    BURST_HASHER = hashlib.sha256()
    num_chunks =0
    once = False
    while not TRANSFER_HAS_ENDED:
        # If we have not received anything we do nothing
        while not PRX.data_ready(): continue

        # Pull the received frame from the FIFO
        frame = PRX.get_payload()

        # NOTE: If the first Byte has the format 11110000 then it is a TRANSFER_INFO
        # message. After we have received this type of message we generate the emtpy
        # STREAM structure with all the allocated slots where we will store each
        # received DATA message. We only generate the structure once, meaning we discard
        # any other TRANSFER_INFO that we may get by error
        if frame[0] == 0xF0:
            if STREAM_HAS_BEEN_GENERATED: continue

            generate_STREAM_based_on_TRANSFER_INFO(frame, STREAM, SIZES)
            STREAM_HAS_BEEN_GENERATED = True
            THROUGHPUT_TIC = time.time()
        

        # NOTE: If the first Byte has the format 0000XXXX then it is a DATA message. We
        # check if it was a retransmision by comparing the PageID, BurstID and ChunkID
        # with the last received ones. We do not expect the TRX to send the frames
        # unordered even if there are lost ACKs, because the TRX will send the next frame
        # only if the ACK has been received.
        #
        # NOTE: The structure of our DATA message looks like this:
                #
                # ┌────────────────────┬─────────┬─────────┬──────┐
                # │ MessageID + PageID │ BurstID │ ChunkID │ DATA │
                # └────────────────────┴─────────┴─────────┴──────┘
                #   ↑           ↑        ↑         ↑         ↑
                #   │           │        │         │         29B: The data to be sent
                #   │           │        │         1B: Identifies a CHUNK inside a BURST, starts from 0 at every BURST: [0 - 255]
                #   │           │        1B: Identifies a BURST inside a PAGE, starts from 0 at every PAGE: [0 - 255]
                #   │           4b: Identifies a PAGE inside a TRANSFER: [0 - 15]
                #   4b: Identifies the kind of message that we are sending: "0000" for DATA messages
        elif (frame[0] & 0xF0) == 0x00:
            if once == False:
                PRX.ack_payload(RF24_RX_ADDR.P1, b"")
                once = True

            # NOTE: We set the ACK payload to be empty to maximize throughput
            PageID  = frame[0]
            BurstID = frame[1]
            ChunkID = frame[2]

            # If the header information is invalid we discard the frame
            if (
                PageID  >= len(SIZES)
            or  BurstID >= len(SIZES[PageID])
            or  ChunkID >= len(SIZES[PageID][BurstID])
            or len(frame[3:]) != len(SIZES[PageID][BurstID][ChunkID])
            ):
                WARN(f"Invalid header information received: {PageID:02d}|{BurstID:03d}|{ChunkID:03d} -> {len(frame[3:])} B")
                continue
            
            # If it is a retransmission we ignore the frame
            if (
                PageID  == LAST_PAGEID
            and BurstID == LAST_BURSTID
            and ChunkID == LAST_CHUNKID    
            ): continue

            STREAM[PageID][BurstID][ChunkID] = bytes(frame)

            LAST_PAGEID  = PageID
            LAST_BURSTID = BurstID
            LAST_CHUNKID = ChunkID

            BURST_HASHER.update(frame)

            if ChunkID == len(STREAM[PageID][BurstID]) - 1:
                SUCC(f"Completed BURST: {PageID:02d}|{BurstID:03d}")
                BURST_HASHER = hashlib.sha256()
                num_chunks = 0
                for ChunkID, chunk in enumerate(STREAM[PageID][BurstID]):
                    num_chunks += num_chunks
                    if not chunk:
                        WARN(f"Missing {ChunkID:03d} in BURST")
                    BURST_HASHER.update(chunk)
                CHECKSUM = BURST_HASHER.digest()
                PRX.ack_payload(RF24_RX_ADDR.P1, CHECKSUM)
                
                INFO(f"Total used chunks {num_chunks}")
                
            

        # NOTE: If the first Byte has the format 11110011 then it is an EMPTY message.
        # This message is used to notify the PRX that the current Burst has finished and
        # to set the ACK payload to be the checksum of the Burst. The TRX will decide
        # which Burst to send after receiving the checksum
        elif frame[0] == 0xF3:
            once = False
            PRX.ack_payload(RF24_RX_ADDR.P1, CHECKSUM)
            SUCC(f"Sending checksum {LAST_PAGEID:02d}|{LAST_BURSTID:03d}: {CHECKSUM.hex()}")
        
        # NOTE: If the first Byte has the format 11111010 then it is a TR_FINISH message.
        # This message notifies the PRX that the transmission has ended and no more data
        # will be sent.
        elif frame[0] == 0xFA:
            TRANSFER_HAS_ENDED = True
            SUCC(f"Transfer has finished successfully")
            THROUGHPUT_TAC = time.time()

            tx_time = THROUGHPUT_TAC - THROUGHPUT_TIC
            tx_data = sum(
                len(chunk)
                for page in STREAM
                for burst in page
                for chunk in burst
            )

    INFO(f"Computed throughput: {tx_data / tx_time / 1024:.2f} KBps over {tx_time:.2f} seconds | {tx_data / 1024:.2f} KB transferred")
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
