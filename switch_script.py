from gpiozero import Button
import os
import time

gpioA = Button(17)  # Position 1 du switch
gpioB = Button(27)  # Position 2 du switch

def check_switch_state():
    if gpioA.is_pressed:
        print("Position A -> Script A")
        os.system("bash /home/pi/exec_p2p.sh") 
    elif gpioB.is_pressed:
        print("Position B -> Script B")
        os.system("bash /home/pi/exec_network.sh")
    else:
        print("Aucune position active")
        
print("Surveillance du switch...")
last_state = None

while True:
    current = (gpioA.is_pressed, gpioB.is_pressed)
    if current != last_state:   # éviter exécutions multiples
        last_state = current
        check_switch_state()
    time.sleep(0.2)
