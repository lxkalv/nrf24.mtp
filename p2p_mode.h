#ifndef P2P_MODE_H
#define P2P_MODE_H

int run_tx(const char *spi_dev, int ce_bcm, const char *input_path);
int run_rx(const char *spi_dev, int ce_bcm, const char *output_path);

#endif
