# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path

import nrf24_mtp.utils.Logger as Logger

import nrf24_mtp.layers.ApplicationLayer as ApplicationLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: load_file_bytes() ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
b = ApplicationLayer.load_file_bytes(None)
Logger.SUCC(f"Read {len(b)} bytes")


b = ApplicationLayer.load_file_bytes(Path("test_files/lorem.txt"))
Logger.SUCC(f"Read {len(b)} bytes")


b = ApplicationLayer.load_file_bytes(Path("lolololo"))
Logger.SUCC(f"Read {b}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: store_file_bytes() :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::if True:
ok = ApplicationLayer.store_file_bytes(None, b"Data 1")
Logger.SUCC(f"Status {ok}")



ok = ApplicationLayer.store_file_bytes(Path("received_files"), b"Data 2")
Logger.SUCC(f"Status {ok}")



ok = ApplicationLayer.store_file_bytes(Path("lolololo"), b"Data 3")
Logger.SUCC(f"Status {ok}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::