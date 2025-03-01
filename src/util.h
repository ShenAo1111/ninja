// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_UTIL_H_
#define NINJA_UTIL_H_

#ifdef _WIN32
#include "win32port.h"
#else
#include <stdint.h>
#endif

#include <stdarg.h>

#include <string>
#include <vector>
#include <chrono>
#include <iostream>
#include <map>
#include <algorithm>
#include <iomanip>

#if !defined(__has_cpp_attribute)
#  define __has_cpp_attribute(x)  0
#endif

#if __has_cpp_attribute(noreturn)
#  define NORETURN [[noreturn]]
#else
#  define NORETURN  // nothing for old compilers
#endif

/// Log a fatal message and exit.
NORETURN void Fatal(const char* msg, ...);

// Have a generic fall-through for different versions of C/C++.
#if __has_cpp_attribute(fallthrough)
#  define NINJA_FALLTHROUGH [[fallthrough]]
#elif defined(__clang__)
#  define NINJA_FALLTHROUGH [[clang::fallthrough]]
#else
#  define NINJA_FALLTHROUGH // nothing
#endif

/// Log a warning message.
void Warning(const char* msg, ...);
void Warning(const char* msg, va_list ap);

/// Log an error message.
void Error(const char* msg, ...);
void Error(const char* msg, va_list ap);

/// Log an informational message.
void Info(const char* msg, ...);
void Info(const char* msg, va_list ap);

/// Canonicalize a path like "foo/../bar.h" into just "bar.h".
/// |slash_bits| has bits set starting from lowest for a backslash that was
/// normalized to a forward slash. (only used on Windows)
void CanonicalizePath(std::string* path, uint64_t* slash_bits);
void CanonicalizePath(char* path, size_t* len, uint64_t* slash_bits);

/// Appends |input| to |*result|, escaping according to the whims of either
/// Bash, or Win32's CommandLineToArgvW().
/// Appends the string directly to |result| without modification if we can
/// determine that it contains no problematic characters.
void GetShellEscapedString(const std::string& input, std::string* result);
void GetWin32EscapedString(const std::string& input, std::string* result);

/// Read a file to a string (in text mode: with CRLF conversion
/// on Windows).
/// Returns -errno and fills in \a err on error.
int ReadFile(const std::string& path, std::string* contents, std::string* err);

/// Mark a file descriptor to not be inherited on exec()s.
void SetCloseOnExec(int fd);

/// Given a misspelled string and a list of correct spellings, returns
/// the closest match or NULL if there is no close enough match.
const char* SpellcheckStringV(const std::string& text,
                              const std::vector<const char*>& words);

/// Like SpellcheckStringV, but takes a NULL-terminated list.
const char* SpellcheckString(const char* text, ...);

bool islatinalpha(int c);

/// Removes all Ansi escape codes (http://www.termsys.demon.co.uk/vtansi.htm).
std::string StripAnsiEscapeCodes(const std::string& in);

/// @return the number of processors on the machine.  Useful for an initial
/// guess for how many jobs to run in parallel.  @return 0 on error.
int GetProcessorCount();

/// @return the load average of the machine. A negative value is returned
/// on error.
double GetLoadAverage();

/// a wrapper for getcwd()
std::string GetWorkingDirectory();

/// Truncates a file to the given size.
bool Truncate(const std::string& path, size_t size, std::string* err);

#ifdef _MSC_VER
#define snprintf _snprintf
#define fileno _fileno
#define chdir _chdir
#define strtoull _strtoui64
#define getcwd _getcwd
#define PATH_MAX _MAX_PATH
#endif

#ifdef _WIN32
/// Convert the value returned by GetLastError() into a string.
std::string GetLastErrorString();

/// Calls Fatal() with a function name and GetLastErrorString.
NORETURN void Win32Fatal(const char* function, const char* hint = NULL);

/// Naive implementation of C++ 20 std::bit_cast(), used to fix Clang and GCC
/// [-Wcast-function-type] warning on casting result of GetProcAddress().
template <class To, class From>
inline To FunctionCast(From from) {
	static_assert(sizeof(To) == sizeof(From), "");
	To result;
	memcpy(&result, &from, sizeof(To));
	return result;
}
#endif

