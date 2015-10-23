/* ps.c - show process list
 *
 * Copyright 2015 Rob Landley <rob@landley.net>
 *
 * See http://pubs.opengroup.org/onlinepubs/9699919799/utilities/ps.html
 * And http://kernel.org/doc/Documentation/filesystems/proc.txt Table 1-4
 * And linux kernel source fs/proc/array.c function do_task_stat()
 *
 * Deviations from posix: no -n because /proc/self/wchan exists.
 * Posix says default output should have field named "TTY" but if you "-o tty"
 * the same field should be called "TT" which is _INSANE_ and I'm not doing it.
 * Similarly -f is outputs UNAME but calls it UID (we call it UNAME).
 * It also says that -o "args" and "comm" should behave differently but use
 * the same title, which is not the same title as the default output. (No.)
 *
 * Posix defines -o ADDR as "The address of the process" but the process
 * start address is a constant on any elf system with mmu. The procps ADDR
 * field always prints "-" with an alignment of 1, which is why it has 11
 * characters left for "cmd" in in 80 column "ps -l" mode. On x86-64 you
 * need 12 chars, leaving nothing for cmd: I.E. posix 2008 ps -l mode can't
 * be sanely implemented on 64 bit Linux systems. In procps there's ps -y
 * which changes -l by removing the "F" column and swapping RSS for ADDR,
 * leaving 9 chars for cmd, so we're using that as our -l output.
 *
 *
 * TODO: ps aux (att & bsd style "ps -ax" vs "ps ax" behavior difference)
 * TODO: finalize F
 * TODO: -uUgG
 * TODO: -o maj_flt,min_flt,stat(<NLnl+),rss --sort -Z
 * TODO: way too many hardwired constants here, how can I generate them?
 * TODO: ADDR? In 2015? Posix is literally _decades_ behind the times.
 *
 * Design issue: the -o fields are an ordered array, and the order is
 * significant. The array index is used in strawberry->which (consumed
 * in do_ps()) and in the bitmasks enabling default fields in ps_main().

USE_PS(NEWTOY(ps, "aAdeflo*p*t*u*U*g*G*w[!ol][+Ae]", TOYFLAG_USR|TOYFLAG_BIN))

config PS
  bool "ps"
  default n
  help
    usage: ps [-Aadeflw] [-gG GROUP] [-o FIELD] [-p PID] [-t TTY] [-uU USER]

    List processes.

    Which processes to show (selections may be comma separated lists):

    -A	All processes
    -a	Processes with terminals that aren't session leaders
    -d	All processes that aren't session leaders
    -e	Same as -A
    -g  belonging to selected session leaders (not groups: posix says so)
    -G	belonging to selected real GROUP IDs
    -p	selected PIDs
    -t	attached to selected TTYs
    -u	owned by selected USERs
    -U	owned by selected real USERs
    -w	Wide output (don't truncate at terminal width)

    Which FIELDs to show. (Default = -o pid,tty,time,cmd)

    -f	Full listing (uid,pid,ppid,c,stime,tty,time,cmd)
    -l	Long listing (f,s,uid,pid,ppid,c,pri,ni,addr,sz,wchan,tty,time,cmd)
    -o	Output the listed FIELDs, each with optional :size and/or =title

    Available -o FIELDs: F S UID PID PPID PRI NI ADDR SZ WCHAN STIME TTY
    TIME CMD ETIME GROUP %CPU PGID RGROUP RUSER USER VSZ RSS UNAME GID STAT

    GROUP %CPU PGID RGROUP RUSER USER VSZ RSS UNAME GID STAT

      ADDR  Instruction pointer
      CMD   Command line
      ETIME Elapsed time since process start
      F     Process flags (PF_*) from linux source file include/sched.h
            (in octal rather than hex because posix)
      GID   Group id
      GROUP Group name
      NI    Niceness of process (lower niceness is higher priority)
      PID   Process id
      PPID  Parent process id
      PRI   Priority
      RSS   Resident Set Size (memory currently used)
      S     Process state:
            R (running) S (sleeping) D (disk sleep) T (stopped)  t (traced)
            Z (zombie)  X (dead)     x (dead)       K (wakekill) W (waking)
      STAT  Process state (S) plus:
            < high priority          N low priority L locked memory
            s session leader         + foreground   l multithreaded
      STIME Start time of process in hh:mm (size :19 shows yyyy-mm-dd hh:mm:ss)
      SZ    Memory Size (4k pages needed to completely swap out process)
      TTY   Controlling terminal
      UID   User id
      UNAME User name
      WCHAN What it's waiting for

    SZ is memory mapped while RSS is pages consumed. ADDR is an address,
    WCHAN is a name. S shows a single state letter, STAT adds substatus.

    Default output is -o PID,TTY,TIME,CMD
    With -f USER:8=UID,PID,PPID,C,STIME,TTY,TIME,CMD
    With -l F,S,UID,PID,PPID,C,PRI,NI,ADDR,SZ,WCHAN,TTY,TIME,CMD
*/

