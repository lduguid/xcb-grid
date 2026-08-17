#ifndef TABLE_H
#define TABLE_H

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

typedef enum {
    COL_EMPTY = 0,
    COL_INT,
    COL_FLOAT,
    COL_STRING
} ColType;

typedef struct {
    char *name;
    ColType type;
    int max_chars;
    int width_px;
} Column;

typedef struct {
    char *path;
    char delim;
    int ncols;
    int nrows;
    Column *cols;
    off_t *offs;
    FILE *fp;

    int buf_start;
    int buf_rows;
    int buf_loads;
    int last_vis0;
    char *arena;
    size_t arena_len;
    size_t arena_cap;
    const char **cells;
} Table;

int table_open(Table *t, const char *path);
void table_close(Table *t);
int table_ensure(Table *t, int vis_row0, int vis_rows);
int table_prefetch(Table *t, int vis_row0, int vis_rows, int budget);
const char *table_cell(const Table *t, int row, int col);
const char *col_type_name(ColType type);

#endif
