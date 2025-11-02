# :::: LIBRARY IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
import struct
import time
import os

os.system("cls" if os.name == "nt" else "clear")

from radio import (
    Role,

    radio,
)

from tx_flow import FULL_TX_MODE

from utils import (
    ERROR,
    SUCC,
    WARN,
    INFO,
)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





# :::: CONSTANTS/GLOBALS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
RECEIVER_TIMEOUT_S = 20
DATA_SIZE          = 32
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


# :::: FLOW FUNCTIONS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def BEGIN_TRANSMITTER_MODE() -> None:
    """
    Transmits the first txt file found in the mounted USB, the flow of the TX MODE
    is the following:
    
    1. An appropiate file is selected from all the candidate files found in the
    mounted USB. The content of the file is extracted as raw bytes

    2. The bytes are splitted into chunks of size `payload_size` and then packed
    for future transmission

    3. An information message is sent containing the number of frames that the
    receiver should expect

    4. The rest of the frames are sent in a stop & wait fashion
    """

    INFO("Starting transmission")

    try:
        FULL_TX_MODE(radio)

    except KeyboardInterrupt:
        ERROR("Process interrupted by user")

    finally:
        radio.power_down()
        radio.pi_custom.stop()
    
    return





def BEGIN_RECEIVER_MODE() -> None:
    """
    Receives multiple frames from a transmitter and reassembles the blocks into a
    `txt` file, the location of the `txt` depends on if there is a mounted USB or
    not. The flow of the RX MODE is the following:

    1. Start the timer that will interrupt the receiving process if there has not
    been any frame for `timeout` seconds

    2. Start listening the channel for frames. The first frame is treated
    differently as it contains the number of frames that the receiver will expect

    3. Start listening for the regular data frames

    4. After all the frames has been received (or connection has timed-out), we
    merge the payloads into one chunk of data and store it in the mounted USB. If
    there is no mounted USB then the file is stored in memory
    """

    INFO(f"Starting reception: {RECEIVER_TIMEOUT_S} seconds time-out")

    try:
        # list that will contain all the received chunks
        chunks:list[str] = []


        # wait for the first frame of the communication containing the expected number
        # of frames and extract its contents
        INFO("Waiting for header packet...")
        while not nrf.data_ready():
            pass

        header_packet = nrf.get_payload()
        total_chunks  = struct.unpack("i", header_packet[:4])[0] # NOTE: the default size of an int is 4 bytes
        SUCC(f"Header received: expecting {total_chunks} chunks")


        # start listening for frames
        received_chunks   = 0 # NOTE: not the ID
        timer_has_started = False

        tic = time.monotonic()
        tac = time.monotonic()
        while received_chunks < total_chunks and (tac - tic) < RECEIVER_TIMEOUT_S:
            tac = time.monotonic()

            # check if there are frames
            while nrf.data_ready():

                if not timer_has_started:
                    throughput_tic = time.monotonic()
                    timer_has_started = True

                packet = nrf.get_payload()

                chunk = struct.unpack(f"<{len(packet)}s", packet)[0] # NOTE: the struct.unpack method returs more things than just the data
                chunks.append(chunk)
                

                # display the progress of the transmission
                received_chunks += 1

                if received_chunks % 100 == 0 or received_chunks == total_chunks:
                    progress_bar(
                        active_msg     = f"Receiving chunks",
                        finished_msg   = f"All chunks received",
                        current_status = received_chunks,
                        max_status     = total_chunks,
                    )
            
                tic = time.monotonic()
            
        throughput_tac = time.monotonic()
        chunks_len     = len(chunks)


        if received_chunks != total_chunks:
            total_time = throughput_tac - throughput_tic - RECEIVER_TIMEOUT_S
            WARN("Connection timed-out")
        
        else:
            total_time = throughput_tac - throughput_tic
        

        if chunks_len == 0:
            ERROR("Did not receive anything")
            return
        
        
        # check if there is a mounted USB. If not, store the file in memory
        usb_mount_point = None # find_usb_mount_point()

        if usb_mount_point:
            file_path = usb_mount_point / "received_file.txt"
        else:
            file_path = "received_file.txt"
        

        # store the file
        content = b"".join(chunks)
        with open(file_path, "wb") as f:
            f.write(content)
        content_len = len(content)
        INFO(f"Saved {content_len} bytes to: {file_path}")
        

        # show a last information message with the througput
        INFO(f"Process finished in {total_time:.2f} seconds | Computed throughput: {((content_len / 1024) / total_time):.2f} KBps")
    
    except KeyboardInterrupt:
        ERROR("Process interrupted by user")

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


    if radio.role is Role.TRANSMITTER:
        BEGIN_TRANSMITTER_MODE()
    
    elif radio.role is Role.RECEIVER:
        BEGIN_RECEIVER_MODE()
        
    elif radio.role is Role.CARRIER:
        BEGIN_CONSTANT_CARRIER_MODE()

    return
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::





if __name__ == "__main__":
    main()