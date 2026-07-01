/* %Z% %M% Version %I% (%G%). copyright venue & Fuji Xerox  */

/*

Unix Interface Communications

*/

/* Don't compile this at all under DOS. */
#ifndef DOS

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* Needed for ptsname on glibc systems. */
#endif

/************************************************************************/
/*									*/
/*	(C) Copyright 1989-1995 by Venue. All Rights Reserved.		*/
/*	Manufactured in the United States of America.			*/
/*									*/
/************************************************************************/

#include "version.h"

#include "lispemul.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <setjmp.h> /* JRB - timeout.h needs setjmp.h */
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#ifdef MAIKO_ENABLE_GHOSTTY_VT
#include <stdbool.h>
#include <stdint.h>
#include <ghostty/vt.h>
#endif

#include "address.h"
#include "adr68k.h"
#include "gcdata.h"
#include "lsptypes.h"
#include "lispmap.h"
#include "emlglob.h"
#include "lspglob.h"
#include "cell.h"
#include "stack.h"
#include "arith.h"
#include "dbprint.h"
#include "timeout.h"

#include "unixcommdefs.h"
#include "byteswapdefs.h"
#include "commondefs.h"

#ifdef XWINDOW
#define MAG_UNIX_HANDLECOMM_MAX 50
#define MAG_RUNTIME_TYPEAHEAD_PATH "/tmp/medley-mag-typeahead"
extern int mag_inject_typeahead_file(const char *path);
#else
#define MAG_UNIX_HANDLECOMM_MAX 48
#endif

static inline ssize_t SAFEREAD(int f, unsigned char *b, int c) {
  ssize_t res;
  do {
    res = read(f, b, c);
    if (res >= 0) return (res);
  } while (errno == EINTR || errno == EAGAIN);
  perror("reading UnixPipeIn");
  return (res);
}

#include "locfile.h" /* for LispStringToCString. */

/* JDS fixing prototypes char *malloc(size_t); */

int NPROCS = 100;

/* The following globals are used to communicate between Unix
   subprocesses and LISP */

/* One of these structures exists for every possible file descriptor */
/* type field encodes kind of stream:                                */

enum UJTYPE {
  UJUNUSED = 0,
  UJSHELL = -1,   /* PTY shell */
  UJPROCESS = -2, /* random process */
  UJSOCKET = -3,  /* socket open for connections */
  UJSOSTREAM = -4 /* connection from a UJSOCKET */
};

enum term_filter_state {
  TERM_FILTER_NORMAL = 0,
  TERM_FILTER_ESC,
  TERM_FILTER_OSC,
  TERM_FILTER_OSC_ESC
};

/* These are indexed by WRITE socket# */
struct unixjob {
  char *pathname; /* used by Lisp direct socket access subr */
  int PID;        /* process ID associated with this slot */
  int status;     /* status returned by subprocess (not shell) */
  enum UJTYPE type;
  enum term_filter_state filter_state;
#ifdef MAIKO_ENABLE_GHOSTTY_VT
  GhosttyTerminal ghostty_terminal;
  GhosttyRenderState ghostty_render;
  GhosttyKeyEncoder ghostty_key_encoder;
  GhosttyMouseEncoder ghostty_mouse_encoder;
  int ghostty_render_valid;
  uint64_t *ghostty_row_hashes;
  int ghostty_hash_rows;
  uint64_t ghostty_vt_write_calls;
  uint64_t ghostty_vt_write_bytes;
  uint64_t ghostty_render_update_calls;
  uint64_t ghostty_changed_row_scans;
  uint64_t ghostty_changed_rows_total;
  uint64_t ghostty_vt_write_us;
  uint64_t ghostty_render_update_us;
  uint64_t ghostty_changed_row_scan_us;
  uint64_t ghostty_last_update_us;
  uint64_t ghostty_last_scan_us;
  int ghostty_last_changed_rows;
#endif
};

struct unixjob *UJ; /* allocated at run time */

long StartTime; /* Time, for creating pipe filenames */

#define valid_slot(slot) ((slot) >= 0 && (slot) < NPROCS && UJ[slot].type != UJUNUSED)

char shcom[2048]; /* Here because I'm suspicious of */
                  /* large allocations on the stack */

static int unix_mag_appendf(unsigned char *out, int cap, int *used, const char *fmt, ...) {
  int remaining, n;
  va_list ap;

  if (out == NULL || used == NULL || cap <= 0) return -1;
  if (*used >= cap) return 1;

  remaining = cap - *used;
  va_start(ap, fmt);
  n = vsnprintf((char *)out + *used, (size_t)remaining, fmt, ap);
  va_end(ap);

  if (n < 0) return -1;
  if (n >= remaining) {
    *used = cap - 1;
    out[cap - 1] = '\0';
    return 1;
  }

  *used += n;
  return 0;
}

static void unixjob_init_slot(struct unixjob *job, enum UJTYPE type) {
  job->pathname = NULL;
  job->PID = -1;
  job->status = -1;
  job->type = type;
  job->filter_state = TERM_FILTER_NORMAL;
#ifdef MAIKO_ENABLE_GHOSTTY_VT
  job->ghostty_terminal = NULL;
  job->ghostty_render = NULL;
  job->ghostty_key_encoder = NULL;
  job->ghostty_mouse_encoder = NULL;
  job->ghostty_render_valid = 0;
  job->ghostty_row_hashes = NULL;
  job->ghostty_hash_rows = 0;
  job->ghostty_vt_write_calls = 0;
  job->ghostty_vt_write_bytes = 0;
  job->ghostty_render_update_calls = 0;
  job->ghostty_changed_row_scans = 0;
  job->ghostty_changed_rows_total = 0;
  job->ghostty_vt_write_us = 0;
  job->ghostty_render_update_us = 0;
  job->ghostty_changed_row_scan_us = 0;
  job->ghostty_last_update_us = 0;
  job->ghostty_last_scan_us = 0;
  job->ghostty_last_changed_rows = 0;
#endif
}

#ifdef MAIKO_ENABLE_GHOSTTY_VT
static void ghostty_job_reset_stats(struct unixjob *job) {
  if (job == NULL) return;
  job->ghostty_vt_write_calls = 0;
  job->ghostty_vt_write_bytes = 0;
  job->ghostty_render_update_calls = 0;
  job->ghostty_changed_row_scans = 0;
  job->ghostty_changed_rows_total = 0;
  job->ghostty_vt_write_us = 0;
  job->ghostty_render_update_us = 0;
  job->ghostty_changed_row_scan_us = 0;
  job->ghostty_last_update_us = 0;
  job->ghostty_last_scan_us = 0;
  job->ghostty_last_changed_rows = 0;
}
#endif

static int filter_terminal_output(struct unixjob *job, unsigned char *buf, int len) {
  int out = 0;

  for (int i = 0; i < len; i++) {
    unsigned char ch = buf[i];

    switch (job->filter_state) {
      case TERM_FILTER_NORMAL:
        if (ch == 0x1b)
          job->filter_state = TERM_FILTER_ESC;
        else
          buf[out++] = ch;
        break;

      case TERM_FILTER_ESC:
        if (ch == ']') {
          job->filter_state = TERM_FILTER_OSC;
        } else {
          buf[out++] = 0x1b;
          if (ch == 0x1b)
            job->filter_state = TERM_FILTER_ESC;
          else {
            buf[out++] = ch;
            job->filter_state = TERM_FILTER_NORMAL;
          }
        }
        break;

      case TERM_FILTER_OSC:
        if (ch == 0x07)
          job->filter_state = TERM_FILTER_NORMAL;
        else if (ch == 0x1b)
          job->filter_state = TERM_FILTER_OSC_ESC;
        break;

      case TERM_FILTER_OSC_ESC:
        if (ch == '\\' || ch == 0x07)
          job->filter_state = TERM_FILTER_NORMAL;
        else if (ch != 0x1b)
          job->filter_state = TERM_FILTER_OSC;
        break;
    }
  }

  return out;
}

static int tcp_connect_stream(const char *host, int port) {
  struct addrinfo hints, *result = NULL, *rp;
  char portstr[16];
  int gai = 0;
  int fd = -1;

  if (host == NULL || host[0] == '\0' || port <= 0 || port > 65535) return -1;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG;
  snprintf(portstr, sizeof(portstr), "%d", port);

  TIMEOUT(gai = getaddrinfo(host, portstr, &hints, &result));
  if (gai != 0) return -1;

  for (rp = result; rp != NULL; rp = rp->ai_next) {
    fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (fd < 0) continue;
    if (fd >= NPROCS) {
      close(fd);
      fd = -1;
      continue;
    }

    TIMEOUT(gai = connect(fd, rp->ai_addr, rp->ai_addrlen));
    if (gai == 0) break;

    close(fd);
    fd = -1;
  }

  freeaddrinfo(result);
  if (fd < 0) return -1;

  if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) == -1) {
    close(fd);
    return -1;
  }

  unixjob_init_slot(&UJ[fd], UJSOSTREAM);
  return fd;
}

static int unix_battery_status(unsigned char *out, int cap) {
  DIR *dir;
  struct dirent *entry;
  char batdir[512];
  char path[768];
  char status[64] = "";
  char capacity_buf[32] = "";
  FILE *file;
  int capacity = -1;
  const char *state = "unk";

  if (out == NULL || cap <= 0) return -1;
  dir = opendir("/sys/class/power_supply");
  if (dir == NULL) return -1;

  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "BAT", 3) != 0) continue;
    snprintf(batdir, sizeof(batdir), "/sys/class/power_supply/%s", entry->d_name);

    snprintf(path, sizeof(path), "%s/capacity", batdir);
    file = fopen(path, "r");
    if (file == NULL) continue;
    if (fgets(capacity_buf, sizeof(capacity_buf), file) != NULL)
      capacity = atoi(capacity_buf);
    fclose(file);
    if (capacity < 0) continue;

    snprintf(path, sizeof(path), "%s/status", batdir);
    file = fopen(path, "r");
    if (file != NULL) {
      if (fgets(status, sizeof(status), file) == NULL) status[0] = '\0';
      fclose(file);
    }
    break;
  }
  closedir(dir);

  if (capacity < 0) return -1;
  if (strncmp(status, "Charging", 8) == 0)
    state = "chg";
  else if (strncmp(status, "Discharging", 11) == 0)
    state = "dis";
  else if (strncmp(status, "Full", 4) == 0)
    state = "full";
  else if (strncmp(status, "Not charging", 12) == 0)
    state = "hold";

  return snprintf((char *)out, (size_t)cap, "B %d%% %s", capacity, state);
}

static int unix_mag_read_request(unsigned char *out, int cap) {
  const char *path = "/tmp/medley-mag-request";
  int fd;
  ssize_t n;

  if (out == NULL || cap <= 1) return -1;
  fd = open(path, O_RDONLY);
  if (fd < 0) return 0;

  do {
    n = read(fd, out, (size_t)(cap - 1));
  } while (n < 0 && errno == EINTR);
  close(fd);

  if (n < 0) return -1;
  out[n] = '\0';
  unlink(path);
  return (int)n;
}

#ifdef MAIKO_ENABLE_GHOSTTY_VT
static uint64_t unix_mag_now_us(void) {
  struct timeval tv;
  if (gettimeofday(&tv, NULL) < 0) return 0;
  return ((uint64_t)tv.tv_sec * 1000000ULL) + (uint64_t)tv.tv_usec;
}

static uint64_t unix_mag_elapsed_us(uint64_t start) {
  uint64_t end;
  if (start == 0) return 0;
  end = unix_mag_now_us();
  return (end >= start) ? (end - start) : 0;
}
#endif

static int unix_mag_debug_status(unsigned char *out, int cap) {
  int used = 0;
  int shells = 0;
  int processes = 0;
  int sockets = 0;
  int streams = 0;
  int unix_helper_alive = 0;
  int ghostty_shells = 0;
  int ghostty_render_valid = 0;
  uint64_t ghostty_vt_write_calls = 0;
  uint64_t ghostty_vt_write_bytes = 0;
  uint64_t ghostty_render_update_calls = 0;
  uint64_t ghostty_changed_row_scans = 0;
  uint64_t ghostty_changed_rows_total = 0;
  uint64_t ghostty_vt_write_us = 0;
  uint64_t ghostty_render_update_us = 0;
  uint64_t ghostty_changed_row_scan_us = 0;
  uint64_t ghostty_last_update_us = 0;
  uint64_t ghostty_last_scan_us = 0;
  int ghostty_hash_rows_total = 0;
  int ghostty_last_changed_rows_total = 0;
  char battery[64] = "battery unavailable";

  if (out == NULL || cap <= 0) return -1;

  if (UnixPID > 0 && (kill(UnixPID, 0) == 0 || errno == EPERM))
    unix_helper_alive = 1;

  if (UJ != NULL) {
    for (int i = 0; i < NPROCS; i++) {
      if (UJ[i].type == UJUNUSED) continue;
      used++;
      switch (UJ[i].type) {
        case UJSHELL:
          shells++;
#ifdef MAIKO_ENABLE_GHOSTTY_VT
          if (UJ[i].ghostty_terminal != NULL) {
            ghostty_shells++;
            ghostty_vt_write_calls += UJ[i].ghostty_vt_write_calls;
            ghostty_vt_write_bytes += UJ[i].ghostty_vt_write_bytes;
            ghostty_render_update_calls += UJ[i].ghostty_render_update_calls;
            ghostty_changed_row_scans += UJ[i].ghostty_changed_row_scans;
            ghostty_changed_rows_total += UJ[i].ghostty_changed_rows_total;
            ghostty_vt_write_us += UJ[i].ghostty_vt_write_us;
            ghostty_render_update_us += UJ[i].ghostty_render_update_us;
            ghostty_changed_row_scan_us += UJ[i].ghostty_changed_row_scan_us;
            ghostty_last_update_us += UJ[i].ghostty_last_update_us;
            ghostty_last_scan_us += UJ[i].ghostty_last_scan_us;
            ghostty_hash_rows_total += UJ[i].ghostty_hash_rows;
            ghostty_last_changed_rows_total += UJ[i].ghostty_last_changed_rows;
          }
          if (UJ[i].ghostty_render_valid) ghostty_render_valid++;
#endif
          break;
        case UJPROCESS:
          processes++;
          break;
        case UJSOCKET:
          sockets++;
          break;
        case UJSOSTREAM:
          streams++;
          break;
        case UJUNUSED:
          break;
      }
    }
  }

  if (unix_battery_status((unsigned char *)battery, sizeof(battery)) < 0)
    snprintf(battery, sizeof(battery), "battery unavailable");

  return snprintf((char *)out, (size_t)cap,
                  "mag-debug\n"
                  "pid=%ld\n"
                  "unix-helper=%d alive=%d\n"
                  "unix-pipes=%d/%d\n"
                  "unix-handlecomm-max=%d\n"
                  "nprocs=%d\n"
                  "jobs-used=%d\n"
                  "shells=%d\n"
                  "processes=%d\n"
                  "sockets=%d\n"
                  "streams=%d\n"
#ifdef MAIKO_ENABLE_GHOSTTY_VT
                  "ghostty-vt=enabled\n"
#else
                  "ghostty-vt=disabled\n"
#endif
                  "ghostty-shells=%d\n"
                  "ghostty-render-valid=%d\n"
                  "gt-write-calls=%llu\n"
                  "gt-write-bytes=%llu\n"
                  "gt-render-updates=%llu\n"
                  "gt-change-scans=%llu\n"
                  "gt-changed-rows=%llu\n"
                  "gt-write-us=%llu\n"
                  "gt-update-us=%llu\n"
                  "gt-scan-us=%llu\n"
                  "gt-last-update-us=%llu\n"
                  "gt-last-scan-us=%llu\n"
                  "gt-last-changed=%d\n"
                  "gt-hash-rows=%d\n"
                  "%s\n",
                  (long)getpid(), UnixPID, unix_helper_alive,
                  UnixPipeIn, UnixPipeOut, MAG_UNIX_HANDLECOMM_MAX,
                  NPROCS, used, shells, processes,
                  sockets, streams, ghostty_shells, ghostty_render_valid,
                  (unsigned long long)ghostty_vt_write_calls,
                  (unsigned long long)ghostty_vt_write_bytes,
                  (unsigned long long)ghostty_render_update_calls,
                  (unsigned long long)ghostty_changed_row_scans,
                  (unsigned long long)ghostty_changed_rows_total,
                  (unsigned long long)ghostty_vt_write_us,
                  (unsigned long long)ghostty_render_update_us,
                  (unsigned long long)ghostty_changed_row_scan_us,
                  (unsigned long long)ghostty_last_update_us,
                  (unsigned long long)ghostty_last_scan_us,
                  ghostty_last_changed_rows_total,
                  ghostty_hash_rows_total,
                  battery);
}

static unsigned int unix_mag_gc_htcoll_max(void) {
#ifdef BIGVM
  return (unsigned int)((HTCOLL_SIZE / DLWORDSPER_CELL) - 16);
#else
  return (unsigned int)(HTCOLL_SIZE - 16);
#endif
}

