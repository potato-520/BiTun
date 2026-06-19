#!/bin/bash
set -e

# Sourced ESP-IDF export script
. "$HOME/esp/esp-idf/export.sh"

# Run the build
idf.py build

echo "=========================================================="
echo "ESP-IDF Build Successful!"
echo "Main component library is located at:"
echo "build/esp-idf/main/libmain.a"
echo "=========================================================="
