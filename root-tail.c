/*
 * Copyright 2001 by Marco d'Itri <md@linux.it>
 *
 * Original version by Mike Baker, then maintained by pcg@goof.com.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#if HAS_REGEX
#include <regex.h>
#endif
#include <X11/Xlib.h>
#include <X11/Xutil.h>

/* data structures */
struct logfile_entry {
    char *fname;		/* name of file                         */
    char *desc;			/* alternative description              */
    FILE *fp;			/* FILE struct associated with file     */
    ino_t inode;		/* inode of the file opened		*/
    off_t last_size;		/* file size at the last check		*/
    unsigned long color;	/* color to be used for printing        */
    struct logfile_entry *next;
};

struct linematrix {
    char *line;
    unsigned long color;
};


/* global variables */
unsigned int width = STD_WIDTH, listlen = STD_HEIGHT;
int win_x = LOC_X, win_y = LOC_Y;
int w = -1, h = -1, font_width, font_height, font_descent;
int do_reopen;
struct timeval interval = { 2, 400000 };	/* see Knuth */

/* command line options */
int opt_noinitial, opt_shade, opt_frame, opt_reverse, opt_nofilename,
    geom_mask, reload = 3600;
const char *command = NULL,
    *fontname = USE_FONT, *dispname = NULL, *def_color = DEF_COLOR;

struct logfile_entry *loglist = NULL, *loglist_tail = NULL;

Display *disp;
Window root;
GC WinGC;

#if HAS_REGEX
struct re_list {
    regex_t from;
    const char *to;
    struct re_list *next;
};
struct re_list *re_head, *re_tail;
#endif


/* prototypes */
void list_files(int);
void force_reopen(int);
void force_refresh(int);
void blank_window(int);

void InitWindow(void);
unsigned long GetColor(const char *);
void redraw(void);
void refresh(struct linematrix *, int, int);

void transform_line(char *s);
int lineinput(char *, int, FILE *);
void reopen(void);
void check_open_files(void);
FILE *openlog(struct logfile_entry *);
void main_loop(void);

void display_version(void);
void display_help(char *);
void install_signal(int, void (*)(int));
void *xstrdup(const char *);
void *xmalloc(size_t);
int daemonize(void);

/* signal handlers */
void list_files(int dummy)
{
    struct logfile_entry *e;

    fprintf(stderr, "Files opened:\n");
    for (e = loglist; e; e = e->next)
	fprintf(stderr, "\t%s (%s)\n", e->fname, e->desc);
}

void force_reopen(int dummy)
{
    do_reopen = 1;
}

void force_refresh(int dummy)
{
    XClearWindow(disp, root);
    redraw();
}

void blank_window(int dummy)
{
    XClearWindow(disp, root);
    XFlush(disp);
    exit(0);
}

/* X related functions */
unsigned long GetColor(const char *ColorName)
{
    XColor Color;
    XWindowAttributes Attributes;

    XGetWindowAttributes(disp, root, &Attributes);
    Color.pixel = 0;
    if (!XParseColor(disp, Attributes.colormap, ColorName, &Color))
	fprintf(stderr, "can't parse %s\n", ColorName);
    else if (!XAllocColor(disp, Attributes.colormap, &Color))
	fprintf(stderr, "can't allocate %s\n", ColorName);
    return Color.pixel;
}

void InitWindow(void)
{
    XGCValues gcv;
    Font font;
    unsigned long gcm;
    XFontStruct *info;
    int screen, ScreenWidth, ScreenHeight;

    if (!(disp = XOpenDisplay(dispname))) {
	fprintf(stderr, "Can't open display %s.\n", dispname);
	exit(1);
    }
    screen = DefaultScreen(disp);
    ScreenHeight = DisplayHeight(disp, screen);
    ScreenWidth = DisplayWidth(disp, screen);
    root = RootWindow(disp, screen);
    gcm = GCBackground;
    gcv.graphics_exposures = True;
    WinGC = XCreateGC(disp, root, gcm, &gcv);
    XMapWindow(disp, root);
    XSetForeground(disp, WinGC, GetColor(DEF_COLOR));

    font = XLoadFont(disp, fontname);
    XSetFont(disp, WinGC, font);
    info = XQueryFont(disp, font);
    font_width = info->max_bounds.width;
    font_descent = info->max_bounds.descent;
    font_height = info->max_bounds.ascent + font_descent;

    w = width * font_width;
    h = listlen * font_height;
    if (geom_mask & XNegative)
	win_x = win_x + ScreenWidth - w;
    if (geom_mask & YNegative)
	win_y = win_y + ScreenHeight - h;

    XSelectInput(disp, root, ExposureMask|FocusChangeMask);
}

