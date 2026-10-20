// Ips3: title module, kernel data exports and the XEX loader functions.
//
// XenonRecomp turns every function import into a host call but leaves data imports alone: their
// slots in the image still hold the import record. The real loader writes the address of the
// kernel variable into each slot instead, so this does the same with storage in guest memory.
#include <stdafx.h>

#include "module.h"

#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/xdm.h>
#include <image.h>
#include <xex.h>

namespace
{
	// The kernel's data exports by ordinal, from the same tables the recompiler names imports with.
	enum ExportType { kFunction, kVariable };

#define XE_EXPORT(MODULE, ORDINAL, NAME, TYPE) { (ORDINAL), { #NAME, (TYPE) == kVariable } }
	const std::unordered_map<uint32_t, std::pair<const char*, bool>> KernelExports =
	{
		#include <xbox/xboxkrnl_table.inc>
	};

	const std::unordered_map<uint32_t, std::pair<const char*, bool>> XamExportTable =
	{
		#include <xbox/xam_table.inc>
	};
#undef XE_EXPORT
}

namespace
{
	uint32_t g_xexHeader;
	uint32_t g_ldrEntry;
	uint32_t g_timeStampBundle;

	// Zeroed storage in guest memory that lives for the whole run.
	uint32_t AllocGuest(size_t size)
	{
		void* ptr = g_userHeap.AllocPhysical(size, 16);
		memset(ptr, 0, size);
		return g_memory.MapVirtual(ptr);
	}

	template<typename T>
	T* Guest(uint32_t address)
	{
		return reinterpret_cast<T*>(g_memory.Translate(address));
	}

	constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;

	// KeTimeStampBundle: interrupt time and system time in 100 ns units, tick count in ms.
	struct XTIME_STAMP_BUNDLE
	{
		be<uint64_t> InterruptTime;
		be<uint64_t> SystemTime;
		be<uint32_t> TickCount;
		be<uint32_t> Padding;
	};

	void UpdateTimeStampBundle()
	{
		static const auto start = std::chrono::steady_clock::now();

		const auto sinceBoot = std::chrono::steady_clock::now() - start;
		const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
		using Ticks = std::chrono::duration<int64_t, std::ratio<1, 10000000>>;

		auto* bundle = Guest<XTIME_STAMP_BUNDLE>(g_timeStampBundle);
		bundle->InterruptTime = std::chrono::duration_cast<Ticks>(sinceBoot).count();
		bundle->SystemTime = std::chrono::duration_cast<Ticks>(sinceEpoch).count() + FILETIME_EPOCH_DIFFERENCE;
		bundle->TickCount = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(sinceBoot).count());
	}

	uint32_t CreateString(std::string_view text, size_t capacity)
	{
		const uint32_t address = AllocGuest(std::max(capacity, text.size() + 1));
		memcpy(Guest<char>(address), text.data(), text.size());
		return address;
	}

	// Returns the guest address the import slot for `name` should hold.
	uint32_t CreateVariable(std::string_view name)
	{
		if (name == "XexExecutableModuleHandle")
		{
			const uint32_t cell = AllocGuest(4);
			*Guest<be<uint32_t>>(cell) = g_ldrEntry;
			return cell;
		}

		if (name == "KeTimeStampBundle")
			return g_timeStampBundle;

		if (name == "XboxHardwareInfo")
		{
			// Flags, then the number of hardware threads.
			const uint32_t info = AllocGuest(16);
			*Guest<be<uint32_t>>(info) = 0x20;
			*Guest<uint8_t>(info + 4) = 6;
			return info;
		}

		if (name == "XboxKrnlVersion" || name == "XboxKrnlBaseVersion")
		{
			const uint32_t version = AllocGuest(8);
			auto* fields = Guest<be<uint16_t>>(version);
			fields[0] = 2;
			fields[1] = 0;
			fields[2] = 17559;
			fields[3] = 0;
			return version;
		}

		if (name == "ExLoadedCommandLine")
			return CreateString("\"default.xex\"", 1024);

		if (name == "ExLoadedImageName")
			return CreateString("\\Device\\Cdrom0\\default.xex", 256);

		if (name == "VdGpuClockInMHz")
		{
			const uint32_t clock = AllocGuest(4);
			*Guest<be<uint32_t>>(clock) = 500;
			return clock;
		}

		if (name == "ExConsoleGameRegion")
		{
			const uint32_t region = AllocGuest(4);
			*Guest<be<uint32_t>>(region) = 0xFFFFFFFF;
			return region;
		}

		// Object types, monitor data, locks: zeroed storage is a valid empty object.
		return AllocGuest(256);
	}

	void BindVariableImports(const uint8_t* xex)
	{
		auto* imports = reinterpret_cast<const Xex2ImportHeader*>(getOptHeaderPtr(xex, XEX_HEADER_IMPORT_LIBRARIES));
		if (imports == nullptr)
			return;

		std::vector<std::string_view> libraryNames;
		const char* strings = reinterpret_cast<const char*>(imports + 1);
		size_t offset = 0;
		for (size_t i = 0; i < imports->numImports; i++)
		{
			libraryNames.emplace_back(strings + offset);
			offset += ((libraryNames.back().size() + 1) + 3) & ~3;
		}

		auto* library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const uint8_t*>(imports) + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
		for (const auto& libraryName : libraryNames)
		{
			const auto* exports = libraryName == "xboxkrnl.exe" ? &KernelExports : libraryName == "xam.xex" ? &XamExportTable : nullptr;
			auto* descriptors = reinterpret_cast<const Xex2ImportDescriptor*>(library + 1);

			for (size_t i = 0; exports != nullptr && i < library->numberOfImports; i++)
			{
				const uint32_t slot = descriptors[i].firstThunk;

				// The image loader replaced function thunks with code and left record slots
				// holding the import record, byte-swapped to host order: type in the top byte,
				// ordinal in the low half. Only records naming a data export get bound.
				const uint32_t record = *Guest<uint32_t>(slot);
				if ((record >> 24) != 0)
					continue;

				auto it = exports->find(record & 0xFFFF);
				if (it == exports->end() || !it->second.second)
					continue;

				const uint32_t address = CreateVariable(it->second.first);
				*Guest<be<uint32_t>>(slot) = address;
				fmt::println(stderr, "[module] {} -> {:08X}", it->second.first, address);
			}

			library = reinterpret_cast<const Xex2ImportLibrary*>(reinterpret_cast<const uint8_t*>(library + 1) + library->numberOfImports * sizeof(Xex2ImportDescriptor));
		}
	}
}

