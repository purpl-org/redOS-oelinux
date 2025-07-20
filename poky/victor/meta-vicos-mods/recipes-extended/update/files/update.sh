#!/usr/bin/env bash

if [ "$1" = "oskr" ]; then
    echo "Updating robot to latest OSKR OTA..."
    update-os https://ota-cdn.ekeleze.net/latest/oskr.ota
elif [ "$1" = "dev" ]; then
    echo "Updating robot to latest DEV OTA..."
    update-os https://ota-cdn.ekeleze.net/latest/dev.ota
else
    echo "Usage: update <oskr/dev>"
    exit 1
fi