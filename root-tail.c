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
#include <locale.h>
#include <ctype.h>
#include <stdarg.h>
#if HAS_REGEX
#include <regex.h>
#endif
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

/* data structures */
struct logfile_entry
{
  struct logfile_entry *next;

  char *fname;                  /* name of file                                */
  char *desc;                   /* alternative description                     */
  char *buf;                    /* text read but not yet displayed             */
  FILE *fp;                     /* FILE struct associated with file            */
  ino_t inode;                  /* inode of the file opened                    */
  off_t last_size;              /* file size at the last check                 */
  unsigned long color;          /* color to be used for printing               */
  int partial;                  /* true if the last line isn't complete        */
  int lastpartial;              /* true if the previous output wasn't complete */
  int index;                    /* index into linematrix of a partial line     */
};

struct linematrix
{
  char *line;
  int len;
  unsigned long color;
};

/* global variables */
struct linematrix *lines;
int width = STD_WIDTH, height = STD_HEIGHT, listlen;
int win_x = LOC_X, win_y = LOC_Y;
int font_descent, font_height;
int do_reopen;
struct timeval interval = { 3, 0 };
XFontSet fontset;

/* command line options */
int opt_noinitial, opt_shade, opt_frame, opt_reverse, opt_nofilename,
  opt_whole, opt_update, geom_mask, reload = 0;
const char *command = NULL,
  *fontname = USE_FONT, *dispname = NULL, *def_color = DEF_COLOR,
  *continuation = "[+]";

struct logfile_entry *loglist = NULL, *loglist_tail = NULL;

Display *disp;
Window root;
GC WinGC;

#if HAS_REGEX
struct re_list
{
  regex_t from;
  const char *to;
  struct re_list *next;
};
struct re_list *re_head, *re_tail;
#endif


/* prototypes */
void list_files (int);
void force_reopen (int);
void force_refresh (int);
void blank_window (int);

void InitWindow (void);
unsigned long GetColor (const char *);
void redraw (void);
void refresh (int, int);

void transform_line (char *s);
int lineinput (struct logfile_entry *);
void reopen (void);
void check_open_files (void);
FILE *openlog (struct logfile_entry *);
static void main_loop (void);

void display_version (void);
void display_help (char *);
void install_signal (int, void (*)(int));
void *xstrdup (const char *);
void *xmalloc (size_t);
int daemonize (void);

/* signal handlers */
void
list_files (int dummy)
{
  struct logfile_entry *e;

  fprintf (stderr, "Files opened:\n");
  for (e = loglist; e; e = e->next)
    fprintf (stderr, "\t%s (%s)\n", e->fname, e->desc);
}

void
force_reopen (int dummy)
{
  do_reopen = 1;
}

void
force_refresh (int dummy)
{
  redraw ();
}

void
blank_window (int dummy)
{
  XClearArea (disp, root, win_x - 2, win_y - 2, width + 5, height + 5, False);
  XFlush (disp);
  exit (0);
}

/* X related functions */
unsigned long
GetColor (const char *ColorName)
{
  XColor Color;
  XWindowAttributes Attributes;

  XGetWindowAttributes (disp, root, &Attributes);
  Color.pixel = 0;

  if (!XParseColor (disp, Attributes.colormap, ColorName, &Color))
    fprintf (stderr, "can't parse %s\n", ColorName);
  else if (!XAllocColor (disp, Attributes.colormap, &Color))
    fprintf (stderr, "can't allocate %s\n", ColorName);

  return Color.pixel;
}

static Window
root_window (Display * display, int screen_number)
{
  Atom SWM_VROOT = XInternAtom (display, "__SWM_VROOT", False);
  Window real_root_window = RootWindow (display, screen_number);

  if (root)                     /* root window set via option */
    return root;

  if (SWM_VROOT != None)
    {
      Window unused, *windows;
      unsigned int count;

      if (XQueryTree (display, real_root_window, &unused, &unused, &windows,
                      &count))
        {
          int i;

          for (i = 0; i < count; i++)
            {
              Atom type;
              int format;
              unsigned long nitems, bytes_after_return;
              unsigned char *virtual_root_window;

              if (XGetWindowProperty (display, windows[i], SWM_VROOT,
                                      0, 1, False, XA_WINDOW, &type, &format,
                                      &nitems, &bytes_after_return,
                                      &virtual_root_window) == Success)
                {
                  if (type != None)
                    {
                      if (type == XA_WINDOW)
                        {
                          XFree (windows);
                          return (Window) virtual_root_window;
                        }
                      else
                        fprintf (stderr,
                                 "__SWM_VROOT property type mismatch");
                    }
                }
              else
                fprintf (stderr,
                         "failed to get __SWM_VROOT property on window 0x%lx",
                         windows[i]);
            }

          if (count)
            XFree (windows);
        }
      else
        fprintf (stderr, "Can't query tree on root window 0x%lx",
                 real_root_window);
    }
  else
    /* This shouldn't happen. The Xlib documentation is wrong BTW. */
    fprintf (stderr, "Can't intern atom __SWM_VROOT");

  return real_root_window;
}

