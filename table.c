#define _POSIX_C_SOURCE 200809L

#include "table.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define BUF_PAD_MIN 512
#define BUF_PAD_SCREENS 4
#define PREFETCH_MIN 128
#define PREFETCH_SCREENS 2
#define CHUNK_MIN 256
#define KEEP_SCREENS 8
#define ARENA_START (64 * 1024)
#define SCHEMA_SCAN_ROWS 25000

static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }

static void table_reset(Table *t)
{
    memset(t, 0, sizeof(*t));
    t->buf_start = -1;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

const char *col_type_name(ColType type)
{
    switch (type) {
    case COL_INT:
        return "int";
    case COL_FLOAT:
        return "float";
    case COL_STRING:
        return "string";
    default:
        return "empty";
    }
}

static int looks_int(const char *s)
{
    if (!s || !*s)
        return 1;
    if (*s == '+' || *s == '-')
        s++;
    if (!*s)
        return 0;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s))
            return 0;
    }
    return 1;
}

static int looks_float(const char *s)
{
    int digits = 0, dots = 0, exp = 0;

    if (!s || !*s)
        return 1;
    if (*s == '+' || *s == '-')
        s++;
    for (; *s; s++) {
        if (isdigit((unsigned char)*s)) {
            digits++;
        } else if (*s == '.' && !dots && !exp) {
            dots++;
        } else if ((*s == 'e' || *s == 'E') && !exp && digits) {
            exp++;
            if (s[1] == '+' || s[1] == '-')
                s++;
            if (!s[1] || !isdigit((unsigned char)s[1]))
                return 0;
        } else {
            return 0;
        }
    }
    return digits > 0 && (dots || exp);
}

static ColType classify_value(const char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return COL_EMPTY;
    if (looks_int(s))
        return COL_INT;
    if (looks_float(s))
        return COL_FLOAT;
    return COL_STRING;
}

static ColType widen_type(ColType cur, ColType next)
{
    if (next == COL_EMPTY)
        return cur;
    if (cur == COL_EMPTY)
        return next;
    if (cur == COL_STRING || next == COL_STRING)
        return COL_STRING;
    if (cur == COL_FLOAT || next == COL_FLOAT)
        return COL_FLOAT;
    return COL_INT;
}

static char detect_delim(const char *path, const char *line)
{
    const char *ext = strrchr(path, '.');
    int tabs = 0, commas = 0, in_q = 0;

    if (ext && !strcasecmp(ext, ".tsv"))
        return '\t';
    if (ext && !strcasecmp(ext, ".tab"))
        return '\t';
    if (ext && !strcasecmp(ext, ".csv"))
        return ',';

    for (const char *p = line; *p && *p != '\n' && *p != '\r'; p++) {
        if (*p == '"') {
            in_q = !in_q;
            continue;
        }
        if (!in_q && *p == '\t')
            tabs++;
        if (!in_q && *p == ',')
            commas++;
    }
    return tabs >= commas ? '\t' : ',';
}

/* Split one record into *count fields stored in arena. Returns 0 on OOM. */
static int split_record(const char *line, char delim, char *arena, size_t *used, size_t cap,
                        const char **fields, int max_fields, int *count)
{
    const char *p = line;
    int n = 0;

    *count = 0;
    while (n < max_fields) {
        size_t start = *used;
        int quoted = 0;

        if (*p == '"') {
            quoted = 1;
            p++;
        }

        while (*p && *p != '\n' && *p != '\r') {
            if (quoted) {
                if (*p == '"') {
                    if (p[1] == '"') {
                        if (*used + 1 >= cap)
                            return 0;
                        arena[(*used)++] = '"';
                        p += 2;
                        continue;
                    }
                    p++;
                    quoted = 0;
                    continue;
                }
            } else if (*p == delim) {
                break;
            }
            if (*used + 1 >= cap)
                return 0;
            arena[(*used)++] = *p++;
        }

        if (*used + 1 >= cap)
            return 0;
        arena[(*used)++] = '\0';
        fields[n++] = arena + start;

        if (*p == delim) {
            p++;
            continue;
        }
        break;
    }
    *count = n;
    return 1;
}

