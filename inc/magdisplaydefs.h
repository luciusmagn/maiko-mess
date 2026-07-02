#ifndef MAGDISPLAYDEFS_H
#define MAGDISPLAYDEFS_H 1

#include "devif.h"

#ifdef XWINDOW
int mag_display_invert_enabled(void);
int mag_display_poll_control_file(void);
void mag_display_control_redraw_if_needed(DspInterface dsp);
#endif

#endif
