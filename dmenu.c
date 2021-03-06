/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif
#include <X11/Xft/Xft.h>
#include <Imlib2.h>

#include <fribidi.h>

#include "drw.h"
#include "util.h"

/* macros */
#define INTERSECT(x,y,w,h,r)  (MAX(0, MIN((x)+(w),(r).x_org+(r).width)  - MAX((x),(r).x_org)) \
                             * MAX(0, MIN((y)+(h),(r).y_org+(r).height) - MAX((y),(r).y_org)))
#define LENGTH(X)             (sizeof X / sizeof X[0])
#define TEXTW(X)              (drw_fontset_getwidth(drw, (X)) + lrpad)
#define NUMBERSMAXDIGITS      100
#define NUMBERSBUFSIZE        (NUMBERSMAXDIGITS * 2) + 1

/* enums */
enum {
	SchemeNorm, SchemeSel, SchemeOut,
	SchemeBorder, SchemeLast
}; /* color schemes */

enum {
	LocTop, LocBottom, LocCenter, LocCursor,
	LocTopRight, LocTopLeft,
	LocBottomRight, LocBottomLeft
}; /* locations */

struct item {
	char *text;
	char *value;
	char *id;
	Icn icon;
	struct item *left, *right;
	int out;
};

static char numbers[NUMBERSBUFSIZE] = "";
static char text[BUFSIZ] = "";
static char fribidi_text[BUFSIZ] = "";
static char *embed;
static int bh, mw, mh;
static int dmx = 0; /* put dmenu at this x offset */
static int dmy = 0; /* put dmenu at this y offset (measured from the bottom if location == LocBottom) */
static unsigned int dmw = 0; /* make dmenu this wide */
static int inputw = 0, promptw, passwd = 0;
static int lrpad; /* sum of left and right padding */
static size_t cursor;
static struct item *items = NULL;
static struct item *matches, *matchend;
static struct item *prev, *curr, *next, *sel;
static int mon = -1, screen;
static int managed = 1;
static int bidi = 0;

static Atom clip, utf8;
static Display *dpy;
static Window root, parentwin, win;
static XIC xic;

static Drw *drw;
static Clr *scheme[SchemeLast];

#include "config.h"

static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;
static char *(*fstrstr)(const char *, const char *) = strstr;

static unsigned int
textw_clamp(const char *str, unsigned int n)
{
	unsigned int w = drw_fontset_getwidth_clamp(drw, str, n) + lrpad;
	return MIN(w, n);
}

static void
appenditem(struct item *item, struct item **list, struct item **last)
{
	if (*last)
		(*last)->right = item;
	else
		*list = item;

	item->left = *last;
	item->right = NULL;
	*last = item;
}

static void
calcoffsets(void)
{
	int i, n;

	if (lines > 0)
		n = lines * columns * bh;
	else
		n = mw - (promptw + inputw + TEXTW("<") + TEXTW(">"));
	/* calculate which items will begin the next page and previous page */
	for (i = 0, next = curr; next; next = next->right)
		if ((i += (lines > 0) ? bh : textw_clamp(next->text, n)) > n)
			break;
	for (i = 0, prev = curr; prev && prev->left; prev = prev->left)
		if ((i += (lines > 0) ? bh : textw_clamp(prev->left->text, n)) > n)
			break;
}

static int
max_textw(void)
{
	int len = 0;
	for (struct item *item = items; item && item->text; item++)
		len = MAX(TEXTW(item->text), len);
	return len;
}

static void
cleanup(void)
{
	size_t i;

	XUngrabKey(dpy, AnyKey, AnyModifier, root);
	for (i = 0; i < SchemeLast; i++)
		free(scheme[i]);
	for (i = 0; items && items[i].text; ++i) {
		free(items[i].text);
		if (items[i].icon.img) {
			imlib_context_set_image(items[i].icon.img);
			imlib_free_image();
		}
	}
	free(items);
	drw_free(drw);
	XSync(dpy, False);
	XCloseDisplay(dpy);
}

static char *
cistrstr(const char *h, const char *n)
{
	size_t i;

	if (!n[0])
		return (char *)h;

	for (; *h; ++h) {
		for (i = 0; n[i] && tolower((unsigned char)n[i]) ==
		            tolower((unsigned char)h[i]); ++i)
			;
		if (n[i] == '\0')
			return (char *)h;
	}
	return NULL;
}

static void
apply_fribidi(char *str)
{
	FriBidiStrIndex len = strlen(str);
	FriBidiChar logical[BUFSIZ];
	FriBidiChar visual[BUFSIZ];
	FriBidiParType base = FRIBIDI_PAR_ON;
	FriBidiCharSet charset;

	fribidi_text[0] = 0;
	if (len > 0) {
		charset = fribidi_parse_charset("UTF-8");
		len = fribidi_charset_to_unicode(charset, str, len, logical);
		fribidi_log2vis(logical, len, &base, visual, NULL, NULL, NULL);
		len = fribidi_unicode_to_charset(charset, visual, len, fribidi_text);
	}
}

