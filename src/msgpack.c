#include "msgpack.h"
#include <SDL3/SDL.h>

/* ------------------------------------------------------------ decode -- */

typedef struct Rd {
    const uint8_t *p;
    size_t len, pos;
    MpObj *nodes;   /* NULL during the counting pass */
    size_t count;
    bool incomplete, bad;
} Rd;

static bool need(Rd *r, size_t n)
{
    if (r->len - r->pos < n) { r->incomplete = true; return false; }
    return true;
}

static uint64_t be(Rd *r, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | r->p[r->pos + i];
    r->pos += (size_t)n;
    return v;
}

static void read_obj(Rd *r, MpObj *o);

static void read_seq(Rd *r, MpObj *o, MpType type, uint32_t n)
{
    o->type = type;
    o->len = n;
    size_t items = type == MP_MAP ? (size_t)n * 2 : n;
    MpObj *dst = NULL;
    if (r->nodes) dst = r->nodes + r->count;
    r->count += items;
    if (r->nodes) o->v.items = dst;
    for (size_t i = 0; i < items && !r->incomplete && !r->bad; ++i) {
        MpObj tmp;
        read_obj(r, dst ? &dst[i] : &tmp);
    }
}

static void read_bytes(Rd *r, MpObj *o, MpType type, size_t n)
{
    if (!need(r, n)) return;
    o->type = type;
    o->len = (uint32_t)n;
    o->v.s = (const char *)r->p + r->pos;
    r->pos += n;
}

static void read_obj(Rd *r, MpObj *o)
{
    if (!need(r, 1)) return;
    uint8_t t = r->p[r->pos++];
    SDL_zerop(o);
    if (t <= 0x7f) { o->type = MP_INT; o->v.i = t; return; }
    if (t >= 0xe0) { o->type = MP_INT; o->v.i = (int8_t)t; return; }
    if ((t & 0xf0) == 0x80) { read_seq(r, o, MP_MAP, t & 0x0f); return; }
    if ((t & 0xf0) == 0x90) { read_seq(r, o, MP_ARRAY, t & 0x0f); return; }
    if ((t & 0xe0) == 0xa0) { read_bytes(r, o, MP_STR, t & 0x1f); return; }

    switch (t) {
    case 0xc0: o->type = MP_NIL; return;
    case 0xc2: o->type = MP_BOOL; o->v.b = false; return;
    case 0xc3: o->type = MP_BOOL; o->v.b = true; return;
    case 0xc4: if (need(r, 1)) read_bytes(r, o, MP_BIN, (size_t)be(r, 1)); return;
    case 0xc5: if (need(r, 2)) read_bytes(r, o, MP_BIN, (size_t)be(r, 2)); return;
    case 0xc6: if (need(r, 4)) read_bytes(r, o, MP_BIN, (size_t)be(r, 4)); return;
    case 0xc7: if (need(r, 2)) { size_t n = (size_t)be(r, 1); r->pos++; read_bytes(r, o, MP_EXT, n); } return;
    case 0xc8: if (need(r, 3)) { size_t n = (size_t)be(r, 2); r->pos++; read_bytes(r, o, MP_EXT, n); } return;
    case 0xc9: if (need(r, 5)) { size_t n = (size_t)be(r, 4); r->pos++; read_bytes(r, o, MP_EXT, n); } return;
    case 0xca: if (need(r, 4)) { uint32_t u = (uint32_t)be(r, 4); float f; SDL_memcpy(&f, &u, 4); o->type = MP_FLOAT; o->v.f = f; } return;
    case 0xcb: if (need(r, 8)) { uint64_t u = be(r, 8); double d; SDL_memcpy(&d, &u, 8); o->type = MP_FLOAT; o->v.f = d; } return;
    case 0xcc: if (need(r, 1)) { o->type = MP_INT; o->v.i = (int64_t)be(r, 1); } return;
    case 0xcd: if (need(r, 2)) { o->type = MP_INT; o->v.i = (int64_t)be(r, 2); } return;
    case 0xce: if (need(r, 4)) { o->type = MP_INT; o->v.i = (int64_t)be(r, 4); } return;
    case 0xcf: if (need(r, 8)) { o->type = MP_INT; o->v.i = (int64_t)be(r, 8); } return;
    case 0xd0: if (need(r, 1)) { o->type = MP_INT; o->v.i = (int8_t)be(r, 1); } return;
    case 0xd1: if (need(r, 2)) { o->type = MP_INT; o->v.i = (int16_t)be(r, 2); } return;
    case 0xd2: if (need(r, 4)) { o->type = MP_INT; o->v.i = (int32_t)be(r, 4); } return;
    case 0xd3: if (need(r, 8)) { o->type = MP_INT; o->v.i = (int64_t)be(r, 8); } return;
    case 0xd4: if (need(r, 2)) { r->pos++; read_bytes(r, o, MP_EXT, 1); } return;
    case 0xd5: if (need(r, 3)) { r->pos++; read_bytes(r, o, MP_EXT, 2); } return;
    case 0xd6: if (need(r, 5)) { r->pos++; read_bytes(r, o, MP_EXT, 4); } return;
    case 0xd7: if (need(r, 9)) { r->pos++; read_bytes(r, o, MP_EXT, 8); } return;
    case 0xd8: if (need(r, 17)) { r->pos++; read_bytes(r, o, MP_EXT, 16); } return;
    case 0xd9: if (need(r, 1)) read_bytes(r, o, MP_STR, (size_t)be(r, 1)); return;
    case 0xda: if (need(r, 2)) read_bytes(r, o, MP_STR, (size_t)be(r, 2)); return;
    case 0xdb: if (need(r, 4)) read_bytes(r, o, MP_STR, (size_t)be(r, 4)); return;
    case 0xdc: if (need(r, 2)) read_seq(r, o, MP_ARRAY, (uint32_t)be(r, 2)); return;
    case 0xdd: if (need(r, 4)) read_seq(r, o, MP_ARRAY, (uint32_t)be(r, 4)); return;
    case 0xde: if (need(r, 2)) read_seq(r, o, MP_MAP, (uint32_t)be(r, 2)); return;
    case 0xdf: if (need(r, 4)) read_seq(r, o, MP_MAP, (uint32_t)be(r, 4)); return;
    default: r->bad = true; return;
    }
}

