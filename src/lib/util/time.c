/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief Platform independent time functions
 * @file lib/util/time.c
 *
 * @copyright 2016 Alan DeKok <aland@freeradius.org>
 */
RCSID("$Id$")

#include <freeradius-devel/autoconf.h>
#include <freeradius-devel/util/time.h>
#include <freeradius-devel/util/dlist.h>

/*
 *	Avoid too many ifdef's later in the code.
 */
#if !defined(HAVE_CLOCK_GETTIME) && !defined(__MACH__)
#error clock_gettime is required
#endif

#if !defined(HAVE_CLOCK_GETTIME) && defined(__MACH__)
/*
 *	AbsoluteToNanoseconds() has been deprecated,
 *	but absolutetime_to_nanoseconds() doesn't
 *	seem to be available, either.
 */
USES_APPLE_DEPRECATED_API
#  include <CoreServices/CoreServices.h>
#  include <mach/mach.h>
#  include <mach/mach_time.h>
#endif

#include <stdatomic.h>

static _Atomic int64_t			our_realtime;	//!< realtime at the start of the epoch in nanoseconds.

#ifdef HAVE_CLOCK_GETTIME
static int64_t				our_epoch;
#else  /* __MACH__ */
static mach_timebase_info_data_t	timebase;
static uint64_t				our_mach_epoch;
#endif

/** Get a new our_realtime value
 *
 * Should be done regularly to adjust for changes in system time.
 *
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static inline int fr_time_sync(void)
{
	/*
	 *	our_realtime represents system time
	 *	at the start of our epoch.
	 *
	 *	So to convert a realtime timeval
	 *	to fr_time we just subtract
	 *	our_realtime from the timeval,
	 *      which leaves the number of nanoseconds
	 *	elapsed since our epoch.
	 */
#ifdef HAVE_CLOCK_GETTIME
	{
		struct timespec ts_realtime, ts_monotime;

		/*
		 *	Call these consecutively to minimise drift...
		 */
		if (clock_gettime(CLOCK_REALTIME, &ts_realtime) < 0) return -1;
		if (clock_gettime(CLOCK_MONOTONIC, &ts_monotime) < 0) return -1;

		our_realtime = fr_time_delta_from_timespec(&ts_realtime) -
			       (fr_time_delta_from_timespec(&ts_monotime) - our_epoch);
		return 0;
	}
#else
	{
		struct timeval	tv_realtime;
		uint64_t	monotime;

		/*
		 *	Call these consecutively to minimise drift...
		 */
		(void) gettimeofday(&tv_realtime, NULL);
		monotime = mach_absolute_time();

		our_realtime = fr_time_delta_from_timeval(&tv_realtime) -
			       (monotime - our_mach_epoch) * (timebase.numer / timebase.denom);
		return 0;
	}
#endif
}

/** Initialize the local time.
 *
 *  MUST be called when the program starts.  MUST NOT be called after
 *  that.
 *
 * @return
 *	- <0 on error
 *	- 0 on success
 */
int fr_time_start(void)
{
	tzset();	/* Populate timezone, daylight and tzname globals */

#ifdef HAVE_CLOCK_GETTIME
	{
		struct timespec ts;

		if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return -1;
		our_epoch = fr_time_delta_from_timespec(&ts);
	}
#else  /* __MACH__ is defined */
	mach_timebase_info(&timebase);
	our_mach_epoch = mach_absolute_time();
#endif

	return fr_time_sync();
}

/** Return a relative time since the server our_epoch
 *
 *  This time is useful for doing time comparisons, deltas, etc.
 *  Human (i.e. printable) time is something else.
 *
 * @returns fr_time_t time in nanoseconds since the server our_epoch.
 */
fr_time_t fr_time(void)
{
#ifdef HAVE_CLOCK_GETTIME
	struct timespec ts;
	(void) clock_gettime(CLOCK_MONOTONIC, &ts);
	return fr_time_delta_from_timespec(&ts) - our_epoch;
#else  /* __MACH__ is defined */
	uint64_t when;

	when = mach_absolute_time();
	when -= our_mach_epoch;

	return when * (timebase.numer / timebase.denom);
#endif
}

/** Convert a fr_time_t to a struct timeval.
 *
 * @param[out] tv the timeval to update
 * @param[in] when the fr_time_t
 */
void fr_time_to_timeval(struct timeval *tv, fr_time_t when)
{
	fr_time_delta_to_timeval(tv, when + our_realtime);
}

/** Convert a fr_time_t to a struct timeval.
 *
 * @param[out] ts the timeval to update
 * @param[in] when the fr_time_t
 */
void fr_time_to_timespec(struct timespec *ts, fr_time_t when)
{
	fr_time_delta_to_timespec(ts, when + our_realtime);
}

/** Convert an fr_time_t to number of usec since the unix epoch
 *
 */
int64_t fr_time_to_usec(fr_time_t when)
{
	return ((when + our_realtime) / 1000);
}

/** Convert an fr_time_t to number of msec since the unix epoch
 *
 */
int64_t fr_time_to_msec(fr_time_t when)
{
	return ((when + our_realtime) / 1000000);
}

/** Convert an fr_time_t to number of sec since the unix epoch
 *
 */
int64_t fr_time_to_sec(fr_time_t when)
{
	return ((when + our_realtime) / NSEC);
}

/** Convert a timeval to a fr_time_t
 *
 * @param[in] when_tv	The timestamp to convert.
 * @return
 *	- >0 number of nanoseconds since the server started.
 *	- 0 when the server started.
 *	- <0 number of nanoseconds before the server started.
 */
