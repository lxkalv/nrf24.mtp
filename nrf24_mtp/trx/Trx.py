"""
a
"""





# :::: IMPORTS ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
import pigpio
import time
import sys

from nrf24 import (
    NRF24,

    RF24_DATA_RATE,
    RF24_PA,
    RF24_CRC,
    RF24_PAYLOAD,
    RF24_RX_ADDR,
)



from ..layers import ApplicationLayer
from ..utils import Logger
# :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::


class TRX(NRF24):
    """
    Custom NRF24 class that allows for extending the NRF24 base class without
    modifying the library itself
    """

    def __init__(
            self: "TRX",
            MODE: "ApplicationLayer.Mode",
            CE_PIN: int,
            CHANNEL: int,
            DATA_RATE: RF24_DATA_RATE,
            PA_LEVEL:RF24_PA,
            CRC_BYTES: RF24_CRC,
            RETRANSMISSION_TRIES: int,
            RETRANSMISSION_DELAY: int,
            spi_speed: float = 10_000_000
        ) -> None:

        # :::: CONFIGURE PIGPIO :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        self.hostname = "localhost"
        self.port     = 8888

        self.pigpio = pigpio.pi(self.hostname, self.port)
        if not self.pigpio:
            Logger.ERROR("Not connected to Raspberry Pi, exiting")
            sys.exit(1)
        # :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



        # :::: INITIALIZE RADIO DEVICE ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        super().__init__(pi = self.pigpio, ce = CE_PIN, spi_speed = spi_speed)
        # :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



        # :::: PARAMETERS :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        self.MODE                 = MODE
        self.CE_PIN               = CE_PIN
        self.CHANNEL              = CHANNEL
        self.DATA_RATE            = DATA_RATE
        self.PA_LEVEL             = PA_LEVEL
        self.CRC_BYTES            = CRC_BYTES
        self.PAYLOAD_SIZE         = RF24_PAYLOAD.ACK
        self.RETRANSMISSION_TRIES = RETRANSMISSION_TRIES
        self.RETRANSMISSION_DELAY = RETRANSMISSION_DELAY
        self.ADDRESS_BYTE_LENGTH  = 3
        # :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::



        # :::: CONFIGURE RADIO DEVICE :::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
        self.set_channel(self.CHANNEL)
        self.set_data_rate(self.DATA_RATE)
        self.set_pa_level(self.PA_LEVEL)
        self.set_crc_length(self.CRC_BYTES)
        self.set_payload_size(self.PAYLOAD_SIZE)
        self.set_retransmission(self.RETRANSMISSION_DELAY, self.RETRANSMISSION_TRIES)
        self.set_address_bytes(self.ADDRESS_BYTE_LENGTH)

        if self.MODE is ApplicationLayer.Mode.TX:
            self.open_writing_pipe(b"TA1")
            self.open_reading_pipe(RF24_RX_ADDR.P1, b"TA0")
        elif self.MODE is ApplicationLayer.Mode.RX:
            self.open_writing_pipe(b"TA0")
            self.open_reading_pipe(RF24_RX_ADDR.P1, b"TA1")
        # ::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::

        return