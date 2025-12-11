# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from nrf24 import (
    NRF24,

    RF24_DATA_RATE,
    RF24_PA,
    RF24_RX_ADDR,
    RF24_PAYLOAD,
    RF24_CRC,
)

from pathlib import Path
import pigpio

import time
import sys
import os

from math import ceil

from hashlib import shake_256

from enum import Enum

import argparse

os.system("sudo pigpiod")
os.system("clear")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
CE_PIN_A                    = 22
CE_PIN_B                    = 25
RECEIVER_TIMEOUT_S          = 20
BYTES_IN_FRAME              = 31
channel_read_timeout        = 1
PERSEVERANCE                = 5
channel_permanence_timeout  = 1
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: HELPER FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def RED(message: str)    -> str: return f"\033[31m{message}\033[0m"
def GREEN(message: str)  -> str: return f"\033[32m{message}\033[0m"
def YELLOW(message: str) -> str: return f"\033[33m{message}\033[0m"
def BLUE(message: str)   -> str: return f"\033[34m{message}\033[0m"

def ERROR(message: str) -> None: print(f"{RED('[~ERR]:')} {message}")
def SUCC(message: str)  -> None: print(f"{GREEN('[SUCC]:')} {message}")
def WARN(message: str)  -> None: print(f"{YELLOW('[WARN]:')} {message}")
def INFO(message: str)  -> None: print(f"{BLUE('[INFO]:')} {message}")
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: NODE CONFIG ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def get_id() -> str:
    id = Path("~/node_id").expanduser().resolve().read_text().strip()
    INFO(f"ID detectao {id}")
    return id

def get_CE_pin(id) -> int:

    if   id == "tan0" or id == "tan1":
        pin = CE_PIN_A
    elif id == "tbn0" or id == "tbn1":
        pin = CE_PIN_B

    return pin

class Role(Enum):
    TRANSMITTER = "TRANSMITTER"
    RECEIVER    = "RECEIVER"

    def __str__(self: "Role") -> str:
        return self.value

def choose_node_role() -> Role:
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

def disable_auto_ack(nrf: NRF24):
    nrf.unset_ce()
    nrf._nrf_write_reg(nrf.EN_AA, 0x00)   # <<< disable auto-ack for all pipes
    nrf.set_ce()

    nrf.set_retransmission(0, 0)  # <<< disable auto-retransmissions (x+1) * 250 µs

def create_radio_object(CE_PIN) -> NRF24:
    # pigpio
    hostname = "localhost"
    port     = 8888

    pi = pigpio.pi(hostname, port)
    if not pi.connected:
        ERROR("Not connected to Raspberry Pi, exiting")
        sys.exit(1)

    # radio object
    nrf = NRF24(
        pi            = pi,
        ce            = CE_PIN,
        spi_speed     = 10e6,
        data_rate     = RF24_DATA_RATE.RATE_2MBPS,
        channel       = 76,
        payload_size  = RF24_PAYLOAD.DYNAMIC,
        address_bytes = 4,
        crc_bytes     = RF24_CRC.BYTES_2,
        pa_level      = RF24_PA.MIN,
    )

    address = b"NMND"
    nrf.open_writing_pipe(address)
    nrf.open_reading_pipe(RF24_RX_ADDR.P1, address)

    disable_auto_ack(nrf)
    
    INFO(f"Radio details:")
    nrf.show_registers()

    return nrf
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



# :::: USB IO :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
USB_MOUNT_PATH = Path("/media")

def get_usb_mount_path() -> Path | None:
    """
    Try to find a valid USB device connected to the USB mount path
    """
    
    for path, _, _ in USB_MOUNT_PATH.walk():
        if path.is_mount():
            return path

    return None

def find_valid_txt_file_in_usb(usb_mount_path: Path) -> Path | None:
    """
    Searches for all the txt files in the first level of depth of the USB mount
    location and returns the path to first one ordered alphabetically
    """
    if not usb_mount_path:
        return None
    
    file = [
        file
        for file in usb_mount_path.iterdir()
        if file.is_file()
        and file.suffix == ".txt"
        and not str(file).startswith(".")
    ]

    file = sorted(file)

    if not file:
        return None

    return file[0].resolve()
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# :::: CHANNELS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def get_channels_based_on_node_id(all_channels: list[int], id) -> tuple[list[int], list[int]]:

    if   id == "tan0":
        offset = 0
    elif id == "tan1":
        offset = 1
    elif id == "tbn0":
        offset = 2
    elif id == "tbn1":
        offset = 3

    INFO(f"MI OFFSET ES {offset}")
    own_channels = all_channels[offset : -1 : 4]
    other_channels = all_channels.copy()

    for channel in own_channels:
        other_channels.remove(channel)

    return own_channels, other_channels

