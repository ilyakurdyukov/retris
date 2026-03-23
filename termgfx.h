#include <unistd.h>
#include <termios.h>
#include <signal.h>

//#define COLOR_STR_MAX 20
#define COLOR_STR_MAX sizeof("\33[48;2;255;255;255m)")

int game_colors[GAME_COLORS] = {
	0x000000, // BG: black
	0x7f7f7f, // border: grey
	0x7f7f7f, // gameover: grey
	0xff8000, // J: green / 2 + red
	0x00ff00, // T: green
	0x80ff00, // L: green + red / 2
	0x0000ff, // I: blue
	0xff00ff, // Z: magenta
	0x00ffff, // S: cyan
	0xff0000, // O: red
};

static const char* const colormode[GAME_COLORS] = {
	"", // BG: default
	"", // border: default
	"", // gameover: default
	"43", // J: yellow
	"42", // T: green
	"0;102", // L: light green
	"44", // I: blue
	"45", // Z: magenta
	"46", // S: cyan
	"41", // O: red
};

typedef struct {
	struct termios tcattr;
	unsigned char readbuf[8];
	int bufidx, nread;
	char controls[128], controls2[128];

	const char *term_colors[GAME_COLORS];
	unsigned old_lines;
	char field[20][20], field1[20][20];
	char notermgfx, colormode;
	char next_num, next_rev;
	char str_buf[GAME_COLORS * COLOR_STR_MAX];
	char ascii_screen[20 * 20 * 2];
} sysctx_t;

static sysctx_t *sys_glob;

static void sys_close(sysctx_t *sys) {
	tcsetattr(0, TCSANOW, &sys->tcattr);
	if (!sys->notermgfx)
		printf("\33[m\33[2J\33[?25h\33[H"); // show cursor
}

static void sys_sighandler(int signo) {
	(void)signo;
	sys_close(sys_glob);
	exit(1);
}

static void sys_init(sysctx_t *sys) {
	struct termios tcattr_new;

	tcgetattr(0, &sys->tcattr);
	tcattr_new = sys->tcattr;
	tcattr_new.c_lflag &= ~(ICANON | ECHO);
	tcattr_new.c_cc[VMIN] = 0;
	tcattr_new.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &tcattr_new);

	memset(sys->controls, EVENT_END, sizeof(sys->controls));
	memset(sys->controls2, EVENT_END, sizeof(sys->controls2));

#define X(k, v) sys->controls2[k] = v;
	X(0x41, EVENT_ROTR)
	X(0x42, EVENT_DOWN)
	X(0x43, EVENT_RIGHT)
	X(0x44, EVENT_LEFT)
#undef X
#define X(k, v) sys->controls[k] = v;
	X(0x1b, EVENT_QUIT) // escape
	X(0x0a, EVENT_DROP) // enter
	X(0x20, EVENT_ROTL) // space
#undef X
#define X(k, v) \
	sys->controls[k & ~32] = sys->controls[k | 32] = v;
	X('w', EVENT_ROTR)
	X('a', EVENT_LEFT)
	X('s', EVENT_DOWN)
	X('d', EVENT_RIGHT)
	X('p', EVENT_PAUSE)
	X('r', EVENT_RESTART)
	X(0x7f, EVENT_REWIND) // backspace
#undef X

	sys->bufidx = sys->nread = 0;

	if (!sys->notermgfx && sys->colormode) {
		char *p = sys->ascii_screen;
		unsigned i;
		memset(p, ' ', sizeof(sys->ascii_screen));
#define X(y, x) p + ((y) * 20 + (x)) * 2
#define SET(y, x, s) memcpy(X(y, x), s, sizeof(s) - 1)
		for (i = 0; i < 19; i++)
			SET(i, 0, " |[][][][][][][][][][]| ");
		SET(i, 0, " \\--------------------/ ");

		for (i = 0; i < 2; i++)
			SET(17 + i, 14, "* * * * * ");

		// level digits
		for (i = 0; i < 5; i++)
#if 1
			SET(11 + i, 13, "()()()()()()()");
#else
			SET(11 + i, 13, "##############");
#endif

		// next
		if (sys->next_num) {
			unsigned n = sys->next_num;
			unsigned nx = 13, ny = n < 3 ? 1 : 0;
			if (sys->next_rev) ny += (n - 1) * 3;
			for (i = 0; i < 8; i++)
				SET(1 + i, nx, " |[][][][]| ");
			SET(ny, nx, " /--------\\ ");
			n = n == 1 ? 4 : 2;
			SET(ny + n + 1, nx, " \\--------/ ");
		}

		if (0) { // debug
			for (i = 0; i < 20; i++)
				printf("%.*s\n", 20 * 2, X(i, 0));
			tcsetattr(0, TCSANOW, &sys->tcattr);
			exit(1);
		}
#undef SET
#undef X
	}

	if (!sys->notermgfx) {
		printf("\33[2J\33[?25l"); // clear screen, hide cursor
		sys_glob = sys;
		signal(SIGINT, sys_sighandler);
		{
			int i, j;
			char *s = sys->str_buf;
			for (i = 0; i < GAME_COLORS; i++) {
				if (sys->colormode == 2) *s = 0;
				else if (sys->colormode)
					snprintf(s, COLOR_STR_MAX, "\33[%sm", colormode[i]);
				else {
					int c = game_colors[i];
					// text color: "\33[38;2;%u;%u;%um"
					snprintf(s, COLOR_STR_MAX, "\33[48;2;%u;%u;%um",
							c >> 16 & 255, c >> 8 & 255, c & 255);
				}
				sys->term_colors[i] = s;
				for (j = 0; j < i; j++)
					if (!strcmp(sys->term_colors[i], sys->term_colors[j])) {
						sys->term_colors[i] = sys->term_colors[j];
						break;
					}
				if (j == i) s += strlen(s) + 1;
			}
		}
	}

	memset(sys->field1, -1, sizeof(sys->field));
	{
		char (*p)[20] = sys->field;
		unsigned i, n = sys->next_num;

		memset(p, 0, sizeof(sys->field));
		for (i = 0; i < 19; i++) p[i][0] = p[i][11] = 1;
		memset(p[i], 1, 12);

		if (n) {
			unsigned nx = 13, ny = n < 3 ? 1 : 0;
			if (sys->next_rev) ny += (n - 1) * 3;
			n = n == 1 ? 4 : 2;
			memset(p[ny] + nx, 1, 6);
			for (i = 1; i < 1 + n; i++)
					p[ny + i][nx] = p[ny + i][nx + 5] = 1;
			memset(p[ny + i] + nx, 1, 6);
		}
	}
	sys->old_lines = ~0;
}