void
InitWindow (void)
{
  XGCValues gcv;
  unsigned long gcm;
  int screen, ScreenWidth, ScreenHeight;

  if (!(disp = XOpenDisplay (dispname)))
    {
      fprintf (stderr, "Can't open display %s.\n", dispname);
      exit (1);
    }

  screen = DefaultScreen (disp);
  ScreenHeight = DisplayHeight (disp, screen);
  ScreenWidth = DisplayWidth (disp, screen);

  root = root_window (disp, screen);

  gcm = GCBackground;
  gcv.graphics_exposures = True;
  WinGC = XCreateGC (disp, root, gcm, &gcv);
  XMapWindow (disp, root);
  XSetForeground (disp, WinGC, GetColor (DEF_COLOR));

  {
    char **missing_charset_list;
    int missing_charset_count;
    char *def_string;

    fontset = XCreateFontSet (disp, fontname,
                              &missing_charset_list, &missing_charset_count,
                              &def_string);

    if (missing_charset_count)
      {
        fprintf (stderr,
                 "Missing charsets in String to FontSet conversion (%s)\n",
                 missing_charset_list[0]);
        XFreeStringList (missing_charset_list);
      }
  }

  if (!fontset)
    {
      fprintf (stderr, "unable to create fontset, exiting.\n");
      exit (1);
    }

  {
    XFontSetExtents *xfe = XExtentsOfFontSet (fontset);

    font_height = xfe->max_logical_extent.height;
    font_descent = xfe->max_logical_extent.y;
  }

  if (geom_mask & XNegative)
    win_x = win_x + ScreenWidth - width;
  if (geom_mask & YNegative)
    win_y = win_y + ScreenHeight - height;

  listlen = height / font_height;

  if (!listlen)
    {
      fprintf (stderr, "height too small for a single line, setting to %d\n",
               font_height);
      listlen = 1;
    }

  height = listlen * font_height;

  XSelectInput (disp, root, ExposureMask | FocusChangeMask);
}

/*
 * redraw does a complete redraw, rather than an update (i.e. the area
 * gets cleared first)
 * the rest is handled by regular refresh()'es
 */
void
redraw (void)
{
  XSetClipMask (disp, WinGC, None);
  XClearArea (disp, root, win_x - 2, win_y - 2, width + 5, height + 5, False);
  refresh (0, 32768);
}

/* Just redraw everything without clearing (i.e. after an EXPOSE event) */
void
refresh (int miny, int maxy)
{
  int lin;
  int offset = (listlen + 1) * font_height;
  unsigned long black_color = GetColor ("black");

  miny -= win_y + font_height;
  maxy -= win_y - font_height;

  for (lin = listlen; lin--;)
    {
      struct linematrix *line = lines + (opt_reverse ? listlen - lin - 1 : lin);

      offset -= font_height;

      if (offset < miny || offset > maxy)
        continue;

      if (opt_shade)
        {
          XSetForeground (disp, WinGC, black_color);
          XmbDrawString (disp, root, fontset, WinGC, win_x + 2,
                         win_y + offset + 2, line->line, line->len);
        }

      XSetForeground (disp, WinGC, line->color);
      XmbDrawString (disp, root, fontset, WinGC, win_x, win_y + offset,
                     line->line, line->len);
    }

  if (opt_frame)
    {
      XSetForeground (disp, WinGC, GetColor (def_color));
      XDrawRectangle (disp, root, WinGC, win_x - 2, win_y - 2, width + 4, height + 4);
    }
}

#if HAS_REGEX
void
transform_line (char *s)
{
#ifdef I_AM_Md
  int i;
  if (1)
    {
      for (i = 16; s[i]; i++)
        s[i] = s[i + 11];
    }
  s[i + 1] = '\0';
#endif

  if (transformre)
    {
      int i;
      regmatch_t matched[16];

      i = regexec (&transformre, string, 16, matched, 0);
      if (i == 0)
        {                       /* matched */
        }
    }
}
#endif

