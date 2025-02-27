/*  Copyright (C) 2000-2006  Kevin Shanahan

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

/*
 * common/log.h
 *
 * Stuff for logging selected output to a file
 * as well as the stdout.
 */

#pragma once

#include <atomic>
#include <cstdarg>
#include <list>
#include <cmath> // for log10
#include <fmt/core.h>
#include <common/bitflags.hh>
#include <common/fs.hh>

// forward declaration
namespace settings
{
class common_settings;
}

namespace logging
{
enum class flag : uint8_t
{
    NONE = 0, // none of the below (still prints though)
    DEFAULT = nth_bit(0), // prints everywhere
    VERBOSE = nth_bit(1), // prints everywhere, if enabled
    PROGRESS = nth_bit(2), // prints only to stdout
    PERCENT = nth_bit(3), // prints everywhere, if enabled
    STAT = nth_bit(4), // prints everywhere, if enabled
    CLOCK_ELAPSED = nth_bit(5), // overrides displayElapsed if disabled
    ALL = 0xFF
};

extern bitflags<flag> mask;
extern bool enable_color_codes;

// Windows: calls SetConsoleMode for ANSI escape sequence processing (so colors work)
void preinitialize();

// initialize logging subsystem
void init(const fs::path &filename, const settings::common_settings &settings);

// shutdown logging subsystem
void close();

// print to respective targets based on log flag
void print(flag logflag, const char *str);

// print to default targets
void print(const char *str);

// format print to specified targets
template<typename... Args>
inline void print(flag type, const char *fmt, const Args &...args)
{
    if (mask & type) {
        print(type, fmt::format(fmt::runtime(fmt), std::forward<const Args &>(args)...).c_str());
    }
}

// format print to default targets
template<typename... Args>
inline void print(const char *formt, const Args &...args)
{
    print(flag::DEFAULT, fmt::format(fmt::runtime(formt), std::forward<const Args &>(args)...).c_str());
}

void header(const char *name);

// TODO: C++20 source_location
#ifdef _MSC_VER
#define funcprint(fmt, ...) print("{}: " fmt, __FUNCTION__, ##__VA_ARGS__)
#define funcheader() header(__FUNCTION__)
#else
#define funcprint(fmt, ...) print("{}: " fmt, __func__, ##__VA_ARGS__)
#define funcheader() header(__func__)
#endif

void assert_(bool success, const char *expr, const char *file, int line);

// Display a percent timer. This also keeps track of how long the
// current task is taking to execute. Note that only one of these
// can be active at a time. Once `count` == `max`, the progress
// bar will "finish" and be inoperable.
// Only use this by hand if you absolutely need to; otherwise,
// use <common/parallel.h>'s parallel_for(_each)
void percent(uint64_t count, uint64_t max, bool displayElapsed = true);

// use this as "max" (and on the final run, "count") to indicate that
// the counter does not have a determinating maximum factor.
constexpr uint64_t indeterminate = std::numeric_limits<uint64_t>::max();

// simple wrapper to percent() to use it in an object-oriented manner. you can
// call print() to explicitly end the clock, or allow it to run out of scope.
struct percent_clock
{
    std::atomic<uint64_t> max;
    bool displayElapsed = true;
    std::atomic<uint64_t> count = 0;
    bool ready = true;

    // runs a tick immediately to show up on stdout
    // unless max is zero
    percent_clock(uint64_t i_max = indeterminate);

    // increase count by 1
    void increase();
    
    // increase count by 1
    void operator()();

    // increase count by 1
    void operator++(int);

    // prints & ends the clock; class is invalid after this call.
    void print();

    // implicitly calls print()
    ~percent_clock();
};

// base class intended to be inherited for stat trackers;
// they will automatically print the results at the end,
// in the order of registration.
struct stat_tracker_t
{
    struct stat
    {
        std::string name;
        bool show_even_if_zero;
        bool is_warning;
        std::atomic_size_t count = 0;

        inline stat(const std::string &name, bool show_even_if_zero, bool is_warning) :
            name(name),
            show_even_if_zero(show_even_if_zero),
            is_warning(is_warning)
        {
        }
        
        inline size_t operator++(int) noexcept { return count++; }
        inline size_t operator++() noexcept { return ++count; }
        inline size_t operator+=(size_t v) noexcept { return count += v; }
        inline size_t operator++(int) volatile noexcept { return count++; }
        inline size_t operator++() volatile noexcept { return ++count; }
        inline size_t operator+=(size_t v) volatile noexcept { return count += v; }
    };

    std::list<stat> stats;
    bool stats_printed = false;

    stat &register_stat(const std::string &name, bool show_even_if_zero = false, bool is_warning = false);
    static size_t number_of_digits(size_t n);
    size_t number_of_digit_padding();
    void print_stats();
    virtual ~stat_tracker_t();
};
}; // namespace logging

[[noreturn]] void Error(const char *error);

template<typename... Args>
[[noreturn]] inline void Error(const char *fmt, const Args &...args)
{
    auto formatted = fmt::format(fmt::runtime(fmt), std::forward<const Args &>(args)...);
    Error(formatted.c_str());
}

#define FError(fmt, ...) Error("{}: " fmt, __func__, ##__VA_ARGS__)

/**
 * assertion macro that is used in all builds (debug/release)
 */
#define Q_stringify__(x) #x
#define Q_stringify(x) Q_stringify__(x)
#define Q_assert(x) logging::assert_((x), Q_stringify(x), __FILE__, __LINE__)

#define Q_assert_unreachable() Q_assert(false)