static void arena_rebase(Table *t, uintptr_t old_addr, char *neu)
{
    size_t i, n;

    if (!old_addr || !neu || (uintptr_t)neu == old_addr || !t->cells || t->buf_rows <= 0)
        return;
    n = (size_t)t->buf_rows * (size_t)t->ncols;
    for (i = 0; i < n; i++) {
        uintptr_t p = (uintptr_t)t->cells[i];
        if (p >= old_addr && p < old_addr + t->arena_len)
            t->cells[i] = neu + (p - old_addr);
    }
}

static int arena_grow(Table *t, size_t need)
{
    size_t cap = t->arena_cap ? t->arena_cap : ARENA_START;
    uintptr_t old_addr = (uintptr_t)t->arena;
    char *p;

    while (cap < need)
        cap *= 2;
    p = realloc(t->arena, cap);
    if (!p)
        return 0;
    if ((uintptr_t)p != old_addr)
        arena_rebase(t, old_addr, p);
    t->arena = p;
    t->arena_cap = cap;
    return 1;
}

static void free_buffer(Table *t)
{
    free(t->arena);
    free(t->cells);
    t->arena = NULL;
    t->cells = NULL;
    t->arena_len = t->arena_cap = 0;
    t->buf_start = -1;
    t->buf_rows = 0;
}

static int parse_row(Table *t, int abs_row, const char **dest, char **line, size_t *linesz,
                     const char **tmp, int max_fields)
{
    ssize_t nread;
    int got = 0, c;
    size_t save;

    if (abs_row < 0 || abs_row >= t->nrows)
        return 0;
    if (fseeko(t->fp, t->offs[abs_row], SEEK_SET) != 0)
        return 0;
    nread = getline(line, linesz, t->fp);
    if (nread < 0)
        return 0;
    if (t->arena_len + (size_t)nread + (size_t)t->ncols + 16 > t->arena_cap) {
        if (!arena_grow(t, t->arena_len + (size_t)nread + 4096))
            return 0;
    }
    save = t->arena_len;
    if (!split_record(*line, t->delim, t->arena, &t->arena_len, t->arena_cap, tmp, max_fields, &got)) {
        t->arena_len = save;
        if (!arena_grow(t, t->arena_cap * 2))
            return 0;
        if (!split_record(*line, t->delim, t->arena, &t->arena_len, t->arena_cap, tmp, max_fields, &got))
            return 0;
    }
    for (c = 0; c < t->ncols; c++)
        dest[c] = (c < got && tmp[c]) ? tmp[c] : "";
    return 1;
}

static int reserve_cells(Table *t, int rows)
{
    const char **p;

    if (rows < 1)
        rows = 1;
    p = realloc(t->cells, (size_t)rows * (size_t)t->ncols * sizeof(*p));
    if (!p)
        return 0;
    t->cells = p;
    return 1;
}

static int append_rows(Table *t, int n)
{
    const int max_fields = t->ncols + 8;
    const char **tmp;
    char *line = NULL;
    size_t linesz = 0;
    int r, old = t->buf_rows;

    if (n < 1 || t->buf_start + t->buf_rows >= t->nrows)
        return 1;
    n = imin(n, t->nrows - (t->buf_start + t->buf_rows));
    tmp = calloc((size_t)max_fields, sizeof(*tmp));
    if (!tmp || !reserve_cells(t, old + n)) {
        free(tmp);
        return 0;
    }
    for (r = 0; r < n; r++) {
        if (!parse_row(t, t->buf_start + old + r, t->cells + (size_t)(old + r) * t->ncols, &line,
                       &linesz, tmp, max_fields))
            break;
        t->buf_rows = old + r + 1;
    }
    free(line);
    free(tmp);
    if (r > 0)
        t->buf_loads++;
    return r > 0 || n == 0;
}

