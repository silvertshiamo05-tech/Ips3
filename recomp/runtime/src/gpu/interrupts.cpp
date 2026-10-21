// Ips3: runs the game's graphics interrupt handler for Xenia's GPU (vblank and PM4 INTERRUPT).
#include <stdafx.h>

#include <cpu/guest_thread.h>
#include <ips3_gpu.h>
#include <kernel/memory.h>

namespace
{
	std::mutex g_interruptMutex;
	std::atomic<uint64_t> g_interruptCounts[2];
}

void ips3_interrupt_counts(uint64_t* vblank, uint64_t* command_processor)
{
	*vblank = g_interruptCounts[0];
	*command_processor = g_interruptCounts[1];
}

// Each host thread that raises interrupts gets its own guest stack; handlers run one at a time,
// as on the console.
void ips3_dispatch_gpu_interrupt(uint32_t callback, uint32_t data, uint32_t source, uint32_t cpu)
{
	thread_local std::unique_ptr<GuestThreadContext> context;
	if (!context)
		context = std::make_unique<GuestThreadContext>(cpu);

	g_interruptCounts[source & 1]++;
	std::lock_guard lock(g_interruptMutex);
	PPCContext& ctx = context->ppcContext;
	ctx.r3.u64 = source;
	ctx.r4.u64 = data;
	g_memory.FindFunction(callback)(ctx, g_memory.base);
}
