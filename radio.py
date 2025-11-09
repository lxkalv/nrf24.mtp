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
DATA_RATE            = RF24_DATA_RATE.RATE_2MBPS   # NOTE: Select one from {250KBPS, 1MBPS, 2MBPS}
PA_LEVEL             = RF24_PA.MIN                 # NOTE: Select one from {MIN (-18dBm), LOW (-12dBm), HIGH (-6dBm), MAX (0dBm)}
CRC_BYTES            = RF24_CRC.BYTES_2            # NOTE: Select one from {DISABLED, BYTES_1, BYTES_2}
PAYLOAD_SIZE         = RF24_PAYLOAD.DYNAMIC        # NOTE: Select one from {ACK, DYNAMIC, MIN (1), MAX (32), [1..32]}
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

        self.pi_custom = pigpio.pi(self.hostname, self.port)
        if not self.pi_custom.connected:
            ERROR("Not connected to Raspberry Pi, exiting")
            sys.exit(1)
        # :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        # :::: INITIALIZE RADIO DEVICE ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        super().__init__(pi = self.pi_custom, ce = CE_PIN, spi_speed = spi_speed)
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

        # :::: INTERNAL CONSTANTS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        self.TXIM = "TXIM"
        self.PAIM = "PAIM"
        self.BUIM = "BUIM"
        # :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        return
    

# :::: CLASS METHODS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
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
            self.open_writing_pipe(b"TA1")
            self.open_reading_pipe(RF24_RX_ADDR.P0, b"TA0")
            INFO("Writing @: TA1 | Reading @: TA0")
        
        elif self.role is Role.RECEIVER:
            self.open_writing_pipe(b"TA0")
            self.open_reading_pipe(RF24_RX_ADDR.P0, b"TA1")
            INFO("Writing @: TA0 | Reading @: TA1")

        return
    
    def send_INFO_message(self: "CustomNRF24", INFO_MESSAGE: bytes, message_name: str) -> None:
        """
        Continuously send a given information message until we receive an ACK. The
        progress is shown with a status bar
        """
        
        t                     = 0
        message_has_been_sent = False
        
        while not message_has_been_sent:
            
            if t % 10 == 0:
                status_bar(
                    message = f"Sending {message_name} message",
                    status  = "INFO",
                )

            t += 1

            self.reset_packages_lost()
            self.send(INFO_MESSAGE)
            
            try:
                self.wait_until_sent()
            
            except TimeoutError:
                status_bar(
                    message = f"Time-out while sending {message_name} message, retrying",
                    status  = "ERROR",
                )

                continue


            if self.get_packages_lost() == 0:
                message_has_been_sent = True

        status_bar(
            message = f"Sent {message_name} succesfully",
            status  = "SUCC",
        )

        return
    
    def receive_INFO_message(self: "CustomNRF24") -> None | tuple[int, int] | tuple[int, int, int]:
        """
        Function to receive and confirm an INFO message without ambiguiti or possible error
        """
        idx_bar = 0
        while not self.data_ready():
            if idx_bar % 100 == 0:
                status_bar(
                    message  = "Waiting for INFO message",
                    finished_msg = "...",
                    status     = False,
                )
                idx_bar += 1
        
        try:
            INFO_MESSAGE: bytes = self.get_payload()
            MessageID           = INFO_MESSAGE[0:4].decode()

        except:
            ERROR(f"Invalid MessageID: {INFO_MESSAGE[0:4]}")
            return None 
        
        if MessageID == self.TXIM:
            TxLength = int.from_bytes(INFO_MESSAGE[4:8]) + 1
            TxWidth  = int.from_bytes(INFO_MESSAGE[8:12])
            status_bar(
                message  = "...",
                finished_msg = f"Received {self.TXIM} message: TxLength -> {TxLength} | TxWidth -> {TxWidth}",
                status     = True,
            )
            return (TxLength, TxWidth)
        
        elif MessageID == self.PAIM:
            PageID     = INFO_MESSAGE[4]
            PageLength = int.from_bytes(INFO_MESSAGE[5:8]) + 1
            PageWidth  = int.from_bytes(INFO_MESSAGE[8:12])
            status_bar(
                message  = "...",
                finished_msg = f"Received {self.PAIM} message: PageID -> {PageID} | PageLength -> {PageLength} | PageWidth -> {PageWidth}",
                status     = True,
            )
            return (PageID, PageLength, PageWidth)
    
        # elif MessageID == self.BUIM:
            

    
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