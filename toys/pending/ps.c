/* ps.c - show process list
 *
 * Copyright 2015 Rob Landley <rob@landley.net>
 *
 * See http://pubs.opengroup.org/onlinepubs/9699919799/utilities/ps.html
 * And http://kernel.org/doc/Documentation/filesystems/proc.txt Table 1-4
 * And linux kernel source fs/proc/array.c function do_task_stat()
 *
 * Deviation from posix: no -n, "-o tty" called "TTY" not "TT", 

USE_PS(NEWTOY(ps, "aAdeflo*", TOYFLAG_USR|TOYFLAG_BIN))

config PS
  bool "ps"
  default n
  help
    usage: ps [-Aade] [-fl] [-gG GROUP] [-o FIELD] [-p PID] [-t TTY] [-u USER]

    List processes.

    -A	All processes
    -a	Processes with terminals, except session leaders
    -d	Processes that aren't session leaders
    -e	Same as -A
    -f	Full listing
    -l	Long listing

    -g  Processes belonging to these session leaders
    -G	Processes with these real group IDs
    -o	Show FIELDS for each process
    -p	select by PID
    -t	select by TTY
    -u	select by USER
    -U	select by USER

     GROUP, FIELD, PID, TTY, and USER are comma separated lists.

    OUTPUT (-o) FIELDS:

      S Linux defines the process state letters as:
        R (running) S (sleeping) D (disk sleep) T (stopped)  t (tracing stop)
        Z (zombie)  X (dead)     x (dead)       K (wakekill) W (waking)
      F Process flags (PF_*) from linux source file include/sched.h
        (in octal rather than hex because posix inexplicably says so)

    Default is PID,TTY,TIME,CMD  With -f UID,PID,PPID,C,STIME,TTY,TIME,CMD
    With -l F,S,UID,PID,PPID,C,PRI,NI,ADDR,SZ,WCHAN,TTY,TIME,CMD
*/

#define FOR_ps
#include "toys.h"

GLOBALS(
  struct arg_list *o;

  unsigned width;
  dev_t tty;
  void *fields;
  long uptime;
)

/*
  l l fl  a   fl   fl l  l  l    l  l     f     a   a    a
  F S UID PID PPID C PRI NI ADDR SZ WCHAN STIME TTY TIME CMD
  ruser user rgroup group pid ppid pgid pcpu vsz nice etime time tty comm args

  todo: thread support /proc/$d/task/%d/stat
    man page: F flags mean...
*/

struct strawberry {
  struct strawberry *next, *prev;
  short which, len;
  char title[];
};

