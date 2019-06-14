#define _GNU_SOURCE
#define _POSIX_C_SOURCE 201906L

#include <stdio.h>
#include <stdlib.h>
#include <pty.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SC(what, ...) do { if (what(__VA_ARGS__) < 0) { perror("Error calling " #what ":"); exit(1); } } while (0)

static pid_t child;

static void fatal(void) {
  kill(child, SIGKILL);
  exit(42);
}

int main(int argc, char **argv) {
  int epm, eps, opm, ops;
  
  SC(openpty, &epm, &eps, NULL, NULL, NULL);
  SC(openpty, &opm, &ops, NULL, NULL, NULL);

  child = fork();

  if (!child) {
    SC(close, epm);
    SC(close, opm);
    SC(close, 0);
    SC(dup2, ops, 0);
    SC(close, 1);
    SC(dup2, ops, 1);
    SC(close, 2);
    SC(dup2, eps, 2);

    write(1, "zeli\n", 6);
    write(2, "bagr\n", 5);

    SC(execvp, argv[1], argv+2);

    abort();
  }

  SC(close, ops);
  SC(close, eps);
  
  struct pollfd pfd[5] = {
    { .fd = 0, .events = POLLIN, },
    { .fd = 1, .events = POLLOUT, },
    { .fd = 2, .events = POLLOUT, },
    { .fd = epm, .events = POLLIN, },
    { .fd = opm, .events = POLLIN | POLLOUT, },
  };

  int instate = 0;
  int outstate = 0;
  int errstate = 0;

  while (1) {
    /*
    if (waitpid(pid, NULL, WNOHANG) == pid)
      break;
      */

    printf("Polling for: %x %x %x %x %x\n", pfd[0].events, pfd[1].events, pfd[2].events, pfd[3].events, pfd[4].events);
    SC(poll, pfd, 5, -1);

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    struct tm *tmp = localtime(&(now.tv_sec));

    if (tmp == NULL) {
      perror("Error calling localtime:");
      exit(1);
    }

    char timebuf[100];
    int cnt = strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S.", tmp);
    sprintf(timebuf+cnt, "%u ", now.tv_nsec / 1000);

    short rev;
    if (rev = pfd[0].revents) {
      if (rev | POLLIN) {
	instate |= 1;
	pfd[0].events &= ~POLLIN;
      }
      else {
	pfd[0].events = 0;
	instate = 4;
      }
      pfd[0].revents = 0;
    }

    if (rev = pfd[1].revents) {
      if (rev | POLLOUT) {
	outstate |= 2;
	pfd[1].events &= ~POLLOUT;
      }
      else
	fatal();

      pfd[1].revents = 0;
    }

    if (rev = pfd[2].revents) {
      if (rev | POLLOUT) {
	errstate |= 2;
	pfd[2].events &= ~POLLOUT;
      }
      else
	fatal();

      pfd[2].revents = 0;
    }

    if (rev = pfd[3].revents) {
      if (rev | POLLIN) {
	errstate |= 1;
	pfd[3].events &= ~POLLIN;
      }
      else {
	pfd[3].events = 0;
	errstate = 4;
      }
      pfd[3].revents = 0;
    }

    if (rev = pfd[4].revents) {
      if (rev | POLLIN & POLLOUT) {
	if (rev | POLLIN) {
	  outstate |= 1;
	  pfd[4].events &= ~POLLIN;
	}
	if (rev | POLLOUT) {
	  instate |= 2;
	  pfd[4].events &= ~POLLOUT;
	}
      } else {
	pfd[4].events = 0;
	outstate = 4;
	instate = 4;
      }
      pfd[4].revents = 0;
    }

    printf("State at %s: %d %d %d\n", timebuf, instate, outstate, errstate);

    char buf[4096], *bptr = buf, *bend = buf;
    while (errstate == 3) {
      ssize_t 
    }
  }
}
