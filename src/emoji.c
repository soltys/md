#include "emoji.h"

size_t utf8_decode(const char *str, size_t n, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)str;
    if (n == 0) { *cp = 0; return 0; }
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    size_t len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    if (len == 1 || len > n) { *cp = 0xFFFD; return 1; }
    uint32_t v = c & (0xFF >> (len + 1));
    for (size_t i = 1; i < len; ++i) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return len;
}

static bool is_emoji_base(uint32_t cp)
{
    return (cp >= 0x1F000 && cp <= 0x1FAFF) ||  /* pictographs, emoticons, etc. */
           (cp >= 0x2600 && cp <= 0x27BF) ||    /* misc symbols, dingbats */
           (cp >= 0x2300 && cp <= 0x23FF && cp != 0x2318) ||
           (cp >= 0x2B05 && cp <= 0x2B55) ||
           cp == 0x3030 || cp == 0x303D || cp == 0x3297 || cp == 0x3299 ||
           cp == 0x2139 || cp == 0x24C2;
}

static bool is_modifier(uint32_t cp)
{
    return cp == 0xFE0F || cp == 0x20E3 || (cp >= 0x1F3FB && cp <= 0x1F3FF) ||
           (cp >= 0xE0020 && cp <= 0xE007F);
}

size_t emoji_cluster(const char *s, size_t n)
{
    uint32_t cp;
    size_t pos = utf8_decode(s, n, &cp);
    if (pos == 0) return 0;

    /* Keycaps: [0-9#*] FE0F? 20E3 */
    if ((cp >= '0' && cp <= '9') || cp == '#' || cp == '*') {
        uint32_t a, b;
        size_t l1 = utf8_decode(s + pos, n - pos, &a);
        if (l1 && a == 0xFE0F) {
            size_t l2 = utf8_decode(s + pos + l1, n - pos - l1, &b);
            if (l2 && b == 0x20E3) return pos + l1 + l2;
        } else if (l1 && a == 0x20E3) {
            return pos + l1;
        }
        return 0;
    }

    /* Flags: two regional indicators. */
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {
        uint32_t b;
        size_t l = utf8_decode(s + pos, n - pos, &b);
        if (l && b >= 0x1F1E6 && b <= 0x1F1FF) return pos + l;
        return pos;
    }

    bool base = is_emoji_base(cp);
    if (!base) {
        /* Text symbols turned into emoji by VS16, e.g. U+2764 is covered,
         * but also things like U+00A9 FE0F. */
        uint32_t b;
        size_t l = utf8_decode(s + pos, n - pos, &b);
        if (cp >= 0x80 && l && b == 0xFE0F) return pos + l;
        return 0;
    }

    for (;;) {
        uint32_t next;
        size_t l = utf8_decode(s + pos, n - pos, &next);
        if (l == 0) break;
        if (is_modifier(next)) { pos += l; continue; }
        if (next == 0x200D) { /* ZWJ joins the next emoji */
            uint32_t after;
            size_t l2 = utf8_decode(s + pos + l, n - pos - l, &after);
            if (l2 && (is_emoji_base(after) || after >= 0x2000)) { pos += l + l2; continue; }
        }
        break;
    }
    return pos;
}
