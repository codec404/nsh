#!/bin/sh
# install.sh — build and install nsh
set -e

INSTALL_DIR="${NSH_PREFIX:-/usr/local/bin}"
BINARY=build/nsh

# ── dependency check (macOS / Homebrew) ──────────────────────────────────────
if [ "$(uname)" = "Darwin" ]; then
    for pkg in readline sqlite; do
        if ! brew list --formula "$pkg" >/dev/null 2>&1; then
            echo "Installing dependency: $pkg"
            brew install "$pkg"
        fi
    done
fi

# ── build ─────────────────────────────────────────────────────────────────────
echo "Building nsh..."
make clean
make

# ── install ───────────────────────────────────────────────────────────────────
echo "Installing to $INSTALL_DIR/nsh"
mkdir -p "$INSTALL_DIR"

if [ -w "$INSTALL_DIR" ]; then
    cp "$BINARY" "$INSTALL_DIR/nsh"
else
    sudo cp "$BINARY" "$INSTALL_DIR/nsh"
fi

echo "nsh installed: $(command -v nsh 2>/dev/null || echo "$INSTALL_DIR/nsh")"

# ── optional: set as login shell ─────────────────────────────────────────────
NSH_PATH="$INSTALL_DIR/nsh"

if [ "${NSH_SET_SHELL:-0}" = "1" ]; then
    if ! grep -qF "$NSH_PATH" /etc/shells; then
        echo "Adding $NSH_PATH to /etc/shells (requires sudo)"
        echo "$NSH_PATH" | sudo tee -a /etc/shells >/dev/null
    fi
    chsh -s "$NSH_PATH"
    echo "Default shell set to nsh. Re-login to apply."
else
    echo ""
    echo "To set nsh as your login shell, run:"
    echo "  NSH_SET_SHELL=1 $0"
fi
