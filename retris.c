#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h> // usleep

#include <time.h>
#include <sys/time.h>
static uint64_t get_time_usec(void) {
	struct timeval time;
	gettimeofday(&time, NULL);
	return time.tv_sec * 1000000LL + time.tv_usec;
}

#include "mstream.h"

#define MSTREAM_BLK 0x1000

#define REPLAY_STR "RETRIS"
#define REPLAY_STR_N 6
#define REPLAY_VER 0

#ifndef REPLAYS_DIR
#define REPLAYS_DIR "replays"
#endif
#define REPLAY_FMT "%s/%u.rec"
#define REPLAY_FMT_N 1000
#define REPLAY_PATH_MAX 260

#define REFRESH_MIN 5
#define REFRESH_MAX 100

#ifndef USE_GAMEPAD
#define USE_GAMEPAD 1
#endif

#ifndef USE_CURTAIN
#define USE_CURTAIN 0
#endif

#if 0
#define DBG_LOG(...) fprintf(stderr, __VA_ARGS__)
#else
#define DBG_LOG(...) (void)0
#endif

typedef struct {
	void *user;
	char *field, *next_pix;
	mstream_t *rec_stream, *rep_stream;
	const char *record_fn, *replay_dir;
	unsigned end_rec, end_rep;
	uint32_t seed, next;
	int next_frame;
	unsigned lines, frame_time, prev_lines, frame_count;
	unsigned char w, h, x, y, r;
	unsigned char state, changed, piece, ncomp, comp[4];
	unsigned char refresh, cur_refresh;
	unsigned char replay, record, replays_num;
} game_t;

enum {
	EVENT_END = 0,

	EVENT_ROTL, EVENT_ROTR,
	EVENT_DOWN, EVENT_LEFT, EVENT_RIGHT,
	EVENT_DROP,

	EVENT_PAUSE, EVENT_RESTART, EVENT_QUIT,
	EVENT_REWIND,

	EVENT_REP = EVENT_PAUSE,
	EVENT_REP_END = 255
};

static const char game_pieces[7][8] = {
#define Y(c, x) c == ' ' ? 0 : x + 3,
#define X(s, x) { \
	Y(s[0],x) Y(s[1],x) Y(s[2],x) Y(s[3],x) \
	Y(s[4],x) Y(s[5],x) Y(s[6],x) Y(s[7],x) },
	X("### "
	  "  # ", 0)
	X("### "
	  " #  ", 1)
	X("### "
	  "#   ", 2)
	X("####"
	  "    ", 3)
	X("##  "
	  " ## ", 4)
	X(" ## "
	  "##  ", 5)
	X(" ## "
	  " ## ", 6)
#undef X
#undef Y
};

#define GAME_COLORS 10

#if USE_CURTAIN
#include "curtain.h"
#endif
#include "termgfx.h"
#if USE_GAMEPAD
#include "joyinput.h"
#endif

static int game_op(game_t *T, int v, int x1, int y1, int r1) {
	static const short tab[] = {
		06201, /* J */ 05201, /* T */ 04201, /* L */
		03201, /* I */ 06501, /* Z */ 04521, /* S */
		06521, /* O */ };
	int k = T->piece, r = (T->r + r1) & 3 >> k / 3, j = tab[k], w = T->w;
	char *p = T->field + (y1 + T->y) * (w + 1) + x1 + T->x;
	if (v > 1) v += k;
	do {
		int t = j & 7, x = (t & 3) - 1, y = t >> 2;
		if ((r + 1) & 2) x = -x;
		if (r & 2) y = -y;
		if (r & 1) t = x, x = y, y = t;
		x += y * (w + 1);
		if (v != 1) p[x] = v;
		else if (p[x]) return 0;
	} while (j >>= 3);
	return 1;
}

static int game_move(game_t *T, int x, int y, int r) {
	int ret;
	game_op(T, 0, 0, 0, 0);
	if ((ret = game_op(T, 1, x, y, r)))
		T->x += x, T->y += y, T->r += r, T->changed = 1;
	game_op(T, 3, 0, 0, 0);
	return ret;
}

