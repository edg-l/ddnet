/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#ifndef BASE_TIME_H
#define BASE_TIME_H

#include <chrono>
#include <cstdint>
#include <ctime>

/**
 * Time utilities.
 *
 * @defgroup Time Time
 *
 * @ref Timestamp
 */

/**
 * Timestamp related functions.
 *
 * @defgroup Timestamp Timestamps
 *
 * @ref Time
 */

/**
 * Clears the cached sample of the high resolution timer.
 *
 * @ingroup Time
 *
 * @see time_get
 */
void set_new_tick();

/**
 * Fetches a sample from a high resolution timer and converts it to nanoseconds.
 *
 * @ingroup Time
 *
 * @return Current value of the timer in nanoseconds.
 */
std::chrono::nanoseconds time_get_nanoseconds();

/**
 * Returns an uncached sample of the real high resolution timer in nanoseconds.
 *
 * @ingroup Time
 *
 * @remark Unlike @link time_get_nanoseconds @endlink this always reflects wall clock
 *         progress and is never affected by @link time_set_fixed_step @endlink. Use it for
 *         frame pacing and profiling.
 */
std::chrono::nanoseconds time_get_real_nanoseconds();

#if defined(CONF_AUTOMATION)
/**
 * Switches the clock reported by time_get and time_get_nanoseconds to a virtual clock that
 * only advances when time_advance_fixed_step is called.
 *
 * @ingroup Time
 *
 * @param step_nanoseconds Amount the virtual clock advances per step, 0 restores the real clock.
 *
 * @remark The virtual clock starts at the current real value, so enabling it introduces no
 *         discontinuity. time_get_impl and time_get_real_nanoseconds are unaffected.
 */
void time_set_fixed_step(int64_t step_nanoseconds);

/** Advances the virtual clock by one step. No effect when the fixed step is 0. */
void time_advance_fixed_step();
#endif

/**
 * Fetches a sample from a high resolution timer.
 *
 * @ingroup Time
 *
 * @return Current value of the timer.
 *
 * @remark To know how fast the timer is ticking, see @link time_freq @endlink.
 *
 * @see time_freq
 */
int64_t time_get_impl();

/**
 * Fetches a cached sample from a high resolution timer.
 *
 * @ingroup Time
 *
 * @return Current value of the timer.
 *
 * @remark To know how fast the timer is ticking, see @link time_freq @endlink.
 * @remark The value is cached for each tick, see @link set_new_tick @endlink.
 *         Uses @link time_get_impl @endlink to fetch the uncached sample.
 * @remark While a fixed step is configured with @link time_set_fixed_step @endlink this returns
 *         the virtual clock directly and neither caches nor calls @link time_get_impl @endlink,
 *         because the virtual clock only moves on @link time_advance_fixed_step @endlink and is
 *         therefore already stable within a tick.
 *
 * @see time_freq time_get_impl
 */
int64_t time_get();

/**
 * @ingroup Time
 *
 * @return The frequency of the high resolution timer.
 */
constexpr int64_t time_freq()
{
	using namespace std::chrono_literals;
	return std::chrono::nanoseconds(1s).count();
}

/**
 * Retrieves the current time as a UNIX timestamp.
 *
 * @ingroup Timestamp
 *
 * @return The time as a UNIX timestamp.
 */
int64_t time_timestamp();

/**
 * Retrieves the hours since midnight (0..23).
 *
 * @ingroup Time
 *
 * @return The current hour of the day.
 */
int time_houroftheday();

/**
 * A season of the year or seasonal event.
 *
 * @ingroup Time
 */
enum class ETimeSeason
{
	SPRING,
	SUMMER,
	AUTUMN,
	WINTER,
	EASTER,
	HALLOWEEN,
	XMAS,
	NEWYEAR,
};

/**
 * Retrieves the current season or event of the year.
 *
 * @ingroup Time
 *
 * @return The current season or event, see `ETimeSeason`.
 */
ETimeSeason time_season();

/**
 * Copies a timestamp of the current time in the format `year-month-day_hour-minute-second` to the string.
 *
 * @ingroup Timestamp
 *
 * @param buffer Pointer to a buffer that shall receive the timestamp string.
 * @param buffer_size Size of the buffer.
 *
 * @remark Guarantees that buffer string will contain null-termination.
 */
