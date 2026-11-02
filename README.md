# ALOS-inputd

Input daemon for the Anbernic RG35XX H running Alpine Linux (ALOS).

Reads raw evdev events from the gamepad and power button, emits keyboard and mouse events on a virtual uinput device. Apps see a normal keyboard/mouse — no gamepad awareness needed.

## Button map

| Button | Action | Shifted (L-stick held) |
|---|---|---|
| D-pad | Arrow keys | Resize window (F23/F24 and F11/F12 → sway binds) |
| L1 | F17 (prev workspace) | F19 (move window to prev workspace) |
| R1 | F18 (next workspace) | F20 (move window to next workspace) |
| L2 | Tab | — |
| R2 | Enter | — |
| A | Right click | — |
| B | Left click | — |
| X | F14 (floating keyboard) | F21 (kill window) |
| Y | F15 (float toggle) | F22 (fullscreen) |
| L-stick axis | Mouse cursor | — |
| R-stick axis | Scroll (REL_WHEEL / REL_HWHEEL) | — |
| L-stick click | Shift modifier | — |
| R-stick click | XF86AudioMute | — |
| Select | Escape | — |
| Start | Super (mod key) | — |
| Select + Start + L2 | Prev TTY (chvt) | — |
| Select + Start + R2 | Next TTY (chvt) | — |
| F (top) | F13 (spawn terminal) | F16 (wmenu) |
| Vol up | XF86AudioRaiseVolume | XF86AudioNext |
| Vol down | XF86AudioLowerVolume | XF86AudioPrev |
| Power short press | Toggle low-power mode (blank display) | — |
| Power long press (≥1.5s) | Graceful shutdown | — |

## Sway config snippet

```
set $mod Mod4

bindsym F13 exec foot
bindsym $mod+F13 exec wmenu-run

bindsym F14 exec wvkbd-mobintl
bindsym $mod+F14 exec wvkbd-mobintl

bindsym F15 floating toggle
bindsym $mod+F15 fullscreen toggle

bindsym F17 workspace prev
bindsym F18 workspace next
bindsym F19 move container to workspace prev
bindsym F20 move container to workspace next

bindsym F21 kill
bindsym F22 fullscreen toggle

bindsym F23 resize shrink width  20px
bindsym F24 resize grow   width  20px
bindsym F11 resize shrink height 20px
bindsym F12 resize grow   height 20px
```

## Build

On the device:
```sh
apk add build-base
make
make install
rc-update add inputd sysinit
```

Cross-compile (on Alpine x86_64 host):
```sh
apk add aarch64-linux-musl-cross
make CC=aarch64-linux-musl-gcc CFLAGS="-O2 -Wall -std=c11 -static"
```

## How it works

- Discovers gamepad (has `BTN_SOUTH` + `ABS_X`) and power button (`KEY_POWER`) from `/dev/input/event*` automatically, or accepts paths as arguments.
- Creates a virtual uinput device exporting key + relative axis events.
- Uses `epoll` to watch both devices simultaneously.
- L-stick click sets a `shift_held` flag; all other buttons check it for their alternate keycode.
- Power button: records `CLOCK_MONOTONIC` timestamp on key-down; on key-up, if ≥1500ms calls `reboot(RB_POWER_OFF)`, else toggles `/sys/class/backlight/*/bl_power`.
- TTY switching uses `VT_ACTIVATE` ioctl on `/dev/tty0` directly — no shell-out.