// dirtree callback.
// toybuf used as: 1024 /proc/$PID/stat, 1024 slot[], 2048 /proc/$PID/cmdline
static int do_ps(struct dirtree *new)
{
  struct strawberry *field;
  long long *slot = (void *)(toybuf+1024);
  char *name, *s, state;
  int nlen, i, fd, len, width = TT.width;

  if (!new->parent) return DIRTREE_RECURSE;
  if (!(*slot = atol(new->name))) return 0;

  // name field limited to 256 bytes by VFS, plus 40 fields * max 20 chars:
  // 1000-ish total, but some forced zero so actually there's headroom.
  sprintf(toybuf, "%lld/stat", *slot);
  if (!readfileat(dirtree_parentfd(new), toybuf, toybuf, 1024)) return 0;

  // parse oddball fields (name and state)
  if (!(s = strchr(toybuf, '('))) return 0;
  for (name = ++s; *s != ')'; s++) if (!*s) return 0;
  nlen = s++-name;
  if (1>sscanf(++s, " %c%n", &state, &i)) return 0;

  // parse numeric fields
  for (len = 1; len<100; len++)
    if (1>sscanf(s += i, " %lld%n", slot+len, &i)) break;

  // skip entries we don't care about.
  if ((toys.optflags&(FLAG_a|FLAG_d)) && getsid(*slot)==*slot) return 0;
  if ((toys.optflags&FLAG_a) && !slot[4]) return 0;
  if (!(toys.optflags*(FLAG_a|FLAG_d|FLAG_A|FLAG_e)) && TT.tty!=slot[4])
    return 0;

  // 0 "F", "S", "UID", "PID", "PPID", "C", "PRI",
  // 7 "NI", "ADDR", "SZ", "WCHAN", "STIME", "TTY", "TIME", "CMD",
  //15 "COMMAND", "ELAPSED", "GROUP", "%CPU", "PGID", "RGROUP",
  //21 "RUSER", "USER", "VSZ"

  for (field = TT.fields; field; field = field->next) {
    char *out = toybuf+2048;

    // Default: unsupported (5 "C")
    sprintf(out, "-");

    // PID, PPID, PRI, NI, ADDR, SZ
    if (-1 != (i = stridx((char[]){3,4,6,7,8,9,0}, field->which)))
      sprintf(out, ((1<<i)&0x10) ? "%llx" : "%lld",
              slot[((char[]){0,2,16,17,22})[i]]>>(((1<<i)&0x20) ? 12 : 0));
    // F
    else if (!(i = field->which)) sprintf(out, "%llo", slot[7]);
    // S
    else if (i == 1)
      sprintf(out, "%c", state);
    // UID and USER
    else if (i == 2 || i == 22) {
      sprintf(out, "%d", new->st.st_uid);
      if (i == 2 || (toys.optflags&FLAG_f)) {
        struct passwd *pw = getpwuid(new->st.st_uid);

        if (pw) out = pw->pw_name;
      }
    // WCHAN
    } else if (i==10) {
      sprintf(toybuf+512, "%lld/wchan", *slot);
      readfileat(dirtree_parentfd(new), toybuf+512, out, 2047);

    // STIME
    // TTY
    } else if (i==12) {

      // Can we readlink() our way to a name?
      for (i=0; i<3; i++) {
        struct stat st;

        sprintf(toybuf+512, "%lld/fd/%i", *slot, i);
        fd = dirtree_parentfd(new);
        if (!fstatat(fd, toybuf+512, &st, 0) && S_ISCHR(st.st_mode)
          && st.st_rdev == slot[4]
          && 0<(len = readlinkat(fd, toybuf+512, out, 2047)))
        {
          out[len] = 0;
          if (!strncmp(out, "/dev/", 5)) out += 5;

          break;
        }
      }

      // Couldn't find it, show major:minor
      if (i==3) {
        i = slot[4];
        sprintf(out, "%d:%d", (i>>8)&0xfff, ((i>>12)&0xfff00)|(i&0xff));
      }

    // TIME
    } else if (i==13 || i==16) {
      long seconds = (i==16) ? slot[20] : slot[11]+slot[12], ll = 60*60*24;

      seconds /= sysconf(_SC_CLK_TCK);
      if (i==16) seconds = TT.uptime-seconds;
      for (s = out, i = 0; i<4; i++) {
        if (i>1 || seconds > ll)
          s += sprintf(s, (i==3) ? "%02ld" : "%ld%c", seconds/ll, "-::"[i]);
        seconds %= ll;
        ll /= i ? 60 : 24;
      }

  //16 "ELAPSED", "GROUP", "%CPU", "PGID", "RGROUP",
  //21 "RUSER", -, "VSZ"


    // Command line limited to 2k displayable. We could dynamically malloc, but
    // it'd almost never get used, querying length of a proc file is awkward,
    // fixed buffer is nommu friendly... Wait for somebody to complain. :)
    } else if (i == 14 || i == 15) {
      int fd;

      len = 0;
      sprintf(out, "%lld/cmdline", *slot);
      fd = openat(dirtree_parentfd(new), out, O_RDONLY);
 
      if (fd != -1) {
        if (0<(len = read(fd, out, 2047))) {
          out[len] = 0;
          for (i = 0; i<len; i++) if (out[i] < ' ') out[i] = ' ';
        }
        close(fd);
      }
      if (len<1) sprintf(out, "[%.*s]", nlen, name);
    }

    i = width<field->len ? width : field->len;
    width -= printf(" %*.*s", i, field->next ? i : width, out);
  }
  xputc('\n');

  return 0;
}

void ps_main(void)
{
  struct strawberry *field;
  // Octal output code followed by header name
  char *typos[] = {
    "F", "S", "UID", "PID", "PPID", "C", "PRI",
    "NI", "ADDR", "SZ", "WCHAN", "STIME", "TTY", "TIME", "CMD",
    "COMMAND", "ELAPSED", "GROUP", "%CPU", "PGID", "RGROUP",
    "RUSER", "USER", "VSZ"
  };
  int i, fd = -1;

  // l l fl  a   fl   fl l  l  l    l  l     f     a   a    a
  // F S UID PID PPID C PRI NI ADDR SZ WCHAN STIME TTY TIME CMD
  // 7 

  TT.width = 80;
  terminal_size(&TT.width, 0);
  TT.width--;

  // find controlling tty, falling back to /dev/tty if none
  for (i = fd = 0; i < 4; i++) {
    struct stat st;

    if (i != 3 || -1 != (i = fd = open("/dev/tty", O_RDONLY))) {
      if (isatty(i) && !fstat(i, &st)) {
        TT.tty = st.st_rdev;
        break;
      }
    }
  }
  if (fd != -1) close(fd);

  sysinfo((void *)toybuf);
  // Because "TT.uptime = *(long *)toybuf;" triggers a bug in gcc.
  {
    long *sigh = (long *)toybuf;
    TT.uptime = *sigh;
  }

  // Select fields
  if (toys.optflags&FLAG_o) {
    printf("todo\n");
  } else {
    unsigned short def = 0x7008;

    if (toys.optflags&FLAG_f) def = 0x783c;
    if (toys.optflags&FLAG_l) def = 0x77ff;

    // order of fields[] matches posix STDOUT section, so add enabled XSI
    // defaults according to bitmask

    for (i=0; def>>i; i++) {
      int len = strlen(typos[i]);

      if (!((def>>i)&1)) continue;

      field = xmalloc(sizeof(struct strawberry)+len+1);
      field->which = i;
      field->len = len;
      strcpy(field->title, typos[i]);
      dlist_add_nomalloc((void *)&TT.fields, (void *)field);
    }
  }
  dlist_terminate(TT.fields);

  for (field = TT.fields; field; field = field->next)
    printf(" %*s", field->len, field->title);
  xputc('\n');

  dirtree_read("/proc", do_ps);
}
