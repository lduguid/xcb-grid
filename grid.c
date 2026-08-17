#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <xcb/xcb.h>

#include "table.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern xcb_connection_t *XGetXCBConnection(Display *dpy);
extern void XSetEventQueueOwner(Display *dpy, int owner);
#ifndef XCBOwnsEventQueue
#define XCBOwnsEventQueue 1
#endif

#define FONT_W 9
#define FONT_H 15
#define ROW_H 22
#define HEADER_H 24
#define TITLE_H 28
#define STATUS_H 22
#define SB 14
#define CELL_PAD 8
#define MIN_COL_W 56
#define WIN_W 960
#define WIN_H 640

enum {
    HIT_NONE = 0,
    HIT_CELL,
    HIT_VBAR,
    HIT_HBAR
};

enum {
    DRAG_NONE = 0,
    DRAG_V,
    DRAG_H
};

static Display *dpy;
static xcb_connection_t *conn;
static xcb_window_t win;
static xcb_screen_t *screen;
static xcb_gcontext_t gc;
static xcb_pixmap_t back;
static xcb_font_t font;
static int win_w = WIN_W;
static int win_h = WIN_H;
static int running = 1;
static int back_w, back_h;
static int back_ready;
static int dirty = 1;

static Table table;
static char **files;
static int nfiles;
static int file_i;

static int scroll_row;
static int scroll_x;
static int sel_row;
static int sel_col;
static int drag;
static int drag_off;

static int row_head_w(void)
{
    int n = table.nrows > 0 ? table.nrows : 1;
    int digits = 1;
    while (n >= 10) {
        n /= 10;
        digits++;
    }
    return digits * FONT_W + CELL_PAD * 2;
}

static int title_h(void) { return TITLE_H; }
static int header_y(void) { return title_h(); }
static int grid_y(void) { return title_h() + HEADER_H; }
static int status_y(void) { return win_h - STATUS_H; }
static int hbar_y(void) { return status_y() - SB; }
static int grid_h(void)
{
    int h = hbar_y() - grid_y();
    return h > ROW_H ? h : ROW_H;
}
static int grid_w(void)
{
    int w = win_w - SB;
    return w > 40 ? w : 40;
}
static int vis_rows(void)
{
    int n = grid_h() / ROW_H;
    return n > 1 ? n : 1;
}

static int col_width(int c)
{
    int w;
    if (c < 0 || c >= table.ncols)
        return MIN_COL_W;
    w = table.cols[c].max_chars * FONT_W + CELL_PAD * 2;
    return w < MIN_COL_W ? MIN_COL_W : w;
}

static int content_w(void)
{
    int w = row_head_w();
    int i;
    for (i = 0; i < table.ncols; i++)
        w += col_width(i);
    return w;
}

static int max_scroll_row(void)
{
    int m = table.nrows - vis_rows();
    return m < 0 ? 0 : m;
}

static int max_scroll_x(void)
{
    int m = content_w() - grid_w();
    return m < 0 ? 0 : m;
}

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static void gc_color(uint32_t pixel)
{
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &pixel);
}