static void game_record(game_t *T, int key) {
	if (key == EVENT_END && ++T->end_rec < EVENT_REP_END - EVENT_REP + 1) return;
	if (T->end_rec) {
		mstream_putc(T->rec_stream, T->end_rec - 1 + EVENT_REP);
		T->end_rec = 0;
		if (key == EVENT_END) return;
	}
	if (key >= 0) mstream_putc(T->rec_stream, key);
}

static void game_swap_streams(game_t *T) {
	mstream_t *m = T->rep_stream;
	T->rep_stream = T->rec_stream;
	T->rec_stream = m;
}

static void game_rec_stop(game_t *T) {
	DBG_LOG("game_rec_stop\n");
	if (!T->record) return;
	T->record = 0;
	game_record(T, -1);
	if (T->replay) { T->prev_lines = 0; return; }
	if (T->prev_lines < T->lines) {
		T->prev_lines = T->lines;
		if (T->record_fn)
			mstream_save(T->rec_stream, T->record_fn);
	}
}

static void game_rep_close(game_t *T, int end) {
	DBG_LOG("game_rep_close\n");
	if (end) {
		T->ncomp = T->h; T->state = 2; T->frame_time = 500;
		game_rec_stop(T);
	}
	T->replay = 0;
}

static int game_replay(game_t *T) {
	int ev;
	if (T->state) return EVENT_END;
	if (T->end_rep) { T->end_rep--; return EVENT_END; }
	ev = mstream_getc(T->rep_stream);
	if (ev >= EVENT_REP && ev <= EVENT_REP_END) {
		T->end_rep = ev - EVENT_REP;
		DBG_LOG("rep = %d\n", T->end_rep);
		return EVENT_END;
	}
	DBG_LOG("ev = %d\n", ev);
	if (ev > EVENT_END && ev <= EVENT_DROP) return ev;
	game_rep_close(T, 1);
	return EVENT_END;
}

static int game_rand(game_t *T, unsigned n) {
	uint64_t a = T->seed = (T->seed * 0x08088405) + 1;
	return a * n >> 32;
}

static void game_randpiece(game_t *T) {
	T->next = T->next >> 3 | game_rand(T, 7) << 27;
}

static void game_next(game_t *T) {
	T->piece = T->next & 7;
	T->x = T->w / 2 - 1; T->y = 0; T->r = 0;
	// check game over
	if (!game_op(T, 1, 0, 0, 0)) {
		game_rec_stop(T);
		T->ncomp = T->h; T->state = 2; T->frame_time = 500;
		return;
	}
	game_op(T, 3, 0, 0, 0); // paint new piece
	game_randpiece(T);
	T->changed = 1;
	T->state = 0;
}

static void game_free(game_t *T) {
	game_rec_stop(T);
	mstream_free(T->rec_stream);
	mstream_free(T->rep_stream);
	free(T);
}

static game_t* game_create(unsigned w, unsigned h) {
	game_t *T; char *p;
	unsigned st = w + 1, n = (h + 2) * st;
	T = malloc(sizeof(game_t) + n);
	if (!T) return T;
	memset(T, 0, sizeof(game_t));
	T->w = w; T->h = h;
	p = (char*)(T + 1);
	T->field = p + st;
	memset(p, 1, n);
	do {
		if (!(T->rec_stream = mstream_new(MSTREAM_BLK))) break;
		if (!(T->rep_stream = mstream_new(MSTREAM_BLK))) break;
		return T;
	} while (0);
	game_free(T);
	return NULL;
}

static void game_rec_init(game_t *T) {
	mstream_t *m = T->rec_stream;
	mstream_rewind(m, 1);
	mstream_write(m, REPLAY_STR, REPLAY_STR_N);
	mstream_putc(m, REPLAY_VER);
	mstream_putc(m, T->w); mstream_putc(m, T->h);
	mstream_putc(m, T->cur_refresh);
	mstream_putc(m, T->seed);
	mstream_putc(m, T->seed >> 8);
	mstream_putc(m, T->seed >> 16);
	mstream_putc(m, T->seed >> 24);
	T->end_rec = 0;
	T->record = 1;
}

