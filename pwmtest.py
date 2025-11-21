from gpiozero import LED
from signal import pause

red = LED(26)

red.blink(0.2)

