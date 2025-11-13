"""
##### Application Layer
Interface between the user and the Transfer protocol.

##### Diagram
```
IO
                        ┌───────────────────┐
RADIO CONFIGURATION  ─> │                   │ ─> RADIO OBJECT
   FILE TO TRANSMIT* ─> │                   │ ─> BYTES TO TRANSMIT*
                        │ APPLICATION LAYER │
      FILE TO STORE* <─ │                   │ <─ BYTES TO STORE*
                        └───────────────────┘
```

##### Inputs
- RADIO CONFIGURATION: Configuration parameters for the radio object
- FILE TO TRANSMIT*: Path to a file to be transmitted (only for TX mode)
- BYTES TO STORE*: Bytes received to be stored in a file (only for RX mode)

##### Outputs
- RADIO OBJECT: Configured radio object ready to transmit/receive data
- BYTES TO TRANSMIT*: Bytes read from the file to be transmitted (only for TX mode)
- FILE TO STORE*: Path to the file where the received bytes will be stored (only for RX mode)
"""





# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
from enum import Enum
import argparse

from nrf24 import (
    RF24_DATA_RATE,
    RF24_PA,
    RF24_CRC,
)

from ..trx import Trx

from ..utils import Logger
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: ARGUMENT PARSING :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
class Mode(Enum):
    TX    = "TX"
    RX    = "RX"

    def __str__(self: "Mode") -> str:
        return self.value



def parse_arguments(mode_name: str) -> dict:
    """
    Parses the user input to configure the radio object and the transfer parameters
    """

    parser = argparse.ArgumentParser(description = mode_name)

    # configure the possible communication arguments
    parser.add_argument(
        "--mode",
        type     = str,
        choices  = ["TX", "RX"],
        required = True,
        help     = "Selects the operation mode: Transmitter (TX) or Receiver (RX)",
    )

    parser.add_argument(
        "--file-path-tx",
        type    = str,
        default = None,
        help    = "Sets the path to the file to be transmitted (default: None)",
    )

    parser.add_argument(
        "--file-path-rx",
        type    = str,
        default = None,
        help    = "Sets the path to the file where the received bytes will be stored (default: None)",
    )

    parser.add_argument(
        "--ce-pin",
        type    = int,
        choices = range(32),
        default = 22,
        help    = "Sets the GPIO pin used for CE (default: 22)",
    )

    parser.add_argument(
        "--channel",
        type    = int,
        default = 76,
        choices = range(126),
        help    = "Sets the RF channel to be used (default: 76)",
    )

    parser.add_argument(
        "--data-rate",
        type    = str,
        choices = ["250KBPS", "1MBPS", "2MBPS"],
        default = "1MBPS",
        help    = "Sets the RF data rate (default: 1MBPS)",
    )

    parser.add_argument(
        "--pa-level",
        type    = str,
        choices = ["MIN", "LOW", "HIGH", "MAX"],
        default = "MIN",
        help    = "Sets the Power Amplifier level (default: MIN)",
    )

    parser.add_argument(
        "--crc-bytes",
        type    = int,
        choices = [0, 1, 2],
        default = 2,
        help    = "Sets the number of CRC bytes (default: 2)",
    )

    parser.add_argument(
        "--retransmission-tries",
        type    = int,
        choices = range(16),
        default = 15,
        help    = "Sets the number of retransmission tries (default: 15)",
    )

    parser.add_argument(
        "--retransmission-delay",
        type    = int,
        choices = range(16),
        default = 2,
        help    = "Sets the retransmission delay (default: 2)",
    )

    parser.add_argument(
        "--autostart",
        action = "store_true",
        help   = "If set, the transmission/reception will start automatically without user input",
    )

    parser.add_argument(
        "--print-config",
        action = "store_true",
        help   = "If set, the radio configuration will be printed before starting",
    )

    # parse the user input
    args = parser.parse_args()

    # NOTE: some arguments require processing before generating the radio object
    args.mode = Mode(args.mode)

    if args.file_path_tx:
        args.file_path_tx = Path(args.file_path_tx).resolve()

    if args.file_path_rx:
        args.file_path_rx = Path(args.file_path_rx).resolve()

    if args.data_rate == "250KBPS":
        args.data_rate = RF24_DATA_RATE.RATE_250KBPS
    elif args.data_rate == "1MBPS":
        args.data_rate = RF24_DATA_RATE.RATE_1MBPS
    elif args.data_rate == "2MBPS":
        args.data_rate = RF24_DATA_RATE.RATE_2MBPS

    if args.pa_level == "MIN":
        args.pa_level = RF24_PA.MIN
    elif args.pa_level == "LOW":
        args.pa_level = RF24_PA.LOW
    elif args.pa_level == "HIGH":
        args.pa_level = RF24_PA.HIGH
    elif args.pa_level == "MAX":
        args.pa_level = RF24_PA.MAX

    if args.crc_bytes == 0:
        args.crc_bytes = RF24_CRC.DISABLED
    elif args.crc_bytes == 1:
        args.crc_bytes = RF24_CRC.BYTES_1
    elif args.crc_bytes == 2:
        args.crc_bytes = RF24_CRC.BYTES_2

    if args.print_config:
        Logger.INFO(f"Radio Configuration:")
        Logger.INFO(f"Operation Mode: {args.mode}")
        Logger.INFO(f"File Path TX: {args.file_path_tx}")
        Logger.INFO(f"File Path RX: {args.file_path_rx}")
        Logger.INFO(f"CE Pin: {args.ce_pin}")
        Logger.INFO(f"Channel: {args.channel}")
        Logger.INFO(f"Data Rate: {args.data_rate}")
        Logger.INFO(f"PA Level: {args.pa_level}")
        Logger.INFO(f"CRC Bytes: {args.crc_bytes}")
        Logger.INFO(f"Retransmission Tries: {args.retransmission_tries}")
        Logger.INFO(f"Retransmission Delay: {args.retransmission_delay}")
        Logger.INFO(f"Autostart: {'Enabled' if args.autostart else 'Disabled'}")

    return vars(args)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: FILE IO FUNCTIONS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
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



