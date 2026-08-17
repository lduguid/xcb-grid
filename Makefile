CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2 $(shell pkg-config --cflags x11 xcb)
MACHINE = $(shell gcc -dumpmachine)
X11XCB = $(shell test -e /usr/lib/$(MACHINE)/libX11-xcb.so && echo -lX11-xcb || echo /usr/lib/$(MACHINE)/libX11-xcb.so.1)
LIBS = $(shell pkg-config --libs x11 xcb) $(X11XCB)

xcb-grid: grid.c table.c table.h
	$(CC) $(CFLAGS) -o $@ grid.c table.c $(LIBS)

test: table.c table.h test_table.c
	$(CC) $(CFLAGS) -o test_table test_table.c table.c
	./test_table

stress: testdata/stress_records.tsv

testdata/stress_records.tsv: gen_stress.py
	python3 gen_stress.py 1000000 testdata/stress_records.tsv

clean:
	rm -f xcb-grid test_table
