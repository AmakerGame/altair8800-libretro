# Altair 8800 Libretro Core

A high-performance Intel 8080-based emulator core for RetroArch, developed by **Edytor Studio**. This core accurately emulates the hardware of the Altair 8800, providing a tactical and visual experience of the early computing era.

## 🚀 Download
Official binaries for Windows, Linux, and macOS (Universal) are available here:
👉 **[Latest Releases](https://github.com/AmakerGame/altair8800-libretro/releases)**

## ✨ Key Features
* **Full Multi-Platform Support**: Seamless operation on Windows, Linux, and macOS.
* **Accurate CPU Emulation**: Full Intel 8080 instruction set running at 2MHz.
* **Front Panel Diagnostics**: Real-time visualization of Program Counter (PC) and memory status via simulated LEDs.
* **Optimized Architecture**: Written in pure C for maximum efficiency and low latency.

## 🛠️ Installation

1. **Core Library**: 
   - Download the build matching your OS (e.g., `altair8800_libretro.dll` for Windows).
   - Copy the file to your RetroArch `cores` directory.
2. **Core Info File**: 
   - Copy `altair8800_libretro.info` to the RetroArch `info` directory to enable proper system identification.
3. **Loading Games**:
   - Open RetroArch, select **Load Core** -> **Altair 8800**.
   - Use **Load Content** to open `.bin` files.

## 🎮 Controls (RetroPad)
* **D-Pad**: Navigate between the address/data toggles.
* **Button A**: Flip the selected switch (ON/OFF).
* **Start**: Toggle the master RUN/STOP switch.

## ⚙️ Technical Specifications
This core targets the Intel 8080 microprocessor. The rendering engine provides a visual representation of the internal bus state, allowing users to "see" the code executing in real-time, just like on the original 1975 hardware.

## ⚖️ License
This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.
Created and maintained by **Edytor Studio**.