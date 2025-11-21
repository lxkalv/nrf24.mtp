from gpiozero import PWMLED
from time import sleep
from signal import pause

# --- Configuración ---
PIN_PWM = 26
FRECUENCIA = 2       
TIEMPO_ENCENDIDO = 10  
TIEMPO_APAGADO = 10    
# ---------------------

# 1. Configuración del objeto
# Usamos PWMLED para mantener tu configuración de frecuencia (100Hz)
led = PWMLED(PIN_PWM, frequency=FRECUENCIA)

# 2. Iniciar el parpadeo en SEGUNDO PLANO
# background=True es el valor por defecto.
# fade_in_time=0 y fade_out_time=0 hacen que el cambio sea instantáneo (como tu 0% a 100%)
led.blink(on_time=TIEMPO_ENCENDIDO, 
          off_time=TIEMPO_APAGADO, 
          fade_in_time=0, 
          fade_out_time=0)

print(f"Iniciando parpadeo (PWM) en GPIO {PIN_PWM} en segundo plano.")

# ---------------------------------------------------------
# 3. AQUÍ SE EJECUTA TU "OTRO CÓDIGO"
# ---------------------------------------------------------
try:
    # Este bucle simula tu programa principal funcionando
    contador = 0
    while True:
        print(f"El código principal sigue corriendo... {contador}")
        contador += 1
        
        # Hacemos una pausa corta aquí solo para no inundar la consola,
        # pero el LED sigue respetando sus tiempos de 10s/10s independientemente de esto.
        sleep(1) 

except KeyboardInterrupt:
    print("\nPrograma detenido.")
    # No hace falta GPIO.cleanup(), gpiozero lo hace solo al cerrar el script.