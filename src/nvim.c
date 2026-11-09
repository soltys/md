#include "nvim.h"
#include "assets.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

static SDL_Thread *start_thread;
static char *start_file;

/* ------------------------------------------------------------ startup -- */

/* Writes the embedded init.lua next to md's other per-user data, skipping
 * the write when the file on disk is already identical. */
static char *install_config(void)
{
    const void *data;
    size_t size;
    if (!asset_get(ASSET_NVIM_INIT, &data, &size)) {
        SDL_SetError("embedded nvim config missing");
        return NULL;
    }
    char *dir = SDL_GetPrefPath(NULL, "md");
    if (!dir) return NULL;
    char *path = NULL;
    SDL_asprintf(&path, "%sinit.lua", dir);
    SDL_free(dir);
    if (!path) return NULL;

    size_t old_size = 0;
    void *old = SDL_LoadFile(path, &old_size);
    bool same = old && old_size == size && SDL_memcmp(old, data, size) == 0;
    SDL_free(old);
    if (!same && !SDL_SaveFile(path, data, size)) {
        SDL_free(path);
        return NULL;
    }
    return path;
}

/* SDL creates the pipes non-blocking; the reader thread wants to sleep in
 * read() and writes should never be partial, so switch them to blocking. */
static void make_blocking(SDL_IOStream *io)
{
    if (!io) return;
    SDL_PropertiesID props = SDL_GetIOProperties(io);
#ifdef _WIN32
    HANDLE h = SDL_GetPointerProperty(props, SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER, NULL);
    if (h) {
        DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
        SetNamedPipeHandleState(h, &mode, NULL, NULL);
    }
#else
    int fd = (int)SDL_GetNumberProperty(props, SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1);
    if (fd >= 0) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
#endif
}

static int SDLCALL start_thread_main(void *userdata)
{
    Nvim *nv = userdata;

    nv->config_path = install_config();
    if (!nv->config_path) {
        SDL_asprintf(&nv->error, "Cannot write Neovim config: %s", SDL_GetError());
        return -1;
    }

    const char *args[6];
    int n = 0;
    args[n++] = "nvim";
    args[n++] = "--embed";
    args[n++] = "-u";
    args[n++] = nv->config_path;
    if (start_file) args[n++] = start_file;
    args[n] = NULL;

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, (void *)args);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true); /* no console */
    nv->process = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);

    if (!nv->process) {
        SDL_asprintf(&nv->error, "Cannot start nvim (is it on PATH?): %s", SDL_GetError());
        return -1;
    }
    nv->in = SDL_GetProcessInput(nv->process);
    nv->out = SDL_GetProcessOutput(nv->process);
    make_blocking(nv->in);
    make_blocking(nv->out);
    return 0;
}

void nvim_start_async(Nvim *nv, const char *file, Uint32 wake_event)
{
    SDL_zerop(nv);
    nv->wake_event = wake_event;
    nv->next_msgid = 1;
    start_file = file ? SDL_strdup(file) : NULL;
    start_thread = SDL_CreateThread(start_thread_main, "md-nvim", nv);
    if (!start_thread) start_thread_main(nv);
}

bool nvim_wait_started(Nvim *nv)
{
    if (start_thread) {
        SDL_WaitThread(start_thread, NULL);
        start_thread = NULL;
    }
    SDL_free(start_file);
    start_file = NULL;
    return nv->process != NULL;
}

/* ------------------------------------------------------------- reader -- */

static void wake(Nvim *nv)
{
    if (!nv->wake_event) return;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = nv->wake_event;
    SDL_PushEvent(&ev);
}

/* One read of whatever is available. SDL_ReadIO is avoided on purpose: on
 * POSIX it follows a short read with a second read() to rule out EOF, which
 * on a blocking pipe waits for nvim's *next* output, so the tail of every
 * burst (a response, the last redraw) arrived late. Returns bytes read,
 * 0 when nothing is ready, -1 on EOF or error. */
