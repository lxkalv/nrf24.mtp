# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path

import nrf24_mtp.utils.Logger as Logger

import nrf24_mtp.layers.ApplicationLayer as ApplicationLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: store_file_bytes() test :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
if True:
    Logger.INFO("Test with no path provided")
    ok = ApplicationLayer.store_file_bytes(None, b"Data 1")
    Logger.SUCC(f"Status {ok}")

if True:
    Logger.INFO("Test with path provided")
    ok = ApplicationLayer.store_file_bytes(Path("received_files"), b"Data 2")
    Logger.SUCC(f"Status {ok}")

if True:
    Logger.INFO("Test with invalid path provided")
    ok = ApplicationLayer.store_file_bytes(Path("lolololo"), b"Data 3")
    Logger.SUCC(f"Status {ok}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::