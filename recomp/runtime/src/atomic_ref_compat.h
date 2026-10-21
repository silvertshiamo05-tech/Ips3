// Ips3: std::atomic_ref for C++ libraries without it (the Android NDK's libc++ 18). Only what
// the runtime uses: load/store/exchange/compare-exchange, and wait/notify on futexes.
#pragma once

#include <atomic>
#include <version>

#if !defined(__cpp_lib_atomic_ref)
#include <climits>
#include <thread>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace std
{
	template <class T>
	struct atomic_ref
	{
		explicit atomic_ref(T& object) : ptr(&object) {}
		atomic_ref(const atomic_ref&) = default;
		atomic_ref& operator=(const atomic_ref&) = delete;

		T load(memory_order order = memory_order_seq_cst) const { return __atomic_load_n(ptr, int(order)); }
		void store(T value, memory_order order = memory_order_seq_cst) const { __atomic_store_n(ptr, value, int(order)); }
		T operator=(T value) const { store(value); return value; }
		operator T() const { return load(); }
		T exchange(T value, memory_order order = memory_order_seq_cst) const { return __atomic_exchange_n(ptr, value, int(order)); }
		bool compare_exchange_weak(T& expected, T desired, memory_order order = memory_order_seq_cst) const
		{
			return __atomic_compare_exchange_n(ptr, &expected, desired, true, int(order), int(memory_order_seq_cst));
		}
		bool compare_exchange_strong(T& expected, T desired, memory_order order = memory_order_seq_cst) const
		{
			return __atomic_compare_exchange_n(ptr, &expected, desired, false, int(order), int(memory_order_seq_cst));
		}

		void wait(T old, memory_order order = memory_order_seq_cst) const
		{
			while (load(order) == old)
			{
#if defined(__linux__)
				if constexpr (sizeof(T) == 4)
				{
					uint32_t raw;
					__builtin_memcpy(&raw, &old, 4);
					syscall(SYS_futex, reinterpret_cast<uint32_t*>(ptr), FUTEX_WAIT_PRIVATE, raw, nullptr, nullptr, 0);
					continue;
				}
#endif
				this_thread::yield();
			}
		}
		void notify_one() const
		{
#if defined(__linux__)
			if constexpr (sizeof(T) == 4)
				syscall(SYS_futex, reinterpret_cast<uint32_t*>(ptr), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
#endif
		}
		void notify_all() const
		{
#if defined(__linux__)
			if constexpr (sizeof(T) == 4)
				syscall(SYS_futex, reinterpret_cast<uint32_t*>(ptr), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#endif
		}

		T* ptr;
	};
}
#endif