static long long read_some(SDL_IOStream *io, char *dst, size_t size)
{
    SDL_PropertiesID props = SDL_GetIOProperties(io);
#ifdef _WIN32
    HANDLE h = SDL_GetPointerProperty(props, SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER, NULL);
    if (h) {
        DWORD got = 0;
        if (!ReadFile(h, dst, (DWORD)SDL_min(size, 1u << 30), &got, NULL)) {
            return GetLastError() == ERROR_NO_DATA ? 0 : -1;
        }
        return got > 0 ? (long long)got : -1;
    }
#else
    int fd = (int)SDL_GetNumberProperty(props, SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1);
    if (fd >= 0) {
        for (;;) {
            ssize_t got = read(fd, dst, size);
            if (got > 0) return (long long)got;
            if (got == 0) return -1;
            if (errno == EINTR) continue;
            return errno == EAGAIN ? 0 : -1;
        }
    }
#endif
    size_t got = SDL_ReadIO(io, dst, size);
    if (got > 0) return (long long)got;
    return SDL_GetIOStatus(io) == SDL_IO_STATUS_NOT_READY ? 0 : -1;
}

static int SDLCALL reader_main(void *userdata)
{
    Nvim *nv = userdata;
    size_t cap = 1 << 16, len = 0;
    char *buf = SDL_malloc(cap);

    for (;;) {
        if (cap - len < 16384) {
            cap *= 2;
            buf = SDL_realloc(buf, cap);
        }
        long long got = read_some(nv->out, buf + len, cap - len);
        if (got == 0) { SDL_Delay(2); continue; } /* not ready (non-blocking fallback) */
        if (got < 0) break; /* EOF or error: nvim is gone */
        len += (size_t)got;

        size_t pos = 0;
        bool queued = false;
        for (;;) {
            MpMsg msg;
            size_t used = mp_decode(buf + pos, len - pos, &msg);
            if (used == 0) break;
            if (used == SIZE_MAX) { len = pos = 0; break; } /* resync: drop */
            pos += used;
            SDL_LockMutex(nv->lock);
            if (nv->queue_len == nv->queue_cap) {
                nv->queue_cap = nv->queue_cap ? nv->queue_cap * 2 : 64;
                nv->queue = SDL_realloc(nv->queue, sizeof(MpMsg) * (size_t)nv->queue_cap);
            }
            nv->queue[nv->queue_len++] = msg;
            SDL_UnlockMutex(nv->lock);
            queued = true;
        }
        if (pos) {
            SDL_memmove(buf, buf + pos, len - pos);
            len -= pos;
        }
        if (queued) wake(nv);
    }
    SDL_free(buf);
    SDL_SetAtomicInt(&nv->exited, 1);
    wake(nv);
    return 0;
}

/* ---------------------------------------------------------------- rpc -- */

static void send(Nvim *nv, MpBuf *b)
{
    if (nv->in && b->len) SDL_WriteIO(nv->in, b->data, b->len);
    mp_buf_free(b);
}

/* Starts a notification [2, method, params]; caller writes params array. */
static void begin_notify(MpBuf *b, const char *method, uint32_t nparams)
{
    mp_w_array(b, 3);
    mp_w_int(b, 2);
    mp_w_cstr(b, method);
    mp_w_array(b, nparams);
}

static uint32_t begin_request(Nvim *nv, MpBuf *b, const char *method, uint32_t nparams)
{
    uint32_t id = nv->next_msgid++;
    mp_w_array(b, 4);
    mp_w_int(b, 0);
    mp_w_int(b, id);
    mp_w_cstr(b, method);
    mp_w_array(b, nparams);
    return id;
}

static void exec_lua_notify(Nvim *nv, const char *code, const char *arg)
{
    MpBuf b = {0};
    begin_notify(&b, "nvim_exec_lua", 2);
    mp_w_cstr(&b, code);
    if (arg) { mp_w_array(&b, 1); mp_w_cstr(&b, arg); }
    else mp_w_array(&b, 0);
    send(nv, &b);
}

/* Lua run once after attaching: tell md when the buffer text changes and
 * where the window is scrolled, for the live preview and scroll sync. */
static const char attach_lua[] =
    "local ch = ...\n"
    "local g = vim.api.nvim_create_augroup('md_ui', { clear = true })\n"
    "vim.api.nvim_create_autocmd({ 'TextChanged', 'TextChangedI', 'TextChangedP', 'BufWinEnter', 'BufWritePost' }, {\n"
    "  group = g, callback = function() vim.rpcnotify(ch, 'md_changed') end })\n"
    "local last = -1\n"
    "vim.api.nvim_create_autocmd({ 'CursorMoved', 'CursorMovedI', 'WinScrolled', 'BufWinEnter' }, {\n"
    "  group = g, callback = function()\n"
    "    local top = vim.fn.line('w0')\n"
    "    if top ~= last then last = top; vim.rpcnotify(ch, 'md_scroll', top) end\n"
    "  end })\n";

