// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include <stdbool.h>

#include "nvim/ascii.h"
#include "nvim/buffer_defs.h"
#include "nvim/charset.h"
#include "nvim/cursor.h"
#include "nvim/diff.h"
#include "nvim/fold.h"
#include "nvim/memline.h"
#include "nvim/misc1.h"
#include "nvim/mouse.h"
#include "nvim/move.h"
#include "nvim/os_unix.h"
#include "nvim/plines.h"
#include "nvim/screen.h"
#include "nvim/state.h"
#include "nvim/strings.h"
#include "nvim/syntax.h"
#include "nvim/ui.h"
#include "nvim/ui_compositor.h"
#include "nvim/vim.h"
#include "nvim/window.h"

#ifdef INCLUDE_GENERATED_DECLARATIONS
# include "mouse.c.generated.h"
#endif

static linenr_T orig_topline = 0;
static int orig_topfill = 0;

/// Move the cursor to the specified row and column on the screen.
/// Change current window if necessary. Returns an integer with the
/// CURSOR_MOVED bit set if the cursor has moved or unset otherwise.
///
/// The MOUSE_FOLD_CLOSE bit is set when clicked on the '-' in a fold column.
/// The MOUSE_FOLD_OPEN bit is set when clicked on the '+' in a fold column.
///
/// If flags has MOUSE_FOCUS, then the current window will not be changed, and
/// if the mouse is outside the window then the text will scroll, or if the
/// mouse was previously on a status line, then the status line may be dragged.
///
/// If flags has MOUSE_MAY_VIS, then VIsual mode will be started before the
/// cursor is moved unless the cursor was on a status line.
/// This function returns one of IN_UNKNOWN, IN_BUFFER, IN_STATUS_LINE or
/// IN_SEP_LINE depending on where the cursor was clicked.
///
/// If flags has MOUSE_MAY_STOP_VIS, then Visual mode will be stopped, unless
/// the mouse is on the status line of the same window.
///
/// If flags has MOUSE_DID_MOVE, nothing is done if the mouse didn't move since
/// the last call.
///
/// If flags has MOUSE_SETPOS, nothing is done, only the current position is
/// remembered.
///
/// @param inclusive  used for inclusive operator, can be NULL
/// @param which_button  MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE
int jump_to_mouse(int flags, bool *inclusive, int which_button)
{
  static int on_status_line = 0;        // #lines below bottom of window
  static int on_sep_line = 0;           // on separator right of window
  static int prev_row = -1;
  static int prev_col = -1;
  static win_T *dragwin = NULL;         // window being dragged
  static int did_drag = false;          // drag was noticed

  win_T *wp, *old_curwin;
  pos_T old_cursor;
  int count;
  bool first;
  int row = mouse_row;
  int col = mouse_col;
  int grid = mouse_grid;
  int fdc = 0;

  mouse_past_bottom = false;
  mouse_past_eol = false;

  if (flags & MOUSE_RELEASED) {
    // On button release we may change window focus if positioned on a
    // status line and no dragging happened.
    if (dragwin != NULL && !did_drag) {
      flags &= ~(MOUSE_FOCUS | MOUSE_DID_MOVE);
    }
    dragwin = NULL;
    did_drag = false;
  }

  if ((flags & MOUSE_DID_MOVE)
      && prev_row == mouse_row
      && prev_col == mouse_col) {
retnomove:
    // before moving the cursor for a left click which is NOT in a status
    // line, stop Visual mode
    if (on_status_line) {
      return IN_STATUS_LINE;
    }
    if (on_sep_line) {
      return IN_SEP_LINE;
    }
    if (flags & MOUSE_MAY_STOP_VIS) {
      end_visual_mode();
      redraw_curbuf_later(INVERTED);            // delete the inversion
    }
    return IN_BUFFER;
  }

  prev_row = mouse_row;
  prev_col = mouse_col;

  if (flags & MOUSE_SETPOS) {
    goto retnomove;                             // ugly goto...
  }
  old_curwin = curwin;
  old_cursor = curwin->w_cursor;

  if (!(flags & MOUSE_FOCUS)) {
    if (row < 0 || col < 0) {                   // check if it makes sense
      return IN_UNKNOWN;
    }

    // find the window where the row is in
    wp = mouse_find_win(&grid, &row, &col);
    if (wp == NULL) {
      return IN_UNKNOWN;
    }
    fdc = win_fdccol_count(wp);
    dragwin = NULL;

    if (row == -1) {
      return IN_OTHER_WIN;
    }

    // winpos and height may change in win_enter()!
    if (grid == DEFAULT_GRID_HANDLE && row >= wp->w_height) {
      // In (or below) status line
      on_status_line = row - wp->w_height + 1;
      dragwin = wp;
    } else {
      on_status_line = 0;
    }

    if (grid == DEFAULT_GRID_HANDLE && col >= wp->w_width) {
      // In separator line
      on_sep_line = col - wp->w_width + 1;
      dragwin = wp;
    } else {
      on_sep_line = 0;
    }

    // The rightmost character of the status line might be a vertical
    // separator character if there is no connecting window to the right.
    if (on_status_line && on_sep_line) {
      if (stl_connected(wp)) {
        on_sep_line = 0;
      } else {
        on_status_line = 0;
      }
    }

    // Before jumping to another buffer, or moving the cursor for a left
    // click, stop Visual mode.
    if (VIsual_active
        && (wp->w_buffer != curwin->w_buffer
            || (!on_status_line
                && !on_sep_line
                && (wp->w_p_rl
                    ? col < wp->w_width_inner - fdc
                    : col >= fdc + (cmdwin_type == 0 && wp == curwin ? 0 : 1))
                && (flags & MOUSE_MAY_STOP_VIS)))) {
      end_visual_mode();
      redraw_curbuf_later(INVERTED);            // delete the inversion
    }
    if (cmdwin_type != 0 && wp != curwin) {
      // A click outside the command-line window: Use modeless
      // selection if possible.  Allow dragging the status lines.
      on_sep_line = 0;
      row = 0;
      col += wp->w_wincol;
      wp = curwin;
    }
    // Only change window focus when not clicking on or dragging the
    // status line.  Do change focus when releasing the mouse button
    // (MOUSE_FOCUS was set above if we dragged first).
    if (dragwin == NULL || (flags & MOUSE_RELEASED)) {
      win_enter(wp, true);                      // can make wp invalid!
    }
    // set topline, to be able to check for double click ourselves
    if (curwin != old_curwin) {
      set_mouse_topline(curwin);
    }
    if (on_status_line) {                       // In (or below) status line
      // Don't use start_arrow() if we're in the same window
      if (curwin == old_curwin) {
        return IN_STATUS_LINE;
      } else {
        return IN_STATUS_LINE | CURSOR_MOVED;
      }
    }
    if (on_sep_line) {                          // In (or below) status line
      // Don't use start_arrow() if we're in the same window
      if (curwin == old_curwin) {
        return IN_SEP_LINE;
      } else {
        return IN_SEP_LINE | CURSOR_MOVED;
      }
    }

    curwin->w_cursor.lnum = curwin->w_topline;
  } else if (on_status_line && which_button == MOUSE_LEFT) {
    if (dragwin != NULL) {
      // Drag the status line
      count = row - dragwin->w_winrow - dragwin->w_height + 1
              - on_status_line;
      win_drag_status_line(dragwin, count);
      did_drag |= count;
    }
    return IN_STATUS_LINE;                      // Cursor didn't move
  } else if (on_sep_line && which_button == MOUSE_LEFT) {
    if (dragwin != NULL) {
      // Drag the separator column
      count = col - dragwin->w_wincol - dragwin->w_width + 1
              - on_sep_line;
      win_drag_vsep_line(dragwin, count);
      did_drag |= count;
    }
    return IN_SEP_LINE;                         // Cursor didn't move
  } else {
    // keep_window_focus must be true
    // before moving the cursor for a left click, stop Visual mode
    if (flags & MOUSE_MAY_STOP_VIS) {
      end_visual_mode();
      redraw_curbuf_later(INVERTED);            // delete the inversion
    }

    if (grid == 0) {
      row -= curwin->w_grid_alloc.comp_row+curwin->w_grid.row_offset;
      col -= curwin->w_grid_alloc.comp_col+curwin->w_grid.col_offset;
    }

    // When clicking beyond the end of the window, scroll the screen.
    // Scroll by however many rows outside the window we are.
    if (row < 0) {
      count = 0;
      for (first = true; curwin->w_topline > 1;) {
        if (curwin->w_topfill < win_get_fill(curwin, curwin->w_topline)) {
          count++;
        } else {
          count += plines_win(curwin, curwin->w_topline - 1, true);
        }
        if (!first && count > -row) {
          break;
        }
        first = false;
        (void)hasFolding(curwin->w_topline, &curwin->w_topline, NULL);
        if (curwin->w_topfill < win_get_fill(curwin, curwin->w_topline)) {
          curwin->w_topfill++;
        } else {
          --curwin->w_topline;
          curwin->w_topfill = 0;
        }
      }
      check_topfill(curwin, false);
      curwin->w_valid &=
        ~(VALID_WROW|VALID_CROW|VALID_BOTLINE|VALID_BOTLINE_AP);
      redraw_later(curwin, VALID);
      row = 0;
    } else if (row >= curwin->w_height_inner) {
      count = 0;
      for (first = true; curwin->w_topline < curbuf->b_ml.ml_line_count;) {
        if (curwin->w_topfill > 0) {
          ++count;
        } else {
          count += plines_win(curwin, curwin->w_topline, true);
        }

        if (!first && count > row - curwin->w_height_inner + 1) {
          break;
        }
        first = false;

        if (hasFolding(curwin->w_topline, NULL, &curwin->w_topline)
            && curwin->w_topline == curbuf->b_ml.ml_line_count) {
          break;
        }

        if (curwin->w_topfill > 0) {
          curwin->w_topfill--;
        } else {
          curwin->w_topline++;
          curwin->w_topfill = win_get_fill(curwin, curwin->w_topline);
        }
      }
      check_topfill(curwin, false);
      redraw_later(curwin, VALID);
      curwin->w_valid &=
        ~(VALID_WROW|VALID_CROW|VALID_BOTLINE|VALID_BOTLINE_AP);
      row = curwin->w_height_inner - 1;
    } else if (row == 0) {
      // When dragging the mouse, while the text has been scrolled up as
      // far as it goes, moving the mouse in the top line should scroll
      // the text down (done later when recomputing w_topline).
      if (mouse_dragging > 0
          && curwin->w_cursor.lnum
          == curwin->w_buffer->b_ml.ml_line_count
          && curwin->w_cursor.lnum == curwin->w_topline) {
        curwin->w_valid &= ~(VALID_TOPLINE);
      }
    }
  }

  // compute the position in the buffer line from the posn on the screen
  if (mouse_comp_pos(curwin, &row, &col, &curwin->w_cursor.lnum)) {
    mouse_past_bottom = true;
  }

  if (!(flags & MOUSE_RELEASED) && which_button == MOUSE_LEFT) {
    col = mouse_adjust_click(curwin, row, col);
  }

  // Start Visual mode before coladvance(), for when 'sel' != "old"
  if ((flags & MOUSE_MAY_VIS) && !VIsual_active) {
    VIsual = old_cursor;
    VIsual_active = true;
    VIsual_reselect = true;
    // if 'selectmode' contains "mouse", start Select mode
    may_start_select('o');
    setmouse();

    if (p_smd && msg_silent == 0) {
      redraw_cmdline = true;            // show visual mode later
    }
  }

  curwin->w_curswant = col;
  curwin->w_set_curswant = false;       // May still have been true
  if (coladvance(col) == FAIL) {        // Mouse click beyond end of line
    if (inclusive != NULL) {
      *inclusive = true;
    }
    mouse_past_eol = true;
  } else if (inclusive != NULL) {
    *inclusive = false;
  }

  count = IN_BUFFER;
  if (curwin != old_curwin || curwin->w_cursor.lnum != old_cursor.lnum
      || curwin->w_cursor.col != old_cursor.col) {
    count |= CURSOR_MOVED;              // Cursor has moved
  }

  count |= mouse_check_fold();

  return count;
}

