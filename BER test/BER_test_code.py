# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from nrf24 import (
    NRF24,

    RF24_DATA_RATE,
    RF24_PA,
    RF24_RX_ADDR,
)

import pigpio
import struct
import time
import sys
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
CE_PIN  = 22
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: HELPER FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def INFO(message: str) -> None:
    """
    Prints a message to the console with the blue prefix `[INFO]:`
    """
    print(f"\033[34m[INFO]:\033[0m {message}")



def SUCC(message: str) -> None:
    """
    Prints a message to the console with the green prefix `[SUCC]:`
    """
    print(f"\033[32m[SUCC]:\033[0m {message}")


def ERROR(message: str) -> None:
    """
    Prints a message to the console with the red prefix `[~ERR]:`
    """
    print(f"\033[31m[~ERR]:\033[0m {message}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: ROLE CONFIG  :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
role = ""

option_is_valid = False
while not option_is_valid:
    val = input("\033[33m[>>>>]:\033[0m Please choose a role for this device [T]ransmitter, [R]eceiver, [C]arrier, [Q]uit: ")
    try:
        val = val.upper()

        if val == "T":
            INFO('Device set to TRANSMITTER role')
            role = "T"
            option_is_valid = True
        
        elif val == "R":
            INFO('Device set to RECEIVER role')
            role = "R"
            option_is_valid = True

        elif val == "C":
            INFO('Device set to CONSTANT CARRIER role')
            role = "C"
            option_is_valid = True
        
        elif val == "Q":
            INFO('Quitting program...')
            role = "Q"
            option_is_valid = True
        
        else:
            continue

    except:
        continue
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: RADIO CONFIG :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

# pigpio
hostname = "localhost"
port     = 8888

pi = pigpio.pi(hostname, port)
if not pi.connected:
    ERROR("Not connected to Raspberry Pi, exiting")
    sys.exit(1)

# radio object
nrf = NRF24(pi, ce = CE_PIN)


# radio channel
nrf.set_channel(76)


# data rate
nrf.set_data_rate(RF24_DATA_RATE.RATE_1MBPS)


# Tx/Rx power
nrf.set_pa_level(RF24_PA.MIN)


# CRC
nrf.enable_crc()
nrf.set_crc_bytes(2)
nrf.disable_crc()


# global payload 
nrf.set_payload_size(32) # [1 - 32] Bytes
payload:list[bytes] = []


# auto-retries
nrf.set_retransmission(1, 15)


# Tx/Rx addresses
nrf.set_address_bytes(4) # [2 - 5] Bytes
possible_addreses = [b"TAN1", b"TAN2"] # Team A Node X 
address = ""


address_is_valid = False
while not address_is_valid:
    val = input("\033[33m[>>>>]:\033[0m Please choose a value for the address [0: TAN1, 1: TAN2]: ")
    try:
        val = int(val)

        if val == 0:
            INFO(f'Address set to {possible_addreses[0]}')
            address_is_valid = True

            if role == "T":
                nrf.open_writing_pipe(possible_addreses[1])
            
            elif role == "R":
                nrf.open_reading_pipe(RF24_RX_ADDR.P1, possible_addreses[0])

        if val == 1:
            INFO(f'Address set to {possible_addreses[1]}')
            address_is_valid = True

            if role == "T":
                nrf.open_writing_pipe(possible_addreses[0])
            
            elif role == "R":
                nrf.open_reading_pipe(RF24_RX_ADDR.P1, possible_addreses[1])
        
        else:
            continue

    except:
        continue


# status visualization
INFO(f"Radio details:")
nrf.show_registers()

# Number of transmitted packets
total_tx_packets = 10
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: FLOW FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def BEGIN_TRANSMITTER_MODE() -> None:
    """
    Transmits the first txt file found in the mounted USB
    """

    INFO('Starting transmission')

    try:
        INFO(f'Start transmission of all 0')

        # store the encoded bytes
        packets = []
        for i in range(total_tx_packets):
            packets.append(b'\x00' * nrf.get_payload_size())

        for idx in range(total_tx_packets):
            INFO(f"Sending packet:  {packets[idx]}")

            # reset the packages that we have lost
            nrf.reset_packages_lost()

            
            nrf.send(packets[idx])


            try:
                tic = time.monotonic_ns()
                nrf.wait_until_sent()
                tac = time.monotonic_ns()
            except TimeoutError:
                ERROR("Timeout while transmitting")

            if nrf.get_packages_lost() == 0:
                SUCC(f"Frame sent in {(tac - tic)/1000:.2f} us and {nrf.get_retries()}")

            else:
                ERROR(f"Lost packet after {nrf.get_retries()} retries")

            time.sleep(1) # wait for one second because why not
    
    finally:
        nrf.power_down()
        pi.stop()
    
    return










def BEGIN_RECEIVER_MODE() -> None:
    """
    Receives multiple frames from a transmitter and reassembles the blocks into a
    txt file
    """

    INFO('Starting reception')

    try:
        # start the timers
        tic     = time.monotonic()
        tac     = time.monotonic()
        timeout = 20
        INFO(f'Timeout set to {timeout} seconds')

        chunks = []
        while (tac - tic) < timeout:
            tac = time.monotonic()

            # check if there are frames
            while nrf.data_ready():

                payload_pipe = nrf.data_pipe()

                packet = nrf.get_payload()
                ones_counter = 0
                for byte_individual in packet:
                    string_binario = f'{byte_individual:08b}'
                    for bit in string_binario:
                        if bit == 1:
                            ones_counter += 1
                        

                chunk: str = struct.unpack(f"<{nrf.get_payload_size()}s", packet)[0] # the struct.unpack method returs more things than just the data
                chunks.append(chunk)
                
                SUCC(f"Received {len(chunk)} bytes on pipe {payload_pipe}: {packet} --> {chunk}")
            
                tic = time.monotonic()
            
            time.sleep(.1)

        INFO('Connection timed-out')
        
        
        INFO('Collected:')
        for chunk in chunks:
            print(f"    {chunk}")
        

        content = bytes()
        for chunk in chunks:
            content += chunk
        INFO(f'Merged data: {content}')
        

        if len(content) == 0:
            ERROR('Did not receive anything')
            return
        
        
        with open("file_received.txt", "wb") as f:
            f.write(content)
        content_len = len(content)
        INFO(f'Saved {content_len} bytes to: file_received.txt')

        BER = ones_counter/(total_tx_packets*nrf.get_payload_size())
        INFO(f'The final BER is of: {BER} with {ones_counter} ones received over {total_tx_packets*nrf.get_payload_size()} total bits sent')

    finally:
        nrf.power_down()
        pi.stop()

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
    match role:
        case "T":
            BEGIN_TRANSMITTER_MODE()
            return
        
        case "R":
            BEGIN_RECEIVER_MODE()
            return
        
        case "C":
            BEGIN_CONSTANT_CARRIER_MODE()
            return
        
        case "Q":
            return
        
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




if __name__ == "__main__":
    main()
