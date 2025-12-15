# EvHz2

A simple cross-platform tool to measure the polling rate (events per second) of input devices, primarily focused on mice, but also works with keyboards, touchpads, and other HID devices.

## Supported Platforms
- **Windows**
- **Linux**
- **macOS**
- **FreeBSD**

## Features
- Measures polling rate by calculating the time between consecutive movement events
- Displays real-time latest Hz and sliding average over the last 64 events
- Supports multiple input devices on Linux/FreeBSD (via `/dev/input/event*`)
- Verbose mode (default) shows every detected event rate
- Non-verbose mode (`-n`) only shows final averages
- Graceful exit with Ctrl+C (or ESC on Windows)

## Build
The program is written in pure C and uses only standard libraries plus platform-specific APIs.

```bash
gcc -o evhz2 evhz2.c -lm
```

On macOS you may need to link CoreFoundation and IOKit:
```bash
gcc -o evhz2 evhz2.c -framework CoreFoundation -framework IOKit
```

No external dependencies.

## Usage
```bash
./evhz2 [options]
```

Options:
- `-n`, `--nonverbose` — Suppress per-event output, only show final averages
- `-h`, `--help`       — Show help message

### Linux / FreeBSD
For full access to all input devices, run as root (or ensure your user has read permissions via udev rules):
```bash
sudo ./evhz
```

The program will automatically detect and monitor all available `/dev/input/event*` devices. Move your mouse or use other input devices to see rates.

### Windows
Just run the executable. Move your mouse around. Press **ESC** to exit.

### macOS
Run normally. Move mouse or use keyboard/touchpad. Press **Ctrl+C** to exit.

Note: On macOS, reported rates may be limited by the OS rather than the hardware capabilities.

## Example Output (Linux)
```
Event Hz Tester - Linux
====================

event2: USB OPTICAL MOUSE
event4: Logitech G Pro Wireless Gaming Mouse
...

Press CTRL-C to exit.

USB OPTICAL MOUSE: Latest  125Hz, Average  125Hz
Logitech G Pro Wireless Gaming Mouse: Latest 1000Hz, Average 1000Hz
...

Average for USB OPTICAL MOUSE:   125Hz
Average for Logitech G Pro Wireless Gaming Mouse:  1000Hz
```

## Limitations & Notes
- **Windows**: Most accurate for mouse polling rate (uses cursor position changes).
- **Linux/FreeBSD**: Uses kernel timestamps from `input_event`. Results reflect delivery rate to userspace, which is usually very close to hardware rate.
- **macOS**: Uses local timestamps in HID callback. Apple often throttles external devices, so results may be lower than advertised hardware polling rate.
- The tool measures only relative/absolute movement events (EV_REL/EV_ABS). Button presses and other events are ignored for Hz calculation.

## License
This project is licensed under the GNU General Public License v3.0 — see the LICENSE file for details.

Enjoy checking if your "8000 Hz" mouse is really delivering! 🖱️🚀
