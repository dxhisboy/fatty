// child.c (part of FaTTY)
// Copyright 2015 Juho Peltonen
// Based on mintty code by Andy Koppe, Thomas Wolff
// Licensed under the terms of the GNU General Public License v3 or later.

#include "child.h"

#include "term.h"
#include "charset.h"

#include "winpriv.h"  /* win_prefix_title */

#include <pwd.h>
#include <fcntl.h>
#include <utmp.h>
#include <dirent.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#ifdef __CYGWIN__
#include <sys/cygwin.h>  // cygwin_internal
#endif

#if CYGWIN_VERSION_API_MINOR >= 93
#include <pty.h>
#else
int forkpty(int *, char *, struct termios *, struct winsize *);
#endif

#include <winbase.h>

#if CYGWIN_VERSION_DLL_MAJOR < 1007
#include <winnls.h>
#include <wincon.h>
#include <wingdi.h>
#include <winuser.h>
#endif

bool clone_size_token = true;

bool logging = false;

#if CYGWIN_VERSION_API_MINOR >= 66
#include <langinfo.h>
#endif

static void
childerror(struct term* term, char * action, bool from_fork, int errno_code, int code)
{
#if CYGWIN_VERSION_API_MINOR >= 66
  bool utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
  char * oldloc;
#else
  char * oldloc = (char *)cs_get_locale();
  bool utf8 = strstr(oldloc, ".65001");
#endif
  if (utf8)
    oldloc = null;
  else {
    oldloc = strdup(cs_get_locale());
    cs_set_locale("C.UTF-8");
  }

  char s[33];
  bool colour_code = code && !errno_code;
  sprintf(s, "\033[30;%dm\033[K", from_fork ? 41 : colour_code ? code : 43);
  term_write(term, s, strlen(s));
  term_write(term, action, strlen(action));
  if (errno_code) {
    char * err = strerror(errno_code);
    if (from_fork && errno_code == ENOENT)
      err = _("There are no available terminals");
    term_write(term, ": ", 2);
    term_write(term, err, strlen(err));
  }
  if (code && !colour_code) {
    sprintf(s, " (%d)", code);
    term_write(term, s, strlen(s));
  }
  term_write(term, ".\033[0m\r\n", 7);

  if (oldloc) {
    cs_set_locale(oldloc);
    free(oldloc);
  }
}

