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

ACK_TIMEOUT_S = 0.15  # por ejemplo 150 ms         # <<< max time waiting for manual ACK (500 µs)
MAX_ATTEMPTS  = 1000               # <<< per-packet retries (you can adjust)

ID_WIND_BYTES=3
ID_CHUNK_BYTES=1
PAYLOAD_SIZE=32
WINDOW_SIZE = 3
SEQ_START   = 1        # first packet ID
ACK_WAIT   = 0.005      # time to wait for an ACK (s)
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
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



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

# :::: RADIO CONFIG :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

# pigpio
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
nrf.set_payload_size(-1) # [1 - 32] Bytes, NOTE: 0 is dynamic
payload:list[bytes] = []



# auto-retries
nrf.set_retransmission(1, 15) # Retransmitting (1+1)*250ms and just 15 times will try it. Automatic ACKs


# Tx/Rx addresses
nrf.set_address_bytes(4) # [2 - 5] Bytes





def enable_noack_command():
    # Read feature
    feature = nrf._nrf_read_reg(nrf.FEATURE, 1)[0]
    # Put 1 to the bit EN_DYN_ACK 
    feature |= nrf.EN_DYN_ACK
    # Escribir de nuevo FEATURE
    nrf._nrf_write_reg(nrf.FEATURE, feature)

