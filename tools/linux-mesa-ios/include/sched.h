/*
 * Minimal compatibility <sched.h> for Apple platforms.
 *
 * macOS provides struct sched_param, SCHED_FIFO and sched_get_priority_max()
 * in its system <sched.h>, but omits sched_setscheduler(). Mesa's Panfrost
 * kmod perf thread calls the latter, so declare it here for link completeness.
 */
#ifndef S21_SCHED_H
#define S21_SCHED_H

#include_next <sched.h>

int sched_setscheduler(int pid, int policy, const struct sched_param *param);

#endif