void
open_logfile(bool toggling)
{
  // Open log file if any
  if (*cfg.log) {
    // use cygwin conversion function to escape unencoded characters 
    // and thus avoid the locale trick (2.2.3)

    if (0 == wcscmp(cfg.log, W("-"))) {
      child_log_fd = fileno(stdout);
      logging = true;
    }
    else {
      char * log = path_win_w_to_posix(cfg.log);
#ifdef debug_logfilename
      printf("<%ls> -> <%s>\n", cfg.log, log);
#endif
      char * format = strchr(log, '%');
      if (format && * ++ format == 'd' && !strchr(format, '%')) {
        char * logf = newn(char, strlen(log) + 20);
        sprintf(logf, log, getpid());
        free(log);
        log = logf;
      }
      else if (format) {
        struct timeval now;
        gettimeofday(& now, 0);
        char * logf = newn(char, MAX_PATH + 1);
        strftime(logf, MAX_PATH, log, localtime (& now.tv_sec));
        free(log);
        log = logf;
      }

      child_log_fd = open(log, O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (child_log_fd < 0) {
        // report message and filename:
        wchar * wpath = path_posix_to_win_w(log);
        char * upath = cs__wcstoutf(wpath);
#ifdef debug_logfilename
        printf(" -> <%ls> -> <%s>\n", wpath, upath);
#endif
        char * msg = _("Error: Could not open log file");
        if (toggling) {
          char * err = strerror(errno);
          char * errmsg = newn(char, strlen(msg) + strlen(err) + strlen(upath) + 4);
          sprintf(errmsg, "%s: %s\n%s", msg, err, upath);
          win_show_warning(errmsg);
          free(errmsg);
        }
        else {
          childerror(win_active_terminal(), msg, false, errno, 0);
          childerror(win_active_terminal(), upath, false, 0, 0);
        }
        free(upath);
        free(wpath);
      }
      else
        logging = true;

      free(log);
    }
  }
}

void
toggle_logging()
{
  if (logging)
    logging = false;
  else if (child_log_fd >= 0)
    logging = true;
  else
    open_logfile(true);
}

void
child_update_charset(struct child * child)
{
#ifdef IUTF8
  if (child->pty_fd >= 0) {
    // Terminal line settings
    struct termios attr;
    tcgetattr(child->pty_fd, &attr);
    bool utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
    if (utf8)
      attr.c_iflag |= IUTF8;
    else
      attr.c_iflag &= ~IUTF8;
    tcsetattr(child->pty_fd, TCSANOW, &attr);
  }
#endif
}

void
child_create(struct child* child, struct term* term,
    char *argv[], struct winsize *winp, const char* path)
{
  int pid;

  child->dir = null;
  child->pty_fd = -1;
  child->term = term;

  string lang = cs_lang();

  // Create the child process and pseudo terminal.
  pid = forkpty(&child->pty_fd, 0, 0, winp);
  if (pid < 0) {
    bool rebase_prompt = (errno == EAGAIN);
    //ENOENT  There are no available terminals.
    //EAGAIN  Cannot allocate sufficient memory to allocate a task structure.
    //EAGAIN  Not possible to create a new process; RLIMIT_NPROC limit.
    //ENOMEM  Memory is tight.
    childerror(term, _("Error: Could not fork child process"), true, errno, pid);
    if (rebase_prompt)
      childerror(term, _("DLL rebasing may be required; see 'rebaseall / rebase --help'"), false, 0, 0);

    child->pid = pid = 0;

    term_hide_cursor(term);
  }
  else if (!pid) { // Child process.
#if CYGWIN_VERSION_DLL_MAJOR < 1007
    // Some native console programs require a console to be attached to the
    // process, otherwise they pop one up themselves, which is rather annoying.
    // Cygwin's exec function from 1.5 onwards automatically allocates a console
    // on an invisible window station if necessary. Unfortunately that trick no
    // longer works on Windows 7, which is why Cygwin 1.7 contains a new hack
    // for creating the invisible console.
    // On Cygwin versions before 1.5 and on Cygwin 1.5 running on Windows 7,
    // we need to create the invisible console ourselves. The hack here is not
    // as clever as Cygwin's, with the console briefly flashing up on startup,
    // but it'll do.
#if CYGWIN_VERSION_DLL_MAJOR == 1005
    DWORD win_version = GetVersion();
    win_version = ((win_version & 0xff) << 8) | ((win_version >> 8) & 0xff);
    if (win_version >= 0x0601)  // Windows 7 is NT 6.1.
#endif
      if (AllocConsole()) {
        HMODULE kernel = GetModuleHandleA("kernel32");
        HWND (WINAPI *pGetConsoleWindow)(void) =
          (void *)GetProcAddress(kernel, "GetConsoleWindow");
        ShowWindowAsync(pGetConsoleWindow(), SW_HIDE);
      }
#endif

    // Reset signals
    signal(SIGHUP, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGCHLD, SIG_DFL);

    // Mimick login's behavior by disabling the job control signals
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    setenv("TERM", cfg.term, true);

    if (lang) {
      unsetenv("LC_ALL");
      unsetenv("LC_COLLATE");
      unsetenv("LC_CTYPE");
      unsetenv("LC_MONETARY");
      unsetenv("LC_NUMERIC");
      unsetenv("LC_TIME");
      unsetenv("LC_MESSAGES");
      setenv("LANG", lang, true);
    }

    // Terminal line settings
    struct termios attr;
    tcgetattr(0, &attr);
    attr.c_cc[VERASE] = cfg.backspace_sends_bs ? CTRL('H') : CDEL;
    attr.c_iflag |= IXANY | IMAXBEL;
#ifdef IUTF8
    bool utf8 = strcmp(nl_langinfo(CODESET), "UTF-8") == 0;
    if (utf8)
      attr.c_iflag |= IUTF8;
    else
      attr.c_iflag &= ~IUTF8;
#endif
    attr.c_lflag |= ECHOE | ECHOK | ECHOCTL | ECHOKE;
    tcsetattr(0, TCSANOW, &attr);

    if (path)
      chdir(path);

    // Invoke command
    execvp(child->cmd, argv);

    // If we get here, exec failed.
    fprintf(stderr, "\033]701;C.UTF-8\007");
    fprintf(stderr, "\033[30;41m\033[K");
    //__ %1$s: client command (e.g. shell) to be run; %2$s: error message
    fprintf(stderr, _("Failed to run '%s': %s"), child->cmd, strerror(errno));
    fprintf(stderr, "\r\n");
    fflush(stderr);

#if CYGWIN_VERSION_DLL_MAJOR < 1005
    // Before Cygwin 1.5, the message above doesn't appear if we exit
    // immediately. So have a little nap first.
    usleep(200000);
#endif

    exit_fatty(255);
  }
  else { // Parent process.
    child->pid = pid;

    fcntl(child->pty_fd, F_SETFL, O_NONBLOCK);
    
    child_update_charset(child);

    if (cfg.create_utmp) {
      char *dev = ptsname(child->pty_fd);
      if (dev) {
        struct utmp ut;
        memset(&ut, 0, sizeof ut);

        if (!strncmp(dev, "/dev/", 5))
          dev += 5;
        strlcpy(ut.ut_line, dev, sizeof ut.ut_line);

        if (dev[1] == 't' && dev[2] == 'y')
          dev += 3;
        else if (!strncmp(dev, "pts/", 4))
          dev += 4;
        strncpy(ut.ut_id, dev, sizeof ut.ut_id);

        ut.ut_type = USER_PROCESS;
        ut.ut_pid = pid;
        ut.ut_time = time(0);
        strlcpy(ut.ut_user, getlogin() ?: "?", sizeof ut.ut_user);
        gethostname(ut.ut_host, sizeof ut.ut_host);
        login(&ut);
      }
    }
  }
}

void
child_free(struct child* child)
{
  if (child->pty_fd >= 0)
    close(child->pty_fd);
  child->pty_fd = -1;
}

bool
child_is_alive(struct child* child)
{
    return child->pid;
}

bool
child_is_parent(struct child* child)
{
  if (!child->pid)
    return false;
  DIR *d = opendir("/proc");
  if (!d)
    return false;
  bool res = false;
  struct dirent *e;
  char fn[18] = "/proc/";
  while ((e = readdir(d))) {
    char *pn = e->d_name;
    if (isdigit((uchar)*pn) && strlen(pn) <= 6) {
      snprintf(fn + 6, 12, "%s/ppid", pn);
      FILE *f = fopen(fn, "r");
      if (!f)
        continue;
      pid_t ppid = 0;
      fscanf(f, "%u", &ppid);
      fclose(f);
      if (ppid == child->pid) {
        res = true;
        break;
      }
    }
  }
  closedir(d);
  return res;
}

void
child_write(struct child* child, const char *buf, uint len)
{
  if (child->pty_fd >= 0)
    write(child->pty_fd, buf, len);
}

void
child_printf(struct child* child, const char *fmt, ...)
{
  if (child->pty_fd >= 0) {
    va_list va;
    va_start(va, fmt);
    char *s;
    int len = vasprintf(&s, fmt, va);
    va_end(va);
    if (len >= 0)
      write(child->pty_fd, s, len);
    free(s);
  }
}

void
child_send(struct child* child, const char *buf, uint len)
{
  term_reset_screen(child->term);
  if (child->term->echoing)
    term_write(child->term, buf, len);
  child_write(child, buf, len);
}

void
child_sendw(struct child* child, const wchar *ws, uint wlen)
{
  char s[wlen * cs_cur_max];
  int len = cs_wcntombn(s, ws, sizeof s, wlen);
  if (len > 0)
    child_send(child, s, len);
}

void
child_resize(struct child* child, struct winsize *winp)
{
  if (child->pty_fd >= 0)
    ioctl(child->pty_fd, TIOCSWINSZ, winp);
}

static int
foreground_pid(struct child* child)
{
  return (child->pty_fd >= 0) ? tcgetpgrp(child->pty_fd) : 0;
}

static char *
foreground_cwd(struct child* child)
{
  // if working dir is communicated interactively, use it
  if (child->dir && *(child->dir))
    return strdup(child->dir);

  // for WSL, do not check foreground process; hope start dir is good
  if (support_wsl) {
    char cwd[MAX_PATH];
    if (getcwd(cwd, sizeof(cwd)))
      return strdup(cwd);
    else
      return 0;
  }

  int fg_pid = foreground_pid(child);
  if (fg_pid > 0) {
    char proc_cwd[32];
    sprintf(proc_cwd, "/proc/%u/cwd", fg_pid);
    return realpath(proc_cwd, 0);
  }
  return 0;
}

char *
foreground_prog(struct child* child)
{
  int fg_pid = foreground_pid(child);
  if (fg_pid > 0) {
    char exename[32];
    sprintf(exename, "/proc/%u/exename", fg_pid);
    FILE * enf = fopen(exename, "r");
    if (enf) {
      char exepath[MAX_PATH + 1];
      fgets(exepath, sizeof exepath, enf);
      fclose(enf);
      // get basename of program path
      char * exebase = strrchr(exepath, '/');
      if (exebase)
        exebase++;
      else
        exebase = exepath;
      return strdup(exebase);
    }
  }
  return 0;
}

void
user_command(struct child* child, int n)
{
  if (*cfg.user_commands) {
    char * cmds = cs__wcstombs(cfg.user_commands);
    char * cmdp = cmds;
    char sepch = ';';
    if ((uchar)*cmdp <= (uchar)' ')
      sepch = *cmdp++;

    char * progp;
    while (n >= 0 && (progp = strchr(cmdp, ':'))) {
      progp++;
      char * sepp = strchr(progp, sepch);
      if (sepp)
        *sepp = '\0';

      if (n == 0) {
        char * fgp = foreground_prog(child);
        if (fgp) {
          setenv("FATTY_PROG", fgp, true);
          free(fgp);
        }
        char * fgd = foreground_cwd(child);
        if (fgd) {
          setenv("FATTY_CWD", fgd, true);
          free(fgd);
        }
        term_cmd(child->term, progp);
        break;
      }
      n--;

      if (sepp)
        cmdp = sepp + 1;
      else
        break;
    }
    free(cmds);
  }
}

/*
   used by win_open
*/
wstring
child_conv_path(struct child* child, wstring wpath)
{
  int wlen = wcslen(wpath);
  int len = wlen * cs_cur_max;
  char path[len];
  len = cs_wcntombn(path, wpath, len, wlen);
  path[len] = 0;

  char *exp_path;  // expanded path
  if (*path == '~') {
    // Tilde expansion
    char *name = path + 1;
    char *rest = strchr(path, '/');
    if (rest)
      *rest++ = 0;
    else
      rest = "";
    char *base;
    if (!*name)
      base = child->home;
    else {
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
      // Find named user's home directory
      struct passwd *pw = getpwnam(name);
      base = (pw ? pw->pw_dir : 0) ?: "";
#else
      // Pre-1.5 Cygwin simply copies HOME into pw_dir, which is no use here.
      base = "";
#endif
    }
    exp_path = asform("%s/%s", base, rest);
  }
  else if (*path != '/') {
#if CYGWIN_VERSION_DLL_MAJOR >= 1005
    // Handle relative paths. Finding the foreground process working directory
    // requires the /proc filesystem, which isn't available before Cygwin 1.5.

    // Find pty's foreground process, if any. Fall back to child process.
    int fg_pid = (child->pty_fd >= 0) ? tcgetpgrp(child->pty_fd) : 0;
    if (fg_pid <= 0)
      fg_pid = child->pid;

    char *cwd = foreground_cwd(child);
    exp_path = asform("%s/%s", cwd ?: child->home, path);
    if (cwd)
      free(cwd);
#else
    // If we're lucky, the path is relative to the home directory.
    exp_path = asform("%s/%s", child->home, path);
#endif
  }
  else
    exp_path = path;

# if CYGWIN_VERSION_API_MINOR >= 222
  // CW_INT_SETLOCALE was introduced in API 0.222
  cygwin_internal(CW_INT_SETLOCALE);
# endif
  wchar *win_wpath = path_posix_to_win_w(exp_path);
  // Drop long path prefix if possible,
  // because some programs have trouble with them.
  if (win_wpath && wcslen(win_wpath) < MAX_PATH) {
    wchar *old_win_wpath = win_wpath;
    if (wcsncmp(win_wpath, W("\\\\?\\UNC\\"), 8) == 0) {
      win_wpath = wcsdup(win_wpath + 6);
      win_wpath[0] = '\\';  // Replace "\\?\UNC\" prefix with "\\"
      free(old_win_wpath);
    }
    else if (wcsncmp(win_wpath, W("\\\\?\\"), 4) == 0) {
      win_wpath = wcsdup(win_wpath + 4);  // Drop "\\?\" prefix
      free(old_win_wpath);
    }
  }

  if (exp_path != path)
    free(exp_path);

  return win_wpath;
}

void
child_set_fork_dir(struct child* child, char * dir)
{
  strset(&child->dir, dir);
}

void
setenvi(char * env, int val)
{
  static char valbuf[22];  // static to prevent #530
  sprintf(valbuf, "%d", val);
  setenv(env, valbuf, true);
}

void
child_fork(struct child* child, int argc, char *argv[], int moni)
{
  void reset_fork_mode()
  {
    clone_size_token = true;
  }

  pid_t clone = fork();

  if (cfg.daemonize) {
    if (clone < 0) {
      childerror(child->term, _("Error: Could not fork child daemon"), true, errno, 0);
      reset_fork_mode();
      return;  // assume next fork will fail too
    }
    if (clone > 0) {  // parent waits for intermediate child
      int status;
      waitpid(clone, &status, 0);
      reset_fork_mode();
      return;
    }

    clone = fork();
    if (clone < 0) {
      exit_fatty(255);
    }
    if (clone > 0) {  // new parent / previous child
      exit_fatty(0);  // exit and make the grandchild a daemon
    }
  }

  if (clone == 0) {  // prepare child process to spawn new terminal
    if (child->pty_fd >= 0)
      close(child->pty_fd);
    if (child_log_fd >= 0)
      close(child_log_fd);
    close(child_win_fd);

    if (child->dir && *child->dir) {
      chdir(child->dir);
      setenv("PWD", child->dir, true);
    }

#ifdef add_child_parameters
    // add child parameters
    int newparams = 0;
    char * * newargv = malloc((argc + newparams + 1) * sizeof(char *));
    int i = 0, j = 0;
    bool addnew = true;
    while (1) {
      if (addnew && (! argv[i] || strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "-") == 0)) {
        addnew = false;
        // insert additional parameters here
        newargv[j++] = "-o";
        static char parbuf1[28];  // static to prevent #530
        sprintf(parbuf1, "Rows=%d",  child->term->rows);
        newargv[j++] = parbuf1;
        newargv[j++] = "-o";
        static char parbuf2[31];  // static to prevent #530
        sprintf(parbuf2, "Columns=%d",  child->term->cols);
        newargv[j++] = parbuf2;
      }
      newargv[j] = argv[i];
      if (! argv[i])
        break;
      i++;
      j++;
    }
    argv = newargv;
#else
    (void) argc;
#endif

    // provide environment to clone size
    if (clone_size_token) {
      setenvi("FATTY_ROWS", child->term->rows);
      setenvi("FATTY_COLS", child->term->cols);
    }
    // provide environment to select monitor
    if (moni > 0)
      setenvi("FATTY_MONITOR", moni);
    // propagate shortcut-inherited icon
    if (icon_is_from_shortcut)
      setenv("FATTY_ICON", cs__wcstoutf(cfg.icon), true);

    //setenv("FATTY_CHILD", "1", true);

#if CYGWIN_VERSION_DLL_MAJOR >= 1005
    execv("/proc/self/exe", argv);
#else
    // /proc/self/exe isn't available before Cygwin 1.5, so use argv[0] instead.
    // Strip enclosing quotes if present.
    char *path = argv[0];
    int len = strlen(path);
    if (path[0] == '"' && path[len - 1] == '"') {
      path = strdup(path + 1);
      path[len - 2] = 0;
    }
    execvp(path, argv);
#endif
    exit_fatty(255);
  }
  reset_fork_mode();
}

