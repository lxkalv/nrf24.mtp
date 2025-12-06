#!/bin/bash
sudo pigpiod
sudo dnsmasq
sudo ~/.venv/bin make clean
sudo ~/.venv/bin make /src/robust_mode.c
sudo ~/.venv/bin/python orchestrator.py