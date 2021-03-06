#ifndef TERMPRIV_H
#define TERMPRIV_H

/*
 * Internal terminal functions, types and structs.
 */

#include "term.h"

#define incpos(p) ((p).x == term->cols ? ((p).x = 0, (p).y++, 1) : ((p).x++, 0))
#define decpos(p) ((p).x == 0 ? ((p).x = term->cols, (p).y--, 1) : ((p).x--, 0))

#define poslt(p1,p2) ((p1).y < (p2).y || ((p1).y == (p2).y && (p1).x < (p2).x))
#define posle(p1,p2) ((p1).y < (p2).y || ((p1).y == (p2).y && (p1).x <= (p2).x))
#define poseq(p1,p2) ((p1).y == (p2).y && (p1).x == (p2).x)
#define posdiff(p1,p2) (((p1).y - (p2).y) * (term->cols + 1) + (p1).x - (p2).x)

/* Product-order comparisons for rectangular block selection. */
#define posPlt(p1,p2) ((p1).y <= (p2).y && (p1).x < (p2).x)
#define posPle(p1,p2) ((p1).y <= (p2).y && (p1).x <= (p2).x)

extern void term_print_finish(struct term* term);

extern void term_schedule_cblink(struct term* term);
extern void term_schedule_vbell(struct term* term, int already_started, int startpoint);

extern void term_switch_screen(struct term* term, bool to_alt, bool reset);
extern void term_check_boundary(struct term* term, int x, int y);
extern void term_do_scroll(struct term* term, int topline, int botline, int lines, bool sb);
extern void term_erase(struct term* term, bool selective, bool line_only, bool from_begin, bool to_end);
extern int  term_last_nonempty_line(struct term* term);

/* Bidi paragraph support */
extern void clear_wrapcontd(struct term* term, termline * line, int y);
extern ushort getparabidi(termline * line);
extern wchar * wcsline(struct term* term, termline * line);  // for debug output

static inline bool
term_selecting(struct term* term)
{ return term->mouse_state < 0 && term->mouse_state >= MS_SEL_LINE; }

extern void term_update_cs(struct term* term);

extern int termchars_equal(termchar * a, termchar * b);
extern int termchars_equal_override(termchar * a, termchar * b, uint bchr, cattr battr);
extern int termattrs_equal_fg(cattr * a, cattr * b);

extern void copy_termchar(termline * destline, int x, termchar * src);
extern void move_termchar(termline * line, termchar * dest, termchar * src);

extern void add_cc(termline *, int col, wchar chr, cattr attr);
extern void clear_cc(termline *, int col);

extern uchar * compressline(termline *);
extern termline * decompressline(uchar *, int * bytes_used);

extern termchar * term_bidi_line(struct term* term, termline *, int scr_y);

extern void term_export_html(bool do_open);
extern char * term_get_html(int level);
extern void print_screen(void);

extern int putlink(char * link);
extern char * geturl(int n);

#endif