char * 
detabificate (char *s)
{
  char * out;
  int i, j;

  out = malloc (8 * strlen (s) + 1);

  for(i = 0, j = 0; s[i]; i++) 
    {
      if (s[i] == '\t') 
        do 
          out[j++] = ' ';
        while (j % 8);      
      else
        out[j++] = s[i];
    }
  
  out[j] = '\0';
  return out;
}

/*
 * redraw does a complete redraw, rather than an update (i.e. the area
 * gets cleared first)
 * the rest is handled by regular refresh()'es
 */
void redraw(void)
{
    XClearArea(disp, root, win_x, win_y, w, h + font_descent + 2, True);
}

/* Just redraw everything without clearing (i.e. after an EXPOSE event) */
void refresh(struct linematrix *lines, int miny, int maxy)
{
    int lin;
    int offset = (listlen + 1) * font_height;
    unsigned long black_color = GetColor("black");

    miny -= win_y + font_height;
    maxy -= win_y - font_height;

    for (lin = listlen; lin--;)
      {
        char *temp;
  
        offset -= font_height;
  
        if (offset < miny || offset > maxy)
          continue;
  
        temp = detabificate (lines[lin].line);
  
        if (opt_shade)
          {
            XSetForeground (disp, WinGC, black_color);
            XDrawString (disp, root, WinGC, win_x + 2, win_y + offset + 2,
                         temp, strlen (temp));
          }
  
        XSetForeground (disp, WinGC, lines[lin].color);
        XDrawString (disp, root, WinGC, win_x, win_y + offset,
  		   temp, strlen (temp));
        
        free (temp);
    }

    if (opt_frame) {
	int bot_y = win_y + h + font_descent + 2;

	XDrawLine(disp, root, WinGC, win_x, win_y, win_x + w, win_y);
	XDrawLine(disp, root, WinGC, win_x + w, win_y, win_x + w, bot_y);
	XDrawLine(disp, root, WinGC, win_x + w, bot_y, win_x, bot_y);
	XDrawLine(disp, root, WinGC, win_x, bot_y, win_x, win_y);
    }
}

#if HAS_REGEX
void transform_line(char *s)
{
#ifdef I_AM_Md
    int i;
    if (1) {
	for (i = 16; s[i]; i++)
	    s[i] = s[i + 11];
    }
    s[i + 1] = '\0';
#endif

    if (transformre) {
	int i;
	regmatch_t matched[16];

	i = regexec(&transformre, string, 16, matched, 0);
	if (i == 0) {			/* matched */
	}
    }
}
#endif


/*
 * This routine should read 'width' characters and not more. However,
 * we really want to read width + 1 charachters if the last char is a '\n',
 * which we should remove afterwards. So, read width+1 chars and ungetc
 * the last character if it's not a newline. This means 'string' must be
 * width + 2 wide!
 */
int lineinput(char *string, int slen, FILE *f)
{
    int len;

    do {
	if (fgets(string, slen, f) == NULL)	/* EOF or Error */
	    return 0;

	len = strlen(string);
    } while (len == 0);

    if (string[len - 1] == '\n')
	string[len - 1] = '\0';			/* erase newline */
    else if (len >= slen - 1) {
	ungetc(string[len - 1], f);
	string[len - 1] = '\0';
    }

#if HAS_REGEX
    transform_line(string);
#endif
    return len;
}

/* input: reads file->fname
 * output: fills file->fp, file->inode
 * returns file->fp
 * in case of error, file->fp is NULL
 */
