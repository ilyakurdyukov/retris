#define _XOPEN_SOURCE 600
#include "btgadget.h"

typedef struct {
	btio_t *io;
	int handle[2];
	int send_id, recv_id;
} zengge_t;

typedef struct {
	btio_t io;
	zengge_t zengge;
} curtain_t;

static void zengge_init(zengge_t *ctx) {
	btio_t *io = ctx->io;
	static const int uuid[] = { 0xff01, 0xff02 };
	int ret, start, end;

	ctx->send_id = 0; ctx->recv_id = -1;

	bt_filter_notify = BT_FILTER_NOTIFY_ALL;
	start = bt_get_type_range(io, 0xffff, &end);
	ret = bt_find_char(io, start, end, 2, uuid, ctx->handle);
	if (ret != 2) ERR_EXIT("can't find char handle\n");
	if (io->verbose >= 1)
		DBG_LOG("write = 0x%x, read = 0x%x\n",
				ctx->handle[0], ctx->handle[1]);
	bt_write_req_desc(io, ctx->handle[1], end);
	bt_filter_notify = ctx->handle[1];
	bt_exchange_mtu(io, 210);
}

static void zengge_cmd(zengge_t *ctx, const uint8_t *src, unsigned len) {
	btio_t *io = ctx->io;
	unsigned chunk = io->mtu, i, n, k;
	unsigned msg_id = ++ctx->send_id;
	int cmd = 0x52; // Write Command
	if (src[0] == 0x0b && src[1] == 0x10)
		cmd = 0x12; // Write Request
	WRITE16_BE(io->buf + 7, len - 1);
	for (i = 0, k = 10; len; k = 8) {
		n = len; if (n > chunk - k) n = chunk - k;
		if (n > 255) n = 255;
		io->buf[k - 1] = n;
		memcpy(io->buf + k, src, n); src += n; len -= n;
		io->buf[0] = cmd;
		WRITE16_LE(io->buf + 1, ctx->handle[0]);
		// 0x04 - from device, 0x40 - multipart
		io->buf[3] = len | i ? 0x40 : 0;
		io->buf[4] = msg_id;
		io->buf[5] = len ? 0 : 0x80; // 0x80 - last part
		io->buf[6] = i++; // part id
		bt_send(io, NULL, k + n);
		if (cmd == 0x12 && (bt_recv(io) != 1 || io->buf[0] != 0x13))
			ERR_EXIT("unexpected response\n");
	}
}

#include "curtain.h"

static curtain_t curtain;
static const char *curtain_mac = NULL;
static unsigned curtain_retry_ms = 0, curtain_refresh_ms = 67;
static int curtain_verbose = 0;
static uint32_t curtain_time;

#define GAME_COLORS 10
extern int game_colors[GAME_COLORS];
static uint16_t curtain_colors[GAME_COLORS];

static uint32_t curtain_gettime(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int curtain_args(int argc, char **argv) {
	int old_argc = argc;
	while (argc > 1) {
		if (argc > 2 && !strcmp(argv[1], "--curtain_mac")) {
			curtain_mac = argv[2];
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--curtain_retry")) {
			curtain_retry_ms = atoi(argv[2]);
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--curtain_refresh")) {
			curtain_refresh_ms = atoi(argv[2]);
			argc -= 2; argv += 2;
		} else if (argc > 2 && !strcmp(argv[1], "--curtain_verbose")) {
			curtain_verbose = atoi(argv[2]);
			argc -= 2; argv += 2;
		} else break;
	}
	return old_argc - argc;
}

static int zengge_rgb2hsv16(unsigned rgb) {
	unsigned r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
	unsigned m0 = r, m2 = b, d, h = 0, s = 0, v;

	if (r > b) m0 = b, m2 = r;
	if (g < m0) m0 = g;
	if (g > m2) m2 = g;

	v = m2 >> 3;
	if ((d = m2 - m0)) {
		s = (d * 15 + m2 / 2) / m2;

		if (r == m2) h = 6 * d + g - b;
		else if (g == m2) h = 2 * d + b - r;
		else h = 4 * d + r - g;

		h = (h * 20 + d / 2) / d;
		if (h >= 120) h -= 120;
	}
	return h << 9 | s << 5 | v;
}

void curtain_init(void) {
	const char *src_str = "00:00:00:00:00:00"; // BDADDR_ANY
	const char *dst_str = curtain_mac;
	bdaddr_t sba, dba;
	int stype = BDADDR_LE_PUBLIC;
	int dtype = BDADDR_LE_PUBLIC;
	btio_t *io = &curtain.io;
	int ret;

	if (!dst_str) return;

	if (str2bdaddr(src_str, &sba))
		ERR_EXIT("malformed src addr");

	if (str2bdaddr(curtain_mac, &dba))
		ERR_EXIT("curtain init failed: %s\n", "malformed dst addr");

	curtain.zengge.io = io;
	io->timeout = 1000;
	io->verbose = curtain_verbose;
	io->mtu = -1;
	io->type = 0;
#define X(name) { \
		fprintf(stderr, "curtain init failed: %s = %d (%s)\n", \
				#name, errno, strerror(errno)); \
		goto retry; }
	for (;;) {
		io->sock = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
		if (io->sock < 0) X(socket)
		ret = l2cap_bind(io->sock, &sba, stype, 0, ATT_CID);
		if (ret) X(bind)
		ret = l2cap_connect(io->sock, &dba, dtype, 0, ATT_CID);
		if (ret) X(connect)
		zengge_init(&curtain.zengge);
		break;
retry:
		if (!curtain_retry_ms) exit(1);
		usleep(curtain_retry_ms * 1000);
	}
#undef X
	curtain_time = curtain_gettime();
	{
		int i;
		for (i = 0; i < GAME_COLORS; i++)
			curtain_colors[i] = zengge_rgb2hsv16(game_colors[i]);
	}
}

void curtain_end(void) {
	if (!curtain_mac) return;
	close(curtain.io.sock);
	curtain_mac = NULL;
}

int curtain_update(char field[20][20]) {
	uint8_t cmd[3 + 400 * 2];
	if (!curtain_mac) return 0;
	if (curtain.io.timeout < 0) {
		close(curtain.io.sock);
		curtain_init();
	}
	{
		uint32_t new_time = curtain_gettime();
		unsigned target = curtain_refresh_ms;
		unsigned diff = new_time - curtain_time;
		if (diff < target) return 0;
		if (diff > target * 2) curtain_time = new_time;
		else curtain_time += target;
	}

	cmd[0] = 0x0a; cmd[1] = 0xe2; cmd[2] = 0x0e;
	{
		unsigned x, y;
		for (y = 0; y < 20; y++)
		for (x = 0; x < 20; x++) {
			int val = field[y][x];
			val = curtain_colors[val];
			WRITE16_BE(cmd + 3 + (x * 20 + y) * 2, val);
		}
	}
	zengge_cmd(&curtain.zengge, cmd, sizeof(cmd));
	return curtain.io.timeout < 0;
}

