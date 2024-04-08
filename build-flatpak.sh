#!/bin/bash
set -e

# Build the application
make

# Create the flatpak bundle
flatpak-builder --user-sourced --force-clean --ccache --build-dir=build com.example.myapp.flatpak com.example.myapp

# Install the flatpak bundle
flatpak install com.example.myapp.flatpak