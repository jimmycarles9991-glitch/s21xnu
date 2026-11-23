/*
 * Minimal <sys/timerfd.h> for the S21 iOS Mesa build.
 *
 * See sys/eventfd.h: Mesa's Panfrost kmod backend needs these declarations to
 * compile on Apple platforms, but never creates a timerfd there.
 */
#ifndef S21_SYS_TIMERFD_H
#define S21_SYS_TIMERFD_H

#define TFD_CLOEXEC  02000000
#define TFD_NONBLOCK 04000
#define TFD_TIMER_ABSTIME 1

/* void * avoids depending on <time.h>'s struct itimerspec, which macOS does
 * not expose without extra feature macros. Any object pointer converts. */
int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags,
                    const void *new_value, void *old_value);
int timerfd_gettime(int fd, void *curr_value);

#endif