static int prepend_rows(Table *t, int n)
{
    const int max_fields = t->ncols + 8;
    const char **tmp;
    char *line = NULL;
    size_t linesz = 0;
    int r, old = t->buf_rows, add;

    if (n < 1 || t->buf_start <= 0)
        return 1;
    add = imin(n, t->buf_start);
    tmp = calloc((size_t)max_fields, sizeof(*tmp));
    if (!tmp || !reserve_cells(t, old + add)) {
        free(tmp);
        return 0;
    }
    memmove(t->cells + (size_t)add * t->ncols, t->cells, (size_t)old * t->ncols * sizeof(*t->cells));
    memset(t->cells, 0, (size_t)add * t->ncols * sizeof(*t->cells));
    t->buf_start -= add;
    t->buf_rows = old + add;
    for (r = 0; r < add; r++) {
        if (!parse_row(t, t->buf_start + r, t->cells + (size_t)r * t->ncols, &line, &linesz, tmp,
                       max_fields)) {
            t->buf_start += r;
            memmove(t->cells, t->cells + (size_t)r * t->ncols,
                    (size_t)(old + add - r) * t->ncols * sizeof(*t->cells));
            t->buf_rows = old + add - r;
            break;
        }
    }
    free(line);
    free(tmp);
    if (r > 0)
        t->buf_loads++;
    return 1;
}

static void trim_front(Table *t, int n)
{
    if (n < 1 || n >= t->buf_rows)
        return;
    memmove(t->cells, t->cells + (size_t)n * t->ncols,
            (size_t)(t->buf_rows - n) * t->ncols * sizeof(*t->cells));
    t->buf_start += n;
    t->buf_rows -= n;
}

static void trim_back(Table *t, int n)
{
    if (n < 1 || n >= t->buf_rows)
        return;
    t->buf_rows -= n;
}

static int compact_arena(Table *t)
{
    char *fresh;
    size_t cap, used = 0;
    int r, c;

    if (t->buf_rows <= 0)
        return 1;
    cap = t->arena_len ? t->arena_len : ARENA_START;
    fresh = malloc(cap);
    if (!fresh)
        return 0;
    for (r = 0; r < t->buf_rows; r++) {
        for (c = 0; c < t->ncols; c++) {
            const char *s = t->cells[r * t->ncols + c];
            size_t n = s ? strlen(s) + 1 : 1;
            if (used + n > cap) {
                char *p;
                cap = (used + n) * 2;
                p = realloc(fresh, cap);
                if (!p) {
                    free(fresh);
                    return 0;
                }
                fresh = p;
            }
            if (!s)
                s = "";
            memcpy(fresh + used, s, n);
            t->cells[r * t->ncols + c] = fresh + used;
            used += n;
        }
    }
    free(t->arena);
    t->arena = fresh;
    t->arena_len = used;
    t->arena_cap = cap;
    return 1;
}

static void buf_limits(int vis_rows, int *pad, int *margin, int *chunk, int *keep)
{
    if (vis_rows < 1)
        vis_rows = 1;
    *pad = imax(BUF_PAD_MIN, vis_rows * BUF_PAD_SCREENS);
    *margin = imax(PREFETCH_MIN, vis_rows * PREFETCH_SCREENS);
    *chunk = imax(CHUNK_MIN, vis_rows * 2);
    *keep = imax(*pad * 2, vis_rows * KEEP_SCREENS);
}

static int load_window(Table *t, int start, int rows)
{
    int got;

    free_buffer(t);
    if (rows <= 0 || start >= t->nrows)
        return 1;
    if (start < 0)
        start = 0;
    if (start + rows > t->nrows)
        rows = t->nrows - start;
    if (!arena_grow(t, ARENA_START))
        return 0;
    t->arena_len = 0;
    t->buf_start = start;
    t->buf_rows = 0;
    got = append_rows(t, rows);
    return got;
}

