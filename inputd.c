/*
 * inputd — ALOS input daemon for Anbernic RG35XX H
 *
 * Reads raw evdev events from the gamepad and emits keyboard/mouse events
 * on a virtual uinput device. Handles:
 *
 *   - Button → keycode mapping with a shift layer (L-stick click held)
 *   - L-stick axis → mouse cursor (REL_X / REL_Y)
 *   - R-stick axis → scroll wheel (REL_WHEEL / REL_HWHEEL)
 *   - L1/R1 → prev/next workspace (F17/F18), shifted: move to prev/next (F19/F20)
 *   - L2/R2 → Tab / Enter
 *   - Select → Escape
 *   - Start → Super (KEY_LEFTMETA)
 *   - A → right click, B → left click
 *   - X → F14 (floating kbd), shifted: F21 (kill window)
 *   - Y → F15 (float toggle), shifted: F22 (fullscreen)
 *   - F (top) → F13 (spawn terminal), shifted: F16 (wmenu)
 *   - Vol up/down → XF86AudioRaiseVolume / XF86AudioLowerVolume
 *     shifted: XF86AudioNext / XF86AudioPrev
 *   - R-stick click → XF86AudioMute
 *   - D-pad → arrow keys, shifted: resize (F23/F24/F25/F26 — sway resize binds)
 *   - Select + Start + L2 → chvt prev TTY
 *   - Select + Start + R2 → chvt next TTY
 *   - Power short press → toggle low-power mode (blank/unblank DSI panel)
 *   - Power long press (≥1500ms) → graceful shutdown
 *
 * Build:
 *   cc -O2 -Wall -std=c11 -o inputd inputd.c
 *
 * Run as root (needs /dev/input/* and /dev/uinput access):
 *   inputd [gamepad-event-path] [power-event-path]
 * Paths are auto-detected if omitted.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Config                                                               */
/* ------------------------------------------------------------------ */

#define POWER_LONG_MS       1500    /* hold duration for shutdown */
#define MOUSE_SENSITIVITY   8       /* pixels per axis tick */
#define MOUSE_DEADZONE      4096    /* abs axis deadzone (0–32767 range) */
#define SCROLL_SENSITIVITY  1       /* scroll clicks per axis tick */
#define SCROLL_DEADZONE     8192
#define AXIS_MAX            32767

#define BACKLIGHT_PATH      "/sys/class/backlight/backlight/bl_power"
#define BACKLIGHT_PATH_ALT  "/sys/class/backlight/sunxi-backlight/bl_power"

/* ------------------------------------------------------------------ */
/* Key mapping tables                                                   */
/* ------------------------------------------------------------------ */

/* Each entry: { evdev_code, normal_key, shifted_key }
 * 0 = unbound, -1 = handled specially in code */

struct btn_map {
    unsigned int code;
    unsigned int normal;
    unsigned int shifted;
};

/* Face / shoulder / misc buttons */
static const struct btn_map BUTTON_MAP[] = {
    /* ABXY */
    { BTN_SOUTH,    BTN_RIGHT,          0             },  /* A → right click */
    { BTN_EAST,     BTN_LEFT,           0             },  /* B → left click */
    { BTN_WEST,     KEY_F14,            KEY_F21       },  /* X → F14 / F21 */
    { BTN_NORTH,    KEY_F15,            KEY_F22       },  /* Y → F15 / F22 */
    /* Shoulders */
    { BTN_TL,       KEY_F17,            KEY_F19       },  /* L1 → F17 / F19 */
    { BTN_TR,       KEY_F18,            KEY_F20       },  /* R1 → F18 / F20 */
    { BTN_TL2,      KEY_TAB,            0             },  /* L2 → Tab */
    { BTN_TR2,      KEY_ENTER,          0             },  /* R2 → Enter */
    /* Select / Start */
    { BTN_SELECT,   KEY_ESC,            0             },  /* Select → Esc */
    { BTN_START,    KEY_LEFTMETA,       0             },  /* Start → Super */
    /* Stick clicks */
    { BTN_THUMBL,   0,                  0             },  /* L-stick: shift (special) */
    { BTN_THUMBR,   KEY_MUTE,           0             },  /* R-stick → mute */
    /* F button — mapped as BTN_MODE on this device */
    { BTN_MODE,     KEY_F13,            KEY_F16       },  /* F → F13 / F16 */
    /* Volume (these come as keys on a separate device) */
    { KEY_VOLUMEUP,   KEY_VOLUMEUP,     KEY_NEXTSONG  },
    { KEY_VOLUMEDOWN, KEY_VOLUMEDOWN,   KEY_PREVIOUSSONG },
    /* D-pad (hat switches converted to keys by kernel) */
    { KEY_LEFT,     KEY_LEFT,           KEY_F23       },  /* D-left / resize */
    { KEY_RIGHT,    KEY_RIGHT,          KEY_F24       },
    { KEY_UP,       KEY_UP,             KEY_F25       },
    { KEY_DOWN,     KEY_DOWN,           KEY_F26       },
};
#define BUTTON_MAP_LEN (sizeof(BUTTON_MAP) / sizeof(BUTTON_MAP[0]))

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static int shift_held   = 0;   /* L-stick click held */
static int select_held  = 0;
static int start_held   = 0;
static int l2_held      = 0;
static int r2_held      = 0;

