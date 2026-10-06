# Woomer (C Port)

A lightweight screen zoom and flashlight tool for Wayland, written in C with **raylib** and a small GLSL shader. It currently supports **Sway** (and other wlroots compositors that provide `grim` and `swaymsg`).

It is a port of the Rust [`woomer`](https://github.com/coffeeispower/woomer) and is inspired by Tsoding's [`boomer`](https://github.com/tsoding/boomer) for X11: take a snapshot of the screen, then zoom, pan and spotlight it.

## Features

* **Instant capture.** The screenshot is taken with `grim` and piped straight into memory. No temporary files.
* **Multi-monitor aware.** Monitor geometry comes from `swaymsg`. You can capture one output or the whole layout, and choose which monitor shows the result.
* **Smooth zoom.** Scroll or keyboard zoom builds momentum and glides to a stop. Every step is a constant percentage, so it feels the same at any zoom level.
* **Smooth flashlight.** Resizing the spotlight eases continuously to the new size, and the edge is anti-aliased.
* **Inertial panning.** Drag with the left mouse button and let go to fling the view.
* **Flashlight burst and mirror mode.** Quick ways to point at something or flip the view horizontally.
* **No JSON library.** A tiny built-in scanner reads the few fields needed from `swaymsg`.

## Requirements

* A C compiler (`gcc` or `clang`) and `make`
* **raylib** 4.0 or newer
* **grim**, for screenshots
* **sway**, which provides `swaymsg`, for monitor geometry

Not supported: GNOME and KDE. They do not provide `swaymsg` and their compositors do not implement the screencopy protocol that `grim` uses.

### Arch / Artix Linux

```
sudo pacman -S gcc make raylib grim sway
```

## Build and Install

```
git clone https://github.com/mushfiqurrahman1729/woomer-c.git
cd woomer-c

# Either
make

# or
chmod +x build.sh
./build.sh
```

This produces the `woomer` binary in the current directory.

The flashlight shader is loaded from the first of these that exists:

1. `shaders/spotlight.fs` (relative to where you run it)
2. `/usr/share/woomer/shaders/spotlight.fs`
3. `/usr/local/share/woomer/shaders/spotlight.fs`

If none is found, an identical copy compiled into the binary is used, so `woomer` still works on its own.

## Usage

Run it from a terminal, or bind it to a key in your Sway config:

```
./woomer [OPTIONS]
```

```
bindsym $mod+z exec woomer
```

### Options

| Option | Description |
| ----- | ----- |
| `--monitor <name>` | Output that the window is shown on. Defaults to the first output. |
| `--output <name>` | Output to capture. Captures all outputs if not given. |
| `--radius <number>` | Starting flashlight size. Must be greater than 0. Defaults to `1`. |
| `-S`, `--show-cursor` | Include the mouse cursor in the screenshot. |

Output names are the ones `swaymsg -t get_outputs` shows, such as `DP-1` or `eDP-1`.

### Controls

| Action | Mouse | Keyboard |
| ----- | ----- | ----- |
| **Zoom in / out** (around the cursor) | Scroll wheel | `=` / `-` |
| **Flashlight bigger / smaller** (flashlight must be on) | `Shift` + scroll wheel | `+` / `_` |
| **Pan** | Hold left button and drag, release to fling | |
| **Toggle flashlight** | | `F` |
| **Flashlight burst** | | `Ctrl` |
| **Mirror** | | `M` |
| **Reset view** | | `0` |
| **Quit** | Right click | `Esc`, `Q` or `A` |

Notes:

* Tap `=` or `-` for one step, hold for continuous zoom. `+` and `_` work the same way for the flashlight.
* `+` and `_` are `Shift` + `=` and `Shift` + `-` on a US-style layout.
* The `Ctrl` burst jumps the flashlight to three times its current size, then eases back.
* `0` resets zoom, position and mirror. It does not change the flashlight size.

## Configuration

The feel of zoom and the flashlight is set by `#define`s at the top of `main.c`:

| Setting | Effect |
| ----- | ----- |
| `ZOOM_DECAY` | How quickly zoom momentum fades. |
| `ZOOM_LOG_GAIN` | How far one scroll tick or key press zooms. |
| `ZOOM_KEY_RATE` | Zoom speed while `=` or `-` is held. |
| `RADIUS_MIN`, `RADIUS_MAX` | Smallest and largest flashlight size. |
| `RADIUS_TICK_LOG` | Size change per scroll tick or key press. |
| `RADIUS_KEY_RATE` | Resize speed while `+` or `_` is held. |
| `RADIUS_SMOOTH` | How quickly the flashlight reaches its new size. Higher is snappier. |
| `BURST_DECAY` | How quickly the `Ctrl` burst fades. |

The dimming colour is `SPOTLIGHT_TINT`, and the base flashlight size is `UNIT_RADIUS` in the shader. Rebuild after changing either.

## License

Released under the GNU General Public License.
