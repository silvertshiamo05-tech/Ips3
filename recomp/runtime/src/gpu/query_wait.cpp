// Ips3: Bayonetta asks the GPU how many pixels of a test draw were visible (Direct3D occlusion
// queries) and then spins on IDirect3DQuery9::GetData until the GPU answers:
//
//     while (GetData(query, &result, 4, D3DGETDATA_FLUSH) == S_FALSE) {}   // sub_82257028
//
// On the console the answer was there almost at once. Here it waits for Xenia's GPU command
// thread to reach the query, and the spin keeps one host core fully busy doing nothing -- on an
// iPad, one of only two fast cores, which the GPU command thread and the driver need. While the
// answer is not ready, this gives the core away: a yield, then short sleeps.
#include <stdafx.h>

#include <chrono>
#include <thread>

// The recompiled GetData (absent from the CI build without game code, hence weak).
extern "C" PPC_FUNC(__imp__sub_822254E0) __attribute__((weak));

PPC_FUNC(sub_822254E0)
{
	__imp__sub_822254E0(ctx, base);

	thread_local uint32_t notReady = 0;
	if (ctx.r3.u32 != 1) // S_OK, or an error: the wait is over
	{
		notReady = 0;
		return;
	}
	if (++notReady < 32)
		std::this_thread::yield();
	else
		std::this_thread::sleep_for(std::chrono::microseconds(100));
}
