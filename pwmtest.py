import RPi.GPIO as GPIO
import time

# --- Configuración ---
PIN_PWM = 26                
FRECUENCIA =  100            # Frecuencia en Hz (este valor no afecta al blink de 0/100%)
TIEMPO_ENCENDIDO = 10       # Segundos que el LED permanece encendido
TIEMPO_APAGADO = 10        # Segundos que el LED permanece apagado
# ---------------------

GPIO.setmode(GPIO.BCM)
GPIO.setup(PIN_PWM, GPIO.OUT)

# Configurar el objeto PWM
pwm = GPIO.PWM(PIN_PWM, FRECUENCIA)

# Iniciar el PWM con ciclo de trabajo 0% (apagado)
pwm.start(0)

print(f"Iniciando parpadeo continuo (PWM) en GPIO {PIN_PWM}. Presiona Ctrl+C para detener.")

try:
    while True:
        # Encender el LED (100% duty cycle)
        pwm.ChangeDutyCycle(100)
        time.sleep(TIEMPO_ENCENDIDO)

        # Apagar el LED (0% duty cycle)
        pwm.ChangeDutyCycle(0)
        time.sleep(TIEMPO_APAGADO)

except KeyboardInterrupt:
    # Detener el PWM y limpiar los pines al presionar Ctrl+C
    pwm.stop()
    GPIO.cleanup()
    print("\nPrograma detenido y pines limpiados.")
