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

# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::







# :::: PROTOCOL LAYERS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def TX_PRESENTATION_LAYER() -> list[bytes]:
    """
    This layer is responsible for for the following things:
    - Finding a candidate file to transmit, either by looking inside a USB or by
    looking in a directory with test files as a fallback
    - Reading the contents of the file and splitting it into pages
    - Compressing each page 
    - Providing the compressed pages to the next layer
    """
    
    # Find a valid txt file from a USB mount point.
    # NOTE: If no USB mount point is found or if no valid file is found inside the
    # USB, then a test file is taken from the "test_files" directory
    test_files_dir = Path("test_files")
    usb_mount_path = get_usb_mount_path()
    file_path      = None

    if usb_mount_path:
        file_path = find_valid_txt_file_in_usb(usb_mount_path)

    if not file_path:
        file_path = test_files_dir / "lorem.txt"  
        WARN(f"File candidate not found, using fallback file: {file_path}")
    
    else:
        INFO(f"Selected file candidate: {file_path}")


    # Read the raw bytes from the file and split them into pages
    # NOTE: The size and number of the pages is completely arbitrary for now, we will
    # split the file into 10 pages of undefined size
    content = file_path.read_bytes()
    
    content_len = len(content)
    page_len    = ceil(content_len / 10)
    pages       = [
        content[i : i + page_len]
        for i in range(0, content_len, page_len)
    ]
    INFO(f"Splitted file into 10 pages of {page_len} Bytes (last page may be shorter)")


    # Compress each page
    # TODO: Find the best compression mechanism that we can. Compression is a very
    # critical aspect in this project given that the achieved throughput is very low
    # and that we expect interferences in the link
    compressed_pages: list[bytes] = []
    compressor = zlib.compressobj(level = 6)
    for idx, page in enumerate(pages, 1):
        compressed_page  = compressor.compress(page)
        compressed_page += compressor.flush(zlib.Z_SYNC_FLUSH)
        compressed_pages.append(compressed_page)

        compressed_len = sum(len(compressed_page) for compressed_page in compressed_pages)
        progress_bar(
            pending_msg     = "Compressing pages...",
            finished_msg    = f"Pages compressed successfully | Compression ratio: ~{compressed_len / content_len * 100:.2f}% | {content_len} B -> {compressed_len} B",
            current_status  = idx,
            finished_status = len(pages)
        )
    
    # Provide the pages to the next layer
    return compressed_pages