#define FOR_ps
#include "toys.h"

GLOBALS(
  struct arg_list *G;
  struct arg_list *g;
  struct arg_list *U;
  struct arg_list *u;
  struct arg_list *t;
  struct arg_list *p;
  struct arg_list *o;

  unsigned width;
  dev_t tty;
  void *fields;
  long pidlen, *pids, ttylen, *ttys;
  long long ticks;
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
  char *title;
  char forever[];
};

static time_t get_uptime(void)
{
  struct sysinfo si;

  sysinfo(&si);

  return si.uptime;
}

static int match_process(long long *slot)
{
  long l;

  // skip processes we don't care about.
  if (TT.pids || TT.ttys) {
    for (l=0; l<TT.pidlen; l++) if (TT.pids[l] == *slot) return 1;
    for (l=0; l<TT.ttylen; l++) if (TT.ttys[l] == slot[4]) return 1;
    return 0;
  } else {
    if ((toys.optflags&(FLAG_a|FLAG_d)) && getsid(*slot)==*slot) return 0;
    if ((toys.optflags&FLAG_a) && !slot[4]) return 0;
    if (!(toys.optflags&(FLAG_a|FLAG_d|FLAG_A|FLAG_e)) && TT.tty!=slot[4])
      return 0;
  }

  return 1;
}