static void game_rep_init(game_t *T) {
	uint8_t buf[REPLAY_STR_N + 8], *p;
	unsigned refresh;
	mstream_t *m = T->rep_stream;
	mstream_rewind(m, 0);
	do {
		if (mstream_read(m, buf, sizeof(buf)) != sizeof(buf)) break;
		if (memcmp(buf, REPLAY_STR, REPLAY_STR_N)) break;
		p = buf + REPLAY_STR_N;
		if (p[0] != REPLAY_VER) break;
		if (p[1] != T->w || p[2] != T->h) break;
		if ((refresh = p[3]) - REFRESH_MIN > REFRESH_MAX - REFRESH_MIN) break;
		T->seed = p[4] | p[5] << 8 | p[6] << 16 | p[7] << 24;
		T->cur_refresh = refresh;
		T->end_rep = 0;
		T->replay = 1;
		return;
	} while (0);
	DBG_LOG("replay check: failed\n");
	T->replay = 0;
}

static void game_restart(game_t *T) {
	unsigned i, w = T->w;
	for (i = 0; i < T->h; i++)
		memset(T->field + i * (w + 1), 0, w);
	T->ncomp = T->lines = 0;
	T->changed = T->state = 1;
	T->cur_refresh = T->refresh;
	T->seed = get_time_usec();
	if (T->replay) game_rep_init(T);
	game_rec_init(T);
	for (i = 0; i < 10; i++) game_randpiece(T);
	T->frame_time = 800;
	T->next_frame = T->frame_count = 0;
}

static void game_comp(game_t *T) {
	unsigned x, y, k = 0, w = T->w;
	char *p = T->field;
	for (y = 0; y < T->h; y++) {
		for (x = 0; x < w; x++)
			if (!p[y * (w + 1) + x]) break;
		if (x == w)
			T->comp[k++] = y,
			memset(p + y * (w + 1), 0, w);
	}
	if (!k) return;
	T->ncomp = k; T->changed = 1;
	T->lines = x = (y = T->lines) + k;
	if (x / 10 > y / 10)
		T->frame_time = (T->frame_time * 9 + 5) / 10;
	for (x = 0; --k; x++) T->comp[x] += k;
}

static int game_anim(game_t *T) {
	int w = T->w; char *p = T->field;
	if (T->state & 2) {
		if (T->ncomp) {
			memset(p + --T->ncomp * (w + 1), T->state & 1 ? 0 : 2, w);
			T->changed = 1;
		} else if (T->state == 2) {
			if (T->prev_lines || T->replays_num) {
				T->ncomp = T->h; T->state = 3;
			}
		} else if (T->state == 3) {
			DBG_LOG("prev_lines = %u, replays_num = %u\n",
					T->prev_lines, T->replays_num);
			if (T->prev_lines) game_swap_streams(T);
			else {
				char buf[REPLAY_PATH_MAX];
				snprintf(buf, sizeof(buf), REPLAY_FMT,
						T->replay_dir, game_rand(T, T->replays_num));
				DBG_LOG("replay_fn = \"%s\"\n", buf);
				mstream_load(T->rep_stream, buf);
			}
			T->replay = 1;
			game_restart(T);
			return 1;
		}
	} else if (T->ncomp) {
		memmove(p + w + 1, p, T->comp[--T->ncomp] * (w + 1));
		memset(p, 0, w);
		T->changed = 1;
	} else game_next(T);
	return 0;
}

