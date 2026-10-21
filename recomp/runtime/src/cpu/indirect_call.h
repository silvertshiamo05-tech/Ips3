// Ips3: force-included into the recompiled game.
//
// An indirect call (bctrl/bctr) whose target has
// no recompiled function would otherwise jump to address 0; this names the guest address and
// the caller instead, which is what the recompiler configuration needs to be fixed.
#pragma once

#include <cstdint>

// mftb: the console's time base runs at 49.875 MHz, the rate KeQueryPerformanceFrequency
// reports (the host TSC runs ~50x faster and does not exist on ARM).
uint64_t PPCQueryTimeBase();
#define PPC_QUERY_TIMEBASE() PPCQueryTimeBase()

[[noreturn]] void PPCUnresolvedCall(uint32_t target, uint32_t lr);

#define PPC_CALL_INDIRECT_FUNC(x)                                                       \
    do                                                                                  \
    {                                                                                   \
        PPCFunc* ips3Target_ = PPC_LOOKUP_FUNC(base, x);                                \
        if (__builtin_expect(ips3Target_ == nullptr, 0))                                \
            PPCUnresolvedCall(uint32_t(x), uint32_t(ctx.lr));                           \
        ips3Target_(ctx, base);                                                         \
    } while (0)
