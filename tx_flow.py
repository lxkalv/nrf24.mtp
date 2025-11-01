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
    # critical aspect in this project given that the achieved throughput is so low and
    # we expect interferences in the link
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





def TX_TRANSPORT_LAYER(compressed_pages: list[bytes]) -> None:
    """
    This layer is responsible for the following things:
    - Splitting the compressed pages into bursts of 7936 Bytes. This is done so that
    later we can split each burst into chunks of 31 Bytes + 1 Byte of ChunkID (IDc)
    - Compute and store the checksum of each burst so that we can verify the integrity
    of the data after sending it
    - Prepare the information messages for the receiver so that we can create a
    reliable link
    - Providing an unified stream to the next layer
    """

    # Split each compressed page into bursts of 7936 Bytes
    # NOTE: The width of 7936 Bytes allows to split the burst into 256 chunks of 31
    # Bytes of width, this allows to limit the size of the IDc to 1 Byte, summing up
    # to a total payload of 32 Bytes. The IDc is resetted to 0 at each burst
    bursts_per_page: list[list[bytes]] = []
    BURST_WIDTH                        = 7936
    page_info_messages: list[bytes]    = []
    for idx, compressed_page in enumerate(compressed_pages):
        page_len = len(compressed_page)
        
        bursts = [
            compressed_page[i : i + BURST_WIDTH]
            for i in range(0, page_len, BURST_WIDTH)
        ]

        bursts_per_page.append(bursts)

        # NOTE: as of now, or page information messages include the following:
        # - 4 Bytes for the page index                   (up to 4.294.967.296 pages)
        # - 4 Bytes for the page length                  (up to 4.294.967.296 Bytes)
        # - 4 Bytes for the number of bursts in the page (up to 4.294.967.296 bursts)
        page_info_messages.append(
            struct.pack("<iii", idx, page_len, len(bursts))
        )
    

    # split each bust into chunks 31 + 1 Bytes
    chunks_per_burst: list[list[bytes]] = []
    CHUNK_WIDTH                         = 31
    burst_info_messages: list[bytes]    = []
    for bursts_in_current_page in bursts_per_page:
        for burst in bursts_in_current_page:
            burst_len = len(burst)

            chunks = [
                burst[i : i + CHUNK_WIDTH]
                for i in range(0, burst_len, CHUNK_WIDTH)
            ]

            WARN("STARTING")
            for idx in range(len(chunks)):
                IDc         = idx.to_bytes(1, "little")
                chunks[idx] = IDc + chunks[idx]
                INFO(IDc)

            chunks_per_burst.append(chunks)

            # NOTE: as of now, or burst information messages include the following:
            # 

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_TX_MODE() -> None:
    compressed_pages = TX_PRESENTATION_LAYER()
    TX_TRANSPORT_LAYER(compressed_pages)
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
