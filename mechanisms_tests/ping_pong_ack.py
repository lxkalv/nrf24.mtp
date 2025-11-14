# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from nrf24 import (
    NRF24,

    RF24_DATA_RATE,
    RF24_PA,
    RF24_RX_ADDR,
    RF24_PAYLOAD,
    RF24_CRC
)

from enum import Enum


import pigpio
import time
import sys

# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
CE_PIN  = 22

ACK_TIMEOUT_S = 10          # <<< max time waiting for manual ACK (500 µs)
MAX_ATTEMPTS  = 1000               # <<< per-packet retries (you can adjust)

ID_WIND_BYTES=3
ID_CHUNK_BYTES=1
PAYLOAD_SIZE=32
WINDOW_SIZE = 3
SEQ_START   = 1        # first packet ID
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




# :::: ANSI COLOR FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def RED(message: str) -> str:
    """
    Returns a copy of the string wrapped in ANSI scape sequences to make it red
    """
    return f"\033[31m{message}\033[0m"



def GREEN(message: str) -> str:
    """
    Returns a copy of the string wrapped in ANSI scape sequences to make it green
    """
    return f"\033[32m{message}\033[0m"



def YELLOW(message: str) -> str:
    """
    Returns a copy of the string wrapped in ANSI scape sequences to make it yellow
    """
    return f"\033[33m{message}\033[0m"



def BLUE(message: str) -> str:
    """
    Returns a copy of the string wrapped in ANSI scape sequences to make it blue
    """
    return f"\033[34m{message}\033[0m"



def INFO(message: str) -> None:
    """
    Prints a message to the console with the blue prefix ⁠ [INFO]: ⁠
    """
    print(f"\033[34m[INFO]:\033[0m {message}")



def SUCC(message: str) -> None:
    """
    Prints a message to the console with the green prefix ⁠ [SUCC]: ⁠
    """
    print(f"\033[32m[SUCC]:\033[0m {message}")



def ERROR(message: str) -> None:
    """
    Prints a message to the console with the red prefix [~ERR]:
    """
    print(f"\033[31m[~ERR]:\033[0m {message}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# :::: RADIO ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
hostname = "localhost"
port     = 8888

pi = pigpio.pi(hostname, port)
if not pi.connected:
    ERROR("Not connected to Raspberry Pi, exiting")
    sys.exit(1)

# radio object
nrf = NRF24(pi, ce = CE_PIN, spi_speed =10e6)


# radio channel
nrf.set_channel(76)


# data rate
nrf.set_data_rate(RF24_DATA_RATE.RATE_1MBPS)


# Tx/Rx power
nrf.set_pa_level(RF24_PA.MIN)


# CRC
nrf.enable_crc()
nrf.set_crc_bytes(2)


# global payload 
nrf.set_payload_size(RF24_PAYLOAD.ACK)
payload:list[bytes] = []



# auto-retries
nrf.set_retransmission(1, 15)


# Tx/Rx addresses
nrf.set_address_bytes(4) # [2 - 5] Bytes
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: NODE CONFIG  :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
class Role(Enum):
    TRANSMITTER = "TRANSMITTER"
    RECEIVER    = "RECEIVER"

    def __str__(self: "Role") -> str:
        return self.value
    

def choose_node_role() -> Role:
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
            return Role.TRANSMITTER
            
        elif val == "R":
            INFO(f"Device set to {Role.RECEIVER} role")
            return Role.RECEIVER
        

def choose_address_based_on_role(role: Role, nrf: NRF24) -> None:
    """
    Choose the address of the current node based on the role that it has been
    assigned
    """
    
    if role is Role.TRANSMITTER:
        nrf.open_writing_pipe(b"TAN1")
        nrf.open_reading_pipe(RF24_RX_ADDR.P1, b"TAN0")
        INFO("Writing @: TAN1 | Reading @: TAN0")
    
    elif role is Role.RECEIVER:
        nrf.open_writing_pipe(b"TAN0")
        nrf.open_reading_pipe(RF24_RX_ADDR.P1, b"TAN1")
        INFO("Writing @: TAN0 | Reading @: TAN1")

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: MODES ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
tx_wait = 1

def BEGIN_TRANSMITTER_MODE() -> None:
    idx = 0
    while True:
        time.sleep(tx_wait)
        packet = idx.to_bytes(32)

        nrf.reset_packages_lost()
        nrf.send(packet)
        INFO(f"Sending: {packet.hex()} -> {idx}")

        try:
            nrf.wait_until_sent()
        except TimeoutError:
            ERROR("Time-out")
            continue

        if nrf.get_packages_lost() > 0:
            ERROR("Packet lost")
            continue

        ack = nrf.get_payload()    
        SUCC(f"Received: {ack.hex()} -> {int.from_bytes(ack)}")
        idx = int.from_bytes(ack) + 1

    return


def BEGIN_RECEIVER_MODE() -> None:
    nrf.ack_payload(RF24_RX_ADDR.P1, b"0")
    while True:
        
        while not nrf.data_ready():
            pass
    
        packet = nrf.get_payload()
        SUCC(f"Received: {int.from_bytes(packet)}")

        byte_to_num = int.from_bytes(packet)

        if byte_to_num % 3 == 0:
            length = 5
        if byte_to_num % 5 == 0:
            length = 32
        if byte_to_num % 15 == 0:
            length = 1
        else:
            next_idx = (byte_to_num + 1).to_bytes(2)
            INFO(f"Setting payload: {next_idx.hex()} -> {byte_to_num + 1}")
            nrf.ack_payload(RF24_RX_ADDR.P1, next_idx)

        if length != 1:
            next_idx = (byte_to_num + 1).to_bytes(length)
        else:
            next_idx = (0).to_bytes(length)

        INFO(f"Setting payload: {next_idx.hex()} -> {byte_to_num + 1}")
        nrf.ack_payload(RF24_RX_ADDR.P1, next_idx)

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# :::: MAIN :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def main():
    """
    Main flow of the application
    """

    role = choose_node_role()
    choose_address_based_on_role(role, nrf)

    nrf.show_registers()

    if role is Role.TRANSMITTER:
        BEGIN_TRANSMITTER_MODE()
    elif role is Role.RECEIVER:
        BEGIN_RECEIVER_MODE()
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        ERROR("Interrupted by user")