void str_timestamp(char *buffer, int buffer_size);

/**
 * Copies a timestamp of the current time in the given format to the string.
 *
 * @ingroup Timestamp
 *
 * @param buffer Pointer to a buffer that shall receive the timestamp string.
 * @param buffer_size Size of the buffer.
 * @param format Time formatting string. See https://cppreference.com/w/c/chrono/strftime.html for format description.
 *               See `TimestampFormat` for common formats.
 *
 * @remark Guarantees that buffer string will contain null-termination.
 */
[[gnu::format(strftime, 3, 0)]] void str_timestamp_format(char *buffer, int buffer_size, const char *format);

/**
 * Copies a timestamp of the given time in the given format to the string.
 *
 * @ingroup Timestamp
 *
 * @param time The time value to represent as a string.
 * @param buffer Pointer to a buffer that shall receive the timestamp string.
 * @param buffer_size Size of the buffer.
 * @param format Time formatting string. See https://cppreference.com/w/c/chrono/strftime.html for format description.
 *               See `TimestampFormat` for common formats.
 *
 * @remark Guarantees that buffer string will contain null-termination.
 */
[[gnu::format(strftime, 4, 0)]] void str_timestamp_ex(time_t time, char *buffer, int buffer_size, const char *format);

/**
 * Parses a string into a timestamp following a specified format.
 *
 * @ingroup Timestamp
 *
 * @param string Pointer to the string to parse.
 * @param format The time format to use. See `TimestampFormat` for common formats.
 * @param timestamp Pointer to the timestamp result.
 *
 * @return `true` on success, `false` if the string could not be parsed with the specified format.
 */
[[gnu::format(strftime, 2, 0)]] bool timestamp_from_str(const char *string, const char *format, time_t *timestamp);

/**
 * Timestamp format strings for the `str_timestamp_format`, `str_timestamp_ex` and `timestamp_from_str` functions.
 *
 * @ingroup Timestamp
 *
 * @see str_timestamp_format
 * @see str_timestamp_ex
 * @see timestamp_from_str
 */
namespace TimestampFormat
{
	inline const char *const TIME = "%H:%M:%S";
	inline const char *const SPACE = "%Y-%m-%d %H:%M:%S";
	inline const char *const NOSPACE = "%Y-%m-%d_%H-%M-%S";
}

/**
 * Time formats for the `str_time` and `str_time_float` functions.
 *
 * @ingroup Timestamp
 *
 * @see str_time
 * @see str_time_float
 */
enum class ETimeFormat
{
	DAYS,
	HOURS,
	MINS,
	HOURS_CENTISECS,
	MINS_CENTISECS,
	SECS_CENTISECS,
};

/**
 * Returns the number of milliseconds from a time float.
 *
 * Takes care to not introduce more rounding issues, which is what a naive
 * `std::roundf(seconds * 1000.0)` would do.
 *
 * @ingroup Timestamp
 *
 * @param seconds Time in seconds.
 *
 * @return Number of milliseconds.
 */
int64_t time_milliseconds_from_seconds(float seconds);

/**
 * Formats a time string.
 *
 * @ingroup Timestamp
 *
 * @param centisecs Time in centiseconds.
 * @param format Format of the time string, see `ETimeFormat`.
 * @param buffer Pointer to a buffer that shall receive the timestamp string.
 * @param buffer_size Size of the buffer.
 *
 * @return Number of bytes written.
 */
int str_time(int64_t centisecs, ETimeFormat format, char *buffer, int buffer_size);

/**
 * Formats a time string.
 *
 * @ingroup Timestamp
 *
 * @param secs Time in seconds.
 * @param format Format of the time string, see `ETimeFormat`.
 * @param buffer Pointer to a buffer that shall receive the timestamp string.
 * @param buffer_size Size of the buffer.
 *
 * @remark The time is rounded to the nearest centisecond.
 *
 * @return Number of bytes written.
 */
int str_time_float(float secs, ETimeFormat format, char *buffer, int buffer_size);

#endif