static int unix_mag_lisp_smallp_value(LispPTR value, int *out) {
  if ((value & SEGMASK) == S_POSITIVE) {
    *out = (int)(value & 0xFFFF);
    return 1;
  }
  if ((value & SEGMASK) == S_NEGATIVE) {
    *out = (int)(value | 0xFFFF0000);
    return 1;
  }
  return 0;
}

static void unix_mag_gc_htcoll_free_scan(unsigned int max, unsigned int *count,
                                         unsigned int *bad,
                                         unsigned int *last_offset) {
  GCENTRY offset;
  unsigned int guard = 0;

  *count = 0;
  *bad = 0;
  *last_offset = 0;
  if (HTcoll == NULL || max == 0) return;

  offset = GETGC(HTcoll);
  while (offset != 0) {
    *last_offset = (unsigned int)offset;
    if ((offset & 1) != 0 || offset + 1 >= max || guard >= max) {
      *bad = 1;
      return;
    }
    (*count)++;
    offset = GETGC((GCENTRY *)HTcoll + offset + 1);
    guard++;
  }
}

static unsigned int unix_mag_gc_htbig_capacity(void) {
  return (unsigned int)(((unsigned long)HTBIG_SIZE * BYTESPER_DLWORD) /
                        sizeof(struct gc_ovfl));
}

static int unix_mag_gc_status(unsigned char *out, int cap) {
  int used = 0;
  unsigned int htcoll_max;
  unsigned int htcoll_capacity_links;
  unsigned int htcoll_free_head = 0;
  unsigned int htcoll_nextfree = 0;
  unsigned int htcoll_highwater_links = 0;
  unsigned int htcoll_free_links = 0;
  unsigned int htcoll_live_links = 0;
  unsigned int htcoll_free_bad = 0;
  unsigned int htcoll_free_last = 0;
  unsigned int htcoll_live_pct = 0;
  unsigned int htcoll_highwater_pct = 0;
  unsigned int htbig_capacity;
  unsigned int htbig_used = 0;
  unsigned int htbig_free = 0;
  unsigned int htbig_empty = 0;
  unsigned int htbig_first_empty = 0;
  LispPTR reclaim_countdown = Reclaim_cnt_word != NULL ? *Reclaim_cnt_word : NIL;
  LispPTR reclaim_min = ReclaimMin_word != NULL ? *ReclaimMin_word : NIL;
  int reclaim_countdown_small = 0;
  int reclaim_min_small = 0;
  int reclaim_countdown_is_small =
      unix_mag_lisp_smallp_value(reclaim_countdown, &reclaim_countdown_small);
  int reclaim_min_is_small =
      unix_mag_lisp_smallp_value(reclaim_min, &reclaim_min_small);

  if (out == NULL || cap <= 0) return -1;
  if (HTcoll == NULL || HTbigcount == NULL) {
    return snprintf((char *)out, (size_t)cap,
                    "mag-gc\nstatus=unavailable reason=no-gc-tables\n");
  }

  htcoll_max = unix_mag_gc_htcoll_max();
  htcoll_capacity_links = htcoll_max > 2 ? (htcoll_max - 2) / 2 : 0;
  htcoll_free_head = (unsigned int)GETGC(HTcoll);
  htcoll_nextfree = (unsigned int)GETGC((GCENTRY *)HTcoll + 1);
  htcoll_highwater_links =
      htcoll_nextfree > 2 ? (htcoll_nextfree - 2) / 2 : 0;
  unix_mag_gc_htcoll_free_scan(htcoll_max, &htcoll_free_links, &htcoll_free_bad,
                               &htcoll_free_last);
  htcoll_live_links =
      htcoll_highwater_links >= htcoll_free_links
          ? htcoll_highwater_links - htcoll_free_links
          : 0;
  if (htcoll_capacity_links != 0) {
    htcoll_live_pct =
        (unsigned int)(((unsigned long)htcoll_live_links * 100UL) /
                       htcoll_capacity_links);
    htcoll_highwater_pct =
        (unsigned int)(((unsigned long)htcoll_highwater_links * 100UL) /
                       htcoll_capacity_links);
  }

  htbig_capacity = unix_mag_gc_htbig_capacity();
  {
    struct gc_ovfl *entry = (struct gc_ovfl *)HTbigcount;
    for (unsigned int i = 0; i < htbig_capacity; i++) {
      LispPTR ptr = entry[i].ovfl_ptr;
      if (ptr == NIL) {
        htbig_first_empty = i;
        htbig_empty = htbig_capacity - i;
        break;
      }
      if (ptr == ATOM_T)
        htbig_free++;
      else
        htbig_used++;
    }
  }

  if (unix_mag_appendf(out, cap, &used,
                       "mag-gc\n"
                       "gc-disabled=%d\n"
                       "htcoll-head=%u\n"
                       "htcoll-next=%u\n"
                       "htcoll-max=%u\n"
                       "htcoll-cap-links=%u\n"
                       "htcoll-hi-links=%u\n"
                       "htcoll-free-links=%u\n"
                       "htcoll-live-links=%u\n"
                       "htcoll-live-pct=%u\n"
                       "htcoll-hi-pct=%u\n"
                       "htcoll-free-bad=%u\n"
                       "htcoll-free-last=%u\n",
                       GcDisabled_word != NULL && *GcDisabled_word == ATOM_T,
                       htcoll_free_head, htcoll_nextfree, htcoll_max,
                       htcoll_capacity_links, htcoll_highwater_links,
                       htcoll_free_links, htcoll_live_links, htcoll_live_pct,
                       htcoll_highwater_pct, htcoll_free_bad,
                       htcoll_free_last) < 0)
    return -1;

  if (unix_mag_appendf(out, cap, &used,
                       "htbig-cap=%u\n"
                       "htbig-used=%u\n"
                       "htbig-free=%u\n"
                       "htbig-empty=%u\n"
                       "htbig-first-empty=%u\n",
                       htbig_capacity, htbig_used, htbig_free, htbig_empty,
                       htbig_first_empty) < 0)
    return -1;

  if (unix_mag_appendf(out, cap, &used,
                       "reclaim-countdown-raw=0x%08x\n",
                       reclaim_countdown) < 0)
    return -1;
  if (reclaim_countdown_is_small) {
    if (unix_mag_appendf(out, cap, &used, "reclaim-countdown-small=%d\n",
                         reclaim_countdown_small) < 0)
      return -1;
  } else if (unix_mag_appendf(out, cap, &used,
                              "reclaim-countdown-small=na\n") < 0) {
    return -1;
  }
  if (unix_mag_appendf(out, cap, &used, "reclaim-min-raw=0x%08x\n",
                       reclaim_min) < 0)
    return -1;
  if (reclaim_min_is_small) {
    if (unix_mag_appendf(out, cap, &used, "reclaim-min-small=%d\n",
                         reclaim_min_small) < 0)
      return -1;
  } else if (unix_mag_appendf(out, cap, &used, "reclaim-min-small=na\n") < 0) {
    return -1;
  }

  return used;
}

static int unix_mag_config_status(unsigned char *out, int cap) {
  extern int TIMER_INTERVAL;
  extern int noscroll;
  extern unsigned LispDisplayRequestedWidth, LispDisplayRequestedHeight;
  extern unsigned LispWindowRequestedWidth, LispWindowRequestedHeight;
#ifdef BIGBIGVM
  const unsigned max_vmem_mb = 256;
#elif defined(BIGVM)
  const unsigned max_vmem_mb = 64;
#else
  const unsigned max_vmem_mb = 32;
#endif

  if (out == NULL || cap <= 0) return -1;

  return snprintf((char *)out, (size_t)cap,
                  "mag-config\n"
                  "vmem-process-mb=%u\n"
                  "vmem-max-mb=%u\n"
                  "vmem-active-pages=%d\n"
                  "vmem-last-page=%u\n"
                  "timer-interval-us=%d\n"
                  "noscroll=%d\n"
                  "window=%ux%u\n"
                  "screen=%ux%u\n",
                  InterfacePage != NULL ? InterfacePage->process_size : 0,
                  max_vmem_mb,
                  InterfacePage != NULL ? InterfacePage->nactivepages : 0,
                  InterfacePage != NULL ? InterfacePage->dllastvmempage : 0,
                  TIMER_INTERVAL, noscroll, LispWindowRequestedWidth,
                  LispWindowRequestedHeight, LispDisplayRequestedWidth,
                  LispDisplayRequestedHeight);
}

static int unix_mag_gopher_viewport(int top, int selected, int delta, int count,
                                    int visible, int jump, unsigned char *out,
                                    int cap) {
  int old_top = top;
  int max_top;
  int repaint;

  if (out == NULL || cap < 9) return -1;
  if (visible < 1) visible = 1;
  if (jump < 1) jump = 1;

  if (count < 1) {
    top = 0;
    selected = 1;
  } else {
    selected += delta;
    if (selected < 1) selected = 1;
    if (selected > count) selected = count;

    max_top = count - visible;
    if (max_top < 0) max_top = 0;
    if (top < 0) top = 0;
    if (top > max_top) top = max_top;

    if (selected <= top) {
      int by_selected = selected - 1;
      int by_jump = top - jump;
      top = by_selected < by_jump ? by_selected : by_jump;
    } else if (selected > top + visible) {
      int by_selected = selected - visible;
      int by_jump = top + jump;
      top = by_selected > by_jump ? by_selected : by_jump;
    }

    if (top < 0) top = 0;
    if (top > max_top) top = max_top;
  }

  repaint = top != old_top;

  out[0] = (unsigned char)(top & 0xff);
  out[1] = (unsigned char)((top >> 8) & 0xff);
  out[2] = (unsigned char)((top >> 16) & 0xff);
  out[3] = (unsigned char)((top >> 24) & 0xff);
  out[4] = (unsigned char)(selected & 0xff);
  out[5] = (unsigned char)((selected >> 8) & 0xff);
  out[6] = (unsigned char)((selected >> 16) & 0xff);
  out[7] = (unsigned char)((selected >> 24) & 0xff);
  out[8] = repaint ? 1 : 0;

  return 9;
}

static int unix_mag_gopher_viewport_status(unsigned char *out, int cap) {
  struct viewport_case {
    const char *name;
    int top;
    int selected;
    int delta;
    int count;
    int visible;
    int jump;
    int expected_top;
    int expected_selected;
    int expected_repaint;
  };
  static const struct viewport_case cases[] = {
      {"same-viewport", 0, 1, 1, 60, 29, 10, 0, 2, 0},
      {"jump-down", 0, 1, 29, 60, 29, 10, 10, 30, 1},
      {"jump-up", 10, 30, -29, 60, 29, 10, 0, 1, 1},
      {"empty", 3, 4, 1, 0, 29, 10, 0, 1, 1},
  };
  int used;
  int fail = 0;
  int i;

  if (out == NULL || cap <= 0) return -1;

  used = snprintf((char *)out, (size_t)cap, "mag-gopher-viewport\n");
  if (used < 0 || used >= cap) return cap;

  for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
    unsigned char buf[9] = {0};
    int n = unix_mag_gopher_viewport(cases[i].top, cases[i].selected,
                                     cases[i].delta, cases[i].count,
                                     cases[i].visible, cases[i].jump, buf,
                                     sizeof(buf));
    int top = (int)buf[0] | ((int)buf[1] << 8) | ((int)buf[2] << 16) |
              ((int)buf[3] << 24);
    int selected = (int)buf[4] | ((int)buf[5] << 8) | ((int)buf[6] << 16) |
                   ((int)buf[7] << 24);
    int repaint = (n >= 9) ? (buf[8] != 0) : 0;
    int ok = n >= 9 && top == cases[i].expected_top &&
             selected == cases[i].expected_selected &&
             repaint == cases[i].expected_repaint;
    int wrote;

    if (!ok) fail = 1;
    if (used >= cap) break;
    wrote = snprintf((char *)out + used, (size_t)(cap - used),
                     "%s %s top=%d selected=%d repaint=%d\n",
                     ok ? "ok" : "FAIL", cases[i].name, top, selected,
                     repaint);
    if (wrote < 0) return -1;
    used += wrote;
  }

  if (used < cap) {
    int wrote = snprintf((char *)out + used, (size_t)(cap - used),
                         "status=%s\n", fail ? "FAIL" : "ok");
    if (wrote < 0) return -1;
    used += wrote;
  }

  return used;
}

static int unix_mag_gopher_label(int index, unsigned char *out, int cap) {
  int k = index;

  if (out == NULL || cap <= 0 || index < 0) return -1;

  if (k < 676) {
    if (cap < 2) return -1;
    out[0] = (unsigned char)('a' + (k / 26));
    out[1] = (unsigned char)('a' + (k % 26));
    return 2;
  }

  k -= 676;
  if (k >= 17576 || cap < 3) return -1;
  out[0] = (unsigned char)('a' + (k / 676));
  out[1] = (unsigned char)('a' + ((k % 676) / 26));
  out[2] = (unsigned char)('a' + (k % 26));
  return 3;
}

static const char *unix_mag_gopher_type_tag(int type) {
  switch (type) {
    case '0': return "TEXT";
    case '1': return "DIR ";
    case '2': return "CSO ";
    case '3': return "ERR ";
    case '4': return "HEX ";
    case '5': return "DOS ";
    case '6': return "UU  ";
    case '7': return "FIND";
    case '8': return "TEL ";
    case '9': return "BIN ";
    case '+': return "RED ";
    case ';': return "VID ";
    case 'P': return "PDF ";
    case 'T': return "TN32";
    case 'd': return "DOC ";
    case 'g': return "GIF ";
    case 'h': return "HTML";
    case 'i': return "    ";
    case 'I': return "IMG ";
    case 'p': return "PNG ";
    case 's': return "SND ";
    default: return "????";
  }
}

static int unix_mag_gopher_type_tag_copy(int type, unsigned char *out, int cap) {
  const char *tag;

  if (out == NULL || cap < 4) return -1;
  tag = unix_mag_gopher_type_tag(type);
  memcpy(out, tag, 4);
  return 4;
}

static const char *unixjob_type_name(enum UJTYPE type) {
  switch (type) {
    case UJUNUSED: return "unused";
    case UJSHELL: return "shell";
    case UJPROCESS: return "process";
    case UJSOCKET: return "socket";
    case UJSOSTREAM: return "stream";
    default: return "unknown";
  }
}

static int unix_mag_job_status(int slot, unsigned char *out, int cap) {
  struct unixjob *job;
  int ghostty = 0;
  int cols = -1;
  int rows = -1;
  int cursor_x = -1;
  int cursor_y = -1;
  int cursor_visible = -1;

  if (out == NULL || cap <= 0) return -1;
  if (UJ == NULL)
    return snprintf((char *)out, (size_t)cap, "mag-job\nslot=%d\nvalid=0\nreason=no UJ\n", slot);
  if (slot < 0 || slot >= NPROCS)
    return snprintf((char *)out, (size_t)cap, "mag-job\nslot=%d\nvalid=0\nreason=range\n", slot);

  job = &UJ[slot];
#ifdef MAIKO_ENABLE_GHOSTTY_VT
  if (job->type == UJSHELL && job->ghostty_terminal != NULL) {
    uint16_t u16 = 0;
    bool visible = false;
    ghostty = 1;
    if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_COLS, &u16) == GHOSTTY_SUCCESS)
      cols = (int)u16;
    if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_ROWS, &u16) == GHOSTTY_SUCCESS)
      rows = (int)u16;
    if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &u16) == GHOSTTY_SUCCESS)
      cursor_x = (int)u16;
    if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &u16) == GHOSTTY_SUCCESS)
      cursor_y = (int)u16;
    if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE, &visible) == GHOSTTY_SUCCESS)
      cursor_visible = visible ? 1 : 0;
  }
#endif

  return snprintf((char *)out, (size_t)cap,
                  "mag-job\n"
                  "slot=%d\n"
                  "valid=%d\n"
                  "type=%s\n"
                  "pid=%d\n"
                  "status=%d\n"
                  "ghostty=%d\n"
                  "cols=%d\n"
                  "rows=%d\n"
                  "cursor-x=%d\n"
                  "cursor-y=%d\n"
                  "cursor-visible=%d\n"
#ifdef MAIKO_ENABLE_GHOSTTY_VT
                  "render-valid=%d\n"
                  "write-calls=%llu\n"
                  "write-bytes=%llu\n"
	                  "render-updates=%llu\n"
	                  "change-scans=%llu\n"
	                  "changed-rows=%llu\n"
	                  "write-us=%llu\n"
	                  "update-us=%llu\n"
	                  "scan-us=%llu\n"
	                  "last-update-us=%llu\n"
	                  "last-scan-us=%llu\n"
	                  "last-changed=%d\n"
	                  "hash-rows=%d\n",
#else
                  "render-valid=0\n"
                  "write-calls=0\n"
                  "write-bytes=0\n"
	                  "render-updates=0\n"
	                  "change-scans=0\n"
	                  "changed-rows=0\n"
	                  "write-us=0\n"
	                  "update-us=0\n"
	                  "scan-us=0\n"
	                  "last-update-us=0\n"
	                  "last-scan-us=0\n"
	                  "last-changed=0\n"
	                  "hash-rows=0\n",
#endif
                  slot, job->type != UJUNUSED, unixjob_type_name(job->type),
                  job->PID, job->status, ghostty, cols, rows, cursor_x,
                  cursor_y, cursor_visible
#ifdef MAIKO_ENABLE_GHOSTTY_VT
                  , job->ghostty_render_valid,
                  (unsigned long long)job->ghostty_vt_write_calls,
                  (unsigned long long)job->ghostty_vt_write_bytes,
	                  (unsigned long long)job->ghostty_render_update_calls,
	                  (unsigned long long)job->ghostty_changed_row_scans,
	                  (unsigned long long)job->ghostty_changed_rows_total,
	                  (unsigned long long)job->ghostty_vt_write_us,
	                  (unsigned long long)job->ghostty_render_update_us,
	                  (unsigned long long)job->ghostty_changed_row_scan_us,
	                  (unsigned long long)job->ghostty_last_update_us,
	                  (unsigned long long)job->ghostty_last_scan_us,
	                  job->ghostty_last_changed_rows,
	                  job->ghostty_hash_rows
#endif
                  );
}