static void fill_rect(int x, int y, int w, int h, uint32_t pixel)
{
    xcb_rectangle_t r = {(int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h};
    gc_color(pixel);
    xcb_poly_fill_rectangle(conn, back, gc, 1, &r);
}

static void draw_line(int x1, int y1, int x2, int y2, uint32_t pixel)
{
    xcb_point_t p[2] = {{(int16_t)x1, (int16_t)y1}, {(int16_t)x2, (int16_t)y2}};
    gc_color(pixel);
    xcb_poly_line(conn, XCB_COORD_MODE_ORIGIN, back, gc, 2, p);
}

static void draw_text(int x, int y, const char *s, uint32_t pixel)
{
    size_t n = strlen(s);
    if (n > 255)
        n = 255;
    gc_color(pixel);
    xcb_image_text_8(conn, (uint8_t)n, back, gc, (int16_t)x, (int16_t)y, s);
}

static void draw_text_clip(int x, int y, int max_w, const char *s, uint32_t pixel)
{
    char buf[256];
    int max_ch = max_w / FONT_W;
    size_t n;

    if (max_ch < 1)
        return;
    if (max_ch > 255)
        max_ch = 255;
    n = strlen(s);
    if ((int)n <= max_ch) {
        draw_text(x, y, s, pixel);
        return;
    }
    memcpy(buf, s, (size_t)max_ch);
    if (max_ch >= 3) {
        buf[max_ch - 3] = '.';
        buf[max_ch - 2] = '.';
        buf[max_ch - 1] = '.';
    }
    buf[max_ch] = '\0';
    draw_text(x, y, buf, pixel);
}

static void mark_dirty(void)
{
    dirty = 1;
}

static void ensure_backbuffer(void)
{
    if (back && back_w == win_w && back_h == win_h)
        return;
    if (back)
        xcb_free_pixmap(conn, back);
    back = xcb_generate_id(conn);
    xcb_create_pixmap(conn, screen->root_depth, back, win, (uint16_t)win_w, (uint16_t)win_h);
    back_w = win_w;
    back_h = win_h;
    back_ready = 0;
}

static void present(void)
{
    if (!back || !back_ready)
        return;
    xcb_copy_area(conn, back, win, gc, 0, 0, 0, 0, (uint16_t)win_w, (uint16_t)win_h);
    xcb_flush(conn);
}

static void clamp_view(void)
{
    if (scroll_row < 0)
        scroll_row = 0;
    if (scroll_row > max_scroll_row())
        scroll_row = max_scroll_row();
    if (scroll_x < 0)
        scroll_x = 0;
    if (scroll_x > max_scroll_x())
        scroll_x = max_scroll_x();
    if (sel_row < 0)
        sel_row = 0;
    if (table.nrows && sel_row >= table.nrows)
        sel_row = table.nrows - 1;
    if (sel_col < 0)
        sel_col = 0;
    if (table.ncols && sel_col >= table.ncols)
        sel_col = table.ncols - 1;
}

static void reveal_selection(void)
{
    int x, c, rh = row_head_w();

    if (sel_row < scroll_row)
        scroll_row = sel_row;
    if (sel_row >= scroll_row + vis_rows())
        scroll_row = sel_row - vis_rows() + 1;

    x = rh;
    for (c = 0; c < sel_col; c++)
        x += col_width(c);
    if (x < scroll_x + rh)
        scroll_x = x - rh;
    if (x + col_width(sel_col) - scroll_x > grid_w())
        scroll_x = x + col_width(sel_col) - grid_w();
    clamp_view();
}

static void vbar_geom(int *track_y, int *track_h, int *thumb_y, int *thumb_h)
{
    int total = table.nrows > 0 ? table.nrows : 1;
    int vis = vis_rows();
    int th;

    *track_y = grid_y();
    *track_h = grid_h();
    th = (int)((long)vis * *track_h / total);
    if (th < 18)
        th = 18;
    if (th > *track_h)
        th = *track_h;
    *thumb_h = th;
    if (max_scroll_row() <= 0)
        *thumb_y = *track_y;
    else
        *thumb_y = *track_y + (int)((long)scroll_row * (*track_h - th) / max_scroll_row());
}

static void hbar_geom(int *track_x, int *track_w, int *thumb_x, int *thumb_w)
{
    int total = content_w();
    int vis = grid_w();
    int tw;

    *track_x = 0;
    *track_w = grid_w();
    tw = total > 0 ? (int)((long)vis * *track_w / total) : *track_w;
    if (tw < 18)
        tw = 18;
    if (tw > *track_w)
        tw = *track_w;
    *thumb_w = tw;
    if (max_scroll_x() <= 0)
        *thumb_x = *track_x;
    else
        *thumb_x = *track_x + (int)((long)scroll_x * (*track_w - tw) / max_scroll_x());
}

static int hit_test(int x, int y, int *out_row, int *out_col)
{
    if (out_row)
        *out_row = -1;
    if (out_col)
        *out_col = -1;

    if (x >= grid_w() && y >= grid_y() && y < hbar_y())
        return HIT_VBAR;
    if (y >= hbar_y() && y < status_y() && x < grid_w())
        return HIT_HBAR;
    if (y >= grid_y() && y < hbar_y() && x >= 0 && x < grid_w()) {
        int row = scroll_row + (y - grid_y()) / ROW_H;
        int cx = -scroll_x + row_head_w();
        int c;
        if (row >= 0 && row < table.nrows && out_row)
            *out_row = row;
        if (x >= row_head_w()) {
            for (c = 0; c < table.ncols; c++) {
                int w = col_width(c);
                if (x >= cx && x < cx + w) {
                    if (out_col)
                        *out_col = c;
                    break;
                }
                cx += w;
            }
        }
        return HIT_CELL;
    }
    return HIT_NONE;
}

static void draw_scrollbars(void)
{
    int ty, th, thy, thh, tx, tw, thx, thw;

    fill_rect(grid_w(), grid_y(), SB, grid_h(), rgb(26, 28, 34));
    vbar_geom(&ty, &th, &thy, &thh);
    fill_rect(grid_w() + 2, thy, SB - 4, thh, rgb(90, 100, 118));

    fill_rect(0, hbar_y(), grid_w(), SB, rgb(26, 28, 34));
    hbar_geom(&tx, &tw, &thx, &thw);
    fill_rect(thx, hbar_y() + 2, thw, SB - 4, rgb(90, 100, 118));
    fill_rect(grid_w(), hbar_y(), SB, SB, rgb(22, 24, 30));
}

static void render_back(void)
{
    const uint32_t bg = rgb(18, 20, 24);
    const uint32_t alt = rgb(24, 27, 34);
    const uint32_t hdr = rgb(40, 48, 62);
    const uint32_t line = rgb(52, 60, 74);
    const uint32_t text = rgb(230, 232, 238);
    const uint32_t htxt = rgb(158, 203, 255);
    const uint32_t num = rgb(180, 190, 205);
    const uint32_t sel = rgb(42, 74, 106);
    const uint32_t title = rgb(14, 16, 20);
    int r, c, x, y, rh, vis, first, last;
    char buf[256];

    ensure_backbuffer();
    fill_rect(0, 0, win_w, win_h, bg);

    snprintf(buf, sizeof(buf), "XCB Grid  %s", table.path ? table.path : "(no file)");
    fill_rect(0, 0, win_w, title_h(), title);
    draw_text(10, 19, buf, text);

    rh = row_head_w();
    vis = vis_rows();
    table_ensure(&table, scroll_row, vis);

    fill_rect(0, header_y(), grid_w(), HEADER_H, hdr);
    draw_text_clip(CELL_PAD, header_y() + 17, rh - CELL_PAD, "#", htxt);
    x = rh - scroll_x;
    for (c = 0; c < table.ncols; c++) {
        int w = col_width(c);
        if (x + w > 0 && x < grid_w()) {
            fill_rect(x, header_y(), w, HEADER_H, hdr);
            draw_text_clip(x + CELL_PAD, header_y() + 17, w - CELL_PAD * 2, table.cols[c].name, htxt);
            draw_line(x, header_y(), x, header_y() + HEADER_H, line);
        }
        x += w;
    }
    draw_line(0, header_y() + HEADER_H - 1, grid_w(), header_y() + HEADER_H - 1, line);

    first = scroll_row;
    last = scroll_row + vis;
    if (last > table.nrows)
        last = table.nrows;

    for (r = first; r < last; r++) {
        y = grid_y() + (r - first) * ROW_H;
        fill_rect(0, y, grid_w(), ROW_H, (r == sel_row) ? sel : ((r & 1) ? alt : bg));
        snprintf(buf, sizeof(buf), "%d", r + 1);
        draw_text_clip(CELL_PAD, y + 16, rh - CELL_PAD, buf, num);
        x = rh - scroll_x;
        for (c = 0; c < table.ncols; c++) {
            int w = col_width(c);
            const char *cell = table_cell(&table, r, c);
            if (x + w > rh && x < grid_w()) {
                if (r == sel_row && c == sel_col)
                    fill_rect(x, y, w, ROW_H, rgb(56, 98, 140));
                if (cell)
                    draw_text_clip(x + CELL_PAD, y + 16, w - CELL_PAD * 2, cell, text);
                draw_line(x, y, x, y + ROW_H, line);
            }
            x += w;
        }
        draw_line(0, y + ROW_H - 1, grid_w(), y + ROW_H - 1, line);
        draw_line(rh, y, rh, y + ROW_H, line);
    }

    draw_scrollbars();

    {
        int b0 = table.buf_start < 0 ? 0 : table.buf_start;
        int b1 = table.buf_start < 0 ? 0 : table.buf_start + table.buf_rows;
        const char *typ = (sel_col >= 0 && sel_col < table.ncols) ? col_type_name(table.cols[sel_col].type) : "-";
        const char *cname = (sel_col >= 0 && sel_col < table.ncols) ? table.cols[sel_col].name : "-";
        fill_rect(0, status_y(), win_w, STATUS_H, rgb(16, 18, 22));
        snprintf(buf, sizeof(buf),
                 "r%d c%d  %s:%s  %d rows  buf %d-%d (%d rows, %.1fKB)  loads=%d  [ ] files",
                 sel_row + 1, sel_col + 1, cname, typ, table.nrows, b0 + (table.nrows ? 1 : 0), b1,
                 table.buf_rows, table.arena_len / 1024.0, table.buf_loads);
        draw_text(8, status_y() + 16, buf, rgb(170, 176, 188));
    }

    back_ready = 1;
}

static void redraw(void)
{
    render_back();
    present();
    dirty = 0;
}

static int load_file(const char *path)
{
    table_close(&table);
    scroll_row = scroll_x = sel_row = sel_col = 0;
    if (!table_open(&table, path)) {
        fprintf(stderr, "cannot open %s\n", path);
        return 0;
    }
    {
        int i, w;
        for (i = 0; i < table.ncols; i++) {
            w = table.cols[i].max_chars * FONT_W + CELL_PAD * 2;
            table.cols[i].width_px = w < MIN_COL_W ? MIN_COL_W : w;
        }
    }
    table_ensure(&table, 0, vis_rows());
    mark_dirty();
    return 1;
}

static int is_data_file(const char *name)
{
    const char *ext = strrchr(name, '.');
    if (!ext)
        return 0;
    return !strcasecmp(ext, ".tsv") || !strcasecmp(ext, ".csv") || !strcasecmp(ext, ".tab");
}

static int cmpstr(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void add_file(const char *path)
{
    char **n = realloc(files, (size_t)(nfiles + 1) * sizeof(char *));
    if (!n)
        return;
    files = n;
    files[nfiles] = strdup(path);
    if (files[nfiles])
        nfiles++;
}

static void add_path(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *de;
        if (!d)
            return;
        while ((de = readdir(d))) {
            char buf[1024];
            if (de->d_name[0] == '.')
                continue;
            if (!is_data_file(de->d_name))
                continue;
            snprintf(buf, sizeof(buf), "%s/%s", path, de->d_name);
            add_file(buf);
        }
        closedir(d);
        qsort(files, (size_t)nfiles, sizeof(char *), cmpstr);
    } else {
        add_file(path);
    }
}

static void switch_file(int delta)
{
    if (nfiles < 1)
        return;
    file_i = (file_i + delta) % nfiles;
    if (file_i < 0)
        file_i += nfiles;
    load_file(files[file_i]);
}

static void page(int dir)
{
    sel_row += dir * vis_rows();
    clamp_view();
    reveal_selection();
}

static void on_key(KeySym ks, uint16_t state)
{
    int ctrl = state & ControlMask;

    if (ks == XK_Escape || ks == XK_q)
        running = 0;
    else if (ks == XK_bracketleft)
        switch_file(-1);
    else if (ks == XK_bracketright)
        switch_file(1);
    else if (ks == XK_Up || ks == XK_KP_Up)
        sel_row--;
    else if (ks == XK_Down || ks == XK_KP_Down)
        sel_row++;
    else if (ks == XK_Left || ks == XK_KP_Left)
        sel_col--;
    else if (ks == XK_Right || ks == XK_KP_Right)
        sel_col++;
    else if (ks == XK_Page_Up || ks == XK_KP_Page_Up)
        page(-1);
    else if (ks == XK_Page_Down || ks == XK_KP_Page_Down)
        page(1);
    else if (ks == XK_Home || ks == XK_KP_Home) {
        sel_row = 0;
        if (ctrl)
            sel_col = 0;
    } else if (ks == XK_End || ks == XK_KP_End) {
        sel_row = table.nrows ? table.nrows - 1 : 0;
        if (ctrl)
            sel_col = table.ncols ? table.ncols - 1 : 0;
    } else
        return;
    clamp_view();
    reveal_selection();
    mark_dirty();
}

static void on_wheel(int btn, int shift)
{
    if (btn == 6 || (btn == 4 && shift))
        scroll_x -= 40;
    else if (btn == 7 || (btn == 5 && shift))
        scroll_x += 40;
    else if (btn == 4)
        scroll_row -= 3;
    else if (btn == 5)
        scroll_row += 3;
    clamp_view();
    mark_dirty();
}

static void on_press(int x, int y, int btn, int state)
{
    int row = -1, col = -1;
    int hit;

    if (btn == 4 || btn == 5 || btn == 6 || btn == 7) {
        on_wheel(btn, state & (ShiftMask | ControlMask));
        return;
    }
    if (btn != 1)
        return;

    xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, win, XCB_CURRENT_TIME);
    hit = hit_test(x, y, &row, &col);
    if (hit == HIT_CELL) {
        if (row >= 0)
            sel_row = row;
        if (col >= 0)
            sel_col = col;
        clamp_view();
    } else if (hit == HIT_VBAR) {
        int ty, th, thy, thh;
        vbar_geom(&ty, &th, &thy, &thh);
        if (y < thy)
            page(-1);
        else if (y > thy + thh)
            page(1);
        else {
            drag = DRAG_V;
            drag_off = y - thy;
        }
    } else if (hit == HIT_HBAR) {
        int tx, tw, thx, thw;
        hbar_geom(&tx, &tw, &thx, &thw);
        if (x < thx)
            scroll_x -= grid_w() / 2;
        else if (x > thx + thw)
            scroll_x += grid_w() / 2;
        else {
            drag = DRAG_H;
            drag_off = x - thx;
        }
        clamp_view();
    }
    mark_dirty();
}

