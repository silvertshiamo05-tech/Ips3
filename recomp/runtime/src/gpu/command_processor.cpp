// Ips3: Xenos command processor (headless). Packet formats and register numbers follow the
// Xenos/R500 PM4 conventions as documented by Xenia.
#include <stdafx.h>

#include "command_processor.h"

#include <cpu/guest_thread.h>
#include <kernel/memory.h>

namespace
{
	// Register indices (dword offsets into the register window).
	enum : uint32_t
	{
		CP_RB_RPTR = 0x01C4,
		CP_RB_WPTR = 0x01C5,
		SCRATCH_UMSK = 0x01DC,
		SCRATCH_ADDR = 0x01DD,
		SCRATCH_REG0 = 0x0578,
		SCRATCH_REG7 = 0x057F,
		COHER_STATUS_HOST = 0x0A2F,
		RB_EDRAM_TIMING = 0x0F00,
		RB_BC_CONTROL = 0x0F01,
		D1MODE_V_COUNTER = 0x194C,
		D1MODE_INTERRUPT_STATUS = 0x1951,
		D1MODE_VIEWPORT_SIZE = 0x1961,
		REGISTER_COUNT = Gpu::REGISTER_WINDOW / 4,
	};

	// Type 3 opcodes.
	enum : uint32_t
	{
		PM4_ME_INIT = 0x48,
		PM4_NOP = 0x10,
		PM4_INTERRUPT = 0x54,
		PM4_WAIT_FOR_IDLE = 0x26,
		PM4_WAIT_REG_MEM = 0x3C,
		PM4_REG_RMW = 0x21,
		PM4_REG_TO_MEM = 0x3E,
		PM4_MEM_WRITE = 0x3D,
		PM4_COND_WRITE = 0x45,
		PM4_EVENT_WRITE = 0x46,
		PM4_EVENT_WRITE_SHD = 0x58,
		PM4_EVENT_WRITE_EXT = 0x59,
		PM4_EVENT_WRITE_ZPD = 0x5B,
		PM4_DRAW_INDX = 0x22,
		PM4_DRAW_INDX_2 = 0x36,
		PM4_SET_CONSTANT = 0x2D,
		PM4_SET_CONSTANT2 = 0x55,
		PM4_LOAD_ALU_CONSTANT = 0x2F,
		PM4_SET_SHADER_CONSTANTS = 0x56,
		PM4_IM_LOAD = 0x27,
		PM4_IM_LOAD_IMMEDIATE = 0x2B,
		PM4_INDIRECT_BUFFER = 0x3F,
		PM4_INDIRECT_BUFFER_PFD = 0x37,
		PM4_XE_SWAP = 0x64, // written by our VdSwap
	};

	uint8_t* Host(uint32_t guest)
	{
		return static_cast<uint8_t*>(g_memory.Translate(guest));
	}

	uint32_t LoadBE(uint32_t guest)
	{
		return ByteSwap(*reinterpret_cast<uint32_t*>(Host(guest)));
	}

	void StoreBE(uint32_t guest, uint32_t value)
	{
		*reinterpret_cast<uint32_t*>(Host(guest)) = ByteSwap(value);
	}

	// The register file is the register window itself, so CPU-side reads and writes and the
	// command processor see the same values.
	uint32_t ReadRegister(uint32_t index)
	{
		return index < REGISTER_COUNT ? LoadBE(Gpu::REGISTER_BASE + index * 4) : 0;
	}

	// Applies a GPU endian mode (the low two bits of a GPU address) to a value bound for memory.
	uint32_t GpuSwap(uint32_t value, uint32_t endian)
	{
		switch (endian & 3)
		{
		case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
		case 2: return ByteSwap(value);
		case 3: return (value >> 16) | (value << 16);
		default: return value;
		}
	}

	// A GPU memory write: the address carries the endian mode in its low bits.
	void WriteMemory(uint32_t gpuAddress, uint32_t value)
	{
		*reinterpret_cast<uint32_t*>(Host(Gpu::Translate(gpuAddress & ~3u))) = GpuSwap(value, gpuAddress & 3);
	}

	uint32_t ReadMemory(uint32_t gpuAddress)
	{
		return GpuSwap(*reinterpret_cast<uint32_t*>(Host(Gpu::Translate(gpuAddress & ~3u))), gpuAddress & 3);
	}