static int lowpower     = 0;   /* display blanked */
static int current_tty  = 1;

/* Power button hold tracking */
static int    power_held    = 0;
static struct timespec power_down_ts;

/* uinput fd */
static int ufd = -1;

/* epoll fd */
static int epfd = -1;

/* ------------------------------------------------------------------ */
/* Backlight                                                            */
/* ------------------------------------------------------------------ */

static const char *backlight_path(void)
{
    if (access(BACKLIGHT_PATH, W_OK) == 0)     return BACKLIGHT_PATH;
    if (access(BACKLIGHT_PATH_ALT, W_OK) == 0) return BACKLIGHT_PATH_ALT;
    return NULL;
}

static void set_backlight(int blank)
{
    const char *p = backlight_path();
    if (!p) { fprintf(stderr, "inputd: no backlight path found\n"); return; }
    int fd = open(p, O_WRONLY);
    if (fd < 0) { perror(p); return; }
    write(fd, blank ? "1\n" : "0\n", 2);
    close(fd);
}

static void toggle_lowpower(void)
{
    lowpower = !lowpower;
    set_backlight(lowpower);
    fprintf(stderr, "inputd: low-power %s\n", lowpower ? "on" : "off");
}

/* ------------------------------------------------------------------ */
/* TTY switching                                                        */
/* ------------------------------------------------------------------ */

static void chvt(int n)
{
    int fd = open("/dev/tty0", O_RDONLY);
    if (fd < 0) fd = open("/dev/tty1", O_RDONLY);
    if (fd < 0) { perror("chvt: open tty"); return; }
    if (ioctl(fd, VT_ACTIVATE, n) < 0) perror("VT_ACTIVATE");
    close(fd);
    current_tty = n;
}

static void tty_prev(void) { if (current_tty > 1) chvt(current_tty - 1); }
static void tty_next(void) { chvt(current_tty + 1); }

/* ------------------------------------------------------------------ */
/* uinput helpers                                                       */
/* ------------------------------------------------------------------ */

static void ui_emit(unsigned int type, unsigned int code, int value)
{
    struct input_event ev = {
        .type  = (__u16)type,
        .code  = (__u16)code,
        .value = value,
    };
    write(ufd, &ev, sizeof(ev));
}

static void ui_sync(void)    { ui_emit(EV_SYN, SYN_REPORT, 0); }
static void ui_key(unsigned int code, int val) { ui_emit(EV_KEY, code, val); ui_sync(); }
static void ui_rel(unsigned int code, int val) { ui_emit(EV_REL, code, val); ui_sync(); }

/* ------------------------------------------------------------------ */
/* uinput device setup                                                  */
/* ------------------------------------------------------------------ */

static void uinput_set_bit(int fd, unsigned long req, unsigned int bit)
{
    if (ioctl(fd, req, bit) < 0) {
        perror("ioctl set bit");
        exit(1);
    }
}