FILE *openlog(struct logfile_entry *file)
{
    struct stat stats;

    if ((file->fp = fopen(file->fname, "r")) == NULL) {
	file->fp = NULL;
	return NULL;
    }

    fstat(fileno(file->fp), &stats);
    if (S_ISFIFO(stats.st_mode)) {
	if (fcntl(fileno(file->fp), F_SETFL, O_NONBLOCK) < 0)
	    perror("fcntl"), exit(1);
	file->inode = 0;
    } else
	file->inode = stats.st_ino;

    if (opt_noinitial)
      fseek (file->fp, 0, SEEK_END);
    else if (stats.st_size > (listlen + 1) * width)
      {
        char dummy[255];

        fseek(file->fp, -((listlen + 2) * width), SEEK_END);
        /* the pointer might point halfway some line. Let's
           be nice and skip this damaged line */
        lineinput(dummy, sizeof(dummy), file->fp);
      }

    file->last_size = stats.st_size;
    return file->fp;
}

void reopen(void)
{
    struct logfile_entry *e;

    for (e = loglist; e; e = e->next) {
	if (!e->inode)
	    continue;			/* skip stdin */

	if (e->fp)
	    fclose(e->fp);
	/* if fp is NULL we will try again later */
	openlog(e);
    }

    do_reopen = 0;
}

void check_open_files(void)
{
    struct logfile_entry *e;
    struct stat stats;

    for (e = loglist; e; e = e->next) {
	if (!e->inode)
	    continue;				/* skip stdin */

	if (stat(e->fname, &stats) < 0) {	/* file missing? */
	    sleep(1);
	    if (e->fp)
		fclose(e->fp);
	    if (openlog(e) == NULL)
		break;
	}

	if (stats.st_ino != e->inode)	{	/* file renamed? */
	    if (e->fp)
		fclose(e->fp);
	    if (openlog(e) == NULL)
		break;
	}

	if (stats.st_size < e->last_size) {	/* file truncated? */
	    fseek(e->fp, 0, SEEK_SET);
	    e->last_size = stats.st_size;
	}
    }
}

#define SCROLL_UP(lines, listlen)				\
{								\
    int cur_line;						\
    for (cur_line = 0; cur_line < (listlen - 1); cur_line++) {	\
	strcpy(lines[cur_line].line, lines[cur_line + 1].line);	\
	lines[cur_line].color = lines[cur_line + 1].color;	\
    }								\
}

void main_loop(void)
{
    struct linematrix *lines = xmalloc(sizeof(struct linematrix) * listlen);
    int lin, miny, maxy, buflen;
    char *buf;
    time_t lastreload;
    Region region = XCreateRegion();
    XEvent xev;

    maxy = 0;
    miny = win_y + h;
    buflen = width + 2;
    buf = xmalloc(buflen);
    lastreload = time(NULL);

    /* Initialize linematrix */
    for (lin = 0; lin < listlen; lin++) {
	lines[lin].line = xmalloc(buflen);
	strcpy(lines[lin].line, "~");
	lines[lin].color = GetColor(def_color);
    }

    if (!opt_noinitial)
	while (lineinput(buf, buflen, loglist->fp) != 0) {
	    SCROLL_UP(lines, listlen);
	    /* print the next line */
	    strcpy(lines[listlen - 1].line, buf);
	}

    for (;;) {
	int need_update = 0;
	struct logfile_entry *current;
	static struct logfile_entry *lastprinted = NULL;

	/* read logs */
	for (current = loglist; current; current = current->next) {
	    if (!current->fp)
		continue;		/* skip missing files */

	    clearerr(current->fp);

	    while (lineinput(buf, buflen, current->fp) != 0) {
		/* print filename if any, and if last line was from
		   different file */
		if (!opt_nofilename &&
			!(lastprinted && lastprinted == current) &&
			current->desc[0]) {
		    SCROLL_UP(lines, listlen);
		    sprintf(lines[listlen - 1].line, "[%s]", current->desc);
		    lines[listlen - 1].color = current->color;
		}

		SCROLL_UP(lines, listlen);
		strcpy(lines[listlen - 1].line, buf);
		lines[listlen - 1].color = current->color;

		lastprinted = current;
		need_update = 1;
	    }
	}

	if (need_update)
	    redraw();
	else {
	    XFlush(disp);
	    if (!XPending(disp)) {
		fd_set fdr;
		struct timeval to = interval;

		FD_ZERO(&fdr);
		FD_SET(ConnectionNumber(disp), &fdr);
		select(ConnectionNumber(disp) + 1, &fdr, 0, 0, &to);
	    }
	}

	check_open_files();

	if (do_reopen)
	    reopen();

	/* we ignore possible errors due to window resizing &c */
	while (XPending(disp)) {
	    XNextEvent(disp, &xev);
	    switch (xev.type) {
	    case Expose:
		{
		    XRectangle r;

		    r.x = xev.xexpose.x;
		    r.y = xev.xexpose.y;
		    r.width = xev.xexpose.width;
		    r.height = xev.xexpose.height;
		    XUnionRectWithRegion(&r, region, region);
		    if (miny > r.y)
			miny = r.y;
		    if (maxy < r.y + r.height)
			maxy = r.y + r.height;
		}
		break;
	    default:
#ifdef DEBUGMODE
		fprintf(stderr, "PANIC! Unknown event %d\n", xev.type);
#endif
		break;
	    }
	}

	/* reload if requested */
	if (reload && lastreload + reload < time(NULL)) {
	    if (command)
		system(command);

	    reopen();
	    lastreload = time(NULL);
	}

	if (!XEmptyRegion(region)) {
	    XSetRegion(disp, WinGC, region);
	    refresh(lines, miny, maxy);
	    XDestroyRegion(region);
	    region = XCreateRegion();
	    maxy = 0;
	    miny = win_y + h;
	}
    }
}


