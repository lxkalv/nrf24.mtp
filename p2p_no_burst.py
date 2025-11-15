# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
import os

os.system("cls")

from radio import (
    Role,

    radio,
)

from tx_flow import FULL_TX_MODE
from rx_flow import FULL_RX_MODE

from utils import INFO
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: MAIN :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def main():
    """
    Main flow of the application
    """

    if radio.role is Role.TRANSMITTER:
        FULL_TX_MODE(radio)
    
    elif radio.role is Role.RECEIVER:
        FULL_RX_MODE(radio)

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



if __name__ == "__main__":
    try:
        os.system("sudo pigpiod")
        main()
    except KeyboardInterrupt:
        INFO("Process interrupted by user")
    finally:
        radio.power_down()
        radio._pi.stop()
        os.system("sudo killall pigpiod")