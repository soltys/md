#include "regex.h"
#include <SDL3/SDL.h>

typedef struct Atom {
    const char *p;   /* start of the atom in the pattern */
    size_t n;        /* its length */
} Atom;

static bool class_match(const char *p, size_t n, char c)
{
    /* p points after '[', n excludes the closing ']' */
    bool neg = n > 0 && p[0] == '^';
    size_t i = neg ? 1 : 0;
    bool hit = false;
    while (i < n) {
        char a = p[i];
        if (a == '\\' && i + 1 < n) {
            char e = p[i + 1];
            if ((e == 'd' && SDL_isdigit(c)) || (e == 'w' && (SDL_isalnum(c) || c == '_')) ||
                (e == 's' && SDL_isspace(c)) || e == c) hit = true;
            i += 2;
        } else if (i + 2 < n && p[i + 1] == '-') {
            if (c >= a && c <= p[i + 2]) hit = true;
            i += 3;
        } else {
            if (a == c) hit = true;
            i++;
        }
    }
    return neg ? !hit : hit;
}

static bool atom_match(Atom a, char c)
{
    if (a.p[0] == '.') return c != '\n';
    if (a.p[0] == '[') return class_match(a.p + 1, a.n - 2, c);
    if (a.p[0] == '\\') {
        char e = a.p[1];
        switch (e) {
        case 'd': return SDL_isdigit(c);
        case 'D': return !SDL_isdigit(c);
        case 'w': return SDL_isalnum(c) || c == '_';
        case 'W': return !(SDL_isalnum(c) || c == '_');
        case 's': return SDL_isspace(c);
        case 'S': return !SDL_isspace(c);
        default:  return c == e;
        }
    }
    return a.p[0] == c;
}

/* Parses the atom at p; returns false on unsupported syntax. */
static bool next_atom(const char *p, Atom *a)
{
    a->p = p;
    if (*p == '[') {
        const char *q = p + 1;
        if (*q == '^') q++;
        if (*q == ']') q++;
        while (*q && *q != ']') q += (*q == '\\' && q[1]) ? 2 : 1;
        if (*q != ']') return false;
        a->n = (size_t)(q - p) + 1;
    } else if (*p == '\\' && p[1]) {
        a->n = 2;
    } else if (*p == '(' || *p == ')' || *p == '|' || *p == '{') {
        return false;
    } else {
        a->n = 1;
    }
    return true;
}

static int match_here(const char *pat, const char *t, const char *end)
{
    if (*pat == 0) return 1;
    if (pat[0] == '$' && pat[1] == 0) return t == end;
    Atom a;
    if (!next_atom(pat, &a)) return -1;
    const char *rest = pat + a.n;
    char q = *rest;
    if (q == '*' || q == '+' || q == '?') {
        rest++;
        size_t min = q == '+' ? 1 : 0;
        size_t max = q == '?' ? 1 : (size_t)-1;
        size_t k = 0;
        while (t + k < end && k < max && atom_match(a, t[k])) k++;
        for (;;) {                   /* greedy, backtrack */
            if (k >= min) {
                int r = match_here(rest, t + k, end);
                if (r != 0) return r;
            }
            if (k == 0) break;
            k--;
        }
        return 0;
    }
    if (t < end && atom_match(a, *t)) return match_here(rest, t + 1, end);
    return 0;
}

int regex_search(const char *pattern, const char *text, size_t len)
{
    const char *end = text + len;
    if (pattern[0] == '^') return match_here(pattern + 1, text, end);
    const char *t = text;
    do {
        int r = match_here(pattern, t, end);
        if (r != 0) return r;
    } while (t++ < end);
    return 0;
}
