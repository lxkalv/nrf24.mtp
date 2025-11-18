# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path

import nrf24_mtp.utils.Logger as Logger

import nrf24_mtp.layers.PresentationLayer as PresentationLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: split_bytes_into_pages() test ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
data = b"This is a test data to be split into pages." * 10
Logger.INFO(f"Original data length: {len(data)} bytes")
pages = PresentationLayer.split_bytes_into_pages(data)
Logger.INFO(f"Number of pages: {len(pages)}")
for i, page in enumerate(pages):
    Logger.INFO(f"Page {i+1} length: {len(page)} bytes: {page}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

# :::: merge_pages_into_bytes() test ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
merged_data = PresentationLayer.merge_pages_into_bytes(pages)
Logger.INFO(f"Merged data length: {len(merged_data)} bytes")
Logger.INFO(f"Merged data matches original: {merged_data == data}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::