# argonine-inputd

Input daemon for [Argonine](https://github.com/woouw/All-Linux-OS-rg35xxh) — Alpine Linux for Anbernic RG handhelds.

Grabs the gamepad, volume and power evdev nodes and re-emits them as keyboard + mouse events on one virtual uinput device. Apps see a normal keyboard and mouse; nothing needs to know about the handheld's button layout.

## Button map

"Shifted" means **L-stick click held**. A shifted button with no shifted action behaves as normal.

| Button | Action | Shifted |
|---|---|---|
| D-pad | Arrow keys | Resize window (XF86Launch1–4) |
| L1 / R1 | F17 / F18 (prev / next workspace) | F19 / F20 (move window to prev / next workspace) |
| L2 | Tab | |
| R2 | Enter | |
| A | Right click | |
| B | Left click | |
| X | F14 (floating keyboard) | F21 (kill window) |
| Y | F15 (floating toggle) | F22 (fullscreen) |
| L-stick | Mouse cursor | |
| R-stick | Scroll | |
| L-stick click | Shift modifier | |
| R-stick click | Mute | |
| Select | Escape (tap) | |
| Start | Super | |
| Select + Start + L2 / R2 | Previous / next TTY | |
| F (top) | F13 (terminal) | F16 (launcher) |
| Vol up / down | Volume up / down | Next / previous track |
| Power, short press | Toggle low-power (display off) | |
| Power, hold 1.5 s | Graceful `poweroff` | |

## Sway config

```
set $mod Mod4

bindsym F13 exec foot
bindsym F16 exec wmenu-run
bindsym F14 exec wvkbd-mobintl
bindsym F15 floating toggle
bindsym F22 fullscreen toggle
bindsym F21 kill

bindsym F17 workspace prev
bindsym F18 workspace next
bindsym F19 move container to workspace prev
bindsym F20 move container to workspace next

bindsym XF86Launch1 resize shrink width  20px
bindsym XF86Launch2 resize grow   width  20px
bindsym XF86Launch3 resize shrink height 20px
bindsym XF86Launch4 resize grow   height 20px
```

## Build

On the device:
```sh
apk add build-base linux-headers
make && make install
rc-update add inputd default
rc-service inputd start
```

Cross-compile:
```sh
make CC=aarch64-linux-musl-gcc CFLAGS="-O2 -Wall -std=c11 -static"
```

## Fixing a wrong button

Nothing about the device layout is hard-coded beyond sensible defaults. If a button does the wrong thing, find out what it really sends:

```sh
doas rc-service inputd stop
doas inputd -d
```

Press the button; the log shows `type`, `code` and `value`. Then remap it in `/etc/inputd.conf`:

```
button 305 A
```

See `inputd.conf` for the logical names.

## Pinning devices

Auto-detection is normally enough. To force specific nodes, put arguments in `/etc/conf.d/inputd`:

```sh
INPUTD_ARGS="--gamepad /dev/input/event3 --power /dev/input/event0"
```

When any path is given, only those nodes are used.

## How it works

- Opens every `/dev/input/event*` that looks like a gamepad (`BTN_SOUTH`/`BTN_EAST`) or has volume/power keys, skipping real keyboards and its own virtual device, and grabs them (`EVIOCGRAB`) so apps don't also see the raw events.
- Analog ranges come from the kernel (`EVIOCGABS`) and are normalised, so any stick range works.
- D-pad is accepted as `ABS_HAT0X/Y`, `BTN_DPAD_*` or `KEY_UP/DOWN/LEFT/RIGHT`.
- A 16 ms timer turns stick deflection into continuous mouse motion (with deadzone and acceleration) and scroll.
- The key emitted on press is remembered per button, so releasing after the shift state changed never leaves a stuck key.
- TTY switching uses `VT_ACTIVATE` directly.
- Power: short press toggles the backlight (`bl_power`, falling back to `brightness`); a 1.5 s hold runs `poweroff` while the button is still down.