static int
cmd_output(char *cmd, char *out)
{
	FILE *fp;
	char buf[512];
	size_t outlen;

	fp = popen(cmd, "r");
	if (fp == NULL) {
		return -1;
	}

	*out = '\0';
	while (fgets(buf, sizeof buf, fp) != NULL) {
        strcat(out, buf);
	}

	outlen = strlen(out);
	if (out[outlen - 1] == '\n')
		out[outlen - 1] = '\0';

	pclose(fp);
	return 0;
}

static int
drawitem(struct item *item, int x, int y, int w)
{
	int ret, icmdret, icx, icy;
	char ipath[1024];
	char icmd[sizeof ipath * 2];
	Imlib_Load_Error ierr;

	if (item == sel)
		drw_setscheme(drw, scheme[SchemeSel]);
	else if (item->out)
		drw_setscheme(drw, scheme[SchemeOut]);
	else
		drw_setscheme(drw, scheme[SchemeNorm]);

	if (bidi) {
		apply_fribidi(item->text);
		ret = drw_text(drw, x, y, w, bh, lrpad / 2, icon_size, fribidi_text, 0);
	} else {
		ret = drw_text(drw, x, y, w, bh, lrpad / 2, icon_size, item->text, 0);
	}

	if (icon_size > 0) {
		if (item->icon.img == NULL && !item->icon.loaded) {
			if (item->icon.fname != NULL) { // provided using inline --icon=
				item->icon.img =
					load_icon_image(drw, item->icon.fname, icon_size, &ierr);
			} else if (icon_command != NULL) { // -icmd option
				sprintf(icmd, "%s '%s'",
						icon_command, item->text); // TODO: escape '
				icmdret = cmd_output(icmd, ipath); // TODO: parallelize
				if (icmdret == 0)
					item->icon.img =
						load_icon_image(drw, ipath, icon_size, &ierr);
			} else { // default
				item->icon.img =
					load_icon_image(drw, item->text, icon_size, &ierr);
			}
			if (item->icon.img == NULL)
				item->icon.img
					= load_icon_image(drw, icon_fallback, icon_size, &ierr);
			item->icon.loaded = 1;

			if (ierr != IMLIB_LOAD_ERROR_NONE)
				fprintf(stderr, "warning: failed loading icon for %s\n",
						item->text);
		}

		if (icon_size > w)
			die("window width is too small or icons size is too large");

		icx = x + ((w - icon_size) / 2);
		icy = y + 2;
		if (item->icon.img != NULL && item->icon.loaded) {
			drw_icon(drw, item->icon, icx, icy);
		} else
			drw_rect(drw, icx, icy, icon_size, icon_size, 0, 0);
	}

	return ret;
}

static void
recalculatenumbers()
{
	return; // TODO: bring back later
	unsigned int numer = 0, denom = 0;
	struct item *item;
	if (matchend) {
		numer++;
		for (item = matchend; item && item->left; item = item->left)
			numer++;
	}
	for (item = items; item && item->text; item++)
		denom++;
	snprintf(numbers, NUMBERSBUFSIZE, "%d/%d", numer, denom);
}

static void
drawmenu(void)
{
	unsigned int curpos;
	struct item *item;
	int x = 0, y = 0, w;
	char *censort;

	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_rect(drw, 0, 0, mw, mh, 1, 1);

	if (prompt && *prompt) {
		drw_setscheme(drw, scheme[SchemeSel]);
		x = drw_text(drw, x, 0, promptw, bh - icon_size, lrpad / 2, 0, prompt, 0);
	}
	/* draw input field */
	w = (lines > 0 || !matches) ? mw - x : inputw;
	drw_setscheme(drw, scheme[SchemeNorm]);
	if (passwd) {
	        censort = ecalloc(1, sizeof(text));
		memset(censort, '.', strlen(text));
		drw_text(drw, x, 0, w, bh - icon_size, lrpad / 2, 0, censort, 0);
		free(censort);
	} else {
		if (bidi) {
			apply_fribidi(text);
			drw_text(drw, x, 0, w, bh - icon_size, lrpad / 2, 0, fribidi_text, 0);
		} else {
			drw_text(drw, x, 0, w, bh - icon_size, lrpad / 2, 0, text, 0);
		}
	}

	curpos = TEXTW(text) - TEXTW(&text[cursor]);
	if ((curpos += lrpad / 2 - 1) < w) {
		drw_setscheme(drw, scheme[SchemeNorm]);
		drw_rect(drw, x + curpos, 2, 2, bh - icon_size - 4, 1, 0);
	}

	recalculatenumbers();
	if (lines > 0) {
		/* draw grid */
		int i = 0;
		for (item = curr; item != next; item = item->right, i++)
			drawitem(
				item,
				x + ((i % columns) * ((mw - x) / columns)),
				y + (((i / columns) + 1) *  bh) - icon_size,
				(mw - x) / columns
			);
	} else if (matches) {
		/* draw horizontal list */
		x += inputw;
		w = TEXTW("<");
		if (curr->left) {
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, x, 0, w, bh, lrpad / 2, 0, "<", 0);
		}
		x += w;
		for (item = curr; item != next; item = item->right)
			x = drawitem(item, x, 0,
						 textw_clamp(item->text,
									 mw - x - TEXTW(">")- TEXTW(numbers)));
		if (next) {
			w = TEXTW(">");
			drw_setscheme(drw, scheme[SchemeNorm]);
			drw_text(drw, mw - w - TEXTW(numbers),
					 0, w, bh, lrpad / 2, 0, ">", 0);
		}
	}

	drw_setscheme(drw, scheme[SchemeNorm]);
	drw_text(drw, mw - TEXTW(numbers), 0,
			 TEXTW(numbers), bh - icon_size, lrpad / 2, 0, numbers, 0);
	drw_map(drw, win, 0, 0, mw, mh);
}

