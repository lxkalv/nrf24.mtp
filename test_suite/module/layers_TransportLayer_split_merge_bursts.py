# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path

import nrf24_mtp.utils.Logger as Logger

import nrf24_mtp.layers.ApplicationLayer  as ApplicationLayer
import nrf24_mtp.layers.PresentationLayer as PresentationLayer
import nrf24_mtp.layers.TransportLayer    as TransportLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: split_page_into_bursts() test ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
content = ApplicationLayer.load_file_bytes(Path("test_files/quijote.txt"))
Logger.INFO(f"Loaded content length: {len(content)} bytes")

pages = PresentationLayer.split_bytes_into_pages(content)
Logger.INFO(f"Split content into {len(pages)} pages | size: {[len(page) for page in pages]} bytes")

compressed_pages = PresentationLayer.compress_pages(pages)
Logger.INFO(f"Compressed pages sizes: {[len(cpage) for cpage in compressed_pages]} bytes")

bursts = TransportLayer.split_page_into_bursts(compressed_pages[0])
Logger.INFO(f"Split first compressed page into {len(bursts)} bursts | frames per burst: {[len(burst) for burst in bursts]}")

for BurstID, burst in enumerate(bursts):
    for FrameID, frame in enumerate(burst):
        Logger.INFO(f"BurstID: {BurstID:02d} | FrameID: {FrameID:03d} | Frame Size: {len(frame)} bytes | Frame Data: {frame.hex()}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: merge_bursts_into_page() test ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
merged_page = TransportLayer.merge_bursts_into_page(bursts)
Logger.INFO(f"Merged bursts back into page | size: {len(merged_page)} bytes")

if merged_page == compressed_pages[0]:
    Logger.SUCC("Merged page matches original compressed page!")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::