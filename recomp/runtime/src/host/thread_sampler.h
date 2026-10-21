// Ips3: a sampling profiler for the status lines. A helper thread interrupts the named threads
// ("guest 0", "GPU Commands") about 200 times a second and notes which functions they are in,
// so a log from a phone or tablet shows where their time goes without a debugger.
#pragma once

#include <string>

namespace host
{
	// Starts sampling (once; later calls do nothing).
	void StartThreadSampler();

	// The most frequent places since the last call, one line per sampled thread:
	// "guest 0: 41% sub_822254E0 < sub_82257028, 20% ...".
	std::string ThreadSamplerReport();
}