static void on_motion(int x, int y)
{
    if (drag == DRAG_V) {
        int ty, th, thy, thh, span;
        vbar_geom(&ty, &th, &thy, &thh);
        span = th - thh;
        if (span < 1)
            return;
        scroll_row = (int)((long)(y - drag_off - ty) * max_scroll_row() / span);
        clamp_view();
        mark_dirty();
    } else if (drag == DRAG_H) {
        int tx, tw, thx, thw, span;
        hbar_geom(&tx, &tw, &thx, &thw);
        span = tw - thw;
        if (span < 1)
            return;
        scroll_x = (int)((long)(x - drag_off - tx) * max_scroll_x() / span);
        clamp_view();
        mark_dirty();
    }
}

static int open_font(void)
{
    const char *names[] = {"9x15bold", "9x15", "fixed", NULL};
    int i;
    font = xcb_generate_id(conn);
    for (i = 0; names[i]; i++) {
        xcb_void_cookie_t c = xcb_open_font_checked(conn, font, (uint16_t)strlen(names[i]), names[i]);
        xcb_generic_error_t *e = xcb_request_check(conn, c);
        if (!e)
            return 1;
        free(e);
    }
    return 0;
}

int main(int argc, char **argv)
{
    xcb_intern_atom_reply_t *proto = NULL, *del = NULL;
    int fd, i;

    if (argc < 2) {
        fprintf(stderr, "usage: %s FILE|DIR [FILE...]\n", argv[0]);
        return 1;
    }
    for (i = 1; i < argc; i++)
        add_path(argv[i]);
    if (nfiles < 1) {
        fprintf(stderr, "no .tsv/.csv files found\n");
        return 1;
    }

    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "cannot open X display\n");
        return 1;
    }
    conn = XGetXCBConnection(dpy);
    if (!conn || xcb_connection_has_error(conn)) {
        fprintf(stderr, "cannot get XCB connection\n");
        return 1;
    }
    XSetEventQueueOwner(dpy, XCBOwnsEventQueue);
    XkbSetDetectableAutoRepeat(dpy, True, NULL);

    screen = xcb_setup_roots_iterator(xcb_get_setup(conn)).data;
    win = xcb_generate_id(conn);
    gc = xcb_generate_id(conn);

    {
        uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_BIT_GRAVITY | XCB_CW_EVENT_MASK;
        uint32_t values[] = {
            XCB_BACK_PIXMAP_NONE,
            XCB_GRAVITY_STATIC,
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY,
        };
        xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, screen->root, 80, 60, WIN_W, WIN_H, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, mask, values);
    }

    {
        const char *title = "XCB Grid";
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                            (uint32_t)strlen(title), title);
    }

    /* ICCCM size hints: min size only, no max — the WM may resize freely. */
    {
        enum {
            HINT_P_SIZE = 8,
            HINT_P_MIN_SIZE = 16,
            HINT_P_BASE_SIZE = 256,
            HINT_P_WIN_GRAVITY = 512
        };
        struct {
            uint32_t flags;
            int32_t x, y, width, height;
            int32_t min_width, min_height;
            int32_t max_width, max_height;
            int32_t width_inc, height_inc;
            int32_t min_aspect_num, min_aspect_den;
            int32_t max_aspect_num, max_aspect_den;
            int32_t base_width, base_height;
            uint32_t win_gravity;
        } hints;
        xcb_intern_atom_cookie_t nh = xcb_intern_atom(conn, 0, 15, "WM_NORMAL_HINTS");
        xcb_intern_atom_cookie_t sh = xcb_intern_atom(conn, 0, 13, "WM_SIZE_HINTS");
        xcb_intern_atom_reply_t *normal = xcb_intern_atom_reply(conn, nh, NULL);
        xcb_intern_atom_reply_t *size = xcb_intern_atom_reply(conn, sh, NULL);

        memset(&hints, 0, sizeof(hints));
        hints.flags = HINT_P_SIZE | HINT_P_MIN_SIZE | HINT_P_BASE_SIZE | HINT_P_WIN_GRAVITY;
        hints.width = WIN_W;
        hints.height = WIN_H;
        hints.min_width = 400;
        hints.min_height = 280;
        hints.base_width = 400;
        hints.base_height = 280;
        hints.win_gravity = XCB_GRAVITY_NORTH_WEST;
        if (normal && size)
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, normal->atom, size->atom, 32,
                                sizeof(hints) / 4, &hints);
        free(normal);
        free(size);
    }

    {
        xcb_intern_atom_cookie_t ac = xcb_intern_atom(conn, 0, 24, "_NET_WM_ALLOWED_ACTIONS");
        xcb_intern_atom_cookie_t c[6];
        xcb_intern_atom_reply_t *allowed, *acts[6];
        xcb_atom_t atoms[6];
        int i;
        static const char *names[] = {
            "_NET_WM_ACTION_MOVE", "_NET_WM_ACTION_RESIZE", "_NET_WM_ACTION_MINIMIZE",
            "_NET_WM_ACTION_MAXIMIZE_HORZ", "_NET_WM_ACTION_MAXIMIZE_VERT", "_NET_WM_ACTION_CLOSE",
        };
        static const int lens[] = {19, 21, 22, 27, 27, 20};

        allowed = xcb_intern_atom_reply(conn, ac, NULL);
        for (i = 0; i < 6; i++)
            c[i] = xcb_intern_atom(conn, 0, (uint16_t)lens[i], names[i]);
        for (i = 0; i < 6; i++) {
            acts[i] = xcb_intern_atom_reply(conn, c[i], NULL);
            atoms[i] = acts[i] ? acts[i]->atom : XCB_ATOM_NONE;
        }
        if (allowed)
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, allowed->atom, XCB_ATOM_ATOM, 32, 6,
                                atoms);
        free(allowed);
        for (i = 0; i < 6; i++)
            free(acts[i]);
    }

    {
        xcb_intern_atom_cookie_t proto_c = xcb_intern_atom(conn, 1, 12, "WM_PROTOCOLS");
        xcb_intern_atom_cookie_t del_c = xcb_intern_atom(conn, 0, 16, "WM_DELETE_WINDOW");
        proto = xcb_intern_atom_reply(conn, proto_c, NULL);
        del = xcb_intern_atom_reply(conn, del_c, NULL);
        if (proto && del)
            xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, proto->atom, XCB_ATOM_ATOM, 32, 1, &del->atom);
    }

    if (!open_font()) {
        fprintf(stderr, "cannot open X font\n");
        return 1;
    }
    {
        uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_FONT | XCB_GC_GRAPHICS_EXPOSURES;
        uint32_t values[] = {screen->white_pixel, screen->black_pixel, font, 0};
        xcb_create_gc(conn, gc, win, mask, values);
    }

    if (!load_file(files[0]))
        return 1;
    file_i = 0;

    xcb_map_window(conn, win);
    xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, win, XCB_CURRENT_TIME);
    xcb_flush(conn);

    fd = xcb_get_file_descriptor(conn);
    while (running) {
        fd_set fds;
        struct timeval tv = {0, 16000};
        xcb_generic_event_t *ev;
        int n;

        if (dirty)
            redraw();

        {
            int b0 = table.buf_start, br = table.buf_rows, bl = table.buf_loads;
            table_prefetch(&table, scroll_row, vis_rows(), 128);
            if (table.buf_start != b0 || table.buf_rows != br || table.buf_loads != bl)
                mark_dirty();
        }

        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        n = select(fd + 1, &fds, NULL, NULL, &tv);
        if (n <= 0)
            continue;

        while ((ev = xcb_poll_for_event(conn))) {
            switch (ev->response_type & ~0x80) {
            case XCB_EXPOSE: {
                xcb_expose_event_t *ex = (xcb_expose_event_t *)ev;
                if (ex->count == 0) {
                    if (back_ready)
                        present();
                    else
                        mark_dirty();
                }
                break;
            }
            case XCB_MAP_NOTIFY:
                xcb_set_input_focus(conn, XCB_INPUT_FOCUS_POINTER_ROOT, win, XCB_CURRENT_TIME);
                mark_dirty();
                break;
            case XCB_CONFIGURE_NOTIFY: {
                xcb_configure_notify_event_t *cfg = (xcb_configure_notify_event_t *)ev;
                if (cfg->width > 0 && cfg->height > 0 &&
                    (cfg->width != win_w || cfg->height != win_h)) {
                    win_w = cfg->width;
                    win_h = cfg->height;
                    clamp_view();
                    mark_dirty();
                }
                break;
            }
            case XCB_BUTTON_PRESS: {
                xcb_button_press_event_t *bp = (xcb_button_press_event_t *)ev;
                on_press(bp->event_x, bp->event_y, bp->detail, bp->state);
                break;
            }
            case XCB_BUTTON_RELEASE:
                drag = DRAG_NONE;
                break;
            case XCB_MOTION_NOTIFY: {
                xcb_motion_notify_event_t *mv = (xcb_motion_notify_event_t *)ev;
                if (drag)
                    on_motion(mv->event_x, mv->event_y);
                break;
            }
            case XCB_KEY_PRESS: {
                xcb_key_press_event_t *kp = (xcb_key_press_event_t *)ev;
                on_key(XkbKeycodeToKeysym(dpy, kp->detail, 0, 0), kp->state);
                break;
            }
            case XCB_CLIENT_MESSAGE: {
                xcb_client_message_event_t *cm = (xcb_client_message_event_t *)ev;
                if (del && cm->data.data32[0] == del->atom)
                    running = 0;
                break;
            }
            case XCB_DESTROY_NOTIFY:
                running = 0;
                break;
            default:
                break;
            }
            free(ev);
        }
    }

    table_close(&table);
    for (i = 0; i < nfiles; i++)
        free(files[i]);
    free(files);
    if (back)
        xcb_free_pixmap(conn, back);
    xcb_free_gc(conn, gc);
    xcb_close_font(conn, font);
    free(proto);
    free(del);
    XCloseDisplay(dpy);
    return 0;
}
