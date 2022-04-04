/* See LICENSE file for copyright and license details. */
/* Default settings; can be overriden by command line. */

static int topbar = 1;                      /* -b  option; if 0, dmenu appears at bottom     */
static int location = LocNormal;            /* -L option; dmenu location */
static int min_width = 300;                 /* minimum width */
/* -fn option overrides fonts[0]; default X11 font or font set */
static const char *fonts[] = {
	"monospace:size=14"
};
static const char *prompt = NULL;      /* -p  option; prompt to the left of input field */
static const char *colors[SchemeLast][2] = {
	/*     fg         bg       */
	[SchemeNorm]   = { "#bbbbbb", "#000000" },
	[SchemeSel]    = { "#000000", "#dddddd" },
	[SchemeOut]    = { "#000000", "#00ffff" },
	[SchemeBorder] = { "#333333", "#000000" },
};
/* -l option; if nonzero, dmenu uses vertical list with given number of lines */
static unsigned int lines = 0;

/*
 * Characters not considered part of a word while deleting words
 * for example: " /?\"&[]"
 */
static const char worddelimiters[] = " ";

/* Size of the window border */
static const unsigned int border_width = 1;

/* -n option; preselected item starting from 0 */
static int preselected = 0;