bool nvim_attach(Nvim *nv, int cols, int rows)
{
    if (!nv->process || nv->attached) return nv->attached;
    nv->lock = SDL_CreateMutex();
    nv->reader = SDL_CreateThread(reader_main, "md-nvim-rpc", nv);
    if (!nv->reader) return false;

    MpBuf b = {0};
    begin_notify(&b, "nvim_ui_attach", 3);
    mp_w_int(&b, SDL_max(cols, 1));
    mp_w_int(&b, SDL_max(rows, 1));
    mp_w_map(&b, 2);
    mp_w_cstr(&b, "rgb");          mp_w_bool(&b, true);
    mp_w_cstr(&b, "ext_linegrid"); mp_w_bool(&b, true);
    send(nv, &b);

    /* The autocmds need md's channel id; ask for it, the reply installs them. */
    nv->api_info_request = begin_request(nv, &b, "nvim_get_api_info", 0);
    send(nv, &b);
    nv->attached = true;
    return true;
}

void nvim_resize(Nvim *nv, int cols, int rows)
{
    if (!nv->attached) return;
    MpBuf b = {0};
    begin_notify(&b, "nvim_ui_try_resize", 2);
    mp_w_int(&b, SDL_max(cols, 1));
    mp_w_int(&b, SDL_max(rows, 1));
    send(nv, &b);
}

void nvim_input(Nvim *nv, const char *keys)
{
    if (!nv->attached || !keys || !*keys) return;
    MpBuf b = {0};
    begin_notify(&b, "nvim_input", 1);
    mp_w_cstr(&b, keys);
    send(nv, &b);
}

void nvim_input_mouse(Nvim *nv, const char *button, const char *action,
                      const char *mods, int row, int col)
{
    if (!nv->attached) return;
    MpBuf b = {0};
    begin_notify(&b, "nvim_input_mouse", 6);
    mp_w_cstr(&b, button);
    mp_w_cstr(&b, action);
    mp_w_cstr(&b, mods);
    mp_w_int(&b, 0); /* grid 0 = global grid (no ext_multigrid) */
    mp_w_int(&b, row);
    mp_w_int(&b, col);
    send(nv, &b);
}

void nvim_edit_file(Nvim *nv, const char *path)
{
    exec_lua_notify(nv, "vim.cmd.edit(vim.fn.fnameescape(...))", path);
}

void nvim_scroll_to_line(Nvim *nv, int line)
{
    if (!nv->attached) return;
    char arg[16];
    SDL_snprintf(arg, sizeof arg, "%d", line);
    /* Moves the cursor only when it would end up off screen, then sets the
     * top line, like scrolling with the mouse wheel. */
    exec_lua_notify(nv,
        "local l = tonumber(...)\n"
        "local w = vim.api.nvim_get_current_win()\n"
        "local n = vim.api.nvim_buf_line_count(0)\n"
        "l = math.max(1, math.min(l, n))\n"
        "local h = vim.api.nvim_win_get_height(w)\n"
        "local so = math.min(vim.wo[w].scrolloff, math.floor((h - 1) / 2))\n"
        "local c = vim.api.nvim_win_get_cursor(w)[1]\n"
        "local lo, hi = l + so, l + h - 1 - so\n"
        "if c < lo or c > hi then\n"
        "  vim.api.nvim_win_set_cursor(w, { math.min(lo, n), 0 })\n"
        "end\n"
        "vim.fn.winrestview({ topline = l })", arg);
}

void nvim_request_quit(Nvim *nv)
{
    /* Leaves any mode first; with unsaved changes nvim refuses and says so. */
    nvim_input(nv, "<C-\\><C-n>:qa<CR>");
}

void nvim_request_text(Nvim *nv)
{
    if (!nv->attached) return;
    if (nv->text_request) { nv->text_dirty_again = true; return; }
    MpBuf b = {0};
    nv->text_request = begin_request(nv, &b, "nvim_exec_lua", 2);
    mp_w_cstr(&b, "return { vim.api.nvim_buf_get_name(0), "
                  "table.concat(vim.api.nvim_buf_get_lines(0, 0, -1, false), '\\n') }");
    mp_w_array(&b, 0);
    send(nv, &b);
    nv->text_changed = false;
}

