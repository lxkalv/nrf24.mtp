# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from pathlib import Path
import shutil
import sys
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: COLORING FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
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
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: MESSAGING FUNCTIONS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def ERROR(message: str, end = "\n") -> None:
    """
    Prints a message to the console with the red prefix `[~ERR]:`
    """
    print(f"{RED('[~ERR]:')} {message}", end = end)

def SUCC(message: str, end = "\n") -> None:
    """
    Prints a message to the console with the green prefix `[SUCC]:`
    """
    print(f"{GREEN('[SUCC]:')} {message}", end = end)

def WARN(message: str, end = "\n") -> None:
    """
    Prints a message to the console with the yellow prefix `[WARN]:`
    """
    print(f"{YELLOW('[WARN]:')} {message}", end = end)

def INFO(message: str, end = "\n") -> None:
    """
    Prints a message to the console with the blue prefix `[INFO]:`
    """
    print(f"{BLUE('[INFO]:')} {message}", end = end)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: PROGRESS FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
SPINNER     = "⣾⣽⣻⢿⡿⣟⣯⣷"
IDX_SPINNER = [0]

def reset_line() -> None:
    """
    Delete the last CMD line
    """

    print("\x1b[2K\r", end = "")
    
    return

def progress_bar(pending_msg: str, finished_msg: str, current_status: int, finished_status: int) -> None:
    """
    Create a progress bar that gets updated everytime this function is called
    """
    
    terminal_width  = shutil.get_terminal_size().columns
    IDX_SPINNER[0] += 1
    spin            = SPINNER[IDX_SPINNER[0] % len(SPINNER)]
    progress        = f"({current_status}/{finished_status}) {spin}"

    if current_status < finished_status:
        progress = f"({current_status}/{finished_status}) {spin}"

        reset_line()
        INFO(f"{pending_msg} {progress.rjust(terminal_width - 8 - len(pending_msg) - 2)}", end = "")
        sys.stdout.flush()
        
    else:
        progress = f"({current_status}/{finished_status}) █"

        reset_line()
        SUCC(f"{finished_msg} {progress.rjust(terminal_width - 8 - len(finished_msg) - 2)}")
    
    return

def status_bar(message: str, status: str) -> None:
    """
    A bar that displays the status of one operation
    """

    terminal_width  = shutil.get_terminal_size().columns
    IDX_SPINNER[0] += 1
    spin            = SPINNER[IDX_SPINNER[0] % len(SPINNER)]

    reset_line()
    if status == "INFO":
        progress = spin

        INFO(f"{message} {progress.rjust(terminal_width - 8 - len(message) - 1)}", end = "")
        sys.stdout.flush()

    elif status == "WARN":
        progress = spin

        WARN(f"{message} {progress.rjust(terminal_width - 8 - len(message) - 1)}", end = "")
        sys.stdout.flush()

    elif status == "SUCC":
        progress = "█"

        SUCC(f"{message} {progress.rjust(terminal_width - 8 - len(message) - 2)}")

    elif status == "ERROR":
        progress = "X"

        ERROR(f"{message} {progress.rjust(terminal_width - 8 - len(message) - 2)}")
        
    return


# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: USB IO FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
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
