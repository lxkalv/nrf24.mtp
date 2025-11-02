# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
import pigpio
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
    INFO,
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

def choose_node_role() -> Role:
    """
    Function to choose the role of the current node
    """

    while True:
        val = input(f"{YELLOW('[>>>>]:')} Please choose a role for this device [T]ransmitter, [R]eceiver, [C]arrier, [Q]uit: ")
        
        try:
            val = val.upper()
        except:
            continue

        if val == "T":
            INFO(f"Device set to {Role.TRANSMITTER} role")
            return Role.TRANSMITTER
            
        elif val == "R":
            INFO(f"Device set to {Role.RECEIVER} role")
            return Role.RECEIVER

        elif val == "C":
            INFO(f"Device set to {Role.CARRIER} role")
            return Role.CARRIER
        
        elif val == "Q":
            INFO("Quitting program...")
            return Role.QUIT
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




# :::: RADIO CONFIG :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
CE_PIN               = 22
CHANNEL              = 76                          # NOTE: Select one from [0..125], Channel is computed as 2.4GHz + (CHANNEL)MHz
DATA_RATE            = RF24_DATA_RATE.RATE_2MBPS   # NOTE: Select one from {250KBPS, 1MBPS, 2MBPS}
PA_LEVEL             = RF24_PA.MIN                 # NOTE: Select one from {MIN (-18dBm), LOW (-12dBm), HIGH (-6dBm), MAX (0dBm)}
CRC_BYTES            = RF24_CRC.BYTES_2            # NOTE: Select one from {DISABLED, BYTES_1, BYTES_2}
PAYLOAD_SIZE         = RF24_PAYLOAD.DYNAMIC        # NOTE: Select one from {DYNAMIC, MIN (1), MAX (32), [1..32]}
RETRANSMISSION_TRIES = 15                          # NOTE: Select one from [1..15]
RETRANSMISSION_DELAY = 1                           # NOTE: Select one from [1..15], Delay is computed as 250us + (250 * RETRANSMISSION_DELAY)us
ADDRESS_BYTE_LENGTH  = 3                           # NOTE: Select one from [3..5]



class CustomNRF24(NRF24):
    """
    Custom NRF24 class that allows for extending the NRF24 base class without
    modifying the library itself
    """
    
    def __init__(self: "CustomNRF24", spi_speed: float = 10_000_000) -> None:
        # :::: CONFIGURE PIGPIO :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        hostname = "localhost"
        port     = 8888

        pi = pigpio.pi(hostname, port)
        if not pi.connected:
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
        self.show_registers()

        self.choose_node_role()
        self.choose_address_based_on_role()
        # :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        return
    


    def choose_node_role(self: "CustomNRF24") -> None:
        """
        Function to choose the role of the current node
        """

        while True:
            val = input(f"{YELLOW('[>>>>]:')} Please choose a role for this device [T]ransmitter, [R]eceiver, [C]arrier, [Q]uit: ")
            
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

            elif val == "C":
                INFO(f"Device set to {Role.CARRIER} role")
                self.role = Role.CARRIER
                return
            
            elif val == "Q":
                INFO("Quitting program...")
                self.role = Role.QUIT
                return

    def choose_address_based_on_role(self: "CustomNRF24") -> None:
        """
        Choose the address of the current node based on the role that it has been
        assigned
        """
        
        if self.role is Role.TRANSMITTER:
            radio.open_writing_pipe(b"TA1")
            radio.open_reading_pipe(RF24_RX_ADDR.P1, b"TA0")
            INFO("Writing @: TA1 | Reading @; TA0")
        
        elif self.role is Role.RECEIVER:
            radio.open_writing_pipe(b"TA0")
            radio.open_reading_pipe(RF24_RX_ADDR.P1, b"TA1")
            INFO("Writing @: TA0 | Reading @; TA1")

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
radio = CustomNRF24()
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::