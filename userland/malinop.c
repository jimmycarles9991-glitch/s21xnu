/* Placeholder for the launchd test slot (the GPU composites the UI in the kernel). It also sets the calendar
 * clock: the XNU port has no PMIC RTC read driver, so without this it boots near 1970 and the Astronomy
 * wallpaper draws the night side. V27 prefers sweet_epoch= from kern.bootargs (written by the boot script
 * at kexec time); /usr/local/etc/sweet_epoch, written when the rootfs was built, is only a fallback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/sysctl.h>
#include <unistd.h>

static long
epoch_from_bootargs(void)
{
	char args[2048];
	size_t size = sizeof(args);
	const char *at = args;
	const char key[] = "sweet_epoch=";

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || size == 0) {
		return 0;
	}
	if (size >= sizeof(args)) {
		size = sizeof(args) - 1;
	}
	args[size] = '\0';

	while ((at = strstr(at, key)) != NULL) {
		const char *digits = at + sizeof(key) - 1;
		char *end;
		unsigned long long value;

		if (at != args && at[-1] != ' ' && at[-1] != '\t') {
			at += sizeof(key) - 1;
			continue;
		}
		value = strtoull(digits, &end, 10);
		if (end != digits && (*end == '\0' || *end == ' ' || *end == '\t' || *end == '\n')) {
			return (long)value;
		}
		at += sizeof(key) - 1;
	}
	return 0;
}

int main(void)
{
	const char *source = "kern.bootargs sweet_epoch=";
	long e = epoch_from_bootargs();

	if (e <= 0) {
		FILE *f = fopen("/usr/local/etc/sweet_epoch", "r");

		source = "/usr/local/etc/sweet_epoch";
		e = 0;
		if (f) {
			if (fscanf(f, "%ld", &e) != 1) {
				e = 0;
			}
			fclose(f);
		}
	}
	struct timeval now; gettimeofday(&now, NULL);
	if (e > 1700000000L && e < 2000000000L && now.tv_sec < e) {
		struct timeval tv = { e, 0 };
		int r = settimeofday(&tv, NULL);

		fprintf(stderr, "malinop: clock set to %ld from %s: %d\n", e, source, r);
	} else {
		fprintf(stderr, "malinop: clock left alone (%ld from %s, now %ld)\n", e, source, (long)now.tv_sec);
	}
	for (;;) pause();
	return 0;
}
