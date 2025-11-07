# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
import os

os.system("cls" if os.name == "nt" else "clear")

from radio import (
    Role,
    
    radio,
)

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