// Compute the position in the buffer line from the posn on the screen in
// window "win".
// Returns true if the position is below the last line.
bool mouse_comp_pos(win_T *win, int *rowp, int *colp, linenr_T *lnump)
{
  int col = *colp;
  int row = *rowp;
  linenr_T lnum;
  bool retval = false;
  int off;
  int count;

  if (win->w_p_rl) {
    col = win->w_width_inner - 1 - col;
  }

  lnum = win->w_topline;

  while (row > 0) {
    // Don't include filler lines in "count"
    if (win_may_fill(win)
        && !hasFoldingWin(win, lnum, NULL, NULL, true, NULL)) {
      if (lnum == win->w_topline) {
        row -= win->w_topfill;
      } else {
        row -= win_get_fill(win, lnum);
      }
      count = plines_win_nofill(win, lnum, true);
    } else {
      count = plines_win(win, lnum, true);
    }

    if (count > row) {
      break;            // Position is in this buffer line.
    }

    (void)hasFoldingWin(win, lnum, NULL, &lnum, true, NULL);

    if (lnum == win->w_buffer->b_ml.ml_line_count) {
      retval = true;
      break;                    // past end of file
    }
    row -= count;
    ++lnum;
  }

  if (!retval) {
    // Compute the column without wrapping.
    off = win_col_off(win) - win_col_off2(win);
    if (col < off) {
      col = off;
    }
    col += row * (win->w_width_inner - off);
    // add skip column (for long wrapping line)
    col += win->w_skipcol;
  }

  if (!win->w_p_wrap) {
    col += win->w_leftcol;
  }

  // skip line number and fold column in front of the line
  col -= win_col_off(win);
  if (col <= 0) {
    col = 0;
  }

  *colp = col;
  *rowp = row;
  *lnump = lnum;
  return retval;
}

