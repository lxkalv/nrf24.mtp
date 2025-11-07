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
    SUCC,
    INFO,
)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

def BEGIN_TRANSMITTER_MODE() -> None:
    INFO("Starting transmission")

    try:
        payload = bytes()
        while True:
            radio.reset_packages_lost()
            radio.send(payload)
            try:
                radio.wait_until_sent()
            except TimeoutError:
                ERROR("Time-out")

            if radio.get_packages_lost() == 0:
                ack = radio.get_payload()
                SUCC(f"Received {ack}")

    except KeyboardInterrupt:
        ERROR("Process interrupted by user")

    finally:
        radio.power_down()
        radio.pi_custom.stop()
    
    return




def BEGIN_RECEIVER_MODE() -> None:
    INFO(f"Starting reception")

    try:
        idx = 0
        while True:
            while radio.data_ready():
                payload = radio.get_payload()
                SUCC(f"Received {payload}")
                idx += 1

                if idx % 100 == 0:
                    radio.ack_payload(idx.to_bytes(5))

    except KeyboardInterrupt:
        ERROR("Process interrupted by user")

    finally:
        radio.power_down()
        radio.pi_custom.stop()

    return