static int unix_mag_jobs_status(unsigned char *out, int cap) {
  int used = 0;
  int jobs = 0;
  int shells = 0;
  int ghostty_shells = 0;
  int emitted = 0;
  int omitted = 0;

  if (out == NULL || cap <= 0) return -1;
  if (unix_mag_appendf(out, cap, &used, "mag-jobs\nunix-handlecomm-max=%d\n",
                       MAG_UNIX_HANDLECOMM_MAX) < 0)
    return -1;

  if (UJ == NULL) {
    if (unix_mag_appendf(out, cap, &used, "status=unavailable reason=no UJ\n") < 0)
      return -1;
    return used;
  }

  for (int i = 0; i < NPROCS; i++) {
    if (UJ[i].type == UJUNUSED) continue;
    jobs++;
    if (UJ[i].type == UJSHELL) {
      shells++;
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      if (UJ[i].ghostty_terminal != NULL) ghostty_shells++;
#endif
    }
  }

  if (unix_mag_appendf(out, cap, &used,
                       "nprocs=%d jobs-used=%d shells=%d ghostty-shells=%d\n",
                       NPROCS, jobs, shells, ghostty_shells) < 0)
    return -1;

  for (int i = 0; i < NPROCS; i++) {
    struct unixjob *job;
    int ghostty = 0;
    int cols = -1;
    int rows = -1;
    int cursor_x = -1;
    int cursor_y = -1;
    int cursor_visible = -1;
    char line[192];
    int line_len;

    if (UJ[i].type == UJUNUSED) continue;
    job = &UJ[i];

#ifdef MAIKO_ENABLE_GHOSTTY_VT
    if (job->type == UJSHELL && job->ghostty_terminal != NULL) {
      uint16_t u16 = 0;
      bool visible = false;
      ghostty = 1;
      if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_COLS, &u16) == GHOSTTY_SUCCESS)
        cols = (int)u16;
      if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_ROWS, &u16) == GHOSTTY_SUCCESS)
        rows = (int)u16;
      if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &u16) == GHOSTTY_SUCCESS)
        cursor_x = (int)u16;
      if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &u16) == GHOSTTY_SUCCESS)
        cursor_y = (int)u16;
      if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE, &visible) == GHOSTTY_SUCCESS)
        cursor_visible = visible ? 1 : 0;
    }
#endif

    line_len = snprintf(line, sizeof(line),
                        "job slot=%d type=%s pid=%d status=%d ghostty=%d size=%dx%d cursor=%d,%d visible=%d calls=%llu rows=%llu\n",
                        i, unixjob_type_name(job->type), job->PID, job->status,
                        ghostty, cols, rows, cursor_x, cursor_y, cursor_visible,
#ifdef MAIKO_ENABLE_GHOSTTY_VT
                        (unsigned long long)job->ghostty_vt_write_calls,
                        (unsigned long long)job->ghostty_changed_rows_total
#else
                        0ULL, 0ULL
#endif
                        );
    if (line_len < 0) return -1;
    if (line_len >= (int)sizeof(line)) {
      line[sizeof(line) - 2] = '\n';
      line[sizeof(line) - 1] = '\0';
      line_len = (int)strlen(line);
    }
    if (used + line_len + 32 >= cap) {
      omitted++;
      continue;
    }
    if (unix_mag_appendf(out, cap, &used, "%s", line) < 0) return -1;
    emitted++;
  }

  if (used + 32 < cap) {
    if (unix_mag_appendf(out, cap, &used, "emitted=%d omitted=%d\n", emitted, omitted) < 0)
      return -1;
  }

  return used;
}

#ifdef MAIKO_ENABLE_GHOSTTY_VT
static void ghostty_job_cleanup(struct unixjob *job) {
  if (job->ghostty_key_encoder != NULL) {
    ghostty_key_encoder_free(job->ghostty_key_encoder);
    job->ghostty_key_encoder = NULL;
  }
  if (job->ghostty_mouse_encoder != NULL) {
    ghostty_mouse_encoder_free(job->ghostty_mouse_encoder);
    job->ghostty_mouse_encoder = NULL;
  }
  if (job->ghostty_render != NULL) {
    ghostty_render_state_free(job->ghostty_render);
    job->ghostty_render = NULL;
  }
  if (job->ghostty_terminal != NULL) {
    ghostty_terminal_free(job->ghostty_terminal);
    job->ghostty_terminal = NULL;
  }
  free(job->ghostty_row_hashes);
  job->ghostty_row_hashes = NULL;
  job->ghostty_hash_rows = 0;
  job->ghostty_render_valid = 0;
  job->ghostty_vt_write_calls = 0;
  job->ghostty_vt_write_bytes = 0;
  job->ghostty_render_update_calls = 0;
  job->ghostty_changed_row_scans = 0;
  job->ghostty_changed_rows_total = 0;
  job->ghostty_vt_write_us = 0;
  job->ghostty_render_update_us = 0;
  job->ghostty_changed_row_scan_us = 0;
  job->ghostty_last_update_us = 0;
  job->ghostty_last_scan_us = 0;
  job->ghostty_last_changed_rows = 0;
}

static ssize_t ghostty_write_all(int fd, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  size_t total = 0;

  while (total < len) {
    ssize_t n = write(fd, p + total, len - total);
    if (n > 0) {
      total += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    return total > 0 ? (ssize_t)total : -1;
  }

  return (ssize_t)total;
}

static void ghostty_write_pty(GhosttyTerminal terminal, void *userdata,
                              const uint8_t *data, size_t len) {
  (void)terminal;
  struct unixjob *job = (struct unixjob *)userdata;
  int slot;

  if (job == NULL || UJ == NULL || data == NULL || len == 0) return;
  slot = (int)(job - UJ);
  if (!valid_slot(slot) || UJ[slot].type != UJSHELL || UJ[slot].status != -1) return;
  (void)ghostty_write_all(slot, data, len);
}

static int ghostty_job_init(struct unixjob *job, uint16_t cols, uint16_t rows) {
	  GhosttyTerminalOptions opts = {
	      .cols = cols,
	      .rows = rows,
	      .max_scrollback = 1000,
	  };
  GhosttyTerminalWritePtyFn write_pty = ghostty_write_pty;

  ghostty_job_cleanup(job);
  if (ghostty_terminal_new(NULL, &job->ghostty_terminal, opts) != GHOSTTY_SUCCESS)
    return 0;
  if (ghostty_render_state_new(NULL, &job->ghostty_render) != GHOSTTY_SUCCESS) {
    ghostty_job_cleanup(job);
    return 0;
  }
  if (ghostty_key_encoder_new(NULL, &job->ghostty_key_encoder) != GHOSTTY_SUCCESS) {
    ghostty_job_cleanup(job);
    return 0;
  }
  if (ghostty_mouse_encoder_new(NULL, &job->ghostty_mouse_encoder) != GHOSTTY_SUCCESS) {
    ghostty_job_cleanup(job);
    return 0;
  }

  ghostty_terminal_set(job->ghostty_terminal, GHOSTTY_TERMINAL_OPT_USERDATA, job);
  ghostty_terminal_set(job->ghostty_terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY, write_pty);
  job->ghostty_render_valid = 0;
  return 1;
}

static void ghostty_job_write(struct unixjob *job, const unsigned char *buf, int len) {
  uint64_t start;
  uint64_t elapsed;
  if (job->ghostty_terminal == NULL || buf == NULL || len <= 0) return;
  start = unix_mag_now_us();
  ghostty_terminal_vt_write(job->ghostty_terminal, (const uint8_t *)buf, (size_t)len);
  elapsed = unix_mag_elapsed_us(start);
  job->ghostty_vt_write_calls++;
  job->ghostty_vt_write_bytes += (uint64_t)len;
  job->ghostty_vt_write_us += elapsed;
  job->ghostty_render_valid = 0;
}

static int ghostty_job_resize(struct unixjob *job, uint16_t rows, uint16_t cols) {
  if (job->ghostty_terminal == NULL) return 0;
  if (rows == 0 || cols == 0) return 0;
  if (ghostty_terminal_resize(job->ghostty_terminal, cols, rows, 8, 16) != GHOSTTY_SUCCESS)
    return 0;
  free(job->ghostty_row_hashes);
  job->ghostty_row_hashes = NULL;
  job->ghostty_hash_rows = 0;
  job->ghostty_render_valid = 0;
  return 1;
}

static int ghostty_job_scroll(struct unixjob *job, int kind, int amount) {
  GhosttyTerminalScrollViewport behavior;

  if (job == NULL || job->ghostty_terminal == NULL) return 0;
  memset(&behavior, 0, sizeof(behavior));
  switch (kind) {
    case 0:
      behavior.tag = GHOSTTY_SCROLL_VIEWPORT_TOP;
      break;
    case 1:
      behavior.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM;
      break;
    case 2:
      behavior.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
      behavior.value.delta = amount;
      break;
    default:
      return 0;
  }
  ghostty_terminal_scroll_viewport(job->ghostty_terminal, behavior);
  job->ghostty_render_valid = 0;
  return 1;
}

static int ghostty_job_update(struct unixjob *job) {
  GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FALSE;

  if (job->ghostty_terminal == NULL || job->ghostty_render == NULL) return -1;
  if (!job->ghostty_render_valid) {
    uint64_t start = unix_mag_now_us();
    uint64_t elapsed;
    if (ghostty_render_state_update(job->ghostty_render, job->ghostty_terminal) != GHOSTTY_SUCCESS)
      return -1;
    elapsed = unix_mag_elapsed_us(start);
    job->ghostty_render_update_calls++;
    job->ghostty_render_update_us += elapsed;
    job->ghostty_last_update_us = elapsed;
    job->ghostty_render_valid = 1;
  }
  if (ghostty_render_state_get(job->ghostty_render, GHOSTTY_RENDER_STATE_DATA_DIRTY,
                               &dirty) != GHOSTTY_SUCCESS)
    return -1;
  return (int)dirty;
}

static int ghostty_job_row_dirty(struct unixjob *job, int target_row) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyResult result;
  bool dirty = false;
  int y = 0;

  if (target_row < 0) return 0;
  if (ghostty_job_update(job) < 0) return 0;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return 0;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return 0;
  }

  while (ghostty_render_state_row_iterator_next(row_iter)) {
    if (y == target_row) {
      ghostty_render_state_row_get(row_iter,
                                   GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY,
                                   &dirty);
      break;
    }
    y++;
  }

  ghostty_render_state_row_iterator_free(row_iter);
  return dirty ? 1 : 0;
}

static int ghostty_job_clear_dirty(struct unixjob *job) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyRenderStateDirty clean_state = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
  GhosttyResult result;
  bool clean_row = false;

  if (job == NULL || job->ghostty_terminal == NULL || job->ghostty_render == NULL)
    return 0;
  if (ghostty_job_update(job) < 0) return 0;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return 0;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return 0;
  }

  while (ghostty_render_state_row_iterator_next(row_iter)) {
    ghostty_render_state_row_set(row_iter,
                                 GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY,
                                 &clean_row);
  }
  ghostty_render_state_row_iterator_free(row_iter);

  return ghostty_render_state_set(job->ghostty_render,
                                  GHOSTTY_RENDER_STATE_OPTION_DIRTY,
                                  &clean_state) == GHOSTTY_SUCCESS;
}

static int ghostty_job_copy_dirty_rows(struct unixjob *job, unsigned char *out, int cap) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyResult result;
  int y = 0;
  int n = 0;

  if (out == NULL || cap <= 0) return -1;
  if (job == NULL || job->ghostty_terminal == NULL || job->ghostty_render == NULL)
    return -1;
  if (ghostty_job_update(job) < 0) return -1;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return -1;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }

  while (n < cap && ghostty_render_state_row_iterator_next(row_iter)) {
    bool dirty = false;
    ghostty_render_state_row_get(row_iter,
                                 GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY,
                                 &dirty);
    if (dirty && y < 256) out[n++] = (unsigned char)y;
    y++;
  }

  ghostty_render_state_row_iterator_free(row_iter);
  return n;
}

static unsigned char ghostty_display_byte(uint32_t codepoint) {
  if (codepoint == 0x00a0) return ' ';
  if (codepoint >= 0x20 && codepoint <= 0x7e) return (unsigned char)codepoint;

  switch (codepoint) {
    case 0x00ad:
      return '-';
    case 0x00c0:
    case 0x00c1:
    case 0x00c2:
    case 0x00c3:
    case 0x00c4:
    case 0x00c5:
    case 0x0100:
    case 0x0102:
    case 0x0104:
      return 'A';
    case 0x00e0:
    case 0x00e1:
    case 0x00e2:
    case 0x00e3:
    case 0x00e4:
    case 0x00e5:
    case 0x0101:
    case 0x0103:
    case 0x0105:
      return 'a';
    case 0x00c7:
    case 0x0106:
    case 0x0108:
    case 0x010a:
    case 0x010c:
      return 'C';
    case 0x00e7:
    case 0x0107:
    case 0x0109:
    case 0x010b:
    case 0x010d:
      return 'c';
    case 0x010e:
    case 0x0110:
      return 'D';
    case 0x010f:
    case 0x0111:
      return 'd';
    case 0x00c8:
    case 0x00c9:
    case 0x00ca:
    case 0x00cb:
    case 0x0112:
    case 0x0114:
    case 0x0116:
    case 0x0118:
    case 0x011a:
      return 'E';
    case 0x00e8:
    case 0x00e9:
    case 0x00ea:
    case 0x00eb:
    case 0x0113:
    case 0x0115:
    case 0x0117:
    case 0x0119:
    case 0x011b:
      return 'e';
    case 0x00cc:
    case 0x00cd:
    case 0x00ce:
    case 0x00cf:
      return 'I';
    case 0x00ec:
    case 0x00ed:
    case 0x00ee:
    case 0x00ef:
      return 'i';
    case 0x00d1:
    case 0x0147:
      return 'N';
    case 0x00f1:
    case 0x0148:
      return 'n';
    case 0x00d2:
    case 0x00d3:
    case 0x00d4:
    case 0x00d5:
    case 0x00d6:
      return 'O';
    case 0x00f2:
    case 0x00f3:
    case 0x00f4:
    case 0x00f5:
    case 0x00f6:
      return 'o';
    case 0x0158:
      return 'R';
    case 0x0159:
      return 'r';
    case 0x015a:
    case 0x015c:
    case 0x015e:
    case 0x0160:
      return 'S';
    case 0x015b:
    case 0x015d:
    case 0x015f:
    case 0x0161:
      return 's';
    case 0x0164:
      return 'T';
    case 0x0165:
      return 't';
    case 0x00d9:
    case 0x00da:
    case 0x00db:
    case 0x00dc:
    case 0x016e:
      return 'U';
    case 0x00f9:
    case 0x00fa:
    case 0x00fb:
    case 0x00fc:
    case 0x016f:
      return 'u';
    case 0x00dd:
      return 'Y';
    case 0x00fd:
    case 0x00ff:
      return 'y';
    case 0x0179:
    case 0x017b:
    case 0x017d:
      return 'Z';
    case 0x017a:
    case 0x017c:
    case 0x017e:
      return 'z';
    case 0x2018:
    case 0x2019:
      return '\'';
    case 0x201c:
    case 0x201d:
      return '"';
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2212:
    case 0x2500:
    case 0x2501:
    case 0x2574:
    case 0x2576:
      return '-';
    case 0x2502:
    case 0x2503:
    case 0x2575:
    case 0x2577:
      return '|';
    case 0x2190:
      return '<';
    case 0x2191:
      return '^';
    case 0x2192:
      return '>';
    case 0x2193:
      return 'v';
    case 0x2304:
      return 'v';
    case 0x00b7:
    case 0x2022:
    case 0x2023:
    case 0x2043:
    case 0x25aa:
    case 0x25ab:
    case 0x25cf:
    case 0x25e6:
      return '*';
    case 0x25b2:
    case 0x25b4:
    case 0x25b5:
    case 0x25b7:
      return '^';
    case 0x25bc:
    case 0x25be:
    case 0x25bf:
    case 0x25c1:
      return 'v';
    case 0x25b6:
    case 0x25b8:
    case 0x25b9:
    case 0x276f:
      return '>';
    case 0x25c0:
    case 0x25c2:
    case 0x25c3:
      return '<';
    case 0x2713:
    case 0x2714:
      return 'v';
    case 0x2717:
    case 0x2718:
      return 'x';
    case 0x2026:
      return '.';
    default:
      break;
  }

  if (codepoint >= 0x2500 && codepoint <= 0x257f) return '+';
  if (codepoint >= 0x2580 && codepoint <= 0x259f) return '#';
  if (codepoint >= 0x2800 && codepoint <= 0x28ff) return '*';
  return '?';
}

