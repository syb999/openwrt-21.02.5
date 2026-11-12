# src/Makefile - built by luci.mk (Build/Compile/Default + install DESTDIR=PKG_INSTALL_DIR)
CC ?= gcc
CFLAGS ?= -O2 -Wall
LDFLAGS ?=
LDLIBS := -lsqlite3 -lz -lm

SRC := util.c gbk.c zip.c xlsx.c xls.c csv.c sheet.c merge.c db.c main.c cgi.c \
       xlsxw.c sqldump.c csvw.c
OBJ := $(SRC:.c=.o)

all: xlsx2sqlite

compile: all

xlsx2sqlite: $(OBJ) x2s.h
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c x2s.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) xlsx2sqlite

install: xlsx2sqlite
	install -d $(DESTDIR)/usr/bin
	install -m 0755 xlsx2sqlite $(DESTDIR)/usr/bin/xlsx2sqlite
	install -d $(DESTDIR)/www/cgi-bin
	install -m 0755 xlsx2sqlite $(DESTDIR)/www/cgi-bin/xlsx2sqlite

.PHONY: all compile clean install
