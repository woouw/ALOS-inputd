/*
 * inputd — ALOS input daemon for Anbernic RG handhelds
 *
 * Grabs the handheld's gamepad / volume / power evdev nodes and re-emits
 * their events as keyboard + mouse events on one virtual uinput device.
 *
 * Nothing here assumes a particular device layout:
 *   - devices are discovered by capability, not by path
 *   - analog ranges are read from the kernel (EVIOCGABS) and normalised
 *   - the D-pad works whether it is ABS_HAT0X/Y, BTN_DPAD_* or KEY_UP/...
 *   - any evdev code can be remapped in /etc/inputd.conf
 *
 * Usage:  inputd [-d] [-c /etc/inputd.conf]
 *   -d   debug: log every raw event (name, code, value) to stderr.
 *        Use it to find the code a button really sends, then remap it.
 *
 * Build:  cc -O2 -Wall -Wextra -std=c11 -o inputd inputd.c
 * Needs:  /dev/uinput (modprobe uinput), root.
 */

#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <linux/vt.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Tunables                                                             */
/* ------------------------------------------------------------------ */

#define TICK_MS            16      /* mouse / scroll / power timer period */
#define POWER_LONG_MS      1500    /* power hold for shutdown */
#define STICK_DEADZONE     0.15f   /* fraction of full deflection */
#define MOUSE_SPEED        12.0f   /* px per tick at full deflection */
#define SCROLL_SPEED       0.12f   /* notches per tick at full deflection */
#define MAX_VT             12
#define VNAME              "ALOS inputd virtual device"

/* ------------------------------------------------------------------ */
/* Logical buttons                                                      */
/* ------------------------------------------------------------------ */

enum {
    B_A, B_B, B_X, B_Y,
    B_L1, B_R1, B_L2, B_R2,
    B_SELECT, B_START, B_L3, B_R3, B_F,
    B_VOLUP, B_VOLDN,
    B_UP, B_DOWN, B_LEFT, B_RIGHT,
    B_COUNT
};

static const char *const BNAME[B_COUNT] = {
    "A", "B", "X", "Y", "L1", "R1", "L2", "R2",
    "SELECT", "START", "L3", "R3", "F", "VOLUP", "VOLDN",
    "UP", "DOWN", "LEFT", "RIGHT",
};

/* What each logical button emits. shifted==0 falls back to normal.
 * L3 is the shift modifier itself and is handled specially. */
static const struct { unsigned normal, shifted; } MAP[B_COUNT] = {
    [B_A]      = { BTN_RIGHT,        0 },
    [B_B]      = { BTN_LEFT,         0 },
    [B_X]      = { KEY_F14,          KEY_F21 },
    [B_Y]      = { KEY_F15,          KEY_F22 },
    [B_L1]     = { KEY_F17,          KEY_F19 },
    [B_R1]     = { KEY_F18,          KEY_F20 },
    [B_L2]     = { KEY_TAB,          0 },
    [B_R2]     = { KEY_ENTER,        0 },
    [B_SELECT] = { KEY_ESC,          0 },   /* emitted as a tap on release */
    [B_START]  = { KEY_LEFTMETA,     0 },
    [B_L3]     = { 0,                0 },
    [B_R3]     = { KEY_MUTE,         0 },
    [B_F]      = { KEY_F13,          KEY_F16 },
    [B_VOLUP]  = { KEY_VOLUMEUP,     KEY_NEXTSONG },
    [B_VOLDN]  = { KEY_VOLUMEDOWN,   KEY_PREVIOUSSONG },
    /* shifted D-pad = resize (XF86Launch1..4 → sway binds) */
    [B_UP]     = { KEY_UP,           KEY_PROG3 },
    [B_DOWN]   = { KEY_DOWN,         KEY_PROG4 },
    [B_LEFT]   = { KEY_LEFT,         KEY_PROG1 },
    [B_RIGHT]  = { KEY_RIGHT,        KEY_PROG2 },
};

/* evdev code -> logical button (-1 = none). Defaults below, overridable
 * from the config file. */
static short code2btn[KEY_CNT];

