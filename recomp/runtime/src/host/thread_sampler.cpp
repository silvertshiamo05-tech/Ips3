// Ips3: sampling profiler (see thread_sampler.h).
#include "thread_sampler.h"

#include <fmt/format.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <cxxabi.h>
#include <dlfcn.h>
#include <pthread.h>

#ifdef __APPLE__
#include <mach/mach.h>
extern "C" bool ips3_thread_name(pthread_t thread, char* name, size_t size);
#else
#include <csignal>
#include <dirent.h>
#ifdef __ANDROID__
#include <unwind.h>
#else
#include <execinfo.h>
#endif
#include <fstream>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace
{
	const char* const SAMPLED[] = { "guest 0", "GPU Commands" };
	constexpr int MAX_FRAMES = 8;

	struct Stack
	{
		int depth = 0;
		uintptr_t pc[MAX_FRAMES];
	};

	std::mutex g_mutex;
	std::map<std::string, std::map<std::vector<uintptr_t>, uint32_t>> g_counts; // thread -> stack -> samples

	// Function name for an address (demangled, without parameters).
	std::string Symbol(uintptr_t address)
	{
		static std::map<uintptr_t, std::string> cache;
		if (auto it = cache.find(address); it != cache.end())
			return it->second;
		std::string name = fmt::format("{:x}", address);
		Dl_info info;
		const bool found = dladdr(reinterpret_cast<void*>(address), &info) != 0;
		if (found && !info.dli_sname && info.dli_fname)
		{
			name = info.dli_fname;
			if (const size_t slash = name.rfind('/'); slash != std::string::npos)
				name = name.substr(slash + 1);
		}
		if (found && info.dli_sname)
		{
			int status = 0;
			char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
			name = status == 0 && demangled ? demangled : info.dli_sname;
			free(demangled);
			if (const size_t paren = name.find('('); paren != std::string::npos && paren > 0)
				name.resize(paren);
			if (name.starts_with("__imp__"))
				name = name.substr(7);
			if (const size_t colon = name.rfind("::"); colon != std::string::npos && name.size() > 48)
				name = name.substr(colon + 2);
		}
		return cache[address] = name;
	}

	// The innermost function, plus the first two recompiled game functions (sub_...) under it.
	std::vector<uintptr_t> Key(const Stack& stack)
	{
		std::vector<uintptr_t> key;
		std::string last;
		int guest = 0;
		for (int i = 0; i < stack.depth && guest < 2; i++)
		{
			const std::string name = Symbol(stack.pc[i]);
			if (name == last)
				continue;
			last = name;
			const bool isGuest = name.starts_with("sub_");
			if (key.empty() || isGuest)
			{
				key.push_back(stack.pc[i]);
				guest += isGuest;
			}
		}
		return key;
	}

#ifdef __APPLE__
	// Apple keeps frame pointers on arm64: the suspended thread's registers give the stack.
	bool Sample(thread_act_t thread, Stack& stack)
	{
		if (thread_suspend(thread) != KERN_SUCCESS)
			return false;
		arm_thread_state64_t state;
		mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
		const bool ok = thread_get_state(thread, ARM_THREAD_STATE64, thread_state_t(&state), &count) == KERN_SUCCESS;
		if (ok)
		{
			stack.depth = 0;
			stack.pc[stack.depth++] = uintptr_t(arm_thread_state64_get_pc(state)) & 0x0000FFFFFFFFFFFFull;
			stack.pc[stack.depth++] = uintptr_t(arm_thread_state64_get_lr(state)) & 0x0000FFFFFFFFFFFFull;
			uintptr_t fp = uintptr_t(arm_thread_state64_get_fp(state)) & 0x0000FFFFFFFFFFFFull;
			const uintptr_t sp = uintptr_t(arm_thread_state64_get_sp(state)) & 0x0000FFFFFFFFFFFFull;
			while (stack.depth < MAX_FRAMES && fp >= sp && fp < sp + 8 * 1024 * 1024 && (fp & 7) == 0)
			{
				const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(fp);
				const uintptr_t ret = frame[1] & 0x0000FFFFFFFFFFFFull;
				if (!ret)
					break;
				stack.pc[stack.depth++] = ret;
				if (frame[0] <= fp)
					break;
				fp = frame[0];
			}
		}
		thread_resume(thread);
		return ok;
	}

	void SamplerThread()
	{
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			thread_act_array_t threads;
			mach_msg_type_number_t count;
			if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
				continue;
			for (mach_msg_type_number_t i = 0; i < count; i++)
			{
				char name[64] = {};
				if (pthread_t thread = pthread_from_mach_thread_np(threads[i]))
				{
					if (!ips3_thread_name(thread, name, sizeof(name)))
						pthread_getname_np(thread, name, sizeof(name));
				}
				for (const char* wanted : SAMPLED)
				{
					Stack stack;
					if (strcmp(name, wanted) == 0 && Sample(threads[i], stack))
					{
						auto key = Key(stack);
						std::lock_guard lock(g_mutex);
						g_counts[wanted][key]++;
					}
				}
				mach_port_deallocate(mach_task_self(), threads[i]);
			}
			vm_deallocate(mach_task_self(), vm_address_t(threads), count * sizeof(thread_act_t));
		}
	}