static void game_event(game_t *T, int ev) {
	int ret = 0;
	//DBG_LOG("event: ev = %d, state = %d\n", ev, T->state);
	if (T->state && ev != EVENT_END) return;
	switch (ev) {
	case EVENT_ROTR: ret = game_move(T, 0, 0, -1); break;
	case EVENT_ROTL: ret = game_move(T, 0, 0, 1); break;
	case EVENT_LEFT: ret = game_move(T, -1, 0, 0); break;
	case EVENT_RIGHT: ret = game_move(T, 1, 0, 0); break;
	case EVENT_END:
		if (!T->state) game_record(T, ev);
loop:
		if (T->next_frame > 0) {
			T->next_frame -= T->cur_refresh;
			T->frame_count++;
			return;
		}
		T->next_frame += T->frame_time;
		if (T->state) {
			if (game_anim(T)) return;
			goto loop;
		}
	/* fallthrough */
	case EVENT_DOWN:
	case EVENT_DROP:
		do ret = game_move(T, 0, 1, 0);
		while (ev == EVENT_DROP && ret);
		if (!ret) {
			game_comp(T);
			if (ev != EVENT_END)
				T->next_frame = T->frame_time;
			T->state = 1;
		}
		if (ev == EVENT_END) goto loop;
		goto record;
	}
	if (ret) {
record:
		game_record(T, ev);
	}
}

static void game_rewind(game_t *T, int rewind_sec) {
	unsigned target = T->frame_count;
	unsigned x = rewind_sec * 1000 / T->cur_refresh;
	target = target < x ? 0 : target - x;
	game_record(T, -1);
	if (!T->replay) T->replay = 1, game_swap_streams(T);
	game_restart(T);
	do game_event(T, game_replay(T));
	while (T->frame_count < target);
}

