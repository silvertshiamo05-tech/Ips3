#include <stdafx.h>
#include "xdm.h"
#include <condition_variable>
#include <cpu/guest_thread.h>
#include "freelist.h"
#include "function.h"

Mutex g_kernelLock;

void DestroyKernelObject(KernelObject* obj)
{
    obj->~KernelObject();
    g_userHeap.Free(obj);
}

uint32_t GetKernelHandle(KernelObject* obj)
{
    assert(obj != GetInvalidKernelObject());
    return g_memory.MapVirtual(obj);
}

void DestroyKernelObject(uint32_t handle)
{
    DestroyKernelObject(GetKernelObject(handle));
}

bool IsKernelObject(uint32_t handle)
{
    return (handle & 0x80000000) != 0;
}

bool IsKernelObject(void* obj)
{
    return IsKernelObject(g_memory.MapVirtual(obj));
}

bool IsInvalidKernelObject(void* obj)
{
    return obj == GetInvalidKernelObject();
}

void ips3::TraceCall(const char* name, const PPCContext& ctx, bool after)
{
    // "1" traces everything; otherwise '|'-separated name fragments.
    static const std::vector<std::string> filters = []
    {
        std::vector<std::string> list;
        std::string f = getenv("IPS3_TRACE");
        if (f == "1")
            return list;
        for (size_t start = 0; start <= f.size();)
        {
            const size_t end = std::min(f.find('|', start), f.size());
            if (end > start)
                list.push_back(f.substr(start, end - start));
            start = end + 1;
        }
        return list;
    }();
    if (!filters.empty() && std::none_of(filters.begin(), filters.end(),
        [&](const std::string& f) { return std::string_view(name).find(f) != std::string_view::npos; }))
        return;

    if (after)
        fmt::println(stderr, "[trace {:5}] {} -> {:08X}", GuestThread::GetCurrentThreadId(), name, ctx.r3.u32);
    else
        fmt::println(stderr, "[trace {:5}] {}({:08X}, {:08X}, {:08X}, {:08X}) lr={:08X}", GuestThread::GetCurrentThreadId(), name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
}

void PPCUnresolvedCall(uint32_t target, uint32_t lr)
{
    fmt::println(stderr, "[cpu] indirect call to {:08X} (from near {:08X}): no recompiled function there", target, lr);
    std::abort();
}

namespace
{
    std::mutex g_waitMutex;
    std::condition_variable g_waitCondition;
    uint64_t g_waitGeneration = 0;
}

void NotifyKernelWaiters()
{
    {
        std::lock_guard lock(g_waitMutex);
        ++g_waitGeneration;
    }
    g_waitCondition.notify_all();
}

uint32_t WaitForKernelObjects(KernelObject* const* objects, size_t count, bool waitAll, uint32_t timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    // Wait all: objects are acquired one by one as they become signalled (not atomically).
    std::vector<bool> acquired(waitAll ? count : 0, false);

    for (;;)
    {
        uint64_t generation;
        {
            std::lock_guard lock(g_waitMutex);
            generation = g_waitGeneration;
        }

        if (waitAll)
        {
            bool all = true;
            for (size_t i = 0; i < count; i++)
            {
                if (!acquired[i] && objects[i]->Wait(0) == STATUS_SUCCESS)
                    acquired[i] = true;
                all = all && acquired[i];
            }
            if (all)
                return STATUS_SUCCESS;
        }
        else
        {
            for (size_t i = 0; i < count; i++)
            {
                if (objects[i]->Wait(0) == STATUS_SUCCESS)
                    return STATUS_WAIT_0 + uint32_t(i);
            }
        }

        if (timeoutMs == 0)
            return STATUS_TIMEOUT;

        std::unique_lock lock(g_waitMutex);
        auto changed = [&] { return g_waitGeneration != generation; };
        if (timeoutMs == INFINITE)
        {
            // Bounded sleep as a safety net for signals that bypass NotifyKernelWaiters.
            g_waitCondition.wait_for(lock, std::chrono::milliseconds(10), changed);
        }
        else if (!g_waitCondition.wait_until(lock, deadline, changed))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                lock.unlock();
                // One last look before reporting the timeout.
                for (size_t i = 0; i < count; i++)
                {
                    if (waitAll ? (acquired[i] || objects[i]->Wait(0) == STATUS_SUCCESS) : objects[i]->Wait(0) == STATUS_SUCCESS)
                    {
                        if (!waitAll)
                            return STATUS_WAIT_0 + uint32_t(i);
                        acquired[i] = true;
                    }
                }
                if (waitAll && std::all_of(acquired.begin(), acquired.end(), [](bool v) { return v; }))
                    return STATUS_SUCCESS;
                return STATUS_TIMEOUT;
            }
        }
    }
}

uint64_t PPCQueryTimeBase()
{
    static const auto start = std::chrono::steady_clock::now();
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    // ns * 49.875e6 / 1e9 without overflow: 49.875 MHz = 399 / 8 per microsecond.
    return (ns / 1000) * 399 / 8 + (ns % 1000) * 399 / 8000;
}
