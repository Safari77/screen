/* Copyright (c) 2008, 2009
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 *      Micah Cowan (micah@cowan.name)
 *      Sadrul Habib Chowdhury (sadrul@users.sourceforge.net)
 * Copyright (c) 1993-2002, 2003, 2005, 2006, 2007
 *      Juergen Weigert (jnweiger@immd4.informatik.uni-erlangen.de)
 *      Michael Schroeder (mlschroe@immd4.informatik.uni-erlangen.de)
 * Copyright (c) 1987 Oliver Laumann
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (see the file COPYING); if not, see
 * https://www.gnu.org/licenses/, or contact Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 *
 ****************************************************************
 */

#include "config.h"
#include "input.h"
#include <stddef.h>
#include "screen.h"
#include "misc.h"
#include <wchar.h>
#include <wctype.h>
#include <stdlib.h>
#include <string.h>

#define INPUTLINE (flayer->l_height - 1)

static void InpProcess(char **, size_t *);
static void InpAbort(void);
static void InpRedisplayLine(int, int, int, int);

struct inpline {
	char buf[MAXSTR + 1];	/* text buffer */
	size_t len;		/* length of the editable string in bytes */
	size_t pos;		/* cursor position in editable string in bytes */
	struct inpline *next, *prev;
};

/* 'inphist' is used to store the current input when scrolling through history.
 * inpline->prev == history-prev
 * inpline->next == history-next
 */
static struct inpline inphist;

struct inpdata {
	struct inpline inp;
	size_t inpmaxlen;		/* MAXSTR, or less, if caller has shorter buffer */
	char *inpstring;		/* the prompt */
	size_t inpstringlen;	/* visual width of the prompt */
	int inpmode;			/* INP_NOECHO, INP_RAW, INP_EVERY */
	void (*inpfinfunc) (char *buf, size_t len, void *priv);
	char *priv;				/* private data for finfunc */
	int privdata;			/* private data space */
	char *search;			/* the search string */
};

static const struct LayFuncs InpLf = {
	InpProcess,
	InpAbort,
	InpRedisplayLine,
	DefClearLine,
	DefResize,
	DefRestore,
	NULL
};

/*
 * glibc helper: Safely calculate visual width of a UTF-8 string portion
 */
static int utf8_strwidth(const char *s, int bytes) {
	int width = 0;
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	int i = 0;
	while (i < bytes) {
		wchar_t wc;
		size_t res = mbrtowc(&wc, s + i, bytes - i, &ps);
		if (res == (size_t)-2) {
			break; /* Incomplete sequence at the end, wait for more bytes */
		} else if (res == (size_t)-1) {
			width += 1;
			i++;
			memset(&ps, 0, sizeof(ps));
		} else if (res == 0) {
			break;
		} else {
			int w = wcwidth(wc);
			width += (w >= 0 ? w : 1);
			i += res;
		}
	}
	return width;
}

/*
 * glibc helpers: Find previous/next valid UTF-8 character boundaries
 */
static size_t prev_char_boundary(const char *buf, size_t pos) {
	size_t i = 0, last = 0;
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	while (i < pos) {
		last = i;
		size_t res = mbrlen(buf + i, pos - i, &ps);
		if (res == (size_t)-1 || res == (size_t)-2 || res == 0) {
			i++;
			memset(&ps, 0, sizeof(ps));
		} else {
			i += res;
		}
	}
	return last;
}

static size_t next_char_boundary(const char *buf, size_t len, size_t pos) {
	if (pos >= len) return len;
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	size_t res = mbrlen(buf + pos, len - pos, &ps);
	if (res == (size_t)-1 || res == (size_t)-2 || res == 0) return pos + 1;
	return pos + res;
}

/*
 * Helper: Converts raw UTF-8 into uint32_t Unicode Points and injects
 * directly into Layer via LPutChar cells for proper rendering.
 */
