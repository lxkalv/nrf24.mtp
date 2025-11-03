# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from radio import CustomNRF24

from utils import (
    SUCC,
    WARN,
    INFO,
)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: PROTOCOL LAYERS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def RX_LINK_LAYER(prx: CustomNRF24) -> None:
    """
    This layer is responsible for the following things:
    - Generating a unified structure containing the data of the transmission orderer
    by page, burst and chunk
    - Reading a TX_INFO message and set up the expected number of pages
    - Reading a PAGE_INFO message
    - Reading a BURST_INFO message
    - Receiving each frame
    - Compute and send the checksum at the end of the burst
    """
    # Generate the unified structure where we are goint to store the bytes categorized
    # by page, burst and chunk.
    # NOTE: The structure looks like this:
    #
    # PAGE 0:
    #     BURST 0:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B32]
    #         ...
    #         CHUNK 255: bytes[B0, B1, B2, ..., B32]
    #     BURST 1:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B32]
    #         ...
    #         CHUNK 255: bytes[B0, B1, B2, ..., B32]
    #     ...
    #     BURST N:
    #         CHUNK 000: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 001: bytes[B0, B1, B2, ..., B32]
    #         CHUNK 002: bytes[B0, B1, B2, ..., B32]
    #         ...
    #         CHUNK 255: bytes[B0, B1, B2, ..., B32]
    # PAGE 1:
    #     BURST 0:
    #         ...
    #     BURST 1:
    #         ...
    #     ...
    #     BURST N:
    #         ...
    # ...
    # PAGE L:
    #     BURST 0:
    #         ...
    #     BURST 1:
    #         ...
    #     ...
    #     BURST N:
    #         ...
    #
    #            PageID    BurstID   ChunkID    Payload(ChunkID + Data)
    #            ↓         ↓         ↓          ↓
    STREAM: dict[str, dict[str, dict[str,       bytes]]] = dict()

    # Wait for a TX_INFO message
    # NOTE: As of now, the TX_INFO message only contains the number of pages that will
    # be sent in the communication and the total ammount of bytes that are expected to
    # be transfered, but it can be changed to include more information
    #
    # | TxLength (4B) | TxWidth (4B) | = 8 Bytes
    #
    # TxLength: The number of pages that will be sent in the communication       [0..4_294_967_295]
    # TxWidth:  The total number of bytes that will be sent in the communication [0..4_294_967_295]
    INFO("Waiting for TX_INFO message")
    while not prx.data_ready():
        pass

    TX_INFO  = prx.get_payload()
    TxLength = int.from_bytes(TX_INFO[0:4])
    TxWidth  = int.from_bytes(TX_INFO[4:8])
    SUCC(f"Received TX_INFO message: TxLength = {TxLength} | TxWidth = {TxWidth}")

    # Iterate over all the pages of the communication
    PageID = 0
    while PageID < TxLength - 1:
        # Wait of a PAGE_INFO message
        # NOTE: everytime we start a page, we send a PAGE_INFO message containing the
        # PageID, the number of bursts in the page (PageLength) and the total ammount of
        # bytes in the page (PageWidth). The PAGE_INFO message payload has the following
        # structure:
        #
        # | PageID (1B) | PageLength (3B) | PageWidth (4B) | = 8 Bytes
        # 
        # PageID:     The identifier of the page           [0..255]
        # PageLength: The number of bursts inside the page [0..16_777_215]
        # PageWidth:  The number of bytes inside the page  [0..4_294_967_295]
        INFO("Waiting for PAGE_INFO message")
        while not prx.data_ready():
            pass

        PAGE_INFO  = prx.get_payload()
        PageID     = PAGE_INFO[0]
        PageLength = int.from_bytes(PAGE_INFO[1:4])
        PageWidth  = int.from_bytes(PAGE_INFO[4:8])
        SUCC(f"Received PAGE_INFO message: PageID = {PageID} | PageLength = {PageLength} | PageWidth = {PageWidth}")

        STREAM[f"PAGE{PageID}"] = dict()

        BurstID = 0
        while BurstID < PageLength - 1:
            # Wait for BURST_INFO message
            # NOTE: everytime we start a burst, we send a BURST_INFO message containing the
            # BurstID, the number of chunks in the burst (BurstLength) and the total ammount
            # of bytes in the burst (BurstWidth). The BURST_INFO message payload has the
            # following structure:
            #
            # | BurstID (4B) | BurstLength (1B) | BurstWidth (2B) | = 7 Bytes
            #
            # BurstID:     The identifier of the burst       [0..4_294_967_295]
            # BurstLenght: The number of chunks in the burst [0..255]
            # BurstWidth:  The number of bytes in the burst  [0..65_535]
            INFO("Waiting for BURST_INFO message")
            while not prx.data_ready():
                pass

            BURST_INFO  = prx.get_payload()
            BurstID     = int.from_bytes(BURST_INFO[0:4])
            BurstLength = BURST_INFO[4]
            BurstWidth  = int.from_bytes(BURST_INFO[5:7])
            SUCC(f"Received BURST_INFO message: BurstID = {BurstID} | BurstLength = {BurstLength} | BurstWidth = {BurstWidth}")

            STREAM[f"PAGE{PageID}"][f"BURST{BurstID}"] = dict()

            received_bytes = 0
            ChunkID        = 0
            while (ChunkID < BurstLength - 1) and (received_bytes < BurstWidth):
                while not prx.data_ready():
                    pass
                
                CHUNK   = prx.get_payload()
                ChunkID = CHUNK[0]
                data    = CHUNK[1:]

                received_bytes += len(CHUNK)

                STREAM[f"PAGE{PageID}"][f"BURST{BurstID}"][f"CHUNK{ChunkID}"] = data

# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: MAIN FLOW ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def FULL_RX_MODE(prx: CustomNRF24) -> None:
    RX_LINK_LAYER(prx)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
