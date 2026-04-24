# Altair 8800 Libretro Core

An authentic, lightweight Intel 8080-based emulator core for RetroArch, developed by **Edytor Studio**. This core emulates the legendary Altair 8800, the machine that sparked the personal computer revolution.

## 🚀 Download
You can find the latest pre-compiled binaries for Windows, Linux, and macOS here:
👉 **[Latest Releases](https://github.com/AmakerGame/altair8800-libretro/releases)**

## ✨ Features
* **Multi-Platform Support**: Available for Windows, Linux, and macOS (Universal Binary).
* **Intel 8080 Emulation**: Accurate instruction set processing at 2MHz.
* **Front Panel Visualization**: Real-time LED status monitoring directly on the screen.
* **Lightweight Design**: Minimal dependencies, ensuring compatibility even on legacy hardware like Windows 7.

## 🛠️ Installation

1. **Core Placement**: 
   - Download the version for your OS (e.g., `altair8800-libretro.dll` for Windows).
   - Move it to your RetroArch `cores` directory.
2. **Info File**: 
   - Move `altair8800-libretro.info` to the RetroArch `info` directory to enable proper naming in the menu.
3. **Running Content**:
   - Open RetroArch and select **Load Core** -> **Altair 8800**.
   - Use **Load Content** to open a `.bin` or `.hex` file.

## 🎮 Controls (RetroPad)
* **D-Pad**: Navigate between address/data switches.
* **Button A**: Toggle the selected switch.
* **Start**: Toggle RUN/STOP state.

## 📜 Technical Details
The core is written in pure C for maximum performance and portability. It utilizes a custom rendering loop to visualize the state of the Program Counter (PC) and memory addresses through simulated LEDs.

## ⚖️ License
This project is released under the **MIT License**. Created by Edytor Studio.