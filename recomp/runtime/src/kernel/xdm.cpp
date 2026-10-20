#include <stdafx.h>
#include "xdm.h"
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
