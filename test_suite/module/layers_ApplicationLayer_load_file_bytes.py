# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path

import nrf24_mtp.utils.Logger as Logger

import nrf24_mtp.layers.ApplicationLayer as ApplicationLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: load_file_bytes() test :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
if True:
    Logger.INFO("Test with no path provided")
    b = ApplicationLayer.load_file_bytes(None)
    Logger.SUCC(f"Read {len(b)}")

if True:
    Logger.INFO("Test with path provided")
    b = ApplicationLayer.load_file_bytes(Path("test_files/lorem.txt"))
    Logger.SUCC(f"Read {len(b)}")

if True:
    Logger.INFO("Test with invalid path provided")
    b = ApplicationLayer.load_file_bytes(Path("lolololo"))
    Logger.SUCC(f"Read {b}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::