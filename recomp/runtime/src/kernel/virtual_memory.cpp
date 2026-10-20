// Ips3: NtAllocateVirtualMemory and friends, kernel pool, memory statistics.
//
// Guest memory is one host reservation that the OS commits lazily on first touch, so "commit"
// costs nothing here. This only has to hand out non-overlapping ranges, remember them for
// NtQueryVirtualMemory, and give zeroed pages back after a decommit or release.
#include <stdafx.h>

#include <map>

#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/xdm.h>

#ifndef _WIN32
#include <sys/mman.h>
#endif

#define MEM_DECOMMIT   0x00004000
#define MEM_RELEASE    0x00008000
#define MEM_FREE       0x00010000
#define MEM_PRIVATE    0x00020000
#define MEM_IMAGE      0x01000000
#define MEM_TOP_DOWN   0x00100000
#define MEM_LARGE_PAGES 0x20000000
#define MEM_16MB_PAGES 0x80000000

#define STATUS_INVALID_PARAMETER      0xC000000D
#define STATUS_NO_MEMORY              0xC0000017
#define STATUS_CONFLICTING_ADDRESSES  0xC0000018
#define STATUS_MEMORY_NOT_ALLOCATED   0xC00000A0

// Shared with Heap::Init: the user heap ends where the virtual allocation range starts.
extern const uint32_t VIRTUAL_MEMORY_BEGIN = 0x40000000;
// Ends below the GPU register window at 0x7FC80000.
extern const uint32_t VIRTUAL_MEMORY_END = 0x7F000000;

namespace
{
	constexpr uint32_t PAGE_SIZE_4K = 0x1000;
	constexpr uint32_t GRANULARITY = 0x10000; // allocation bases, as on Windows

	// What the title sees as installed memory: 512 MiB in 4 KiB pages.
	constexpr uint32_t TOTAL_PHYSICAL_PAGES = 0x20000;

	struct Region
	{
		uint32_t size;
		uint32_t protect;
		bool committed;
	};

	std::mutex g_mutex;
	std::map<uint32_t, Region> g_regions; // by base address
	uint64_t g_bytesInUse;