static void init_default_codes(void)
{
    for (int i = 0; i < KEY_CNT; i++) code2btn[i] = -1;
    static const struct { int code, btn; } d[] = {
        { BTN_SOUTH, B_A }, { BTN_EAST, B_B }, { BTN_WEST, B_X }, { BTN_NORTH, B_Y },
        { BTN_TL, B_L1 }, { BTN_TR, B_R1 }, { BTN_TL2, B_L2 }, { BTN_TR2, B_R2 },
        { BTN_SELECT, B_SELECT }, { BTN_START, B_START },
        { BTN_THUMBL, B_L3 }, { BTN_THUMBR, B_R3 }, { BTN_MODE, B_F },
        { KEY_VOLUMEUP, B_VOLUP }, { KEY_VOLUMEDOWN, B_VOLDN },
        { BTN_DPAD_UP, B_UP }, { BTN_DPAD_DOWN, B_DOWN },
        { BTN_DPAD_LEFT, B_LEFT }, { BTN_DPAD_RIGHT, B_RIGHT },
        { KEY_UP, B_UP }, { KEY_DOWN, B_DOWN },
        { KEY_LEFT, B_LEFT }, { KEY_RIGHT, B_RIGHT },
    };
    for (size_t i = 0; i < sizeof(d) / sizeof(d[0]); i++)
        code2btn[d[i].code] = (short)d[i].btn;
}

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */

static int debug;
static int ufd = -1;

static int      down[B_COUNT];
static unsigned emitted[B_COUNT];   /* key emitted on press, so release matches */
static int      shift;              /* L3 held */
static int      select_tap;         /* Select pressed, no chord yet */

static float lx, ly, rx, ry;        /* normalised sticks, -1..1 */
static float mx_acc, my_acc, sx_acc, sy_acc;

static int hat_x, hat_y;            /* previous hat state */
static int trig[2];                 /* analog L2/R2 pressed state */

static int rs_x = ABS_RX, rs_y = ABS_RY;   /* right stick axes (may be Z/RZ) */

static struct { int valid, min, max; } ax[ABS_CNT];

static int       lowpower;
static int       power_down, power_fired;
static long long power_t0;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static float absf(float v) { return v < 0 ? -v : v; }

static int test_bit(const unsigned long *a, unsigned bit)
{
    return !!(a[bit / (8 * sizeof(long))] & (1UL << (bit % (8 * sizeof(long)))));
}

static void emit(unsigned type, unsigned code, int val)
{
    struct input_event e;
    memset(&e, 0, sizeof(e));
    e.type = (__u16)type;
    e.code = (__u16)code;
    e.value = val;
    if (write(ufd, &e, sizeof(e)) < 0 && errno != EAGAIN)
        perror("uinput write");
}

static void flush(void) { emit(EV_SYN, SYN_REPORT, 0); }

/* ------------------------------------------------------------------ */
/* System actions                                                       */
/* ------------------------------------------------------------------ */

static void write_file(const char *path, const char *s)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, s, strlen(s)) < 0) { /* ignore */ }
    close(fd);
}

/* Blank/unblank every backlight. Prefer bl_power; fall back to brightness. */
static void set_display(int blank)
{
    static char saved[64][16];
    glob_t g;
    int did = 0;

    if (glob("/sys/class/backlight/*/bl_power", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            write_file(g.gl_pathv[i], blank ? "4\n" : "0\n");
            did = 1;
        }
        globfree(&g);
    }
    if (did) return;

    if (glob("/sys/class/backlight/*/brightness", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc && i < 64; i++) {
            if (blank) {
                FILE *f = fopen(g.gl_pathv[i], "r");
                if (f) {
                    if (!fgets(saved[i], sizeof(saved[i]), f)) saved[i][0] = 0;
                    fclose(f);
                }
                write_file(g.gl_pathv[i], "0\n");
            } else {
                write_file(g.gl_pathv[i], saved[i][0] ? saved[i] : "128\n");
            }
        }
        globfree(&g);
    }
}

static void toggle_lowpower(void)
{
    lowpower = !lowpower;
    set_display(lowpower);
    fprintf(stderr, "inputd: low-power %s\n", lowpower ? "on" : "off");
}

static void do_poweroff(void)
{
    fprintf(stderr, "inputd: long press -> poweroff\n");
    if (lowpower) set_display(0);
    if (system("poweroff") != 0) { /* nothing more we can do */ }
}