int main(int argc, char *argv[])
{
    int i;
    int opt_daemonize = 0;
#if HAS_REGEX
    char *transform = NULL;
#endif

    /* window needs to be initialized before colorlookups can be done */
    /* just a dummy to get the color lookups right */
    geom_mask = NoValue;
    InitWindow();

    for (i = 1; i < argc; i++) {
	if (argv[i][0] == '-' && argv[i][1] != '\0' && argv[i][1] != ',') {
	    if (!strcmp(argv[i], "--?") ||
		!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h"))
		display_help(argv[0]);
	    else if (!strcmp(argv[i], "-V"))
		display_version();
	    else if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "-geometry"))
		geom_mask = XParseGeometry(argv[++i],
			&win_x, &win_y, &width, &listlen);
	    else if (!strcmp(argv[i], "-display"))
		dispname = argv[++i];
	    else if (!strcmp(argv[i], "-font") || !strcmp(argv[i], "-fn"))
		fontname = argv[++i];
#if HAS_REGEX
	    else if (!strcmp(argv[i], "-t"))
		transform = argv[++i];
#endif
	    else if (!strcmp(argv[i], "-fork") || !strcmp(argv[i], "-f"))
		opt_daemonize = 1;
	    else if (!strcmp(argv[i], "-reload")) {
		reload = atoi(argv[++i]);
		command = argv[++i];
	    }
	    else if (!strcmp(argv[i], "-shade"))
		opt_shade = 1;
	    else if (!strcmp(argv[i], "-frame"))
		opt_frame = 1;
	    else if (!strcmp(argv[i], "-no-filename"))
		opt_nofilename = 1;
	    else if (!strcmp(argv[i], "-reverse"))
		opt_reverse = 1;
	    else if (!strcmp(argv[i], "-color"))
		def_color = argv[++i];
	    else if (!strcmp(argv[i], "-noinitial"))
		opt_noinitial = 1;
	    else if (!strcmp(argv[i], "-interval") || !strcmp(argv[i], "-i")) {
		double iv = atof(argv[++i]);

		interval.tv_sec = (int) iv;
		interval.tv_usec = (iv - interval.tv_sec) * 1e6;
	    } else {
		fprintf(stderr, "Unknown option '%s'.\n"
			"Try --help for more information.\n", argv[i]);
		exit(1);
	    }
	} else {		/* it must be a filename */
	    struct logfile_entry *e;
	    const char *fname, *desc, *fcolor = def_color;
	    char *p;

	    /* this is not foolproof yet (',' in filenames are not allowed) */
	    fname = desc = argv[i];
	    if ((p = strchr(argv[i], ','))) {
		*p = '\0';
		fcolor = p + 1;

		if ((p = strchr(fcolor, ','))) {
		    *p = '\0';
		    desc = p + 1;
		}
	    }

	    e = xmalloc(sizeof(struct logfile_entry));
	    if (argv[i][0] == '-' && argv[i][1] == '\0') {
		if ((e->fp = fdopen(0, "r")) == NULL)
		    perror("fdopen"), exit(1);
		if (fcntl(0, F_SETFL, O_NONBLOCK) < 0)
		    perror("fcntl"), exit(1);
		e->fname = NULL;
		e->inode = 0;
		e->desc = xstrdup("stdin");
	    } else {
		int l;

		e->fname = xstrdup(fname);
		if (openlog(e) == NULL)
		    perror(fname), exit(1);

		l = strlen(desc);
		if (l > width - 2)		/* must account for [ ] */
		    l = width - 2;
		e->desc = xmalloc(l + 1);
		memcpy(e->desc, desc, l);
		*(e->desc + l) = '\0';
	    }

	    e->color = GetColor(fcolor);
	    e->next = NULL;

	    if (!loglist)
		loglist = e;
	    if (loglist_tail)
		loglist_tail->next = e;
	    loglist_tail = e;
	}
    }

    if (!loglist) {
	fprintf(stderr, "You did not specify any files to tail\n"
		"use %s --help for help\n", argv[0]);
	exit(1);
    }