static void
grabfocus(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000  };
	Window focuswin;
	int i, revertwin;

	for (i = 0; i < 100; ++i) {
		XGetInputFocus(dpy, &focuswin, &revertwin);
		if (focuswin == win)
			return;
		XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
		nanosleep(&ts, NULL);
	}
	die("cannot grab focus");
}

static void
grabkeyboard(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000  };
	int i;

	if (embed || managed)
		return;
	/* try to grab keyboard, we may have to wait for another process to ungrab */
	for (i = 0; i < 1000; i++) {
		if (XGrabKeyboard(dpy, DefaultRootWindow(dpy), True, GrabModeAsync,
		                  GrabModeAsync, CurrentTime) == GrabSuccess)
			return;
		nanosleep(&ts, NULL);
	}
	die("cannot grab keyboard");
}

static void
match(void)
{
	static char **tokv = NULL;
	static int tokn = 0;

	char buf[sizeof text], *s;
	int i, tokc = 0;
	size_t len, textsize;
	struct item *item, *lprefix, *lsubstr, *prefixend, *substrend;

	strcpy(buf, text);
	/* separate input text into tokens to be matched individually */
	for (s = strtok(buf, " "); s; tokv[tokc - 1] = s, s = strtok(NULL, " "))
		if (++tokc > tokn && !(tokv = realloc(tokv, ++tokn * sizeof *tokv)))
			die("cannot realloc %zu bytes:", tokn * sizeof *tokv);
	len = tokc ? strlen(tokv[0]) : 0;

	matches = lprefix = lsubstr = matchend = prefixend = substrend = NULL;
	textsize = strlen(text) + 1;
	for (item = items; item && item->text; item++) {
		for (i = 0; i < tokc; i++)
			if (!fstrstr(item->text, tokv[i]))
				break;
		if (i != tokc) /* not all tokens match */
			continue;
		/* exact matches go first, then prefixes, then substrings */
		if (!tokc || !fstrncmp(text, item->text, textsize))
			appenditem(item, &matches, &matchend);
		else if (!fstrncmp(tokv[0], item->text, len))
			appenditem(item, &lprefix, &prefixend);
		else
			appenditem(item, &lsubstr, &substrend);
	}
	if (lprefix) {
		if (matches) {
			matchend->right = lprefix;
			lprefix->left = matchend;
		} else
			matches = lprefix;
		matchend = prefixend;
	}
	if (lsubstr) {
		if (matches) {
			matchend->right = lsubstr;
			lsubstr->left = matchend;
		} else
			matches = lsubstr;
		matchend = substrend;
	}
	curr = sel = matches;
	calcoffsets();
}

static void
insert(const char *str, ssize_t n)
{
	if (strlen(text) + n > sizeof text - 1)
		return;
	/* move existing text out of the way, insert new text, and update cursor */
	memmove(&text[cursor + n], &text[cursor], sizeof text - cursor - MAX(n, 0));
	if (n > 0)
		memcpy(&text[cursor], str, n);
	cursor += n;
	match();
}