char *
concat_line (const char *p1, const char *p2)
{
  int l1 = p1 ? strlen (p1) : 0;
  int l2 = strlen (p2);
  char *r = xmalloc (l1 + l2 + 1);

  memcpy (r, p1, l1);
  memcpy (r + l1, p2, l2);
  r[l1 + l2] = 0;

  return r;
}

/*
 * This routine should read a single line, no matter how long.
 */
int
lineinput (struct logfile_entry *logfile)
{
  char buff[1024], *p = buff;
  int ch;
  int ofs = logfile->buf ? strlen (logfile->buf) : 0;

  do
    {
      ch = fgetc (logfile->fp);

      if (ch == '\n' || ch == EOF)
        break;
      else if (ch == '\r')
        continue; /* skip */
      else if (ch == '\t')
        {
          do
            {
              *p++ = ' ';
              ofs++;
            }
          while (ofs & 7);
        }
      else
        {
          *p++ = ch;
          ofs++;
        }
    }
  while (p < buff + (sizeof buff) - 8 - 1);

  if (p == buff && ch == EOF)
    return 0;

  *p = 0;

  p = concat_line (logfile->buf, buff);
  free (logfile->buf); logfile->buf = p;

  logfile->lastpartial = logfile->partial;
  logfile->partial = ch == EOF;
  
  if (logfile->partial && opt_whole)
    return 0;

#if HAS_REGEX
  transform_line (logfile->buf);
#endif
  return 1;
}

/* input: reads file->fname
 * output: fills file->fp, file->inode
 * returns file->fp
 * in case of error, file->fp is NULL
 */
FILE *
openlog (struct logfile_entry * file)
{
  struct stat stats;

  if ((file->fp = fopen (file->fname, "r")) == NULL)
    {
      file->fp = NULL;
      return NULL;
    }

  fstat (fileno (file->fp), &stats);
  if (S_ISFIFO (stats.st_mode))
    {
      if (fcntl (fileno (file->fp), F_SETFL, O_NONBLOCK) < 0)
        perror ("fcntl"), exit (1);
      file->inode = 0;
    }
  else
    file->inode = stats.st_ino;

  if (opt_noinitial)
    fseek (file->fp, 0, SEEK_END);
  else if (stats.st_size > (listlen + 1) * width)
    fseek (file->fp, -((listlen + 2) * width), SEEK_END);

  file->last_size = stats.st_size;
  return file->fp;
}

void
reopen (void)
{
  struct logfile_entry *e;

  for (e = loglist; e; e = e->next)
    {
      if (!e->inode)
        continue;               /* skip stdin */

      if (e->fp)
        fclose (e->fp);
      /* if fp is NULL we will try again later */
      openlog (e);
    }

  do_reopen = 0;
}

void
check_open_files (void)
{
  struct logfile_entry *e;
  struct stat stats;

  for (e = loglist; e; e = e->next)
    {
      if (!e->inode)
        continue;               /* skip stdin */

      if (stat (e->fname, &stats) < 0)
        {                       /* file missing? */
          sleep (1);
          if (e->fp)
            fclose (e->fp);
          if (openlog (e) == NULL)
            break;
        }

      if (stats.st_ino != e->inode)
        {                       /* file renamed? */
          if (e->fp)
            fclose (e->fp);
          if (openlog (e) == NULL)
            break;
        }

      if (stats.st_size < e->last_size)
        {                       /* file truncated? */
          fseek (e->fp, 0, SEEK_SET);
          e->last_size = stats.st_size;
        }
    }
}

/*
 * insert a single physical line (that must be short enough to fit)
 * at position "idx" by pushing up lines above it. the caller
 * MUST then fill in lines[idx] with valid data.
 */
static void
insert_line (int idx)
{
  int cur_line;
  struct logfile_entry *current;

  free (lines[0].line);

  for (cur_line = 0; cur_line < idx; cur_line++)
    lines[cur_line] = lines[cur_line + 1];

  for (current = loglist; current; current = current->next)
    if (current->index <= idx)
      current->index--;
}

/*
 * remove a single physical line at position "idx" by moving the lines above it
 * down and inserting a "~" line at the top.
 */
static void
delete_line (int idx)
{
  int cur_line;
  struct logfile_entry *current;

  for (cur_line = idx; cur_line > 0; cur_line--)
    lines[cur_line] = lines[cur_line - 1];

  lines[0].line = strdup ("~");

  for (current = loglist; current; current = current->next)
    if (current->index >= 0 && current->index <= idx)
      current->index++;
}

