# Woomer (C Port)

A lightweight, high-performance zoom and spotlight utility for Wayland compositors (for Sway now), written in C using **raylib** and GLSL shaders.

It is inspired by the original Rust version of `woomer`, and Tsoding's `boomer` for (X11) designed to capture screen output seamlessly and provide smooth zoom, flash, and spotlight effects.

## Features

* **Fast Screen Capture:** Captures output directly via `grim` into memory without saving temporary files to disk.

* **Dynamic Monitor Detection:** Scans monitor geometry using `swaymsg`.

* **Smooth Inertial Zoom:** Natural, responsive camera movement and zoom scaling.

* **Custom Shader Effects:** Built-in GLSL shaders for spotlight mask, dimming, and custom color overlays.

* **Flashlight Burst & Mirroring:** Interactive shortcuts for highlighting areas on screen.

* **Zero Heavy JSON Dependencies:** Includes a custom lightweight inline JSON scanner for minimal overhead.

## Prerequisites

Before building `woomer`, ensure you have the following dependencies installed on your system:

* **C Compiler** (`gcc` or `clang`)

* **Make** / **Bash**

* **raylib** (v4.0+)

* **grim** (for screenshotting on Wayland)

* **swaymsg** or compatible Wayland IPC utility

### Installing Dependencies

#### Arch / Artix Linux

```
sudo pacman -S gcc make raylib grim sway
```

## Building and Installing

Clone the repository and compile using either `make` or the build script:

```
# Clone the repository
git clone https://github.com/mushfiqurrahman1729/woomer-c.git
cd woomer-c

# Build using Makefile
make

# Or build using build.sh
chmod +x build.sh
./build.sh
```

This will generate the `woomer` binary in your current directory.

## Usage

Run `woomer` directly from your terminal or bind it to a hotkey in your window manager:

```
./woomer

```

### Controls

| Action | Control | 
 | ----- | ----- | 
| **Zoom In / Out** | Mouse Wheel / Scroll | 
| **Adjust Spotlight Size** | `Shift` + Mouse Wheel | 
| **Pan Camera** | Move Mouse | 
| **Toggle Mirror Mode** | Keypress / Mouse Click | 
| **Flashlight**  | f | 
| **Exit** | `Escape` / `Q` | 

## Configuration

You can customize `woomer` by modifying variables at the top of `main.c` or editing the embedded GLSL shader logic before compiling.

## License

This project is open-source and available under the GNU Public license.
