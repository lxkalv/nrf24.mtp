# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
import os

os.system("cls" if os.name == "nt" else "clear")

from radio import (
    Role,

    radio,
)

from tx_flow import FULL_TX_MODE
from rx_flow import FULL_RX_MODE

from utils import (
    ERROR,
    INFO,
)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
RECEIVER_TIMEOUT_S = 20
DATA_SIZE          = 32
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# :::: FLOW FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def BEGIN_TRANSMITTER_MODE() -> None:
    """
    Transmits the first txt file found in the mounted USB, the flow of the TX MODE
    is the following:
    
    1. An appropiate file is selected from all the candidate files found in the
    mounted USB. The content of the file is extracted as raw bytes

    2. The bytes are splitted into chunks of size `payload_size` and then packed
    for future transmission

    3. An information message is sent containing the number of frames that the
    receiver should expect

    4. The rest of the frames are sent in a stop & wait fashion
    """

    INFO("Starting transmission")

    try:
        FULL_TX_MODE(radio)

    except KeyboardInterrupt:
        ERROR("Process interrupted by user")

    finally:
        radio.power_down()
        radio.pi_custom.stop()
    
    return





def BEGIN_RECEIVER_MODE() -> None:
    """
    Receives multiple frames from a transmitter and reassembles the blocks into a
    `txt` file, the location of the `txt` depends on if there is a mounted USB or
    not. The flow of the RX MODE is the following:

    1. Start the timer that will interrupt the receiving process if there has not
    been any frame for `timeout` seconds

    2. Start listening the channel for frames. The first frame is treated
    differently as it contains the number of frames that the receiver will expect

    3. Start listening for the regular data frames

    4. After all the frames has been received (or connection has timed-out), we
    merge the payloads into one chunk of data and store it in the mounted USB. If
    there is no mounted USB then the file is stored in memory
    """

    INFO(f"Starting reception")

    try:
        FULL_RX_MODE(radio)

    except KeyboardInterrupt:
        ERROR("Process interrupted by user")

    finally:
        radio.power_down()
        radio.pi_custom.stop()

    return





def BEGIN_CONSTANT_CARRIER_MODE() -> None:
    """
    Transmits a constant carrier until the user exits with CTRL+C
    """
    
    ERROR("TODO")
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: MAIN :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def main():
    """
    Main flow of the application
    """


    if radio.role is Role.TRANSMITTER:
        BEGIN_TRANSMITTER_MODE()
    
    elif radio.role is Role.RECEIVER:
        BEGIN_RECEIVER_MODE()
        
    elif radio.role is Role.CARRIER:
        BEGIN_CONSTANT_CARRIER_MODE()

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





if __name__ == "__main__":
    main()