fr_time_t fr_time_from_timeval(struct timeval const *when_tv)
{
	return fr_time_delta_from_timeval(when_tv) - our_realtime;
}

/** Convert a timespec to a fr_time_t
 *
 * @param[in] when_ts	The timestamp to convert.
 * @return
 *	- >0 number of nanoseconds since the server started.
 *	- 0 when the server started.
 *	- 0 if when_tv occurred before the server started.
 */
fr_time_t fr_time_from_timespec(struct timespec const *when_ts)
{
	return fr_time_delta_from_timespec(when_ts) - our_realtime;
}

/** Start time tracking for a request.
 *
 * @param[in] tt the time tracking structure.
 * @param[in] when the event happened
 * @param[out] worker time tracking for the worker thread
 */
void fr_time_tracking_start(fr_time_tracking_t *tt, fr_time_t when, fr_time_tracking_t *worker)
{
	memset(tt, 0, sizeof(*tt));

	tt->when = when;
	tt->start = when;
	tt->resumed = when;

	fr_dlist_init(&(worker->list), fr_time_tracking_t, list.entry);
	fr_dlist_entry_init(&tt->list.entry);
}


#define IALPHA (8)
#define RTT(_old, _new) ((_new + ((IALPHA - 1) * _old)) / IALPHA)

/** End time tracking for this request.
 *
 * After this call, all request processing should be finished.
 *
 * @param[in] tt the time tracking structure.
 * @param[in] when the event happened
 * @param[out] worker time tracking for the worker thread
 */
void fr_time_tracking_end(fr_time_tracking_t *tt, fr_time_t when, fr_time_tracking_t *worker)
{
	tt->when = when;
	tt->end = when;
	tt->running += (tt->end - tt->resumed);

	/*
	 *	This request cannot be in any list.
	 */
	rad_assert(tt->list.entry.prev == &(tt->list.entry));
	rad_assert(tt->list.entry.next == &(tt->list.entry));

	/*
	 *	Update the time that the worker spent processing the request.
	 */
	worker->running += tt->running;
	worker->waiting += tt->waiting;

	if (!worker->predicted) {
		worker->predicted = tt->running;
	} else {
		worker->predicted = RTT(worker->predicted, tt->running);
	}
}


/** Track that a request yielded.
 *
 * @param[in] tt the time tracking structure.
 * @param[in] when the event happened
 * @param[out] worker time tracking for the worker thread
 */
void fr_time_tracking_yield(fr_time_tracking_t *tt, fr_time_t when, fr_time_tracking_t *worker)
{
	tt->when = when;
	tt->yielded = when;

	rad_assert(tt->resumed <= tt->yielded);
	tt->running += (tt->yielded - tt->resumed);

	/*
	 *	Insert this request into the TAIL of the worker's list
	 *	of waiting requests.
	 */
	fr_dlist_insert_head(&worker->list, tt);
}


/** Track that a request resumed.
 *
 * @param[in] tt the time tracking structure.
 * @param[in] when the event happened
 * @param[out] worker time tracking for the worker thread
 */
void fr_time_tracking_resume(fr_time_tracking_t *tt, fr_time_t when, fr_time_tracking_t *worker)
{
	tt->when = when;
	tt->resumed = when;

	rad_assert(tt->resumed >= tt->yielded);

	tt->waiting += (tt->resumed - tt->yielded);

	/*
	 *	Remove this request into the workers list of waiting
	 *	requests.
	 */
	fr_dlist_remove(&worker->list, tt);
}


/** Print debug information about the time tracking structure
 *
 * @param[in] tt the time tracking structure
 * @param[in] fp the file where the debug output is printed.
 */
void fr_time_tracking_debug(fr_time_tracking_t *tt, FILE *fp)
{
#define DPRINT(_x) fprintf(fp, "\t" #_x " = %"PRIu64"\n", tt->_x);

	DPRINT(start);
	DPRINT(end);
	DPRINT(when);

	DPRINT(yielded);
	DPRINT(resumed);

	DPRINT(predicted);
	DPRINT(running);
	DPRINT(waiting);
}

void fr_time_elapsed_update(fr_time_elapsed_t *elapsed, fr_time_t start, fr_time_t end)
{
	fr_time_t delay;

	if (start >= end) {
		delay = 0;
	} else {
		delay = end - start;
	}

	if (delay < 1000) { /* microseconds */
		elapsed->array[0]++;

	} else if (delay < 10000) {
		elapsed->array[1]++;

	} else if (delay < 100000) {
		elapsed->array[2]++;

	} else if (delay < 1000000) { /* milliseconds */
		elapsed->array[3]++;

	} else if (delay < 10000000) {
		elapsed->array[4]++;

	} else if (delay < (fr_time_t) 100000000) {
		elapsed->array[5]++;

	} else if (delay < (fr_time_t) 1000000000) { /* seconds */
		elapsed->array[6]++;

	} else {		/* tens of seconds or more */
		elapsed->array[7]++;

	}
}

static const char *names[8] = {
	"1us", "10us", "100us",
	"1ms", "10ms", "100ms",
	"1s", "10s"
};

static char const *tab_string = "\t\t\t\t\t\t\t\t\t\t\t\t\t\t";

void fr_time_elapsed_fprint(FILE *fp, fr_time_elapsed_t const *elapsed, char const *prefix, int tabs)
{
	int i;

	if (!prefix) prefix = "elapsed";

	for (i = 0; i < 8; i++) {
		if (!elapsed->array[i]) continue;

		fprintf(fp, "%s.%s\t%.*s%" PRIu64 "\n",
			prefix, names[i], tabs, tab_string, elapsed->array[i]);
	}
}