/// Find the window at "grid" position "*rowp" and "*colp".  The positions are
/// updated to become relative to the top-left of the window.
///
/// @return NULL when something is wrong.
win_T *mouse_find_win(int *gridp, int *rowp, int *colp)
{
  win_T *wp_grid = mouse_find_grid_win(gridp, rowp, colp);
  if (wp_grid) {
    return wp_grid;
  } else if (*gridp > 1) {
    return NULL;
  }


  frame_T *fp;

  fp = topframe;
  *rowp -= firstwin->w_winrow;
  for (;;) {
    if (fp->fr_layout == FR_LEAF) {
      break;
    }
    if (fp->fr_layout == FR_ROW) {
      for (fp = fp->fr_child; fp->fr_next != NULL; fp = fp->fr_next) {
        if (*colp < fp->fr_width) {
          break;
        }
        *colp -= fp->fr_width;
      }
    } else {  // fr_layout == FR_COL
      for (fp = fp->fr_child; fp->fr_next != NULL; fp = fp->fr_next) {
        if (*rowp < fp->fr_height) {
          break;
        }
        *rowp -= fp->fr_height;
      }
    }
  }
  // When using a timer that closes a window the window might not actually
  // exist.
  FOR_ALL_WINDOWS_IN_TAB(wp, curtab) {
    if (wp == fp->fr_win) {
      return wp;
    }
  }
  return NULL;
}