def load_file_bytes(file_path: Path) -> bytes | None:
    """
    Loads the content of a file and returns it as bytes
    """

    # NOTE: If no specific file_path is provided, then we will try to find a valid
    # .txt file inside a mounted USB device
    if not file_path:
        file_path = find_valid_txt_file_in_usb(get_usb_mount_path())

    # NOTE: If after looking for a valid .txt file we still don't have a valid path,
    # then we fallback to a predefined test file inside the project directory
    if not file_path:
        fallback_dir = Path("test_files")
        file_path = fallback_dir / "quijote.txt"

        Logger.WARN(f"USB file candidate not found, using fallback file: {file_path}")

    # NOTE: If the file does not exist, raise an error and exit
    if not file_path.exists():
        Logger.ERROR(f"File not found: {file_path}")
        return None
    
    return file_path.read_bytes()



def store_file_bytes(file_path: Path, data: bytes) -> bool:
    """
    Stores the given bytes into a file at the specified path
    """

    if not file_path:
        file_path = get_usb_mount_path()

    if not file_path:
        fallback_dir = Path("received_files")
        fallback_dir.mkdir(exist_ok = True)

        file_path = fallback_dir / Logger.timestamp()
    
    else:
        file_path = file_path / Logger.timestamp()

    try:
        file_path.write_bytes(data)
        Logger.SUCC(f"Stored {len(data)} bytes into file: {file_path}")
        return True

    except Exception as e:
        Logger.ERROR(f"Failed to store bytes into file: {file_path} ({e})")
        return False
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# :::: RADIO CONFIG FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def configure_radio_object(config: dict) -> "Trx.TRX":
    """
    Configures and returns a CustomNRF24 radio object based on the provided
    configuration dictionary
    """

    radio = Trx.TRX(
        MODE                 = config["mode"],
        CE_PIN               = config["ce_pin"],
        CHANNEL              = config["channel"],
        DATA_RATE            = config["data_rate"],
        PA_LEVEL             = config["pa_level"],
        CRC_BYTES            = config["crc_bytes"],
        RETRANSMISSION_TRIES = config["retransmission_tries"],
        RETRANSMISSION_DELAY = config["retransmission_delay"],
    )

    return radio
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::     