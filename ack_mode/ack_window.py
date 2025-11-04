# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from nrf24 import (
    NRF24,

    RF24_DATA_RATE,
    RF24_PA,
    RF24_RX_ADDR,
)

import math
import pigpio
import struct
import time
import sys
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
CE_PIN  = 22

ACK_TIMEOUT_S = 500e-6          # <<< max time waiting for manual ACK (500 µs)
MAX_ATTEMPTS  = 3               # <<< per-packet retries (you can adjust)

ID_BYTES=1
PAYLOAD_SIZE=32;
DATA_BYTES   = PAYLOAD_SIZE - ID_BYTES
WINDOW_SIZE = 3
SEQ_START   = 1        # first packet ID
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





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


# global payload 
nrf.set_payload_size(0) # [1 - 32] Bytes, NOTE: 0 is dynamic
payload:list[bytes] = []

PAYLOAD_SIZE = nrf.get_payload_size()   # <<< local shortcut





# auto-retries
# nrf.set_retransmission(1, 15) # Retransmitting (1+1)*250ms and just 15 times will try it. Automatic ACKs


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

            # --- Open both sides to enable the ACK --------------------
            nrf.open_reading_pipe(RF24_RX_ADDR.P1, possible_addreses[0])   # pipe for RX
            nrf.open_writing_pipe(possible_addreses[1])                    # pipe for TX
            # -------------------------------------------------------------------

        if val == 1:
            INFO(f'Address set to {possible_addreses[1]}')
            address_is_valid = True

            # --- Open both sides to enable the ACK --------------------
            nrf.open_reading_pipe(RF24_RX_ADDR.P1, possible_addreses[1])   # pipe for Rx
            nrf.open_writing_pipe(possible_addreses[0])                    # pipe for TX
            # -------------------------------------------------------------------
        
        else:
            continue

    except:
        continue


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
    nrf.wait_until_sent()                           # block until TX done (radio goes back to RX)



def _wait_for_ack(timeout_s: float) -> bool:
    t0 = time.monotonic()                           # start a small timeout window
    while (time.monotonic() - t0) < timeout_s:      # poll until we hit the timeout
        if nrf.data_ready():                        # got something in RX FIFO
                return True # ACK → success
        else:
            print("ERROR no data ready")
    return False                                     # timed out without a valid "ACK"




def BEGIN_TRANSMITTER_MODE() -> None:
    """
    Transmits the first txt file found in the mounted USB
    """

    INFO('Starting transmission (manual ACK)')
    try:
        # open the file to read
        with open("file_to_send.txt", "rb") as file:
            content = file.read()

        content_len = len(content)
        INFO(f'Read {content_len} raw bytes read from file_to_send.txt: {content}')
        k=0;
        # split the contents into chunks
        chunks = []
        start_val = 0
        chunk_id = 0

        while start_val < content_len:
            # tous les WINDOW_SIZE chunks, on retire ID_BYTES
            if chunk_id % WINDOW_SIZE == 0:
                size = PAYLOAD_SIZE - ID_BYTES
                end_val = min(start_val + size, content_len)
                ident = (chunk_id // WINDOW_SIZE).to_bytes(ID_BYTES, "big")  # exactly DATA_BYTES
                final_content = ident + content[start:end_val]
            else:
                size = PAYLOAD_SIZE
                end_val = min(start_val + size, content_len)
                final_content = content[start_val:end_val]

            
            chunks.append(final_content)
            start_val = end_val
            chunk_id += 1
        
        #chunks_len = len(chunks) # Total number of chunks
        total_wind = math.ceil(chunk_id / WINDOW_SIZE)
        last_window_size = chunk_id % WINDOW_SIZE if (chunk_id % WINDOW_SIZE) != 0 else WINDOW_SIZE
        header = total_wind.to_bytes(ID_BYTES, "big") + last_window_size.to_bytes(1, "big")
        
        got_ack_id = False

        while not got_ack_id:
            nrf.send(struct.pack(f"<{len(header)}s", header))
            try:
                nrf.wait_until_sent()
            except TimeoutError:
                ERROR(f"Timeout while transmitting ID header packet")

            got_ack_id = _wait_for_ack(ACK_TIMEOUT_S)

        
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
                for p_idx, pkt in window_packet: 
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
                ERROR(f"Giving up window #{current_window} after {MAX_ATTEMPTS} attempts")
                # break
            
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

            raw = header_packet[:ID_BYTES+1]
            total_wind, last_window_size = struct.unpack(f">{ID_BYTES}sB", raw)
            total_wind = int.from_bytes(total_wind, "big") #Check that we don't need to 0 for the index of the payload
            
            _send_ack_packet()
            tic = time.monotonic()
            break
        
        current_window = 0
        current_chunk_in_window=0
        chunks = []
        window_chunks=[]

        # check if there are frames
        while nrf.data_ready() and ((tac - tic) < timeout) and (current_window < total_wind):
            tac = time.monotonic()

            payload_pipe = nrf.data_pipe()

            packet = nrf.get_payload()

            chunk: bytes = struct.unpack(f"<{nrf.get_payload_size()}s", packet)[0]

            if current_chunk_in_window == 0 :
                ... # get window ID (ident)
                # check that ident == current_window + manage if transmitter didn't recive ACK
                # get chunk part (chunk=chunkpart)

            window_chunks.append(chunk)
            current_chunk_in_window +=1
            SUCC(f"Received chunk {current_chunk_in_window}/{WINDOW_SIZE} for window {current_window+1}")
            
            # if window completed
            if (current_window != total_wind-1) and (current_chunk_in_window == WINDOW_SIZE):
                # --- SEND ACK --------------------------------
                nrf.power_up_tx()                   
                _send_ack_packet()                  
                nrf.power_up_rx()                 
                # ---------------------------------------------
                current_window +=1
                chunks.extend(window_chunks)
                window_chunks.clear()
                SUCC(f"ACK send for window {current_window} / {total_wind}")
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

        # time.sleep(.1)

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