void
child_launch(struct child* child, int n, int argc, char * argv[], int moni)
{
  if (*cfg.session_commands) {
    char * cmds = cs__wcstombs(cfg.session_commands);
    char * cmdp = cmds;
    char sepch = ';';
    if ((uchar)*cmdp <= (uchar)' ')
      sepch = *cmdp++;

    char * paramp;
    while (n >= 0 && (paramp = strchr(cmdp, ':'))) {
      paramp++;
      char * sepp = strchr(paramp, sepch);
      if (sepp)
        *sepp = '\0';

      if (n == 0) {
        if (cfg.geom_sync) {
          if (win_is_fullscreen) {
            setenvi("FATTY_DX", 0);
            setenvi("FATTY_DY", 0);
          }
          else {
            RECT r;
            GetWindowRect(wnd, &r);
            setenvi("FATTY_X", r.left);
            setenvi("FATTY_Y", r.top);
            setenvi("FATTY_DX", r.right - r.left);
            setenvi("FATTY_DY", r.bottom - r.top);
          }
        }
        argc = 1;
        char ** new_argv = newn(char *, argc + 1);
        new_argv[0] = argv[0];
        // prepare launch parameters from config string
        while (*paramp) {
          while (*paramp == ' ')
            paramp++;
          if (*paramp) {
            new_argv = renewn(new_argv, argc + 2);
            new_argv[argc] = paramp;
            argc++;
            while (*paramp && *paramp != ' ')
              paramp++;
            if (*paramp == ' ')
              *paramp++ = '\0';
          }
        }
        new_argv[argc] = 0;
        child_fork(child, argc, new_argv, moni);
        free(new_argv);
        break;
      }
      n--;

      if (sepp)
        cmdp = sepp + 1;
      else
        break;
    }
    free(cmds);
  }
}

