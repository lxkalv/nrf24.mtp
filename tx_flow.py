# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
from math import ceil
import hashlib
import zlib

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
NUMBER_OF_PAGES = 10
BURST_WIDTH     = 7905
CHUNK_WIDTH     = 31
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
    """
    This layer is responsible for the following things:
    - Generate and send a TRANSFER_INFO message at the start of the communication to
    provide the PRX with the necessary information
    - Send all the DATA inside the STREAM structure in an ordered manner
    - Verify the CHECKSUM of each BURST to ensure data integrity
    """

    # Generate and send the TRANSFER_INFO message
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
    TRANSFER_INFO  = bytes()
    TRANSFER_INFO += 0xF0.to_bytes(1) # NOTE: Translates to 11110000
    for PageID in range(len(STREAM)):
        TRANSFER_INFO += len(STREAM[PageID]).to_bytes(1)
        TRANSFER_INFO += len(STREAM[PageID][-1]).to_bytes(1)
        TRANSFER_INFO += len(STREAM[PageID][-1][-1]).to_bytes(1)
    PTX.send_CONTROL_message(TRANSFER_INFO, "TRANSFER_INFO")

    # Send all the DATA inside the STREAM structure in an ordered manner
    PageID = 0
    while PageID < len(STREAM):
        BurstID = 0
        while BurstID < len(STREAM[PageID]):
            ChunkID = 0
            INFO(f"Sending BURST {BurstID} expected CHECKSUM: {CHECKSUMS[PageID][BurstID].hex()}")

            while ChunkID < len(STREAM[PageID][BurstID]):
                PTX.send_DATA_message(STREAM[PageID][BurstID][ChunkID], PageID, BurstID, ChunkID)
                # INFO(f"Burst sent {STREAM[PageID][BurstID][ChunkID].hex()}")
                ChunkID += 1
            # NOTE: After we have completed sending a BURST, we send empty frames until we
            # receive a valid CHECKSUM in the auto-ACK of the PRX
            while True:
                # status_bar(f"Waiting for CHECKSUM: {BurstID} | {CHECKSUMS[PageID][BurstID].hex()}", "INFO")

                # Generate and send an EMPTY_FRAME message to trigger the auto-ACK with the
                # checksum
                #
                # NOTE: The structure of our EMPTY message looks like this:
                #
                # ┌────────────────────┬────┬────┬─────┬────┐
                # │ MessageID + InfoID │ F3 │ F3 │ ... │ F3 │ = 1B + 31B = 32B
                # └────────────────────┴────┴────┴─────┴────┘
                #   ↑           ↑
                #   │           4b: Identifies the type of CONTROL message that we are sending: "0011" for EMPTY
                #   4b: Identifies the kind of message that we are sending: "1111" for CONTROL message
                EMPTY = 0xF3.to_bytes(1)

                PTX.send_CONTROL_message(EMPTY, "EMPTY", progress = False)
                while not PTX.data_ready(): continue
                ACK = PTX.get_payload()
                
                if len(ACK) < 32: continue

                if ACK == CHECKSUMS[PageID][BurstID]:
                    status_bar(f"Received VALID checksum for ({PageID}/{BurstID}): {ACK.hex()}", "SUCC")
                    BurstID += 1

                else:
                    Inv_checksums[0] += 1
                    status_bar(f"Received INVALID checksum for ({PageID}/{BurstID}): {ACK.hex()}", "ERROR")
                break
        
        PageID += 1
                    


    # Generate and send a TRANSFER_FINISH message to signal the end of the
    # communication
    #
    # NOTE: The structure of our TRANSFER_FINISH payload looks like this:
    #
    # ┌────────────────────┬────┬────┬─────┬────┐
    # │ MessageID + InfoID │ FA │ FA │ ... │ FA │ = 1B + 31B = 32B
    # └────────────────────┴────┴────┴─────┴────┘
    #   ↑           ↑
    #   │           4b: Identifies the type of CONTROL message that we are sending: "1010" for TRANSFER_FINISH
    #   4b: Identifies the kind of message that we are sending: "1111" for CONTROL message
    TRANSFER_FINISH = 0xFA.to_bytes(1)
    PTX.send_CONTROL_message(TRANSFER_FINISH, "TRANSFER_FINISH")
    INFO(f"Number of invalid checksums {Inv_checksums[0]}")
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_TX_MODE(ptx: CustomNRF24) -> None:
    compressed_pages  = TX_PRESENTATION_LAYER()
    STREAM, CHECKSUMS = TX_TRANSPORT_LAYER(compressed_pages)

    with open("STREAM.txt", "w") as f:
        for PageID, PAGE in enumerate(STREAM):
            f.write(f"PAGE {PageID:02d}:\n")
            for BurstID, BURST in enumerate(PAGE):
                f.write(f"    BURST {BurstID:03d}:\n")
                for ChunkID, CHUNK in enumerate(BURST):
                    f.write(f"        CHUNK {ChunkID:03d}: {CHUNK.hex()}\n")

    return
    TX_LINK_LAYER(ptx, STREAM, CHECKSUMS)

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
