# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from typing import Any
import os

os.system("clear")
os.system("sudo pigpiod")

import nrf24_mtp.utils.Logger as Logger
import nrf24_mtp.trx.Trx as Trx

import nrf24_mtp.layers.ApplicationLayer  as ApplicationLayer
import nrf24_mtp.layers.PresentationLayer as PresentationLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: TX FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def TX_MODE(nrf: Trx, config: dict[str, Any]) -> None:
    nrf.show_registers()
    if not config["autostart"]:
        input(f"{Logger.YELLOW('[>>>>]')} Press Enter to start TX mode...")

    content = ApplicationLayer.load_file_bytes(config["file_path_tx"])
    if not content:
        Logger.ERROR("No data to send. Exiting TX mode.")
        return
    
    pages            = PresentationLayer.split_bytes_into_pages(content)
    compressed_pages = PresentationLayer.compress_pages(pages)
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: RX FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def RX_MODE(nrf: Trx, config: dict[str, Any]) -> None:
    nrf.show_registers()
    if not config["autostart"]:
        input(f"{Logger.YELLOW('[>>>>]')} Press Enter to start RX mode...")
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def main() -> None:
    config = ApplicationLayer.parse_arguments("P2P")
    nrf    = ApplicationLayer.configure_radio_object(config)

    try:
        if config["mode"] == ApplicationLayer.Mode.TX:
            TX_MODE(nrf, config)
        elif config["mode"] == ApplicationLayer.Mode.RX:
            RX_MODE(nrf, config)
    finally:
        nrf.power_down()
        return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: ENTRY POINT ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        Logger.WARN("Program interrupted by user. Exiting...")
    except Exception as e:
        Logger.ERROR(f"An error occurred: {e}")
    finally:
        os.system("sudo killall pigpiod")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::