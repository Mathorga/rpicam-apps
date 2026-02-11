#!/bin/bash

sudo apt install -y libcamera-dev libepoxy-dev libjpeg-dev libtiff5-dev libpng-dev libopencv-dev

# Qt support.
sudo apt install -y qtbase5-dev libqt5core5a libqt5gui5 libqt5widgets5

# Libav support.
sudo apt install libavcodec-dev libavdevice-dev libavformat-dev libswresample-dev

# Build systems.
sudo apt install -y cmake libboost-program-options-dev libdrm-dev libexif-dev
sudo apt install -y meson ninja-build

# Install GPIO support.
sudo apt install libgpiod-dev