// Ips3: how much CPU time each of the process's threads used, for the status lines: shows
// whether the game's code, the GPU command thread or nothing at all (everything waiting) is
// what holds the frame rate back.
#pragma once

#include <string>

namespace host
{
	// "name 97%, name 40%, ..." for the busiest threads since the last call (busier than 3%).
	std::string ThreadUsage();
}