size_t mp_decode(const char *buf, size_t len, MpMsg *out)
{
    /* Pass 1: completeness check and node count. */
    Rd r = {(const uint8_t *)buf, len, 0, NULL, 0, false, false};
    MpObj tmp;
    read_obj(&r, &tmp);
    if (r.bad) return SIZE_MAX;
    if (r.incomplete) return 0;
    size_t used = r.pos;

    /* Pass 2: decode into owned storage, strings pointing into raw. */
    out->raw = SDL_malloc(used ? used : 1);
    SDL_memcpy(out->raw, buf, used);
    out->nodes = SDL_malloc(sizeof(MpObj) * (r.count ? r.count : 1));
    Rd r2 = {(const uint8_t *)out->raw, used, 0, out->nodes, 0, false, false};
    read_obj(&r2, &out->root);
    return used;
}

void mp_msg_free(MpMsg *msg)
{
    SDL_free(msg->raw);
    SDL_free(msg->nodes);
    msg->raw = NULL;
    msg->nodes = NULL;
}

int64_t mp_int(const MpObj *o)
{
    if (!o) return 0;
    if (o->type == MP_INT) return o->v.i;
    if (o->type == MP_FLOAT) return (int64_t)o->v.f;
    if (o->type == MP_BOOL) return o->v.b;
    return 0;
}

bool mp_str_eq(const MpObj *o, const char *s)
{
    if (!o || o->type != MP_STR) return false;
    size_t n = SDL_strlen(s);
    return o->len == n && SDL_memcmp(o->v.s, s, n) == 0;
}

const MpObj *mp_at(const MpObj *a, uint32_t i)
{
    if (!a || a->type != MP_ARRAY || i >= a->len) return NULL;
    return &a->v.items[i];
}

const MpObj *mp_map_get(const MpObj *m, const char *key)
{
    if (!m || m->type != MP_MAP) return NULL;
    for (uint32_t i = 0; i < m->len; ++i) {
        if (mp_str_eq(&m->v.items[2 * i], key)) return &m->v.items[2 * i + 1];
    }
    return NULL;
}

/* ------------------------------------------------------------ encode -- */

static void put(MpBuf *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->len + n) cap *= 2;
        b->data = SDL_realloc(b->data, cap);
        b->cap = cap;
    }
    SDL_memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void put_be(MpBuf *b, uint8_t tag, uint64_t v, int n)
{
    uint8_t tmp[9];
    tmp[0] = tag;
    for (int i = 0; i < n; ++i) tmp[1 + i] = (uint8_t)(v >> (8 * (n - 1 - i)));
    put(b, tmp, (size_t)n + 1);
}

void mp_w_nil(MpBuf *b) { uint8_t t = 0xc0; put(b, &t, 1); }
void mp_w_bool(MpBuf *b, bool v) { uint8_t t = v ? 0xc3 : 0xc2; put(b, &t, 1); }

void mp_w_int(MpBuf *b, int64_t v)
{
    if (v >= 0 && v <= 0x7f) { uint8_t t = (uint8_t)v; put(b, &t, 1); }
    else if (v < 0 && v >= -32) { uint8_t t = (uint8_t)(int8_t)v; put(b, &t, 1); }
    else if (v >= 0 && v <= 0xffffffffLL) put_be(b, 0xce, (uint64_t)v, 4);
    else put_be(b, 0xd3, (uint64_t)v, 8);
}

void mp_w_str(MpBuf *b, const char *s, size_t n)
{
    if (n < 32) { uint8_t t = (uint8_t)(0xa0 | n); put(b, &t, 1); }
    else put_be(b, 0xdb, n, 4);
    put(b, s, n);
}

void mp_w_cstr(MpBuf *b, const char *s) { mp_w_str(b, s, SDL_strlen(s)); }

void mp_w_array(MpBuf *b, uint32_t n)
{
    if (n < 16) { uint8_t t = (uint8_t)(0x90 | n); put(b, &t, 1); }
    else put_be(b, 0xdd, n, 4);
}

void mp_w_map(MpBuf *b, uint32_t n)
{
    if (n < 16) { uint8_t t = (uint8_t)(0x80 | n); put(b, &t, 1); }
    else put_be(b, 0xdf, n, 4);
}

void mp_buf_free(MpBuf *b)
{
    SDL_free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}
