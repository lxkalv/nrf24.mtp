#ifndef ROBUST_MODE_IFACE_H
#define ROBUST_MODE_IFACE_H

#include "libs/app_layer.h"  // app_config_t, app_parse_arguments, etc.

int run_tx(const char *spi_dev,
           const app_config_t *cfg,
           const uint8_t *file_data,
           size_t file_len);

int run_rx(const char *spi_dev, const app_config_t *cfg);

/* Estas funciones ya existen en robust_mode.c */
const char *get_spi_device_path(void);
void update_radio_params_from_config(const app_config_t *cfg);

#endif
