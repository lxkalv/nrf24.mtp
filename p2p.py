# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from typing import Any
import os

import nrf24_mtp.utils.Logger as Logger
import nrf24_mtp.trx.Trx as Trx

import nrf24_mtp.layers.ApplicationLayer  as ApplicationLayer
import nrf24_mtp.layers.PresentationLayer as PresentationLayer
import nrf24_mtp.layers.TransportLayer    as TransportLayer
import nrf24_mtp.layers.LinkLayer         as LinkLayer
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: TX FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def TX_MODE(nrf: Trx.TRX, config: dict[str, Any]) -> None:
    nrf.show_registers()
    if not config["autostart"]:
        input(f"{Logger.YELLOW('[>>>>]')} Press Enter to start TX mode...")

    content = ApplicationLayer.load_file_bytes(config["file_path_tx"])
    if not content:
        Logger.ERROR("No data to send. Exiting TX mode.")
        return
    
    pages            = PresentationLayer.split_bytes_into_pages(content)
    compressed_pages = PresentationLayer.compress_pages(pages)

    for PageID, compressed_page in enumerate(compressed_pages):
        bursts = TransportLayer.split_page_into_bursts(compressed_page)

        BurstID = 0
        while BurstID < len(bursts):
            burst      = bursts[BurstID]
            BURST_HASH = TransportLayer.compute_burst_hash(burst)
            BURST_INFO = TransportLayer.generate_burst_info_control_message(PageID, BurstID, burst)
            LinkLayer.send_frame(nrf, BURST_INFO)

            for FrameID, frame in enumerate(burst):
                LinkLayer.send_frame(nrf, frame)
            
            nrf.power_up_rx()
            rx_hash = LinkLayer.read_frame(nrf)

            if rx_hash == BURST_HASH:
                Logger.SUCC(f"PageID: {PageID:02d} | BurstID: {BurstID:02d} transmitted successfully.")
                BurstID += 1
            else:
                Logger.WARN(f"PageID: {PageID:02d} | BurstID: {BurstID:02d} transmission failed. Retrying...")

    TRANSFER_FINISH = TransportLayer.generate_transfer_finish_control_message()
    LinkLayer.send_frame(nrf, TRANSFER_FINISH)
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: RX FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def RX_MODE(nrf: Trx, config: dict[str, Any]) -> None:
    nrf.show_registers()
    if not config["autostart"]:
        input(f"{Logger.YELLOW('[>>>>]')} Press Enter to start RX mode...")

    transfer_has_finished = False
    while not transfer_has_finished:
        frame = LinkLayer.read_frame(nrf)
        
        if frame[0] == 0xFF and frame[1] == 0xF0:
            PageID, BurstID, BurstSize = TransportLayer.read_burst_info_control_message(frame)

        if frame[0] == 0xFF and frame[1] == 0xFF:
            transfer_has_finished = True
        
        if frame[0] < 0xF0:
            pass
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
        os.system("clear")
        os.system("sudo pigpiod")
        
        main()
    except KeyboardInterrupt:
        Logger.WARN("Program interrupted by user. Exiting...")
    except Exception as e:
        Logger.ERROR(f"An error occurred: {e}")
    finally:
        os.system("sudo killall pigpiod")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::