def is_channel_free(nrf: NRF24) -> int:
    return nrf._nrf_read_reg(NRF24.RPD, 1)[0] & 1

def choose_free_channel(nrf: NRF24, own_channels: list[int]) -> int:
    nrf.power_up_rx()

    INFO("CALLARSE QUE QUIERO ELEGIR UN CANAL PA TRANSMITIR")
    number_of_cycles    = 10
    channel_occupability = [
        0 for _ in own_channels
    ]
    for i in range(number_of_cycles):
        for idx, channel in enumerate(own_channels):
            nrf.set_channel(channel)
            time.sleep(.2)
            channel_occupability[idx] += is_channel_free(nrf)

    selected = own_channels[0]
    n        = number_of_cycles + 1
    for occ, channel in zip(channel_occupability, own_channels):
        if occ < n:
            selected = channel
            n        = occ

    INFO(f"POS TRANSMITO EN EL CANAL {selected}")
    INFO(F"LA OKUPABILIDAD DE ESE CANAL ES {n}")
    return selected
            
def choose_occupied_channel(nrf: NRF24, other_channels: list[int], channel_idx) -> tuple[int, int]:
    nrf.power_up_rx()

    #channel_idx = 0
    INFO("CALLARSE QUE ESTOY ESCUCHANDO CANALES")
    while True:
        channel = other_channels[channel_idx % len(other_channels)]
        INFO(f"TOY PROBANDO EN EL CANAL {channel} ")
        tic = time.time()
        tac = time.time()
        INFO(f"TOY PROBANDO 2 EN EL CANAL {channel} ")
        while (tac - tic) < channel_read_timeout:
            tac = time.time()
            nrf.set_channel(channel)
            time.sleep(.2)

            if not nrf.data_ready(): continue
            
            INFO(f"POS ESCUCHO EN EL CANAL {channel}")
            return channel, channel_idx

        channel_idx += 1
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# :::: FLOW FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def ACT_AS_TX(nrf: NRF24, content: bytes, own_channels: list[int], first_node) -> None:
    INFO("SOY UN TRANSMISOR PUTA")
    channel = choose_free_channel(nrf, own_channels)
    nrf.set_channel(channel)
    
    # split the bytes into frames with a FrameID
    frames = [
        FrameID.to_bytes(1) + content[i : i + BYTES_IN_FRAME]
        for FrameID, i in enumerate(range(0, len(content), BYTES_IN_FRAME))
    ]

    control_message  = bytes()
    control_message += 0xFF.to_bytes(1)              # Header reserved to control messages
    control_message += shake_256(content).digest(29) # Checksum of the file
    control_message += len(content).to_bytes(2)      # Ammount of data to transmit

    cycle = []
    cycle.append(control_message)
    cycle.extend(frames)

    INFO(f"Cycle: {cycle}")

    cycle_len = len(cycle)

    idx = 0
    while True:
        message = cycle[idx % cycle_len]
        nrf.send(message)
        idx += 1

        if not first_node:
            usb_mount_path = get_usb_mount_path()
            if usb_mount_path:
                INFO("SE HA ENCONTRADO UN USB PA GUARDAR LAS COSAS ERMANIKO") 
                (usb_mount_path / "file_received").write_bytes(content)
                first_node = True #No es que sea el primer nodo, pero como ya ha guardado el archivo lo pongo en TRUE porque ya ha cumplido su funcion
                SUCC("ARCHIVO GUARDADO EN EL USB")

    return