static int ghostty_medley_bmp_supported(uint32_t codepoint) {
  if (codepoint >= 0x2500 && codepoint <= 0x257f) return 1;
  if (codepoint >= 0x20 && codepoint <= 0x7e) return 1;
  return 0;
}

static uint32_t ghostty_cell_codepoint(GhosttyRenderStateRowCells cells) {
  uint32_t grapheme_len = 0;
  uint32_t codepoints[8] = {0};

  if (ghostty_render_state_row_cells_get(cells,
                                         GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                                         &grapheme_len) != GHOSTTY_SUCCESS ||
      grapheme_len == 0)
    return ' ';
  if (ghostty_render_state_row_cells_get(cells,
                                         GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                                         codepoints) != GHOSTTY_SUCCESS ||
      codepoints[0] == 0)
    return ' ';
  return codepoints[0];
}

static uint16_t ghostty_display_bmp_codepoint(uint32_t codepoint) {
  if (ghostty_medley_bmp_supported(codepoint)) return (uint16_t)codepoint;
  return (uint16_t)ghostty_display_byte(codepoint);
}

#define GHOSTTY_BOX_LEFT 0x10
#define GHOSTTY_BOX_RIGHT 0x20
#define GHOSTTY_BOX_UP 0x40
#define GHOSTTY_BOX_DOWN 0x80

static int ghostty_codepoint_in_u16_list(uint32_t codepoint, const uint16_t *values,
                                         size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (codepoint == values[i]) return 1;
  }
  return 0;
}

static int ghostty_codepoint_in_range(uint32_t codepoint, uint32_t first, uint32_t last) {
  return codepoint >= first && codepoint <= last;
}

static unsigned char ghostty_box_flags(uint32_t codepoint) {
  static const uint16_t left_list[] = {
      9472, 9473, 9476, 9477, 9480, 9481, 9548, 9549, 9552, 9588, 9592, 9596, 9598, 9582, 9583};
  static const uint16_t right_list[] = {
      9472, 9473, 9476, 9477, 9480, 9481, 9548, 9549, 9552, 9590, 9594, 9596, 9598, 9581, 9584};
  static const uint16_t up_list[] = {
      9474, 9475, 9478, 9479, 9482, 9483, 9553, 9589, 9593, 9597, 9599, 9583, 9584};
  static const uint16_t down_list[] = {
      9474, 9475, 9478, 9479, 9482, 9483, 9553, 9591, 9595, 9597, 9599, 9581, 9582};
  unsigned char flags = 0;

  if (!ghostty_codepoint_in_range(codepoint, 0x2500, 0x257f)) return 0;

  if (ghostty_codepoint_in_u16_list(codepoint, left_list,
                                    sizeof(left_list) / sizeof(left_list[0])) ||
      ghostty_codepoint_in_range(codepoint, 9488, 9491) ||
      ghostty_codepoint_in_range(codepoint, 9496, 9499) ||
      ghostty_codepoint_in_range(codepoint, 9508, 9515) ||
      ghostty_codepoint_in_range(codepoint, 9516, 9547) ||
      ghostty_codepoint_in_range(codepoint, 9557, 9565) ||
      ghostty_codepoint_in_range(codepoint, 9569, 9580))
    flags |= GHOSTTY_BOX_LEFT;

  if (ghostty_codepoint_in_u16_list(codepoint, right_list,
                                    sizeof(right_list) / sizeof(right_list[0])) ||
      ghostty_codepoint_in_range(codepoint, 9484, 9487) ||
      ghostty_codepoint_in_range(codepoint, 9492, 9495) ||
      ghostty_codepoint_in_range(codepoint, 9500, 9507) ||
      ghostty_codepoint_in_range(codepoint, 9516, 9547) ||
      ghostty_codepoint_in_range(codepoint, 9554, 9562) ||
      ghostty_codepoint_in_range(codepoint, 9566, 9580))
    flags |= GHOSTTY_BOX_RIGHT;

  if (ghostty_codepoint_in_u16_list(codepoint, up_list,
                                    sizeof(up_list) / sizeof(up_list[0])) ||
      ghostty_codepoint_in_range(codepoint, 9492, 9499) ||
      ghostty_codepoint_in_range(codepoint, 9500, 9515) ||
      ghostty_codepoint_in_range(codepoint, 9524, 9547) ||
      ghostty_codepoint_in_range(codepoint, 9560, 9571) ||
      ghostty_codepoint_in_range(codepoint, 9575, 9580))
    flags |= GHOSTTY_BOX_UP;

  if (ghostty_codepoint_in_u16_list(codepoint, down_list,
                                    sizeof(down_list) / sizeof(down_list[0])) ||
      ghostty_codepoint_in_range(codepoint, 9484, 9491) ||
      ghostty_codepoint_in_range(codepoint, 9500, 9523) ||
      ghostty_codepoint_in_range(codepoint, 9532, 9547) ||
      ghostty_codepoint_in_range(codepoint, 9554, 9574) ||
      ghostty_codepoint_in_range(codepoint, 9578, 9580))
    flags |= GHOSTTY_BOX_DOWN;

  return flags == 0 ? (GHOSTTY_BOX_LEFT | GHOSTTY_BOX_RIGHT | GHOSTTY_BOX_UP | GHOSTTY_BOX_DOWN)
                    : flags;
}

static unsigned char ghostty_style_flags(const GhosttyStyle *style) {
  unsigned char flags = 0;

  if (style == NULL) return 0;
  if (style->inverse) flags |= 1;
  if (style->bold) flags |= 2;
  if (style->underline) flags |= 4;
  if (style->italic) flags |= 8;
  return flags;
}

static unsigned char ghostty_cell_flags(GhosttyRenderStateRowCells cells, uint32_t codepoint,
                                        int *invisible) {
  GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
  unsigned char flags = ghostty_box_flags(codepoint);

  if (invisible != NULL) *invisible = 0;
  if (ghostty_render_state_row_cells_get(cells,
                                         GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                                         &style) == GHOSTTY_SUCCESS) {
    flags |= ghostty_style_flags(&style);
    if (style.invisible && invisible != NULL) *invisible = 1;
  }
  return flags;
}


static unsigned char ghostty_nearest_ansi_color(GhosttyColorRgb color) {
  static const GhosttyColorRgb palette[16] = {
      {0, 0, 0},       {205, 49, 49},    {13, 188, 121},   {229, 229, 16},
      {36, 114, 200},  {188, 63, 188},   {17, 168, 205},   {229, 229, 229},
      {102, 102, 102}, {241, 76, 76},    {35, 209, 139},   {245, 245, 67},
      {59, 142, 234},  {214, 112, 214},  {41, 184, 219},   {255, 255, 255},
  };
  unsigned int best_distance = ~0u;
  unsigned char best = 7;

  for (unsigned char i = 0; i < 16; i++) {
    int dr = (int)color.r - (int)palette[i].r;
    int dg = (int)color.g - (int)palette[i].g;
    int db = (int)color.b - (int)palette[i].b;
    unsigned int distance = (unsigned int)(dr * dr + dg * dg + db * db);
    if (distance < best_distance) {
      best_distance = distance;
      best = i;
    }
  }

  return best;
}

static unsigned char ghostty_cell_color_index(GhosttyRenderStateRowCells cells,
                                              GhosttyRenderStateRowCellsData data) {
  GhosttyColorRgb color = {0};

  if (ghostty_render_state_row_cells_get(cells, data, &color) != GHOSTTY_SUCCESS)
    return 255;
  return ghostty_nearest_ansi_color(color);
}

static unsigned char ghostty_cell_fg_index(GhosttyRenderStateRowCells cells) {
  GhosttyColorRgb color = {0};

  if (ghostty_render_state_row_cells_get(cells,
                                         GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                                         &color) != GHOSTTY_SUCCESS)
    return 255;

  /*
   * Ghostty's default foreground is a light gray. On the Acme off-white Medley
   * background that reads as "missing" text, so render default-ish light text as
   * black unless an application chooses a more specific color.
   */
  if (color.r >= 220 && color.g >= 220 && color.b >= 220)
    return 0;

  return ghostty_nearest_ansi_color(color);
}

static unsigned char ghostty_cell_bg_index(GhosttyRenderStateRowCells cells) {
  unsigned char bg = ghostty_cell_color_index(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR);

  /*
   * The embedded terminal's built-in default background is black, but Mag Shell
   * has already cleared the row to the Medley window background. Treat black as
   * transparent/default so prompts do not become black-on-black or black blocks.
   */
  return bg == 0 ? 255 : bg;
}

static uint64_t ghostty_hash_mix(uint64_t hash, uint64_t value) {
  for (int i = 0; i < 8; i++) {
    hash ^= (value >> (i * 8)) & 0xff;
    hash *= 1099511628211ULL;
  }
  return hash;
}

static uint64_t ghostty_rendered_row_hash(GhosttyRenderStateRowCells cells) {
  uint64_t hash = 1469598103934665603ULL;
  uint64_t cell_count = 0;

  while (ghostty_render_state_row_cells_next(cells)) {
    uint32_t codepoint = ghostty_cell_codepoint(cells);
    uint16_t ch = ghostty_display_bmp_codepoint(codepoint);
    int invisible = 0;
    unsigned char flags = ghostty_cell_flags(cells, codepoint, &invisible);
    unsigned char fg = ghostty_cell_fg_index(cells);
    unsigned char bg = ghostty_cell_bg_index(cells);

    if (invisible) ch = ' ';

    hash = ghostty_hash_mix(hash, ch);
    hash = ghostty_hash_mix(hash, flags);
    hash = ghostty_hash_mix(hash, fg);
    hash = ghostty_hash_mix(hash, bg);
    cell_count++;
  }

  return ghostty_hash_mix(hash, cell_count);
}

static int ghostty_job_ensure_row_hashes(struct unixjob *job, int rows) {
  uint64_t *hashes;

  if (rows <= job->ghostty_hash_rows) return 1;
  hashes = (uint64_t *)realloc(job->ghostty_row_hashes, (size_t)rows * sizeof(uint64_t));
  if (hashes == NULL) return 0;
  memset(hashes + job->ghostty_hash_rows, 0,
         (size_t)(rows - job->ghostty_hash_rows) * sizeof(uint64_t));
  job->ghostty_row_hashes = hashes;
  job->ghostty_hash_rows = rows;
  return 1;
}

static int ghostty_job_copy_changed_rows(struct unixjob *job, unsigned char *out, int cap) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyRenderStateRowCells cells = NULL;
  GhosttyResult result;
  uint64_t start = 0;
  uint64_t elapsed;
  int y = 0;
  int n = 0;

  if (out == NULL || cap <= 0) return -1;
  if (job == NULL || job->ghostty_terminal == NULL || job->ghostty_render == NULL)
    return -1;
  job->ghostty_changed_row_scans++;
  start = unix_mag_now_us();
  if (ghostty_job_update(job) < 0) return -1;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return -1;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }
  result = ghostty_render_state_row_cells_new(NULL, &cells);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }

  while (ghostty_render_state_row_iterator_next(row_iter)) {
    uint64_t hash;

    if (!ghostty_job_ensure_row_hashes(job, y + 1)) {
      n = -1;
      break;
    }
    result = ghostty_render_state_row_get(row_iter,
                                          GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                          &cells);
    if (result != GHOSTTY_SUCCESS) {
      n = -1;
      break;
    }
    hash = ghostty_rendered_row_hash(cells);
    if (hash != job->ghostty_row_hashes[y]) {
      job->ghostty_row_hashes[y] = hash;
      if (n < cap && y < 256) out[n++] = (unsigned char)y;
    }
    y++;
  }

  ghostty_render_state_row_cells_free(cells);
  ghostty_render_state_row_iterator_free(row_iter);
  elapsed = unix_mag_elapsed_us(start);
  job->ghostty_changed_row_scan_us += elapsed;
  job->ghostty_last_scan_us = elapsed;
  if (n >= 0) {
    job->ghostty_last_changed_rows = n;
    job->ghostty_changed_rows_total += (uint64_t)n;
  }
  return n;
}

static GhosttyColorRgb ghostty_cell_color_rgb(GhosttyRenderStateRowCells cells,
                                              GhosttyRenderStateRowCellsData data,
                                              GhosttyColorRgb fallback) {
  GhosttyColorRgb color = fallback;

  ghostty_render_state_row_cells_get(cells, data, &color);
  return color;
}

static int ghostty_job_copy_row(struct unixjob *job, int row, unsigned char *out, int cap) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyRenderStateRowCells cells = NULL;
  GhosttyResult result;
  int y = 0;
  int n = 0;

  if (out == NULL || cap <= 0) return -1;
  if (row < 0) return -1;
  if (ghostty_job_update(job) < 0) return -1;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return -1;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }
  result = ghostty_render_state_row_cells_new(NULL, &cells);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }

  while (ghostty_render_state_row_iterator_next(row_iter)) {
    if (y == row) {
      result = ghostty_render_state_row_get(row_iter,
                                            GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                            &cells);
      if (result != GHOSTTY_SUCCESS) {
        n = -1;
        break;
      }

      while (n < cap && ghostty_render_state_row_cells_next(cells)) {
        uint32_t grapheme_len = 0;
        uint32_t codepoints[8] = {0};
        unsigned char ch = ' ';

        ghostty_render_state_row_cells_get(cells,
                                           GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                                           &grapheme_len);
        if (grapheme_len > 0) {
          ghostty_render_state_row_cells_get(cells,
                                             GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                                             codepoints);
          ch = ghostty_display_byte(codepoints[0]);
        }
        out[n++] = ch;
      }
      while (n > 0 && out[n - 1] == ' ') n--;
      break;
    }
    y++;
  }

  ghostty_render_state_row_cells_free(cells);
  ghostty_render_state_row_iterator_free(row_iter);
  return n;
}

static int ghostty_job_copy_row_styled(struct unixjob *job, int row, unsigned char *out, int cap) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyRenderStateRowCells cells = NULL;
  GhosttyResult result;
  int y = 0;
  int n = 0;

  if (out == NULL || cap <= 1) return -1;
  if (row < 0) return -1;
  if (ghostty_job_update(job) < 0) return -1;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return -1;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }
  result = ghostty_render_state_row_cells_new(NULL, &cells);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }

  while (ghostty_render_state_row_iterator_next(row_iter)) {
    if (y == row) {
      result = ghostty_render_state_row_get(row_iter,
                                            GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                            &cells);
      if (result != GHOSTTY_SUCCESS) {
        n = -1;
        break;
      }

      while ((n + 1) < cap && ghostty_render_state_row_cells_next(cells)) {
        uint32_t grapheme_len = 0;
        uint32_t codepoints[8] = {0};
        uint32_t codepoint = ' ';
        unsigned char ch = ' ';
        int invisible = 0;
        unsigned char flags;

        ghostty_render_state_row_cells_get(cells,
                                           GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                                           &grapheme_len);
        if (grapheme_len > 0) {
          ghostty_render_state_row_cells_get(cells,
                                             GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                                             codepoints);
          codepoint = codepoints[0];
          ch = ghostty_display_byte(codepoint);
        }
        flags = ghostty_cell_flags(cells, codepoint, &invisible);
        if (invisible) ch = ' ';

        out[n++] = ch;
        out[n++] = flags;
      }
      while (n >= 2 && out[n - 2] == ' ' && out[n - 1] == 0) n -= 2;
      break;
    }
    y++;
  }

  ghostty_render_state_row_cells_free(cells);
  ghostty_render_state_row_iterator_free(row_iter);
  return n / 2;
}

