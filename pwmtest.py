from gpiozero import LED
import time

# Configuración del LED en el pin GPIO 26
red = LED(26)

print("Sistema listo.")
print("Escribe 'start' para parpadear.")
print("Escribe 'stop' para detener.")
print("Escribe 'salir' para cerrar el programa.")

try:
    while True:
        # El programa espera aquí a que escribas algo y presiones Enter
        comando = input("Esperando orden: ").lower().strip()

        if comando == "start":
            print(">>> Iniciando parpadeo...")
            # on_time=0.5, off_time=0.5 hace que parpadee rápido (medio segundo)
            red.blink(on_time=0.5, off_time=0.5)
            
        elif comando == "stop":
            print(">>> Deteniendo LED.")
            red.off() # Esto apaga el LED y cancela el parpadeo
            
        elif comando == "salir":
            print("Cerrando programa.")
            red.off()
            break # Rompe el bucle y termina el script
            
        else:
            print("Comando no reconocido. Intenta con 'start' o 'stop'.")

except KeyboardInterrupt:
    # Esto permite salir limpiamente presionando Ctrl+C
    red.off()
    print("\nPrograma interrumpido.")