/*
 * takes a logical log file line and split it into multiple physical
 * screen lines by splitting it whenever a part becomes too long.
 * lal lines will be inserted at position "idx".
 */
static void
split_line (int idx, const char *str, unsigned long color)
{
  int l = strlen (str);
  const char *p = str;

  do
    {
      const char *beg = p;
      int w = 0;

      while (*p)
        {
          int len = mblen (p, l);
          if (len <= 0)
            len = 1; /* ignore (don't skip) ilegal character sequences */

          int cw = XmbTextEscapement (fontset, p, len);
          if (cw + w >= width)
            break;

          w += cw;
          p += len;
          l -= len;
        }

      {
        char *s = xmalloc (p - beg + 1);
        memcpy (s, beg, p - beg);
        s[p - beg] = 0;
        insert_line (idx);
        lines[idx].line = s;
        lines[idx].len = p - beg;
        lines[idx].color = color;
      }
    }
  while (l);
}

/*
 * append something to an existing physical line. this is done
 * by deleting the file on-screen, concatenating the new data to it
 * and splitting it again.
 */
static void
append_line (int idx, const char *str)
{
  unsigned long color = lines[idx].color;
  char *old = lines[idx].line;
  char *new = concat_line (old, str);

  free (old);

  delete_line (idx);
  split_line (idx, new, color);
}

static void
main_loop (void)
{
  lines = xmalloc (sizeof (struct linematrix) * listlen);
  int lin;
  time_t lastreload;
  Region region = XCreateRegion ();
  XEvent xev;
  struct logfile_entry *lastprinted = NULL;
  struct logfile_entry *current;
  int need_update = 1;

  lastreload = time (NULL);

  /* Initialize linematrix */
  for (lin = 0; lin < listlen; lin++)
    {
      lines[lin].line = strdup ("~");
      lines[lin].len = 1;
      lines[lin].color = GetColor (def_color);
    }

  for (;;)
    {
      /* read logs */
      for (current = loglist; current; current = current->next)
        {
          if (!current->fp)
            continue;           /* skip missing files */

          clearerr (current->fp);

          while (lineinput (current))
            {
              need_update = 1;
              /* if we're trying to update old partial lines in
               * place, and the last time this file was updated the
               * output was partial, and that partial line is not
               * too close to the top of the screen, then update
               * that partial line */
              if (opt_update && current->lastpartial && current->index >= 0)
                {
                  int idx = current->index;
                  append_line (idx, current->buf);
                  current->index = idx;
                  free (current->buf), current->buf = 0;
                  continue;
                }

              /* print filename if any, and if last line was from
               * different file */
              if (!opt_nofilename && lastprinted != current && current->desc[0])
                {
                  char buf[1024]; /* HACK */
                  snprintf (buf, sizeof (buf), "[%s]", current->desc);
                  split_line (listlen - 1, buf, current->color);
                }

              /* if this is the same file we showed last, and the
               * last time we showed it, it wasn't finished, then
               * append to the last line shown */
              if (lastprinted == current && current->lastpartial)
                {
                  append_line (listlen - 1, current->buf);
                  free (current->buf), current->buf = 0;
                  continue;
                }
              else
                {
                  split_line (listlen - 1, current->buf, current->color);
                  free (current->buf), current->buf = 0;
                }

              current->index = listlen - 1;
              lastprinted = current;
            }
        }

      if (need_update)
        {
          redraw ();
          need_update = 0;
        }
      else
        {
          XFlush (disp);

          if (!XPending (disp))
            {
              fd_set fdr;
              struct timeval to = interval;

              FD_ZERO (&fdr);
              FD_SET (ConnectionNumber (disp), &fdr);
              select (ConnectionNumber (disp) + 1, &fdr, 0, 0, &to);
            }
        }

      check_open_files ();

      if (do_reopen)
        reopen ();

      /* we ignore possible errors due to window resizing &c */
      while (XPending (disp))
        {
          XNextEvent (disp, &xev);

          switch (xev.type)
            {
            case Expose:
              {
                XRectangle r;

                r.x = xev.xexpose.x;
                r.y = xev.xexpose.y;
                r.width = xev.xexpose.width;
                r.height = xev.xexpose.height;

                XUnionRectWithRegion (&r, region, region);
              }
              break;
            default:
#ifdef DEBUGMODE
              fprintf (stderr, "PANIC! Unknown event %d\n", xev.type);
#endif
              break;
            }
        }

      /* reload if requested */
      if (reload && lastreload + reload < time (NULL))
        {
          if (command && command[0])
            system (command);

          reopen ();
          lastreload = time (NULL);
        }

      if (!XEmptyRegion (region))
        {
          XRectangle r;

          XSetRegion (disp, WinGC, region);
          XClipBox (region, &r);

          refresh (r.y, r.y + r.height);

          XDestroyRegion (region);
          region = XCreateRegion ();
        }
    }
}