static win_T *mouse_find_grid_win(int *gridp, int *rowp, int *colp)
{
  if (*gridp == msg_grid.handle) {
    *rowp += msg_grid_pos;
    *gridp = DEFAULT_GRID_HANDLE;
  } else if (*gridp > 1) {
    win_T *wp = get_win_by_grid_handle(*gridp);
    if (wp && wp->w_grid_alloc.chars
        && !(wp->w_floating && !wp->w_float_config.focusable)) {
      *rowp = MIN(*rowp-wp->w_grid.row_offset, wp->w_grid.Rows-1);
      *colp = MIN(*colp-wp->w_grid.col_offset, wp->w_grid.Columns-1);
      return wp;
    }
  } else if (*gridp == 0) {
    ScreenGrid *grid = ui_comp_mouse_focus(*rowp, *colp);
    FOR_ALL_WINDOWS_IN_TAB(wp, curtab) {
      if (&wp->w_grid_alloc != grid) {
        continue;
      }
      *gridp = grid->handle;
      *rowp -= grid->comp_row+wp->w_grid.row_offset;
      *colp -= grid->comp_col+wp->w_grid.col_offset;
      return wp;
    }

    // no float found, click on the default grid
    // TODO(bfredl): grid can be &pum_grid, allow select pum items by mouse?
    *gridp = DEFAULT_GRID_HANDLE;
  }
  return NULL;
}

/// Set UI mouse depending on current mode and 'mouse'.
///
/// Emits mouse_on/mouse_off UI event (unless 'mouse' is empty).
void setmouse(void)
{
  ui_cursor_shape();
  ui_check_mouse();
}


