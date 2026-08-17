#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "table.h"

static int fail;

static void expect(int ok, const char *msg)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", msg);
        fail++;
    }
}

int main(void)
{
    Table t;
    const char *cell;

    expect(table_open(&t, "/home/luke/xcb-grid/testdata/people.tsv"), "open people");
    expect(t.ncols == 5, "people cols");
    expect(t.nrows == 15, "people rows");
    expect(t.delim == '\t', "people delim");
    expect(t.cols[1].type == COL_INT, "age int");
    expect(t.cols[4].type == COL_FLOAT, "score float");
    expect(t.cols[0].type == COL_STRING, "name string");
    expect(table_ensure(&t, 0, 10), "people buffer");
    cell = table_cell(&t, 0, 0);
    expect(cell && !strcmp(cell, "Alice Nguyen"), "first name");
    table_close(&t);

    expect(table_open(&t, "/home/luke/xcb-grid/testdata/sales.csv"), "open sales");
    expect(t.delim == ',', "sales delim");
    expect(t.cols[0].type == COL_INT, "order_id int");
    expect(t.cols[5].type == COL_FLOAT, "unit_price float");
    expect(table_ensure(&t, 0, 8), "sales buffer");
    cell = table_cell(&t, 0, 6);
    expect(cell && strstr(cell, "rush") && strstr(cell, "dock"), "quoted comma field");
    cell = table_cell(&t, 3, 6);
    expect(cell && strchr(cell, '"'), "escaped quotes");
    table_close(&t);

    expect(table_open(&t, "/home/luke/xcb-grid/testdata/big_log.tsv"), "open big");
    expect(t.nrows == 2500, "big rows");
    expect(t.cols[0].type == COL_INT, "id int");
    expect(t.cols[4].type == COL_INT, "latency int");
    expect(table_ensure(&t, 0, 30), "big first buffer");
    expect(t.buf_start == 0, "buf starts 0");
    expect(t.buf_rows > 30, "buf padded");
    expect(table_ensure(&t, 1200, 30), "big mid buffer");
    expect(t.buf_start <= 1200 && t.buf_start + t.buf_rows > 1230, "mid window");
    cell = table_cell(&t, 1200, 0);
    expect(cell && !strcmp(cell, "1201"), "row 1201 id");
    table_close(&t);

    expect(table_open(&t, "/home/luke/xcb-grid/testdata/scientific.tsv"), "open sci");
    expect(t.cols[1].type == COL_FLOAT, "mass float");
    expect(t.cols[2].type == COL_FLOAT, "charge float");
    table_close(&t);

    expect(table_open(&t, "/home/luke/xcb-grid/testdata/empty_cells.csv"), "open empty");
    expect(t.cols[0].type == COL_INT, "id still int");
    expect(t.cols[2].type == COL_STRING, "email string");
    table_close(&t);

    {
        const char *path = "/tmp/xcb-grid-page.tsv";
        FILE *fp = fopen(path, "w");
        int i, loads;
        size_t peak = 0;
        expect(fp != NULL, "write page file");
        if (fp) {
            fputs("id\tname\tval\n", fp);
            for (i = 1; i <= 40000; i++)
                fprintf(fp, "%d\tname-%d\t%.2f\n", i, i, i * 0.25);
            fclose(fp);
        }
        expect(table_open(&t, path), "open page file");
        expect(t.nrows == 40000, "40k rows");
        expect(table_ensure(&t, 0, 25), "page start");
        expect(t.buf_rows > 200, "start pad is generous");
        expect(t.buf_rows < 4000, "start buffer bounded");
        expect(table_cell(&t, 0, 0) && !strcmp(table_cell(&t, 0, 0), "1"), "row1 in buf");
        {
            int end0 = t.buf_start + t.buf_rows;
            int v = end0 - 25 - 80;
            if (v < 0)
                v = 0;
            expect(table_ensure(&t, v, 25), "approach edge");
            expect(t.buf_start + t.buf_rows > end0, "prefetched before edge");
            expect(table_cell(&t, end0, 0) != NULL, "next chunk already resident");
        }
        loads = t.buf_loads;
        peak = t.arena_len;
        expect(table_ensure(&t, 20000, 25), "page middle");
        expect(t.buf_start > 0, "unloaded start");
        expect(table_cell(&t, 0, 0) == NULL, "old window evicted");
        expect(table_cell(&t, 20000, 0) != NULL, "mid window loaded");
        expect(t.buf_rows < 4000, "mid buffer bounded");
        expect(t.buf_loads > loads, "buffer reloaded");
        if (t.arena_len > peak)
            peak = t.arena_len;
        expect(table_ensure(&t, 39900, 25), "page end");
        expect(table_cell(&t, 20000, 0) == NULL, "mid window evicted");
        expect(t.buf_rows < 4000, "end buffer bounded");
        expect(peak < 2 * 1024 * 1024, "cell arena stays small");
        table_close(&t);
        remove(path);
    }

    if (fail) {
        fprintf(stderr, "%d checks failed\n", fail);
        return 1;
    }
    puts("table checks ok");
    return 0;
}