/* ------------------------------------------------------------- redraw -- */

const NvHl *nvim_hl(const Nvim *nv, int id)
{
    static const NvHl none = {0};
    if (id <= 0 || id >= nv->hl_count) return &none;
    return &nv->hls[id];
}

static NvCell *cell(Nvim *nv, int row, int col)
{
    return &nv->cells[(size_t)row * (size_t)nv->cols + (size_t)col];
}

static void grid_resize(Nvim *nv, int cols, int rows)
{
    if (cols == nv->cols && rows == nv->rows) return;
    NvCell *cells = SDL_calloc((size_t)cols * (size_t)rows, sizeof(NvCell));
    for (int r = 0; r < rows && r < nv->rows; ++r) {
        for (int c = 0; c < cols && c < nv->cols; ++c) {
            cells[(size_t)r * (size_t)cols + (size_t)c] = *cell(nv, r, c);
        }
    }
    for (size_t i = 0; i < (size_t)cols * (size_t)rows; ++i) {
        if (cells[i].len == 0 && cells[i].text[0] == 0 && cells[i].hl == 0) {
            cells[i].text[0] = ' ';
            cells[i].len = 1;
        }
    }
    SDL_free(nv->cells);
    SDL_free(nv->row_dirty);
    nv->cells = cells;
    nv->cols = cols;
    nv->rows = rows;
    nv->row_dirty = SDL_calloc((size_t)rows, sizeof(bool));
    for (int r = 0; r < rows; ++r) nv->row_dirty[r] = true;
}

static void grid_clear(Nvim *nv)
{
    for (int r = 0; r < nv->rows; ++r) {
        for (int c = 0; c < nv->cols; ++c) {
            NvCell *x = cell(nv, r, c);
            x->text[0] = ' ';
            x->text[1] = 0;
            x->len = 1;
            x->hl = 0;
        }
        nv->row_dirty[r] = true;
    }
}

static void grid_line(Nvim *nv, const MpObj *a)
{
    int row = (int)mp_int(mp_at(a, 1));
    int col = (int)mp_int(mp_at(a, 2));
    const MpObj *cells = mp_at(a, 3);
    if (row < 0 || row >= nv->rows || !cells) return;
    int hl = 0;
    for (uint32_t i = 0; i < cells->len; ++i) {
        const MpObj *c = mp_at(cells, i);
        const MpObj *text = mp_at(c, 0);
        if (c && c->len >= 2) hl = (int)mp_int(mp_at(c, 1));
        int repeat = c && c->len >= 3 ? (int)mp_int(mp_at(c, 2)) : 1;
        for (int k = 0; k < repeat && col < nv->cols; ++k, ++col) {
            NvCell *x = cell(nv, row, col);
            size_t n = text && text->type == MP_STR ? text->len : 0;
            if (n >= sizeof(x->text)) n = sizeof(x->text) - 1;
            if (n) SDL_memcpy(x->text, text->v.s, n);
            x->text[n] = 0;
            x->len = (uint8_t)n;
            x->hl = hl;
        }
    }
    nv->row_dirty[row] = true;
}

static void grid_scroll(Nvim *nv, const MpObj *a)
{
    int top = (int)mp_int(mp_at(a, 1)), bot = (int)mp_int(mp_at(a, 2));
    int left = (int)mp_int(mp_at(a, 3)), right = (int)mp_int(mp_at(a, 4));
    int rows = (int)mp_int(mp_at(a, 5));
    if (top < 0 || bot > nv->rows || left < 0 || right > nv->cols || rows == 0) return;
    size_t width = (size_t)(right - left) * sizeof(NvCell);
    if (rows > 0) {
        for (int r = top; r < bot - rows; ++r) {
            SDL_memcpy(cell(nv, r, left), cell(nv, r + rows, left), width);
            nv->row_dirty[r] = true;
        }
    } else {
        for (int r = bot - 1; r >= top - rows; --r) {
            SDL_memcpy(cell(nv, r, left), cell(nv, r + rows, left), width);
            nv->row_dirty[r] = true;
        }
    }
}

