#!/bin/env bash
nix-shell --arg cross true --run "run-test-windows"

# Copy runtime DLLs in nix-shell first
nix-shell --arg cross true --run "copy-windows-dlls"

# Run wine using the host's wine64 / wine command
# This ensures it inherits the host system's OpenGL and X11/Wayland driver settings.
WINE_CMD="wine64"
if ! command -v wine64 &> /dev/null; then
    WINE_CMD="wine"
fi

if ! command -v $WINE_CMD &> /dev/null; then
    # Fallback to the nix-shell wine if host wine is not installed
    echo "Host Wine not found. Falling back to running Wine inside nix-shell..."
    nix-shell --arg cross true --run "run-windows"
else
    echo "Running Windows executable via host's $WINE_CMD..."
    WINEPATH="$(pwd)/build-windows/lib;${WINEPATH:+$WINEPATH}" $WINE_CMD build-windows/TestWindowPaint.exe
fi