void Module::Init(const uint8_t* xex, size_t xexSize, const Image& image)
{
	auto* header = reinterpret_cast<const Xex2Header*>(xex);
	const size_t headerSize = std::min<size_t>(header->headerSize, xexSize);

	g_xexHeader = AllocGuest(headerSize);
	memcpy(Guest<uint8_t>(g_xexHeader), xex, headerSize);

	// LDR_DATA_TABLE_ENTRY: DllBase at 0x18, EntryPoint at 0x1C, SizeOfImage at 0x20, the XEX
	// header at 0x58.
	g_ldrEntry = AllocGuest(0x100);
	*Guest<be<uint32_t>>(g_ldrEntry + 0x18) = uint32_t(image.base);
	*Guest<be<uint32_t>>(g_ldrEntry + 0x1C) = uint32_t(image.entry_point);
	*Guest<be<uint32_t>>(g_ldrEntry + 0x20) = uint32_t(image.size);
	*Guest<be<uint32_t>>(g_ldrEntry + 0x58) = g_xexHeader;

	g_timeStampBundle = AllocGuest(sizeof(XTIME_STAMP_BUNDLE));
	UpdateTimeStampBundle();
	std::thread([]
	{
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			UpdateTimeStampBundle();
		}
	}).detach();

	BindVariableImports(xex);
}

uint32_t Module::Handle()
{
	return g_ldrEntry;
}

uint32_t Module::XexHeader()
{
	return g_xexHeader;
}

// void* RtlImageXexHeaderField(void* xexHeader, uint32_t key)
// Inline values (key low byte 0) are returned as is, single-word values (low byte 1) by the
// address of the word, everything else by the address of its data.
uint32_t RtlImageXexHeaderField(uint32_t xexHeader, uint32_t key)
{
	if (xexHeader == 0)
		xexHeader = g_xexHeader;

	auto* header = Guest<Xex2Header>(xexHeader);
	auto* optHeaders = reinterpret_cast<Xex2OptHeader*>(header + 1);

	for (uint32_t i = 0; i < header->headerCount; i++)
	{
		if (optHeaders[i].key != key)
			continue;

		switch (key & 0xFF)
		{
		case 0x00:
			return optHeaders[i].value;
		case 0x01:
			return g_memory.MapVirtual(&optHeaders[i].value);
		default:
			return xexHeader + optHeaders[i].offset;
		}
	}

	return 0;
}

// NTSTATUS XexGetModuleHandle(const char* name, HANDLE* handle)
uint32_t XexGetModuleHandle(const char* name, be<uint32_t>* handle)
{
	if (name == nullptr || strcasecmp(name, "default.xex") == 0)
	{
		*handle = g_ldrEntry;
		return STATUS_SUCCESS;
	}

	LOGF_UTILITY("{}", name);
	*handle = 0;
	return 0xC0000135; // STATUS_DLL_NOT_FOUND
}

// NTSTATUS XexGetProcedureAddress(HANDLE module, uint32_t ordinal, void** address)
uint32_t XexGetProcedureAddress(uint32_t module, uint32_t ordinal, be<uint32_t>* address)
{
	LOGF_UTILITY("module {:08X} ordinal {}", module, ordinal);
	*address = 0;
	return 0xC000007A; // STATUS_PROCEDURE_NOT_FOUND
}

GUEST_FUNCTION_HOOK(__imp__RtlImageXexHeaderField, RtlImageXexHeaderField);
GUEST_FUNCTION_HOOK(__imp__XexGetModuleHandle, XexGetModuleHandle);
GUEST_FUNCTION_HOOK(__imp__XexGetProcedureAddress, XexGetProcedureAddress);