static size_t
nextrune(int inc)
{
	ssize_t n;

	/* return location of next utf8 rune in the given direction (+1 or -1) */
	for (n = cursor + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
		;
	return n;
}

static void
movewordedge(int dir)
{
	if (dir < 0) { /* move cursor to the start of the word*/
		while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
		while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
			cursor = nextrune(-1);
	} else { /* move cursor to the end of the word */
		while (text[cursor] && strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
		while (text[cursor] && !strchr(worddelimiters, text[cursor]))
			cursor = nextrune(+1);
	}
}

static void
keypress(XKeyEvent *ev)
{
	char buf[32];
	int len;
	KeySym ksym;
	Status status;
	int i, offscreen = 0;
	struct item *tmpsel;

	len = XmbLookupString(xic, ev, buf, sizeof buf, &ksym, &status);
	switch (status) {
	default: /* XLookupNone, XBufferOverflow */
		return;
	case XLookupChars:
		goto insert;
	case XLookupKeySym:
	case XLookupBoth:
		break;
	}

	if (ev->state & ControlMask) {
		switch(ksym) {
		case XK_a: ksym = XK_Home;      break;
		case XK_b: ksym = XK_Left;      break;
		case XK_c: ksym = XK_Escape;    break;
		case XK_d: ksym = XK_Delete;    break;
		case XK_e: ksym = XK_End;       break;
		case XK_f: ksym = XK_Right;     break;
		case XK_g: ksym = XK_Escape;    break;
		case XK_h: ksym = XK_BackSpace; break;
		case XK_i: ksym = XK_Tab;       break;
		case XK_j: /* fallthrough */
		case XK_J: /* fallthrough */
		case XK_m: /* fallthrough */
		case XK_M: ksym = XK_Return; ev->state &= ~ControlMask; break;
		case XK_n: ksym = XK_Down;      break;
		case XK_p: ksym = XK_Up;        break;

		case XK_k: /* delete right */
			text[cursor] = '\0';
			match();
			break;
		case XK_u: /* delete left */
			insert(NULL, 0 - cursor);
			break;
		case XK_w: /* delete word */
			while (cursor > 0 && strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, nextrune(-1) - cursor);
			while (cursor > 0 && !strchr(worddelimiters, text[nextrune(-1)]))
				insert(NULL, nextrune(-1) - cursor);
			break;
		case XK_y: /* paste selection */
		case XK_Y:
			XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
			                  utf8, utf8, win, CurrentTime);
			return;
		case XK_Left:
		case XK_KP_Left:
			movewordedge(-1);
			goto draw;
		case XK_Right:
		case XK_KP_Right:
			movewordedge(+1);
			goto draw;
		case XK_Return:
		case XK_KP_Enter:
			break;
		case XK_bracketleft:
			cleanup();
			exit(1);
		default:
			return;
		}
	} else if (ev->state & Mod1Mask) {
		switch(ksym) {
		case XK_b:
			movewordedge(-1);
			goto draw;
		case XK_f:
			movewordedge(+1);
			goto draw;
		case XK_g: ksym = XK_Home;  break;
		case XK_G: ksym = XK_End;   break;
		case XK_h: ksym = XK_Up;    break;
		case XK_j: ksym = XK_Next;  break;
		case XK_k: ksym = XK_Prior; break;
		case XK_l: ksym = XK_Down;  break;
		default:
			return;
		}
	}

	switch(ksym) {
	default:
insert:
		if (!iscntrl((unsigned char)*buf))
			insert(buf, len);
		break;
	case XK_Delete:
	case XK_KP_Delete:
		if (text[cursor] == '\0')
			return;
		cursor = nextrune(+1);
		/* fallthrough */
	case XK_BackSpace:
		if (cursor == 0)
			return;
		insert(NULL, nextrune(-1) - cursor);
		break;
	case XK_End:
	case XK_KP_End:
		if (text[cursor] != '\0') {
			cursor = strlen(text);
			break;
		}
		if (next) {
			/* jump to end of list and position items in reverse */
			curr = matchend;
			calcoffsets();
			curr = prev;
			calcoffsets();
			while (next && (curr = curr->right))
				calcoffsets();
		}
		sel = matchend;
		break;
	case XK_Escape:
		cleanup();
		exit(1);
	case XK_Home:
	case XK_KP_Home:
		if (sel == matches) {
			cursor = 0;
			break;
		}
		sel = curr = matches;
		calcoffsets();
		break;
	case XK_Left:
	case XK_KP_Left:
		if ((columns > 1 || lines == 0) &&
			sel && sel->left && (sel = sel->left)->right == curr) {
			curr = prev;
			calcoffsets();
            break;
		}
		if (cursor > 0 && (!sel || !sel->left || lines > 0)) {
			cursor = nextrune(-1);
			break;
		}
		break;
	case XK_Up:
	case XK_KP_Up:
		if (!sel)
			return;
		tmpsel = sel;
		for (i = 0; i < columns; i++) {
			if (!tmpsel->left || tmpsel->left->right != tmpsel) {
				if (offscreen)
					break;
				return;
			}
			if (tmpsel == curr)
				offscreen = 1;
			tmpsel = tmpsel->left;
		}
		sel = tmpsel;
		if (offscreen) {
			curr = prev;
			calcoffsets();
		}
		break;
	case XK_Next:
	case XK_KP_Next:
		if (!next)
			return;
		sel = curr = next;
		calcoffsets();
		break;
	case XK_Prior:
	case XK_KP_Prior:
		if (!prev)
			return;
		sel = curr = prev;
		calcoffsets();
		break;
	case XK_Return:
	case XK_KP_Enter:
		puts((sel && !(ev->state & ShiftMask)) ?
			 (sel->value == NULL ? sel->text : sel->value)
			 : text);
		if (!(ev->state & ControlMask)) {
			cleanup();
			exit(0);
		}
		if (sel)
			sel->out = 1;
		break;
	case XK_Right:
	case XK_KP_Right:
		if ((columns > 1 || lines == 0) &&
			sel && sel->right && (sel = sel->right) == next) {
			curr = next;
			calcoffsets();
			break;
		}
		if (text[cursor] != '\0') {
			cursor = nextrune(+1);
			break;
		}
		break;
	case XK_Down:
	case XK_KP_Down:
		if (!sel)
			return;
		tmpsel = sel;
		for (i = 0; i < columns; i++) {
			if (!tmpsel->right || tmpsel->right->left != tmpsel) {
				if (offscreen)
					break;
				return;
			}
			tmpsel = tmpsel->right;
			if (tmpsel == next)
				offscreen = 1;
		}
		sel = tmpsel;
		if (offscreen) {
			curr = next;
			calcoffsets();
		}
		break;
	case XK_Tab:
		if (!sel)
			return;
		strncpy(text, sel->text, sizeof text - 1);
		text[sizeof text - 1] = '\0';
		cursor = strlen(text);
		match();
		break;
	}

draw:
	drawmenu();
}

static void
buttonpress(XEvent *e)
{
	struct item *item;
	XButtonPressedEvent *ev = &e->xbutton;
	int x = 0, y = -icon_size, h = bh, w, item_num = 0;

	if (ev->window != win)
		return;

	/* right-click: exit */
	if (ev->button == Button3)
		exit(1);

	if (prompt && *prompt)
		x += promptw;

	/* input field */
	w = (lines > 0 || !matches) ? mw - x : inputw;

	/* left-click on input: clear input,
	 * NOTE: if there is no left-arrow the space for < is reserved so
	 *		 add that to the input width */
	if (ev->button == Button1 &&
		((lines <= 0 && ev->x >= 0 && ev->x <= x + w +
		  ((!prev || !curr->left) ? TEXTW("<") : 0)) ||
		 (lines > 0 && ev->y >= y && ev->y <= y + h))) {
		insert(NULL, -cursor);
		drawmenu();
		return;
	}
	/* middle-mouse click: paste selection */
	if (ev->button == Button2) {
		XConvertSelection(dpy, (ev->state & ShiftMask) ? clip : XA_PRIMARY,
						  utf8, utf8, win, CurrentTime);
		drawmenu();
		return;
	}
	/* scroll up */
	if (ev->button == Button4 && prev) {
		sel = curr = prev;
		calcoffsets();
		drawmenu();
		return;
	}
	/* scroll down */
	if (ev->button == Button5 && next) {
		sel = curr = next;
		calcoffsets();
		drawmenu();
		return;
	}
	if (ev->button != Button1)
		return;
	if (ev->state & ~ControlMask)
		return;
	if (lines > 0) {
		/* vertical list: (ctrl)left-click on item */
		w = (mw - promptw) / columns;
		for (item = curr; item != next; item = item->right) {
			x = promptw + ((item_num % columns) * w);
			y = (((item_num / columns) + 1) *  h) - icon_size;
			if (ev->y >= y && ev->y <= (y + h) &&
				ev->x >= x && ev->x <= (x + w)) {
				puts((sel->value == NULL ? sel->text : sel->value));
				if (!(ev->state & ControlMask))
					exit(0);
				sel = item;
				if (sel) {
					sel->out = 1;
					drawmenu();
				}
				return;
			}
			++item_num;
		}
	} else if (matches) {
		/* left-click on left arrow */
		x += inputw;
		w = TEXTW("<");
		if (prev && curr->left) {
			if (ev->x >= x && ev->x <= x + w) {
				sel = curr = prev;
				calcoffsets();
				drawmenu();
				return;
			}
		}
		/* horizontal list: (ctrl)left-click on item */
		for (item = curr; item != next; item = item->right) {
			x += w;
			w = MIN(TEXTW(item->text), mw - x - TEXTW(">"));
			if (ev->x >= x && ev->x <= x + w) {
				puts((sel->value == NULL ? sel->text : sel->value));
				if (!(ev->state & ControlMask))
					exit(0);
				sel = item;
				if (sel) {
					sel->out = 1;
					drawmenu();
				}
				return;
			}
		}
		/* left-click on right arrow */
		w = TEXTW(">");
		x = mw - w;
		if (next && ev->x >= x && ev->x <= x + w) {
			sel = curr = next;
			calcoffsets();
			drawmenu();
			return;
		}
	}
}

static void
mousemove(XEvent *e)
{
	struct item *item;
	XPointerMovedEvent *ev = &e->xmotion;
	int x = 0, y = 0, h = bh, w, item_num = 0;

	if (lines > 0) {
		w = (mw - promptw) / columns;
		for (item = curr; item != next; item = item->right) { // TODO: too slow
			x = promptw + ((item_num % columns) * w);
			y = (((item_num / columns) + 1) *  h) - icon_size;
			if (ev->y >= y && ev->y <= (y + h) &&
				ev->x >= x && ev->x <= (x + w)) {
				sel = item;
				calcoffsets();
				drawmenu();
				return;
			}
			++item_num;
		}
	} else if (matches) {
		x += inputw + promptw;
		w = TEXTW("<");
		for (item = curr; item != next; item = item->right) {
			x += w;
			w = MIN(TEXTW(item->text), mw - x - TEXTW(">"));
			if (ev->x >= x && ev->x <= x + w) {
				sel = item;
				calcoffsets();
				drawmenu();
				return;
			}
		}
	}
}

static void
paste(void)
{
	char *p, *q;
	int di;
	unsigned long dl;
	Atom da;

	/* we have been given the current selection, now insert it into input */
	if (XGetWindowProperty(dpy, win, utf8, 0, (sizeof text / 4) + 1, False,
						   utf8, &da, &di, &dl, &dl, (unsigned char **)&p)
		== Success && p) {
		insert(p, (q = strchr(p, '\n')) ? q - p : (ssize_t)strlen(p));
		XFree(p);
	}
	drawmenu();
}

static void
parseline(struct item *item, char *line)
{
	char *text, *val, *dupped;
	int found_opts = 1;

	const char *options[] = {"--icon=", "--value=", "--id="};

	text = line;
	item->value = NULL;
	item->icon.fname = NULL;
	item->icon.img = NULL;
	item->icon.loaded = 0;
	item->out = 0;

	// TODO: build a sane parse
	while (strncmp(text, "--", 2) == 0) {
		for (int i = 0; i < sizeof options / sizeof options[0]; ++i) {
			if (strncmp(text, options[i], strlen(options[i])) == 0) {
				val = strtok(found_opts ? text : NULL, " ") +
					strlen(options[i]);
				if (!(dupped = strdup(val)))
					die("cannot strdup %zu bytes:", strlen(val) + 1);

				switch (i) {
				case 0: item->icon.fname = dupped; break;
				case 1: item->value      = dupped; break;
				case 2: item->id         = dupped; break;
				}

				text += strlen(options[i]) + strlen(dupped) + 1;
				found_opts = 0;
				break;
			}
		}
	}

	if (!(item->text = strdup(text)))
		die("cannot strdup %zu bytes:", strlen(text) + 1);
}

static void
readstdin(void)
{
	char buf[sizeof text], *p;
	size_t i, size = 0;

	if (passwd) {
		inputw = lines = 0;
		return;
	}

	/* read each line from stdin and add it to the item list */
	for (i = 0; fgets(buf, sizeof buf, stdin); i++) {
		if (i + 1 >= size / sizeof *items)
			if (!(items = realloc(items, (size += BUFSIZ))))
				die("cannot realloc %zu bytes:", size);
		if ((p = strchr(buf, '\n')))
			*p = '\0';

		parseline(&items[i], buf);
	}
	if (items)
		items[i].text = NULL;
	lines = MIN(lines, i);
}

static void
run(void)
{
	XEvent ev;
	int i;

	while (!XNextEvent(dpy, &ev)) {
		if (preselected) {
			if (preselected < 0)
				preselected = lines + preselected;
			for (i = 0; i < preselected; i++) {
				if (sel && sel->right && (sel = sel->right) == next) {
					curr = next;
					calcoffsets();
				}
			}
			drawmenu();
			preselected = 0;
		}

		if (XFilterEvent(&ev, win))
			continue;
		switch(ev.type) {
		case DestroyNotify:
			if (ev.xdestroywindow.window != win)
				break;
			cleanup();
			exit(1);
		case ButtonPress:
			buttonpress(&ev);
			break;
		case MotionNotify:
			mousemove(&ev);
			break;
		case Expose:
			if (ev.xexpose.count == 0)
				drw_map(drw, win, 0, 0, mw, mh);
			break;
		case FocusIn:
			/* regrab focus from parent window */
			if (ev.xfocus.window != win)
				grabfocus();
			break;
		case FocusOut: // TODO: fix this for empty desktops
			//cleanup();
			//exit(1);
			break;
		case KeyPress:
			keypress(&ev.xkey);
			break;
		case SelectionNotify:
			if (ev.xselection.property == utf8)
				paste();
			break;
		case VisibilityNotify:
			if (ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, win);
			break;
		}
	}
}

static void
cursor_pos(int *x, int *y)
{
	Window root_window;
	unsigned int mask;
	XQueryPointer(dpy, DefaultRootWindow(dpy),
				  &root_window, &root_window,
				  x, y, x, y, &mask);
}

static void
setup(void)
{
	int x, y, i, j;
	unsigned int du, tmp;
	XSetWindowAttributes swa;
	XIM xim;
	Window w, dw, *dws;
	XWindowAttributes wa;
	XClassHint ch = {"dmenu", "dmenu"};
	struct item *item;
#ifdef XINERAMA
	XineramaScreenInfo *info;
	Window pw;
	int a, di, n, area = 0;
#endif
	/* init appearance */
	for (j = 0; j < SchemeLast; j++)
		scheme[j] = drw_scm_create(drw, colors[j], 2);

	clip = XInternAtom(dpy, "CLIPBOARD",   False);
	utf8 = XInternAtom(dpy, "UTF8_STRING", False);

	if (lines < 1)
		icon_size = 0;

	/* calculate menu geometry */
	bh = drw->fonts->h + 2 + icon_size;
	lines = MAX(lines, 0);
	mh = (lines + 1) * bh - icon_size;
	promptw = (prompt && *prompt) ? TEXTW(prompt) - lrpad / 4 : 0;

#ifdef XINERAMA
	i = 0;
	if (parentwin == root && (info = XineramaQueryScreens(dpy, &n))) {
		XGetInputFocus(dpy, &w, &di);
		if (mon >= 0 && mon < n)
			i = mon;
		else if (w != root && w != PointerRoot && w != None) {
			/* find top-level window containing current input focus */
			do {
				if (XQueryTree(dpy, (pw = w), &dw, &w, &dws, &du) && dws)
					XFree(dws);
			} while (w != root && w != pw);
			/* find xinerama screen with which the window intersects most */
			if (XGetWindowAttributes(dpy, pw, &wa))
				for (j = 0; j < n; j++)
					if ((a = INTERSECT(wa.x, wa.y, wa.width, wa.height, info[j])) > area) {
						area = a;
						i = j;
					}
		}
		/* no focused window is on screen, so use pointer location instead */
		if (mon < 0 && !area && XQueryPointer(dpy, root, &dw, &dw, &x, &y, &di, &di, &du))
			for (i = 0; i < n; i++)
				if (INTERSECT(x, y, 1, 1, info[i]) != 0)
					break;

		if (dmw > 0)
			mw = dmw;
		else if (location == LocTop || location == LocBottom)
			mw = info[i].width;
		else
			mw = MIN(MAX(max_textw() + promptw, min_width), info[i].width);

		x = info[i].x_org;
		y = info[i].y_org;

		switch (location) {
		case LocCenter:
			x = info[i].x_org + ((info[i].width  - mw) / 2);
			y = info[i].y_org + ((info[i].height - mh) / 2);
			break;
		case LocTop:
		case LocTopLeft:
			x += dmx;
			y += dmy;
			break;
		case LocTopRight:
			x += dmx + info[i].width - mw;
			y += dmy;
			break;
		case LocBottom:
		case LocBottomLeft:
			x += dmx;
			y += dmy + info[i].height - mh;
			break;
		case LocBottomRight:
			x += dmx + info[i].width - mw;
			y += dmy + info[i].height - mh;
			break;
		case LocCursor:
			cursor_pos(&x, &y);
			x -= mw / 2;
			x += dmx;
			y += dmy;
			break;
		}

		/* some extra space for the wm frame */
		if (managed) {
			if (location == LocBottom     ||
				location == LocBottomLeft ||
				location == LocBottomRight)
				y -= 2;
			else
				y += 1;
			mw -= 2;
		}

		XFree(info);
	} else
#endif
	{
		if (!XGetWindowAttributes(dpy, parentwin, &wa))
			die("could not get embedding window attributes: 0x%lx",
			    parentwin);

		if (location == LocCenter) {
			mw = MIN(MAX(max_textw() + promptw, min_width), wa.width);
			x = (wa.width  - mw) / 2;
			y = (wa.height - mh) / 2;
		} else {
			x = dmx;
			y = location != LocBottom ? dmy : wa.height - mh - dmy;
			mw = (dmw > 0 ? dmw : wa.width);
		}
	}
	for (item = items; item && item->text; ++item) {
		if ((tmp = textw_clamp(item->text, mw/3)) > inputw) {
			if ((inputw = tmp) == mw/3)
				break;
		}
	}
	match();

	/* create menu window */
	swa.override_redirect = managed ? False : True;
	swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
	swa.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask |
	                 ButtonPressMask | PointerMotionMask | FocusChangeMask;
	int bwidth = managed ? 0 : border_width;
	mw -= bwidth * 2;
	win = XCreateWindow(dpy, parentwin, x, y, mw, mh, bwidth,
	                    CopyFromParent, CopyFromParent, CopyFromParent,
	                    CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
	if (bwidth != 0)
		XSetWindowBorder(dpy, win, scheme[SchemeBorder][ColFg].pixel);
	XSetClassHint(dpy, win, &ch);

	/* imlib2 stuff */
	if (lines > 0) {
		/* set our cache to 2 Mb so it doesn't have to go hit the disk as long as */
		/* the images we use use less than 2Mb of RAM (that is uncompressed) */
		imlib_set_cache_size(2048 * 1024);
		/* set the maximum number of colors to allocate for 8bpp and less to 128 */
		imlib_set_color_usage(128);
		/* dither for depths < 24bpp */
		imlib_context_set_dither(1);
		/* set the display , visual, colormap and drawable we are using */
		imlib_context_set_display(dpy);
		imlib_context_set_visual(DefaultVisual(dpy, screen));
        imlib_context_set_colormap(DefaultColormap(dpy, screen));
	}

	/* input methods */
	if ((xim = XOpenIM(dpy, NULL, NULL, NULL)) == NULL)
		die("XOpenIM failed: could not open input device");

	xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
	                XNClientWindow, win, XNFocusWindow, win, NULL);

	XMapRaised(dpy, win);

	if (managed) {
		XTextProperty prop;
		char *windowtitle = prompt != NULL ? prompt : "Menu";
		Xutf8TextListToTextProperty(dpy, &windowtitle, 1, XUTF8StringStyle, &prop);
		XSetWMName(dpy, win, &prop);
		XSetTextProperty(dpy, win, &prop, XInternAtom(dpy, "_NET_WM_NAME", False));
		XFree(prop.value);
	} else {
		XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
	}

	if (embed) {
		XSelectInput(dpy, parentwin, FocusChangeMask | SubstructureNotifyMask);
		if (XQueryTree(dpy, parentwin, &dw, &w, &dws, &du) && dws) {
			for (i = 0; i < du && dws[i] != win; ++i)
				XSelectInput(dpy, dws[i], FocusChangeMask);
			XFree(dws);
		}
		grabfocus();
	}
	drw_resize(drw, mw, mh);
	drawmenu();
}

static void
usage(void)
{
	fputs("usage: dmenu [-bcCfiPv] [-l lines] [-p prompt] [-fn font] [-m monitor]\n"
	      "             [-x xoffset] [-y yoffset] [-z width]\n"
	      "             [-nb color] [-nf color] [-sb color] [-sf color]\n"
	      "             [-icmd command] [-isize size] [-bidi]\n"
	      "             [-w windowid] [-n number] [-nm]\n", stderr);
	exit(1);
}

int
main(int argc, char *argv[])
{
	XWindowAttributes wa;
	int i, fast = 0;

	char *envfont = getenv("FONT_SIZE");
	if (envfont == NULL || strcmp(envfont, "") == 0)
		envfont = getenv("FONT");
	if (envfont != NULL && strcmp(envfont, "") != 0)
		fonts[0] = envfont;

	for (i = 1; i < argc; i++)
		/* these options take no arguments */
		if (!strcmp(argv[i], "-v")) { /* prints version information */
			puts("dmenu-"VERSION"-naheel");
			exit(0);
		} else if (!strcmp(argv[i], "-b")) { /* appears at the bottom of the screen */
			location = LocBottom;
		} else if (!strcmp(argv[i], "-f")) { /* grabs keyboard before reading stdin */
			fast = 1;
		} else if (!strcmp(argv[i], "-i")) { /* case-insensitive item matching */
			fstrncmp = strncasecmp;
			fstrstr = cistrstr;
		} else if (!strcmp(argv[i], "-P")) { /* is the input a password */
			passwd = 1;
		} else if (!strcmp(argv[i], "-nm")) { /* do not display as managed wm window */
			managed = 0;
		} else if (!strcmp(argv[i], "-bidi")) {
			bidi = 1;
		} else if (i + 1 == argc) {
			usage();

		/* these options take one argument */
		} else if (!strcmp(argv[i], "-c")) { /* number of columns in grid */
			columns = atoi(argv[++i]);
			if (lines == 0) lines = 1;
		} else if (!strcmp(argv[i], "-l")) { /* number of lines in grid */
			lines = atoi(argv[++i]);
			if (columns == 0) columns = 1;
		} else if (!strcmp(argv[i], "-x")) { /* window x offset */
			dmx = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-y")) { /* window y offset (from bottom up if -b) */
			dmy = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-z")) { /* make dmenu this wide */
			dmw = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-m")) { /* monitor */
			mon = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-p")) { /* adds prompt to left of input field */
			prompt = argv[++i];
		} else if (!strcmp(argv[i], "-fn")) { /* font or font set */
			fonts[0] = argv[++i];
		} else if (!strcmp(argv[i], "-nb")) { /* normal background color */
			colors[SchemeNorm][ColBg] = argv[++i];
		} else if (!strcmp(argv[i], "-nf")) { /* normal foreground color */
			colors[SchemeNorm][ColFg] = argv[++i];
		} else if (!strcmp(argv[i], "-sb")) { /* selected background color */
			colors[SchemeSel][ColBg] = argv[++i];
		} else if (!strcmp(argv[i], "-sf")) { /* selected foreground color */
			colors[SchemeSel][ColFg] = argv[++i];
		} else if (!strcmp(argv[i], "-w")) { /* embedding window id */
			embed = argv[++i];
		} else if (!strcmp(argv[i], "-n")) { /* preselected item */
			preselected = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-L")) { /* dmenu location */
			char *arg = argv[++i];
			if (strcmp(arg, "top") == 0)
				location = LocTop;
			else if (strcmp(arg, "bottom") == 0)
				location = LocBottom;
			else if (strcmp(arg, "center") == 0)
				location = LocCenter;
			else if (strcmp(arg, "cursor") == 0)
				location = LocCursor;
			else if (strcmp(arg, "top-right") == 0)
				location = LocTopRight;
			else if (strcmp(arg, "top-left") == 0)
				location = LocTopLeft;
			else if (strcmp(arg, "bottom-right") == 0)
				location = LocBottomRight;
			else if (strcmp(arg, "bottom-left") == 0)
				location = LocBottomLeft;
			else
				die("unknown location");
		} else if (!strcmp(argv[i], "-icmd")) { /* icon command */
			icon_command = argv[++i];
		} else if (!strcmp(argv[i], "-isize")) { /* icon size */
			icon_size = atoi(argv[++i]);
		} else {
			usage();
		}

	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		fputs("warning: no locale support\n", stderr);
	if (!(dpy = XOpenDisplay(NULL)))
		die("cannot open display");
	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);
	if (!embed || !(parentwin = strtol(embed, NULL, 0)))
		parentwin = root;
	if (!XGetWindowAttributes(dpy, parentwin, &wa))
		die("could not get embedding window attributes: 0x%lx",
		    parentwin);
	drw = drw_create(dpy, screen, root, wa.width, wa.height);
	if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
		die("no fonts could be loaded.");
	lrpad = drw->fonts->h;

#ifdef __OpenBSD__
	if (pledge("stdio rpath", NULL) == -1)
		die("pledge");
#endif

	if (fast && !isatty(0)) {
		grabkeyboard();
		readstdin();
	} else {
		readstdin();
		grabkeyboard();
	}
	setup();
	run();

	return 1; /* unreachable */
}