int
main (int argc, char *argv[])
{
  int i;
  int opt_daemonize = 0;
  int opt_partial = 0, file_count = 0;
#if HAS_REGEX
  char *transform = NULL;
#endif

  setlocale (LC_CTYPE, "");     /* try to initialize the locale. */

  /* window needs to be initialized before colorlookups can be done */
  /* just a dummy to get the color lookups right */
  geom_mask = NoValue;
  InitWindow ();

  for (i = 1; i < argc; i++)
    {
      const char *arg = argv[i];

      if (arg[0] == '-' && arg[1] != '\0' && arg[1] != ',')
        {
          if (arg[1] == '-')
            arg++;

          if (!strcmp (arg, "-?") ||
              !strcmp (arg, "-help") || !strcmp (arg, "-h"))
            display_help (argv[0]);
          else if (!strcmp (arg, "-V"))
            display_version ();
          else if (!strcmp (arg, "-g") || !strcmp (arg, "-geometry"))
            geom_mask =
              XParseGeometry (argv[++i], &win_x, &win_y, &width, &height);
          else if (!strcmp (arg, "-display"))
            dispname = argv[++i];
          else if (!strcmp (arg, "-cont"))
            continuation = argv[++i];
          else if (!strcmp (arg, "-font") || !strcmp (arg, "-fn"))
            fontname = argv[++i];
#if HAS_REGEX
          else if (!strcmp (arg, "-t"))
            transform = argv[++i];
#endif
          else if (!strcmp (arg, "-fork") || !strcmp (arg, "-f"))
            opt_daemonize = 1;
          else if (!strcmp (arg, "-reload"))
            {
              reload = atoi (argv[++i]);
              command = argv[++i];
            }
          else if (!strcmp (arg, "-shade"))
            opt_shade = 1;
          else if (!strcmp (arg, "-frame"))
            opt_frame = 1;
          else if (!strcmp (arg, "-no-filename"))
            opt_nofilename = 1;
          else if (!strcmp (arg, "-reverse"))
            opt_reverse = 1;
          else if (!strcmp (arg, "-whole"))
            opt_whole = 1;
          else if (!strcmp (arg, "-partial"))
            opt_partial = 1;
          else if (!strcmp (arg, "-update"))
            opt_update = opt_partial = 1;
          else if (!strcmp (arg, "-color"))
            def_color = argv[++i];
          else if (!strcmp (arg, "-noinitial"))
            opt_noinitial = 1;
          else if (!strcmp (arg, "-id"))
            root = atoi (argv[++i]);
          else if (!strcmp (arg, "-interval") || !strcmp (arg, "-i"))
            {
              double iv = atof (argv[++i]);

              interval.tv_sec = (int) iv;
              interval.tv_usec = (iv - interval.tv_sec) * 1e6;
            }
          else
            {
              fprintf (stderr, "Unknown option '%s'.\n"
                       "Try --help for more information.\n", arg);
              exit (1);
            }
        }
      else
        {                       /* it must be a filename */
          struct logfile_entry *e;
          const char *fname, *desc, *fcolor = def_color;
          char *p;

          file_count++;

          /* this is not foolproof yet (',' in filenames are not allowed) */
          fname = desc = arg;
          if ((p = strchr (arg, ',')))
            {
              *p = '\0';
              fcolor = p + 1;

              if ((p = strchr (fcolor, ',')))
                {
                  *p = '\0';
                  desc = p + 1;
                }
            }

          e = xmalloc (sizeof (struct logfile_entry));
          e->partial = 0;
          e->buf = 0;
          e->index = -1;

          if (arg[0] == '-' && arg[1] == '\0')
            {
              if ((e->fp = fdopen (0, "r")) == NULL)
                perror ("fdopen"), exit (1);
              if (fcntl (0, F_SETFL, O_NONBLOCK) < 0)
                perror ("fcntl"), exit (1);
              e->fname = NULL;
              e->inode = 0;
              e->desc = xstrdup ("stdin");
            }
          else
            {
              int l;

              e->fname = xstrdup (fname);
              if (openlog (e) == NULL)
                perror (fname), exit (1);

              l = strlen (desc);
              if (l > width - 2)        /* must account for [ ] */
                l = width - 2;
              e->desc = xmalloc (l + 1);
              memcpy (e->desc, desc, l);
              *(e->desc + l) = '\0';
            }

          e->color = GetColor (fcolor);
          e->partial = 0;
          e->next = NULL;

          if (!loglist)
            loglist = e;
          if (loglist_tail)
            loglist_tail->next = e;
          loglist_tail = e;
        }
    }

  if (!loglist)
    {
      fprintf (stderr, "You did not specify any files to tail\n"
               "use %s --help for help\n", argv[0]);
      exit (1);
    }

  if (opt_partial && opt_whole)
    {
      fprintf (stderr, "Specify at most one of -partial and -whole\n");
      exit (1);
    }

  if (opt_partial)
    /* if we specifically requested to see partial lines then don't insist on whole lines */
    opt_whole = 0;
  else if (file_count > 1)
    /* otherwise, if we've viewing multiple files, default to showing whole lines */
    opt_whole = 1;

#if HAS_REGEX
  if (transform)
    {
      int i;

      transformre = xmalloc (sizeof (transformre));
      i = regcomp (&transformre, transform, REG_EXTENDED);
      if (i != 0)
        {
          char buf[512];

          regerror (i, &transformre, buf, sizeof (buf));
          fprintf (stderr, "Cannot compile regular expression: %s\n", buf);
        }
    }
#endif

  InitWindow ();

  install_signal (SIGINT, blank_window);
  install_signal (SIGQUIT, blank_window);
  install_signal (SIGTERM, blank_window);
  install_signal (SIGHUP, force_reopen);
  install_signal (SIGUSR1, list_files);
  install_signal (SIGUSR2, force_refresh);

  if (opt_daemonize)
    daemonize ();

  main_loop ();

  exit (1);                     /* to make gcc -Wall stop complaining */
}

