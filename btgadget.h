#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/rfcomm.h>

#define ATT_CID 4

static void print_mem(FILE *f, const uint8_t *buf, size_t len) {
	size_t i; int a, j, n;
	for (i = 0; i < len; i += 16) {
		n = len - i;
		if (n > 16) n = 16;
		for (j = 0; j < n; j++) fprintf(f, "%02x ", buf[i + j]);
		for (; j < 16; j++) fprintf(f, "   ");
		fprintf(f, " |");
		for (j = 0; j < n; j++) {
			a = buf[i + j];
			fprintf(f, "%c", a > 0x20 && a < 0x7f ? a : '.');
		}
		fprintf(f, "|\n");
	}
}

#define L2CAP_ADDR \
	struct sockaddr_l2 addr = { 0 }; \
	addr.l2_family = AF_BLUETOOTH; \
	memcpy(&addr.l2_bdaddr, bdaddr, sizeof(bdaddr_t)); \
	if (cid) addr.l2_cid = htobs(cid); \
	else addr.l2_psm = htobs(psm); \
	addr.l2_bdaddr_type = type;

static int l2cap_bind(int sock, const bdaddr_t *bdaddr,
		int type, int psm, int cid) {
	L2CAP_ADDR
	if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
		return -errno;
	return 0;
}

static int l2cap_connect(int sock, const bdaddr_t *bdaddr,
		int type, int psm, int cid) {
	int err;
	L2CAP_ADDR
	err = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
	if (err < 0 && errno != EAGAIN && errno != EINPROGRESS)
		return -errno;
	return 0;
}
#undef L2CAP_ADDR

#define DBG_LOG(...) fprintf(stderr, __VA_ARGS__)

#define ERR_EXIT(...) \
	do { fprintf(stderr, __VA_ARGS__); \
	exit(1); } while (0)

#define WRITE16_LE(p, a) do { \
	uint32_t __tmp = a; \
	((uint8_t*)(p))[0] = (uint8_t)(a); \
	((uint8_t*)(p))[1] = (uint8_t)(__tmp >> 8); \
} while (0)

#define WRITE16_BE(p, a) do { \
	uint32_t __tmp = a; \
	((uint8_t*)(p))[0] = (uint8_t)(__tmp >> 8); \
	((uint8_t*)(p))[1] = (uint8_t)(a); \
} while (0)

#define READ16_LE(p) ( \
	((uint8_t*)(p))[1] << 8 | \
	((uint8_t*)(p))[0])

#define IO_BUFSIZE 256

typedef struct {
	int sock, verbose, timeout, type, mtu;
	uint8_t buf[IO_BUFSIZE];
} btio_t;

static int bt_send(btio_t *io, const void *data, int len);

#define BT_FILTER_NOTIFY_NONE -1
#define BT_FILTER_NOTIFY_ALL -2
static int bt_filter_notify = BT_FILTER_NOTIFY_NONE;

static int bt_recv(btio_t *io) {
	int ret, len;
	if (io->timeout < 0) return 0;
loop:
	if (io->timeout >= 0) {
		struct pollfd fds = { 0 };
		fds.fd = io->sock;
		fds.events = POLLIN;
		ret = poll(&fds, 1, io->timeout);
		if (ret < 0) {
			DBG_LOG("curtain recv failed: %s = %d (%s)\n",
					"poll", errno, strerror(errno));
			io->timeout = -1;
			return 0;
		}
		if (fds.revents & POLLHUP) {
			DBG_LOG("curtain recv failed: connection closed\n");
			io->timeout = -1;
			return 0;
		}
		if (!ret) return 0;
	}
	len = read(io->sock, io->buf, sizeof(io->buf));
	if (io->verbose >= 2 && len > 0) {
		DBG_LOG("recv (%d):\n", len);
		print_mem(stderr, io->buf, len);
	}
	if (io->type != 0) return len;
	// Exchange MTU Request
	if (len == 3 && io->buf[0] == 0x02) {
		// Exchange MTU Response
		uint8_t cmd[] = { 0x03, io->buf[1], io->buf[2] };
		io->mtu = READ16_LE(io->buf + 1);
		if (io->verbose >= 1)
			DBG_LOG("exchange_mtu = %u\n", io->mtu);
		bt_send(io, cmd, sizeof(cmd));
		goto loop;
	}
	// Handle Value Notification
	if (bt_filter_notify != BT_FILTER_NOTIFY_NONE &&
			len > 3 && io->buf[0] == 0x1b) {
		int handle = READ16_LE(io->buf + 1);
		if (handle != bt_filter_notify) goto loop;
	}
	return len;
}

