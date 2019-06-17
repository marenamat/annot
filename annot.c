/*
 * Command line output annotator.
 *
 * (c) 2019 Maria Matejka <mq@ucw.cz>
 *
 * Can be freely distributed and used under the terms of the GNU GPL.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 201906L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SC(what, ...) do { if (what(__VA_ARGS__) < 0) { perror("Error calling " #what ":"); fatal(); } } while (0)

//#define debug printf
#define debug(...)

static pid_t child = -1;

static void fatal(void) {
  if (child != -1)
    kill(child, SIGKILL);

  abort();
  exit(42);
}

typedef unsigned int uint;

struct channel {
  const char *fmtpre, *fmtpost;
  char *buf, *bfrom, *bnl, *bto, *bend;
  uint bskip;
  int ifd, ofd, idx;
  uint ipp, opp;
  uint bol:1;
  uint ird:1;
  uint ord:1;
  uint iev:1;
  uint oev:1;
  uint hup:1;
};

#define BUFSIZE 4096

static struct channel *channels[3];
int finished_channels = 0;
int chn = 0;

static void channel_wait_in(uint idx) {
  struct channel *ch = channels[idx];
  debug("poll start for fd %d\n", ch->ifd);
  ch->iev = 1;
}

static void channel_wait_out(uint idx) {
  struct channel *ch = channels[idx];
  debug("poll start for fd %d\n", ch->ofd);
  ch->oev = 1;
}

static size_t channel_annotate(uint idx, char *dest) {
  struct channel *ch = channels[idx];
  struct timespec now;
  SC(clock_gettime, CLOCK_REALTIME, &now);

  uint ms;
  if (ch->fmtpost) {
    /* Convert to milliseconds */
    ms = (now.tv_nsec + 500000) / 1000000;
    if (ms >= 1000) {
      ms -= 1000;
      now.tv_sec++;
    }
  } else {
    if (now.tv_nsec >= 500000000)
      now.tv_sec++;
  }

  struct tm *tmp = localtime(&now.tv_sec);
  int out = strftime(dest, ch->bskip, ch->fmtpre, tmp);

  if (!ch->fmtpost)
    return out;

  out += snprintf(dest+out, ch->bskip - out, "%03u", ms);
  out += strftime(dest+out, ch->bskip - out, ch->fmtpost, tmp);

  return out;
}

static void channel_write(uint idx);

static void channel_close(uint idx) {
  struct channel *ch = channels[idx];
  debug("Closing (ifd %d ofd %d) channel %d\n", ch->ifd, ch->ofd, idx);
  if (idx == 0)
    SC(shutdown, ch->ofd, SHUT_WR);

  ch->ifd = -1;
  ch->ofd = -1;
  free(ch->buf);
  channels[ch->idx] = NULL;
  free(ch);

  finished_channels++;
}

static void channel_close_in(uint idx) {
  struct channel *ch = channels[idx];
  debug("Close in (fd %d) channel %d\n", ch->ifd, idx);
  if (ch->bfrom == ch->bto)
    return channel_close(idx);

  ch->hup = 1;
  ch->iev = 0;
  if (ch->ord)
    channel_write(idx);
}

static void channel_ready_both(uint idx) {
  struct channel *ch = channels[idx];
  while (1) {
    /* Write something if we have any data */
    if (ch->bto > ch->bfrom)
      channel_write(idx);

    /* Write may have closed the channel */
    if (!channels[idx])
      return;

    /* Read some data */
    ssize_t sz = read(ch->ifd, ch->bto, (ch->bend - ch->bto));
    if (sz < 0) {
      if (errno == EAGAIN) {
	if (ch->hup)
	  return channel_close_in(idx);

	ch->ird = 0;
	return channel_wait_in(idx);
      }

      fprintf(stderr, "Error reading from %d: %m\n", ch->ifd);
      fatal();
    }

    if (sz == 0)
      channel_close_in(idx);

    /* Close in may have closed the channel at all */
    if (!channels[idx])
      return;

    ch->bto += sz;

    debug("Channel %d read %d bytes of data\n", idx, sz);
  }
}