	void WriteRegister(uint32_t index, uint32_t value)
	{
		if (index >= REGISTER_COUNT)
			return;

		if (index == COHER_STATUS_HOST)
			value &= ~0x80000000u; // caches are always coherent here: report done at once

		StoreBE(Gpu::REGISTER_BASE + index * 4, value);

		if (index >= SCRATCH_REG0 && index <= SCRATCH_REG7)
		{
			const uint32_t slot = index - SCRATCH_REG0;
			if (ReadRegister(SCRATCH_UMSK) & (1u << slot))
				StoreBE(Gpu::Translate(ReadRegister(SCRATCH_ADDR) + slot * 4), value);
		}
	}

	struct State
	{
		std::mutex mutex;
		bool started = false;
		uint32_t ringBase = 0;     // guest address
		uint32_t ringDwords = 0;
		uint32_t readPointer = 0;  // dword index
		uint32_t writeBack = 0;    // guest address of the read pointer copy, or 0

		std::atomic<uint32_t> interruptCallback{ 0 };
		std::atomic<uint32_t> interruptData{ 0 };
		std::mutex interruptMutex;

		std::atomic<uint64_t> swaps{ 0 };
		uint64_t packets = 0;
		uint64_t draws = 0;
	};

	State g_state;

	// Runs the game's graphics interrupt handler on the calling host thread. Each host thread
	// gets its own guest stack; handlers run one at a time, as on the console.
	void DispatchInterrupt(uint32_t source, uint32_t cpu)
	{
		const uint32_t callback = g_state.interruptCallback;
		if (callback == 0)
			return;

		thread_local std::unique_ptr<GuestThreadContext> context;
		if (!context)
			context = std::make_unique<GuestThreadContext>(cpu);

		std::lock_guard lock(g_state.interruptMutex);
		PPCContext& ctx = context->ppcContext;
		ctx.r3.u64 = source;
		ctx.r4.u64 = g_state.interruptData;
		g_memory.FindFunction(callback)(ctx, g_memory.base);
	}

	// Reads dwords from the ring (wrapping) or from an indirect buffer (linear).
	struct Reader
	{
		uint32_t base;  // guest address
		uint32_t size;  // dwords; 0 = linear
		uint32_t pos;   // dword index
		uint32_t end;   // dword index to stop at

		bool Done() const { return pos == end; }
		uint32_t Remaining() const { return size ? (end + size - pos) % size : end - pos; }

		uint32_t Read()
		{
			const uint32_t value = LoadBE(base + pos * 4);
			pos = size ? (pos + 1) % size : pos + 1;
			return value;
		}

		void Skip(uint32_t count)
		{
			pos = size ? (pos + count) % size : pos + count;
		}
	};

	bool Compare(uint32_t function, uint32_t value, uint32_t reference)
	{
		switch (function & 7)
		{
		case 0: return false;
		case 1: return value < reference;
		case 2: return value <= reference;
		case 3: return value == reference;
		case 4: return value != reference;
		case 5: return value >= reference;
		case 6: return value > reference;
		default: return true;
		}
	}

	void Execute(Reader& reader, int depth);

