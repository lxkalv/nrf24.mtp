# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path

import nrf24_mtp.utils.Logger as Logger

import nrf24_mtp.layers.PresentationLayer as PresentationLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: compress_pages() test ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
data = b"This is a test data to be split into pages." * 10
Logger.INFO(f"Original data length: {len(data)} bytes")
pages = PresentationLayer.split_bytes_into_pages(data)
compressed_pages = PresentationLayer.compress_pages(pages)
Logger.INFO(f"Number of compressed pages: {len(compressed_pages)}")
for i, compressed_page in enumerate(compressed_pages):
    Logger.INFO(f"Compressed Page {i+1} length: {len(compressed_page)} bytes: {compressed_page}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

# :::: decompress_pages() test ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
decompressed_pages = PresentationLayer.decompress_pages(compressed_pages)
merged_data = PresentationLayer.merge_pages_into_bytes(decompressed_pages)
Logger.INFO(f"Merged data length: {len(merged_data)} bytes")
Logger.INFO(f"Merged data matches original: {merged_data == data}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::