static void channel_write(uint idx) {
  struct channel *ch = channels[idx];
  while (1) {
    /* Prepend annotation if we should do it. */
    if (ch->fmtpre) {
      ch->bnl = ch->bfrom;
      while ((ch->bnl < ch->bto) && (*ch->bnl++ != '\n'))
	;

      if (ch->bol) {
	char *annot = alloca(ch->bskip);
	size_t alen = channel_annotate(idx, annot);

	memcpy(ch->bfrom - alen, annot, alen);
	ch->bol = 0;
	ch->bfrom -= alen;
      }
    } else
      ch->bnl = ch->bto;

    /* Write some data */
    size_t sz = write(ch->ofd, ch->bfrom, ch->bnl - ch->bfrom);
    if (sz < 0) {
      if (errno == EAGAIN) {
	ch->ord = 0;
	return channel_wait_out(idx);
      }

      fprintf(stderr, "Error writing to %d: %m\n", ch->ofd);
      fatal();
    }

    if (sz == 0)
      abort();

    ch->bfrom += sz;

    debug("Channel %d wrote %d bytes of data\n", idx, sz);

    /* Check end of line */
    if (ch->bfrom[-1] == '\n')
      ch->bol = 1;

    /* Empty buffer */
    if (ch->bfrom >= ch->bto) {
      if (ch->hup)
	return channel_close(idx);

      ch->bfrom = ch->bto = ch->buf + ch->bskip;
      return;
    }

    /* Move to beginning */
    if ((ch->bend - ch->bto) > (ch->bfrom - ch->buf + ch->bskip)) {
      char *nb = ch->buf + ch->bskip;
      char *ne = nb + (ch->bto - ch->bfrom);
      char *nl = ch->bnl - (ch->bfrom - nb);
      memmove(nb, ch->bfrom, ne - nb);
      ch->bfrom = nb;
      ch->bto = ne;
      ch->bnl = nl;
    }
  }
}

static void channel_ready_in(uint idx) {
  struct channel *ch = channels[idx];
  debug("Channel %d (fd %d) ready in (ord %d)\n", idx, ch->ifd, ch->ord);
  ch->iev = 0;
  ch->ird = 1;

  if (ch->ord)
    channel_ready_both(idx);
}

static void channel_ready_out(uint idx) {
  struct channel *ch = channels[idx];
  debug("Channel %d (fd %d) ready out (ird %d)\n", idx, ch->ofd, ch->ird);
  ch->oev = 0;
  ch->ord = 1;

  if (ch->ird)
    channel_ready_both(idx);
  else if (ch->bto > ch->bfrom)
    channel_write(idx);
}

static void channel_flush(uint idx) {
  struct channel *ch = channels[idx];
  debug("Flushing (fd %d) channel %d\n", ch->ifd, idx);
  ch->hup = 1;
  channel_ready_in(idx);
}

static struct channel *channel_init(int ifd, int ofd, const char *fmt) {
  struct channel *ch = malloc(sizeof(struct channel));
  memset(ch, 0, sizeof(struct channel));

  channels[ch->idx = chn++] = ch;

  ch->ifd = ifd;
  ch->ofd = ofd;

  SC(fcntl, ifd, F_SETFL, O_NONBLOCK);
  SC(fcntl, ofd, F_SETFL, O_NONBLOCK);

  channel_wait_in(ch->idx);
  channel_wait_out(ch->idx);

  ch->buf = malloc(BUFSIZE);

  uint flen;
  if (fmt && (flen = strlen(fmt))) {
    for (const char *pf = fmt; *pf; pf++) {
      if (*pf != '%')
	continue;

      switch (*++pf) {
	case 0:
	  fprintf(stderr, "Invalid pattern: %s\n", fmt);
	  fatal();
	  break;
	case 'f':
	  {
	    char *pre = malloc(pf - fmt);
	    memcpy(pre, fmt, pf - fmt - 1);
	    pre[pf - fmt - 1] = 0;

	    char *post = malloc(flen - (pf - fmt));
	    memcpy(post, pf + 1, flen - (pf - fmt) - 1);
	    post[flen - (pf - fmt) - 1] = 0;

	    ch->fmtpre = pre;
	    ch->fmtpost = post;
	    break;
	  }
	default:
	  continue;
      }
      break;
    }

    if (!ch->fmtpre)
      ch->fmtpre = fmt;

    ch->bskip = BUFSIZE;
    size_t len = channel_annotate(ch->idx, ch->buf);
    if (len == 0) {
      fprintf(stderr, "Too long annotator string: %s\n", fmt);
      fatal();
    }
    ch->bskip = len + 42;
  }

  ch->bfrom = ch->bto = ch->buf + ch->bskip;
  ch->bend = ch->buf + BUFSIZE;

  ch->bol = 1;
  return ch;
}