// Set orig_topline.  Used when jumping to another window, so that a double
// click still works.
void set_mouse_topline(win_T *wp)
{
  orig_topline = wp->w_topline;
  orig_topfill = wp->w_topfill;
}

///
/// Return length of line "lnum" for horizontal scrolling.
///
static colnr_T scroll_line_len(linenr_T lnum)
{
  colnr_T col = 0;
  char_u *line = ml_get(lnum);
  if (*line != NUL) {
    for (;;) {
      int numchar = win_chartabsize(curwin, line, col);
      MB_PTR_ADV(line);
      if (*line == NUL) {    // don't count the last character
        break;
      }
      col += numchar;
    }
  }
  return col;
}

///
/// Find longest visible line number.
///
static linenr_T find_longest_lnum(void)
{
  linenr_T ret = 0;

  // Calculate maximum for horizontal scrollbar.  Check for reasonable
  // line numbers, topline and botline can be invalid when displaying is
  // postponed.
  if (curwin->w_topline <= curwin->w_cursor.lnum
      && curwin->w_botline > curwin->w_cursor.lnum
      && curwin->w_botline <= curbuf->b_ml.ml_line_count + 1) {
    long max = 0;

    // Use maximum of all visible lines.  Remember the lnum of the
    // longest line, closest to the cursor line.  Used when scrolling
    // below.
    for (linenr_T lnum = curwin->w_topline; lnum < curwin->w_botline; lnum++) {
      colnr_T len = scroll_line_len(lnum);
      if (len > (colnr_T)max) {
        max = len;
        ret = lnum;
      } else if (len == (colnr_T)max
                 && abs((int)(lnum - curwin->w_cursor.lnum))
                 < abs((int)(ret - curwin->w_cursor.lnum))) {
        ret = lnum;
      }
    }
  } else {
    // Use cursor line only.
    ret = curwin->w_cursor.lnum;
  }

  return ret;
}

// CUSTOM_UI begin
/// Do a horizontal scroll.
/// @return true if the cursor moved, false otherwise.
/// Use step to set the lines to scroll.
bool mouse_scroll_horiz_step(int dir, int step)
{
  if (curwin->w_p_wrap) {
    return false;
  }

  if (mod_mask & (MOD_MASK_SHIFT | MOD_MASK_CTRL)) {
    step = curwin->w_width_inner;
  }

  int leftcol = curwin->w_leftcol + (dir == MSCR_RIGHT ? -step : +step);
  if (leftcol < 0) {
    leftcol = 0;
  }

  if (curwin->w_leftcol == leftcol) {
    return false;
  }

  curwin->w_leftcol = (colnr_T)leftcol;

  // When the line of the cursor is too short, move the cursor to the
  // longest visible line.
  if (!virtual_active()
      && (colnr_T)leftcol > scroll_line_len(curwin->w_cursor.lnum)) {
    curwin->w_cursor.lnum = find_longest_lnum();
    curwin->w_cursor.col = 0;
  }

  return leftcol_changed();
}

bool mouse_scroll_horiz(int dir)
{
  return mouse_scroll_horiz_step(dir, 6);
}
// CUSTOM_UI end

