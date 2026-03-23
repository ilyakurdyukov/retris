#include <stdint.h>
#include <stdlib.h> // malloc, free
#include <string.h>

#include <linux/joystick.h>
#include <sys/ioctl.h> // ioctl
#include <fcntl.h> // open
#include <unistd.h> // read, close

#define USE_POLL 0

#if USE_POLL
#include <sys/poll.h>
#else
#include <errno.h>
#endif

typedef struct {
	int count;
} js_event_t;

typedef struct {
	char ev; unsigned char st;
} js_state_t;

#define MAX_EVENTS (EVENT_QUIT + 1)

typedef struct {
	js_state_t (*ax)[2], *btn;
	int fd;
	unsigned axes, buttons, repeat_ms;
	int thr[2], i_event;
	uint32_t timers[MAX_EVENTS];
	char events[MAX_EVENTS];
} jsctx_t;

static void joy_close(jsctx_t *js) {
	if (js->fd >= 0) { close(js->fd); js->fd = -1; }
	if (js->ax) { free(js->ax); js->ax = NULL; }
}

#define JS_ERR(...) { \
	fprintf(stderr, __VA_ARGS__); \
	joy_close(js); return; }

static void joy_init(jsctx_t *js, const char *js_dev) {
	uint8_t axmap[ABS_CNT];
	uint16_t btnmap[KEY_CNT - BTN_MISC];
	char name[128 + 1];
	uint8_t buttons, axes;
	unsigned i, n;
	char an_trig = 0;

	js->ax = NULL;
	js->fd = -1;
	if (!js_dev) return;
	js->fd = open(js_dev, O_RDONLY | O_NONBLOCK);
	if (js->fd < 0) JS_ERR("open(js_dev) failed\n")
	if (ioctl(js->fd, JSIOCGAXES, &axes) < 0)
		JS_ERR("ioctl(JSIOCGAXES) failed\n");
	if (ioctl(js->fd, JSIOCGBUTTONS, &buttons) < 0)
		JS_ERR("ioctl(JSIOCGBUTTONS) failed\n");
	if (ioctl(js->fd, JSIOCGAXMAP, axmap) < 0)
		JS_ERR("ioctl(JSIOCGAXMAP) failed\n");
	if (ioctl(js->fd, JSIOCGBTNMAP, btnmap) < 0)
		JS_ERR("ioctl(JSIOCGBTNMAP) failed\n");

	memset(name, 0, sizeof(name));
	if (ioctl(js->fd, JSIOCGNAME(128), name) < 0) name[0] = 0;

	memset(js->events, 0, sizeof(js->events));

#include "joycompat.h"

	js->i_event = MAX_EVENTS;
	js->axes = axes;
	js->buttons = buttons;
	n = buttons + axes * 2;
	{
		js_state_t *p = malloc(n * sizeof(js_state_t));
		if (!p) JS_ERR("malloc failed\n");
		js->ax = (js_state_t(*)[2])p;
		js->btn = p + axes * 2;
		for (i = 0; i < n; i++) {
			p[i].ev = -1;
			p[i].st = 0;
		}
	}

	for (i = 0; i < axes; i++)
		switch (axmap[i]) {
		case ABS_X: case ABS_RX:
		case ABS_HAT0X:
			js->ax[i][0].ev = EVENT_LEFT;
			js->ax[i][1].ev = EVENT_RIGHT; break;
		case ABS_Y: case ABS_RY:
		case ABS_HAT0Y:
			js->ax[i][0].ev = EVENT_ROTR;
			js->ax[i][1].ev = EVENT_DOWN; break;
		case ABS_Z: case ABS_RZ: // LT/RT
			an_trig = 1;
			js->ax[i][1].ev = EVENT_DROP; break;
		}

	for (i = 0; i < buttons; i++)
		switch (btnmap[i]) {

#ifndef BTN_DPAD_UP
#define BTN_DPAD_UP     0x220
#define BTN_DPAD_DOWN   0x221
#define BTN_DPAD_LEFT   0x222
#define BTN_DPAD_RIGHT  0x223
#endif

#define X(x) { js->btn[i].ev = EVENT_##x; } break;
		case BTN_DPAD_UP: X(ROTR)
		case BTN_DPAD_DOWN: X(DOWN)
		case BTN_DPAD_LEFT: X(LEFT)
		case BTN_DPAD_RIGHT: X(RIGHT)

		case BTN_TL: // LB
		case BTN_A: case BTN_X: X(ROTL)

		case BTN_TR: // RB
		case BTN_B: case BTN_Y: X(ROTR)

		case BTN_TL2: // LT
		case BTN_TR2: // RT
			if (!an_trig) X(DROP)

		case BTN_START:
		case BTN_MODE: X(PAUSE)
		case BTN_SELECT: X(RESTART)
#undef X
		}
}