static int ghostty_job_copy_row_colored(struct unixjob *job, int row, unsigned char *out, int cap) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyRenderStateRowCells cells = NULL;
  GhosttyResult result;
  int y = 0;
  int n = 0;

  if (out == NULL || cap <= 3) return -1;
  if (row < 0) return -1;
  if (ghostty_job_update(job) < 0) return -1;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return -1;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }
  result = ghostty_render_state_row_cells_new(NULL, &cells);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }

  while (ghostty_render_state_row_iterator_next(row_iter)) {
    if (y == row) {
      result = ghostty_render_state_row_get(row_iter,
                                            GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                            &cells);
      if (result != GHOSTTY_SUCCESS) {
        n = -1;
        break;
      }

      while ((n + 3) < cap && ghostty_render_state_row_cells_next(cells)) {
        uint32_t grapheme_len = 0;
        uint32_t codepoints[8] = {0};
        uint32_t codepoint = ' ';
        unsigned char ch = ' ';
        int invisible = 0;
        unsigned char flags;
        unsigned char fg = ghostty_cell_fg_index(cells);
        unsigned char bg = ghostty_cell_bg_index(cells);

        ghostty_render_state_row_cells_get(cells,
                                           GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                                           &grapheme_len);
        if (grapheme_len > 0) {
          ghostty_render_state_row_cells_get(cells,
                                             GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                                             codepoints);
          codepoint = codepoints[0];
          ch = ghostty_display_byte(codepoint);
        }
        flags = ghostty_cell_flags(cells, codepoint, &invisible);
        if (invisible) ch = ' ';

        out[n++] = ch;
        out[n++] = flags;
        out[n++] = fg;
        out[n++] = bg;
      }
      while (n >= 4 && out[n - 4] == ' ' && out[n - 3] == 0 &&
             out[n - 2] == 255 && out[n - 1] == 255)
        n -= 4;
      break;
    }
    y++;
  }

  ghostty_render_state_row_cells_free(cells);
  ghostty_render_state_row_iterator_free(row_iter);
  return n / 4;
}

static int ghostty_job_copy_row_bmp_colored(struct unixjob *job, int row, unsigned char *out, int cap) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyRenderStateRowCells cells = NULL;
  GhosttyResult result;
  int y = 0;
  int n = 0;
  int cell_count = 0;
  int last_nonblank_count = 0;
  int last_nonblank_n = 0;
  int failed = 0;

  if (out == NULL || cap <= 4) return -1;
  if (row < 0) return -1;
  if (ghostty_job_update(job) < 0) return -1;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return -1;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }
  result = ghostty_render_state_row_cells_new(NULL, &cells);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }

  while (ghostty_render_state_row_iterator_next(row_iter)) {
    if (y == row) {
      result = ghostty_render_state_row_get(row_iter,
                                            GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                            &cells);
      if (result != GHOSTTY_SUCCESS) {
        failed = 1;
        break;
      }

      while ((n + 3) < cap && ghostty_render_state_row_cells_next(cells)) {
        uint32_t codepoint = ghostty_cell_codepoint(cells);
        uint16_t ch = ghostty_display_bmp_codepoint(codepoint);
        int invisible = 0;
        unsigned char flags = ghostty_cell_flags(cells, codepoint, &invisible);
        unsigned char fg = ghostty_cell_fg_index(cells);
        unsigned char bg = ghostty_cell_bg_index(cells);

        if (invisible) ch = ' ';

        if (ch < 255) {
          if ((n + 3) >= cap) break;
          out[n++] = (unsigned char)ch;
          out[n++] = flags;
          out[n++] = fg;
          out[n++] = bg;
        } else {
          if ((n + 5) >= cap) break;
          out[n++] = 255;
          out[n++] = (unsigned char)(ch & 0xff);
          out[n++] = (unsigned char)((ch >> 8) & 0xff);
          out[n++] = flags;
          out[n++] = fg;
          out[n++] = bg;
        }
        cell_count++;
        if (!(ch == ' ' && flags == 0 && fg == 255 && bg == 255)) {
          last_nonblank_count = cell_count;
          last_nonblank_n = n;
        }
      }
      n = last_nonblank_n;
      cell_count = last_nonblank_count;
      break;
    }
    y++;
  }

  ghostty_render_state_row_cells_free(cells);
  ghostty_render_state_row_iterator_free(row_iter);
  return failed ? -1 : cell_count;
}

static int ghostty_job_copy_row_bmp_plain(struct unixjob *job, int row, unsigned char *out, int cap) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyRenderStateRowCells cells = NULL;
  GhosttyResult result;
  int y = 0;
  int n = 0;
  int cell_count = 0;
  int last_nonblank_count = 0;
  int last_nonblank_n = 0;
  int failed = 0;

  if (out == NULL || cap <= 3) return -1;
  if (row < 0) return -1;
  if (ghostty_job_update(job) < 0) return -1;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return -1;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }
  result = ghostty_render_state_row_cells_new(NULL, &cells);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }

  while (ghostty_render_state_row_iterator_next(row_iter)) {
    if (y == row) {
      result = ghostty_render_state_row_get(row_iter,
                                            GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                            &cells);
      if (result != GHOSTTY_SUCCESS) {
        failed = 1;
        break;
      }

      while (ghostty_render_state_row_cells_next(cells)) {
        uint16_t ch = ghostty_display_bmp_codepoint(ghostty_cell_codepoint(cells));

        if (ch < 255) {
          if (n >= cap) break;
          out[n++] = (unsigned char)ch;
        } else {
          if ((n + 2) >= cap) break;
          out[n++] = 255;
          out[n++] = (unsigned char)(ch & 0xff);
          out[n++] = (unsigned char)((ch >> 8) & 0xff);
        }
        cell_count++;
        if (ch != ' ') {
          last_nonblank_count = cell_count;
          last_nonblank_n = n;
        }
      }
      n = last_nonblank_n;
      cell_count = last_nonblank_count;
      break;
    }
    y++;
  }

  ghostty_render_state_row_cells_free(cells);
  ghostty_render_state_row_iterator_free(row_iter);
  return failed ? -1 : cell_count;
}

static int ghostty_job_copy_row_bmp_colored_segment(struct unixjob *job, int row, int start_cell,
                                                    unsigned char *out, int cap) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyRenderStateRowCells cells = NULL;
  GhosttyResult result;
  int y = 0;
  int n = 0;
  int cell_index = 0;
  int copied = 0;
  int failed = 0;

  if (out == NULL || cap <= 4) return -1;
  if (row < 0 || start_cell < 0) return -1;
  if (ghostty_job_update(job) < 0) return -1;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return -1;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }
  result = ghostty_render_state_row_cells_new(NULL, &cells);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }

  while (ghostty_render_state_row_iterator_next(row_iter)) {
    if (y == row) {
      result = ghostty_render_state_row_get(row_iter,
                                            GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                            &cells);
      if (result != GHOSTTY_SUCCESS) {
        failed = 1;
        break;
      }

      while ((n + 3) < cap && ghostty_render_state_row_cells_next(cells)) {
        uint16_t ch;
        unsigned char flags = 0;
        uint32_t codepoint;
        int invisible = 0;
        unsigned char fg;
        unsigned char bg;

        if (cell_index++ < start_cell) continue;

        codepoint = ghostty_cell_codepoint(cells);
        ch = ghostty_display_bmp_codepoint(codepoint);
        fg = ghostty_cell_fg_index(cells);
        bg = ghostty_cell_bg_index(cells);
        flags = ghostty_cell_flags(cells, codepoint, &invisible);

        if (invisible) ch = ' ';

        if (ch < 255) {
          if ((n + 3) >= cap) break;
          out[n++] = (unsigned char)ch;
          out[n++] = flags;
          out[n++] = fg;
          out[n++] = bg;
        } else {
          if ((n + 5) >= cap) break;
          out[n++] = 255;
          out[n++] = (unsigned char)(ch & 0xff);
          out[n++] = (unsigned char)((ch >> 8) & 0xff);
          out[n++] = flags;
          out[n++] = fg;
          out[n++] = bg;
        }
        copied++;
      }
      break;
    }
    y++;
  }

  ghostty_render_state_row_cells_free(cells);
  ghostty_render_state_row_iterator_free(row_iter);
  return failed ? -1 : copied;
}

static int ghostty_job_copy_row_bmp_rgb_segment(struct unixjob *job, int row, int start_cell,
                                                unsigned char *out, int cap) {
  GhosttyRenderStateRowIterator row_iter = NULL;
  GhosttyRenderStateRowCells cells = NULL;
  GhosttyResult result;
  int y = 0;
  int n = 0;
  int cell_index = 0;
  int copied = 0;
  int failed = 0;

  if (out == NULL || cap <= 8) return -1;
  if (row < 0 || start_cell < 0) return -1;
  if (ghostty_job_update(job) < 0) return -1;
  result = ghostty_render_state_row_iterator_new(NULL, &row_iter);
  if (result != GHOSTTY_SUCCESS) return -1;
  result = ghostty_render_state_get(job->ghostty_render,
                                    GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
                                    &row_iter);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }
  result = ghostty_render_state_row_cells_new(NULL, &cells);
  if (result != GHOSTTY_SUCCESS) {
    ghostty_render_state_row_iterator_free(row_iter);
    return -1;
  }

  while (ghostty_render_state_row_iterator_next(row_iter)) {
    if (y == row) {
      result = ghostty_render_state_row_get(row_iter,
                                            GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                                            &cells);
      if (result != GHOSTTY_SUCCESS) {
        failed = 1;
        break;
      }

      while (ghostty_render_state_row_cells_next(cells)) {
        GhosttyColorRgb fg;
        GhosttyColorRgb bg;
        uint16_t ch;
        uint32_t codepoint;
        int invisible = 0;
        unsigned char flags = 0;

        if (cell_index++ < start_cell) continue;

        codepoint = ghostty_cell_codepoint(cells);
        ch = ghostty_display_bmp_codepoint(codepoint);
        fg = ghostty_cell_color_rgb(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                                    (GhosttyColorRgb){229, 229, 229});
        bg = ghostty_cell_color_rgb(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                                    (GhosttyColorRgb){0, 0, 0});
        flags = ghostty_cell_flags(cells, codepoint, &invisible);

        if (invisible) ch = ' ';

        if (ch < 255) {
          if ((n + 7) >= cap) break;
          out[n++] = (unsigned char)ch;
          out[n++] = flags;
        } else {
          if ((n + 9) >= cap) break;
          out[n++] = 255;
          out[n++] = (unsigned char)(ch & 0xff);
          out[n++] = (unsigned char)((ch >> 8) & 0xff);
          out[n++] = flags;
        }
        out[n++] = fg.r;
        out[n++] = fg.g;
        out[n++] = fg.b;
        out[n++] = bg.r;
        out[n++] = bg.g;
        out[n++] = bg.b;
        copied++;
      }
      break;
    }
    y++;
  }

  ghostty_render_state_row_cells_free(cells);
  ghostty_render_state_row_iterator_free(row_iter);
  return failed ? -1 : copied;
}

static int ghostty_job_copy_meta(struct unixjob *job, unsigned char *out, int cap) {
  uint16_t cols = 0, rows = 0, cx = 0, cy = 0;
  bool visible = false;

  if (out == NULL || cap < 9 || job->ghostty_terminal == NULL) return -1;
  if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_COLS, &cols) != GHOSTTY_SUCCESS)
    return -1;
  if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_ROWS, &rows) != GHOSTTY_SUCCESS)
    return -1;
  ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_X, &cx);
  ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_Y, &cy);
  ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_CURSOR_VISIBLE, &visible);

  out[0] = (unsigned char)(cols & 0xff);
  out[1] = (unsigned char)((cols >> 8) & 0xff);
  out[2] = (unsigned char)(rows & 0xff);
  out[3] = (unsigned char)((rows >> 8) & 0xff);
  out[4] = (unsigned char)(cx & 0xff);
  out[5] = (unsigned char)((cx >> 8) & 0xff);
  out[6] = (unsigned char)(cy & 0xff);
  out[7] = (unsigned char)((cy >> 8) & 0xff);
  out[8] = visible ? 1 : 0;
  return 9;
}

static int ghostty_job_copy_title(struct unixjob *job, unsigned char *out, int cap) {
  (void)job;
  (void)out;
  (void)cap;
  return 0;
}

static int ghostty_mag_key(int key_id, GhosttyKey *out_key) {
  if (out_key == NULL) return 0;
  switch (key_id) {
    case 1: *out_key = GHOSTTY_KEY_ARROW_UP; return 1;
    case 2: *out_key = GHOSTTY_KEY_ARROW_DOWN; return 1;
    case 3: *out_key = GHOSTTY_KEY_ARROW_RIGHT; return 1;
    case 4: *out_key = GHOSTTY_KEY_ARROW_LEFT; return 1;
    case 5: *out_key = GHOSTTY_KEY_HOME; return 1;
    case 6: *out_key = GHOSTTY_KEY_END; return 1;
    case 7: *out_key = GHOSTTY_KEY_PAGE_UP; return 1;
    case 8: *out_key = GHOSTTY_KEY_PAGE_DOWN; return 1;
    case 9: *out_key = GHOSTTY_KEY_DELETE; return 1;
    case 10: *out_key = GHOSTTY_KEY_INSERT; return 1;
    case 11: *out_key = GHOSTTY_KEY_BACKSPACE; return 1;
    case 12: *out_key = GHOSTTY_KEY_ENTER; return 1;
    case 13: *out_key = GHOSTTY_KEY_ESCAPE; return 1;
    case 14: *out_key = GHOSTTY_KEY_TAB; return 1;
    case 21: *out_key = GHOSTTY_KEY_F1; return 1;
    case 22: *out_key = GHOSTTY_KEY_F2; return 1;
    case 23: *out_key = GHOSTTY_KEY_F3; return 1;
    case 24: *out_key = GHOSTTY_KEY_F4; return 1;
    case 25: *out_key = GHOSTTY_KEY_F5; return 1;
    case 26: *out_key = GHOSTTY_KEY_F6; return 1;
    case 27: *out_key = GHOSTTY_KEY_F7; return 1;
    case 28: *out_key = GHOSTTY_KEY_F8; return 1;
    case 29: *out_key = GHOSTTY_KEY_F9; return 1;
    case 30: *out_key = GHOSTTY_KEY_F10; return 1;
    case 31: *out_key = GHOSTTY_KEY_F11; return 1;
    case 32: *out_key = GHOSTTY_KEY_F12; return 1;
    default: return 0;
  }
}

static int ghostty_utf8_encode(uint32_t codepoint, char out[4]) {
  if (codepoint == 0) return 0;
  if (codepoint <= 0x7f) {
    out[0] = (char)codepoint;
    return 1;
  }
  if (codepoint <= 0x7ff) {
    out[0] = (char)(0xc0 | (codepoint >> 6));
    out[1] = (char)(0x80 | (codepoint & 0x3f));
    return 2;
  }
  if (codepoint <= 0xffff) {
    out[0] = (char)(0xe0 | (codepoint >> 12));
    out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
    out[2] = (char)(0x80 | (codepoint & 0x3f));
    return 3;
  }
  if (codepoint <= 0x10ffff) {
    out[0] = (char)(0xf0 | (codepoint >> 18));
    out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
    out[3] = (char)(0x80 | (codepoint & 0x3f));
    return 4;
  }
  return 0;
}

static const char *ghostty_mag_direct_csi_key(int key_id) {
  switch (key_id) {
    case 1: return "\033[A";
    case 2: return "\033[B";
    case 3: return "\033[C";
    case 4: return "\033[D";
    default: return NULL;
  }
}

static int unix_mag_key_encode_status(unsigned char *out, int cap) {
  static const int key_ids[] = {1, 2, 3, 4};
  static const char *names[] = {"up", "down", "right", "left"};
  int used = 0;

  if (out == NULL || cap <= 0) return -1;

  used += snprintf((char *)out + used, (size_t)(cap - used),
                   "mag-key-encode\n");
  for (int i = 0; i < 4 && used < cap; i++) {
    const char *seq = ghostty_mag_direct_csi_key(key_ids[i]);
    if (seq == NULL) continue;
    used += snprintf((char *)out + used, (size_t)(cap - used),
                     "key-id=%d name=%s bytes=%02X %02X %02X final=%c source=direct-csi\n",
                     key_ids[i], names[i],
                     (unsigned char)seq[0], (unsigned char)seq[1],
                     (unsigned char)seq[2], seq[2]);
  }

  return used < cap ? used : cap - 1;
}

static int ghostty_job_send_key(struct unixjob *job, int key_id, int mods, uint32_t codepoint) {
  GhosttyKey key;
  GhosttyKeyEvent event = NULL;
  char utf8[4];
  int utf8_len;
  char out[128];
  size_t written = 0;
  GhosttyResult result;

  if (job == NULL || job->ghostty_terminal == NULL || job->ghostty_key_encoder == NULL) return -1;

  if (mods == 0 && codepoint == 0) {
    const char *seq = ghostty_mag_direct_csi_key(key_id);
    if (seq != NULL) return (int)ghostty_write_all((int)(job - UJ), seq, 3);
  }

  if (!ghostty_mag_key(key_id, &key)) return -1;
  if (ghostty_key_event_new(NULL, &event) != GHOSTTY_SUCCESS) return -1;

  ghostty_key_encoder_setopt_from_terminal(job->ghostty_key_encoder, job->ghostty_terminal);
  ghostty_key_event_set_action(event, GHOSTTY_KEY_ACTION_PRESS);
  ghostty_key_event_set_key(event, key);
  ghostty_key_event_set_mods(event, (GhosttyMods)mods);
  utf8_len = ghostty_utf8_encode(codepoint, utf8);
  if (utf8_len > 0) {
    ghostty_key_event_set_utf8(event, utf8, (size_t)utf8_len);
    ghostty_key_event_set_unshifted_codepoint(event, codepoint);
  }

  result = ghostty_key_encoder_encode(job->ghostty_key_encoder, event, out, sizeof(out), &written);
  ghostty_key_event_free(event);
  if (result != GHOSTTY_SUCCESS) return -1;
  if (written == 0) return 0;
  return (int)ghostty_write_all((int)(job - UJ), out, written);
}