static int bt_send(btio_t *io, const void *data, int len) {
	const uint8_t *buf = (const uint8_t*)data;
	int ret;

	if (io->timeout < 0) return 0;
	if (!buf) buf = io->buf;
	if (!len) ERR_EXIT("empty message\n");
	if (io->verbose >= 2) {
		DBG_LOG("send (%d):\n", len);
		print_mem(stderr, buf, len);
	}

	ret = write(io->sock, buf, len);
	if (ret < 0) {
		DBG_LOG("curtain send failed: %s = %d (%s)\n",
				"write", errno, strerror(errno));
		io->timeout = -1;
		return 0;
	}
	return ret;
}

static int bt_exchange_mtu(btio_t *io, int val) {
	int len;
	io->mtu = val;
	io->buf[0] = 0x02; // Exchange MTU Request
	WRITE16_LE(io->buf + 1, val);
	bt_send(io, NULL, 3);
	len = bt_recv(io);
	if (len == 5) {
		static const uint8_t cmd[] = { 0x01,0x02,0x00,0x00,0x06 };
		if (memcmp(io->buf, cmd, sizeof(cmd)))
			ERR_EXIT("unhexpected response\n");
		DBG_LOG("exchange_mtu unsupported\n");
		return -1;
	}
	if (len != 3 || io->buf[0] != 3)
		ERR_EXIT("unhexpected response\n");
	return READ16_LE(io->buf + 1);
}

static void bt_write_req(btio_t *io, int handle) {
	io->buf[0] = 0x12;
	WRITE16_LE(io->buf + 1, handle);
	io->buf[3] = 1;
	io->buf[4] = 0;
	bt_send(io, NULL, 5);
	bt_recv(io);
}

static int bt_get_type_range(btio_t *io, int value, int *end) {
	int len, start;
	io->buf[0] = 0x06; // Find By Type Value Request
	WRITE16_LE(io->buf + 1, 1);
	WRITE16_LE(io->buf + 3, 0xffff);
	WRITE16_LE(io->buf + 5, 0x2800);
	WRITE16_LE(io->buf + 7, value);
	bt_send(io, NULL, 9);
	len = bt_recv(io);
	if (len != 5 || io->buf[0] != 0x07) {
		ERR_EXIT("unexpected response\n");
	}
	start = READ16_LE(io->buf + 1);
	*end = READ16_LE(io->buf + 3);
	return start;
}

enum { ENUM_PRIMARY, ENUM_CHARS, ENUM_CHAR_DESC };

