#!/bin/bash
# Script to setup MTP application to run at boot

echo "Setting up MTP application autostart..."

sudo systemctl stop mtp-app

# Copy service file to systemd directory
sudo cp mtp-app.service /etc/systemd/system/

# Reload systemd to recognize the new service
sudo systemctl daemon-reload

# Enable the service to start at boot
sudo systemctl enable mtp-app.service

# Start the service now
sudo systemctl start mtp-app.service

echo "✓ Service installed and started!"
echo ""
echo "Useful commands:"
echo "  sudo systemctl status mtp-app    - Check service status"
echo "  sudo systemctl stop mtp-app      - Stop the service"
echo "  sudo systemctl start mtp-app     - Start the service"
echo "  sudo systemctl restart mtp-app   - Restart the service"
echo "  sudo systemctl disable mtp-app   - Disable autostart"
echo "  sudo journalctl -u mtp-app -f    - View live logs"