#else
	// Linux (for testing): a signal makes the thread record its own stack.
	std::atomic<bool> g_sampleReady{ false };
	Stack g_signalStack;

#ifdef __ANDROID__
	// Android's C library gains backtrace() only in API 33: walk the stack with the unwinder.
	struct UnwindState
	{
		void** frames;
		int max, count;
	};

	_Unwind_Reason_Code UnwindFrame(_Unwind_Context* context, void* arg)
	{
		auto* state = static_cast<UnwindState*>(arg);
		if (state->count >= state->max)
			return _URC_END_OF_STACK;
		if (const uintptr_t pc = _Unwind_GetIP(context))
			state->frames[state->count++] = reinterpret_cast<void*>(pc);
		return _URC_NO_REASON;
	}

	int backtrace(void** frames, int max)
	{
		UnwindState state{ frames, max, 0 };
		_Unwind_Backtrace(UnwindFrame, &state);
		return state.count;
	}
#endif

	void OnSampleSignal(int)
	{
		void* frames[MAX_FRAMES + 2];
		const int depth = backtrace(frames, MAX_FRAMES + 2);
		g_signalStack.depth = 0;
		for (int i = 2; i < depth; i++) // skip this handler and the signal trampoline
			g_signalStack.pc[g_signalStack.depth++] = uintptr_t(frames[i]);
		g_sampleReady = true;
	}

	void SamplerThread()
	{
		{
			void* warm[1];
			backtrace(warm, 1); // loads libgcc before any signal needs it
		}
		struct sigaction action = {};
		action.sa_handler = OnSampleSignal;
		action.sa_flags = SA_RESTART;
		sigaction(SIGPROF, &action, nullptr);
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			DIR* dir = opendir("/proc/self/task");
			if (!dir)
				continue;
			while (dirent* entry = readdir(dir))
			{
				if (entry->d_name[0] == '.')
					continue;
				std::string name;
				std::getline(std::ifstream(std::string("/proc/self/task/") + entry->d_name + "/comm"), name);
				for (const char* wanted : SAMPLED)
				{
					if (name != wanted)
						continue;
					g_sampleReady = false;
					syscall(SYS_tgkill, getpid(), atoi(entry->d_name), SIGPROF);
					for (int i = 0; i < 200 && !g_sampleReady; i++)
						std::this_thread::sleep_for(std::chrono::microseconds(50));
					if (g_sampleReady)
					{
						auto key = Key(g_signalStack);
						std::lock_guard lock(g_mutex);
						g_counts[wanted][key]++;
					}
				}
			}
			closedir(dir);
		}
	}
#endif
}

void host::StartThreadSampler()
{
	static std::once_flag once;
	std::call_once(once, [] { std::thread(SamplerThread).detach(); });
}

std::string host::ThreadSamplerReport()
{
	std::map<std::string, std::map<std::vector<uintptr_t>, uint32_t>> counts;
	{
		std::lock_guard lock(g_mutex);
		counts.swap(g_counts);
	}
	std::string text;
	for (auto& [thread, stacks] : counts)
	{
		uint32_t total = 0;
		std::map<std::string, uint32_t> byName; // merged by symbol text
		for (auto& [key, n] : stacks)
		{
			std::string name;
			for (size_t i = 0; i < key.size(); i++)
				name += (i ? " < " : "") + Symbol(key[i]);
			byName[name] += n;
			total += n;
		}
		std::vector<std::pair<uint32_t, std::string>> sorted;
		for (auto& [name, n] : byName)
			sorted.emplace_back(n, name);
		std::sort(sorted.rbegin(), sorted.rend());
		text += fmt::format("[profile] {} ({} samples):", thread, total);
		for (size_t i = 0; i < sorted.size() && i < 5; i++)
			text += fmt::format(" {}% {};", sorted[i].first * 100 / std::max(1u, total), sorted[i].second);
		text += "\n";
	}
	return text;
}
