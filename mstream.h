#include <stdlib.h>
#include <stdio.h>

typedef struct {
	char *next, *rpos, *wpos, *rend, *wend; size_t size;
} mstream_t;

#ifndef MSTREAM_ATTR
#define MSTREAM_ATTR __attribute__((unused)) static
#endif

MSTREAM_ATTR void mstream_rewind(mstream_t *m, int rewrite) {
	char *p = (char*)(m + 1), *e = (char*)m + m->size;
	m->rpos = p; m->rend = e;
	if (rewrite) m->wpos = p, m->wend = e;
}

MSTREAM_ATTR mstream_t* mstream_new(size_t size) {
	mstream_t *m = (mstream_t*)malloc(size);
	if (m)
		m->next = NULL,
		m->rpos = m->wpos = (char*)(m + 1),
		m->rend = m->wend = (char*)m + size,
		m->size = size;
	return m;
}

MSTREAM_ATTR void mstream_free(mstream_t *m) {
	void *p, *n = m;
	while (n) p = n, n = *(void**)p, free(p);
}

MSTREAM_ATTR void mstream_putc(mstream_t *m, int ch) {
	char *p = m->wpos, *e = m->wend;
	if (p == e) {
		size_t size = m->size;
		void **last = (void**)(e - size), *next = *last;
		if (!next) {
			if (!(next = malloc(size))) return;
			*last = next; *(void**)next = NULL;
		}
		m->wend = (char*)next + size;
		p = (char*)((void**)next + 1);
	}
	*p++ = ch; m->wpos = p;
}

MSTREAM_ATTR int mstream_getc(mstream_t *m) {
	char *p = m->rpos, *e;
	if (p == m->wpos) return -1;
	if (p == (e = m->rend)) {
		size_t size = m->size;
		void *next = *(void**)(e - size);
		m->rend = (char*)next + size;
		p = (char*)((void**)next + 1);
	}
	m->rpos = p + 1;
	return *(unsigned char*)p;
}

// TODO: fast implementation using memcpy
MSTREAM_ATTR size_t mstream_write(mstream_t *m, const void *buf, size_t n) {
	const char *p = (const char*)buf;
	size_t i;
	for (i = 0; i < n; i++) mstream_putc(m, p[i]);
	return i;
}

MSTREAM_ATTR size_t mstream_read(mstream_t *m, void *buf, size_t n) {
	char *p = (char*)buf;
	size_t i;
	for (i = 0; i < n; i++) {
		int a = mstream_getc(m);
		if (a < 0) break;
		p[i] = a;
	}
	return i;
}

MSTREAM_ATTR size_t mstream_save(mstream_t *m, const char *fn) {
	size_t ret = 0, n, k;
	FILE *f = fopen(fn, "wb");
	if (f) {
		char *p = (char*)(m + 1), *e;
		void *next = m; int stop;
		for (;;) {
			e = (char*)next + m->size;
			n = e - p; k = m->wpos - p;
			if (!k) break;
			if ((stop = n >= k)) n = k;
			ret += k = fwrite(p, 1, n, f);
			if (stop || k != n) break;
			next = *(void**)next;
			p = (char*)((void**)next + 1);
		}
		fclose(f);
	}
	return ret;
}

MSTREAM_ATTR size_t mstream_load(mstream_t *m, const char *fn) {
	size_t ret = 0, n, k = 0;
	char *p = (char*)(m + 1), *e;
	size_t size = m->size;
	void **last = (void**)m, *next;
	FILE *f = fopen(fn, "rb");
	m->rpos = p; m->rend = (char*)last + size;
	if (f) {
		for (;;) {
			e = (char*)last + size;
			n = e - p;
			ret += k = fread(p, 1, n, f);
			if (k != n) break;
			next = *last;
			if (!next) {
				if (!(next = malloc(size))) break;
				*last = next; *(void**)next = NULL;
			}
			p = (char*)((void**)(last = next) + 1);
		}
		fclose(f);
	}
	m->wpos = p + k; m->wend = (char*)last + size;
	return ret;
}