def TX_TRANSPORT_LAYER(pages: list[bytes]) -> tuple[list[list[list[bytes]]], list[list[bytes]]]:
    """
    This layer is responsible for the following things:
    - Splitting the compressed pages into bursts of 7936 Bytes. This is done so that
    later we can split each burst into chunks of 31 Bytes + 1 Byte of ChunkID (IDc)
    - Compute and store the checksum of each burst so that we can verify the integrity
    of the data after sending it
    - Providing a unified structure containing the bytes to be transmitted in an
    ordered and hierarchical way
    - Providing a unified structure containing the checksums of each burst ordered by
    PageID and BurstID
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

    # NOTE: We provide a STREAM-like structure that contains the checksums of each
    # burst for every page. The structure looks like this
    #
    # PAGE 0:
    #     BURST 0: str[ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb]
    #     BURST 1: str[ea325d761f98c6b73320e442b67f2a3574d9924716d788ddc0dbbdcaca853fe7]
    #     BURST 2: str[d26cd84ddae9829c5a1053fce8e1c1d969086940e58c56d65d27989b6b46bba2]
    #     ...
    #     BURST N: str[1694f1a2500bf2aa881c461c92655b3621cae5bbf70b4177d02a2aa92c1aa903]
    # PAGE 1:
    #     BURST 0: ...
    #     BURST 1: ...
    #     ...
    #     BURST N: ...
    # ...
    # PAGE L:
    #     BURST 0: ...
    #     BURST 1: ...
    #     ...
    #     BURST N: ...
    #
    # NOTE: As of now, the checksum is computed INCLUDING all the headers
    #          PageID    BurstID    CHECKSUM
    #          ↓         ↓          ↓
    CHECKSUMS: list[     list[      bytes]] = list()

    # Split each compressed Page into Bursts of 7424B
    # NOTE: The width of 7424B allows to split the Burst into 256 Chunks of 29
    # Bytes of width, this allows to limit the size of the ChunkID to 1B, summing up
    # to a total payload of 32B (after adding the PageID and the BurstID). The ChunkID
    # is set to 0 at the start of each Burst
    BURST_WIDTH = 7424
    CHUNK_WIDTH = 29
    for PageID, page in enumerate(pages):

        STREAM.append(list())
        CHECKSUMS.append(list())

        page_len = len(page)
        
        # split the Page into Bursts
        bursts = [
            page[i : i + BURST_WIDTH]
            for i in range(0, page_len, BURST_WIDTH)
        ]

        for BurstID, burst in enumerate(bursts):

            STREAM[PageID].append(list())
            CHECKSUMS[PageID].append("")
            
            burst_len = len(burst)

            # split the Burst into Chunks
            chunks = [
                burst[i : i + CHUNK_WIDTH]
                for i in range(0, burst_len, CHUNK_WIDTH)
            ]

            burst_hasher = hashlib.sha256()
            for ChunkID, chunk in enumerate(chunks):

                STREAM[PageID][BurstID].append(bytes())
                STREAM[PageID][BurstID][ChunkID] += PageID.to_bytes(1) # NOTE: as there are 10 pages, converting the PageID to bytes directly is correct because the first 4 bits will be set to 0
                STREAM[PageID][BurstID][ChunkID] += BurstID.to_bytes(1)
                STREAM[PageID][BurstID][ChunkID] += ChunkID.to_bytes(1)
                STREAM[PageID][BurstID][ChunkID] += chunk
                
                burst_hasher.update(STREAM[PageID][BurstID][ChunkID])

            CHECKSUMS[PageID][BurstID] = burst_hasher.digest()

    # TODO: probably some information prints would be useful but I cannot come up with
    # something clean right now
    return (STREAM, CHECKSUMS)





def TX_LINK_LAYER(ptx: CustomNRF24, STREAM: list[list[list[bytes]]], CHECKSUMS: list[list[str]]) -> None:
    """
    This layer is responsible for the following things:
    - Generating and sending a TX_INFO message containing the number of pages to
    expect in the transmission, and possibly some other things
    - Generating a PAGE_INFO message at the start of each page containing the PageID
    and the PageLength
    - Generating a BURST_INFO message at the start of each burst containing the
    BurstID and the BurstLength
    - Sending each chunk inside a burst
    - Validating the checksum received by the PRX in order to decide wether to resend
    the burst or move on to the next
    """

    # Generate and send the TRANSFER_INFO message
    #
    # NOTE: The structure of our TRANSFER_INFO payload looks like this:
    # ┌───────────────────────┬───────────────┬────────────────────┬────────────────────┬─────┬────────────────┬─────────────────────┬─────────────────────┐
    # │ MessageID + ControlID │ Page0 N Burst │ Page0 L Last Burst │ Page0 L Last Chunk | ... | Page10 N Burst │ Page10 L Last Burst │ Page10 L Last Chunk │ = 1B + (N Pages * 3B)
    # └───────────────────────┴───────────────┴────────────────────┴────────────────────┴─────┴────────────────┴─────────────────────┴─────────────────────┘
    #   ↑           ↑           ↑               ↑                    ↑
    #   │           │           │               │                    1B: The length of the last Chunk of the last Burst: [0 - 255]
    #   │           │           │               1B: The number of Chunks in the last Burst: [0 - 255]
    #   │           │           1B: The number of Bursts in the page: [0 - 255]
    #   │           4b: Identifies the type of CONTROL message that we are sending: "0000" for TRANSFER_INFO
    #   4b: Identifies the kind of message that we are sending: "1111" for CONTROL message
    TRANSFER_INFO  = bytes()
    TRANSFER_INFO += 0xF0.to_bytes(1) # NOTE: Translates to 11110000
    for PageID in range(len(STREAM)):
        TRANSFER_INFO += len(STREAM[PageID]).to_bytes(1)
        TRANSFER_INFO += len(STREAM[PageID][-1]).to_bytes(1)
        TRANSFER_INFO += len(STREAM[PageID][-1][-1]).to_bytes(1)
    ptx.send_CONTROL_message(TRANSFER_INFO, "TRANSFER_INFO")

    PageID  = 0
    while PageID < len(STREAM):
        BurstID = 0
        while BurstID < len(STREAM[PageID]):
            ChunkID = 0
            while ChunkID < len(STREAM[PageID][BurstID]):
                packets_lost = 0

                while True:
                    status_bar(f"Sending frame ({PageID}/{BurstID}/{ChunkID}) | retries = {packets_lost}", "INFO")

                    ptx.reset_packages_lost()
                    ptx.send(STREAM[PageID][BurstID][ChunkID])

                    try:
                        ptx.wait_until_sent()
                    
                    except TimeoutError:
                        ERROR("Time-out while transmitting")

                    if ptx.get_packages_lost() == 0:
                        ChunkID += 1
                        break
                    
                    else:
                        packets_lost += 1

            # NOTE: After we have completed sending a Burst, we send empty frames until we
            # receive a checksum in the auto-ACK of the PRX
            while True:
                status_bar(f"Waiting for checksum of Burst {BurstID}, expecting {CHECKSUMS[PageID][BurstID].hex()}", "INFO")

                # Generate a LOAD_CHECKSUM message to signal the PRX to load the checksum into
                # the auto-ACK payload
                #
                # NOTE: The structure of our LOAD_CHECKSUM payload looks like this:
                # ┌────────────────────┬────┬────┬─────┬────┐
                # │ MessageID + InfoID │ F2 │ F2 │ ... │ F2 │ = 1B + 31B = 32B
                # └────────────────────┴────┴────┴─────┴────┘
                #   ↑           ↑
                #   │           4b: Identifies the type of INFO message that we are sending: [0 - 15], for LOAD_CHECKSUM it is set to 0010
                #   4b: Identifies the kind of message that we are sending, for CONTROL payload it is set to 1111
                LOAD_CHECKSUM  = bytes()
                for _ in range(32):
                    LOAD_CHECKSUM += 0xF2.to_bytes(1) # NOTE: Translates to 11110010
                ptx.send_CONTROL_message(LOAD_CHECKSUM, "LOAD_CHECKSUM", progress = False)

                # Generate and send an EMPTY_FRAME message to trigger the auto-ACK with the
                # checksum
                #
                # NOTE: The structure of our EMPTY payload looks like this:
                # ┌────────────────────┬────┬────┬─────┬────┐
                # │ MessageID + InfoID │ F3 │ F3 │ ... │ F3 │ = 1B + 31B = 32B
                # └────────────────────┴────┴────┴─────┴────┘
                #   ↑           ↑
                #   │           4b: Identifies the type of CONTROL message that we are sending: [0 - 15], for EMPTY it is set to 0011
                #   4b: Identifies the kind of message that we are sending, for CONTROL payload is set to 1111
                EMPTY  = bytes()
                for _ in range(32):
                    EMPTY += 0xF3.to_bytes(1) # NOTE: Translates to 11110011
                ptx.send_CONTROL_message(EMPTY, "EMPTY", progress = False)
                
                ACK = ptx.get_payload()
                
                if len(ACK) < 32: continue

                if ACK == CHECKSUMS[PageID][BurstID]:
                    status_bar(f"Received   VALID checksum for ({PageID}/{BurstID}): {ACK.hex()}", "SUCC")

                    BurstID += 1

                else:
                    status_bar(f"Received INVALID checksum for ({PageID}/{BurstID}): {ACK.hex()}", "ERROR")

                break
        
        PageID += 1
                    


    # Generate and send a Transfer Finish message (TR_FINISH)
    # NOTE: As of now the TR_FINISH message does not contain any data, only the header
    #
    # NOTE: The structure of our TR_FINISH payload looks like this (for now):
    #
    # ┌────────────────────┬────┬────┬─────┬────┐
    # │ MessageID + InfoID │ FA │ FA │ ... │ FA │ = 1B + 31B = 32B
    # └────────────────────┴────┴────┴─────┴────┘
    #   ↑           ↑
    #   │           4b: Identifies the type of CONTROL message that we are sending: [0 - 15], for TR_INFO it is set to 1010
    #   4b: Identifies the kind of message that we are sending, for INFO payload is set to 1111
    TR_FINISH  = bytes()
    for _ in range(32):
        TR_FINISH += 0xFA.to_bytes(1) # NOTE: Translates to 11111010
    ptx.send_CONTROL_message(TR_FINISH, "TR_FINISH")
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_TX_MODE(ptx: CustomNRF24) -> None:
    compressed_pages  = TX_PRESENTATION_LAYER()
    STREAM, CHECKSUMS = TX_TRANSPORT_LAYER(compressed_pages)
    TX_LINK_LAYER(ptx, STREAM, CHECKSUMS)

    with open("STREAM.txt", "w") as f:
        for PageID, page in enumerate(STREAM):
            f.write(f"PAGE {PageID}:\n")
            for BurstID, burst in enumerate(page):
                f.write(f"    BURST {BurstID}:\n")
                for ChunkID, chunk in enumerate(burst):
                    f.write(f"        CHUNK {ChunkID:03d}: {chunk.hex()}\n")
            f.write("\n")


    with open("CHECKSUMS.txt", "w") as f:
        for PageID, page in enumerate(CHECKSUMS):
            f.write(f"PAGE {PageID}:\n")
            for BurstID, checksum in enumerate(page):
                f.write(f"    BURST {BurstID}: {checksum.hex()}\n")
            f.write("\n")

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
