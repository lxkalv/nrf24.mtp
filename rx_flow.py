# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from radio import CustomNRF24

from utils import (
    SUCC,
    WARN,
    INFO,
)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: PROTOCOL LAYERS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def RX_LINK_LAYER(prx: CustomNRF24) -> None:
    """
    This layer is responsible for the following things:
    - Reading a TX_INFO message and set up the expected number of pages
    - Reading a PAGE_INFO message
    - Reading a BURST_INFO message
    - Receiving each frame
    - Compute and send the checksum at the end of the burst
    """
    
    # Waint for a TX_INFO message
    # NOTE: As of now, the TX_INFO message only contains the number of pages that will
    # be sent in the communication and the total ammount of bytes that are expected to
    # be transfered, but it can be changed to include more information
    #
    # | TxLength (4B) | TxWidth (4B) | = 8 Bytes
    #
    # TxLength: The number of pages that will be sent in the communication       [0..4_294_967_295]
    # TxWidth:  The total number of bytes that will be sent in the communication [0..4_294_967_295]
    INFO("Waiting for TX_INFO message")
    while not prx.data_ready():
        pass

    TX_INFO  = prx.get_payload()
    TxLength = int.from_bytes(TX_INFO[0:4])
    TxWidth  = int.from_bytes(TX_INFO[4:8])
    SUCC(f"Received TX_INFO message: TxLength = {TxLength} | TxWidth = {TxWidth}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_RX_MODE(prx: CustomNRF24) -> None:
    RX_LINK_LAYER(prx)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
