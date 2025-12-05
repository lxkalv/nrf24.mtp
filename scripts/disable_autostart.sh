#!/bin/bash
# Script to disable MTP application autostart

echo "Disabling MTP application autostart..."

# Stop the service if it's running
sudo systemctl stop mtp-app.service

# Disable the service from starting at boot
sudo systemctl disable mtp-app.service

echo "✓ Service stopped and disabled!"
echo ""
echo "To re-enable autostart, run: ./setup_autostart.sh"