static void process_signal(int fd) {
  debug("Got signal!\n");
  struct signalfd_siginfo i;
  size_t sz = read(fd, &i, sizeof(i));
  if (sz != sizeof(i)) {
    debug("Short read from signalfd: %zd, want %zu", sz, sizeof(i));
    fatal();
  }

  if (i.ssi_signo != SIGCHLD) {
    debug("Got unknown signal: %u", i.ssi_signo);
    return;
  }

  if (i.ssi_code != CLD_EXITED) {
    debug("Child not exited: %d", i.ssi_code);
    return;
  }

  if (channels[0])
    channel_close(0);

  if (channels[1])
    channel_flush(1);

  if (channels[2])
    channel_flush(2);
}

int main(int argc, char **argv) {
  int epm, eps, opm, ops;

  SC(openpty, &epm, &eps, NULL, NULL, NULL);
  SC(openpty, &opm, &ops, NULL, NULL, NULL);

  struct termios ts;
  SC(tcgetattr, eps, &ts);
  ts.c_oflag &= ~OPOST;
  SC(tcsetattr, eps, TCSANOW, &ts);

  SC(tcgetattr, ops, &ts);
  ts.c_oflag &= ~OPOST;
  SC(tcsetattr, ops, TCSANOW, &ts);

  int ins[2];
  SC(socketpair, AF_UNIX, SOCK_STREAM, 0, ins);

  /*
  struct stat ist, ost, est;
  SC(fstat, 0, &ist);
  SC(fstat, 1, &ost);
  SC(fstat, 2, &est);

  debug("ioe reg: %d %d %d\n", S_ISREG(ist.st_mode), S_ISREG(ost.st_mode), S_ISREG(est.st_mode));
  */

  sigset_t sfdmask;
  sigemptyset(&sfdmask);
  sigaddset(&sfdmask, SIGCHLD);
  SC(sigprocmask, SIG_BLOCK, &sfdmask, NULL);
  int sfd = signalfd(-1, &sfdmask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (sfd < 0) {
    perror("Error calling signalfd:");
    fatal();
  }

  child = fork();

  if (!child) {
    SC(close, epm);
    SC(close, opm);
    SC(close, ins[0]);

    SC(close, 0);
    SC(dup2, ins[1], 0);
    SC(close, ins[1]);

    SC(close, 1);
    SC(dup2, ops, 1);
    SC(close, ops);

    SC(close, 2);
    SC(dup2, eps, 2);
    SC(close, eps);

    SC(execvp, argv[1], argv+1);

    abort();
  }

  SC(close, ins[1]);

  channel_init(0, ins[0], NULL),
  channel_init(opm, 1, "%Y-%m-%d %H:%M:%S.%f LOG "),
  channel_init(epm, 2, "%Y-%m-%d %H:%M:%S.%f ERR ");

  while (finished_channels < 3) {
    struct pollfd pfd[7];
    int nfds = 0;
    pfd[nfds++] = (struct pollfd) {
      .fd = sfd,
      .events = POLLIN,
    };
    for (int i=0; i<3; i++) {
      if (!channels[i])
	continue;

      if (channels[i]->iev) {
	pfd[channels[i]->ipp = nfds++] = (struct pollfd) {
	  .fd = channels[i]->ifd,
	  .events = POLLIN | POLLRDHUP,
	};
      }

      if (channels[i]->oev) {
	pfd[channels[i]->opp = nfds++] = (struct pollfd) {
	  .fd = channels[i]->ofd,
	  .events = POLLOUT,
	};
      }

      debug("Channel %d state: iev %d oev %d ird %d ord %d data %d\n",
	  i,
	  channels[i]->iev, channels[i]->oev,
	  channels[i]->ird, channels[i]->ord,
	  channels[i]->bto - channels[i]->bfrom);
    }
    debug("Total pollfds: %d\n", nfds);

    if (!nfds)
      exit(0);

    int n = poll(pfd, nfds, -1);

    if (n > 0) {
      for (int i=0; i<3; i++) {
	if (channels[i] && channels[i]->iev)
	  if (pfd[channels[i]->ipp].revents & POLLIN)
	    channel_ready_in(i);
	  else if (pfd[channels[i]->ipp].revents || channels[i]->hup)
	    channel_close_in(i);

	if (channels[i] && channels[i]->oev && pfd[channels[i]->opp].revents)
	  if (pfd[channels[i]->opp].revents & POLLOUT)
	    channel_ready_out(i);
	  else
	    channel_close(i);
      }
      if (pfd[0].revents & POLLIN) {
	process_signal(sfd);
      }
    } else if (n == 0)
      fatal();
    else if ((errno == EAGAIN) || (errno == EINTR))
      continue;
    else {
      perror("Error calling epoll_wait:");
      fatal();
    }
  }
}
