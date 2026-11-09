#ifndef MD_MSGPACK_H
#define MD_MSGPACK_H

/* Minimal MessagePack reader/writer, just enough for Neovim's RPC. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum MpType {
    MP_NIL, MP_BOOL, MP_INT, MP_FLOAT, MP_STR, MP_BIN, MP_ARRAY, MP_MAP, MP_EXT
} MpType;

typedef struct MpObj {
    MpType type;
    uint32_t len;              /* bytes for STR/BIN/EXT, items for ARRAY, pairs for MAP */
    union {
        bool b;
        int64_t i;
        double f;
        const char *s;         /* STR/BIN/EXT data, not NUL-terminated */
        struct MpObj *items;   /* ARRAY: len items; MAP: 2*len (key, value, ...) */
    } v;
} MpObj;

/* A decoded message owns a copy of its bytes and all its nodes. */
typedef struct MpMsg {
    char *raw;
    MpObj *nodes;
    MpObj root;
} MpMsg;

/* Tries to decode one object from buf. Returns bytes consumed, 0 if the
 * buffer does not yet hold a complete object, or SIZE_MAX if malformed. */
size_t mp_decode(const char *buf, size_t len, MpMsg *out);
void mp_msg_free(MpMsg *msg);

/* Accessors that tolerate wrong types (return defaults). */
int64_t mp_int(const MpObj *o);
bool mp_str_eq(const MpObj *o, const char *s);
const MpObj *mp_at(const MpObj *array, uint32_t i);  /* NULL if out of range */
const MpObj *mp_map_get(const MpObj *map, const char *key);

/* ------------------------------------------------------------ writer -- */

typedef struct MpBuf {
    char *data;
    size_t len, cap;
} MpBuf;

void mp_w_nil(MpBuf *b);
void mp_w_bool(MpBuf *b, bool v);
void mp_w_int(MpBuf *b, int64_t v);
void mp_w_str(MpBuf *b, const char *s, size_t n);
void mp_w_cstr(MpBuf *b, const char *s);
void mp_w_array(MpBuf *b, uint32_t n);
void mp_w_map(MpBuf *b, uint32_t n);
void mp_buf_free(MpBuf *b);

#endif