static int enum_handles(btio_t *io, int start, int end, int mode,
		int (*cb)(void*, const uint8_t*, int), void *data) {
	int i, j, len, n;
	while (start <= end) {
		if (mode == ENUM_PRIMARY) {
			io->buf[0] = 0x10; // Read By Group Type Request
			WRITE16_LE(io->buf + 1, start);
			WRITE16_LE(io->buf + 3, end);
			WRITE16_LE(io->buf + 5, 0x2800);
			n = 7;
		} else if (mode == ENUM_CHARS) {
			io->buf[0] = 0x08; // Read By Type Request
			WRITE16_LE(io->buf + 1, start);
			WRITE16_LE(io->buf + 3, end);
			WRITE16_LE(io->buf + 5, 0x2803);
			n = 7;
		} else if (mode == ENUM_CHAR_DESC) {
			io->buf[0] = 0x04; // Find Information Request
			WRITE16_LE(io->buf + 1, start);
			WRITE16_LE(io->buf + 3, end);
			n = 5;
		} else break;
		j = io->buf[0];
		bt_send(io, NULL, n);
		len = bt_recv(io);
		if (len <= 2) {
			DBG_LOG("unexpected length\n");
			break;
		}
		if (io->buf[0] == 0x01) {
			if (len != 5 || io->buf[1] != j || READ16_LE(io->buf + 2) != start || io->buf[4] != 0x0a)
				DBG_LOG("unexpected error response\n");
			else start = end + 1;
			break;
		}
		if (io->buf[0] != j + 1) {
			DBG_LOG("unexpected opcode (0x%02x)\n", io->buf[0]);
			break;
		}
		n = io->buf[1]; j = 0;
		if (mode == ENUM_PRIMARY) {
			if (n == 4 + 2 || n == 4 + 16) j = 4;
		} else if (mode == ENUM_CHARS) {
			if (n == 5 + 2 || n == 5 + 16) j = 5;
		} else if (mode == ENUM_CHAR_DESC) {
			if (n == 1) j = 2, n = 2 + 2;
			else if (n == 2) j = 2, n = 2 + 16;
		}
		if (!j) {
			DBG_LOG("unexpected type (%u)\n", n);
			break;
		}
		if ((len - 2) % n) {
			DBG_LOG("unexpected remainder\n");
			break;
		}
		for (i = 2; i < len; i += n) {
			int h = READ16_LE(io->buf + i);
			if (h < start || h > end) {
				DBG_LOG("handle out of range\n");
				return 1;
			}
			start = h + 1;
			j = cb(data, io->buf + i, n);
			if (j) return j;
		}
	}
	return start <= end;
}

struct bt_find_char_data {
	int len; const int *uuid; int *dest;
};

static int bt_find_char_cb(void *data, const uint8_t *buf, int n) {
	struct bt_find_char_data *x = data;
	int k, j, len = x->len;
	const int *uuid = x->uuid;
	int *dest = x->dest;
	int a = READ16_LE(buf + 5);
	for (k = j = 0; j < len; j++) {
		if (dest[j] == -1 && n == 5 + 2 && *uuid == a)
			dest[j] = READ16_LE(buf + 3);
		uuid++;
		k += dest[j] != -1;
	}
	return k == len ? 2 : 0;
}

static int bt_find_char(btio_t *io, int start, int end,
		int n, const int *uuid, int *dest) {
	int i, k;
	struct bt_find_char_data data = { n, uuid, dest };
	memset(dest, -1, n * sizeof(*dest));
	i = enum_handles(io, start, end, ENUM_CHARS,
			&bt_find_char_cb, &data);
	if (i == 2) return n;
	for (k = i = 0; i < n; i++) k += dest[i] != -1;
	return k;
}

static int bt_find_char_desc_cb(void *data, const uint8_t *buf, int n) {
	int *x = data;
	if (n == 4 && *x == READ16_LE(buf + 2)) {
		*x = READ16_LE(buf);
		return 2;
	}
	return 0;
}

static int bt_find_char_desc(btio_t *io, int start, int end, int value) {
	int i, data = value;
	i = enum_handles(io, start, end, ENUM_CHAR_DESC,
			&bt_find_char_desc_cb, &data);
	if (i == 2) return data;
	return -1;
}

static void bt_write_req_desc(btio_t *io, int desc_handle, int end) {
	int ret = desc_handle + 1;
	if (ret <= end) {
		ret = bt_find_char_desc(io, ret, ret, 0x2902);
		if (ret >= 0) {
			bt_write_req(io, ret);
			return;
		}
	}
	ERR_EXIT("can't find char desc\n");
}

static int ch2hex(unsigned a) {
	const char *tab = "abcdef0123456789ABCDEF";
	const char *p = strchr(tab, a);
	return p ? (p - tab + 10) & 15 : -1;
}

static int str2bdaddr(const char *s, bdaddr_t *d) {
	int i;
	for (i = 0; i < 6; i++) {
		int h = ch2hex(*s++), a;
		if (h < 0 || (a = ch2hex(*s++)) < 0) break;
		d->b[5 - i] = h << 4 | a;
		if (*s++ != (i == 5 ? 0 : ':')) break;
	}
	return i - 6;
}