def send_no_ack(data):
        # We expect a list of byte values to be sent.  However, popular types
        # such as string, integer, bytes, and bytearray are handled automatically using
        # this conversion code.
        if not isinstance(data, list):
            if isinstance(data, str):
                data = list(map(ord, data))
            elif isinstance(data, int):
                data = list(data.to_bytes(-(-data.bit_length() // 8), 'little'))              
            else:
                data = list(data)
        
        # Flush TX if buffers are full or max retries is set.
        status = nrf.get_status()
        if status & (nrf.TX_FULL | nrf.MAX_RT):
            nrf.flush_tx()

        if nrf._payload_size >= RF24_PAYLOAD.MIN:  # fixed payload
            data = nrf._make_fixed_width(data, nrf._payload_size, nrf._padding)

        nrf._nrf_command([nrf.W_TX_PAYLOAD_NO_ACK] + data)
        nrf.power_up_tx()

# =================================================================================

# :::: FLOW FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



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

def send_DATA_message(nrf : NRF24, DATA_MESSAGE : bytes, message_type) -> None:
        """
        Continuously send a given data message until we receive the expected ACK
        """
        message_has_been_sent = False
        packets_lost          = 0

        while not message_has_been_sent:
            # status_bar(f"Sending DATA message: {PageID:02d}|{BurstID:03d}|{ChunkID:03d}|{packets_lost}", "INFO")
            
            nrf.flush_rx()
            # nrf.flush_tx()
            nrf.reset_packages_lost()
            nrf.send(DATA_MESSAGE)
            
            try:
                nrf.wait_until_sent()
            except TimeoutError:
                INFO(f"Time-out while sending DATA message {message_type}, retrying")
                packets_lost += 1
                continue
            
            if nrf.get_packages_lost() == 0:
                message_has_been_sent = True
            
            else:
                #time.sleep(250e-6 * nrf.RETRANSMISSION_DELAY)
                packets_lost += 1

        return


def tx_empty(nrf : NRF24):
    fifo_status = nrf._nrf_read_reg(nrf.FIFO_STATUS, 1)[0]
    is_empty = bool(fifo_status & nrf.FTX_EMPTY) # Bit TX_EMPTY (0x10)
    return is_empty




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
        chunks = []
        start_val = 0
        chunk_id = 0

        # Create the frame depending on the chunk of the window
        while start_val < content_len:
            if chunk_id % WINDOW_SIZE == 0:
                size = PAYLOAD_SIZE - ID_CHUNK_BYTES - ID_WIND_BYTES
                end_val = min(start_val + size, content_len)
                ident_wind = (chunk_id // WINDOW_SIZE).to_bytes(ID_WIND_BYTES, "big")
                ident_chunk = (chunk_id % WINDOW_SIZE).to_bytes(ID_CHUNK_BYTES, "big")
                final_content = ident_chunk + ident_wind + content[start_val:end_val]
            else:
                size = PAYLOAD_SIZE - ID_CHUNK_BYTES
                end_val = min(start_val + size, content_len)
                ident_chunk = (chunk_id % WINDOW_SIZE).to_bytes(ID_CHUNK_BYTES, "big")
                final_content = ident_chunk + content[start_val:end_val]

            chunks.append(final_content)
            start_val = end_val
            chunk_id += 1

        # Creating and sending the first packet (HEADER)
        total_wind = math.ceil(chunk_id / WINDOW_SIZE)
        last_window_size = chunk_id % WINDOW_SIZE if (chunk_id % WINDOW_SIZE) != 0 else WINDOW_SIZE
        header = total_wind.to_bytes(ID_WIND_BYTES, "big") + last_window_size.to_bytes(1, "big")

        # store the encoded bytes
        packets = [struct.pack(f"<{len(chunk)}s", chunk) for chunk in chunks]
        
        # send Header Message
        send_DATA_message(nrf, struct.pack(f"<{len(header)}s", header), "HEADER")
        while not nrf.data_ready():
            pass

        ack_message = nrf.get_payload()
        print(f"ACK HEADER message recieved correctly. ACK:{ack_message}")

        # Start transmitting                       
        current_window = 0  
        current_chunk = 0    

        while current_window < total_wind:
            start = time.monotonic()
            attempt = 1

            while attempt <= MAX_ATTEMPTS:          # Manual attempts
                INFO(f"Sending window #{current_window} (attempt {attempt}) of the window)")
                window_packet = packets[current_chunk:current_chunk + WINDOW_SIZE]

                ack_ok = False
                ack_message = None

                # limpiamos RX solo al inicio de cada intento, para no mezclar con ACKs viejos
                nrf.flush_rx()

                for p_idx, pkt in enumerate(window_packet):
                    # Último chunk de la ventana (o último de la última ventana)
                    is_last_chunk_of_window = (p_idx == WINDOW_SIZE - 1)
                    is_last_chunk_of_last_window = (
                        current_window == total_wind - 1 and
                        p_idx == (last_window_size - 1)
                    )

                    if is_last_chunk_of_window or is_last_chunk_of_last_window:
                        # Este SÍ espera ACK
                        send_DATA_message(nrf, pkt, current_window)

                        start_wait = time.monotonic()
                        while time.monotonic() - start_wait < ACK_TIMEOUT_S:
                            if not nrf.data_ready():
                                continue

                            raw_ack = nrf.get_payload()

                            # Esperamos un ACK de exactamente ID_WIND_BYTES bytes (3)
                            if len(raw_ack) != ID_WIND_BYTES:
                                INFO(f"Ignoring non-ACK payload {raw_ack}")
                                continue

                            ack_win = int.from_bytes(raw_ack, "big")

                            # Si el receptor dice "tengo hasta la ventana ack_win",
                            # y ack_win >= current_window → damos por buena current_window.
                            if ack_win >= current_window:
                                ack_ok = True
                                ack_message = raw_ack
                                break
                            else:
                                # ACK viejo: receptor confirmando algo que ya dimos por hecho
                                INFO(
                                    f"Ignoring old ACK for window {ack_win} "
                                    f"(already waiting for >= {current_window})"
                                )
                                continue

                        # Si ha salido del while sin encontrar ACK válido,
                        # ack_ok seguirá siendo False y se gestionará fuera del for
                        if not ack_ok:
                            # salimos del for para reintentar la ventana completa
                            break

                    else:
                        # Chunks intermedios sin ACK
                        send_no_ack(pkt)
                        time.sleep(0.0001)  # Small delay between packets

                # Fin del for de los 3 chunks

                if not ack_ok:
                    ERROR(f"No valid ACK for window {current_window}, retrying...")
                    attempt += 1
                    # nrf.flush_rx() ya se hace al inicio del intento siguiente
                    continue

                # ACK correcto para esta ventana
                ack_win_int = int.from_bytes(ack_message, "big")
                print(
                    f"Recieved ACK {ack_message}  // RX says last complete window = "
                    f"{ack_win_int}, current_window = {current_window}"
                )
                ack_rtt_ms = (time.monotonic() - start) * 1000.0
                SUCC(
                    f"[ACK win] chunks {current_chunk}..{current_chunk+WINDOW_SIZE-1} ok "
                    f"| app_retries={attempt} | rtt={ack_rtt_ms:.2f} ms"
                )
                break  # salimos del while attempt<=MAX_ATTEMPTS

            if attempt > MAX_ATTEMPTS:
                ERROR(
                    f"Giving up the transmssion because couldn't be sent the "
                    f"#{current_window} after {MAX_ATTEMPTS} attempts"
                )
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

        # ACK para el HEADER
        nrf.ack_payload(RF24_RX_ADDR.P1, b"OK")  

        INFO("Waiting for header packet...")
        while (tac - tic) < timeout:
            tac = time.monotonic()

            if not nrf.data_ready():
                continue
            
            header_packet = nrf.get_payload()
            raw = header_packet[:ID_WIND_BYTES+1]
            total_wind, last_window_size = struct.unpack(f">{ID_WIND_BYTES}sB", raw)
            total_wind = int.from_bytes(total_wind, "big")

            print(f"Received header packet with total_wind={total_wind} and last_window_size={last_window_size}")
            
            tic = time.monotonic()
            break

        expected_window          = 0   # siguiente ventana que queremos (0, 1, 2, ...)
        extracted_window         = 0   # última ventana decodificada de un paquete
        expected_chunk_in_window = 0   # siguiente chunk esperado dentro de la ventana
        chunks                   = []  # lista global de chunks en orden
        window_chunks            = []  # chunks de la ventana actual
        timer_has_started        = False          

        # check if there are frames
        while ((tac - tic) < timeout) and (expected_window < total_wind):

            tac = time.monotonic()
            while nrf.data_ready():

                if not timer_has_started:
                    throughput_tic = time.monotonic()
                    timer_has_started = True

                packet = nrf.get_payload()

                # Decodificamos ventana y chunk
                extracted_window, extracted_chunk, chunk = _decode_packet(packet, extracted_window)

                # Solo aceptamos la ventana que toca
                if extracted_window != expected_window:
                    ERROR(
                        f"Discarding chunk for window {extracted_window}, "
                        f"expecting window {expected_window}"
                    )
                    # No tocamos window_chunks ni expected_chunk_in_window
                    continue

                # Chunk en orden dentro de la ventana
                if expected_chunk_in_window == extracted_chunk:
                    expected_chunk_in_window += 1
                    window_chunks.append(chunk)
                    SUCC(
                        f"Received chunk {extracted_chunk + 1}/{WINDOW_SIZE} "
                        f"for window {extracted_window}. We are expecting {expected_window}"
                    )

                    # ¿Cuántos chunks debe tener ESTA ventana?
                    is_last_window       = (extracted_window == total_wind - 1)
                    window_size_for_this = last_window_size if is_last_window else WINDOW_SIZE

                    # PRE-CARGA DEL ACK: penúltimo chunk de esta ventana
                    if expected_chunk_in_window == window_size_for_this - 1:
                        # Hemos visto todos menos el último -> preparamos ACK para
                        # cuando llegue el último chunk (el HW lo enviará con él)
                        nrf.flush_tx()
                        nrf.ack_payload(
                            RF24_RX_ADDR.P1,
                            extracted_window.to_bytes(ID_WIND_BYTES, "big")
                        )
                        INFO(
                            f"Preloaded ACK payload for window {extracted_window} "
                            f"(window_size={window_size_for_this})"
                        )

                    # Ventana completa
                    if expected_chunk_in_window == window_size_for_this:
                        SUCC(f"Extracted window Completed {extracted_window}")

                        # Añadimos los chunks de esta ventana al buffer global
                        chunks.extend(window_chunks)

                        # IMPORTANTE:
                        # NO volvemos a llamar a ack_payload aquí. El ACK para esta ventana
                        # ya se ha pre-cargado antes del último chunk y viaja con él.

                        SUCC(
                            f"ACK send for window {extracted_window} / {total_wind}"
                        )

                        # Avanzamos a la siguiente ventana esperada
                        expected_window          += 1
                        expected_chunk_in_window  = 0
                        window_chunks.clear()
                        tic = time.monotonic()

                else:
                    # Chunk fuera de orden dentro de la ventana actual → tiramos la ventana
                    ERROR(
                        f"Received out-of-order chunk (expected {expected_chunk_in_window}, "
                        f"got {extracted_chunk}), discarding window {extracted_window}"
                    )
                    window_chunks.clear()
                    expected_chunk_in_window = 0
                    # No mandamos ACK → el TX hará timeout y retransmitirá toda la ventana
                    tic = time.monotonic()
                    continue

        INFO('Connection timed-out or all chunks recieved')
        throughput_tac = time.monotonic()
        total_time     = throughput_tac - throughput_tic

        content = bytes()
        for chunk in chunks:
            content += chunk

        if len(content) == 0:
            ERROR('Did not receive anything')
            return

        with open("file_received.txt", "wb") as f:
            f.write(content)
        content_len = len(content)
        SUCC(f'Saved {content_len} bytes to: file_received.txt')
        INFO(f"Computed throughput: {((content_len / 1024) / total_time):.2f} KBps")

    finally:
        nrf.power_down()
        pi.stop()

    return


# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: MAIN :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def main():
    """
    Main flow of the application
    """

    role = choose_node_role()
    choose_address_based_on_role(role, nrf)
    enable_noack_command()
    INFO("EN_AA after disabling again:")
    nrf.show_registers()  # opcional, para comprobar que EN_AA=0

    if role is Role.TRANSMITTER:
        BEGIN_TRANSMITTER_MODE()

    elif role is Role.RECEIVER:
        BEGIN_RECEIVER_MODE()

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




if __name__ == "__main__":
    main()