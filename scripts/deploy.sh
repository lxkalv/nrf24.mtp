#!/bin/bash
sudo pigpiod
sudo dnsmasq
sudo ~/.venv/bin/python orchestrator.py