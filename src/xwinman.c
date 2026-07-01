/* $Id: xwinman.c,v 1.3 2001/12/26 22:17:07 sybalsky Exp $ (C) Copyright Venue, All Rights Reserved
 */

/************************************************************************/
/*									*/
/*	(C) Copyright 1989, 1990, 1990, 1991, 1992, 1993, 1994, 2000 Venue.	*/
/*	    All Rights Reserved.		*/
/*	Manufactured in the United States of America.			*/
/*									*/
/************************************************************************/

#include "version.h"

#include <X11/X.h>         // for Button1, Cursor, ButtonPress, Button2, But...
#include <X11/Xlib.h>      // for XEvent, XMoveResizeWindow, XAnyEvent, XBut...
#include <X11/keysym.h>    // for XK_Left, XK_Right, XK_Up, XK_Down
#include <X11/Xutil.h>     // for XLookupString
#include <stdio.h>         // for printf
#include <stdlib.h>        // for getenv, atoi
#include <string.h>        // for memset
#include <sys/types.h>     // for u_char
#include <time.h>          // for time
#include "devif.h"         // for (anonymous), MRegion, DefineCursor, OUTER_...
#include "keyeventdefs.h"  // for kb_trans
#include "keysym.h"        // for KEY_* Lisp key codes
#include "lispemul.h"      // for PUTBASEBIT68K, FALSE, TRUE, DLword, state
#include "lspglob.h"       // for MiscStats
#include "xdefs.h"         // for XLOCK, XUNLOCK
#include "xlspwindefs.h"   // for DoRing
#include "xscrolldefs.h"   // for JumpScrollHor, JumpScrollVer, Scroll, Scro...
#include "xwinmandefs.h"   // for Set_BitGravity, beep_Xkeyboard, dis...

extern int Mouse_Included;
int Mouse_Included = FALSE;

extern Cursor WaitCursor, DefaultCursor, VertScrollCursor, VertThumbCursor, ScrollUpCursor,
    ScrollDownCursor, HorizScrollCursor, HorizThumbCursor, ScrollLeftCursor, ScrollRightCursor;
extern int noscroll;
extern DspInterface currentdsp;

extern DLword *EmCursorX68K, *EmCursorY68K;
extern DLword *EmMouseX68K, *EmMouseY68K, *EmKbdAd068K, *EmRealUtilin68K;
extern LispPTR *CLastUserActionCell68k;
extern int KBDEventFlg;
extern u_char *SUNLispKeyMap;
extern void DoRing(void);
#define KEYCODE_OFFSET 7 /* Sun Keycode offset */

/* bits within the EmRealUtilin word */
#define KEYSET_LEFT 8
#define KEYSET_LEFTMIDDLE 9
#define KEYSET_MIDDLE  10
#define KEYSET_RIGHTMIDDLE 11
#define KEYSET_RIGHT 12
/* Mouse buttons */
#define MOUSE_LEFT 13
#define MOUSE_RIGHT 14
#define MOUSE_MIDDLE 15

typedef struct {
  u_char code;
  unsigned char handled;
  unsigned char synth_shift;
  unsigned char neutral_lshift;
  unsigned char neutral_rshift;
} XSentKey;

static XSentKey x_sent_keys[256];
static int x_lshift_down = FALSE;
static int x_rshift_down = FALSE;
static int startup_typeahead_state = 0;
static time_t startup_typeahead_at = 0;

static void record_key_event(void)
{
  DoRing();
  if ((KBDEventFlg += 1) > 0) Irq_Stk_End = Irq_Stk_Check = 0;
}

static void inject_lisp_key(u_char code, int needs_shift)
{
  if (needs_shift) {
    kb_trans(KEY_LEFTSHIFT, FALSE);
    record_key_event();
  }

  kb_trans(code, FALSE);
  record_key_event();
  kb_trans(code, TRUE);
  record_key_event();

  if (needs_shift) {
    kb_trans(KEY_LEFTSHIFT, TRUE);
    record_key_event();
  }
}