static int ghostty_mag_mouse_action(int action_id, GhosttyMouseAction *out_action) {
  if (out_action == NULL) return 0;
  switch (action_id) {
    case 0: *out_action = GHOSTTY_MOUSE_ACTION_PRESS; return 1;
    case 1: *out_action = GHOSTTY_MOUSE_ACTION_RELEASE; return 1;
    case 2: *out_action = GHOSTTY_MOUSE_ACTION_MOTION; return 1;
    default: return 0;
  }
}

static int ghostty_mag_mouse_button(int button_id, GhosttyMouseButton *out_button) {
  if (out_button == NULL) return 0;
  switch (button_id) {
    case 1: *out_button = GHOSTTY_MOUSE_BUTTON_LEFT; return 1;
    case 2: *out_button = GHOSTTY_MOUSE_BUTTON_RIGHT; return 1;
    case 3: *out_button = GHOSTTY_MOUSE_BUTTON_MIDDLE; return 1;
    case 4: *out_button = GHOSTTY_MOUSE_BUTTON_FOUR; return 1;
    case 5: *out_button = GHOSTTY_MOUSE_BUTTON_FIVE; return 1;
    default: return 0;
  }
}

static int ghostty_job_send_mouse(struct unixjob *job, int action_id, int button_id,
                                  int col, int row, int mods) {
  GhosttyMouseAction action;
  GhosttyMouseButton button;
  GhosttyMouseEvent event = NULL;
  GhosttyMouseEncoderSize size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
  GhosttyMousePosition position;
  bool any_button_pressed;
  uint16_t cols = 0, rows = 0;
  char out[128];
  size_t written = 0;
  GhosttyResult result;

  if (job == NULL || job->ghostty_terminal == NULL || job->ghostty_mouse_encoder == NULL)
    return -1;
  if (!ghostty_mag_mouse_action(action_id, &action)) return -1;
  if (!ghostty_mag_mouse_button(button_id, &button)) return -1;
  if (col < 0 || row < 0) return -1;
  if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_COLS, &cols) != GHOSTTY_SUCCESS)
    return -1;
  if (ghostty_terminal_get(job->ghostty_terminal, GHOSTTY_TERMINAL_DATA_ROWS, &rows) != GHOSTTY_SUCCESS)
    return -1;
  if (cols == 0 || rows == 0) return -1;
  if (col >= cols) col = cols - 1;
  if (row >= rows) row = rows - 1;

  if (ghostty_mouse_event_new(NULL, &event) != GHOSTTY_SUCCESS) return -1;

  ghostty_mouse_encoder_setopt_from_terminal(job->ghostty_mouse_encoder, job->ghostty_terminal);
  size.screen_width = cols;
  size.screen_height = rows;
  size.cell_width = 1;
  size.cell_height = 1;
  ghostty_mouse_encoder_setopt(job->ghostty_mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);

  any_button_pressed = action != GHOSTTY_MOUSE_ACTION_RELEASE;
  ghostty_mouse_encoder_setopt(job->ghostty_mouse_encoder,
                               GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
                               &any_button_pressed);
  ghostty_mouse_event_set_action(event, action);
  ghostty_mouse_event_set_button(event, button);
  ghostty_mouse_event_set_mods(event, (GhosttyMods)mods);
  position.x = (float)col;
  position.y = (float)row;
  ghostty_mouse_event_set_position(event, position);

  result = ghostty_mouse_encoder_encode(job->ghostty_mouse_encoder, event,
                                        out, sizeof(out), &written);
  ghostty_mouse_event_free(event);
  if (result != GHOSTTY_SUCCESS) return -1;
  if (written == 0) return 0;
  return (int)ghostty_write_all((int)(job - UJ), out, written);
}

static int ghostty_job_paste_buffer(struct unixjob *job, const unsigned char *buf, int len) {
  char *data = NULL;
  char *out = NULL;
  size_t out_cap, written = 0;
  bool bracketed = false;
  GhosttyResult result;
  ssize_t n;

  if (job == NULL || job->ghostty_terminal == NULL || buf == NULL || len < 0) return -1;
  if (len == 0) return 0;

  data = (char *)malloc((size_t)len);
  if (data == NULL) return -1;
  memcpy(data, buf, (size_t)len);

  (void)ghostty_terminal_mode_get(job->ghostty_terminal,
                                  GHOSTTY_MODE_BRACKETED_PASTE,
                                  &bracketed);

  out_cap = (size_t)len + 64;
  out = (char *)malloc(out_cap);
  if (out == NULL) {
    free(data);
    return -1;
  }

  result = ghostty_paste_encode(data, (size_t)len, bracketed, out, out_cap, &written);
  if (result == GHOSTTY_OUT_OF_SPACE) {
    char *larger = (char *)realloc(out, written);
    if (larger == NULL) {
      free(out);
      free(data);
      return -1;
    }
    out = larger;
    out_cap = written;
    memcpy(data, buf, (size_t)len);
    result = ghostty_paste_encode(data, (size_t)len, bracketed, out, out_cap, &written);
  }

  if (result != GHOSTTY_SUCCESS) {
    free(out);
    free(data);
    return -1;
  }

  n = ghostty_write_all((int)(job - UJ), out, written);
  free(out);
  free(data);
  return n >= 0 ? (int)n : -1;
}
#endif

/************************************************************************/
/*									*/
/*		f i n d _ p r o c e s s _ s l o t			*/
/*									*/
/*	Find the slot in UJ with process id 'pid'.		        */
/*	Returns the slot #, or -1 if pid isn't found                    */
/*									*/
/*									*/
/************************************************************************/

int find_process_slot(int pid)
/* Find a slot with the specified pid */

{
  for (int slot = 0; slot < NPROCS; slot++)
    if (UJ[slot].PID == pid) {
      DBPRINT(("find_process_slot = %d.\n", slot));
      return slot;
    }
  return -1;
}

/************************************************************************/
/*									*/
/*		w a i t _ f o r _ c o m m _ p r o c e s s e s		*/
/*									*/
/*									*/
/*									*/
/*									*/
/************************************************************************/

void wait_for_comm_processes(void) {
  int pid;
  int slot;
  unsigned char d[6];

  memset(d, 0, sizeof(d));
  d[0] = 'W';
  write(UnixPipeOut, d, 6);
  SAFEREAD(UnixPipeIn, d, 6);

  pid = (d[0] << 8) | d[1] | (d[4] << 16) | (d[5] << 24);
  while (pid != 0) {
    slot = find_process_slot(pid);
    /* Ignore processes that we didn't start (shouldn't happen but
       occasionally does) */
    if (slot >= 0) {
      if (d[2] == 0) {
        DBPRINT(("Process %d exited status %d\n", pid, d[3]));
        UJ[slot].status = d[3];
      } else {
        DBPRINT(("Process %d terminated with signal %d\n", pid, d[2]));
        UJ[slot].status = (d[2] << 8);
      }
    }
    /* Look for another stopped process. */
    memset(d, 0, sizeof(d));
    d[0] = 'W';
    write(UnixPipeOut, d, 6);
    SAFEREAD(UnixPipeIn, d, 6);

    pid = (d[0] << 8) | d[1] | (d[4] << 16) | (d[5] << 24);
  }
}

/************************************************************************/
/*									*/
/*		b u i l d _ s o c k e t _ p a t h n a m e               */
/*									*/
/*	Returns a string which is the pathname associated with a        */
/*       socket descriptor.  Has ONE string buffer.                     */
/************************************************************************/
char *build_socket_pathname(int desc) {
  static char PathName[50];

  sprintf(PathName, "/tmp/LPU%ld-%d", StartTime, desc);
  return (PathName);
}

/************************************************************************/
/*									*/
/*		c l o s e _ u n i x _ d e s c r i p t o r s             */
/*									*/
/*	Kill off forked PTY-shells and forked-command processes		*/
/*	Also close sockets						*/
/*									*/
/************************************************************************/

void close_unix_descriptors(void) /* Get ready to shut Maiko down */
{
  for (int slot = 0; slot < NPROCS; slot++) {
    /* If this slot has an active job */
    switch (UJ[slot].type) {
      case UJUNUSED:
        break;
      case UJSHELL:
        if (kill(UJ[slot].PID, SIGKILL) < 0) perror("Killing shell");
        UJ[slot].PID = 0;
        DBPRINT(("Kill 5 closing shell desc %d.\n", slot));
        close(slot);
        break;

      case UJPROCESS:
        if (kill(UJ[slot].PID, SIGKILL) < 0) perror("Killing process");
        UJ[slot].PID = 0;
        DBPRINT(("Kill 5 closing process desc %d.\n", slot));
        close(slot);
        break;

      case UJSOCKET:
        close(slot);
        if (UJ[slot].pathname != NULL) {
          /* socket created directly from Lisp; pathname is in .pathname */
          DBPRINT(("Closing socket %d bound to %s\n", slot, UJ[slot].pathname));
          unlink(UJ[slot].pathname);
          free(UJ[slot].pathname);
          UJ[slot].pathname = NULL;
        }
        break;

      case UJSOSTREAM: close(slot); break;
    }
#ifdef MAIKO_ENABLE_GHOSTTY_VT
    ghostty_job_cleanup(&UJ[slot]);
#endif
    UJ[slot].type = UJUNUSED;
  }

  /* make sure everyone's really dead before proceeding */
  wait_for_comm_processes();
}

/************************************************************************/
/*								        */
/*			F i n d U n i x P i p e s		        */
/*								        */
/*   Find the file descriptors of the UnixPipe{In,Out} pipes	        */
/*    and a few other important numbers that were set originally        */
/*    before the unixcomm process was forked off; it stuck them in the  */
/*    environment so we could find them after the original lde process  */
/*    got overlaid with the real emulator			        */
/*                                                                      */
/************************************************************************/

int FindUnixPipes(void) {
  char *envtmp;
  int inttmp;
  struct unixjob cleareduj;

  DBPRINT(("Entering FindUnixPipes\n"));
  UnixPipeIn = UnixPipeOut = StartTime = UnixPID = -1;
  if ((envtmp = getenv("LDEPIPEIN"))) {
    errno = 0;
    inttmp = (int)strtol(envtmp, (char **)NULL, 10);
    if (errno == 0)
      UnixPipeIn = inttmp;
  }
  if ((envtmp = getenv("LDEPIPEOUT"))) {
    errno = 0;
    inttmp = (int)strtol(envtmp, (char **)NULL, 10);
    if (errno == 0)
      UnixPipeOut = inttmp;
  }
  if ((envtmp = getenv("LDESTARTTIME"))) {
    errno = 0;
    inttmp = (int)strtol(envtmp, (char **)NULL, 10);
    if (errno == 0)
      StartTime = inttmp;
  }
  if ((envtmp = getenv("LDEUNIXPID"))) {
    errno = 0;
    inttmp = (int)strtol(envtmp, (char **)NULL, 10);
    if (errno == 0)
      UnixPID = inttmp;
  }

/* This is a good place to initialize stuff like the UJ table */
  NPROCS = sysconf(_SC_OPEN_MAX);

  UJ = (struct unixjob *)malloc(NPROCS * sizeof(struct unixjob));
  unixjob_init_slot(&cleareduj, UJUNUSED);
  cleareduj.PID = 0;
  for (int i = 0; i < NPROCS; i++) UJ[i] = cleareduj;

  DBPRINT(("NPROCS is %d; leaving FindUnixPipes\n", NPROCS));
  return (UnixPipeIn == -1 || UnixPipeOut == -1 || StartTime == -1 || UnixPID == -1);
}

/************************************************************************/
/*									*/
/*		    F i n d A v a i l a b l e P t y			*/
/*									*/
/*	Fill string Slave with the path name to the slave		*/
/*	pseudo-terminal.						*/
/*									*/
/*	Return the fd for the master psuedo-terminal.			*/
/*									*/
/*	This uses POSIX pseudoterminals.				*/
/*									*/
/************************************************************************/

static int FindAvailablePty(char *Slave, size_t SlaveLen) {
  int res;

  res = posix_openpt(O_RDWR);
  if (res < 0) {
    perror("open_pt failed");
    return (-1);
  }
  grantpt(res);
  unlockpt(res);
  strlcpy(Slave, ptsname(res), SlaveLen);
  DBPRINT(("slave pty name is %s.\n", Slave));

  if (res != -1) {
    fcntl(res, F_SETFL, fcntl(res, F_GETFL, 0) | O_NONBLOCK);
    return (res);
  }
  return (-1);
}

/************************************************************************/
/*                                                                      */
/*  U n i x _ h a n d l e c o m m                                       */
/*                                                                      */
/*	LISP subr to talk to the forked "Unix process".                     */
/*                                                                      */
/*	The first argument (Arg[0]) is the command number.                  */
/*	Second argument (Arg[1]) is the Job # (except as indicated).        */
/*                                                                      */
/*	Commands are:                                                       */
/*                                                                      */
/*		0 Fork Pipe, Arg1 is a string for system();                     */
/*		     => Job # or NIL                                            */
/*		1 Write Byte, Arg2 is Byte;                                     */
/*		     => 1 (success), NIL (fail)                                 */
/*		2 Read Byte => Byte, NIL (no data), or T (EOF)                  */
/*		3 Kill Job => Status or T                                       */
/*		4 Fork PTY to Shell (no args) => Job # or NIL                   */
/*		5 Kill All (no args) => T                                       */
/*		6 Close (EOF)                                                   */
/*		7 Job status => T or status                                     */
/*		8 => the largest supported command #                            */
/*		9 Read Buffer, Arg1 = vmempage (512 byte buffer)                */
/*		     => byte count (<= 512), NIL (no data), or T (EOF)          */
/*	   10 Set Window Size, Arg2 = rows, Arg3 = columns                  */
/*	   11 Fork PTY to Shell (obsoletes command 4)                       */
/*        Arg1 = termtype, Arg2 = shell command string                    */
/*		     => Job # or NIL                                            */
/*     12 Create Unix Socket                                            */
/*        Arg1 = pathname to bind socket to (string)                    */
/*           => Socket # or NIL                                         */
/*     13 Try to accept on unix socket                                  */
/*           => Accepted socket #, NIL (fail) or T (try again)          */
/*     14 Query job type                                                */
/*           => type number or NIL if not a job                         */
/*     15 Write Buffer, Arg1 = Job #, Arg2 = vmempage,                  */
/*           Arg3 = # of bytes to write from buffer                     */
/*           => # of bytes written or NIL (failed)                      */
/*     16 Ghostty VT available => T or NIL                              */
/*     17 Ghostty VT resize, Arg1 = Job #, Arg2 = rows, Arg3 = cols     */
/*           => T or NIL                                                */
/*     18 Ghostty VT update render state, Arg1 = Job #                  */
/*           => dirty state number or NIL                               */
/*     19 Ghostty VT copy row, Arg1 = Job #, Arg2 = row, Arg3 = buffer  */
/*           => byte count or NIL                                       */
/*     20 Ghostty VT copy metadata, Arg1 = Job #, Arg2 = buffer         */
/*           => byte count or NIL                                       */
/*     21 Connect TCP stream, Arg1 = host string, Arg2 = port           */
/*           => stream job # or NIL                                     */
/*     22 Ghostty VT copy styled row, Arg1 = Job #, Arg2 = row,         */
/*           Arg3 = buffer => cell count or NIL                         */
/*     23 Ghostty VT scroll viewport, Arg1 = Job #, Arg2 = kind,        */
/*           Arg3 = amount => T or NIL                                  */
/*     24 Ghostty VT copy title, Arg1 = Job #, Arg2 = buffer            */
/*           => byte count or NIL                                       */
/*     25 Ghostty VT encode special key and write to PTY,               */
/*           Arg1 = Job #, Arg2 = key id, Arg3 = mods, Arg4 = codepoint */
/*           => byte count or NIL                                       */
/*     26 Ghostty VT copy colored row, Arg1 = Job #, Arg2 = row,        */
/*           Arg3 = buffer => cell count or NIL                         */
/*     27 Ghostty VT copy compact BMP colored row, Arg1 = Job #,        */
/*           Arg2 = row, Arg3 = buffer => cell count or NIL             */
/*     28 Ghostty VT encode mouse event and write to PTY,               */
/*           Arg1 = Job #, Arg2 = action, Arg3 = button, Arg4 = col,    */
/*           Arg5 = row, Arg6 = mods => byte count or NIL               */
/*     29 Ghostty VT copy compact BMP colored row segment,              */
/*           Arg1 = Job #, Arg2 = row, Arg3 = start cell, Arg4 = buffer */
/*           => copied cell count or NIL                                */
/*     30 Ghostty VT encode paste buffer and write to PTY,              */
/*           Arg1 = Job #, Arg2 = buffer, Arg3 = byte count             */
/*           => byte count written or NIL                               */
/*     31 Ghostty VT copy compact BMP RGB row segment,                  */
/*           Arg1 = Job #, Arg2 = row, Arg3 = start cell, Arg4 = buffer */
/*           => copied cell count or NIL                                */
/*     32 Ghostty VT row dirty?, Arg1 = Job #, Arg2 = row => T or NIL   */
/*     33 Ghostty VT clear render dirty flags, Arg1 = Job # => T or NIL */
/*     34 Battery status, Arg1 = buffer => byte count or NIL            */
/*     35 Ghostty VT copy dirty row numbers, Arg1 = Job #, Arg2 = buf   */
/*           => row count or NIL                                        */
/*     36 Ghostty VT copy compact BMP plain row, Arg1 = Job #,          */
/*           Arg2 = row, Arg3 = buffer => cell count or NIL             */
/*     37 Ghostty VT copy changed row numbers by rendered row hash,      */
/*           Arg1 = Job #, Arg2 = buffer => row count or NIL             */
/*     38 Mag debug status, Arg1 = buffer => byte count or NIL           */
/*     39 Mag debug request read+consume, Arg1 = buffer                  */
/*           => byte count or NIL                                        */
/*     40 Mag per-job terminal status, Arg1 = Job #, Arg2 = buffer       */
/*           => byte count or NIL                                        */
/*     41 Mag runtime config status, Arg1 = buffer => byte count or NIL  */
/*     42 Mag reset Ghostty counters, Arg1 = Job # or -1 for all         */
/*           => reset job count or NIL                                    */
/*     43 Mag Gopher viewport step, Arg1 = top, Arg2 = selected,          */
/*           Arg3 = delta, Arg4 = count, Arg5 = visible, Arg6 = jump,     */
/*           Arg7 = buffer => byte count or NIL                           */
/*     44 Mag terminal key encode status, Arg1 = buffer                   */
/*           => byte count or NIL                                          */
/*     45 Mag Gopher native viewport self-test, Arg1 = buffer             */
/*           => byte count or NIL                                          */
/*     46 Mag bounded native job list, Arg1 = buffer                       */
/*           => byte count or NIL                                          */
/*     47 Mag Gopher item label, Arg1 = zero-based index, Arg2 = buffer    */
/*           => byte count or NIL                                          */
/*     48 Mag Gopher type tag, Arg1 = type byte, Arg2 = buffer             */
/*           => byte count or NIL                                          */
/*                                                                      */
/************************************************************************/

