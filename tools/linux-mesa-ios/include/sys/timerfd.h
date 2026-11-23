/*
 * Minimal <sys/timerfd.h> for the S21 iOS Mesa build.
 *
 * See sys/eventfd.h: Mesa's Panfrost kmod backend needs these declarations to
 * compile on Apple platforms, but never creates a timerfd there.
 */
#ifndef S21_SYS_TIMERFD_H
#define S21_SYS_TIMERFD_H

#include <time.h>

/* macOS does not expose struct itimerspec; define the POSIX layout. Linux
 * gets it from <time.h> via <sched.h>, so skip it there to avoid a redefinition. */
#ifndef __linux__
struct itimerspec {
   struct timespec it_interval;
   struct timespec it_value;
};
#endif

#define TFD_CLOEXEC  02000000
#define TFD_NONBLOCK 04000
#define TFD_TIMER_ABSTIME 1

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags,
                    const struct itimerspec *new_value,
                    struct itimerspec *old_value);
int timerfd_gettime(int fd, struct itimerspec *curr_value);

#endif
