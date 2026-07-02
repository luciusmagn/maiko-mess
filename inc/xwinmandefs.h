#ifndef XWINMANDEFS_H
#define XWINMANDEFS_H 1
#include <X11/X.h>     /* for Window */
#include <X11/Xlib.h>  /* for XButtonEvent */
#include "devif.h"     /* for DspInterface */
void Set_BitGravity(XButtonEvent *event, DspInterface dsp, Window window, int grav);
void enable_Xkeyboard(DspInterface dsp);
void disable_Xkeyboard(DspInterface dsp);
void beep_Xkeyboard(DspInterface dsp);
void process_Xevents(DspInterface dsp);
void maiko_handle_X_key(XKeyEvent *event, int upflg);
int mag_inject_typeahead_file(const char *path);
#endif