static int uinput_create(void)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("/dev/uinput"); exit(1); }

    uinput_set_bit(fd, UI_SET_EVBIT, EV_KEY);
    uinput_set_bit(fd, UI_SET_EVBIT, EV_REL);
    uinput_set_bit(fd, UI_SET_EVBIT, EV_SYN);

    /* Keys */
    static const unsigned int keys[] = {
        KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
        KEY_TAB, KEY_ENTER, KEY_ESC, KEY_LEFTMETA,
        KEY_MUTE, KEY_VOLUMEUP, KEY_VOLUMEDOWN,
        KEY_NEXTSONG, KEY_PREVIOUSSONG,
        KEY_F13, KEY_F14, KEY_F15, KEY_F16,
        KEY_F17, KEY_F18, KEY_F19, KEY_F20,
        KEY_F21, KEY_F22, KEY_F23, KEY_F24,
        BTN_LEFT, BTN_RIGHT,
    };
    for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); i++)
        uinput_set_bit(fd, UI_SET_KEYBIT, keys[i]);

    /* Relative axes */
    uinput_set_bit(fd, UI_SET_RELBIT, REL_X);
    uinput_set_bit(fd, UI_SET_RELBIT, REL_Y);
    uinput_set_bit(fd, UI_SET_RELBIT, REL_WHEEL);
    uinput_set_bit(fd, UI_SET_RELBIT, REL_HWHEEL);

    struct uinput_setup us = {
        .name = "ALOS inputd virtual device",
        .id   = { .bustype = BUS_VIRTUAL, .vendor = 0x1, .product = 0x1, .version = 1 },
    };
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0) { perror("UI_DEV_SETUP"); exit(1); }
    if (ioctl(fd, UI_DEV_CREATE) < 0)     { perror("UI_DEV_CREATE"); exit(1); }

    return fd;
}

/* ------------------------------------------------------------------ */
/* evdev device discovery                                               */
/* ------------------------------------------------------------------ */

/* Returns 1 if the evdev fd has the given key/button bit set */
static int has_key(int fd, unsigned int key)
{
    unsigned long bits[(KEY_MAX + 1) / (8 * sizeof(unsigned long)) + 1];
    memset(bits, 0, sizeof(bits));
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits);
    return !!(bits[key / (8 * sizeof(unsigned long))] &
              (1UL << (key % (8 * sizeof(unsigned long)))));
}

static int has_abs(int fd, unsigned int axis)
{
    unsigned long bits[(ABS_MAX + 1) / (8 * sizeof(unsigned long)) + 1];
    memset(bits, 0, sizeof(bits));
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits);
    return !!(bits[axis / (8 * sizeof(unsigned long))] &
              (1UL << (axis % (8 * sizeof(unsigned long)))));
}

/* Open all matching event devices; returns count */
static int open_devices(int *gamepad_fd, int *power_fd,
                         const char *gamepad_arg, const char *power_arg)
{
    *gamepad_fd = -1;
    *power_fd   = -1;

    if (gamepad_arg) {
        *gamepad_fd = open(gamepad_arg, O_RDONLY | O_NONBLOCK);
        if (*gamepad_fd < 0) { perror(gamepad_arg); exit(1); }
    }
    if (power_arg) {
        *power_fd = open(power_arg, O_RDONLY | O_NONBLOCK);
        if (*power_fd < 0) { perror(power_arg); exit(1); }
    }
    if (*gamepad_fd >= 0 && *power_fd >= 0) return 2;

    DIR *d = opendir("/dev/input");
    if (!d) { perror("/dev/input"); exit(1); }

    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;

        if (*gamepad_fd < 0 && has_key(fd, BTN_SOUTH) && has_abs(fd, ABS_X)) {
            *gamepad_fd = fd;
            fprintf(stderr, "inputd: gamepad  → %s\n", path);
            continue;
        }
        if (*power_fd < 0 && has_key(fd, KEY_POWER)) {
            *power_fd = fd;
            fprintf(stderr, "inputd: power    → %s\n", path);
            continue;
        }
        close(fd);
    }
    closedir(d);

    if (*gamepad_fd < 0) { fprintf(stderr, "inputd: no gamepad found\n"); exit(1); }
    if (*power_fd   < 0)   fprintf(stderr, "inputd: no power button found (continuing)\n");

    return (*power_fd >= 0) ? 2 : 1;
}

/* ------------------------------------------------------------------ */
/* Event processing                                                     */
/* ------------------------------------------------------------------ */