// dirtree callback.
// toybuf used as: 1024 /proc/$PID/stat, 1024 slot[], 2048 /proc/$PID/cmdline
static int do_ps(struct dirtree *new)
{
  struct strawberry *field;
  long long *slot = (void *)(toybuf+1024), ll;
  char *name, *s, state;
  int nlen, i, fd, len, width = TT.width;

  if (!new->parent) return DIRTREE_RECURSE|DIRTREE_SHUTUP;
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

  // parse numeric fields (PID = 0, skip 2, then 4th field goes in slot[1])
  for (len = 1; len<100; len++)
    if (1>sscanf(s += i, " %lld%n", slot+len, &i)) break;

  // skip processes we don't care about.
  if (!match_process(slot)) return 0;

  // At this point 512 bytes at toybuf+512 are free (already parsed).
  // Start of toybuf still has name in it.

  // Loop through fields
  for (field = TT.fields; field; field = field->next) {
    char *out = toybuf+2048, *scratch = toybuf+512;

    // Default: unsupported (5 "C")
    sprintf(out, "-");

    // PID, PPID, PRI, NI, ADDR, SZ, RSS
    if (-1!=(i = stridx((char[]){3,4,6,7,8,9,24,0}, field->which))) {
      char *fmt = "%lld";

      ll = slot[((char[]){0,1,15,16,27,20,21})[i]];
      if (i==2) ll--;
      if (i==4) fmt = "%llx";
      else if (i==5) ll >>= 12;
      else if (i==6) ll <<= 2;
      sprintf(out, fmt, ll);
    // F (also assignment of i used by later tests)
    // Posix doesn't specify what flags should say. Man page says
    // 1 for PF_FORKNOEXEC and 4 for PF_SUPERPRIV from linux/sched.h
    } else if (!(i = field->which)) sprintf(out, "%llo", (slot[6]>>6)&5);
    // S
    else if (i==1) sprintf(out, "%c", state);
    // UID and USER
    else if (i==2 || i==22) {
      sprintf(out, "%d", new->st.st_uid);
      if (i==22) {
        struct passwd *pw = getpwuid(new->st.st_uid);

        if (pw) out = pw->pw_name;
      }
    // WCHAN
    } else if (i==10) {
      sprintf(scratch, "%lld/wchan", *slot);
      readfileat(dirtree_parentfd(new), scratch, out, 2047);

    // STIME
    } else if (i==11) {
      time_t t = time(0) - get_uptime() + slot[19]/sysconf(_SC_CLK_TCK);

      // Padding behavior's a bit odd: default field size is just hh:mm.
      // Increasing stime:size reveals more data at left until full
      // yyyy-mm-dd hh:mm revealed at :16, then adds :ss at end for :19. But
      // expanding last field just adds :ss.
      strftime(scratch, 512, "%F %T", localtime(&t));
      out = scratch+strlen(scratch)-3-abs(field->len);
      if (out<scratch) out = scratch;

    // TTY
    } else if (i==12) {

      // Can we readlink() our way to a name?
      for (i=0; i<3; i++) {
        struct stat st;

        sprintf(scratch, "%lld/fd/%i", *slot, i);
        fd = dirtree_parentfd(new);
        if (!fstatat(fd, scratch, &st, 0) && S_ISCHR(st.st_mode)
          && st.st_rdev == slot[4]
          && 0<(len = readlinkat(fd, scratch, out, 2047)))
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

    // TIME ELAPSED
    } else if (i==13 || i==16) {
      int unit = 60*60*24, j = sysconf(_SC_CLK_TCK);
      time_t seconds = (i==16) ? (get_uptime()*j)-slot[19] : slot[11]+slot[12];

      seconds /= j;
      for (s = 0, j = 0; j<4; j++) {
        // TIME has 3 required fields, ETIME has 2. (Posix!)
        if (!s && (seconds>unit || j == 1+(i==16))) s = out;
        if (s) {
          s += sprintf(s, j ? "%02ld": "%2ld", (long)(seconds/unit));
          if ((*s = "-::"[j])) s++;
        }
        seconds %= unit;
        unit /= j ? 60 : 24;
      }

    // COMMAND CMD
    // Command line limited to 2k displayable. We could dynamically malloc, but
    // it'd almost never get used, querying length of a proc file is awkward,
    // fixed buffer is nommu friendly... Wait for somebody to complain. :)
    } else if (i==14 || i==15) {
      int fd;

      len = 0;
      sprintf(out, "%lld/cmdline", *slot);
      fd = openat(dirtree_parentfd(new), out, O_RDONLY);
 
      if (fd != -1) {
        if (0<(len = read(fd, out, 2047))) {
          if (!out[len-1]) len--;
          else out[len] = 0;
          for (i = 0; i<len; i++) if (out[i] < ' ') out[i] = ' ';
        }
        close(fd);
      }

      if (len<1) sprintf(out, "[%.*s]", nlen, name);
    // GROUP GID
    } else if (i==17 || i==26) {
      sprintf(out, "%ld", (long)new->st.st_gid);
      if (i == 17) {
        struct group *gr = getgrgid(new->st.st_gid);
        if (gr) out = gr->gr_name;
      }
    // %CPU
    } else if (i==18) {
      ll = (get_uptime()*sysconf(_SC_CLK_TCK)-slot[19]);
      len = ((slot[11]+slot[12])*1000)/ll;
      sprintf(out, "%d.%d", len/10, len%10);
    }

    // Output the field, appropriately padded
    len = width - (field != TT.fields);
    if (!field->next && field->len<0) i = 0;
    else {
      i = len<abs(field->len) ? len : field->len;
      len = abs(i);
    }

    // TODO test utf8 fontmetrics
    width -= printf(" %*.*s" + (field == TT.fields), i, len, out);
    if (!width) break;
  }
  xputc('\n');

  return 0;
}

void ps_main(void)
{
  struct strawberry *field;
  // Octal output code followed by header name
  char widths[] = {1,1,5,5,5,2,3,3,4+sizeof(long),5,
                   6,5,8,8,27,27,11,8,
                   4,5,8,8,8,6,5},
       *typos[] = {
         "F", "S", "UID", "PID", "PPID", "C", "PRI", "NI", "ADDR", "SZ",
         "WCHAN", "STIME", "TTY", "TIME", "CMD", "COMMAND", "ELAPSED", "GROUP",
         "%CPU", "PGID", "RGROUP", "RUSER", "USER", "VSZ", "RSS", "UNAME",
         "GID", "STAT"
       };
  int i, fd = -1;

  TT.width = 99999;
  if (!FLAG_w) terminal_size(&TT.width, 0);

  // find controlling tty, falling back to /dev/tty if none
  for (i = fd = 0; i<4; i++) {
    struct stat st;

    if (i!=3 || -1 != (i = fd = open("/dev/tty", O_RDONLY))) {
      if (isatty(i) && !fstat(i, &st)) {
        TT.tty = st.st_rdev;
        break;
      }
    }
  }
  if (fd!=-1) close(fd);

  // pid list via -p
  if (toys.optflags&FLAG_p) {
    struct arg_list *pl;
    char *next, *end, *arg;
    int len;

    for (pl = TT.p; pl; pl = pl->next) {
      arg = pl->arg;
      while ((next = comma_iterate(&arg, &len))) {
        if (!(15&TT.pidlen))
          TT.pids = xrealloc(TT.pids, sizeof(long)*(TT.pidlen+16));
        if ((TT.pids[TT.pidlen++] = xstrtol(next, &end, 10))<1 || end!=next+len)
          perror_exit("-p '%s'@%ld", pl->arg, 1+end-pl->arg);
      }
    }
  }

  // tty list via -t
  if (toys.optflags&FLAG_t) {
    struct arg_list *tl;
    char *next, *arg, *ss;
    int len, pts;

    for (tl = TT.t; tl; tl = tl->next) {
      arg = tl->arg;
      while ((next = comma_iterate(&arg, &len))) {
        if (!(15&TT.ttylen))
          TT.ttys = xrealloc(TT.ttys, sizeof(long)*(TT.ttylen+16));

        // -t pts = 12,pts/12, tty = /dev/tty2,tty2,S0
        pts = 0;
        if (isdigit(*next)) pts++;
        else {
          if (strstart(&next, strcpy(toybuf, "/dev/"))) len -= 5;
          if (strstart(&next, "pts/")) {
            len -= 4;
            pts++;
          } else if (strstart(&next, "tty")) len -= 3;
        }
        if (len<256 && (!(ss = strchr(next, '/')) || ss-next>len))
        {
          struct stat st;

          ss = toybuf + sprintf(toybuf, "/dev/%s", pts ? "pts/" : "tty");
          memcpy(ss, next, len);
          ss[len] = 0;
          xstat(toybuf, &st);
          TT.ttys[TT.ttylen++] = st.st_rdev;
          continue;
        }
        perror_exit("-t '%s'@%ld", tl->arg, 1+next-tl->arg);
      }
    }
  }

  // Manual field selection via -o
  if (toys.optflags&FLAG_o) {
    struct arg_list *ol;
    int length;

    for (ol = TT.o; ol; ol = ol->next) {
      char *width, *type, *title, *end, *arg = ol->arg;

      // Set title, length of title, type, end of type, and display width
      while ((type = comma_iterate(&arg, &length))) {
        if ((end = strchr(type, '=')) && length>(end-type)) {
          title = end+1;
          length -= (end-type)+1;
        } else {
          end = type+length;
          title = 0;
        }

        // If changing display width, trim title at the :
        if ((width = strchr(type, ':')) && width<end) {
          if (!title) length = width-type;
        } else width = 0;

        // Allocate structure, copy title
        field = xzalloc(sizeof(struct strawberry)+(length+1)*!!title);
        if (title) {
          memcpy(field->title = field->forever, title, length);
          field->title[field->len = length] = 0;
        }

        if (width) {
          field->len = strtol(++width, &title, 10);
          if (!isdigit(*width) || title != end)
            error_exit("bad : in -o %s@%ld", ol->arg, title-ol->arg);
          end = --width;
        }

        // Find type (reuse width as temp because we're done with it)
        for (i = 0; i<ARRAY_LEN(typos); i++) {
          int j, k;
          char *s;

          field->which = i;
          for (j = 0; j<2; j++) {
            if (!j) s = typos[i];
            // posix requires alternate names for some fields
            else if (-1==(k = stridx((char []){7, 14, 15, 16, 18, 0}, i)))
              continue;
            else s = ((char *[]){"NICE", "ARGS", "COMM", "ETIME", "PCPU"})[k];

            if (!strncasecmp(type, s, end-type) && strlen(s)==end-type) break;
          }
          if (j!=2) break;
        }
        if (i==ARRAY_LEN(typos)) error_exit("bad -o %.*s", end-type, type);
        if (!field->title) field->title = typos[field->which];
        if (!field->len) field->len = widths[field->which];
        dlist_add_nomalloc((void *)&TT.fields, (void *)field);
      }
    }

  // Default fields (also with -f and -l)
  } else {
    unsigned short def = 0x7008;

    if (toys.optflags&FLAG_f) def = 0x783c;
    if (toys.optflags&FLAG_l) def = 0x77ff;

    // order of fields[] matches posix STDOUT section, so add enabled XSI
    // defaults according to bitmask
    for (i=0; def>>i; i++) {
      if (!((def>>i)&1)) continue;

      field = xmalloc(sizeof(struct strawberry)+strlen(typos[i])+1);
      field->which = i;
      field->len = widths[i];
      strcpy(field->title = field->forever, typos[i]);
      dlist_add_nomalloc((void *)&TT.fields, (void *)field);
    }
  }
  dlist_terminate(TT.fields);

  // Print padded headers. (Numbers are right justified, everyting else left.
  // time and pcpu count as numbers, tty does not)
  for (field = TT.fields; field; field = field->next) {

    // right justify F UID PID PPID PRI NI ADDR SZ TIME ELAPSED %CPU STIME
    if (!((1<<field->which)&0x527dd)) field->len *= -1;
    printf(" %*s" + (field == TT.fields), field->len, field->title);

    // -f prints USER but calls it UID (but "ps -o uid -f" is numeric...?)
    if ((toys.optflags&(FLAG_f|FLAG_o))==FLAG_f && field->which==2)
      field->which = 22;
  }
  xputc('\n');

  dirtree_read("/proc", do_ps);

  if (CFG_TOYBOX_FREE) {
    free(TT.pids);
    free(TT.ttys);
    llist_traverse(TT.fields, free);
  }
}
