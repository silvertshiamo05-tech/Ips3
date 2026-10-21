// Ips3: per-thread CPU time (see thread_usage.h).
#include "thread_usage.h"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

#include <pthread.h>

#ifdef __APPLE__
#include <mach/mach.h>

// threading_posix.cc (Xenia) keeps the names given to threads from other threads.
extern "C" bool ips3_thread_name(pthread_t thread, char* name, size_t size);
#else
#include <dirent.h>
#include <fstream>
#include <unistd.h>
#endif

namespace
{
	struct Sample
	{
		std::string name;
		uint64_t cpuNs; // total so far
	};

	// Every thread of the process: id -> name and CPU time so far.
	std::map<uint64_t, Sample> SampleThreads()
	{
		std::map<uint64_t, Sample> samples;
#ifdef __APPLE__
		thread_act_array_t threads;
		mach_msg_type_number_t count;
		if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
			return samples;
		for (mach_msg_type_number_t i = 0; i < count; i++)
		{
			thread_identifier_info_data_t id;
			mach_msg_type_number_t idCount = THREAD_IDENTIFIER_INFO_COUNT;
			thread_basic_info_data_t basic;
			mach_msg_type_number_t basicCount = THREAD_BASIC_INFO_COUNT;
			if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, thread_info_t(&id), &idCount) == KERN_SUCCESS &&
				thread_info(threads[i], THREAD_BASIC_INFO, thread_info_t(&basic), &basicCount) == KERN_SUCCESS)
			{
				char name[64] = {};
				if (pthread_t thread = pthread_from_mach_thread_np(threads[i]))
				{
					if (!ips3_thread_name(thread, name, sizeof(name)))
						pthread_getname_np(thread, name, sizeof(name));
				}
				auto ns = [](time_value_t t) { return uint64_t(t.seconds) * 1000000000ull + uint64_t(t.microseconds) * 1000ull; };
				samples[id.thread_id] = { name[0] ? name : fmt::format("thread {}", id.thread_id),
					ns(basic.user_time) + ns(basic.system_time) };
			}
			mach_port_deallocate(mach_task_self(), threads[i]);
		}
		vm_deallocate(mach_task_self(), vm_address_t(threads), count * sizeof(thread_act_t));
#else
		const long ticks = sysconf(_SC_CLK_TCK);
		if (DIR* dir = opendir("/proc/self/task"))
		{
			while (dirent* entry = readdir(dir))
			{
				if (entry->d_name[0] == '.')
					continue;
				const std::string base = std::string("/proc/self/task/") + entry->d_name;
				std::ifstream statFile(base + "/stat");
				std::string stat((std::istreambuf_iterator<char>(statFile)), {});
				const size_t close = stat.rfind(')');
				if (close == std::string::npos)
					continue;
				// After "pid (comm)": state is field 3, utime and stime are fields 14 and 15.
				std::vector<std::string> fields;
				size_t at = close + 2;
				while (at < stat.size() && fields.size() < 13)
				{
					const size_t space = stat.find(' ', at);
					fields.push_back(stat.substr(at, space - at));
					at = space == std::string::npos ? stat.size() : space + 1;
				}
				if (fields.size() < 13)
					continue;
				std::string name;
				std::getline(std::ifstream(base + "/comm"), name);
				const uint64_t cpuTicks = std::stoull(fields[11]) + std::stoull(fields[12]);
				samples[std::stoull(entry->d_name)] = { name, cpuTicks * 1000000000ull / uint64_t(ticks) };
			}
			closedir(dir);
		}
#endif
		return samples;
	}
}

std::string host::ThreadUsage()
{
	static std::map<uint64_t, Sample> last;
	static auto lastTime = std::chrono::steady_clock::now();

	auto samples = SampleThreads();
	const auto now = std::chrono::steady_clock::now();
	const double elapsedNs = double(std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastTime).count());
	lastTime = now;

	if (last.empty())
	{
		last = std::move(samples); // the first call only starts the measurement
		return "measuring";
	}

	std::vector<std::pair<double, std::string>> busy;
	double total = 0;
	for (const auto& [id, sample] : samples)
	{
		const auto previous = last.find(id);
		const uint64_t before = previous != last.end() ? previous->second.cpuNs : 0;
		const double percent = sample.cpuNs > before ? double(sample.cpuNs - before) * 100.0 / elapsedNs : 0;
		total += percent;
		if (percent >= 3.0)
			busy.emplace_back(percent, sample.name);
	}
	last = std::move(samples);

	std::sort(busy.begin(), busy.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	std::string text = fmt::format("all threads {:.0f}%", total);
	for (size_t i = 0; i < busy.size() && i < 10; i++)
		text += fmt::format(", {} {:.0f}%", busy[i].second, busy[i].first);
	return text;
}