static inline void joy_release_event(char *ev) {
	int x = *ev;
	*ev = (x - 1) & ((x - 2) | 0x7f);
}

static int joy_events(jsctx_t *js) {
	struct js_event event;
#if USE_POLL
	struct pollfd fds = { 0 };
#endif
	if (js->fd < 0) return EVENT_END;
	if (js->i_event < MAX_EVENTS) goto check_events;
#if USE_POLL
	fds.fd = js->fd;
	fds.events = POLLIN;
	while (poll(&fds, 1, 0)) {
#else
	for (;;) {
#endif
		int n = read(js->fd, &event, sizeof(event));
		int type, val, ev = -1; unsigned num;
		if (n != (int)sizeof(event)) {
#if !USE_POLL
			if (n == -1 && errno == EAGAIN) break;
			// ENODEV on disconnect
#endif
			joy_close(js);
			break;
		}
		if (0) fprintf(stderr, "0x%08x 0x%04x 0x%02x 0x%02x\n",
				event.time, event.value & 0xffff, event.type, event.number);
		type = event.type;
		// type &= ~JS_EVENT_INIT; // wrong init value for DirectInput
		val = event.value;
		num = event.number;
		if (type == JS_EVENT_BUTTON) {
			if (num >= js->buttons) continue;
			if ((ev = js->btn[num].ev) < 0) continue;
			val = val != 0;
			if (js->btn[num].st == val) continue;
			js->btn[num].st = val;
			if (!val) joy_release_event(&js->events[ev]);
			else if (!js->events[ev]++) {
				js->timers[ev] = get_time_usec();
				if (ev < EVENT_RESTART) return ev;
				if (ev == EVENT_RESTART) return EVENT_REWIND;
			}
#define REL_AX_EVENT(i) \
	if (js->ax[num][i].st) { \
		ev = js->ax[num][i].ev; \
		if (ev >= 0) joy_release_event(&js->events[ev]); \
		js->ax[num][i].st = 0; \
	}
		} else if (type == JS_EVENT_AXIS) {
			if (num >= js->axes) continue;

			if (val >= js->thr[js->ax[num][1].st]) val = 1;
			else if (val <= -js->thr[js->ax[num][0].st]) val = 0;
			else { val = js->ax[num][1].st; REL_AX_EVENT(val) continue; }

			if (js->ax[num][val].st) continue;
			REL_AX_EVENT(val ^ 1)
			js->ax[num][val].st = 1;
			ev = js->ax[num][val].ev;
			if (ev >= 0 && !js->events[ev]++) {
				js->timers[ev] = get_time_usec();
				if (ev < EVENT_RESTART) return ev;
				if (ev == EVENT_RESTART) return EVENT_REWIND;
			}
		}
#undef REL_AX_EVENT
	}
	js->i_event = EVENT_DOWN;
check_events:
	{
		int i = js->i_event;
		uint32_t t1 = get_time_usec();
		uint32_t r = js->repeat_ms * 1000;
		for (; i <= EVENT_RIGHT; i++)
			if (js->events[i] && t1 - js->timers[i] >= r) {
				js->timers[i] += r;
				js->i_event = i;
				return i;
			}
		r = 1000 * 1000;
		for (i = EVENT_RESTART; i <= EVENT_QUIT; i++)
			if (js->events[i] > 0 && t1 - js->timers[i] >= r) {
				js->events[i] |= 0x80; // no repeat
				js->i_event = i;
				return i;
			}
		js->i_event = i;
	}
	return EVENT_END;
}

