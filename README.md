# xcb-grid

You are writing a **spreadsheet viewer**, not a game. The interesting constraint is the table, not the window: a TSV (or similar) that may be huge, with only a sliding row window in RAM.

`table.h` is the platform. `grid.c` is one X11 skin on top of it.

## Build

Linux / WSL only:

```bash
make
./xcb-grid testdata/something.tsv
make test
```

`python3 gen_stress.py` can build a million-row file under `testdata/`.

## The table API

```c
Table t;
table_open(&t, path);                 /* sniff columns, record file offsets */
table_ensure(&t, vis_row0, vis_rows); /* load the visible slice */
table_prefetch(&t, vis_row0, vis_rows, budget);
const char *s = table_cell(&t, row, col);
table_close(&t);
```

You do **not** load the whole file. `offs[]` is a row index into the stream. The arena holds only `buf_rows` around the viewport. When the user scrolls, `table_ensure` slides that window; `table_prefetch` spends a small budget loading ahead so the next page is already warm.

Column types (`COL_INT`, `COL_FLOAT`, `COL_STRING`) are sniffed from the header/body so the viewer can right-align numbers. Delimiter is detected on open.

## The viewer (`grid.c`)

If you are changing the UI: keyboard and scrollbars move `vis_row0`; each expose calls `table_ensure` then draws header + cells with a fixed bitmap font. That is all. Do not add a second copy of the file in memory.

## Tests

`test_table.c` walks `table.h` without X11. Keep it green if you change the buffer policy.