static int maintain_buffer(Table *t, int vis_row0, int vis_rows, int budget, int force_visible)
{
    int pad, margin, chunk, keep;
    int vis1, buf_end, above, below, dir;

    if (!t->fp || t->nrows <= 0)
        return 1;
    if (vis_rows < 1)
        vis_rows = 1;
    if (vis_row0 < 0)
        vis_row0 = 0;
    if (vis_row0 >= t->nrows)
        vis_row0 = t->nrows - 1;
    vis1 = vis_row0 + vis_rows;
    if (vis1 > t->nrows)
        vis1 = t->nrows;

    buf_limits(vis_rows, &pad, &margin, &chunk, &keep);
    if (budget < 1)
        budget = chunk;

    dir = vis_row0 - t->last_vis0;
    t->last_vis0 = vis_row0;

    if (t->buf_start < 0 || vis_row0 < t->buf_start || vis1 > t->buf_start + t->buf_rows) {
        int start = vis_row0 - pad;
        int end = vis1 + pad;
        if (start < 0)
            start = 0;
        if (end > t->nrows)
            end = t->nrows;
        return load_window(t, start, end - start);
    }

    if (!force_visible && budget <= 0)
        return 1;

    buf_end = t->buf_start + t->buf_rows;
    above = vis_row0 - t->buf_start;
    below = buf_end - vis1;

    if (dir >= 0) {
        if (below < margin && buf_end < t->nrows && budget > 0) {
            int need = imin(chunk, imin(budget, imax(1, pad - below)));
            if (append_rows(t, need))
                budget -= need;
        }
        if (above < margin && t->buf_start > 0 && budget > 0) {
            int need = imin(chunk, imin(budget, imax(1, pad - above)));
            prepend_rows(t, need);
        }
    } else {
        if (above < margin && t->buf_start > 0 && budget > 0) {
            int need = imin(chunk, imin(budget, imax(1, pad - above)));
            if (prepend_rows(t, need))
                budget -= need;
        }
        if (below < margin && buf_end < t->nrows && budget > 0) {
            int need = imin(chunk, imin(budget, imax(1, pad - below)));
            append_rows(t, need);
        }
    }

    buf_end = t->buf_start + t->buf_rows;
    above = vis_row0 - t->buf_start;
    below = buf_end - vis1;
    if (below > keep)
        trim_back(t, below - keep);
    if (above > keep)
        trim_front(t, above - keep);
    if (t->arena_len > 512 * 1024 && t->buf_rows > 0 &&
        t->arena_len / (size_t)t->buf_rows > 256)
        compact_arena(t);
    return 1;
}

int table_ensure(Table *t, int vis_row0, int vis_rows)
{
    return maintain_buffer(t, vis_row0, vis_rows, 96, 1);
}

int table_prefetch(Table *t, int vis_row0, int vis_rows, int budget)
{
    return maintain_buffer(t, vis_row0, vis_rows, budget, 0);
}

const char *table_cell(const Table *t, int row, int col)
{
    int local;

    if (row < 0 || col < 0 || col >= t->ncols)
        return "";
    if (t->buf_start < 0 || row < t->buf_start || row >= t->buf_start + t->buf_rows)
        return NULL;
    local = row - t->buf_start;
    return t->cells[local * t->ncols + col] ? t->cells[local * t->ncols + col] : "";
}