static void chvt_rel(int delta)
{
    int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
    if (fd < 0) fd = open("/dev/console", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("chvt: open tty"); return; }

    struct vt_stat st;
    if (ioctl(fd, VT_GETSTATE, &st) == 0) {
        int n = st.v_active + delta;
        if (n >= 1 && n <= MAX_VT && ioctl(fd, VT_ACTIVATE, n) < 0)
            perror("VT_ACTIVATE");
    } else {
        perror("VT_GETSTATE");
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* Logical button handling                                              */
/* ------------------------------------------------------------------ */

static void btn_event(int b, int val)
{
    if (val == 2) return;   /* kernel autorepeat; the virtual device makes its own */

    if (b == B_L3) { shift = val; return; }

    down[b] = val;

    if (val) {
        /* Chord: Select + Start + L2/R2 switches VT */
        if ((b == B_L2 || b == B_R2) && down[B_SELECT] && down[B_START]) {
            chvt_rel(b == B_L2 ? -1 : +1);
            select_tap = 0;
            emitted[b] = 0;
            return;
        }
        if (b == B_SELECT) { select_tap = 1; emitted[b] = 0; return; }

        unsigned key = (shift && MAP[b].shifted) ? MAP[b].shifted : MAP[b].normal;
        emitted[b] = key;
        if (key) emit(EV_KEY, key, 1);
    } else {
        if (b == B_SELECT) {
            if (select_tap) {            /* plain tap -> Escape */
                emit(EV_KEY, KEY_ESC, 1);
                emit(EV_KEY, KEY_ESC, 0);
            }
            select_tap = 0;
            return;
        }
        if (emitted[b]) emit(EV_KEY, emitted[b], 0);
        emitted[b] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Raw event handling                                                   */
/* ------------------------------------------------------------------ */

static float norm(int code, int v)
{
    if (code < 0 || code >= ABS_CNT || !ax[code].valid || ax[code].max <= ax[code].min)
        return 0;
    float c = (ax[code].min + ax[code].max) / 2.0f;
    float h = (ax[code].max - ax[code].min) / 2.0f;
    float f = ((float)v - c) / h;
    return f > 1 ? 1 : (f < -1 ? -1 : f);
}

static void hat(int *prev, int v, int neg_btn, int pos_btn)
{
    int s = v < 0 ? -1 : (v > 0 ? 1 : 0);
    if (s == *prev) return;
    if (*prev < 0) btn_event(neg_btn, 0);
    if (*prev > 0) btn_event(pos_btn, 0);
    if (s < 0) btn_event(neg_btn, 1);
    if (s > 0) btn_event(pos_btn, 1);
    *prev = s;
}

static void handle_abs(int code, int v)
{
    if      (code == ABS_X)  lx = norm(code, v);
    else if (code == ABS_Y)  ly = norm(code, v);
    else if (code == rs_x)   rx = norm(code, v);
    else if (code == rs_y)   ry = norm(code, v);
    else if (code == ABS_HAT0X) hat(&hat_x, v, B_LEFT, B_RIGHT);
    else if (code == ABS_HAT0Y) hat(&hat_y, v, B_UP, B_DOWN);
    else if (code == ABS_BRAKE || code == ABS_GAS) {
        int i = (code == ABS_GAS);
        int on = norm(code, v) > 0;   /* past half travel */
        if (on != trig[i]) { trig[i] = on; btn_event(i ? B_R2 : B_L2, on); }
    }
}

static void handle_event(const char *devname, const struct input_event *ev)
{
    if (debug && ev->type != EV_SYN && ev->type != EV_MSC)
        fprintf(stderr, "[%s] type=%u code=%u value=%d\n",
                devname, ev->type, ev->code, ev->value);

    if (ev->type == EV_KEY) {
        if (ev->code == KEY_POWER) {
            if (ev->value == 1) {
                power_down = 1; power_fired = 0; power_t0 = now_ms();
            } else if (ev->value == 0 && power_down) {
                power_down = 0;
                if (!power_fired) toggle_lowpower();
            }
            return;
        }
        if (ev->code < KEY_CNT && code2btn[ev->code] >= 0)
            btn_event(code2btn[ev->code], ev->value);
    } else if (ev->type == EV_ABS) {
        handle_abs(ev->code, ev->value);
    } else if (ev->type == EV_SYN) {
        flush();
    }
}

/* ------------------------------------------------------------------ */
/* Timer: sticks -> mouse/scroll, power long-press                      */
/* ------------------------------------------------------------------ */

static float curve(float v)
{
    float a = absf(v);
    if (a < STICK_DEADZONE) return 0;
    a = (a - STICK_DEADZONE) / (1.0f - STICK_DEADZONE);
    a *= a;                          /* gentle acceleration */
    return v < 0 ? -a : a;
}

static void tick(void)
{
    int out = 0;

    mx_acc += curve(lx) * MOUSE_SPEED;
    my_acc += curve(ly) * MOUSE_SPEED;
    int ix = (int)mx_acc, iy = (int)my_acc;
    if (ix) { emit(EV_REL, REL_X, ix); mx_acc -= ix; out = 1; }
    if (iy) { emit(EV_REL, REL_Y, iy); my_acc -= iy; out = 1; }

    sx_acc += curve(rx) * SCROLL_SPEED;
    sy_acc += -curve(ry) * SCROLL_SPEED;   /* push up = scroll up */
    int jx = (int)sx_acc, jy = (int)sy_acc;
    if (jx) { emit(EV_REL, REL_HWHEEL, jx); sx_acc -= jx; out = 1; }
    if (jy) { emit(EV_REL, REL_WHEEL,  jy); sy_acc -= jy; out = 1; }

    /* stop residue drifting when the stick is centred */
    if (curve(lx) == 0) mx_acc = 0;
    if (curve(ly) == 0) my_acc = 0;
    if (curve(rx) == 0) sx_acc = 0;
    if (curve(ry) == 0) sy_acc = 0;

    if (out) flush();

    if (power_down && !power_fired && now_ms() - power_t0 >= POWER_LONG_MS) {
        power_fired = 1;
        do_poweroff();
    }
}

/* ------------------------------------------------------------------ */
/* Config file                                                          */
/* ------------------------------------------------------------------ */

/* Lines:   button <evdev-code> <NAME>     e.g.  button 304 B
 *          debug                          */
static void load_conf(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        unsigned code;
        char name[16];
        if (line[0] == '#') continue;
        if (sscanf(line, "button %i %15s", (int *)&code, name) == 2) {
            int found = -1;
            for (int i = 0; i < B_COUNT; i++)
                if (strcasecmp(name, BNAME[i]) == 0) found = i;
            if (found < 0 || code >= KEY_CNT)
                fprintf(stderr, "inputd: bad config line: %s", line);
            else
                code2btn[code] = (short)found;
        } else if (strncmp(line, "debug", 5) == 0) {
            debug = 1;
        }
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Devices                                                              */
/* ------------------------------------------------------------------ */

#define MAX_DEV 16
static struct { int fd; char name[64]; } devs[MAX_DEV];
static int ndev;

static void load_abs(int fd)
{
    unsigned long bits[ABS_CNT / (8 * sizeof(long)) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) < 0) return;

    for (int c = 0; c < ABS_CNT; c++) {
        if (!test_bit(bits, c)) continue;
        struct input_absinfo ai;
        if (ioctl(fd, EVIOCGABS(c), &ai) < 0) continue;
        ax[c].valid = 1; ax[c].min = ai.minimum; ax[c].max = ai.maximum;
    }
    /* No dedicated right stick? Many pads use Z/RZ for it. */
    if (!test_bit(bits, ABS_RX) && test_bit(bits, ABS_Z) && test_bit(bits, ABS_RZ)) {
        rs_x = ABS_Z; rs_y = ABS_RZ;
    }
}

static void scan_devices(int ep)
{
    DIR *d = opendir("/dev/input");
    if (!d) { perror("/dev/input"); exit(1); }

    struct dirent *e;
    while ((e = readdir(d)) && ndev < MAX_DEV) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;

        char name[64] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);

        unsigned long keys[KEY_CNT / (8 * sizeof(long)) + 1];
        memset(keys, 0, sizeof(keys));
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);

        int ours   = strcmp(name, VNAME) == 0;
        int is_kbd = test_bit(keys, KEY_A);      /* real keyboards: leave alone */
        int is_pad = test_bit(keys, BTN_SOUTH) || test_bit(keys, BTN_EAST);
        int is_key = test_bit(keys, KEY_VOLUMEUP) || test_bit(keys, KEY_VOLUMEDOWN) ||
                     test_bit(keys, KEY_POWER);

        if (ours || is_kbd || !(is_pad || is_key)) { close(fd); continue; }

        if (is_pad) load_abs(fd);
        if (ioctl(fd, EVIOCGRAB, 1) < 0)
            fprintf(stderr, "inputd: could not grab %s (%s)\n", path, name);

        struct epoll_event ev = { .events = EPOLLIN, .data.u32 = (uint32_t)ndev };
        epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
        devs[ndev].fd = fd;
        snprintf(devs[ndev].name, sizeof(devs[ndev].name), "%s", name);
        fprintf(stderr, "inputd: using %s \"%s\"%s%s\n", path, name,
                is_pad ? " [pad]" : "", is_key ? " [keys]" : "");
        ndev++;
    }
    closedir(d);
}

/* ------------------------------------------------------------------ */
/* uinput                                                               */
/* ------------------------------------------------------------------ */

static void ui_bit(unsigned long req, unsigned bit)
{
    if (ioctl(ufd, req, bit) < 0) { perror("uinput ioctl"); exit(1); }
}

static void uinput_create(void)
{
    ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) ufd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) {
        perror("open /dev/uinput (is the uinput module loaded?)");
        exit(1);
    }

    ui_bit(UI_SET_EVBIT, EV_KEY);
    ui_bit(UI_SET_EVBIT, EV_REL);
    ui_bit(UI_SET_EVBIT, EV_REP);    /* lets the kernel autorepeat held keys */
    ui_bit(UI_SET_EVBIT, EV_SYN);

    ui_bit(UI_SET_KEYBIT, KEY_ESC);
    for (int b = 0; b < B_COUNT; b++) {
        if (MAP[b].normal)  ui_bit(UI_SET_KEYBIT, MAP[b].normal);
        if (MAP[b].shifted) ui_bit(UI_SET_KEYBIT, MAP[b].shifted);
    }

    ui_bit(UI_SET_RELBIT, REL_X);
    ui_bit(UI_SET_RELBIT, REL_Y);
    ui_bit(UI_SET_RELBIT, REL_WHEEL);
    ui_bit(UI_SET_RELBIT, REL_HWHEEL);

    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    snprintf(us.name, sizeof(us.name), "%s", VNAME);
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0x1;
    us.id.product = 0x1;
    us.id.version = 1;
    if (ioctl(ufd, UI_DEV_SETUP, &us) < 0) { perror("UI_DEV_SETUP"); exit(1); }
    if (ioctl(ufd, UI_DEV_CREATE) < 0)     { perror("UI_DEV_CREATE"); exit(1); }
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