#if HAS_REGEX
    if (transform) {
	int i;

	transformre = xmalloc(sizeof(transformre));
	i = regcomp(&transformre, transform, REG_EXTENDED);
	if (i != 0) {
	    char buf[512];

	    regerror(i, &transformre, buf, sizeof(buf));
	    fprintf(stderr, "Cannot compile regular expression: %s\n", buf);
	}
    }
#endif

    InitWindow();

    install_signal(SIGINT, blank_window);
    install_signal(SIGQUIT, blank_window);
    install_signal(SIGTERM, blank_window);
    install_signal(SIGHUP, force_reopen);
    install_signal(SIGUSR1, list_files);
    install_signal(SIGUSR2, force_refresh);

    if (opt_daemonize)
	daemonize();

    main_loop();

    exit(1);			/* to make gcc -Wall stop complaining */
}

void install_signal(int sig, void (*handler)(int))
{
    struct sigaction action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(sig, &action, NULL) < 0)
	fprintf(stderr, "sigaction(%d): %s\n", sig, strerror(errno)), exit(1);
}

void *xstrdup(const char *string)
{
    void *p;

    while ((p = strdup(string)) == NULL) {
        fprintf(stderr, "Memory exausted.");
	sleep(10);
    }
    return p;
}

void *xmalloc(size_t size)
{
    void *p;

    while ((p = malloc(size)) == NULL) {
        fprintf(stderr, "Memory exausted.");
	sleep(10);
    }
    return p;
}

void display_help(char *myname)
{
    printf("Usage: %s [options] file1[,color[,desc]] "
	   "[file2[,color[,desc]] ...]\n", myname);
    printf(" -g | -geometry geometry   -g WIDTHxHEIGHT+X+Y\n"
	    " -color    color           use color $color as default\n"
	    " -reload sec command       reload after $sec and run command\n"
	    "                           by default -- 3 mins\n"
	    " -font FONTSPEC            (-fn) font to use\n"
	    " -f | -fork                fork into background\n"
	    " -reverse                  print new lines at the top\n"
	    " -shade                    add shading to font\n"
	    " -noinitial                don't display the last file lines on\n"
	    "                           startup\n"
	    " -i | -interval seconds    interval between checks (fractional\n"
	    "                           values o.k.). Default 3\n"
	    " -V                        display version information and exit\n"
	    "\n");
    printf("Example:\n%s -g 80x25+100+50 -font fixed /var/log/messages,green "
	 "/var/log/secure,red,'ALERT'\n", myname);
    exit(0);
}

void display_version(void) {
    printf("root-tail version " VERSION "\n");
    exit(0);
}

int daemonize(void) {
    switch (fork()) {
    case -1:
	return -1;
    case 0:
	break;
    default:
	_exit(0);
    }

    if (setsid() == -1)
	return -1;

    return 0;
}