int main(int argc, char **argv) {
	sysctx_t sysctx = { 0 }, *sys = &sysctx;
	uint64_t last_timer;
	unsigned char refresh = 20; // 50 FPS
	unsigned char rewind_sec = 10;
	game_t *T;
	const char *record_fn = NULL, *replay_fn = NULL;
	const char *replays_dir = REPLAYS_DIR;
	char testdemo = 0; int seek_end = 0;
#if USE_GAMEPAD
	jsctx_t jsctx, *js = &jsctx;
	const char *js_fn = NULL;
	unsigned joy_timer = 0;
	js->thr[0] = 0x3fff + 0x1000;
	js->thr[1] = 0x3fff - 0x1000;
	js->repeat_ms = 100;
#endif

	sys->colormode = 1;
	sys->next_num = 1;
	sys->next_rev = 0;

	while (argc > 1) {
		if (argc > 2 && !strcmp(argv[1], "--refresh")) {
			int x = atoi(argv[2]);
			if (x < REFRESH_MIN) x = REFRESH_MIN;
			if (x > REFRESH_MAX) x = REFRESH_MAX;
			refresh = x;
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--next")) {
			int x = atoi(argv[2]);
			if (x < 0) x = -x, sys->next_rev = 1;
			sys->next_num = x < 0 ? 0 : x > 3 ? 3 : x;
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--record")) {
			record_fn = argv[2];
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--play")) {
			replay_fn = argv[2];
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--seekend")) {
			int x = atoi(argv[2]);
			seek_end = x < 0 ? 0 : x > 3600 ? 3600 : x;
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--dir")) {
			if (strlen(argv[2]) < REPLAY_PATH_MAX - 16)
				replays_dir = argv[2];
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--rewind")) {
			int x = atoi(argv[2]);
			rewind_sec = x < 1 ? 1 : x > 120 ? 120 : x;
			argc -= 2; argv += 2;
		} else if (!strcmp(argv[1], "--testdemo")) {
			testdemo = 1;
			argc -= 1; argv += 1;
		} else if (!strcmp(argv[1], "--notermgfx")) {
			sys->notermgfx = 1;
			argc -= 1; argv += 1;
		} else if (argc > 2 && !strcmp(argv[1], "--color")) {
			if (!strcmp(argv[2], "rgb")) sys->colormode = 0;
			else if (!strcmp(argv[2], "simple")) sys->colormode = 1; 
			else if (!strcmp(argv[2], "no")) sys->colormode = 2;
			argc -= 2; argv += 2;
#if USE_GAMEPAD
		} else if (argc > 2 && !strcmp(argv[1], "--js")) {
			js_fn = argv[2];
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--js_thr")) {
			char *s = argv[2];
			js->thr[0] = js->thr[1] = strtol(s, &s, 0);
			if (*s == ',') js->thr[1] = strtol(s + 1, NULL, 0);
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--js_rep")) {
			js->repeat_ms = atoi(argv[2]);
			argc -= 2; argv += 2;
#endif
		} else {
#if USE_CURTAIN
			int n = curtain_args(argc, argv);
			if (n) { argc -= n; argv += n; continue; }
#endif
			fprintf(stderr, "unknown option: \"%s\"\n", argv[1]);
			return 1;
		}
	}
	if (!replay_fn) testdemo = 0;

	T = game_create(10, 20);
	if (!T) return 1;
	sys->notermgfx |= testdemo;
	sys_init(sys);
	T->user = sys;
	T->replay_dir = replays_dir;
	T->record_fn = record_fn;
	T->refresh = refresh;

	if (replay_fn) {
		mstream_load(T->rep_stream, replay_fn);
		game_rep_init(T);
		if (!T->replay) {
			fprintf(stderr, "bad replay file\n");
			goto end;
		}
	} else {
		char buf[REPLAY_PATH_MAX]; int i; FILE *f;
		for (i = 0; i < REPLAY_FMT_N; i++) {
			snprintf(buf, sizeof(buf), REPLAY_FMT, T->replay_dir, i);
			if (!(f = fopen(buf, "rb"))) break;
			fclose(f);
		}
		T->replays_num = i;
	}

#if USE_CURTAIN
	curtain_init();
#endif
#if USE_GAMEPAD
	joy_init(js, js_fn);
#endif
	game_restart(T);

	if (testdemo) {
		uint32_t seed = T->seed;
		do game_event(T, game_replay(T));
		while (!(T->state & 2));
		if (T->record_fn)
			mstream_save(T->rec_stream, T->record_fn);
		{
			unsigned time = T->frame_count * T->cur_refresh;
			unsigned sec = time / 1000;
			printf("seed = 0x%08x, lines = %u, time = %u:%02u.%03u (%u ms)\n",
					seed, T->lines, sec / 60, sec % 60, time % 1000, time);
		}
		goto end;
	}

	if (T->replay && seek_end) {
		do game_event(T, game_replay(T));
		while (!(T->state & 2));
		game_rewind(T, seek_end);
	}

	last_timer = get_time_usec();
	for (;;) {
#if USE_GAMEPAD
		int ev_joy = 0;
#endif
		int ev;
		do {
#if USE_GAMEPAD
			if (!ev_joy) {
				ev = sys_events(sys);
				if (ev == EVENT_END) ev_joy = 1;
			}
			if (ev_joy) {
				if (js->fd < 0) {
					if ((joy_timer += T->cur_refresh) >= 1000)
						joy_timer = 0, joy_init(js, js_fn);
				}
				ev = EVENT_END;
				if (js->fd >= 0) {
					ev = joy_events(js);
					if (js->fd < 0) T->state |= 4;
				}
			}
#else
			ev = sys_events(sys);
#endif
			if (ev >= EVENT_PAUSE) {
				if (ev == EVENT_PAUSE) { T->state ^= 4; continue; }
				if (ev == EVENT_REWIND) {
					game_rewind(T, rewind_sec);
					continue;
				}
				if (ev == EVENT_RESTART) {
					T->replay = 0;
					game_rec_stop(T);
					T->prev_lines = 0;
					game_restart(T); continue;
				}
				if (ev == EVENT_QUIT) goto end;
			}
			if (T->state & 4) continue;
			if (T->replay) {
				if (ev == EVENT_END) {
					do game_event(T, ev = game_replay(T));
					while (ev != EVENT_END);
					break;
				} else game_rep_close(T, 0);
			}
			game_event(T, ev);
		} while (ev != EVENT_END);

		if (T->changed) T->changed = 0, sys_refresh(sys, T);
		{
			int64_t delay, t1 = get_time_usec();
			last_timer += T->cur_refresh * 1000;
			delay = last_timer - t1;
			if (delay > 0) usleep(delay);
			else last_timer = t1;
		}
	}
end:
	sys_close(sys);
	game_free(T);
#if USE_GAMEPAD
	if (js_fn) joy_close(js);
#endif
#if USE_CURTAIN
	curtain_end();
#endif
	return 0;
}
