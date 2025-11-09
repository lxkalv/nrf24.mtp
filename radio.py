# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
import pigpio
import time
import sys

from nrf24 import (
    NRF24,

    RF24_DATA_RATE,
    RF24_PA,
    RF24_CRC,
    RF24_PAYLOAD,
    RF24_RX_ADDR,
)

from utils import (
    YELLOW,

    ERROR,
    SUCC,
    WARN,
    INFO,

    status_bar,
)

from enum import Enum
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: ROLE CONFIG :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
class Role(Enum):
    UNSET       = "UNSET"
    TRANSMITTER = "TRANSMITTER"
    RECEIVER    = "RECEIVER"
    CARRIER     = "CARRIER"
    QUIT        = "QUIT"

    def __str__(self: "Role") -> str:
        return self.value
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: RADIO CONFIG :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
CE_PIN               = 22
CHANNEL              = 76                          # NOTE: Select one from [0..125], Channel is computed as 2.4GHz + (CHANNEL)MHz
DATA_RATE            = RF24_DATA_RATE.RATE_1MBPS   # NOTE: Select one from {250KBPS, 1MBPS, 2MBPS}
PA_LEVEL             = RF24_PA.MIN                 # NOTE: Select one from {MIN (-18dBm), LOW (-12dBm), HIGH (-6dBm), MAX (0dBm)}
CRC_BYTES            = RF24_CRC.BYTES_2            # NOTE: Select one from {DISABLED, BYTES_1, BYTES_2}
PAYLOAD_SIZE         = RF24_PAYLOAD.ACK            # NOTE: Select one from {ACK, DYNAMIC, MIN (1), MAX (32), [1..32]}
RETRANSMISSION_TRIES = 15                          # NOTE: Select one from [1..15]
RETRANSMISSION_DELAY = 1                           # NOTE: Select one from [1..15], Delay is computed as 250us + (250 * RETRANSMISSION_DELAY)us
ADDRESS_BYTE_LENGTH  = 3                           # NOTE: Select one from [3..5]


# TODO: kill pigpiod correctly
class CustomNRF24(NRF24):
    """
    Custom NRF24 class that allows for extending the NRF24 base class without
    modifying the library itself
    """
    
    def __init__(self: "CustomNRF24", spi_speed: float = 10_000_000) -> None:
        # :::: CONFIGURE PIGPIO :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        self.hostname = "localhost"
        self.port     = 8888

        pi = pigpio.pi(self.hostname, self.port)
        if not pi:
            ERROR("Not connected to Raspberry Pi, exiting")
            sys.exit(1)
        # :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        # :::: INITIALIZE RADIO DEVICE ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        super().__init__(pi = pi, ce = CE_PIN, spi_speed = spi_speed)
        self.role = Role.UNSET
        # :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        # :::: CONFIGURE RADIO DEVICE :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        

        self.set_channel(CHANNEL)
        self.set_data_rate(DATA_RATE)
        self.set_pa_level(PA_LEVEL)
        self.set_crc_bytes(CRC_BYTES)
        self.set_payload_size(PAYLOAD_SIZE)
        self.set_retransmission(RETRANSMISSION_DELAY, RETRANSMISSION_TRIES)
        self.set_address_bytes(ADDRESS_BYTE_LENGTH)

        self.ack_payload(RF24_RX_ADDR.P1, b"")

        self.choose_node_role()
        self.choose_address_based_on_role()

        self.show_registers()
        # :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        return
    

# :::: CLASS METHODS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
    def choose_node_role(self: "CustomNRF24") -> None:
        """
        Function to choose the role of the current node
        """

        while True:
            val = input(f"{YELLOW('[>>>>]:')} Please choose a role for this device [T]ransmitter, [R]eceiver: ")
            
            try:
                val = val.upper()
            except:
                continue

            if val == "T":
                INFO(f"Device set to {Role.TRANSMITTER} role")
                self.role = Role.TRANSMITTER
                return
                
            elif val == "R":
                INFO(f"Device set to {Role.RECEIVER} role")
                self.role = Role.RECEIVER
                return

    def choose_address_based_on_role(self: "CustomNRF24") -> None:
        """
        Choose the address of the current node based on the role that it has been
        assigned
        """
        
        if self.role is Role.TRANSMITTER:
            self.open_writing_pipe(b"TA1")
            self.open_reading_pipe(RF24_RX_ADDR.P1, b"TA0")
            INFO("Writing @: TA1 | Reading @: TA0")
        
        elif self.role is Role.RECEIVER:
            self.open_writing_pipe(b"TA0")
            self.open_reading_pipe(RF24_RX_ADDR.P1, b"TA1")
            INFO("Writing @: TA0 | Reading @: TA1")

        return
    
    def send_CONTROL_message(self: "CustomNRF24", CONTROL_MESSAGE: bytes, message_name: str, progress: bool = True, delay: float = 0) -> None:
        """
        Continuously send a given information message until we receive an ACK. The
        progress is shown with a status bar
        """

        if progress:
            t = 0
        message_has_been_sent = False
        
        while not message_has_been_sent:
            

            if progress:
                if t % 10 == 0:
                    status_bar(
                        message = f"Sending {message_name} message",
                        status  = "INFO",
                    )

                t += 1

            self.reset_packages_lost()
            self.send(CONTROL_MESSAGE)
            
            try:
                self.wait_until_sent()
            
            except TimeoutError:
                status_bar(
                    message = f"Time-out while sending {message_name} message, retrying",
                    status  = "ERROR",
                )

                continue


            if self.get_packages_lost() == 0 and self.get_payload() == message_name.encode():
                message_has_been_sent = True

            time.sleep(delay)

        if progress:
            status_bar(
                message = f"Sent {message_name} succesfully",
                status  = "SUCC",
            )

        return        

    
    # NOTE: I trust that someday my wonderful team will either develop or discard
    # this function
    # def send_three_frames_fast(self: "CustomNRF24", frame_1: list[bytes], frame_2: list[bytes] | None, frame_3: list[bytes] | None) -> None:
    #     """
    #     Function to send three frames without waiting for an ACK between them
    #     """
    # 
    #     if frame_2 is None:
    #         self.send(frame_1)
    # 
    #     
    #     return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
radio = CustomNRF24()

# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::