	void ExecuteType3(Reader& reader, uint32_t packet, int depth)
	{
		const uint32_t opcode = (packet >> 8) & 0x7F;
		const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
		const uint32_t start = reader.pos;
		auto skipRest = [&]
		{
			const uint32_t used = reader.size ? (reader.pos + reader.size - start) % reader.size : reader.pos - start;
			if (used < count)
				reader.Skip(count - used);
		};

		switch (opcode)
		{
		case PM4_INDIRECT_BUFFER:
		case PM4_INDIRECT_BUFFER_PFD:
		{
			const uint32_t address = reader.Read();
			const uint32_t length = reader.Read() & 0xFFFFF;
			skipRest();
			if (depth < 4)
			{
				Reader sub{ Gpu::Translate(address), 0, 0, length };
				Execute(sub, depth + 1);
			}
			return;
		}
		case PM4_WAIT_REG_MEM:
		{
			const uint32_t info = reader.Read();
			const uint32_t address = reader.Read();
			const uint32_t reference = reader.Read();
			const uint32_t mask = reader.Read();
			const uint32_t wait = reader.Read();
			skipRest();

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			for (;;)
			{
				const uint32_t value = (info & 0x10) ? ReadMemory(address) : ReadRegister(address);
				if (Compare(info, value & mask, reference))
					break;
				if (std::chrono::steady_clock::now() > deadline)
				{
					fmt::println(stderr, "[gpu] WAIT_REG_MEM on {} {:08X} (== {:08X} under {:08X}) timed out; continuing",
						(info & 0x10) ? "memory" : "register", address, reference, mask);
					break;
				}
				if (wait >= 0x100)
					std::this_thread::sleep_for(std::chrono::microseconds(100));
				else
					std::this_thread::yield();
			}
			return;
		}
		case PM4_REG_RMW:
		{
			const uint32_t info = reader.Read();
			const uint32_t andMask = reader.Read();
			const uint32_t orMask = reader.Read();
			skipRest();
			uint32_t value = ReadRegister(info & 0x1FFF);
			value &= (info >> 31) ? ReadRegister(andMask & 0x1FFF) : andMask;
			value |= ((info >> 30) & 1) ? ReadRegister(orMask & 0x1FFF) : orMask;
			WriteRegister(info & 0x1FFF, value);
			return;
		}
		case PM4_REG_TO_MEM:
		{
			const uint32_t index = reader.Read();
			const uint32_t address = reader.Read();
			skipRest();
			WriteMemory(address, ReadRegister(index));
			return;
		}
		case PM4_MEM_WRITE:
		{
			const uint32_t address = reader.Read();
			for (uint32_t i = 1; i < count; i++)
				WriteMemory(address + i * 4 - 4, reader.Read());
			return;
		}
		case PM4_COND_WRITE:
		{
			const uint32_t info = reader.Read();
			const uint32_t address = reader.Read();
			const uint32_t reference = reader.Read();
			const uint32_t mask = reader.Read();
			const uint32_t target = reader.Read();
			const uint32_t data = reader.Read();
			skipRest();
			const uint32_t value = (info & 0x10) ? ReadMemory(address) : ReadRegister(address);
			if (Compare(info, value & mask, reference))
			{
				if (info & 0x100)
					WriteMemory(target, data);
				else
					WriteRegister(target, data);
			}
			return;
		}
		case PM4_EVENT_WRITE_SHD:
		{
			const uint32_t initiator = reader.Read();
			const uint32_t address = reader.Read();
			const uint32_t value = reader.Read();
			skipRest();
			// Bit 31 of the initiator asks for the swap counter instead of the given value.
			WriteMemory(address, (initiator >> 31) ? uint32_t(g_state.swaps.load()) : value);
			return;
		}
		case PM4_EVENT_WRITE_EXT:
		{
			reader.Read(); // initiator
			const uint32_t address = reader.Read();
			skipRest();
			// Screen extents: the whole render target, as six 16-bit values.
			const uint16_t extents[6] = { 0, 8192 >> 3, 0, 8192 >> 3, 0, 1 };
			uint8_t* out = Host(Gpu::Translate(address & ~3u));
			for (int i = 0; i < 6; i++)
				*reinterpret_cast<uint16_t*>(out + i * 2) = ByteSwap(extents[i]);
			return;
		}
		case PM4_INTERRUPT:
		{
			const uint32_t cpuMask = reader.Read();
			skipRest();
			for (uint32_t cpu = 0; cpu < 6; cpu++)
			{
				if (cpuMask & (1u << cpu))
					DispatchInterrupt(1, cpu);
			}
			return;
		}
		case PM4_SET_CONSTANT:
		case PM4_SET_CONSTANT2:
		case PM4_SET_SHADER_CONSTANTS:
		{
			const uint32_t offsetType = reader.Read();
			uint32_t index;
			if (opcode == PM4_SET_CONSTANT)
			{
				static constexpr uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
				const uint32_t type = (offsetType >> 16) & 0xFF;
				index = (type < 5 ? bases[type] : 0x2000) + (offsetType & 0x7FF);
			}
			else
			{
				index = offsetType & 0xFFFF;
			}
			for (uint32_t i = 1; i < count; i++)
				WriteRegister(index + i - 1, reader.Read());
			return;
		}
		case PM4_LOAD_ALU_CONSTANT:
		{
			const uint32_t address = reader.Read() & 0x3FFFFFFF;
			const uint32_t offsetType = reader.Read();
			const uint32_t dwords = reader.Read();
			skipRest();
			static constexpr uint32_t bases[] = { 0x4000, 0x4800, 0x4900, 0x4908, 0x2000 };
			const uint32_t type = (offsetType >> 16) & 0xFF;
			const uint32_t index = (type < 5 ? bases[type] : 0x2000) + (offsetType & 0x7FF);
			for (uint32_t i = 0; i < dwords; i++)
				WriteRegister(index + i, LoadBE(Gpu::Translate(address) + i * 4));
			return;
		}
		case PM4_DRAW_INDX:
		case PM4_DRAW_INDX_2:
			g_state.draws++;
			reader.Skip(count);
			return;
		case PM4_XE_SWAP:
			g_state.swaps++;
			reader.Skip(count);
			return;
		default:
			reader.Skip(count);
			return;
		}
	}

