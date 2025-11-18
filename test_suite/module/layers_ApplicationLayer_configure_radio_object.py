# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
import sys

import nrf24_mtp.utils.Logger as Logger

import nrf24_mtp.layers.ApplicationLayer as ApplicationLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: configure_radio_object() test ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
if True:
    sys.argv = ["layers_ApplicationLayer_configure_radio_object.py", "--mode", "TX"]
    Logger.INFO("Test with default config")
    config = ApplicationLayer.parse_arguments("Test")
    radio  = ApplicationLayer.configure_radio_object(config)
    radio.show_registers()
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::