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

ACK_TIMEOUT_S = 500e-6          # <<< tiempo máx esperando ACK manual (500 us)
MAX_ATTEMPTS  = 3             # <<< reintentos por paquete (puedes ajustar)
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


# global payload 
nrf.set_payload_size(32) # [1 - 32] Bytes
payload:list[bytes] = []

PAYLOAD_SIZE = nrf.get_payload_size()   # <<< atajo local





# auto-retries
# nrf.set_retransmission(1, 15) #Retransmitting (1+1)*250ms and just 15 times will try it. Automatic ACKs


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


# === DESACTIVATE AUTO-ACK DE HARDWARE (EN_AA=0) TO MAKE ACKz MANUALS=========
def _disable_auto_ack(nrf_obj):
    nrf_obj.unset_ce()
    nrf_obj._nrf_write_reg(nrf_obj.EN_AA, 0x00)   # <<< disable auto-ack for all pipes
    nrf_obj.set_ce()

    nrf_obj.set_retransmission(0, 0)  # <<< disable auto-retransmissions (x+1) * 250us

_disable_auto_ack(nrf) 
# =================================================================================


# status visualization
INFO(f"Radio details:")
nrf.show_registers()
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: FLOW FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def _send_ack_packet() -> None:
    ack = b"ACK"                                    # build 32B: "ACK" 
    nrf.send(ack)                                   # send (this flips radio to TX)
    nrf.wait_until_sent()                           # block until TX done (radio goes back to RX)


def _wait_for_ack(timeout_s: float) -> bool:
    t0 = time.monotonic()                           # start a small timeout window
    while (time.monotonic() - t0) < timeout_s:      # poll until we hit the timeout
        if nrf.data_ready():                        # got something in RX FIFO
            payload = nrf.get_payload()             # read & clear one frame from FIFO
            print(payload)
            print(b"ACK")
            # sanity: ensure we have at least 3 bytes and they are "ACK"
            if len(payload) >= 3 and bytes(payload[:3]) == b"ACK":
                return True                         # legit ACK → success
            else:
                ERROR(f"Invalid ACK payload: {bytes(payload[:8])!r}")  # not an ACK, keep waiting
                # note: we keep looping to allow the real ACK to arrive within the timeout
        else:
            print("ERRIRR no data ready")
       
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


        # split the contents into chunks
        chunks = [
            content[i:i+nrf.get_payload_size()]
            for i in range(0, content_len, nrf.get_payload_size())
        ]
        chunks_len = len(chunks) # Total number of chunks

        packets = []
        for chunk in chunks:
            packets.append(struct.pack(f"<{nrf.get_payload_size()}s", chunk))

        # We start transmitting                       
        start = time.monotonic()          
        for idx in range(chunks_len):
            attempt = 1
            sent_ok = False

            while attempt <= MAX_ATTEMPTS:          # Manual attempts
                INFO(f"Sending packet #{idx} (attempt {attempt}): {chunks[idx]} --> {packets[idx]}")
           
                nrf.send(packets[idx])              # Data send

                try:
                    tic = time.monotonic_ns()
                    nrf.wait_until_sent()           # Wait until sent
                    tac = time.monotonic_ns()
                except TimeoutError:
                    ERROR("Timeout while transmitting")

                # Wait for the reception of the ACK
                got_ack = _wait_for_ack(ACK_TIMEOUT_S)    # Listen to RX for ACK

                if got_ack:
                    ack_rtt_ms = (time.monotonic() - start) * 1000.0  # RTT of the manual ACK
                    SUCC(f"[ACK] pkt#{idx} ok | app_retries={attempt-1} | tx_time={(tac - tic)/1000:.1f} us | ack_rtt={ack_rtt_ms:.2f} ms")
                    sent_ok = True
                    break
                else:
                    ERROR(f"No manual ACK for packet #{idx}")
                    attempt += 1
                    time.sleep(0.005)

            if not sent_ok:
                ERROR(f"Giving up packet #{idx} after {MAX_ATTEMPTS} attempts")
                # break

            time.sleep(0.2) 

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

        chunks = []
        while (tac - tic) < timeout:
            tac = time.monotonic()

            # check if there are frames
            while nrf.data_ready():

                payload_pipe = nrf.data_pipe()

                packet = nrf.get_payload()

                chunk: bytes = struct.unpack(f"<{nrf.get_payload_size()}s", packet)[0]
                chunks.append(chunk)
                
                SUCC(f"Received {len(chunk)} bytes on pipe {payload_pipe}: {packet} --> {chunk}")

                # --- SEND ACK --------------------------------
                nrf.power_up_tx()                   
                _send_ack_packet()                  
                nrf.power_up_rx()                 
                # -----------------------------------------------------------------

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
