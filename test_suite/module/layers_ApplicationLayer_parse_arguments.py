# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
import sys

import nrf24_mtp.utils.Logger as Logger

import nrf24_mtp.layers.ApplicationLayer as ApplicationLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: parse_arguments() test :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
if False:
    Logger.INFO("Test with no args provided")
    sys.argv = [
        "layers_ApplicationLayer.py",
    ]
    ApplicationLayer.parse_arguments("Test")

if True:
    Logger.INFO("Test with mode provided")
    sys.argv = [
        "layers_ApplicationLayer.py",
        "--mode", "TX",
        "--print-config"
    ]
    ApplicationLayer.parse_arguments("Test")

if True:
    Logger.INFO("Test with invalid arg range provided")
    sys.argv = [
        "layers_ApplicationLayer.py",
        "--mode", "TX",
        "--channel", "500",
        "--print-config"
    ]
    ApplicationLayer.parse_arguments("Test")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::