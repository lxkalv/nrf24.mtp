"""
##### Link Layer
Sends bytes using the NRF24 module.

##### Diagram
```
IO
                 ┌────────────────────┐
 RADIO OBJECT ─> │                    │
                 │     LINK LAYER     │ <─> RADIO WAVES
 FRAME BYTES <─> │                    │
                 └────────────────────┘
```

##### Inputs:
- RADIO OBJECT: The NRF24 radio object used to send and receive data

##### Bidirectional
- FRAME BYTES: The raw bytes to be sent or received over the NRF24 link
- RADIO WAVES: The radio waves used to transmit data over the air
"""

# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
import nrf24_mtp.utils.Logger as Logger
import nrf24_mtp.trx.Trx      as Trx
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: FRAME HANDLING :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def send_frame(nrf: Trx.TRX, frame: bytes) -> None:
    """
    Blocking function that sends a single frame of bytes using the given NRF24 until
    the ACK is received
    """

    while True:
        nrf.reset_packages_lost()
        nrf.send(frame)

        try:
            nrf.wait_until_sent()
        except TimeoutError:
            Logger.WARN("Frame sending timed out, retrying...")
            continue

        if nrf.get_packages_lost() > 0:
            Logger.WARN("Frame sending failed, retrying...")
            continue
            
        return



def read_frame(nrf: Trx.TRX) -> bytes:
    """
    Blocking function that waits until a frame is received using the given NRF24 and
    returns the received bytes
    """

    while not nrf.data_ready():
        pass

    return nrf.get_payload()
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::