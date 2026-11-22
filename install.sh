#!/usr/bin/env bash
set -euo pipefail

REPO="AbdoMG15/LWFG"
ASSET_URL="https://github.com/${REPO}/releases/latest/download/LWFG-x86_64.AppImage"

INSTALL_DIR="$HOME/.local/share/lwfg"
BIN_DIR="$HOME/.local/bin"
APP_DIR="$HOME/.local/share/applications"
ICON_128="$HOME/.local/share/icons/hicolor/128x128/apps/lfg-linux.png"
ICON_32="$HOME/.local/share/icons/hicolor/32x32/apps/lfg-linux.png"
PIXMAP="$HOME/.local/share/pixmaps/lfg-linux.png"
DESKTOP_FILE="$APP_DIR/lfg-linux.desktop"

refresh_caches() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$APP_DIR" >/dev/null 2>&1 || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t "$HOME/.local/share/icons/hicolor" >/dev/null 2>&1 || true
    fi
    if command -v kbuildsycoca6 >/dev/null 2>&1; then
        kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
    elif command -v kbuildsycoca5 >/dev/null 2>&1; then
        kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
    fi
}

if [ "${1:-}" = "--uninstall" ]; then
    echo "==> Stopping any running LWFG instance..."
    "$BIN_DIR/lwfg" --quit >/dev/null 2>&1 || true
    echo "==> Removing LWFG..."
    rm -rf "$INSTALL_DIR"
    rm -f "$BIN_DIR/lwfg" "$BIN_DIR/lfg" "$DESKTOP_FILE" "$ICON_128" "$ICON_32" "$PIXMAP"
    refresh_caches
    echo "LWFG has been uninstalled."
    exit 0
fi

ARCH="$(uname -m)"
if [ "$ARCH" != "x86_64" ]; then
    echo "Error: Unsupported architecture '$ARCH'. Prebuilt LWFG releases require x86_64." >&2
    exit 1
fi

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT
APPIMAGE_TMP="$TMPDIR/LWFG-x86_64.AppImage"

if [ -n "${LWFG_APPIMAGE_PATH:-}" ]; then
    echo "==> Using local AppImage: $LWFG_APPIMAGE_PATH"
    cp -f "$LWFG_APPIMAGE_PATH" "$APPIMAGE_TMP"
else
    echo "==> Downloading latest LWFG release from GitHub (${REPO})..."
    if command -v curl >/dev/null 2>&1; then
        if ! curl -fSL "$ASSET_URL" -o "$APPIMAGE_TMP"; then
            echo "Error: Failed to download $ASSET_URL. Make sure a GitHub Release with LWFG-x86_64.AppImage is published." >&2
            exit 1
        fi
    elif command -v wget >/dev/null 2>&1; then
        if ! wget -qO "$APPIMAGE_TMP" "$ASSET_URL"; then
            echo "Error: Failed to download $ASSET_URL. Make sure a GitHub Release with LWFG-x86_64.AppImage is published." >&2
            exit 1
        fi
    else
        echo "Error: Neither 'curl' nor 'wget' is installed." >&2
        exit 1
    fi
fi

chmod +x "$APPIMAGE_TMP"

echo "==> Extracting LWFG..."
(cd "$TMPDIR" && ./LWFG-x86_64.AppImage --appimage-extract >/dev/null)

"$BIN_DIR/lwfg" --quit >/dev/null 2>&1 || true

rm -rf "$INSTALL_DIR"
mkdir -p "$(dirname "$INSTALL_DIR")" "$BIN_DIR" "$APP_DIR" \
    "$(dirname "$ICON_128")" "$(dirname "$ICON_32")" "$(dirname "$PIXMAP")"

mv "$TMPDIR/squashfs-root" "$INSTALL_DIR"

ln -sf "$INSTALL_DIR/usr/bin/lfg" "$BIN_DIR/lfg"
ln -sf "$INSTALL_DIR/usr/bin/lfg" "$BIN_DIR/lwfg"

if [ -f "$INSTALL_DIR/usr/share/icons/hicolor/128x128/apps/lfg-linux.png" ]; then
    cp -f "$INSTALL_DIR/usr/share/icons/hicolor/128x128/apps/lfg-linux.png" "$ICON_128"
    cp -f "$INSTALL_DIR/usr/share/icons/hicolor/128x128/apps/lfg-linux.png" "$PIXMAP"
elif [ -f "$INSTALL_DIR/lfg-linux.png" ]; then
    cp -f "$INSTALL_DIR/lfg-linux.png" "$ICON_128"
    cp -f "$INSTALL_DIR/lfg-linux.png" "$PIXMAP"
fi

if [ -f "$INSTALL_DIR/usr/share/icons/hicolor/32x32/apps/lfg-linux.png" ]; then
    cp -f "$INSTALL_DIR/usr/share/icons/hicolor/32x32/apps/lfg-linux.png" "$ICON_32"
fi

EXEC_BIN="$(printf '%s\n' "$BIN_DIR/lwfg" | sed 's/\\/\\\\/g; s/"/\\"/g; s/`/\\`/g; s/\$/\\$/g')"
cat > "$DESKTOP_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=LWFG
GenericName=Linux Windowed Frame Generation
Comment=LWFG (Linux Windowed Frame Generation) — Lossless Scaling LSFG 3.1 Overlay for Linux
TryExec=$BIN_DIR/lwfg
Exec="$EXEC_BIN"
Icon=$ICON_128
StartupWMClass=lfg-linux
Terminal=false
Categories=Game;
Keywords=lwfg;lfg;frame;generation;lossless;scaling;vulkan;fps;adaptive;lsfg;
Actions=ToggleFG;QuitFG;

[Desktop Action ToggleFG]
Name=Start / Stop Frame Generation
Exec="$EXEC_BIN" --toggle

[Desktop Action QuitFG]
Name=Quit LWFG
Exec="$EXEC_BIN" --quit
EOF

refresh_caches

echo ""
echo "=================================================================="
echo "LWFG installed successfully!"
echo "  App Menu  : LWFG (look in your Application Launcher / Games)"
echo "  Terminal  : $BIN_DIR/lwfg (or 'lwfg' if ~/.local/bin is in PATH)"
echo "  Uninstall : curl -fsSL https://raw.githubusercontent.com/${REPO}/main/install.sh | bash -s -- --uninstall"
echo "=================================================================="