static void hl_define(Nvim *nv, const MpObj *a)
{
    int id = (int)mp_int(mp_at(a, 0));
    const MpObj *attrs = mp_at(a, 1);
    if (id <= 0 || id > 1 << 20) return;
    if (id >= nv->hl_count) {
        int n = SDL_max(id + 1, nv->hl_count * 2);
        nv->hls = SDL_realloc(nv->hls, sizeof(NvHl) * (size_t)n);
        SDL_memset(nv->hls + nv->hl_count, 0, sizeof(NvHl) * (size_t)(n - nv->hl_count));
        nv->hl_count = n;
    }
    NvHl *h = &nv->hls[id];
    SDL_zerop(h);
    const MpObj *v;
    if ((v = mp_map_get(attrs, "foreground"))) { h->fg = (uint32_t)mp_int(v); h->has_fg = true; }
    if ((v = mp_map_get(attrs, "background"))) { h->bg = (uint32_t)mp_int(v); h->has_bg = true; }
    if ((v = mp_map_get(attrs, "special")))    { h->sp = (uint32_t)mp_int(v); h->has_sp = true; }
    h->bold = mp_int(mp_map_get(attrs, "bold")) != 0;
    h->italic = mp_int(mp_map_get(attrs, "italic")) != 0;
    h->underline = mp_int(mp_map_get(attrs, "underline")) != 0;
    h->undercurl = mp_int(mp_map_get(attrs, "undercurl")) != 0;
    h->strikethrough = mp_int(mp_map_get(attrs, "strikethrough")) != 0;
    h->reverse = mp_int(mp_map_get(attrs, "reverse")) != 0;
    nv->colors_changed = true;
}

static void mode_info_set(Nvim *nv, const MpObj *a)
{
    const MpObj *list = mp_at(a, 1);
    nv->mode_count = 0;
    for (uint32_t i = 0; list && i < list->len && i < 32; ++i) {
        const MpObj *m = mp_at(list, i);
        NvModeInfo *mi = &nv->modes[nv->mode_count++];
        const MpObj *shape = mp_map_get(m, "cursor_shape");
        mi->shape = mp_str_eq(shape, "vertical") ? NV_CURSOR_VERTICAL
                  : mp_str_eq(shape, "horizontal") ? NV_CURSOR_HORIZONTAL : NV_CURSOR_BLOCK;
        const MpObj *pct = mp_map_get(m, "cell_percentage");
        mi->percentage = pct ? (int)mp_int(pct) : 100;
        mi->attr_id = (int)mp_int(mp_map_get(m, "attr_id"));
    }
}

static void handle_redraw(Nvim *nv, const MpObj *events)
{
    for (uint32_t i = 0; events && i < events->len; ++i) {
        const MpObj *ev = mp_at(events, i);
        const MpObj *name = mp_at(ev, 0);
        if (!name) continue;
        for (uint32_t k = 1; k < ev->len; ++k) {
            const MpObj *a = mp_at(ev, k);
            if (mp_str_eq(name, "grid_line")) grid_line(nv, a);
            else if (mp_str_eq(name, "grid_scroll")) grid_scroll(nv, a);
            else if (mp_str_eq(name, "grid_cursor_goto")) {
                int r = (int)mp_int(mp_at(a, 1)), c = (int)mp_int(mp_at(a, 2));
                if (nv->cursor_row < nv->rows) nv->row_dirty[nv->cursor_row] = true;
                nv->cursor_row = r;
                nv->cursor_col = c;
                if (r >= 0 && r < nv->rows) nv->row_dirty[r] = true;
            } else if (mp_str_eq(name, "grid_clear")) grid_clear(nv);
            else if (mp_str_eq(name, "grid_resize")) {
                grid_resize(nv, (int)mp_int(mp_at(a, 1)), (int)mp_int(mp_at(a, 2)));
            } else if (mp_str_eq(name, "hl_attr_define")) hl_define(nv, a);
            else if (mp_str_eq(name, "default_colors_set")) {
                nv->def_fg = (uint32_t)mp_int(mp_at(a, 0));
                nv->def_bg = (uint32_t)mp_int(mp_at(a, 1));
                nv->def_sp = (uint32_t)mp_int(mp_at(a, 2));
                nv->colors_changed = true;
                for (int r = 0; r < nv->rows; ++r) nv->row_dirty[r] = true;
            } else if (mp_str_eq(name, "mode_info_set")) mode_info_set(nv, a);
            else if (mp_str_eq(name, "mode_change")) {
                nv->mode_idx = (int)mp_int(mp_at(a, 1));
                if (nv->cursor_row < nv->rows) nv->row_dirty[nv->cursor_row] = true;
            } else if (mp_str_eq(name, "busy_start")) nv->busy = true;
            else if (mp_str_eq(name, "busy_stop")) nv->busy = false;
            else if (mp_str_eq(name, "flush")) nv->flushed = true;
        }
        if (mp_str_eq(name, "flush") && ev->len == 1) nv->flushed = true;
    }
}