int main(int argc, char **argv)
{
    const char *conf = "/etc/inputd.conf";
    int opt_debug = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) opt_debug = 1;
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) conf = argv[++i];
        else { fprintf(stderr, "usage: %s [-d] [-c conf]\n", argv[0]); return 2; }
    }

    init_default_codes();
    load_conf(conf);
    if (opt_debug) debug = 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) { perror("epoll_create1"); return 1; }

    uinput_create();
    scan_devices(ep);
    if (ndev == 0) {
        fprintf(stderr, "inputd: no gamepad / volume / power devices found\n");
        return 1;
    }

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec its = {
        .it_interval = { 0, TICK_MS * 1000000L },
        .it_value    = { 0, TICK_MS * 1000000L },
    };
    timerfd_settime(tfd, 0, &its, NULL);
    struct epoll_event tev = { .events = EPOLLIN, .data.u32 = 0xFFFFFFFFu };
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &tev);

    fprintf(stderr, "inputd: running\n");

    struct epoll_event evs[8];
    while (running) {
        int n = epoll_wait(ep, evs, 8, -1);
        if (n < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < n; i++) {
            if (evs[i].data.u32 == 0xFFFFFFFFu) {
                uint64_t exp;
                if (read(tfd, &exp, sizeof(exp)) < 0) { /* EAGAIN */ }
                tick();
                continue;
            }
            int di = (int)evs[i].data.u32;
            struct input_event ie;
            while (read(devs[di].fd, &ie, sizeof(ie)) == (ssize_t)sizeof(ie))
                handle_event(devs[di].name, &ie);
        }
    }

    fprintf(stderr, "inputd: shutting down\n");
    if (lowpower) set_display(0);
    ioctl(ufd, UI_DEV_DESTROY);
    return 0;
}