void
install_signal (int sig, void (*handler) (int))
{
  struct sigaction action;

  action.sa_handler = handler;
  sigemptyset (&action.sa_mask);
  action.sa_flags = SA_RESTART;

  if (sigaction (sig, &action, NULL) < 0)
    fprintf (stderr, "sigaction(%d): %s\n", sig, strerror (errno)), exit (1);
}

void *
xstrdup (const char *string)
{
  void *p;

  while ((p = strdup (string)) == NULL)
    {
      fprintf (stderr, "Memory exausted.");
      sleep (10);
    }

  return p;
}

void *
xmalloc (size_t size)
{
  void *p;

  while ((p = malloc (size)) == NULL)
    {
      fprintf (stderr, "Memory exausted.");
      sleep (10);
    }

  return p;
}

void
display_help (char *myname)
{
  printf ("Usage: %s [options] file1[,color[,desc]] "
          "[file2[,color[,desc]] ...]\n", myname);
  printf (" -g | -geometry geometry   -g WIDTHxHEIGHT+X+Y\n"
          " -color    color           use color $color as default\n"
          " -reload sec command       reload after $sec and run command\n"
          " -id id                    window id to use instead of the root window\n"
          " -font FONTSPEC            (-fn) font to use\n"
          " -f | -fork                fork into background\n"
          " -reverse                  print new lines at the top\n"
          " -whole                    wait for \\n before showing a line\n"
          " -partial                  show lines even if they don't end with a \\n\n"
          " -update                   allow updates to old partial lines\n"
          " -cont                     string to prefix continued partial lines with\n"
          "                           defaults to \"[+]\"\n"
          " -shade                    add shading to font\n"
          " -noinitial                don't display the last file lines on\n"
          "                           startup\n"
          " -i | -interval seconds    interval between checks (fractional\n"
          "                           values o.k.). Default 3 seconds\n"
          " -V                        display version information and exit\n"
          "\n");
  printf ("Example:\n%s -g 80x25+100+50 -font fixed /var/log/messages,green "
          "/var/log/secure,red,'ALERT'\n", myname);
  exit (0);
}

void
display_version (void)
{
  printf ("root-tail version " VERSION "\n");
  exit (0);
}

int
daemonize (void)
{
  switch (fork ())
    {
    case -1:
      return -1;
    case 0:
      break;
    default:
      _exit (0);
    }

  if (setsid () == -1)
    return -1;

  return 0;
}