int platformAwareUnlink(const char* filename);


class Profiler {
public:
    Profiler(int n = 7, int m = 5) 
        : root_duration(0), 
            root_start_set(false),
            max_levels(n),
            max_items_per_level(m) {}

    void rootStart() {
        if (root_start_set) {
            std::cout << "错误：root 已开始" << std::endl;
            return;
        }
        root_start = std::chrono::high_resolution_clock::now();
        root_start_set = true;
    }

    void rootEnd() {  // 移除参数，使用类的成员变量
        if (!root_start_set) {
            std::cout << "错误：root 未开始" << std::endl;
            return;
        }
        if (root_duration != 0) {
            std::cout << "错误：root 已结束" << std::endl;
            return;
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        root_duration = std::chrono::duration_cast<std::chrono::microseconds>
                        (end_time - root_start).count();
        
        std::cout << "总耗时: " << root_duration << " 微秒" << std::endl;
        
        // 只处理前 max_levels 个 level
        int level_count = 1;
        for (const auto& level_pair : level_name_duration) {
            if (level_count > max_levels) break;
            
            int level = level_pair.first;
            const auto& name_duration = level_pair.second;

            // 将 name_duration 转换为 vector 并按 duration 降序排序
            std::vector<std::pair<std::string, long long>> sorted_names(
                name_duration.begin(), name_duration.end());
            std::sort(sorted_names.begin(), sorted_names.end(),
                        [](const auto& a, const auto& b) { return a.second > b.second; });

            // 打印前 max_items_per_level 个
            std::cout << "\nLevel " << level << " 前 " << max_items_per_level << " 个耗时最多的记录:" << std::endl;
            int count = 0;
            for (const auto& pair : sorted_names) {
                if (count >= max_items_per_level) break;
                double percentage = (pair.second * 100.0) / root_duration;
                std::cout << "  " << pair.first << ": " << pair.second << " 微秒 ("
                            << std::fixed << std::setprecision(2) << percentage << "%)"
                            << std::endl;
                count++;
            }
            level_count++;
        }
        std::cout << "Total edges started: " << total_edge_started << std::endl;
        std::cout << "Total edges finished: " << total_edge_finished << std::endl;

    }

    void start(const std::string& name) {
        timings.emplace_back(name, std::chrono::high_resolution_clock::now(), timings.size());
    }

    void end() {  // 移除参数，使用类的成员变量
        if (timings.empty()) {
            std::cout << "错误：没有活动的性能分析会话" << std::endl;
            return;
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto& timing = timings.back();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>
                        (end_time - timing.start_time).count();

        // 实时打印所有计时信息
        std::cout << "PROFILER_LOG: "
                    << "level=" << timing.level << "|"
                    << "name=" << timing.name << "|"
                    << "duration=" << duration << " 微秒"
                    << std::endl;

        // 只记录每个 level 的前 max_levels 个 name
        int level = timing.level;
        if (level <= max_levels) {
            level_name_duration[level][timing.name] += duration;
        }

        timings.pop_back();
    }

    void StartEdgeRecord() {
        total_edge_started++;
    }
    
    void FinishEdgeRecord() {
        total_edge_finished++;
    }

private:
    struct Timing {
        std::string name;
        std::chrono::high_resolution_clock::time_point start_time;
        int level;

        Timing(const std::string& n, 
                std::chrono::high_resolution_clock::time_point t,
                int lvl) 
            : name(n), start_time(t), level(lvl) {}
    };

    std::vector<Timing> timings;
    std::map<int, std::map<std::string, long long>> level_name_duration;
    std::chrono::high_resolution_clock::time_point root_start;
    long long root_duration;
    bool root_start_set;
    int max_levels;         // 新增成员变量存储 n
    int max_items_per_level; // 新增成员变量存储 m
    int total_edge_started = 0;
    int total_edge_finished = 0;
};

extern Profiler profiler;

#endif  // NINJA_UTIL_H_
