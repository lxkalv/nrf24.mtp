# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
from math import ceil
import struct
import zlib


from utils import (
    WARN,
    INFO,

    progress_bar,

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

        progress_bar(
            active_msg     = "Compressing pages...",
            finished_msg   = f"Pages compressed successfully, | Compression ratio: ~{(sum(len(compressed_page) for compressed_page in compressed_pages) / content_len) * 100:.2f}%",
            current_status = idx,
            max_status     = len(pages)
        )
    
    # Provide the pages to the next layer
    return compressed_pages





def TX_TRANSPORT_LAYER(pages: list[bytes]) -> dict[str, dict[str, dict[str, bytes]]]:
    """
    This layer is responsible for the following things:
    - Splitting the compressed pages into bursts of 7936 Bytes. This is done so that
    later we can split each burst into chunks of 31 Bytes + 1 Byte of ChunkID (IDc)
    - Compute and store the checksum of each burst so that we can verify the integrity
    of the data after sending it
    - Providing a unified structure containing the bytes to be transmitted in an
    ordered and hierarchical way
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
    # PAGE L:
    #     BURST 0:
    #         ...
    #     BURST 1:
    #         ...
    #     ...
    #     BURST N:
    #         ...
    #
    #            PageID    BurstID   ChunkID
    #            ↓         ↓         ↓
    STREAM: dict[str, dict[str, dict[str, bytes]]] = dict()

    # Split each compressed page into bursts of 7936 Bytes
    # NOTE: The width of 7936 Bytes allows to split the burst into 256 chunks of 31
    # Bytes of width, this allows to limit the size of the IDc to 1 Byte, summing up
    # to a total payload of 32 Bytes. The IDc is resetted to 0 at each burst
    BURST_WIDTH = 7936
    CHUNK_WIDTH = 31
    for idx_page, page in enumerate(pages):

        STREAM[f"PAGE{idx_page}"] = dict()

        page_len = len(page)
        
        # split the page into bursts
        bursts = [
            page[i : i + BURST_WIDTH]
            for i in range(0, page_len, BURST_WIDTH)
        ]

        for idx_burst, burst in enumerate(bursts):

            STREAM[f"PAGE{idx_page}"][f"BURST{idx_burst}"] = dict()
            
            burst_len = len(burst)

            # split the burst into chunks
            chunks = [
                burst[i : i + CHUNK_WIDTH]
                for i in range(0, burst_len, CHUNK_WIDTH)
            ]

            for idx_chunk, chunk in enumerate(chunks):

                STREAM[f"PAGE{idx_page}"][f"BURST{idx_burst}"][f"CHUNK{idx_chunk}"]  = bytes()
                STREAM[f"PAGE{idx_page}"][f"BURST{idx_burst}"][f"CHUNK{idx_chunk}"] += idx_chunk.to_bytes(1)
                STREAM[f"PAGE{idx_page}"][f"BURST{idx_burst}"][f"CHUNK{idx_chunk}"] += chunk

    return STREAM
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_TX_MODE() -> None:
    compressed_pages = TX_PRESENTATION_LAYER()
    STREAM = TX_TRANSPORT_LAYER(compressed_pages)

    for page in STREAM:
        for burst in STREAM[page]:
            for chunk in STREAM[page][burst]:
                STREAM[page][burst][chunk] = STREAM[page][burst][chunk].hex()

    import json
    with open("STREAM.json", "w") as f:
        json.dump(STREAM, f, indent = 4)
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
