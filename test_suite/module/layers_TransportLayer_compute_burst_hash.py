# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
from hashlib import sha256

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

burst = bursts[0]

burst_hash = TransportLayer.compute_burst_hash(burst)
Logger.INFO(f"Computed hash for BurstID 00: {burst_hash.hex()}")

hasher = sha256()
for FrameID, frame in enumerate(burst):
    hasher.update(frame)
expected_hash = hasher.digest()
Logger.INFO(f"Expected hash for BurstID 00: {expected_hash.hex()}")
    
if burst_hash == expected_hash:
    Logger.SUCC("Burst hash matches expected hash.")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::