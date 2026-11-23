/*
 * Minimal <sys/eventfd.h> for the S21 iOS Mesa build.
 *
 * Mesa's Panfrost kmod backend includes it unconditionally, but neither macOS
 * (native compiler-tools build) nor iOS has it. The declarations are enough to
 * compile the backend; no eventfd is ever created on the Apple side.
 */
#ifndef S21_SYS_EVENTFD_H
#define S21_SYS_EVENTFD_H

#include <stdint.h>

typedef uint64_t eventfd_t;

int eventfd(unsigned int initval, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);

#endif