static void draw_digit(char *p, int num) {
	const char *font =
		".@. .@. @@@ @@@ @.@ @@@ .@@ @@@ @@@ @@@"
		"@.@ @@. ..@ ..@ @.@ @.. @.. ..@ @.@ @.@"
		"@.@ .@. @@@ @@@ @@@ @@@ @@@ .@. @@@ @@@"
		"@.@ .@. @.. ..@ ..@ ..@ @.@ .@. @.@ ..@"
		".@. @@@ @@@ @@@ ..@ @@@ @@@ .@. @@@ @@." + num * 4;
	int x, y;
	for (x = 0; x < 3; x++)
	for (y = 0; y < 5; y++)
		p[y * 20 + x] = font[y * 39 + x] == '@' ? 1 : 0;
}

static void sys_update_score(sysctx_t *sys, game_t *T) {
	unsigned i, j, k, x1 = 13, y1 = 11;
	char (*p)[20] = sys->field;

	for (i = 0; i < 5; i++)
		memset(p[y1 + i] + x1, 0, 7);

	k = T->lines; j = k % 10; k /= 10;
	for (i = 0; i < 5; i++)
		p[17][14 + i] = j > i ? 1 : 0;
	for (i = 0; i < 5; i++)
		p[18][14 + i] = j > i + 5 ? 1 : 0;

	if (k < 10) draw_digit(p[y1] + x1 + 2, k);
	else {
		if (k > 99) k = 99;
		draw_digit(p[y1] + x1, k / 10);
		draw_digit(p[y1] + x1 + 4, k % 10);
	}
}

static void sys_refresh(sysctx_t *sys, game_t *T) {
	unsigned x, y, w = 10, h = 19;
	char *s = T->field + (w + 1);
	char (*p)[20] = sys->field;
	const char *color1 = "";
	// copy playfield
	for (y = 0; y < h; y++)
		memcpy(p[y] + 1, s + y * (w + 1), w);
	// copy next
	{
		unsigned n = sys->next_num;
		unsigned nx = 13 + 1, ny = 4 - n;
		for (x = 0; x < n; x++, ny += 3) {
			unsigned a = x;
			if (sys->next_rev) a = n - 1 - a;
			a = (T->next >> a * 3) & 7;
			for (y = 0; y < 2; y++)
				memcpy(p[ny + y] + nx,
						game_pieces[a] + y * 4, 4);
		}
	}

	if (sys->old_lines != T->lines) {
		sys->old_lines = T->lines;
		sys_update_score(sys, T);
	}

#if USE_CURTAIN
	if (curtain_update(sys->field)) T->state |= 4;
#endif

	if (sys->notermgfx) return;

	for (y = 0; y < 20; y++) {
		int new_pos = 1;
		// printf("\33[%uH", y + 1);
		for (x = 0; x < 20; x++) {
			int t = p[y][x]; const char *color;
			if (t == sys->field1[y][x]) { new_pos = 1; continue; }
			if (new_pos) new_pos = 0, printf("\33[%u;%uH", y + 1, x * 2 + 1);
			color = sys->term_colors[t];
			if (color1 != color) color1 = color, fputs(color, stdout);
			{
				const char *s;
				if (!sys->colormode) s = t > 1 ? "[]" : "  ";
				else s = t ? sys->ascii_screen + (y * 20 + x) * 2 : "  ";
				fwrite(s, 1, 2, stdout);
			}
		}
	}
	// reset color: "\33[m"
	puts("\33[H"); // refresh screen

	memcpy(sys->field1, sys->field, sizeof(sys->field));
}

static int sys_events(sysctx_t *sys) {
	int i = sys->bufidx, n = sys->nread;
	int a, status = 0, key = EVENT_END;
	do {
		if (i == n) {
			if (n != (int)sizeof(sys->readbuf) && status == 1) {
				key = sys->controls[0x1b]; // escape
				break;
			}
			i = 0;
			n = read(0, sys->readbuf, sizeof(sys->readbuf));
			if (!n) break;
		}
		a = sys->readbuf[i++];
		if (a >= 128) continue;
		if (a == 0x1b) status = 1;
		else if (a == 0x5b && status == 1) status = 2;
		else if (status == 2) key = sys->controls2[a], status = 0;
		else key = sys->controls[a];
	} while (key == EVENT_END);
	sys->bufidx = i; sys->nread = n;
	return key;
}

