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
from typing import Any
from pathlib import Path

import math
import pigpio
import struct
import shutil
import time
import sys
import os

# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
CE_PIN  = 22

ACK_TIMEOUT_S = 10          # <<< max time waiting for manual ACK (500 µs)
MAX_ATTEMPTS  = 1000               # <<< per-packet retries (you can adjust)

ID_WIND_BYTES=2
ID_CHUNK_BYTES=1
PAYLOAD_SIZE=32
WINDOW_SIZE = 3
SEQ_START   = 1        # first packet ID
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

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



# :::: HELPER FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
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
    Prints a message to the console with the red prefix ⁠ [~ERR]: ⁠
    """
    print(f"\033[31m[~ERR]:\033[0m {message}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::






# :::: NODE CONFIG  :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
class Role(Enum):
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



def choose_address_based_on_role(role: Role, nrf: NRF24) -> None:
    """
    Choose the address of the current node based on the role that it has been
    assigned
    """
    
    if role is Role.TRANSMITTER:
        nrf.open_writing_pipe(b"TAN1")
        nrf.open_reading_pipe(RF24_RX_ADDR.P1, b"TAN0")
        INFO("Writing @: TAN1 | Reading @; TAN0")
    
    elif role is Role.RECEIVER:
        nrf.open_writing_pipe(b"TAN0")
        nrf.open_reading_pipe(RF24_RX_ADDR.P1, b"TAN1")
        INFO("Writing @: TAN0 | Reading @; TAN1")

    return

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


# global payload 
nrf.set_payload_size(0) # [1 - 32] Bytes, NOTE: 0 is dynamic
payload:list[bytes] = []



# auto-retries
# nrf.set_retransmission(1, 15) # Retransmitting (1+1)*250ms and just 15 times will try it. Automatic ACKs


# Tx/Rx addresses
nrf.set_address_bytes(4) # [2 - 5] Bytes





# === DISABLE HARDWARE AUTO-ACK (EN_AA=0) TO MAKE MANUAL ACKs =========
def _disable_auto_ack(nrf_obj):
    nrf_obj.unset_ce()
    nrf_obj._nrf_write_reg(nrf_obj.EN_AA, 0x00)   # <<< disable auto-ack for all pipes
    nrf_obj.set_ce()

    nrf_obj.set_retransmission(0, 0)  # <<< disable auto-retransmissions (x+1) * 250 µs

_disable_auto_ack(nrf) 
# =================================================================================


# status visualization
INFO(f"Radio details:")
nrf.show_registers()
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: FLOW FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

def _send_ack_packet() -> None:
    ack = b"ACK"                                    # build 32B: "ACK" 
    nrf.unset_ce()                                  # disable CE during config
    nrf.send(ack)                                   # send (this flips radio to TX)
    try:
        nrf.wait_until_sent()
    except TimeoutError:
                ERROR(f"Timeout while transmitting ID header packet")                          # block until TX done (radio goes back to RX)



def _wait_for_ack(timeout_s: float) -> bool:
    t0 = time.monotonic()                         # start a small timeout window
    while (time.monotonic() - t0) < timeout_s:      # poll until we hit the timeout
        if nrf.data_ready():
            payload_pipe = nrf.data_pipe()
            packet = nrf.get_payload()
            print(f'Recieved {packet}')                        # got something in RX FIFO
            return True # ACK → success
    return False                                     # timed out without a valid "ACK"

# --- helpers arriba de BEGIN_RECEIVER_MODE ---
def _decode_packet(pkt: bytes, extracted_window: int) -> tuple[int, int, bytes]:

    extracted_chunk = int.from_bytes(pkt[0:ID_CHUNK_BYTES], "big")
    if extracted_chunk == 0:
        extracted_window= int.from_bytes(pkt[ID_CHUNK_BYTES:ID_CHUNK_BYTES+ID_WIND_BYTES], "big")
        data = pkt[ID_WIND_BYTES+ID_CHUNK_BYTES:]
        return extracted_window, extracted_chunk, data
    else:
        data = pkt[ID_CHUNK_BYTES:]
        return extracted_window, extracted_chunk, data






def BEGIN_TRANSMITTER_MODE() -> None:
    """
    Transmits the first txt file found in the mounted USB
    """

    INFO('Starting transmission (manual ACK)')
    try:
        # open the file to read
        with open("lorem.txt", "rb") as file:
            content = file.read()

        content_len = len(content)
        INFO(f'Read {content_len} raw bytes read from file_to_send.txt: {content}')
        k=0;
        # split the contents into chunks
        chunks = []
        start_val = 0
        chunk_id = 0

        while start_val < content_len:
            # tous les WINDOW_SIZE chunks, on retire ID_WIND_BYTES
            if chunk_id % WINDOW_SIZE == 0:
                size = PAYLOAD_SIZE - ID_CHUNK_BYTES - ID_WIND_BYTES
                end_val = min(start_val + size, content_len)
                ident_wind = (chunk_id // WINDOW_SIZE).to_bytes(ID_WIND_BYTES, "big")  # exactly DATA_BYTES
                print(f"Window ID bytes: {ident_wind} for window {(chunk_id // WINDOW_SIZE)}")
                ident_chunk = (chunk_id % WINDOW_SIZE).to_bytes(ID_CHUNK_BYTES, "big")  # exactly DATA_BYTES
                final_content = ident_chunk + ident_wind + content[start_val:end_val]
            else:
                size = PAYLOAD_SIZE - ID_CHUNK_BYTES
                end_val = min(start_val + size, content_len)
                ident_chunk = (chunk_id % WINDOW_SIZE).to_bytes(ID_CHUNK_BYTES, "big")  # exactly DATA_BYTES
                final_content = ident_chunk + content[start_val:end_val]

            
            chunks.append(final_content)
            start_val = end_val
            chunk_id += 1
        
        #chunks_len = len(chunks) # Total number of chunks
        total_wind = math.ceil(chunk_id / WINDOW_SIZE)
        last_window_size = chunk_id % WINDOW_SIZE if (chunk_id % WINDOW_SIZE) != 0 else WINDOW_SIZE
        header = total_wind.to_bytes(ID_WIND_BYTES, "big") + last_window_size.to_bytes(1, "big")
        
        got_ack_id = False
        
        while not got_ack_id:
            nrf.send(struct.pack(f"<{len(header)}s", header))
            try:
                nrf.power_up_rx() 
                got_ack_id = _wait_for_ack(ACK_TIMEOUT_S)
                nrf.power_up_tx() 
            except TimeoutError:
                ERROR(f"Timeout while transmitting ID header packet") 
            
        
        
        # store the encoded bytes
        packets = []
        for chunk in chunks:
            packets.append(struct.pack(f"<{len(chunk)}s", chunk))

        # Start transmitting                       
        start = time.monotonic()   
        current_window = 0  
        current_chunk = 0     
        while current_window < total_wind:
            attempt = 1
            sent_ok = False
            window_packet = packets[current_chunk:current_chunk+WINDOW_SIZE]
            while attempt <= MAX_ATTEMPTS:          # Manual attempts
                INFO(f"Sending window #{current_window} (attempt {attempt}) of the window)")
                for p_idx, pkt in enumerate(window_packet): 
                    nrf.send(pkt)
                    try:
                        nrf.wait_until_sent()
                    except TimeoutError:
                        ERROR(f"Timeout while transmitting packet in window at local index {current_chunk+p_idx}")
                
                # Wait for the reception of the ACK. Cumulative ACK of the last seq_id

                got_ack = _wait_for_ack(ACK_TIMEOUT_S)    # Listen to RX for ACK

                if got_ack: 
                    ack_rtt_ms = (time.monotonic() - start) * 1000.0  # RTT of the manual ACK
                    SUCC(f"[ACK win] chunks {current_chunk}..{current_chunk+WINDOW_SIZE-1} ok | app_retries={attempt-1} | rtt={ack_rtt_ms:.2f} ms")
                    sent_ok = True
                    break
                else:
                    ERROR(f"No manual ACK for the window seq={current_window}")
                    attempt += 1
            

            if not sent_ok:
                ERROR(f"Giving up the transmssion because couldn't be sent the #{current_window} after {MAX_ATTEMPTS} attempts")
                break
            
            current_window += 1
            current_chunk += WINDOW_SIZE
            

    finally:
        nrf.power_down()
        pi.stop()
    
    return



def BEGIN_RECEIVER_MODE() -> None:
    """
    Receives multiple frames from a transmitter and reassembles the blocks into a
    txt file
    (responde con ACKs manuales tras cada recepción)
    """

    INFO('Starting reception (manual ACK)')

    try:

        # start the timers
        tic     = time.monotonic()
        tac     = time.monotonic()
        timeout = 20
        INFO(f'Timeout set to {timeout} seconds')

        while (tac - tic) < timeout:
            tac = time.monotonic()

            INFO("Waiting for header packet...")
            while not nrf.data_ready():
                pass

            header_packet = nrf.get_payload()
            raw = header_packet[:ID_WIND_BYTES+1]
            total_wind, last_window_size = struct.unpack(f">{ID_WIND_BYTES}sB", raw)
            total_wind = int.from_bytes(total_wind, "big") #Check that we don't need to 0 for the index of the payload
            
            print(f"Received header packet with total_wind={total_wind} and last_window_size={last_window_size}")

            _send_ack_packet()

            print(f"ACK sent for header packet")
            tic = time.monotonic()
            break
        
        current_window = 0
        extracted_window= 0
        current_chunk_in_window=0
        chunks = []
        window_chunks=[]
        timer_has_started = False

        # check if there are frames
        while ((tac - tic) < timeout) and (current_window < total_wind):
            
            tac = time.monotonic()
            while nrf.data_ready():
                if not timer_has_started:
                    throughput_tic = time.monotonic()
                    timer_has_started = True
                payload_pipe = nrf.data_pipe()

                packet = nrf.get_payload()

                extracted_window, extracted_chunk, chunk = _decode_packet(packet, extracted_window)
                print(f"Extracted window:{extracted_window} Extracted cunck: {extracted_chunk}")
                if current_chunk_in_window == extracted_chunk:
                    window_chunks.append(chunk)
                    current_chunk_in_window +=1
                    SUCC(f"Received chunk {current_chunk_in_window}/{WINDOW_SIZE} for window {extracted_window}. We are expecting {current_window}")
                    
                    if (extracted_window!=current_window) and ((current_chunk_in_window == WINDOW_SIZE) or ((extracted_window == total_wind-1) and (current_chunk_in_window == last_window_size))):
                        # --- SEND ACK --------------------------------
                        nrf.power_up_tx()                   
                        _send_ack_packet()                  
                        nrf.power_up_rx()                 
                        # ---------------------------------------------
                        window_chunks.clear()
                        SUCC(f"ACK send for window {extracted_window} / {total_wind} we wait for window {current_window}")
                        current_chunk_in_window = 0
                    # if window completed
                    elif (current_window != total_wind-1) and (current_chunk_in_window == WINDOW_SIZE):
                        # --- SEND ACK --------------------------------
                        nrf.power_up_tx()                   
                        _send_ack_packet()                  
                        nrf.power_up_rx()                 
                        # ---------------------------------------------
                        SUCC(f"ACK send for window {current_window} / {total_wind}")

                        current_window +=1
                        chunks.extend(window_chunks)
                        window_chunks.clear()

                        
                        current_chunk_in_window = 0
                    # last window completed
                    elif (current_window == total_wind-1) and (current_chunk_in_window == last_window_size) :
                        # --- SEND ACK --------------------------------
                        nrf.power_up_tx()                   
                        _send_ack_packet()                  
                        nrf.power_up_rx()                 
                        # ---------------------------------------------
                        current_window +=1
                        chunks.extend(window_chunks)
                        SUCC(f"ACK send for last window ({current_window} / {total_wind})")
                        break
                    tic = time.monotonic()
                else:
                    ERROR(f"Received out-of-order chunk (expected {current_chunk_in_window}, got {extracted_chunk}), discarding")
                    # Optional: could implement NACK or request retransmission here
                    tic = time.monotonic()
        INFO('Connection timed-out or all chunks recieved')
        
        
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
        SUCC(f'Saved {content_len} bytes to: file_received.txt')

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
    """
    Main flow of the application
    """

    role = choose_node_role()
    choose_address_based_on_role(role, nrf)

    if role is Role.TRANSMITTER:
        BEGIN_TRANSMITTER_MODE()
    
    elif role is Role.RECEIVER:
        BEGIN_RECEIVER_MODE()
        
    elif role is Role.CARRIER:
        BEGIN_CONSTANT_CARRIER_MODE()

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




if __name__ == "__main__":
    main()
