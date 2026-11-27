# <img src="assets/lfg-linux.png" width="38" align="top" /> LWFG (Linux Windowed Frame Generation)

**LWFG** is an **unofficial**, independent Linux windowed frame generation overlay powered by **Lossless Scaling Frame Generation (LSFG 3.1)** and **lsfg-vk** by **PancakeTAS**.
> [!WARNING]
> - This is an **unofficial** third-party project and is **not** affiliated with, endorsed by, or supported by the creator of Lossless Scaling.
> - You **must purchase and install [Lossless Scaling on Steam](https://store.steampowered.com/app/993090/Lossless_Scaling/)** first. **LWFG** does **not** include or redistribute any proprietary files.
> - If LWFG does not automatically detect `Lossless.dll` from your Steam library on startup, open the **Settings** tab inside the app and **locate `Lossless.dll` (or its folder) manually** — frame generation will not work until `Lossless.dll` is loaded.

## Installation

### Option 1: One-Line Terminal Installer (Any `x86_64` Linux Distro)
Installs the latest release into `~/.local`, sets up the `lwfg` command, and adds LWFG to your application menu (no `sudo` or compiling needed):

```bash
curl -fsSL https://raw.githubusercontent.com/AbdoMG15/LWFG/main/install.sh | bash
```

To uninstall later:
```bash
curl -fsSL https://raw.githubusercontent.com/AbdoMG15/LWFG/main/install.sh | bash -s -- --uninstall
```

### Option 2: Download from [Releases](https://github.com/AbdoMG15/LWFG/releases/latest)
- **Ubuntu / Debian / Linux Mint / Pop!_OS:** Download `LWFG-0.1.5-amd64.deb` and double-click it (or run `sudo apt install ./LWFG-0.1.5-amd64.deb`).
- **Fedora / Nobara / openSUSE:** Download `LWFG-0.1.5-x86_64.rpm` and double-click it (or run `sudo dnf install ./LWFG-0.1.5-x86_64.rpm`).
- **Arch / CachyOS / SteamOS / Bazzite / Portable:** Download `LWFG-x86_64.AppImage`, make it executable, and run it:
  ```bash
  chmod +x LWFG-x86_64.AppImage
  ./LWFG-x86_64.AppImage
  ```
  *(Automatically adds LWFG + icon to your application menu on first launch. Run `./LWFG-x86_64.AppImage --uninstall-desktop` to remove the menu entry.)*

### Option 3: Build from Source
Install CMake, a C++20 compiler, `pkg-config`, and development headers for Vulkan (`>= 1.3`), PipeWire, `libportal`, GLib/GIO, XCB (`xcb`, `xcb-shape`, `xcb-randr`), and `libdrm`, then run:

```bash
git clone https://github.com/AbdoMG15/LWFG.git
cd LWFG
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build
```


## How to Use

1. **Launch LWFG** from your **Application Menu** (Games) or by running `lwfg` in a terminal.
2. **Check `Lossless.dll` status:**
   - LWFG automatically scans standard Steam library paths for `Lossless.dll`.
   - If it shows a notice that `Lossless.dll` was not found, go to the **Settings** tab and paste or browse to the path of your Steam copy of `Lossless.dll` (typically inside `.../SteamLibrary/steamapps/common/Lossless Scaling/`).
3. **Choose your settings** on the main menu:
   - **Mode:** Fixed (`2X`, `3X`, `4X`) or **Adaptive** (set your Target FPS).
   - **Flow Scale:** `25%–100%` (lower values improve GPU performance; `50%–75%` is great for high FPS targets).
   - **Performance Mode:** Lighter shader preset for lower-power GPUs.
   - **VSync:** `OFF` for uncapped FPS, `ON` to cap to your monitor's refresh rate.
4. **Start Frame Generation:**
   - Click **START** (or press the global shortcut, default **`Alt+F9`**).
   - Pick the game window, video player, or screen in the system capture prompt.
   - Press **`Alt+F9`** (or click **STOP**) anytime to pause or stop.


## How It Works
1. **Captures your window or screen:** Uses `xdg-desktop-portal` and PipeWire to grab each new frame on the GPU.
2. **Generates intermediate frames:** LWFG uses [lsfg-vk](https://lsfg-vk.dev/) by **PancakeTAS** to apply Lossless Scaling's frame generation.
3. **Displays over your app:** Presents the real and generated frames on the screen.


## Credits

- **[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)** — Official LSFG 3.1 compute shaders loaded at runtime from the user's legally purchased Steam installation.
- **[lsfg-vk](https://lsfg-vk.dev/)** by **PancakeTAS** — for the GPL-3.0 `lsfg-vk-backend` LSFG 3.1 Vulkan compute execution backend.

## Important Notes
- This project is **vibecoded**.
- I only got to test it on two laptops having CachyOS installed on them.
- For reporting bugs or suggestions contact: abdomg.contact@gmail.com