LispPTR Unix_handlecomm(LispPTR *args) {
  int command, dest, slot;
  unsigned char d[6];
  unsigned char ch;
  unsigned char buf[1];

  /* Get command */
  N_GETNUMBER(args[0], command, bad);
  DBPRINT(("\nUnix_handlecomm: command %d\n", command));

  switch (command) {
    case 0: /* Fork pipe process */
    {
      char *PipeName;
      int PipeFD, sockFD;

      /* First create the socket */
      struct sockaddr_un sock;
      sockFD = socket(AF_UNIX, SOCK_STREAM, 0);
      if (sockFD < 0) {
        perror("socket open");
        return (NIL);
      }

      /* then bind it to a canonical pathname */
      PipeName = build_socket_pathname(sockFD);
      memset(&sock, 0, sizeof(sock));
      sock.sun_family = AF_UNIX;
      strlcpy(sock.sun_path, PipeName, sizeof(sock.sun_path));
      if (bind(sockFD, (struct sockaddr *)&sock, sizeof(struct sockaddr_un)) < 0) {
        close(sockFD);
        perror("binding sockets");
        unlink(PipeName);
        return (NIL);
      }

      DBPRINT(("Socket %d bound to name %s.\n", sockFD, PipeName));

      if (listen(sockFD, 1) < 0) perror("Listen");

      memset(d, 0, sizeof(d));
      d[0] = 'F';
      d[3] = sockFD;
      write(UnixPipeOut, d, 6);
      WriteLispStringToPipe(args[1]);

      DBPRINT(("Sending cmd string: %s\n", shcom));

      /* Get status */
      SAFEREAD(UnixPipeIn, d, 6);

      /* If it worked, return job # */
      if (d[3] == 1) {
      case0_lp:
        TIMEOUT(PipeFD = accept(sockFD, NULL, NULL));
        if (PipeFD < 0) {
          if (errno == EINTR) goto case0_lp;
          perror("Accept.");
          close(sockFD);
          if (unlink(PipeName) < 0) perror("Unlink");
          return (NIL);
        }
        if (fcntl(PipeFD, F_SETFL, fcntl(PipeFD, F_GETFL, 0) | O_NONBLOCK) == -1) {
          perror("setting up fifo to nodelay");
          return (NIL);
        }
        unixjob_init_slot(&UJ[PipeFD], UJPROCESS);
        UJ[PipeFD].PID = (d[1] << 8) | d[2] | (d[4] << 16) | (d[5] << 24);
        close(sockFD);
        unlink(PipeName);
        DBPRINT(("New process: slot/PipeFD %d PID %d\n", PipeFD, UJ[PipeFD].PID));
        return (GetSmallp(PipeFD));
      } else {
        DBPRINT(("Fork request failed."));
        close(sockFD);
        unlink(PipeName);
        return (NIL);
      }
    }

    case 1: /* Write byte */
      /* Get job #, Byte */
      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], dest, bad);
      ch = dest; /* ch is a char */

      if (valid_slot(slot) && (UJ[slot].status == -1)) switch (UJ[slot].type) {
          case UJPROCESS:
          case UJSHELL:
          case UJSOSTREAM:
#ifdef MAIKO_ENABLE_GHOSTTY_VT
            dest = ghostty_write_all(slot, &ch, 1);
#else
            dest = write(slot, &ch, 1);
#endif
            if (dest == 1) {
              return (GetSmallp(1));
            } else {
              wait_for_comm_processes();
              return (NIL);
            }

	  case UJSOCKET:
	  case UJUNUSED:
	    return (NIL);
        }
      break;

    case 2: /* Read byte */
      /**********************************************************/
      /* 							    */
      /* NB that it is possible for the other end of the stream */
      /* to have terminated, and hence status != -1.	    */
      /* EVEN IF THERE ARE STILL CHARACTERS TO READ.	    */
      /* 							    */
      /**********************************************************/

      N_GETNUMBER(args[1], slot, bad); /* Get job # */

      if (!valid_slot(slot)) return (NIL); /* No fd open; punt the read */
      switch (UJ[slot].type) {
        case UJPROCESS:
        case UJSHELL:
        case UJSOSTREAM:
          TIMEOUT(dest = read(slot, buf, 1));
          if (dest > 0) {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
            if (UJ[slot].type == UJSHELL) ghostty_job_write(&UJ[slot], buf, dest);
#endif
            return (GetSmallp(buf[0]));
          }
          /* Something's amiss; check our process status */
          wait_for_comm_processes();
          if ((dest == 0) && (UJ[slot].type != UJSOSTREAM) &&
              (UJ[slot].status == -1)) { /* No available chars, but other guy still running */
            DBPRINT(("dest = 0, status still -1\n"));
            return (ATOM_T);
          }
          if ((UJ[slot].status == -1) &&
              ((errno == EWOULDBLOCK) ||
               (errno == EAGAIN))) { /* No available chars, but other guy still running */
            DBPRINT((" dest<0, EWOULDBLOCK\n"));
            return (ATOM_T);
          }
          /* At this point, we either got an I/O error, or there */
          /* were no chars available and the other end has terminated. */
          /* Either way, signal EOF. */
          DBPRINT(("Indicating EOF from PTY desc %d.\n", slot));
          return (NIL);

	case UJSOCKET:
	case UJUNUSED:
	    return (NIL);
      }

    case 3: /* Kill process */
            /* Maiko uses this as CLOSEF, so "process" is a misnomer */

      N_GETNUMBER(args[1], slot, bad);

      DBPRINT(("Terminating process in slot %d.\n", slot));
      if (!valid_slot(slot)) return (ATOM_T);
      /* in all cases we need to close() the file descriptor */
      if (slot == 0) DBPRINT(("ZERO SLOT\n"));
      close(slot);
      switch (UJ[slot].type) {
      case UJSHELL:
      case UJPROCESS:
        /* wait for up to 0.1s for it to exit on its own after the close() */
        for (int i = 0; i < 10; i++) {
          wait_for_comm_processes();
          if (UJ[slot].status != -1) break;
          usleep(10000);
        }
        /* check again before we terminate it */
        if (UJ[slot].status != -1) break;
        kill(UJ[slot].PID, SIGKILL);
        for (int i = 0; i < 10; i++) {
          /* Waiting for the process to exit is possibly risky.
             Sending SIGKILL is always supposed to kill
             a process, but on very rare occurrences this doesn't
             happen because of a Unix kernel bug, usually a user-
             written device driver which hasn't been fully
             debugged.  So we time it out just be safe. */
          wait_for_comm_processes();
          usleep(10000);
          if (UJ[slot].status != -1) break;
        }
        break;
      case UJSOCKET:
        if (UJ[slot].pathname) {
          DBPRINT(("Unlinking %s\n", UJ[slot].pathname));
          if (unlink(UJ[slot].pathname) < 0) perror("Kill 3 unlink");
          free(UJ[slot].pathname);
          UJ[slot].pathname = NULL;
        }
        break;
      case UJSOSTREAM:
      case UJUNUSED:
	break;
      }
      UJ[slot].type = UJUNUSED;
      UJ[slot].PID = 0;
      UJ[slot].pathname = NULL;
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      ghostty_job_cleanup(&UJ[slot]);
#endif

      /* If status available, return it, otherwise T */
      return (GetSmallp(UJ[slot].status));

    case 4:
    case 11: /* Fork PTY process */
    {
      char SlavePTY[32];
      int Master;
      unsigned short len;

      Master = FindAvailablePty(SlavePTY, sizeof(SlavePTY));
      DBPRINT(("Fork Shell; Master PTY = %d. Slave=%c%c.\n", Master, SlavePTY[0], SlavePTY[1]));
      if (Master < 0) {
        printf("Open of lisp side of PTY failed.\n");
        fflush(stdout);
        return (NIL);
      }

      d[0] = (command == 4) ? 'S' : 'P';
      d[1] = SlavePTY[0];
      d[2] = SlavePTY[1];
      d[3] = Master;
      d[4] = '\0';
      d[5] = '\0';
      write(UnixPipeOut, d, 6);

      len = strlen(SlavePTY) + 1;
      write(UnixPipeOut, &len, 2);
      write(UnixPipeOut, SlavePTY, len);

      if (command != 4) { /* New style has arg1 = termtype, arg2 = command */
        WriteLispStringToPipe(args[1]);
        WriteLispStringToPipe(args[2]);
      }

      /* Get status */
      SAFEREAD(UnixPipeIn, d, 6);

      /* If successful, return job # */
      DBPRINT(("Pipe/fork result = %d.\n", d[3]));
      if (d[3] == 1) {
        /* Set up the IO not to block */
        fcntl(Master, F_SETFL, fcntl(Master, F_GETFL, 0) | O_NONBLOCK);

        unixjob_init_slot(&UJ[Master], UJSHELL); /* so we can find them */
        UJ[Master].PID = (d[1] << 8) | d[2] | (d[4] << 16) | (d[5] << 24);
        printf("Shell job %d, PID = %d\n", Master, UJ[Master].PID);
#ifdef MAIKO_ENABLE_GHOSTTY_VT
        ghostty_job_init(&UJ[Master], 80, 24);
#endif
        DBPRINT(("Forked pty in slot %d.\n", Master));
        return (GetSmallp(Master));
      } else {
        printf("Fork failed.\n");
        fflush(stdout);
        printf("d = %d, %d, %d, %d, %d, %d\n", d[0], d[1], d[2], d[3], d[4], d[5]);
        close(Master);
        return (NIL);
      }
    }

    case 5: /* Kill all the subprocesses */ close_unix_descriptors(); return (ATOM_T);

    case 6: /* Kill this subprocess */
      memset(d, 0, sizeof(d));
      d[0] = 'C';

	      /* Get job # */
	      N_GETNUMBER(args[1], dest, bad);
	      if (!valid_slot(dest)) return (ATOM_T);
	      d[1] = dest;

      d[3] = 1;
      write(UnixPipeOut, d, 6);

      /* Get status */
      SAFEREAD(UnixPipeIn, d, 6);

      switch (UJ[dest].type) {
        case UJUNUSED:
          break;

        case UJSHELL:
          DBPRINT(("Kill 5 closing shell desc %d.\n", dest));
          close(dest);
          break;

        case UJPROCESS:
          DBPRINT(("Kill 5 closing process desc %d.\n", dest));
          close(dest);
          break;

        case UJSOCKET:
          /* close a socket; be sure and unlink the file handle */
          DBPRINT(("Kill 5 closing raw socket desc %d.\n", dest));
          close(dest);
          if (UJ[dest].pathname != NULL) {
            unlink(UJ[dest].pathname);
            free(UJ[dest].pathname);
            UJ[dest].pathname = NULL;
          } /* else return an error somehow... */
          break;

        case UJSOSTREAM:
          DBPRINT(("Kill 5 closing socket stream %d.\n", dest));
          close(dest);
          break;
      }

      UJ[dest].type = UJUNUSED;
      UJ[dest].PID = 0;
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      ghostty_job_cleanup(&UJ[dest]);
#endif
      return (ATOM_T);
    /* break; */

    case 7: /* Current job status */

	      N_GETNUMBER(args[1], slot, bad); /* Get job # */
	      if (!valid_slot(slot)) return (NIL);
	      wait_for_comm_processes();       /* Make sure we're up to date */

      if (UJ[slot].status == -1)
        return (ATOM_T);
      else
        return (GetSmallp(UJ[slot].status));

    case 8: /* Return largest supported command */
      return (GetSmallp(MAG_UNIX_HANDLECOMM_MAX));

    case 9: /* Read buffer */
      /**********************************************************/
      /* 							    */
      /* NB that it is possible for the other end of the stream */
      /* to have terminated, and hence ForkedStatus != -1.	    */
      /* EVEN IF THERE ARE STILL CHARACTERS TO READ.	    */
      /* 							    */
      /**********************************************************/

      {
        DLword *bufp;
        int terno; /* holds errno thru sys calls after I/O fails */
        int rawdest = 0;

        N_GETNUMBER(args[1], slot, bad);     /* Get job # */
        if (!valid_slot(slot)) return (NIL); /* No fd open; punt the read */

        bufp = (NativeAligned2FromLAddr(args[2])); /* User buffer */
        DBPRINT(("Read buffer slot %d, type is %d buffer LAddr 0x%x (native %p)\n", slot, UJ[slot].type, args[2], bufp));

        switch (UJ[slot].type) {
          case UJSHELL:
          case UJPROCESS:
          case UJSOSTREAM: rawdest = dest = read(slot, bufp, 512);
            if (rawdest > 0 && UJ[slot].type == UJSHELL) {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
              if (UJ[slot].ghostty_terminal != NULL) {
                ghostty_job_write(&UJ[slot], (unsigned char *)bufp, rawdest);
                dest = rawdest;
              } else {
                dest = filter_terminal_output(&UJ[slot], (unsigned char *)bufp, rawdest);
              }
#else
              dest = filter_terminal_output(&UJ[slot], (unsigned char *)bufp, rawdest);
#endif
            }
#ifdef BYTESWAP
            word_swap_page(bufp, 128);
#endif /* BYTESWAP */

            if (dest > 0) { /* Got characters.  If debugging, print len &c */
              /* printf("got %d chars\n", dest); */
              return (GetSmallp(dest));
		            }

            if (rawdest > 0 && UJ[slot].type == UJSHELL) return (ATOM_T);

		            /* Something's amiss; update process status */
	            DBPRINT(("Problem: Got status %d from read, errno %d.\n", dest, errno));
	            terno = errno;
	            wait_for_comm_processes(); /* make sure we're up to date */
            if ((UJ[slot].status == -1) &&
                (((dest == 0) && (UJ[slot].type != UJSOSTREAM)) ||
                 ((dest < 0) && ((errno == EINTR) || (errno == 0) ||
                                 (errno == EAGAIN) || (errno == EWOULDBLOCK)))))
              /* No available chars, but other guy still running */
              return (ATOM_T);

            /* At this point, we either got an I/O error, or there */
            /* were no chars available and the other end has terminated. */
            /* Either way, signal EOF. */
            DBPRINT(("read failed; dest = %d, errno = %d, status = %d\n", dest, terno,
                     UJ[slot].status));
            DBPRINT(("Indicating EOF from PTY desc %d.\n", slot));
            return (NIL);

          case UJSOCKET:
          case UJUNUSED:
            return (NIL);
        }
      }

    case 10: /* Change window */
    {
      int rows, cols, pgrp, pty;
      struct winsize w;

      /* Get job #, rows, columns */
      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], rows, bad);
      N_GETNUMBER(args[3], cols, bad);

      if (valid_slot(slot) && (UJ[slot].type == UJSHELL) && (UJ[slot].status == -1)) {
        w.ws_row = rows;
        w.ws_col = cols;
        w.ws_xpixel = 0; /* not used */
        w.ws_ypixel = 0;
        pty = slot;
        /* Change window size, then
           notify process group of the change */
        if ((ioctl(pty, TIOCSWINSZ, &w) >= 0) &&
            ((pgrp = tcgetpgrp(pty)) >= 0) &&
            (killpg(pgrp, SIGWINCH) >= 0))
          return (ATOM_T);
        return (GetSmallp(errno));
      }

      return (NIL);
    }

    case 12: /* create Unix socket */

    {
      int sockFD;
      struct sockaddr_un sock;
      size_t pathsize;

      /* First open the socket */
      sockFD = socket(AF_UNIX, SOCK_STREAM, 0);
      if (sockFD < 0) {
        perror("socket open");
        return (NIL);
      }
      unixjob_init_slot(&UJ[sockFD], UJSOCKET);
      /* Then get a process slot and blit the pathname of the
         socket into it */
      /* need to type-check the string here */
      LispStringToCString(args[1], shcom, 2048);
      pathsize = strlen(shcom) + 1;
      UJ[sockFD].pathname = malloc(pathsize);
      if (UJ[sockFD].pathname == NULL) {
        close(sockFD);
        unixjob_init_slot(&UJ[sockFD], UJUNUSED);
        return (NIL);
      }
      strlcpy(UJ[sockFD].pathname, shcom, pathsize);
      /* Then bind it to the pathname, and get it	listening properly */

      sock.sun_family = AF_UNIX;
      strlcpy(sock.sun_path, shcom, sizeof(sock.sun_path));
      if (bind(sockFD, (struct sockaddr *)&sock, sizeof(struct sockaddr_un)) < 0) {
        close(sockFD);
        free(UJ[sockFD].pathname);
        unixjob_init_slot(&UJ[sockFD], UJUNUSED);
        perror("binding Lisp sockets");
        return (NIL);
      }
      DBPRINT(("Socket %d bound to name %s.\n", sockFD, shcom));
      if (listen(sockFD, 1) < 0) perror("Listen");
      /* Set up the IO not to block */
      if (fcntl(sockFD, F_SETFL, fcntl(sockFD, F_GETFL, 0) | O_NONBLOCK) == -1) {
        close(sockFD);
        unlink(UJ[sockFD].pathname);
        free(UJ[sockFD].pathname);
        unixjob_init_slot(&UJ[sockFD], UJUNUSED);
        return (NIL);
      }

      return (GetSmallp(sockFD));
    }

    case 13: /* try to accept */
    {
      /* returns file descriptor if successful,
         NIL if no connection available or failure */
      int sockFD, newFD;
      fd_set readfds;
      struct timeval nowait;

      N_GETNUMBER(args[1], sockFD, bad);
      if (valid_slot(sockFD) && UJ[sockFD].type == UJSOCKET && UJ[sockFD].pathname != NULL) {
        FD_ZERO(&readfds);
        FD_SET(sockFD, &readfds);
        nowait.tv_sec = 0;
        nowait.tv_usec = 0;
        dest = select(sockFD + 1, &readfds, NULL, NULL, &nowait);
        if (dest < 0) {
          if (errno == EINTR) return (NIL);
          perror("Lisp socket accept select");
          return (NIL);
        }
        if (dest == 0 || !FD_ISSET(sockFD, &readfds)) return (NIL);

      case13_lp:
        newFD = accept(sockFD, NULL, NULL);
        if (newFD < 0)
          if (errno == EINTR)
            goto case13_lp;
          else if (errno == EWOULDBLOCK || errno == EAGAIN)
            return (NIL);
          else {
            perror("Lisp socket accept");
            return (NIL);
          }
        else {
          if (newFD >= NPROCS) {
            close(newFD);
            return (NIL);
          }
          if (fcntl(newFD, F_SETFL, fcntl(newFD, F_GETFL, 0) | O_NONBLOCK) == -1) {
            close(newFD);
            return (NIL);
          }
          unixjob_init_slot(&UJ[newFD], UJSOSTREAM);
          return (GetSmallp(newFD));
        }
      } else
        return (NIL);
    }

    case 14: /* return type of socket */
    {
      int streamFD;

      N_GETNUMBER(args[1], streamFD, bad);
      if (valid_slot(streamFD))
        return GetSmallp(UJ[streamFD].type);
      else
        return NIL;
    }

    case 15: /* Write buffer */
    {
      DLword *bufp;
      int i;
      N_GETNUMBER(args[1], slot, bad);              /* Get job # */
      bufp = (NativeAligned2FromLAddr(args[2])); /* User buffer */
      N_GETNUMBER(args[3], i, bad);                 /* # to write */
      if (!valid_slot(slot)) return (NIL);
      DBPRINT(("Write buffer, type is %d\n", UJ[slot].type));

      switch (UJ[slot].type) {
        case UJSHELL:
        case UJPROCESS:
        case UJSOSTREAM:
#ifdef BYTESWAP
          word_swap_page(bufp, (i + 3) >> 2);
#endif /* BYTESWAP */

#ifdef MAIKO_ENABLE_GHOSTTY_VT
          dest = ghostty_write_all(slot, bufp, i);
#else
          dest = write(slot, bufp, i);
#endif
#ifdef BYTESWAP
          word_swap_page(bufp, (i + 3) >> 2);
#endif /* BYTESWAP */

          if (dest > 0) return (GetSmallp(dest));
          /* Something's amiss; update process status */
          wait_for_comm_processes(); /* make sure we're up to date */
          if (((dest == 0) || (errno == EWOULDBLOCK)) && (UJ[slot].status == -1))
            /* No room to write, but other guy still running */
            return (ATOM_T);
          /* At this point, we either got an I/O error, or there */
          /* were no chars available and the other end has terminated. */
          /* Either way, signal EOF. */
          DBPRINT(("Indicating write failure from PTY desc %d.\n", slot));
          return (NIL);

        case UJUNUSED:
        case UJSOCKET:
          return (NIL);
      }
    }

    case 16: /* Ghostty VT available */
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      return (ATOM_T);
#else
      return (NIL);
