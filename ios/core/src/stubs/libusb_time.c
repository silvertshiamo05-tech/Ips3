// libusb leaves its clock to the OS backend; the null backend used on iOS has none.
#include <time.h>

void usbi_get_monotonic_time(struct timespec* tp)
{
	clock_gettime(CLOCK_MONOTONIC, tp);
}

void usbi_get_real_time(struct timespec* tp)
{
	clock_gettime(CLOCK_REALTIME, tp);
}
