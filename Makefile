COPTS = -g

CFLAGS = -Wall $(COPTS) -DUSE_TOON_GET_ROOT_WINDOW

LDFLAGS = -L/usr/X11R6/lib -lX11

PREFIX = /usr
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1
DOCDIR = $(PREFIX)/share/doc/root-tail

SOURCES = root-tail.c toon_root.c
all: root-tail man

root-tail: $(SOURCES) config.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES)

man: root-tail.1.gz

root-tail.1.gz: root-tail.man
	cp root-tail.man root-tail.1
	gzip -f9 root-tail.1

clean:
	rm -f root-tail root-tail.1.gz

install: all
	install -D -o root -g root root-tail $(BINDIR)
	install -D -m 0644 -o root -g root root-tail.1.gz $(MANDIR)
	install -D -m 0644 -o root -g root README $(DOCDIR)
	install -m 0644 -o root -g root Changes $(DOCDIR)

uninstall:
	rm -f $(BINDIR)/root-tail
	rm -f $(MANDIR)/root-tail.1.gz
	rm -f $(DOCDIR)/Changes
	rm -f $(DOCDIR)/README
	rmdir --ignore-fail-on-non-empty $(DOCDIR)