	void Execute(Reader& reader, int depth)
	{
		while (!reader.Done())
		{
			const uint32_t packet = reader.Read();
			g_state.packets++;

			switch (packet >> 30)
			{
			case 0:
			{
				const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
				const uint32_t base = packet & 0x7FFF;
				const bool oneRegister = (packet >> 15) & 1;
				for (uint32_t i = 0; i < count; i++)
					WriteRegister(oneRegister ? base : base + i, reader.Read());
				break;
			}
			case 1:
			{
				const uint32_t first = reader.Read();
				const uint32_t second = reader.Read();
				WriteRegister(packet & 0x7FF, first);
				WriteRegister((packet >> 11) & 0x7FF, second);
				break;
			}
			case 2:
				break; // type 2: no-op
			case 3:
				ExecuteType3(reader, packet, depth);
				break;
			}
		}
	}

	void CommandProcessorThread()
	{
		GuestThreadContext context(0);
		auto lastReport = std::chrono::steady_clock::now();

		for (;;)
		{
			uint32_t base, dwords, writeBack;
			{
				std::lock_guard lock(g_state.mutex);
				base = g_state.ringBase;
				dwords = g_state.ringDwords;
				writeBack = g_state.writeBack;
			}

			const uint32_t writePointer = ReadRegister(CP_RB_WPTR) % dwords;
			if (writePointer != g_state.readPointer)
			{
				Reader reader{ base, dwords, g_state.readPointer, writePointer };
				Execute(reader, 0);
				g_state.readPointer = reader.pos;

				WriteRegister(CP_RB_RPTR, g_state.readPointer);
				if (writeBack != 0)
					StoreBE(writeBack, g_state.readPointer);
			}
			else
			{
				std::this_thread::sleep_for(std::chrono::microseconds(200));
			}

			const auto now = std::chrono::steady_clock::now();
			if (now - lastReport > std::chrono::seconds(5))
			{
				lastReport = now;
				fmt::println(stderr, "[gpu] {} packets, {} draws, {} frames", g_state.packets, g_state.draws, g_state.swaps.load());
			}
		}
	}

	// 60 Hz vertical blank: interrupt source 0, delivered to CPU 2 like the console's.
	void VblankThread()
	{
		auto next = std::chrono::steady_clock::now();
		for (;;)
		{
			next += std::chrono::microseconds(16667);
			std::this_thread::sleep_until(next);
			DispatchInterrupt(0, 2);
		}
	}

	void InitRegisterWindow()
	{
		StoreBE(Gpu::REGISTER_BASE + RB_EDRAM_TIMING * 4, 0x08100748);
		StoreBE(Gpu::REGISTER_BASE + RB_BC_CONTROL * 4, 0x0000200E);
		StoreBE(Gpu::REGISTER_BASE + D1MODE_V_COUNTER * 4, 720); // always "in vblank"
		StoreBE(Gpu::REGISTER_BASE + D1MODE_INTERRUPT_STATUS * 4, 1);
		StoreBE(Gpu::REGISTER_BASE + D1MODE_VIEWPORT_SIZE * 4, (1280 << 16) | 720);
	}
}

void Gpu::InitializeRingBuffer(uint32_t gpuAddress, uint32_t sizeLog2)
{
	std::lock_guard lock(g_state.mutex);
	g_state.ringBase = Translate(gpuAddress);
	g_state.ringDwords = (1u << (sizeLog2 + 3)) / 4;
	g_state.readPointer = 0;
	fmt::println(stderr, "[gpu] ring buffer at {:08X}, {} dwords", g_state.ringBase, g_state.ringDwords);

	if (!g_state.started)
	{
		g_state.started = true;
		InitRegisterWindow();
		std::thread(CommandProcessorThread).detach();
		std::thread(VblankThread).detach();
	}
}

void Gpu::EnableReadPointerWriteBack(uint32_t gpuAddress, uint32_t blockLog2)
{
	std::lock_guard lock(g_state.mutex);
	g_state.writeBack = Translate(gpuAddress);
}

void Gpu::SetInterruptCallback(uint32_t callback, uint32_t userData)
{
	g_state.interruptData = userData;
	g_state.interruptCallback = callback;
}

uint64_t Gpu::SwapCount()
{
	return g_state.swaps;
}