static int LPutUtf8Str(Layer *l, const char *s, int bytes, int x, int y, int xs, int xe) {
	mbstate_t ps;
	memset(&ps, 0, sizeof(ps));
	int curr_x = x;
	int i = 0;

	while (i < bytes) {
		wchar_t wc;
		size_t res = mbrtowc(&wc, s + i, bytes - i, &ps);

		if (res == (size_t)-2) {
			break; /* Incomplete sequence, gracefully hold drawing to avoid garbage output */
		} else if (res == (size_t)-1) {
			struct mchar mc = mchar_so;
			mc.image = (uint32_t)(unsigned char)s[i];
			if (curr_x >= xs && curr_x <= xe) LPutChar(l, &mc, curr_x, y);
			curr_x++;
			i++;
			memset(&ps, 0, sizeof(ps));
		} else if (res == 0) {
			break;
		} else {
			int w = wcwidth(wc);
			if (w < 0) w = 1;

			struct mchar mc = mchar_so;
			mc.image = (uint32_t)wc;

			if (curr_x >= xs && curr_x <= xe) LPutChar(l, &mc, curr_x, y);

			/* Explicitly populate the adjacent cell to prevent artifacts for CJK/Emojis */
			if (w == 2) {
				struct mchar mc_dummy = mchar_so;
				mc_dummy.image = 0xFFFD; /* Screen relies on 0xFFFD to pad right-half of double-width glyphs */
				if (curr_x + 1 >= xs && curr_x + 1 <= xe) LPutChar(l, &mc_dummy, curr_x + 1, y);
			}

			curr_x += (w > 0 ? w : 1);
			i += res;
		}
	}
	return curr_x;
}

/* called once, after InitOverlayPage in Input() or Isearch() */
void inp_setprompt(char *p, char *s)
{
	struct inpdata *inpdata = (struct inpdata *)flayer->l_data;
	if (p) {
		inpdata->inpstringlen = utf8_strwidth(p, strlen(p));
		inpdata->inpstring = p;
	}
	if (s) {
		if (s != inpdata->inp.buf)
			strncpy(inpdata->inp.buf, s, ARRAY_SIZE(inpdata->inp.buf) - 1);
		inpdata->inp.buf[ARRAY_SIZE(inpdata->inp.buf) - 1] = 0;
		inpdata->inp.pos = inpdata->inp.len = strlen(inpdata->inp.buf);
	}
	InpRedisplayLine(INPUTLINE, 0, flayer->l_width - 1, 0);
	flayer->l_x = inpdata->inpstringlen + (inpdata->inpmode & INP_NOECHO ? 0 : utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos));
	flayer->l_y = INPUTLINE;
}

/*
 * We dont use HS status line with Input().
 * If we would use it, then we should check e_tgetflag("es") if
 * we are allowed to use esc sequences there.
 *
 * mode is an OR of
 * INP_NOECHO == suppress echoing of characters.
 * INP_RAW    == raw mode. call finfunc after each character typed.
 * INP_EVERY  == digraph mode.
 */
void Input(char *istr, size_t len, int mode, void (*finfunc) (char *buf, size_t len, void *priv), char *priv, int data)
{
	size_t maxlen;
	struct inpdata *inpdata;

	if (!flayer) return;
	if (len > MAXSTR) len = MAXSTR;

	if (!(mode & INP_NOECHO)) {
		maxlen = flayer->l_width - 1 - (istr ? strlen(istr) : 0);
		if (len > maxlen) len = maxlen;
	}
	if (InitOverlayPage(sizeof(struct inpdata), &InpLf, 1)) return;

	flayer->l_mode = 1;
	flayer->l_encoding = display ? display->d_encoding : 0; /* Inherit terminal encoding to prevent recasting */

	inpdata = (struct inpdata *)flayer->l_data;
	inpdata->inpmaxlen = len;
	inpdata->inpfinfunc = finfunc;
	inpdata->inp.pos = inpdata->inp.len = 0;
	inpdata->inp.prev = inphist.prev;
	inpdata->inpmode = mode;
	inpdata->privdata = data;
	if (!priv) priv = (char *)&inpdata->privdata;
	inpdata->priv = priv;
	inpdata->inpstringlen = 0;
	inpdata->inpstring = NULL;
	inpdata->search = NULL;

	if (istr) inp_setprompt(istr, (char *)NULL);
}

