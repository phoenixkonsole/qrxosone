/* pthread_cond_timedwait with a deadline that has already passed.
 *
 * AROS's pthread.library turns the absolute deadline into a relative time and checks only that the
 * result is not zero. A deadline that passed before the call gives a negative time, which
 * timer.device reads as about 136 years, so the wait never ends. Blender's render loop waits 5 ms at
 * a time on its queue of finished tiles: one task switch longer than that between computing the
 * deadline and waiting was enough to hang a render for good. Python's GIL waits the same way. */
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>

int pthread_cond_timedwait_relative_np(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *reltime);

int __wrap_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime)
{
	struct timeval now;
	struct timespec rel;

	if (!cond || !mutex || !abstime) return EINVAL;
	gettimeofday(&now, NULL);
	rel.tv_sec = abstime->tv_sec - now.tv_sec;
	rel.tv_nsec = abstime->tv_nsec - now.tv_usec * 1000L;
	while (rel.tv_nsec < 0) {
		rel.tv_nsec += 1000000000L;
		rel.tv_sec--;
	}
	while (rel.tv_nsec >= 1000000000L) {
		rel.tv_nsec -= 1000000000L;
		rel.tv_sec++;
	}
	/* timer.device needs at least a microsecond; the caller still holds the mutex, as POSIX asks. */
	if (rel.tv_sec < 0 || (rel.tv_sec == 0 && rel.tv_nsec < 1000)) return ETIMEDOUT;
	return pthread_cond_timedwait_relative_np(cond, mutex, &rel);
}
