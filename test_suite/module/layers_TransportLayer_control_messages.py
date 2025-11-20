# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
from hashlib import sha256

import nrf24_mtp.utils.Logger as Logger

import nrf24_mtp.layers.ApplicationLayer  as ApplicationLayer
import nrf24_mtp.layers.PresentationLayer as PresentationLayer
import nrf24_mtp.layers.TransportLayer    as TransportLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: generate_burst_info_control_message() test :::::::::::::::::::::::::::::::::::::::::::::::::
content = ApplicationLayer.load_file_bytes(Path("test_files/quijote.txt"))
Logger.INFO(f"Loaded content length: {len(content)} bytes")

pages = PresentationLayer.split_bytes_into_pages(content)
Logger.INFO(f"Split content into {len(pages)} pages | size: {[len(page) for page in pages]} bytes")

compressed_pages = PresentationLayer.compress_pages(pages)
Logger.INFO(f"Compressed pages sizes: {[len(cpage) for cpage in compressed_pages]} bytes")

bursts = TransportLayer.split_page_into_bursts(compressed_pages[0])
Logger.INFO(f"Split first compressed page into {len(bursts)} bursts | frames per burst: {[len(burst) for burst in bursts]}")

cm = TransportLayer.generate_burst_info_control_message(2, 5, bursts[0])
Logger.INFO(f"Generated CONTROL MESSAGE: {cm.hex()}")
Logger.INFO(f"CONTROL MESSAGE Length: {len(cm)} bytes")
Logger.INFO(f"CONTROL MESSAGE Type: {cm[0]:02x}")
Logger.INFO(f"CONTROL MESSAGE BurstID: {cm[1]:02x}")
Logger.INFO(f"CONTROL MESSAGE PageID: {cm[2]:d}")
Logger.INFO(f"CONTROL MESSAGE BurstID: {cm[3]:d}")
Logger.INFO(f"CONTROL MESSAGE Burst Length: {int.from_bytes(cm[4:6])} bytes")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# :::: read_burst_info_control_message() test ::::::::::::::::::::::::::::::::::::::::::::::::::::::
parsed = TransportLayer.read_burst_info_control_message(cm)
if parsed:
    PageID, BurstID, BurstSize = parsed
    Logger.SUCC(f"Parsed CONTROL MESSAGE - PageID: {PageID}, BurstID: {BurstID}, Burst Size: {BurstSize} bytes")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::