#endif

    case 17: /* Ghostty VT resize */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      int rows, cols;
      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], rows, bad);
      N_GETNUMBER(args[3], cols, bad);

      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);
      if (UJ[slot].ghostty_terminal == NULL && !ghostty_job_init(&UJ[slot], (uint16_t)cols, (uint16_t)rows))
        return (NIL);
      return ghostty_job_resize(&UJ[slot], (uint16_t)rows, (uint16_t)cols) ? ATOM_T : NIL;
#else
      return (NIL);
#endif
    }

    case 18: /* Ghostty VT update render state */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      int dirty;
      N_GETNUMBER(args[1], slot, bad);

      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);
      dirty = ghostty_job_update(&UJ[slot]);
      return (dirty >= 0) ? GetSmallp(dirty) : NIL;
#else
      return (NIL);
#endif
    }

    case 19: /* Ghostty VT copy row text */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int row, n;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], row, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[3]);
      n = ghostty_job_copy_row(&UJ[slot], row, (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 20: /* Ghostty VT copy metadata */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int n;

      N_GETNUMBER(args[1], slot, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[2]);
      n = ghostty_job_copy_meta(&UJ[slot], (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 21: /* Connect TCP stream */
    {
      int port, fd;

      N_GETNUMBER(args[2], port, bad);
      LispStringToCString(args[1], shcom, sizeof(shcom));
      fd = tcp_connect_stream(shcom, port);
      return (fd >= 0) ? GetSmallp(fd) : NIL;
    }

    case 22: /* Ghostty VT copy styled row; pairs of byte, flags */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int row, n;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], row, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[3]);
      n = ghostty_job_copy_row_styled(&UJ[slot], row, (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 23: /* Ghostty VT scroll viewport: 0 top, 1 bottom, 2 delta */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      int kind, amount;
      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], kind, bad);
      N_GETNUMBER(args[3], amount, bad);

      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);
      return ghostty_job_scroll(&UJ[slot], kind, amount) ? ATOM_T : NIL;
#else
      return (NIL);
#endif
    }

    case 24: /* Ghostty VT copy terminal title */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int n;

      N_GETNUMBER(args[1], slot, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[2]);
      n = ghostty_job_copy_title(&UJ[slot], (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 25: /* Ghostty VT encode key and write to PTY */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      int key_id, mods, codepoint, n;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], key_id, bad);
      N_GETNUMBER(args[3], mods, bad);
      N_GETNUMBER(args[4], codepoint, bad);

      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);
      n = ghostty_job_send_key(&UJ[slot], key_id, mods, (uint32_t)codepoint);
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 26: /* Ghostty VT copy colored row; quads of byte, flags, fg, bg */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int row, n;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], row, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[3]);
      n = ghostty_job_copy_row_colored(&UJ[slot], row, (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 27: /* Ghostty VT copy compact BMP colored row; byte cells or 255, low, high, flags, fg, bg */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int row, n;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], row, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[3]);
      n = ghostty_job_copy_row_bmp_colored(&UJ[slot], row, (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 28: /* Ghostty VT encode mouse event and write to PTY */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      int action, button, col, row, mods, n;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], action, bad);
      N_GETNUMBER(args[3], button, bad);
      N_GETNUMBER(args[4], col, bad);
      N_GETNUMBER(args[5], row, bad);
      N_GETNUMBER(args[6], mods, bad);

      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);
      n = ghostty_job_send_mouse(&UJ[slot], action, button, col, row, mods);
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 29: /* Ghostty VT copy compact BMP colored row segment */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int row, start_cell, n;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], row, bad);
      N_GETNUMBER(args[3], start_cell, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[4]);
      n = ghostty_job_copy_row_bmp_colored_segment(&UJ[slot], row, start_cell,
                                                   (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 30: /* Ghostty VT encode paste buffer and write to PTY */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int len, n;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[3], len, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);
      if (len < 0 || len > 512) return (NIL);

      bufp = NativeAligned2FromLAddr(args[2]);
#ifdef BYTESWAP
      word_swap_page(bufp, (len + 3) >> 2);
#endif /* BYTESWAP */
      n = ghostty_job_paste_buffer(&UJ[slot], (const unsigned char *)bufp, len);
#ifdef BYTESWAP
      word_swap_page(bufp, (len + 3) >> 2);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 31: /* Ghostty VT copy compact BMP RGB row segment */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int row, start_cell, n;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], row, bad);
      N_GETNUMBER(args[3], start_cell, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[4]);
      n = ghostty_job_copy_row_bmp_rgb_segment(&UJ[slot], row, start_cell,
                                               (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 32: /* Ghostty VT row dirty? */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      int row;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], row, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);
      return ghostty_job_row_dirty(&UJ[slot], row) ? ATOM_T : NIL;
#else
      return (NIL);
#endif
    }

    case 33: /* Ghostty VT clear render dirty flags */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      N_GETNUMBER(args[1], slot, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);
      return ghostty_job_clear_dirty(&UJ[slot]) ? ATOM_T : NIL;
#else
      return (NIL);
#endif
    }

    case 34: /* Battery status */
    {
      DLword *bufp;
      int n;

      bufp = NativeAligned2FromLAddr(args[1]);
      n = unix_battery_status((unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 35: /* Ghostty VT copy dirty row numbers */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int n;

      N_GETNUMBER(args[1], slot, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[2]);
      n = ghostty_job_copy_dirty_rows(&UJ[slot], (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 36: /* Ghostty VT copy compact BMP plain row */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int row, n;

      N_GETNUMBER(args[1], slot, bad);
      N_GETNUMBER(args[2], row, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[3]);
      n = ghostty_job_copy_row_bmp_plain(&UJ[slot], row, (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 37: /* Ghostty VT copy changed row numbers by rendered row hash */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      DLword *bufp;
      int n;

      N_GETNUMBER(args[1], slot, bad);
      if (!valid_slot(slot) || UJ[slot].type != UJSHELL) return (NIL);

      bufp = NativeAligned2FromLAddr(args[2]);
      n = ghostty_job_copy_changed_rows(&UJ[slot], (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 38: /* Mag debug status */
    {
      DLword *bufp;
      int n;

      bufp = NativeAligned2FromLAddr(args[1]);
      n = unix_mag_debug_status((unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 39: /* Mag debug request file read-and-consume */
    {
      DLword *bufp;
      int n;

      bufp = NativeAligned2FromLAddr(args[1]);
      n = unix_mag_read_request((unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n > 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 40: /* Mag per-job debug status */
    {
      DLword *bufp;
      int n;

      N_GETNUMBER(args[1], slot, bad);
      bufp = NativeAligned2FromLAddr(args[2]);
      n = unix_mag_job_status(slot, (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 41: /* Mag runtime config status */
    {
      DLword *bufp;
      int n;

      bufp = NativeAligned2FromLAddr(args[1]);
      n = unix_mag_config_status((unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 42: /* Mag reset Ghostty counters */
    {
#ifdef MAIKO_ENABLE_GHOSTTY_VT
      int reset_count = 0;

      N_GETNUMBER(args[1], slot, bad);
      if (slot < 0) {
        if (UJ == NULL) return (NIL);
        for (int i = 0; i < NPROCS; i++) {
          if (UJ[i].type == UJSHELL && UJ[i].ghostty_terminal != NULL) {
            ghostty_job_reset_stats(&UJ[i]);
            reset_count++;
          }
        }
        return GetSmallp(reset_count);
      }

      if (!valid_slot(slot) || UJ[slot].type != UJSHELL ||
          UJ[slot].ghostty_terminal == NULL)
        return (NIL);
      ghostty_job_reset_stats(&UJ[slot]);
      return GetSmallp(1);
#else
      return (NIL);
#endif
    }

    case 43: /* Mag Gopher viewport step */
    {
      DLword *bufp;
      int top, selected, delta, count, visible, jump, n;

      N_GETNUMBER(args[1], top, bad);
      N_GETNUMBER(args[2], selected, bad);
      N_GETNUMBER(args[3], delta, bad);
      N_GETNUMBER(args[4], count, bad);
      N_GETNUMBER(args[5], visible, bad);
      N_GETNUMBER(args[6], jump, bad);
      bufp = NativeAligned2FromLAddr(args[7]);
      n = unix_mag_gopher_viewport(top, selected, delta, count, visible, jump,
                                   (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 44: /* Mag terminal key encode status */
    {
      DLword *bufp;
      int n;

      bufp = NativeAligned2FromLAddr(args[1]);
      n = unix_mag_key_encode_status((unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 45: /* Mag Gopher native viewport self-test */
    {
      DLword *bufp;
      int n;

      bufp = NativeAligned2FromLAddr(args[1]);
      n = unix_mag_gopher_viewport_status((unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 46: /* Mag bounded native job list */
    {
      DLword *bufp;
      int n;

      bufp = NativeAligned2FromLAddr(args[1]);
      n = unix_mag_jobs_status((unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 47: /* Mag Gopher item label */
    {
      DLword *bufp;
      int index, n;

      N_GETNUMBER(args[1], index, bad);
      bufp = NativeAligned2FromLAddr(args[2]);
      n = unix_mag_gopher_label(index, (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 48: /* Mag Gopher type tag */
    {
      DLword *bufp;
      int type, n;

      N_GETNUMBER(args[1], type, bad);
      bufp = NativeAligned2FromLAddr(args[2]);
      n = unix_mag_gopher_type_tag_copy(type, (unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    case 49: /* Mag runtime typeahead injection for MCP eval */
    {
#ifdef XWINDOW
      int n = mag_inject_typeahead_file(MAG_RUNTIME_TYPEAHEAD_PATH);
      if (n >= 0) unlink(MAG_RUNTIME_TYPEAHEAD_PATH);
      return (n >= 0) ? GetSmallp(n) : NIL;
#else
      return (NIL);
#endif
    }

    case 50: /* Mag GC table status */
    {
      DLword *bufp;
      int n;

      bufp = NativeAligned2FromLAddr(args[1]);
      n = unix_mag_gc_status((unsigned char *)bufp, 512);
#ifdef BYTESWAP
      word_swap_page(bufp, 128);
#endif /* BYTESWAP */
      return (n >= 0 && n < 512) ? GetSmallp(n) : NIL;
    }

    default: return (NIL);
  }

bad:
  DBPRINT(("Bad input value."));
  return (NIL);
}

/************************************************************************/
/*									*/
/*		W r i t e L i s p S t r i n g T o P i p e		*/
/*									*/
/*	Convert a lisp string to a C string (both format and byte-	*/
/*	order), write 2 bytes of length and the string			*/
/*									*/
/*									*/
/************************************************************************/

void WriteLispStringToPipe(LispPTR lispstr) {
  unsigned short len;
  LispStringToCString(lispstr, shcom, 2048);
  /* Write string length, then string */
  len = strlen(shcom) + 1;
  write(UnixPipeOut, &len, 2);
  write(UnixPipeOut, shcom, len);
}

#endif /* DOS */
