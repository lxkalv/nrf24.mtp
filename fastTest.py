import RPi.GPIO as GPIO
import time

# Pin Definitions
led_pin = 26
button_pin = 17

# Setup GPIO mode
GPIO.setmode(GPIO.BCM) # Use BCM GPIO numbers
GPIO.setup(led_pin, GPIO.OUT)
# Setup button with an internal Pull-Up Resistor
GPIO.setup(button_pin, GPIO.IN, pull_up_down=GPIO.PUD_UP)

led_state = False

try:
    print("System Ready. Press CTRL+C to exit.")
    while True:
        input_state = GPIO.input(button_pin)
        
        # If button is pressed (Low state because connected to Ground)
        if input_state == False:
            print("Button Pressed")
            led_state = not led_state # Toggle state
            GPIO.output(led_pin, led_state)
            
            # specific delay to debounce the switch
            time.sleep(0.2)
            
except KeyboardInterrupt:
    print("Cleaning up...")
    GPIO.cleanup()