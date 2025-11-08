# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
from radio import CustomNRF24

from utils import (
    SUCC,
    WARN,
    INFO,
)
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::










# :::: PROTOCOL LAYERS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
def generate_STREAM_structure_based_on_TR_INFO_message(TR_INFO: bytes, STREAM: list[list[list[bytes]]]) -> None:
    """
    Allocate all the slots of the STREAM structure to fill them up later with DATA
    messages. We use the information contained in the TR_INFO message to do so
    """
    # NOTE: The structure of our DATA payload looks like this (for now):
    #
    # ┌────────────────────┬───────────────┬────────────────────┬────────────────────┬─────┬────────────────┬─────────────────────┬─────────────────────┐
    # │ MessageID + InfoID │ Page0 N Burst │ Page0 L Last Burst │ Page0 L Last Chunk | ... | Page10 N Burst │ Page10 L Last Burst │ Page10 L Last Chunk │
    # └────────────────────┴───────────────┴────────────────────┴────────────────────┴─────┴────────────────┴─────────────────────┴─────────────────────┘
    #   ↑           ↑        ↑               ↑                    ↑
    #   │           │        │               │                    1B: The length of the last Chunk of the last Burst: [0 - 255]
    #   │           │        │               1B: The number of Chunks in the last Burst: [0 - 255]
    #   │           │        1B: The number of Bursts in the page: [0 - 255]
    #   │           4b: Identifies the type of INFO message that we are sending: [0 - 15], for TR_INFO is set to 0000
    #   4b: Identifies the kind of message that we are sending, for INFO payload is set to 1111
    MESSAGE           = TR_INFO[1:]
    number_of_pages   = len(MESSAGE) % 3
    burst_in_page     = [byte for byte in MESSAGE[0:-1:3]]
    length_last_burst = [byte for byte in MESSAGE[1:-1:3]]
    length_last_chunk = [byte for byte in MESSAGE[2:-1:3]]

    INFO("Generating STREAM structure based on TR_INFO message")
    INFO(f"Number of Pages to be received: {number_of_pages}")
    INFO(f"Page Widths: {burst_in_page}")
    INFO(f"Last Burst Widths: {length_last_burst}")
    INFO(f"Last Chunk Widths: {length_last_chunk}")

    # Build STREAM as list[pages] -> list[bursts] -> list[chunks]
    # Use the parsed arrays: burst_in_page, length_last_burst, length_last_chunk
    pages_count = len(burst_in_page)

    for page_idx in range(pages_count):
        bursts_count = int(burst_in_page[page_idx])
        page = []
        for burst_idx in range(bursts_count):
            # For all bursts except the last one assume full 256 chunk slots (0..255).
            # For the last burst use the provided last-burst chunk count.
            if burst_idx == bursts_count - 1:
                chunks_count = int(length_last_burst[page_idx])
            else:
                chunks_count = 256

            # Initialize each chunk slot with an empty bytes object to be filled later.
            burst = [b"" for _ in range(chunks_count)]
            page.append(burst)

        STREAM.append(page)

    INFO(f"Allocated STREAM: {len(STREAM)} pages")
    return

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
    # by Page, Burst and Chunk.
    #
    # NOTE: The structure of our DATA payload looks like this (for now):
    #
    # ┌────────────────────┬─────────┬─────────┬──────┐
    # │ MessageID + PageID │ BurstID │ ChunkID │ DATA │
    # └────────────────────┴─────────┴─────────┴──────┘
    #   ↑           ↑        ↑         ↑         ↑
    #   │           │        │         │         29B: The data to be sent, either if it is part of a file or data from an info message
    #   │           │        │         1B: Identifies a chunk inside a burst, starts from 0 at every Burst: [0 - 255]
    #   │           │        1B: Identifies a Burst inside a Page, starts from 0 at every Page: [0 - 255]
    #   │           4b: Identifies a Page inside a Transfer: [0 - 15]
    #   4b: Identifies the kind of message that we are sending, for DATA payload is set to 0000
    #
    # NOTE: The unified structure of the Transfer looks like this:
    # NOTE: N ≤ 255
    # NOTE: L ≤ 15
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
    #       PageID    BurstID   ChunkID    Payload(MessageID + PageID + BurstID + ChunkID + DATA)
    #       ↓         ↓         ↓          ↓
    STREAM: list[     list[     list[      bytes]]] = list()

    # NOTE: The flow of the PRX is as follows, we keep receiving frames until the
    # transmission has finished. We do not care about the order of the frames as we
    # assume that the PTX will take care of that. We treat each frame differently
    # depending on the information inside the frame itself. After each received frame,
    # we evaluate if the transmission has ended or not
    TRANSFER_HAS_ENDED        = False
    STREAM_HAS_BEEN_GENERATED = False
    while not TRANSFER_HAS_ENDED:
        # If we have not received anything we do nothing
        while not prx.data_ready():
            continue

        # If we have received something we pull it from the FIFO and analyze the first
        # Byte to check what type of message we have received
        #
        # NOTE: If the byte has the format 1111XXXX then it is an INFO message and we need
        # to read the next 4 bits to identify the type of INFO message:
        # 11110000: Corresponds to TR_INFO
        # 11110001: Corresponds to EMPTY
        #
        # NOTE: If the byte has the format 0000XXXX then it is a DATA message and the
        # first 3 Bytes correspond to the PageID, BurstID and ChunkID
        frame = prx.get_payload()

        # NOTE: INFO message
        if frame[0] & 0xF0:

           # NOTE: TR_INFO
            if not frame[0] & 0x0F:
                generate_STREAM_structure_based_on_TR_INFO_message(frame, STREAM)
                INFO("Generated STREAM structure")
                # help me visualize the size of the structure
                print(STREAM)
                # INFO(f"Total Pages: {len(STREAM)}")
                # for PageID in enumerate(len(STREAM)):
                #     INFO(f"  Page {PageID}: Total Bursts: {len(STREAM[PageID])}")
                #     for BurstID in STREAM[PageID]:
                #         INFO(f"    Burst {BurstID}: Total Chunks: {len(STREAM[PageID][BurstID])}")

        
        # NOTE: DATA message
        else:
            PageID  = frame[0]
            BurstID = frame[1]
            ChunkID = frame[2]
            STREAM[PageID][BurstID][ChunkID] = frame
                


    return
    # Wait for a TX_INFO message
    # NOTE: As of now, the TX_INFO message only contains the number of pages that will
    # be sent in the communication and the total ammount of bytes that are expected to
    # be transfered, but it can be changed to include more information
    #
    # | MessageID (4B) | TxLength (4B) | TxWidth (4B) | = 12 Bytes
    #
    # MessageID:  The identifier of the type of info message:                    [TXIM] (TX Info Message)
    # TxLength: The number of pages that will be sent in the communication       [0..4_294_967_295]
    # TxWidth:  The total number of bytes that will be sent in the communication [0..4_294_967_295]
    INFO("Waiting for TX_INFO message")
    while not prx.data_ready():
        pass

    TX_INFO: bytes = prx.get_payload()
    MessageID      = TX_INFO[0:4].decode()
    TxLength       = int.from_bytes(TX_INFO[4:8]) + 1
    TxWidth        = int.from_bytes(TX_INFO[8:12])
    SUCC(f"Received {MessageID} message: TxLength = {TxLength} | TxWidth = {TxWidth}")

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

        PAGE_INFO: bytes = prx.get_payload()
        MessageID        = PAGE_INFO[0:4]
        PageID           = PAGE_INFO[4]
        PageLength       = int.from_bytes(PAGE_INFO[5:9]) + 1
        PageWidth        = int.from_bytes(PAGE_INFO[9:12])
        SUCC(f"Received {MessageID.decode()} message: PageID = {PageID} | PageLength = {PageLength} | PageWidth = {PageWidth}")

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
            BurstLength = BURST_INFO[4] + 1
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
