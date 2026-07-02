/* $Id: xbbt.c,v 1.2 1999/01/03 02:07:46 sybalsky Exp $ (C) Copyright Venue, All Rights Reserved  */

/************************************************************************/
/*									*/
/*	(C) Copyright 1989, 1990, 1990, 1991, 1992, 1993, 1994, 1995 Venue.	*/
/*	    All Rights Reserved.		*/
/*	Manufactured in the United States of America.			*/
/*									*/
/************************************************************************/

#include "version.h"

#include <X11/Xlib.h>  // for XFlush, XPutImage
#include <stdlib.h>    // for realloc
#include <string.h>    // for memcpy
#include "devif.h"     // for (anonymous), MRegion, DspInterface
#include "lispemul.h"  // for DLword
#include "magdisplaydefs.h"  // for mag_display_invert_enabled
#include "xbbtdefs.h"  // for clipping_Xbitblt
#include "xdefs.h"     // for XLOCK, XUNLOCK

static XImage *mag_screen_image_for_put(DspInterface dsp) {
  static char *inverted_data = NULL;
  static size_t inverted_capacity = 0;
  static XImage inverted_image;
  XImage *source = &dsp->ScreenBitmap;
  size_t size;
  size_t i;

  if (!mag_display_invert_enabled()) return source;
  if (source->data == NULL || source->bytes_per_line <= 0 || source->height <= 0) return source;

  size = (size_t)source->bytes_per_line * (size_t)source->height;
  if (size > inverted_capacity) {
    char *next = realloc(inverted_data, size);
    if (next == NULL) return source;
    inverted_data = next;
    inverted_capacity = size;
  }

  memcpy(inverted_data, source->data, size);
  for (i = 0; i < size; i += 1) inverted_data[i] = (char)~((unsigned char)inverted_data[i]);

  inverted_image = *source;
  inverted_image.data = inverted_data;
  return &inverted_image;
}

/************************************************************************/
/*									*/
/*		    c l i p p i n g _ X b i t b l t			*/
/*									*/
/*	BITBLT from the display region to the X server's display,	*/
/*	clipping to fit the window we're in on the server.		*/
/*									*/
/*	dummy is the placeholder for the bitmap to be blitted		*/
/*									*/
/************************************************************************/
unsigned long clipping_Xbitblt(DspInterface dsp, DLword *dummy, int x, int y, int w, int h)
{
  int temp_x, temp_y, LowerRightX, LowerRightY;

  LowerRightX = dsp->Visible.x + (int)dsp->Visible.width - 1;
  LowerRightY = dsp->Visible.y + (int)dsp->Visible.height - 1;

  /* display region of interest lower right x, y pixel */
  temp_x = x + w - 1;
  temp_y = y + h - 1;

  /* if the display region of interest is completely outside the visible window */
  if ((temp_x < dsp->Visible.x) || (x > LowerRightX) || (temp_y < dsp->Visible.y) ||
      (y > LowerRightY))
    return (0);

  /* if the display region of interest is completely within the visible window */
  if ((x >= dsp->Visible.x) && (temp_x <= LowerRightX) && (y >= dsp->Visible.y) &&
      (temp_y <= LowerRightY)) {
    XLOCK;
    XPutImage(dsp->display_id, dsp->DisplayWindow, dsp->Copy_GC, mag_screen_image_for_put(dsp), x, y,
              x - dsp->Visible.x, y - dsp->Visible.y, (unsigned)w, (unsigned)h);
    XFlush(dsp->display_id);
    XUNLOCK(dsp);
    return (1);
  }

  /* clip left to visible window */
  if (x < dsp->Visible.x) {
    w -= dsp->Visible.x - x;
    x = dsp->Visible.x;
  }

  /* clip right to visible window */
  if (temp_x > LowerRightX) w -= temp_x - LowerRightX;

  /* clip top to visible window */
  if (y < dsp->Visible.y) {
    h -= dsp->Visible.y - y;
    y = dsp->Visible.y;
  }

  /* clip bottom to visible window */
  if (temp_y > LowerRightY) h -= temp_y - LowerRightY;

  if ((w > 0) && (h > 0)) {
    XLOCK;
    XPutImage(dsp->display_id, dsp->DisplayWindow, dsp->Copy_GC, mag_screen_image_for_put(dsp), x, y,
              x - dsp->Visible.x, y - dsp->Visible.y, (unsigned)w, (unsigned)h);
    XFlush(dsp->display_id);
    XUNLOCK(dsp);
  }
  return (1);

} /* end clipping_Xbitblt */