static void handle_button(unsigned int code, int value)
{
    /* Shift modifier (L-stick click) */
    if (code == BTN_THUMBL) {
        shift_held = value;
        return;
    }

    /* Track chord components */
    if (code == BTN_SELECT)  { select_held = value; }
    if (code == BTN_START)   { start_held  = value; }
    if (code == BTN_TL2)     { l2_held     = value; }
    if (code == BTN_TR2)     { r2_held     = value; }

    /* TTY chord: Select + Start + L2/R2 (fire on press only) */
    if (value == 1 && select_held && start_held) {
        if (code == BTN_TL2) { tty_prev(); return; }
        if (code == BTN_TR2) { tty_next(); return; }
    }

    /* Suppress Select/Start/L2/R2 normal emit when used in chord */
    if ((code == BTN_SELECT || code == BTN_START ||
         code == BTN_TL2    || code == BTN_TR2) &&
        (select_held && start_held)) {
        return;
    }

    /* Look up in table */
    for (size_t i = 0; i < BUTTON_MAP_LEN; i++) {
        if (BUTTON_MAP[i].code != code) continue;

        unsigned int key = shift_held ? BUTTON_MAP[i].shifted : BUTTON_MAP[i].normal;
        if (key == 0) return; /* unbound */

        /* Mouse buttons */
        if (key == BTN_LEFT || key == BTN_RIGHT) {
            ui_emit(EV_KEY, key, value);
            ui_sync();
        } else {
            ui_emit(EV_KEY, key, value);
            ui_sync();
        }
        return;
    }
}

static void handle_axis(unsigned int code, int raw_value)
{
    int v = raw_value;

    switch (code) {
    /* L-stick → mouse cursor */
    case ABS_X:
        if (abs(v) < MOUSE_DEADZONE) break;
        ui_rel(REL_X, (v * MOUSE_SENSITIVITY) / AXIS_MAX);
        break;
    case ABS_Y:
        if (abs(v) < MOUSE_DEADZONE) break;
        ui_rel(REL_Y, (v * MOUSE_SENSITIVITY) / AXIS_MAX);
        break;
    /* R-stick → scroll */
    case ABS_RX:
        if (abs(v) < SCROLL_DEADZONE) break;
        ui_rel(REL_HWHEEL, (v * SCROLL_SENSITIVITY) / AXIS_MAX);
        break;
    case ABS_RY:
        if (abs(v) < SCROLL_DEADZONE) break;
        /* Invert Y: push up = scroll up */
        ui_rel(REL_WHEEL, -(v * SCROLL_SENSITIVITY) / AXIS_MAX);
        break;
    default:
        break;
    }
}

static void handle_power(int value)
{
    if (value == 1) {
        /* Key down — record time */
        power_held = 1;
        clock_gettime(CLOCK_MONOTONIC, &power_down_ts);
    } else if (value == 0 && power_held) {
        /* Key up — measure hold duration */
        power_held = 0;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec  - power_down_ts.tv_sec)  * 1000 +
                  (now.tv_nsec - power_down_ts.tv_nsec) / 1000000;

        if (ms >= POWER_LONG_MS) {
            fprintf(stderr, "inputd: long press → shutdown\n");
            sync();
            reboot(RB_POWER_OFF);
        } else {
            toggle_lowpower();
        }
    }
}

static void process_event(const struct input_event *ev)
{
    if (ev->type == EV_KEY) {
        handle_button(ev->code, ev->value);
    } else if (ev->type == EV_ABS) {
        handle_axis(ev->code, ev->value);
    }
}

static void process_power_event(const struct input_event *ev)
{
    if (ev->type == EV_KEY && ev->code == KEY_POWER)
        handle_power(ev->value);
}

/* ------------------------------------------------------------------ */
/* Signal handling                                                      */
/* ------------------------------------------------------------------ */

static volatile int running = 1;
static void on_signal(int s) { (void)s; running = 0; }

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    const char *gamepad_arg = argc > 1 ? argv[1] : NULL;
    const char *power_arg   = argc > 2 ? argv[2] : NULL;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    int gpfd, pwfd;
    open_devices(&gpfd, &pwfd, gamepad_arg, power_arg);

    ufd  = uinput_create();
    epfd = epoll_create1(0);
    if (epfd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = gpfd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, gpfd, &ev);
    if (pwfd >= 0) {
        ev.data.fd = pwfd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, pwfd, &ev);
    }

    fprintf(stderr, "inputd: running\n");

    struct epoll_event events[8];
    while (running) {
        int n = epoll_wait(epfd, events, 8, 100);
        if (n < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < n; i++) {
            struct input_event iev;
            int fd = events[i].data.fd;
            while (read(fd, &iev, sizeof(iev)) == sizeof(iev)) {
                if (fd == pwfd)
                    process_power_event(&iev);
                else
                    process_event(&iev);
            }
        }
    }

    fprintf(stderr, "inputd: shutting down\n");
    ioctl(ufd, UI_DEV_DESTROY);
    close(ufd);
    close(gpfd);
    if (pwfd >= 0) close(pwfd);
    close(epfd);
    return 0;
}