	uint32_t AlignUp(uint32_t value, uint32_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

	// Gives guest pages back to the host so they read as zero when reused.
	void ResetPages(uint32_t address, uint32_t size)
	{
		uint8_t* host = static_cast<uint8_t*>(g_memory.Translate(address));

#ifdef _WIN32
		memset(host, 0, size);
#else
		const uintptr_t hostPage = uintptr_t(sysconf(_SC_PAGESIZE));
		const uintptr_t begin = (uintptr_t(host) + hostPage - 1) & ~(hostPage - 1);
		const uintptr_t end = (uintptr_t(host) + size) & ~(hostPage - 1);

		if (begin >= end)
		{
			memset(host, 0, size);
			return;
		}

		memset(host, 0, begin - uintptr_t(host));
		memset(reinterpret_cast<void*>(end), 0, uintptr_t(host) + size - end);
		mmap(reinterpret_cast<void*>(begin), end - begin, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
#endif
	}

	// The region containing `address`, or end().
	std::map<uint32_t, Region>::iterator FindRegion(uint32_t address)
	{
		auto it = g_regions.upper_bound(address);
		if (it == g_regions.begin())
			return g_regions.end();

		--it;
		return address < it->first + it->second.size ? it : g_regions.end();
	}

	bool RangeIsFree(uint32_t base, uint32_t size)
	{
		if (base < VIRTUAL_MEMORY_BEGIN || uint64_t(base) + size > VIRTUAL_MEMORY_END)
			return false;

		auto next = g_regions.lower_bound(base);
		if (next != g_regions.end() && next->first < base + size)
			return false;

		if (next != g_regions.begin())
		{
			auto prev = std::prev(next);
			if (prev->first + prev->second.size > base)
				return false;
		}

		return true;
	}

	uint32_t FindFreeRange(uint32_t size, uint32_t alignment, bool topDown)
	{
		// Candidate bases are the range edges and the edges of existing regions.
		std::vector<uint32_t> candidates;
		if (topDown)
		{
			candidates.push_back((VIRTUAL_MEMORY_END - size) & ~(alignment - 1));
			for (auto it = g_regions.rbegin(); it != g_regions.rend(); ++it)
			{
				if (it->first >= VIRTUAL_MEMORY_BEGIN + size)
					candidates.push_back((it->first - size) & ~(alignment - 1));
			}
		}
		else
		{
			candidates.push_back(AlignUp(VIRTUAL_MEMORY_BEGIN, alignment));
			for (auto& [base, region] : g_regions)
				candidates.push_back(AlignUp(base + region.size, alignment));
		}

		for (uint32_t candidate : candidates)
		{
			if (RangeIsFree(candidate, size))
				return candidate;
		}

		return 0;
	}
}

// NTSTATUS NtAllocateVirtualMemory(void** baseAddress, size_t* regionSize, uint32_t allocationType,
//                                  uint32_t protect, uint32_t debugMemory)
uint32_t NtAllocateVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t allocationType, uint32_t protect, uint32_t debugMemory)
{
	if (baseAddress == nullptr || regionSize == nullptr || *regionSize == 0)
		return STATUS_INVALID_PARAMETER;

	const uint32_t pageSize = (allocationType & MEM_16MB_PAGES) ? 0x1000000 : (allocationType & MEM_LARGE_PAGES) ? GRANULARITY : PAGE_SIZE_4K;
	const uint32_t requested = *baseAddress;
	const uint32_t size = AlignUp(*regionSize, pageSize);

	std::lock_guard lock(g_mutex);

	if (requested != 0)
	{
		// Committing inside a range reserved earlier.
		auto it = FindRegion(requested);
		if (it != g_regions.end())
		{
			if (requested + size > it->first + it->second.size)
				return STATUS_CONFLICTING_ADDRESSES;

			it->second.committed |= (allocationType & MEM_COMMIT) != 0;
			*baseAddress = requested & ~(PAGE_SIZE_4K - 1);
			*regionSize = size;
			return STATUS_SUCCESS;
		}

		if (!(allocationType & MEM_RESERVE))
			return STATUS_MEMORY_NOT_ALLOCATED;

		const uint32_t base = requested & ~(GRANULARITY - 1);
		const uint32_t total = AlignUp(requested + size - base, GRANULARITY);
		if (!RangeIsFree(base, total))
			return STATUS_CONFLICTING_ADDRESSES;

		g_regions[base] = { total, protect, (allocationType & MEM_COMMIT) != 0 };
		g_bytesInUse += total;
		*baseAddress = base;
		*regionSize = size;
		return STATUS_SUCCESS;
	}

	const uint32_t total = AlignUp(size, GRANULARITY);
	const uint32_t base = FindFreeRange(total, std::max(pageSize, GRANULARITY), (allocationType & MEM_TOP_DOWN) != 0);
	if (base == 0)
	{
		fmt::println(stderr, "[memory] NtAllocateVirtualMemory: no room for 0x{:X} bytes", size);
		return STATUS_NO_MEMORY;
	}

	g_regions[base] = { total, protect, (allocationType & MEM_COMMIT) != 0 };
	g_bytesInUse += total;
	*baseAddress = base;
	*regionSize = size;
	return STATUS_SUCCESS;
}

// NTSTATUS NtFreeVirtualMemory(void** baseAddress, size_t* regionSize, uint32_t freeType, uint32_t debugMemory)
uint32_t NtFreeVirtualMemory(be<uint32_t>* baseAddress, be<uint32_t>* regionSize, uint32_t freeType, uint32_t debugMemory)
{
	if (baseAddress == nullptr)
		return STATUS_INVALID_PARAMETER;

	std::lock_guard lock(g_mutex);

	auto it = FindRegion(*baseAddress);
	if (it == g_regions.end())
		return STATUS_MEMORY_NOT_ALLOCATED;

	if (freeType & MEM_RELEASE)
	{
		const uint32_t base = it->first;
		const uint32_t size = it->second.size;
		g_bytesInUse -= size;
		g_regions.erase(it);
		ResetPages(base, size);

		*baseAddress = base;
		if (regionSize != nullptr)
			*regionSize = size;
		return STATUS_SUCCESS;
	}

	if (freeType & MEM_DECOMMIT)
	{
		const uint32_t base = *baseAddress & ~(PAGE_SIZE_4K - 1);
		uint32_t size = regionSize != nullptr && *regionSize != 0 ? AlignUp(*regionSize, PAGE_SIZE_4K) : it->first + it->second.size - base;
		size = std::min(size, it->first + it->second.size - base);
		ResetPages(base, size);

		*baseAddress = base;
		if (regionSize != nullptr)
			*regionSize = size;
		return STATUS_SUCCESS;
	}

	return STATUS_INVALID_PARAMETER;
}

struct XMEMORY_BASIC_INFORMATION
{
	be<uint32_t> BaseAddress;
	be<uint32_t> AllocationBase;
	be<uint32_t> AllocationProtect;
	be<uint32_t> RegionSize;
	be<uint32_t> State;
	be<uint32_t> Protect;
	be<uint32_t> Type;
};

// NTSTATUS NtQueryVirtualMemory(void* address, MEMORY_BASIC_INFORMATION* info, uint32_t debugMemory)
uint32_t NtQueryVirtualMemory(uint32_t address, XMEMORY_BASIC_INFORMATION* info, uint32_t debugMemory)
{
	if (info == nullptr)
		return STATUS_INVALID_PARAMETER;

	const uint32_t page = address & ~(PAGE_SIZE_4K - 1);
	memset(info, 0, sizeof(*info));
	info->BaseAddress = page;

	if (address >= PPC_IMAGE_BASE && address < PPC_IMAGE_BASE + PPC_IMAGE_SIZE)
	{
		info->AllocationBase = uint32_t(PPC_IMAGE_BASE);
		info->AllocationProtect = 0x04; // PAGE_READWRITE
		info->RegionSize = uint32_t(PPC_IMAGE_BASE + PPC_IMAGE_SIZE) - page;
		info->State = MEM_COMMIT;
		info->Protect = 0x04;
		info->Type = MEM_IMAGE;
		return STATUS_SUCCESS;
	}

	std::lock_guard lock(g_mutex);

	auto it = FindRegion(address);
	if (it != g_regions.end())
	{
		info->AllocationBase = it->first;
		info->AllocationProtect = it->second.protect;
		info->RegionSize = it->first + it->second.size - page;
		info->State = it->second.committed ? MEM_COMMIT : MEM_RESERVE;
		info->Protect = it->second.protect;
		info->Type = MEM_PRIVATE;
		return STATUS_SUCCESS;
	}

	auto next = g_regions.upper_bound(address);
	const uint32_t end = next != g_regions.end() ? next->first : (address < VIRTUAL_MEMORY_END ? VIRTUAL_MEMORY_END : 0);
	info->RegionSize = end > page ? end - page : PAGE_SIZE_4K;
	info->State = MEM_FREE;
	info->Protect = 0x01; // PAGE_NOACCESS
	return STATUS_SUCCESS;
}

struct XMM_STATISTICS_SECTION
{
	be<uint32_t> AvailablePages;
	be<uint32_t> TotalVirtualMemoryBytes;
	be<uint32_t> ReservedVirtualMemoryBytes;
	be<uint32_t> PhysicalPages;
	be<uint32_t> PoolPages;
	be<uint32_t> StackPages;
	be<uint32_t> ImagePages;
	be<uint32_t> HeapPages;
	be<uint32_t> VirtualPages;
	be<uint32_t> PageTablePages;
	be<uint32_t> CachePages;
};

struct XMM_STATISTICS
{
	be<uint32_t> Length;
	be<uint32_t> TotalPhysicalPages;
	be<uint32_t> KernelPages;
	XMM_STATISTICS_SECTION Title;
	XMM_STATISTICS_SECTION System;
	be<uint32_t> HighestPhysicalPage;
};
static_assert(sizeof(XMM_STATISTICS) == 104);

// NTSTATUS MmQueryStatistics(MM_STATISTICS* stats)
uint32_t MmQueryStatistics(XMM_STATISTICS* stats)
{
	if (stats == nullptr || stats->Length != sizeof(XMM_STATISTICS))
		return STATUS_INVALID_PARAMETER;

	uint32_t usedPages;
	{
		std::lock_guard lock(g_mutex);
		usedPages = uint32_t(g_bytesInUse / PAGE_SIZE_4K);
	}

	constexpr uint32_t kernelPages = 0x1000; // 16 MiB kept by the system
	const uint32_t imagePages = uint32_t(PPC_IMAGE_SIZE / PAGE_SIZE_4K);

	memset(stats, 0, sizeof(*stats));
	stats->Length = sizeof(XMM_STATISTICS);
	stats->TotalPhysicalPages = TOTAL_PHYSICAL_PAGES;
	stats->KernelPages = kernelPages;
	stats->Title.AvailablePages = TOTAL_PHYSICAL_PAGES - std::min(TOTAL_PHYSICAL_PAGES - kernelPages, kernelPages + imagePages + usedPages);
	stats->Title.TotalVirtualMemoryBytes = VIRTUAL_MEMORY_END - VIRTUAL_MEMORY_BEGIN;
	stats->Title.ReservedVirtualMemoryBytes = uint32_t(std::min<uint64_t>(g_bytesInUse, 0xFFFFFFFF));
	stats->Title.ImagePages = imagePages;
	stats->Title.VirtualPages = usedPages;
	stats->HighestPhysicalPage = TOTAL_PHYSICAL_PAGES - 1;
	return STATUS_SUCCESS;
}

// void* ExAllocatePoolTypeWithTag(size_t size, uint32_t tag, uint32_t poolType)
uint32_t ExAllocatePoolTypeWithTag(uint32_t size, uint32_t tag, uint32_t poolType)
{
	void* ptr = g_userHeap.AllocPhysical(size, 16);
	return ptr ? g_memory.MapVirtual(ptr) : 0;
}

// void ExFreePool(void* ptr)
void ExFreePool(uint32_t address)
{
	if (address != 0)
		g_userHeap.Free(g_memory.Translate(address));
}

GUEST_FUNCTION_HOOK(__imp__NtAllocateVirtualMemory, NtAllocateVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtFreeVirtualMemory, NtFreeVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__NtQueryVirtualMemory, NtQueryVirtualMemory);
GUEST_FUNCTION_HOOK(__imp__MmQueryStatistics, MmQueryStatistics);
GUEST_FUNCTION_HOOK(__imp__ExAllocatePoolTypeWithTag, ExAllocatePoolTypeWithTag);
GUEST_FUNCTION_HOOK(__imp__ExFreePool, ExFreePool);
