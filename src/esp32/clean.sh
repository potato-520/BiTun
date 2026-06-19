#!/bin/bash
set -e

# Sourced ESP-IDF export script
. "$HOME/esp/esp-idf/export.sh"

# Run fullclean
idf.py fullclean
