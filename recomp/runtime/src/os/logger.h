// Logging used by the adapted UnleashedRecomp kernel code. Stubs log their name once per call
// site, so a log of a run reads as the list of imports the game actually reached.
#pragma once

#include <atomic>
#include <cstdio>
#include <fmt/core.h>

namespace os::logger
{
	inline void Write(const char* level, const char* func, const std::string& text)
	{
		std::fprintf(stderr, "[%s] %s: %s\n", level, func, text.c_str());
	}
}

#define IPS3_LOG_ONCE(level, text)                                          \
	do                                                                      \
	{                                                                       \
		static std::atomic<bool> s_logged{false};                          \
		if (!s_logged.exchange(true))                                       \
			os::logger::Write(level, __func__, text);                      \
	} while (0)

#define LOG_UTILITY(str) IPS3_LOG_ONCE("stub", str)
#define LOGF_UTILITY(str, ...) IPS3_LOG_ONCE("stub", fmt::format(str, ##__VA_ARGS__))
#define LOG(str) os::logger::Write("info", __func__, str)
#define LOGF(str, ...) os::logger::Write("info", __func__, fmt::format(str, ##__VA_ARGS__))
#define LOGN(str) os::logger::Write("info", __func__, str)
#define LOGFN(str, ...) os::logger::Write("info", __func__, fmt::format(str, ##__VA_ARGS__))
#define LOGN_WARNING(str) os::logger::Write("warn", __func__, str)
#define LOGFN_WARNING(str, ...) os::logger::Write("warn", __func__, fmt::format(str, ##__VA_ARGS__))
#define LOGN_ERROR(str) os::logger::Write("error", __func__, str)
#define LOGFN_ERROR(str, ...) os::logger::Write("error", __func__, fmt::format(str, ##__VA_ARGS__))
#define LOG_WARNING(str) LOGN_WARNING(str)
#define LOGF_WARNING(str, ...) LOGFN_WARNING(str, ##__VA_ARGS__)
#define LOG_ERROR(str) LOGN_ERROR(str)
#define LOGF_ERROR(str, ...) LOGFN_ERROR(str, ##__VA_ARGS__)