static int ascii_to_lisp_key(unsigned char c, u_char *code, int *needs_shift)
{
  *needs_shift = FALSE;

  if (c >= 'a' && c <= 'z') {
    static const u_char keys[] = {
        KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
        KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
        KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
    *code = keys[c - 'a'];
    return TRUE;
  }

  if (c >= 'A' && c <= 'Z') {
    if (!ascii_to_lisp_key((unsigned char)(c - 'A' + 'a'), code, needs_shift)) return FALSE;
    *needs_shift = TRUE;
    return TRUE;
  }

  switch (c) {
    case '0': *code = KEY_0; return TRUE;
    case '1': *code = KEY_1; return TRUE;
    case '2': *code = KEY_2; return TRUE;
    case '3': *code = KEY_3; return TRUE;
    case '4': *code = KEY_4; return TRUE;
    case '5': *code = KEY_5; return TRUE;
    case '6': *code = KEY_6; return TRUE;
    case '7': *code = KEY_7; return TRUE;
    case '8': *code = KEY_8; return TRUE;
    case '9': *code = KEY_9; return TRUE;
    case ' ': *code = KEY_SPACE; return TRUE;
    case '\t': *code = KEY_TAB; return TRUE;
    case '\r': *code = KEY_RETURN; return TRUE;
    case '\n': *code = KEY_RETURN; return TRUE;
    case '-': *code = KEY_MINUS; return TRUE;
    case '_': *code = KEY_MINUS; *needs_shift = TRUE; return TRUE;
    case '=': *code = KEY_EQUAL; return TRUE;
    case '+': *code = KEY_EQUAL; *needs_shift = TRUE; return TRUE;
    case '[': *code = KEY_OPENBRACK; return TRUE;
    case '{': *code = KEY_OPENBRACK; *needs_shift = TRUE; return TRUE;
    case ']': *code = KEY_CLOSEBRACK; return TRUE;
    case '}': *code = KEY_CLOSEBRACK; *needs_shift = TRUE; return TRUE;
    case ';': *code = KEY_SEMICOLON; return TRUE;
    case ':': *code = KEY_SEMICOLON; *needs_shift = TRUE; return TRUE;
    case '\'': *code = KEY_QUOTE; return TRUE;
    case '"': *code = KEY_QUOTE; *needs_shift = TRUE; return TRUE;
    case '`': *code = KEY_BACKQUOTE; return TRUE;
    case '~': *code = KEY_BACKQUOTE; *needs_shift = TRUE; return TRUE;
    case '\\': *code = 105; return TRUE;
    case '|': *code = 105; *needs_shift = TRUE; return TRUE;
    case ',': *code = KEY_COMMA; return TRUE;
    case '<': *code = KEY_COMMA; *needs_shift = TRUE; return TRUE;
    case '.': *code = KEY_PERIOD; return TRUE;
    case '>': *code = KEY_PERIOD; *needs_shift = TRUE; return TRUE;
    case '/': *code = KEY_SLASH; return TRUE;
    case '?': *code = KEY_SLASH; *needs_shift = TRUE; return TRUE;
    case '!': *code = KEY_1; *needs_shift = TRUE; return TRUE;
    case '@': *code = KEY_2; *needs_shift = TRUE; return TRUE;
    case '#': *code = KEY_3; *needs_shift = TRUE; return TRUE;
    case '$': *code = KEY_4; *needs_shift = TRUE; return TRUE;
    case '%': *code = KEY_5; *needs_shift = TRUE; return TRUE;
    case '^': *code = KEY_6; *needs_shift = TRUE; return TRUE;
    case '&': *code = KEY_7; *needs_shift = TRUE; return TRUE;
    case '*': *code = KEY_8; *needs_shift = TRUE; return TRUE;
    case '(': *code = KEY_9; *needs_shift = TRUE; return TRUE;
    case ')': *code = KEY_0; *needs_shift = TRUE; return TRUE;
    default: return FALSE;
  }
}

static void inject_ascii_char(int ch)
{
  u_char code = 255;
  int needs_shift = FALSE;

  if (ch == '\r') ch = '\n';
  if (ascii_to_lisp_key((unsigned char)ch, &code, &needs_shift)) inject_lisp_key(code, needs_shift);
}

static void inject_startup_typeahead_file(const char *path)
{
  FILE *file = NULL;
  int ch;

  if (path == NULL || path[0] == '\0') return;

  file = fopen(path, "r");
  if (file == NULL) {
    perror("MAIKO_STARTUP_TYPEAHEAD_FILE");
    return;
  }

  while ((ch = fgetc(file)) != EOF) inject_ascii_char(ch);

  fclose(file);
}

static void maybe_inject_startup_typeahead(void)
{
  const char *path;
  const char *delay_text;
  int delay = 10;
  time_t now;

  if (startup_typeahead_state == 2) return;

  path = getenv("MAIKO_STARTUP_TYPEAHEAD_FILE");
  if (path == NULL || path[0] == '\0') {
    startup_typeahead_state = 2;
    return;
  }

  now = time(NULL);
  if (startup_typeahead_state == 0) {
    delay_text = getenv("MAIKO_STARTUP_TYPEAHEAD_DELAY");
    if (delay_text != NULL && delay_text[0] != '\0') {
      int parsed = atoi(delay_text);
      if (parsed >= 0 && parsed < 300) delay = parsed;
    }
    startup_typeahead_at = now + delay;
    startup_typeahead_state = 1;
  }

  if (now < startup_typeahead_at) return;

  startup_typeahead_state = 2;
  inject_startup_typeahead_file(path);
}

static u_char xkey_to_lisp_key(const XKeyEvent *event)
{
  int index = (int)event->keycode - KEYCODE_OFFSET;

  if (index < 0 || index >= 256) return 255;
  return SUNLispKeyMap[index];
}

static u_char x_arrow_keysym_to_lisp_key(KeySym keysym)
{
  switch (keysym) {
    case XK_Left:
#ifdef XK_KP_Left
    case XK_KP_Left:
#endif
      return KEY_KP_4;
    case XK_Right:
#ifdef XK_KP_Right
    case XK_KP_Right:
#endif
      return KEY_KP_6;
    case XK_Up:
#ifdef XK_KP_Up
    case XK_KP_Up:
#endif
      return KEY_KP_8;
    case XK_Down:
#ifdef XK_KP_Down
    case XK_KP_Down:
#endif
      return KEY_KP_2;
    default:
      return 255;
  }
}

static void update_tracked_shift(u_char code, int upflg)
{
  if (code == KEY_LEFTSHIFT) x_lshift_down = !upflg;
  if (code == KEY_RIGHTSHIFT) x_rshift_down = !upflg;
}

static void handle_X_key(XKeyEvent *event, int upflg)
{
  int index = (int)event->keycode;
  XSentKey *sent = (index >= 0 && index < 256) ? &x_sent_keys[index] : NULL;
  u_char arrow_code = x_arrow_keysym_to_lisp_key(XLookupKeysym(event, 0));

  if (arrow_code != 255) {
    if (sent) memset(sent, 0, sizeof(*sent));
    kb_trans(arrow_code, upflg);
    record_key_event();
    return;
  }

  if (upflg) {
    if (sent && sent->handled) {
      if (sent->neutral_lshift && x_lshift_down) {
        kb_trans(KEY_LEFTSHIFT, TRUE);
        record_key_event();
      }
      if (sent->neutral_rshift && x_rshift_down) {
        kb_trans(KEY_RIGHTSHIFT, TRUE);
        record_key_event();
      }
      kb_trans(sent->code, TRUE);
      record_key_event();
      if (sent->synth_shift) {
        kb_trans(KEY_LEFTSHIFT, TRUE);
        record_key_event();
      }
      if (sent->neutral_lshift && x_lshift_down) {
        kb_trans(KEY_LEFTSHIFT, FALSE);
        record_key_event();
      }
      if (sent->neutral_rshift && x_rshift_down) {
        kb_trans(KEY_RIGHTSHIFT, FALSE);
        record_key_event();
      }
      memset(sent, 0, sizeof(*sent));
      return;
    }
  } else if (sent && !(event->state & ControlMask)) {
    char text[8];
    KeySym keysym = NoSymbol;
    u_char code = 255;
    int needs_shift = FALSE;
    int len = XLookupString(event, text, (int)sizeof(text), &keysym, NULL);

    if (len == 1 && ascii_to_lisp_key((unsigned char)text[0], &code, &needs_shift)) {
      memset(sent, 0, sizeof(*sent));
      sent->handled = TRUE;
      sent->code = code;

      if (needs_shift) {
        if (!x_lshift_down && !x_rshift_down) {
          kb_trans(KEY_LEFTSHIFT, FALSE);
          record_key_event();
          sent->synth_shift = TRUE;
        }
      } else {
        if (x_lshift_down) {
          kb_trans(KEY_LEFTSHIFT, TRUE);
          record_key_event();
          sent->neutral_lshift = TRUE;
        }
        if (x_rshift_down) {
          kb_trans(KEY_RIGHTSHIFT, TRUE);
          record_key_event();
          sent->neutral_rshift = TRUE;
        }
      }

      kb_trans(code, FALSE);
      record_key_event();

      if (sent->neutral_lshift) {
        kb_trans(KEY_LEFTSHIFT, FALSE);
        record_key_event();
      }
      if (sent->neutral_rshift) {
        kb_trans(KEY_RIGHTSHIFT, FALSE);
        record_key_event();
      }
      return;
    }
  }

  {
    u_char code = xkey_to_lisp_key(event);
    if (code != 255) {
      kb_trans(code, upflg);
      update_tracked_shift(code, upflg);
      record_key_event();
    }
  }
}

/* ubound: return (unsigned) value if it is between lower and upper otherwise lower or upper */
static inline unsigned ubound(unsigned lower, unsigned value, unsigned upper)
{
  if (value <= lower)
    return (lower);
  else if (value >= upper)
    return (upper);
  else
    return (value);
}

static void focus_Xkeyboard(DspInterface dsp)
{
  XWindowAttributes attrs;

  if (dsp->DisplayWindow &&
      XGetWindowAttributes(dsp->display_id, dsp->DisplayWindow, &attrs) &&
      attrs.map_state == IsViewable) {
    XSetInputFocus(dsp->display_id, dsp->DisplayWindow, RevertToParent, CurrentTime);
  }
}

void Set_BitGravity(XButtonEvent *event, DspInterface dsp, Window window, int grav)
{
  Window OldWindow = 0;

  /* Change Background Pixmap of Gravity Window */
  XLOCK;
  switch (dsp->BitGravity) {
    case NorthWestGravity: OldWindow = dsp->NWGrav; break;
    case NorthEastGravity: OldWindow = dsp->NEGrav; break;
    case SouthWestGravity: OldWindow = dsp->SWGrav; break;
    case SouthEastGravity: OldWindow = dsp->SEGrav; break;
  }

  dsp->BitGravity = grav;

  XSetWindowBackgroundPixmap(event->display, OldWindow, dsp->GravityOffPixmap);
  XClearWindow(event->display, OldWindow);

  XSetWindowBackgroundPixmap(event->display, window, dsp->GravityOnPixmap);
  XClearWindow(event->display, window);
  XUNLOCK(dsp);
} /* end Set_BitGravity */

static void lisp_Xconfigure(DspInterface dsp, int x, int y, unsigned lspWinWidth, unsigned lspWinHeight)
{
  int Col2, Row2, Col3, Row3;
  unsigned int GravSize;

  /* The Visible width and height changes when */
  /* we configure the window. Make them */
  /* stay within bounds. */
  dsp->Visible.width =
      ubound(OUTER_SB_WIDTH(dsp) + 2, lspWinWidth, dsp->Display.width + OUTER_SB_WIDTH(dsp)) -
      OUTER_SB_WIDTH(dsp);
  dsp->Visible.height =
      ubound(OUTER_SB_WIDTH(dsp) + 2, lspWinHeight, dsp->Display.height + OUTER_SB_WIDTH(dsp)) -
      OUTER_SB_WIDTH(dsp);

  GravSize = (dsp->ScrollBarWidth / 2) - (dsp->InternalBorderWidth);
  Col2 = (int)dsp->Visible.width;
  Row2 = (int)dsp->Visible.height;
  Col3 = (int)(dsp->Visible.width + (OUTER_SB_WIDTH(dsp) / 2));
  Row3 = (int)(dsp->Visible.height + (OUTER_SB_WIDTH(dsp) / 2));

  XLOCK;
  XMoveResizeWindow(dsp->display_id, dsp->DisplayWindow, 0, 0, dsp->Visible.width,
                    dsp->Visible.height);
  if (noscroll == 0) {
    /* Scroll bars */
    XMoveResizeWindow(dsp->display_id, dsp->VerScrollBar, Col2, 0 - (int)dsp->InternalBorderWidth, /* y */
                      dsp->ScrollBarWidth,   /* width */
                      dsp->Visible.height); /* height */
    XMoveResizeWindow(dsp->display_id, dsp->HorScrollBar, 0 - (int)dsp->InternalBorderWidth, Row2, /* y */
                      dsp->Visible.width,  /* width */
                      dsp->ScrollBarWidth); /* height */

    /* Scroll buttons */
    XMoveResizeWindow(
                      dsp->display_id, dsp->HorScrollButton,
                      (dsp->Visible.x * (int)dsp->Visible.width) / (int)dsp->Display.width,         /* x */
                      0 - (int)dsp->InternalBorderWidth,                                                /* y */
                      ((dsp->Visible.width * dsp->Visible.width) / dsp->Display.width) + 1, /* width */
                      dsp->ScrollBarWidth);                                                        /* height */
    XMoveResizeWindow(
                      dsp->display_id, dsp->VerScrollButton, 0 - (int)dsp->InternalBorderWidth,             /* x */
                      (dsp->Visible.y * (int)dsp->Visible.height) / (int)dsp->Display.height,           /* y */
                      dsp->ScrollBarWidth,                                                             /* width */
                      ((dsp->Visible.height * dsp->Visible.height) / dsp->Display.height) + 1); /* height */

    /* Gravity windows */
    XMoveResizeWindow(dsp->display_id, dsp->NWGrav, Col2, Row2, GravSize, GravSize);
    XMoveResizeWindow(dsp->display_id, dsp->NEGrav, Col3, Row2, GravSize, GravSize);
    XMoveResizeWindow(dsp->display_id, dsp->SEGrav, Col3, Row3, GravSize, GravSize);
    XMoveResizeWindow(dsp->display_id, dsp->SWGrav, Col2, Row3, GravSize, GravSize);
    Scroll(dsp, dsp->Visible.x, dsp->Visible.y);
  }
  XFlush(dsp->display_id);
  XUNLOCK(dsp);
} /* end lisp_Xconfigure */

void enable_Xkeyboard(DspInterface dsp)
{
  XLOCK;
  XSelectInput(dsp->display_id, dsp->DisplayWindow, dsp->EnableEventMask);
  focus_Xkeyboard(dsp);
  XFlush(dsp->display_id);
  XUNLOCK(dsp);
}

void disable_Xkeyboard(DspInterface dsp)
{
  XLOCK;
  XSelectInput(dsp->display_id, dsp->DisplayWindow, dsp->DisableEventMask);
  XFlush(dsp->display_id);
  XUNLOCK(dsp);
}

void beep_Xkeyboard(DspInterface dsp)
{
#ifdef TRACE
  printf("TRACE: beep_Xkeyboard()\n");
#endif

  XLOCK;
  XBell(dsp->display_id, (int)50);
  XFlush(dsp->display_id);
  XUNLOCK(dsp);

} /* end beep_Xkeyboard */

/************************************************************************/
/*									*/
/*		    p r o c e s s _ X e v e n t s			*/
/*									*/
/*  Take X key/mouse events and turn them into Lisp events		*/
/*									*/
/************************************************************************/

extern int Current_Hot_X, Current_Hot_Y; /* Cursor hotspot */

void process_Xevents(DspInterface dsp)
{
  XEvent report;

  maybe_inject_startup_typeahead();

  while (XPending(dsp->display_id)) {
    XNextEvent(dsp->display_id, &report);
    if (report.xany.window == dsp->DisplayWindow) /* Try the most important window first. */
      switch (report.type) {
#ifndef INIT
        case MotionNotify:
          *CLastUserActionCell68k = MiscStats->secondstmp;
          *EmCursorX68K = (*((DLword *)EmMouseX68K)) =
              (short)((report.xmotion.x + dsp->Visible.x) & 0xFFFF) - Current_Hot_X;
          *EmCursorY68K = (*((DLword *)EmMouseY68K)) =
              (short)((report.xmotion.y + dsp->Visible.y) & 0xFFFF) - Current_Hot_Y;
          break;
        case KeyPress:
          handle_X_key(&report.xkey, FALSE);
          break;
        case KeyRelease:
          handle_X_key(&report.xkey, TRUE);
          break;
        case ButtonPress:
          switch (report.xbutton.button) {
            case Button1: PUTBASEBIT68K(EmRealUtilin68K, MOUSE_LEFT, FALSE); break;
            case Button2: PUTBASEBIT68K(EmRealUtilin68K, MOUSE_MIDDLE, FALSE); break;
            case Button3: PUTBASEBIT68K(EmRealUtilin68K, MOUSE_RIGHT, FALSE); break;
            case Button4: PUTBASEBIT68K(EmRealUtilin68K, KEYSET_LEFT, FALSE); break;
            case Button5: PUTBASEBIT68K(EmRealUtilin68K, KEYSET_LEFTMIDDLE, FALSE); break;
            case Button5 + 1: PUTBASEBIT68K(EmRealUtilin68K, KEYSET_RIGHT, FALSE); break;
            case Button5 + 2: PUTBASEBIT68K(EmRealUtilin68K, KEYSET_RIGHTMIDDLE, FALSE); break;
            default: break;
          }
          DoRing();
          if ((KBDEventFlg += 1) > 0) Irq_Stk_End = Irq_Stk_Check = 0;
          break;
        case ButtonRelease:
          switch (report.xbutton.button) {
            case Button1: PUTBASEBIT68K(EmRealUtilin68K, MOUSE_LEFT, TRUE); break;
            case Button2: PUTBASEBIT68K(EmRealUtilin68K, MOUSE_MIDDLE, TRUE); break;
            case Button3: PUTBASEBIT68K(EmRealUtilin68K, MOUSE_RIGHT, TRUE); break;
            case Button4: PUTBASEBIT68K(EmRealUtilin68K, KEYSET_LEFT, TRUE); break;
            case Button5: PUTBASEBIT68K(EmRealUtilin68K, KEYSET_LEFTMIDDLE, TRUE); break;
            case Button5 + 1: PUTBASEBIT68K(EmRealUtilin68K, KEYSET_RIGHT, TRUE); break;
            case Button5 + 2: PUTBASEBIT68K(EmRealUtilin68K, KEYSET_RIGHTMIDDLE, TRUE); break;
            default: break;
          }
          DoRing();
          if ((KBDEventFlg += 1) > 0) Irq_Stk_End = Irq_Stk_Check = 0;
          break;
        case EnterNotify: Mouse_Included = TRUE; break;
        case LeaveNotify: Mouse_Included = FALSE; break;
#endif
        case Expose:
          (dsp->bitblt_to_screen)(dsp, 0, report.xexpose.x + dsp->Visible.x,
                                  report.xexpose.y + dsp->Visible.y, report.xexpose.width,
                                  report.xexpose.height);
          break;
        default: break;
      }
    else if (report.xany.window == dsp->LispWindow)
      switch (report.xany.type) {
        case KeyPress:
          handle_X_key(&report.xkey, FALSE);
          break;
        case KeyRelease:
          handle_X_key(&report.xkey, TRUE);
          break;
        case ConfigureNotify:
          lisp_Xconfigure(dsp, report.xconfigure.x, report.xconfigure.y, (unsigned)report.xconfigure.width,
                          (unsigned)report.xconfigure.height);
          break;
        case ButtonPress:
        case FocusIn:
        case EnterNotify: enable_Xkeyboard(currentdsp); break;
        case LeaveNotify: break;
        case MapNotify:
          /* Turn the blitting to the screen on */
          break;
        case UnmapNotify:
          /* Turn the blitting to the screen off */
          break;
        default: break;
      }
    else if (noscroll) continue;
    else if (report.xany.window == dsp->HorScrollBar)
      switch (report.type) {
        case ButtonPress:
          switch (report.xbutton.button) {
            case Button1:
              DefineCursor(dsp, dsp->HorScrollBar, &ScrollLeftCursor);
              ScrollLeft(dsp);
              break;
            case Button2:
              DefineCursor(dsp, dsp->HorScrollBar, &HorizThumbCursor);
              break;
            case Button3:
              DefineCursor(dsp, dsp->HorScrollBar, &ScrollRightCursor);
              ScrollRight(dsp);
              break;
            default: break;
          } /* end switch */
          break;
        case ButtonRelease:
          switch (report.xbutton.button) {
            case Button1:
              DefineCursor(dsp, report.xany.window, &HorizScrollCursor);
              break;
            case Button2:
              JumpScrollHor(dsp, report.xbutton.x);
              DefineCursor(dsp, report.xany.window, &HorizScrollCursor);
              break;
            case Button3:
              DefineCursor(dsp, report.xany.window, &HorizScrollCursor);
              break;
            default: break;
          } /* end switch */
	  break;
        default: break;
      }
    else if (report.xany.window == dsp->VerScrollBar)
      switch (report.type) {
        case ButtonPress:
          switch (report.xbutton.button) {
            case Button1:
              DefineCursor(dsp, report.xany.window, &ScrollUpCursor);
              ScrollUp(dsp);
              break;
            case Button2:
              DefineCursor(dsp, report.xany.window, &VertThumbCursor);
              break;
            case Button3:
              DefineCursor(dsp, report.xany.window, &ScrollDownCursor);
              ScrollDown(dsp);
              break;
            default: break;
          } /* end switch */
          break;
        case ButtonRelease:
          switch (report.xbutton.button) {
            case Button1:
              DefineCursor(dsp, report.xany.window, &VertScrollCursor);
              break;
            case Button3:
              DefineCursor(dsp, report.xany.window, &VertScrollCursor);
              break;
            case Button2:
              JumpScrollVer(dsp, report.xbutton.y);
              DefineCursor(dsp, report.xany.window, &VertScrollCursor);
              break;
            default: break;
          } /* end switch */
          break;
        default: break;
      }
    else if ((report.xany.window == dsp->NEGrav) && (report.xany.type == ButtonPress) &&
             ((report.xbutton.button & 0xFF) == Button1))
      Set_BitGravity(&report.xbutton, dsp, dsp->NEGrav, NorthEastGravity);
    else if ((report.xany.window == dsp->SEGrav) && (report.xany.type == ButtonPress) &&
             ((report.xbutton.button & 0xFF) == Button1))
      Set_BitGravity(&report.xbutton, dsp, dsp->SEGrav, SouthEastGravity);
    else if ((report.xany.window == dsp->SWGrav) && (report.xany.type == ButtonPress) &&
             ((report.xbutton.button & 0xFF) == Button1))
      Set_BitGravity(&report.xbutton, dsp, dsp->SWGrav, SouthWestGravity);
    else if ((report.xany.window == dsp->NWGrav) && (report.xany.type == ButtonPress) &&
             ((report.xbutton.button & 0xFF) == Button1))
      Set_BitGravity(&report.xbutton, dsp, dsp->NWGrav, NorthWestGravity);
    XFlush(dsp->display_id);
  } /* end while */
} /* end process_Xevents */