/// Adjusts the clicked column position when 'conceallevel' > 0
static int mouse_adjust_click(win_T *wp, int row, int col)
{
  if (!(wp->w_p_cole > 0 && curbuf->b_p_smc > 0
        && wp->w_leftcol < curbuf->b_p_smc && conceal_cursor_line(wp))) {
    return col;
  }

  // `col` is the position within the current line that is highlighted by the
  // cursor without consideration for concealed characters.  The current line is
  // scanned *up to* `col`, nudging it left or right when concealed characters
  // are encountered.
  //
  // win_chartabsize() is used to keep track of the virtual column position
  // relative to the line's bytes.  For example: if col == 9 and the line
  // starts with a tab that's 8 columns wide, we would want the cursor to be
  // highlighting the second byte, not the ninth.

  linenr_T lnum = wp->w_cursor.lnum;
  char_u *line = ml_get(lnum);
  char_u *ptr = line;
  char_u *ptr_end;
  char_u *ptr_row_offset = line;  // Where we begin adjusting `ptr_end`

  // Find the offset where scanning should begin.
  int offset = wp->w_leftcol;
  if (row > 0) {
    offset += row * (wp->w_width_inner - win_col_off(wp) - win_col_off2(wp) -
                     wp->w_leftcol + wp->w_skipcol);
  }

  int vcol;

  if (offset) {
    // Skip everything up to an offset since nvim takes care of displaying the
    // correct portion of the line when horizontally scrolling.
    // When 'wrap' is enabled, only the row (of the wrapped line) needs to be
    // checked for concealed characters.
    vcol = 0;
    while (vcol < offset && *ptr != NUL) {
      vcol += win_chartabsize(curwin, ptr, vcol);
      ptr += utfc_ptr2len(ptr);
    }

    ptr_row_offset = ptr;
  }

  // Align `ptr_end` with `col`
  vcol = offset;
  ptr_end = ptr_row_offset;
  while (vcol < col && *ptr_end != NUL) {
    vcol += win_chartabsize(curwin, ptr_end, vcol);
    ptr_end += utfc_ptr2len(ptr_end);
  }

  int matchid;
  int prev_matchid;
  int nudge = 0;
  int cwidth = 0;

  vcol = offset;

#define INCR() nudge++; ptr_end += utfc_ptr2len(ptr_end)
#define DECR() nudge--; ptr_end -= utfc_ptr2len(ptr_end)

  while (ptr < ptr_end && *ptr != NUL) {
    cwidth = win_chartabsize(curwin, ptr, vcol);
    vcol += cwidth;
    if (cwidth > 1 && *ptr == '\t' && nudge > 0) {
      // A tab will "absorb" any previous adjustments.
      cwidth = MIN(cwidth, nudge);
      while (cwidth > 0) {
        DECR();
        cwidth--;
      }
    }

    matchid = syn_get_concealed_id(wp, lnum, (colnr_T)(ptr - line));
    if (matchid != 0) {
      if (wp->w_p_cole == 3) {
        INCR();
      } else {
        if (!(row > 0 && ptr == ptr_row_offset)
            && (wp->w_p_cole == 1 || (wp->w_p_cole == 2
                                      && (wp->w_p_lcs_chars.conceal != NUL
                                          || syn_get_sub_char() != NUL)))) {
          // At least one placeholder character will be displayed.
          DECR();
        }

        prev_matchid = matchid;

        while (prev_matchid == matchid && *ptr != NUL) {
          INCR();
          ptr += utfc_ptr2len(ptr);
          matchid = syn_get_concealed_id(wp, lnum, (colnr_T)(ptr - line));
        }

        continue;
      }
    }

    ptr += utfc_ptr2len(ptr);
  }

  return col + nudge;
}

// Check clicked cell is foldcolumn
int mouse_check_fold(void)
{
  int click_grid = mouse_grid;
  int click_row = mouse_row;
  int click_col = mouse_col;
  int mouse_char = ' ';
  int max_row = Rows;
  int max_col = Columns;
  int multigrid = ui_has(kUIMultigrid);

  win_T *wp;

  wp = mouse_find_win(&click_grid, &click_row, &click_col);
  if (wp && multigrid) {
    max_row = wp->w_grid_alloc.Rows;
    max_col = wp->w_grid_alloc.Columns;
  }

  if (wp && mouse_row >= 0 && mouse_row < max_row
      && mouse_col >= 0 && mouse_col < max_col) {
    ScreenGrid *gp = multigrid ? &wp->w_grid_alloc : &default_grid;
    int fdc = win_fdccol_count(wp);
    int row = multigrid && mouse_grid == 0 ? click_row : mouse_row;
    int col = multigrid && mouse_grid == 0 ? click_col : mouse_col;

    // Remember the character under the mouse, might be one of foldclose or
    // foldopen fillchars in the fold column.
    if (gp->chars != NULL) {
      mouse_char = utf_ptr2char(gp->chars[gp->line_offset[row]
                                          + (unsigned)col]);
    }

    // Check for position outside of the fold column.
    if (wp->w_p_rl ? click_col < wp->w_width_inner - fdc :
        click_col >= fdc + (cmdwin_type == 0 ? 0 : 1)) {
      mouse_char = ' ';
    }
  }

  if (wp && mouse_char == wp->w_p_fcs_chars.foldclosed) {
    return MOUSE_FOLD_OPEN;
  } else if (mouse_char != ' ') {
    return MOUSE_FOLD_CLOSE;
  }

  return 0;
}