static void InpProcess(char **ppbuf, size_t *plen)
{
	int len, cx;
	char *pbuf;
	char ch;
	struct inpdata *inpdata = (struct inpdata *)flayer->l_data;
	Display *inpdisplay = display;
	int prev, next, search = 0;

#define RESET_SEARCH do { if (inpdata->search) { free(inpdata->search); inpdata->search = NULL; } } while (0)

	cx = inpdata->inpstringlen + (inpdata->inpmode & INP_NOECHO ? 0 : utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos));
	LGotoPos(flayer, cx, INPUTLINE);

	if (ppbuf == NULL) {
		InpAbort();
		return;
	}

	len = *plen;
	pbuf = *ppbuf;

	while (len) {
		char *p = inpdata->inp.buf + inpdata->inp.pos;
		ch = *pbuf++;
		len--;
		unsigned char uch = (unsigned char)ch;

		if (inpdata->inpmode & INP_EVERY) {
			inpdata->inp.buf[inpdata->inp.len] = ch;
			if (ch) {
				display = inpdisplay;
				(*inpdata->inpfinfunc) (inpdata->inp.buf, inpdata->inp.len, inpdata->priv);
				ch = inpdata->inp.buf[inpdata->inp.len];
			}
		} else if (inpdata->inpmode & INP_RAW) {
			display = inpdisplay;
			(*inpdata->inpfinfunc) (&ch, 1, inpdata->priv);
			if (ch) continue;
		}

		/* Accept any byte >= 32, including high UTF-8 bytes (128-255), skipping 127 (DEL) */
		if ((uch >= ' ' && uch != 0177) && inpdata->inp.len < inpdata->inpmaxlen) {
			if (inpdata->inp.len > inpdata->inp.pos)
				memmove(p + 1, p, inpdata->inp.len - inpdata->inp.pos);
			inpdata->inp.buf[inpdata->inp.pos++] = ch;
			inpdata->inp.len++;

			if (!(inpdata->inpmode & INP_NOECHO)) {
				InpRedisplayLine(INPUTLINE, 0, flayer->l_width - 1, 0);
				int new_cx = inpdata->inpstringlen + utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos);
				LGotoPos(flayer, new_cx, INPUTLINE);
			}
			RESET_SEARCH;
		} else if ((ch == '\b' || ch == 0177) && inpdata->inp.pos > 0) {
			size_t new_pos = prev_char_boundary(inpdata->inp.buf, inpdata->inp.pos);
			size_t chng = inpdata->inp.pos - new_pos;
			memmove(inpdata->inp.buf + new_pos, inpdata->inp.buf + inpdata->inp.pos, inpdata->inp.len - inpdata->inp.pos);
			inpdata->inp.pos = new_pos;
			inpdata->inp.len -= chng;
			if (!(inpdata->inpmode & INP_NOECHO)) {
				InpRedisplayLine(INPUTLINE, 0, flayer->l_width - 1, 0);
				int new_cx = inpdata->inpstringlen + utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos);
				LGotoPos(flayer, new_cx, INPUTLINE);
			}
			RESET_SEARCH;
		} else if (ch == '\025') {	/* CTRL-U */
			if (inpdata->inp.len > 0) {
				memmove(inpdata->inp.buf, inpdata->inp.buf + inpdata->inp.pos, inpdata->inp.len - inpdata->inp.pos);
				inpdata->inp.len -= inpdata->inp.pos;
				inpdata->inp.pos = 0;
				if (!(inpdata->inpmode & INP_NOECHO)) {
					InpRedisplayLine(INPUTLINE, 0, flayer->l_width - 1, 0);
					LGotoPos(flayer, inpdata->inpstringlen, INPUTLINE);
				}
			}
		} else if (ch == '\013') {	/* CTRL-K */
			if (inpdata->inp.len > inpdata->inp.pos) {
				inpdata->inp.len = inpdata->inp.pos;
				if (!(inpdata->inpmode & INP_NOECHO)) {
					InpRedisplayLine(INPUTLINE, 0, flayer->l_width - 1, 0);
					int new_cx = inpdata->inpstringlen + utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos);
					LGotoPos(flayer, new_cx, INPUTLINE);
				}
			}
		} else if (ch == '\027' && inpdata->inp.pos > 0) {	/* CTRL-W */
			size_t p_idx = inpdata->inp.pos;
			while (p_idx > 0 && inpdata->inp.buf[p_idx - 1] == ' ') p_idx--;
			while (p_idx > 0) {
				size_t prev_pos = prev_char_boundary(inpdata->inp.buf, p_idx);
				if (inpdata->inp.buf[prev_pos] == ' ') break;
				p_idx = prev_pos;
			}
			size_t chng = inpdata->inp.pos - p_idx;
			memmove(inpdata->inp.buf + p_idx, inpdata->inp.buf + inpdata->inp.pos, inpdata->inp.len - inpdata->inp.pos);
			inpdata->inp.pos = p_idx;
			inpdata->inp.len -= chng;
			if (!(inpdata->inpmode & INP_NOECHO)) {
				InpRedisplayLine(INPUTLINE, 0, flayer->l_width - 1, 0);
				int new_cx = inpdata->inpstringlen + utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos);
				LGotoPos(flayer, new_cx, INPUTLINE);
			}
			RESET_SEARCH;
		} else if (ch == '\004' && inpdata->inp.pos < inpdata->inp.len) {	/* CTRL-D */
			size_t next_p = next_char_boundary(inpdata->inp.buf, inpdata->inp.len, inpdata->inp.pos);
			size_t chng = next_p - inpdata->inp.pos;
			memmove(inpdata->inp.buf + inpdata->inp.pos, inpdata->inp.buf + next_p, inpdata->inp.len - next_p);
			inpdata->inp.len -= chng;
			if (!(inpdata->inpmode & INP_NOECHO)) {
				InpRedisplayLine(INPUTLINE, 0, flayer->l_width - 1, 0);
				int new_cx = inpdata->inpstringlen + utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos);
				LGotoPos(flayer, new_cx, INPUTLINE);
			}
			RESET_SEARCH;
		} else if (ch == '\001' || (unsigned char)ch == 0201) {	/* CTRL-A */
			inpdata->inp.pos = 0;
			LGotoPos(flayer, inpdata->inpstringlen, INPUTLINE);
		} else if ((ch == '\002' || (unsigned char)ch == 0202) && inpdata->inp.pos > 0) {	/* CTRL-B */
			inpdata->inp.pos = prev_char_boundary(inpdata->inp.buf, inpdata->inp.pos);
			int new_cx = inpdata->inpstringlen + utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos);
			LGotoPos(flayer, new_cx, INPUTLINE);
		} else if (ch == '\005' || (unsigned char)ch == 0205) {	/* CTRL-E */
			inpdata->inp.pos = inpdata->inp.len;
			int new_cx = inpdata->inpstringlen + utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos);
			LGotoPos(flayer, new_cx, INPUTLINE);
		} else if ((ch == '\006' || (unsigned char)ch == 0206) && inpdata->inp.pos < inpdata->inp.len) {	/* CTRL-F */
			inpdata->inp.pos = next_char_boundary(inpdata->inp.buf, inpdata->inp.len, inpdata->inp.pos);
			int new_cx = inpdata->inpstringlen + utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos);
			LGotoPos(flayer, new_cx, INPUTLINE);
		} else if ((prev = ((ch == '\020' || (unsigned char)ch == 0220) &&	/* CTRL-P */
				    inpdata->inp.prev)) || (next = ((ch == '\016' || (unsigned char)ch == 0216) &&	/* CTRL-N */
								    inpdata->inp.next)) ||
			   (search = ((ch == '\022' || (unsigned char)ch == 0222) && inpdata->inp.prev))) {

			struct inpline *sel;
			int pos = -1;

			if (prev)
				sel = inpdata->inp.prev;
			else if (next)
				sel = inpdata->inp.next;
			else {
				inpdata->inp.buf[inpdata->inp.len] = 0;
				if (!inpdata->search)
					inpdata->search = SaveStr(inpdata->inp.buf);
				for (sel = inpdata->inp.prev; sel; sel = sel->prev) {
					char *f;
					if ((f = strstr(sel->buf, inpdata->search))) {
						pos = f - sel->buf;
						break;
					}
				}
				if (!sel)
					/* Did not find a match. Process the next input. */
					continue;
			}

			if ((prev || search) && !inpdata->inp.next)
				inphist = inpdata->inp;
			memmove(&inpdata->inp, sel, sizeof(struct inpline));
			if (pos != -1)
				inpdata->inp.pos = pos;
			if (inpdata->inp.len > inpdata->inpmaxlen)
				inpdata->inp.len = inpdata->inpmaxlen;
			if (inpdata->inp.pos > inpdata->inp.len)
				inpdata->inp.pos = inpdata->inp.len;

			if (!(inpdata->inpmode & INP_NOECHO))
				InpRedisplayLine(INPUTLINE, 0, flayer->l_width - 1, 0);

			int new_cx = inpdata->inpstringlen + (inpdata->inpmode & INP_NOECHO ? 0 : utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos));
			LGotoPos(flayer, new_cx, INPUTLINE);
		}
		else if (ch == '\003' || ch == '\007' || ch == '\033' || ch == '\000' || ch == '\n' || ch == '\r') {
			if (ch != '\n' && ch != '\r')
				inpdata->inp.len = 0;
			inpdata->inp.buf[inpdata->inp.len] = 0;

			if (inpdata->inp.len && !(inpdata->inpmode & (INP_NOECHO | INP_RAW))) {
				struct inpline *store;

				/* Look for a duplicate first */
				for (store = inphist.prev; store; store = store->prev) {
					if (strcmp(store->buf, inpdata->inp.buf) == 0) {
						if (store->next) store->next->prev = store->prev;
						if (store->prev) store->prev->next = store->next;
						store->pos = inpdata->inp.pos;
						break;
					}
				}

				if (!store) {
					store = malloc(sizeof(struct inpline));
					memmove(store, &inpdata->inp, sizeof(struct inpline));
				}
				store->next = &inphist;
				store->prev = inphist.prev;
				if (inphist.prev) inphist.prev->next = store;
				inphist.prev = store;
			}

			flayer->l_data = NULL;	/* so inpdata does not get freed */
			InpAbort();				/* redisplays... */
			*ppbuf = pbuf;
			*plen = len;
			display = inpdisplay;
			if ((inpdata->inpmode & INP_RAW) == 0)
				(*inpdata->inpfinfunc) (inpdata->inp.buf, inpdata->inp.len, inpdata->priv);
			else
				(*inpdata->inpfinfunc) (pbuf - 1, 0, inpdata->priv);
			if (inpdata->search) free(inpdata->search);
			free(inpdata);
			return;
		} else {
			/* The user was searching, and then pressed some non-control input. So reset
			 * the search string. */
			RESET_SEARCH;
		}
	}
	if (!(inpdata->inpmode & INP_RAW)) {
		flayer->l_x = inpdata->inpstringlen + (inpdata->inpmode & INP_NOECHO ? 0 : utf8_strwidth(inpdata->inp.buf, inpdata->inp.pos));
		flayer->l_y = INPUTLINE;
	}
	*ppbuf = pbuf;
	*plen = len;
}

static void InpAbort(void)
{
	LAY_CALL_UP(LayRedisplayLine(INPUTLINE, 0, flayer->l_width - 1, 0));
	ExitOverlayPage();
}

static void InpRedisplayLine(int y, int xs, int xe, int isblank)
{
	struct inpdata *inpdata = (struct inpdata *)flayer->l_data;

	if (y != INPUTLINE) {
		LAY_CALL_UP(LayRedisplayLine(y, xs, xe, isblank));
		return;
	}
	inpdata->inp.buf[inpdata->inp.len] = '\0';

	if (isblank) {
		LClearArea(flayer, xs, y, xe, y, 0, 0);
		return;
	}

	int curr_x = 0;

	if (inpdata->inpstring) {
		curr_x = LPutUtf8Str(flayer, inpdata->inpstring, strlen(inpdata->inpstring), curr_x, y, xs, xe);
	}

	if (!(inpdata->inpmode & INP_NOECHO)) {
		curr_x = LPutUtf8Str(flayer, inpdata->inp.buf, inpdata->inp.len, curr_x, y, xs, xe);
	}

	if (curr_x <= xe) {
		LClearArea(flayer, curr_x, y, xe, y, 0, 0);
	}
}
