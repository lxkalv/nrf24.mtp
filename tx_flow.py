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
    status_bar,

    get_usb_mount_path,
    find_valid_txt_file_in_usb,
)
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
            finished_msg    = f"Pages compressed successfully, | Compression ratio: ~{compressed_len / content_len * 100:.2f}% | {content_len} B -> {compressed_len} B",
            current_status  = idx,
            finished_status = len(pages)
        )
    
    # Provide the pages to the next layer
    return compressed_pages





def TX_TRANSPORT_LAYER(pages: list[bytes]) -> tuple[dict[str, dict[str, dict[str, bytes]]], dict[str, dict[str, str]]]:
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
    # by page, burst and chunk.
    # NOTE: The structure looks like this:
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
    #            PageID    BurstID   ChunkID    Payload(ChunkID + Data)
    #            ↓         ↓         ↓          ↓
    STREAM: dict[str, dict[str, dict[str,       bytes]]] = dict()

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
    #     BURST 1: ..
    #     ...
    #     BURST N: ...
    #
    # NOTE: As of now, the checksum is computed INCLUDING the ChunkID
    #               PageID    BurstID    checksum
    #               ↓         ↓          ↓
    CHECKSUMS: dict[str, dict[str,       str]] = dict()

    # Split each compressed page into bursts of 7936 Bytes
    # NOTE: The width of 7936 Bytes allows to split the burst into 256 chunks of 31
    # Bytes of width, this allows to limit the size of the IDc to 1 Byte, summing up
    # to a total payload of 32 Bytes. The IDc is resetted to 0 at each burst
    BURST_WIDTH = 7936
    CHUNK_WIDTH = 31
    for idx_page, page in enumerate(pages):

        STREAM[f"PAGE{idx_page}"]    = dict()
        CHECKSUMS[f"PAGE{idx_page}"] = dict()

        page_len = len(page)
        
        # split the page into bursts
        bursts = [
            page[i : i + BURST_WIDTH]
            for i in range(0, page_len, BURST_WIDTH)
        ]

        for idx_burst, burst in enumerate(bursts):

            STREAM[f"PAGE{idx_page}"][f"BURST{idx_burst}"]    = dict()
            CHECKSUMS[f"PAGE{idx_page}"][f"BURST{idx_burst}"] = ""
            
            burst_len = len(burst)

            # split the burst into chunks
            chunks = [
                burst[i : i + CHUNK_WIDTH]
                for i in range(0, burst_len, CHUNK_WIDTH)
            ]

            burst_hasher = hashlib.sha256()
            for idx_chunk, chunk in enumerate(chunks):

                STREAM[f"PAGE{idx_page}"][f"BURST{idx_burst}"][f"CHUNK{idx_chunk}"]  = bytes()
                STREAM[f"PAGE{idx_page}"][f"BURST{idx_burst}"][f"CHUNK{idx_chunk}"] += idx_chunk.to_bytes(1)
                STREAM[f"PAGE{idx_page}"][f"BURST{idx_burst}"][f"CHUNK{idx_chunk}"] += chunk
                
                burst_hasher.update(STREAM[f"PAGE{idx_page}"][f"BURST{idx_burst}"][f"CHUNK{idx_chunk}"])

            CHECKSUMS[f"PAGE{idx_page}"][f"BURST{idx_burst}"] = burst_hasher.hexdigest()

    # TODO: probably some information prints would be useful but I cannot come up with
    # something clean right now
    return (STREAM, CHECKSUMS)





def TX_LINK_LAYER(ptx: CustomNRF24, STREAM: dict[str, dict[str, dict[str, bytes]]], CHECKSUMS: dict[str, dict[str, str]]) -> None:
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

    # Generate and send the TX_INFO message
    # NOTE: As of now, the TX_INFO message only contains the number of pages that will
    # be sent in the communication and the total ammount of bytes that are expected to
    # be transfered, but it can be changed to include more information
    #
    # | TxLength (4B) | TxWidth (4B) | = 8 Bytes
    #
    # TxLength: The number of pages that will be sent in the communication       [0..4_294_967_295]
    # TxWidth:  The total number of bytes that will be sent in the communication [0..4_294_967_295]
    TX_INFO  = bytes()
    TX_INFO += len(STREAM).to_bytes(4)
    TX_INFO += sum(len(STREAM[PageID][BurstID][ChunkID]) for PageID in STREAM for BurstID in STREAM[PageID] for ChunkID in STREAM[PageID][BurstID]).to_bytes(4)
    ptx.send_INFO_message(TX_INFO)


    for idx_page in range(len(STREAM)):
        page = STREAM[f"PAGE{idx_page}"]
        # NOTE: everytime we start a page, we send a PAGE_INFO message containing the
        # PageID, the ammount of bytes in the page (page_width) and the number of bursts
        # in the page (page length). The PAGE_INFO message payload has the following
        # structure:
        #
        # | PageID (1B) | PageLength (3B) | = 4 Bytes
        # 
        # PageID:     The identifier of the page           [0..255]
        # PageLength: The number of bursts inside the page [0..16_777_215]
        PAGE_INFO  = bytes()
        PAGE_INFO += idx_page.to_bytes(1)
        PAGE_INFO += (len(page)).to_bytes(3)
        ptx.send_INFO_message(PAGE_INFO)

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_TX_MODE(ptx: CustomNRF24) -> None:
    compressed_pages  = TX_PRESENTATION_LAYER()
    STREAM, CHECKSUMS = TX_TRANSPORT_LAYER(compressed_pages)
    TX_LINK_LAYER(ptx, STREAM, CHECKSUMS)

    for page in STREAM:
        for burst in STREAM[page]:
            for chunk in STREAM[page][burst]:
                STREAM[page][burst][chunk] = STREAM[page][burst][chunk].hex()

    import json
    with open("STREAM.json", "w") as f:
        json.dump(STREAM, f, indent = 4)

    with open("CHECKSUMS.json", "w") as f:
        json.dump(CHECKSUMS, f, indent = 4)

    
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
