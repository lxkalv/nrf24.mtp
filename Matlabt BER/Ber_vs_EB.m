clc;clear;
clear all;

% --- 1. Definir los parámetros fijos ---

% ¡Modifica estos valores como necesites!
R = 250e3;     % Data Rate (R) en bits por segundo (ej. 1 Mbps)
W = 1e6;     % Ancho de banda (W) en Hertz (ej. 4 MHz)

% Calculamos la relación W/R (Factor de expansión de ancho de banda)
% Esta relación es clave.
W_sobres_R = W / R;

% --- 2. Definir el rango de SNR (el nuevo eje X) ---

% Definimos el vector de SNR en decibelios (dB)
% Este rango puede necesitar ajuste dependiendo de tu W/R
SNR_dB = -10:0.5:20;

% --- 3. Convertir SNR de dB a escala lineal ---

% La fórmula matemática utiliza el ratio lineal
SNR_linear = 10.^(SNR_dB / 10);

% --- 4. Calcular el BER (Pb) usando la fórmula combinada ---

% Primero, calculamos el Eb/N0 lineal para cada valor de SNR
% Eb/N0 = SNR * (W / R)
EbN0_linear = SNR_linear * W_sobres_R;

% Ahora, usamos el Eb/N0 calculado en la fórmula de BER original
% Pb ≈ 2 * Q(sqrt(2 * Eb/N0))
Pb_teorico = 2 * qfunc(sqrt(2 * EbN0_linear));

% --- 5. Graficar los resultados ---

figure; % Crea una nueva ventana para la figura
semilogy(SNR_dB, Pb_teorico, 'r-s', 'LineWidth', 2);

% --- 6. Añadir etiquetas y formato a la gráfica ---

grid on;
xlabel('SNR (dB)'); % ¡El eje X ahora es SNR!
ylabel('Bit Error Rate (BER)');
title(sprintf('BER vs. SNR para QFSK Coherente (R=%.1e, W=%.1e)', R, W));
legend(sprintf('P_b vs. SNR (W/R = %.1f)', W_sobres_R), 'Location', 'southwest');

ylim([10^-8 1]);