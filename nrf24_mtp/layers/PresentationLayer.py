"""
##### Presentation Layer
Processes the data. Generates compressed pages from file bytes and
merges compressed pages back into file bytes.

##### Diagram
```
IO
                ┌────────────────────┐
                │                    │
 FILE BYTES <─> │ PRESENTATION LAYER │ <─> COMPRESSED PAGES
                │                    │
                └────────────────────┘
```

##### Bidirectional
- FILE BYTES: The raw bytes of a file from or to the layer above
- COMPRESSED PAGES: The compressed pages to be sent over the NRF24 link
"""

# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from math import ceil
import zlib

import nrf24_mtp.utils.Logger as Logger
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: PAGE SPLITTING :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
NUMBER_OF_PAGES = 10



def split_bytes_into_pages(content: bytes) -> list[bytes]:
    content_len = len(content)

    page_len = ceil(content_len / NUMBER_OF_PAGES)
    pages    = [
        content[i : i + page_len]
        for i in range(0, content_len, page_len)
    ]
    
    return pages



def merge_pages_into_bytes(pages: list[bytes]) -> bytes:
    content = b"".join(pages)
    return content
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: COMPRESSION ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def compress_pages(pages: list[bytes]) -> list[bytes]:
    
    compressed_pages: list[bytes] = []

    compressor = zlib.compressobj(level = 6)

    try:    
        for page in pages:
            compressed_page  = compressor.compress(page)
            compressed_page += compressor.flush(zlib.Z_SYNC_FLUSH)

            compressed_pages.append(compressed_page)

    except Exception as e:
        Logger.ERROR(f"Error compressing pages: {e}")

    finally:
        return compressed_pages



def decompress_pages(compressed_pages: list[bytes]) -> list[bytes]:
    
    pages: list[bytes] = []

    decompressor = zlib.decompressobj()

    try:
        for compressed_page in compressed_pages:
            page  = decompressor.decompress(compressed_page)
            
            pages.append(page)

    except Exception as e:
        Logger.ERROR(f"Error decompressing pages: {e}")

    finally:
        return pages
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::