static int scan_file(Table *t)
{
    char *line = NULL;
    size_t linesz = 0;
    ssize_t nread;
    off_t pos;
    const int max_fields = 256;
    const char **fields = calloc((size_t)max_fields, sizeof(char *));
    char *scratch = malloc(8192);
    int header_n = 0, i;
    size_t off_cap = 256;

    if (!fields || !scratch) {
        free(fields);
        free(scratch);
        return 0;
    }

    pos = ftello(t->fp);
    nread = getline(&line, &linesz, t->fp);
    if (nread < 0) {
        free(line);
        free(fields);
        free(scratch);
        return 0;
    }

    t->delim = detect_delim(t->path, line);
    {
        size_t used = 0;
        if (!split_record(line, t->delim, scratch, &used, 8192, fields, max_fields, &header_n) || header_n < 1) {
            free(line);
            free(fields);
            free(scratch);
            return 0;
        }
    }

    t->ncols = header_n;
    t->cols = calloc((size_t)t->ncols, sizeof(Column));
    if (!t->cols) {
        free(line);
        free(fields);
        free(scratch);
        return 0;
    }
    for (i = 0; i < t->ncols; i++) {
        t->cols[i].name = xstrdup(fields[i] && fields[i][0] ? fields[i] : "col");
        t->cols[i].type = COL_EMPTY;
        t->cols[i].max_chars = (int)strlen(t->cols[i].name);
    }

    t->offs = malloc(off_cap * sizeof(off_t));
    if (!t->offs) {
        free(line);
        free(fields);
        free(scratch);
        return 0;
    }

    while (1) {
        size_t used = 0;
        int got = 0;
        char *big = NULL;

        pos = ftello(t->fp);
        nread = getline(&line, &linesz, t->fp);
        if (nread < 0)
            break;
        if (nread == 0 || (nread == 1 && (line[0] == '\n' || line[0] == '\r')))
            continue;

        if ((size_t)t->nrows >= off_cap) {
            off_t *noffs;
            off_cap *= 2;
            noffs = realloc(t->offs, off_cap * sizeof(off_t));
            if (!noffs) {
                free(line);
                free(fields);
                free(scratch);
                return 0;
            }
            t->offs = noffs;
        }
        t->offs[t->nrows++] = pos;

        /* After the schema sample, only keep line offsets — not cell text. */
        if (t->nrows > SCHEMA_SCAN_ROWS)
            continue;

        if ((size_t)nread + 16 > 8192) {
            size_t cap = (size_t)nread + 64;
            size_t u = 0;
            big = malloc(cap);
            if (!big || !split_record(line, t->delim, big, &u, cap, fields, max_fields, &got)) {
                free(big);
                continue;
            }
        } else if (!split_record(line, t->delim, scratch, &used, 8192, fields, max_fields, &got)) {
            continue;
        }

        if (got > t->ncols) {
            Column *ncols = realloc(t->cols, (size_t)got * sizeof(Column));
            if (ncols) {
                int c;
                t->cols = ncols;
                for (c = t->ncols; c < got; c++) {
                    char name[32];
                    snprintf(name, sizeof(name), "col%d", c + 1);
                    t->cols[c].name = xstrdup(name);
                    t->cols[c].type = COL_EMPTY;
                    t->cols[c].max_chars = (int)strlen(name);
                }
                t->ncols = got;
            }
        }

        for (i = 0; i < t->ncols && i < got; i++) {
            int len = (int)strlen(fields[i]);
            if (len > t->cols[i].max_chars)
                t->cols[i].max_chars = len;
            t->cols[i].type = widen_type(t->cols[i].type, classify_value(fields[i]));
        }
        free(big);
    }

    for (i = 0; i < t->ncols; i++) {
        if (t->cols[i].type == COL_EMPTY)
            t->cols[i].type = COL_STRING;
        if (t->cols[i].max_chars < 4)
            t->cols[i].max_chars = 4;
        if (t->cols[i].max_chars > 48)
            t->cols[i].max_chars = 48;
    }

    free(line);
    free(fields);
    free(scratch);
    (void)pos;
    return 1;
}

int table_open(Table *t, const char *path)
{
    table_reset(t);
    t->path = xstrdup(path);
    t->fp = fopen(path, "rb");
    t->buf_start = -1;
    if (!t->path || !t->fp)
        return 0;
    if (!scan_file(t))
        return 0;
    return 1;
}

void table_close(Table *t)
{
    int i;

    if (!t)
        return;
    if (t->fp)
        fclose(t->fp);
    free_buffer(t);
    if (t->cols) {
        for (i = 0; i < t->ncols; i++)
            free(t->cols[i].name);
    }
    free(t->cols);
    free(t->offs);
    free(t->path);
    table_reset(t);
}