def ACT_AS_RX(nrf: NRF24, other_channels: list[int]) -> bytes:
    INFO("SOY UN RECEPTOR")
    channel, channel_idx = choose_occupied_channel(nrf, other_channels, 0)
    nrf.set_channel(channel)

    checksum           = None
    is_reading_frames  = False
    slot_not_generated = True
    slots              = []

    file_received = False

    tries = 0

    tic = time.time()
    
    while not file_received:
        
        if not nrf.data_ready():
            tac = time.time()
            if (tac- tic) > channel_permanence_timeout:
                INFO("VOY A PROBAR A CAMBIAR DE CANAL PORK ESTE VA TO MAL")
                channel, channel_idx = choose_occupied_channel(nrf, other_channels, channel_idx+1)
                nrf.set_channel(channel)
            continue

        frame: bytes = nrf.get_payload()

        if frame[0] == 0xFF:
            _        = frame[0]
            checksum = frame[1:30]
            data_len = int.from_bytes(frame[30:32])

            num_of_frames = ceil(data_len / BYTES_IN_FRAME)

            if slot_not_generated:
                slots = [
                    bytes()
                    for _ in range(num_of_frames)
                ]

                slot_not_generated = False

            is_reading_frames = True



        if is_reading_frames and (frame[0] < 0xFF):
            FrameID        = frame[0]
            slots[FrameID] = frame[1:]



        if is_reading_frames and (frame[0] == num_of_frames - 1):
            computed_checksum = shake_256(b"".join(slots)).digest(29)

            if computed_checksum == checksum:
                SUCC("EL CHESUM TA TO BIEN PRIMIKO")
                return b"".join(slots)

            else:
                WARN("EL CHESUM TA MAL LOKO")
                
                tries += 1
                if tries >= PERSEVERANCE:
                    INFO("VOY A PROBAR A CAMBIAR DE CANAL PORK ESTE VA TO MAL")
                    channel, channel_idx = choose_occupied_channel(nrf, other_channels, channel_idx+1)
                    nrf.set_channel(channel)
                    
        tic = time.time()
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: MAIN :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def main():
    """
    Main flow of the application
    """
    parser = argparse.ArgumentParser(description="NRF24 Network Mode")
    parser.add_argument(
        "--first",
        action="store_true",      # Si se pone la flag vale True, si no False
        help="Declara el nodo como el primero",
    )
    args = parser.parse_args()

    id = get_id()
    CE_PIN = get_CE_pin(id)

    nrf            = create_radio_object(CE_PIN) 
    usb_mount_path = get_usb_mount_path()
    file_path      = find_valid_txt_file_in_usb(usb_mount_path)

    all_channels   = [channel for channel in range(0, 115 + 1, 5)]
    own_channels, other_channels = get_channels_based_on_node_id(all_channels,id)

    INFO(f"OWN CHANNELS: {own_channels}")
    INFO(f"OTHER CHANNELS: {other_channels}")

    content        = None

    first_node = args.first

    if first_node:
        last_msg = None
        msg = None
        INFO("ESTE NODO HA SIDO ESCOGIDO COMO EL PRIMERO Y UNICO DE SU ESPECIE")
        while not file_path:
            if not usb_mount_path:
                msg = "ESPERANDO A QUE SE INTRODUZCA UN USB..."
            else:
                msg = "NO SE ENCUENTRA NINGUN ARCHIVO EN EL USB"

            if msg is not None and msg != last_msg:
                INFO(msg)
                last_msg = msg
            
            usb_mount_path = get_usb_mount_path()
            file_path      = find_valid_txt_file_in_usb(usb_mount_path)


        INFO("HAY UN USB CON UN ARCHIVO DENTRO")
        INFO(f"SE HA ENCONTRADO EL SIGUIENTE ARCHIVO:{file_path}")
        content = file_path.read_bytes()
        ACT_AS_TX(nrf, content, own_channels, first_node)

    else:
        INFO("NO HE SIDO ESCOGIDO COMO EL PRIMERO :((")
        #La parte de codigo dentro de el if se deberia de eliminar y dejar solo la del else, de momento lo dejo igual
        #porque no deberia de dar problemas, y en caso de que no se quiera hacer servir el script con la logica de settear el primer nodo
        #y esperar al usb, va a funcionar igual. De tal manera que, se utilice la logica del primer nodo o no, funciona.
        if file_path:
            INFO("HAY UN USB CON UN ARCHIVO DENTRO")
            INFO(f"SE HA ENCONTRADO EL SIGUIENTE ARCHIVO:{file_path}")
            content = file_path.read_bytes()
            first_node = True #Esta linea habilita el uso de las dos logicas
            ACT_AS_TX(nrf, content, own_channels, first_node)
        else:
            INFO("NO HAY UN USB CON UN ARCHIVO DENTRO")
            content = ACT_AS_RX(nrf, other_channels)
            usb_mount_path = get_usb_mount_path()
            if usb_mount_path:
                INFO("SE HA ENCONTRADO UN USB PA GUARDAR LAS COSAS ERMANIKO") 
                (usb_mount_path / "file_received").write_bytes(content)
                first_node = True #No es que sea el primer nodo, pero como ya ha guardado el archivo lo pongo en TRUE porque ya ha cumplido su funcion
                SUCC("ARCHIVO GUARDADO EN EL USB")
            else:
                INFO("NO SA ENCONTRAO EL USB PA GUARDAR, LO GUARDO POR AHI")
                Path("file_received").write_bytes(content)
            ACT_AS_TX(nrf, content, own_channels, first_node)

    nrf.power_down()
    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::




if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        ERROR("Process interrupted by the user")
    finally:
        os.system("sudo killall pigpiod")