static void handle_response(Nvim *nv, const MpObj *msg)
{
    uint32_t id = (uint32_t)mp_int(mp_at(msg, 1));
    const MpObj *err = mp_at(msg, 2);
    const MpObj *result = mp_at(msg, 3);
    if (id == nv->api_info_request) {
        nv->api_info_request = 0;
        MpBuf b = {0};
        begin_notify(&b, "nvim_exec_lua", 2);
        mp_w_cstr(&b, attach_lua);
        mp_w_array(&b, 1);
        mp_w_int(&b, mp_int(mp_at(result, 0)));
        send(nv, &b);
        return;
    }
    if (id != nv->text_request) return;
    nv->text_request = 0;
    if (err && err->type != MP_NIL) return;
    const MpObj *name = mp_at(result, 0);
    const MpObj *text = mp_at(result, 1);
    SDL_free(nv->buf_name);
    SDL_free(nv->buf_text);
    nv->buf_name = name && name->type == MP_STR ? SDL_strndup(name->v.s, name->len) : SDL_strdup("");
    if (text && text->type == MP_STR) {
        nv->buf_text = SDL_strndup(text->v.s, text->len);
        nv->buf_text_len = text->len;
    } else {
        nv->buf_text = SDL_strdup("");
        nv->buf_text_len = 0;
    }
    nv->buf_ready = true;
    if (nv->text_dirty_again) {
        nv->text_dirty_again = false;
        nv->text_changed = true;
    }
}

bool nvim_process(Nvim *nv)
{
    if (!nv->lock) return false;
    SDL_LockMutex(nv->lock);
    MpMsg *queue = nv->queue;
    int count = nv->queue_len;
    nv->queue = NULL;
    nv->queue_len = nv->queue_cap = 0;
    SDL_UnlockMutex(nv->lock);

    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const MpObj *m = &queue[i].root;
        int64_t type = mp_int(mp_at(m, 0));
        if (type == 2) {
            const MpObj *method = mp_at(m, 1);
            if (mp_str_eq(method, "redraw")) {
                handle_redraw(nv, mp_at(m, 2));
                changed = true;
            } else if (mp_str_eq(method, "md_changed")) {
                nv->text_changed = true;
                changed = true;
            } else if (mp_str_eq(method, "md_scroll")) {
                nv->top_line = (int)mp_int(mp_at(mp_at(m, 2), 0));
                nv->scrolled = true;
                changed = true;
            }
        } else if (type == 1) {
            handle_response(nv, m);
            changed = true;
        }
        mp_msg_free(&queue[i]);
    }
    SDL_free(queue);
    return changed;
}

bool nvim_has_exited(Nvim *nv)
{
    return SDL_GetAtomicInt(&nv->exited) != 0;
}

void nvim_stop(Nvim *nv)
{
    nvim_wait_started(nv);
    if (nv->process) {
        /* The pipes are owned by the process object and closed with it. */
        SDL_KillProcess(nv->process, false);
        SDL_WaitProcess(nv->process, true, NULL);
        if (nv->reader) SDL_WaitThread(nv->reader, NULL);
        SDL_DestroyProcess(nv->process);
    }
    if (nv->lock) {
        for (int i = 0; i < nv->queue_len; ++i) mp_msg_free(&nv->queue[i]);
        SDL_free(nv->queue);
        SDL_DestroyMutex(nv->lock);
    }
    SDL_free(nv->cells);
    SDL_free(nv->row_dirty);
    SDL_free(nv->hls);
    SDL_free(nv->buf_name);
    SDL_free(nv->buf_text);
    SDL_free(nv->config_path);
    SDL_free(nv->error);
    SDL_zerop(nv);
}
