// Ips3: the C runtime formatting functions the game imports from the kernel (sprintf,
// _snprintf, _vsnprintf, DbgPrint).
//
// UnleashedRecomp stubs these out; Bayonetta builds file paths with them, so they have to work.
// Variadic arguments on the Xbox 360 each take one 64-bit slot: r3..r10 first, then the
// caller's parameter area on the stack. Integers sit in the low half of a slot, doubles fill
// it. A guest va_list is a pointer to the next slot.
#include <stdafx.h>

#include <kernel/function.h>
#include <kernel/memory.h>

namespace
{
	// Supplies successive argument slots.
	struct ArgSlots
	{
		PPCContext* ctx = nullptr; // register form
		uint8_t* base = nullptr;
		uint32_t next = 0;         // next slot index (0 = r3)
		uint32_t vaList = 0;       // va_list form: guest address of the next slot

		uint64_t Next()
		{
			if (vaList)
			{
				const uint64_t value = *reinterpret_cast<be<uint64_t>*>(base + vaList);
				vaList += 8;
				return value;
			}

			const uint32_t slot = next++;
			switch (slot)
			{
			case 0: return ctx->r3.u64;
			case 1: return ctx->r4.u64;
			case 2: return ctx->r5.u64;
			case 3: return ctx->r6.u64;
			case 4: return ctx->r7.u64;
			case 5: return ctx->r8.u64;
			case 6: return ctx->r9.u64;
			case 7: return ctx->r10.u64;
			default: break;
			}

			// Parameter save area: 0x50 bytes of linkage/register home, then one slot per arg.
			return *reinterpret_cast<be<uint64_t>*>(base + ctx->r1.u32 + 0x50 + (slot - 8) * 8);
		}

		uint32_t NextU32() { return uint32_t(Next()); }

		double NextDouble()
		{
			const uint64_t bits = Next();
			double value;
			memcpy(&value, &bits, sizeof(value));
			return value;
		}
	};

	std::string GuestWideToUtf8(const be<uint16_t>* str, int maxChars)
	{
		std::string out;
		for (int i = 0; (maxChars < 0 || i < maxChars) && str[i] != 0; i++)
		{
			const uint32_t c = str[i];
			if (c < 0x80)
			{
				out += char(c);
			}
			else if (c < 0x800)
			{
				out += char(0xC0 | (c >> 6));
				out += char(0x80 | (c & 0x3F));
			}
			else
			{
				out += char(0xE0 | (c >> 12));
				out += char(0x80 | ((c >> 6) & 0x3F));
				out += char(0x80 | (c & 0x3F));
			}
		}
		return out;
	}

	// Formats `format` (a guest string) into a host string, taking arguments from `args`.
	std::string Format(uint8_t* base, const char* format, ArgSlots& args)
	{
		std::string out;
		char spec[64];
		char buffer[512];

		for (const char* p = format; *p; p++)
		{
			if (*p != '%')
			{
				out += *p;
				continue;
			}

			if (p[1] == '%')
			{
				out += '%';
				p++;
				continue;
			}

			// %[flags][width][.precision][length]conversion
			const char* start = p++;
			std::string flags;
			while (*p && strchr("-+ #0", *p))
				flags += *p++;

			int width = -1;
			if (*p == '*')
			{
				width = int32_t(args.NextU32());
				p++;
			}
			else
			{
				while (*p >= '0' && *p <= '9')
					width = (width < 0 ? 0 : width * 10) + (*p++ - '0');
			}

			int precision = -1;
			if (*p == '.')
			{
				p++;
				precision = 0;
				if (*p == '*')
				{
					precision = int32_t(args.NextU32());
					p++;
				}
				else
				{
					while (*p >= '0' && *p <= '9')
						precision = precision * 10 + (*p++ - '0');
				}
			}

			// Length: h, hh, l, ll, L, I64, I32, w (MSVC wide).
			bool is64 = false, isShort = false, isChar = false, isWide = false;
			for (;;)
			{
				if (*p == 'h') { isChar = isShort; isShort = true; p++; }
				else if (*p == 'l') { if (p[1] == 'l') { is64 = true; p += 2; } else { isWide = true; p++; } }
				else if (*p == 'L' || *p == 'q') { is64 = true; p++; }
				else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { is64 = true; p += 3; }
				else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') { p += 3; }
				else if (*p == 'w') { isWide = true; p++; }
				else break;
			}

			const char conv = *p;
			if (!conv)
			{
				out.append(start);
				break;
			}

			// Host spec without the length modifier; the value is passed at full width below.
			std::string hostSpec = "%" + flags;
			if (width >= 0) hostSpec += std::to_string(width);
			if (precision >= 0) hostSpec += "." + std::to_string(precision);

			switch (conv)
			{
			case 'd':
			case 'i':
			{
				int64_t v = is64 ? int64_t(args.Next()) : int64_t(int32_t(args.NextU32()));
				if (isChar) v = int8_t(v);
				else if (isShort) v = int16_t(v);
				snprintf(spec, sizeof(spec), "%slld", hostSpec.c_str());
				snprintf(buffer, sizeof(buffer), spec, (long long)v);
				out += buffer;
				break;
			}
			case 'u':
			case 'x':
			case 'X':
			case 'o':
			{
				uint64_t v = is64 ? args.Next() : uint64_t(args.NextU32());
				if (isChar) v = uint8_t(v);
				else if (isShort) v = uint16_t(v);
				snprintf(spec, sizeof(spec), "%sll%c", hostSpec.c_str(), conv);
				snprintf(buffer, sizeof(buffer), spec, (unsigned long long)v);
				out += buffer;
				break;
			}
			case 'p':
			{
				snprintf(buffer, sizeof(buffer), "%08X", args.NextU32());
				out += buffer;
				break;
			}
			case 'c':
			case 'C':
			{
				const uint32_t c = args.NextU32();
				snprintf(spec, sizeof(spec), "%sc", hostSpec.c_str());
				snprintf(buffer, sizeof(buffer), spec, int(c & 0xFF));
				out += buffer;
				break;
			}
			case 's':
			case 'S':
			{
				const uint32_t ptr = args.NextU32();
				// MSVC: %S in a narrow printf is a wide string; %ls/%ws always wide.
				const bool wide = isWide || conv == 'S';
				std::string str;

				if (!ptr)
					str = "(null)";
				else if (wide)
					str = GuestWideToUtf8(reinterpret_cast<be<uint16_t>*>(base + ptr), precision);
				else
					str = precision >= 0 ? std::string(reinterpret_cast<const char*>(base + ptr), strnlen(reinterpret_cast<const char*>(base + ptr), precision))
						: std::string(reinterpret_cast<const char*>(base + ptr));

				std::string pad;
				if (width > int(str.size()))
					pad.assign(width - str.size(), ' ');
				out += flags.find('-') != std::string::npos ? str + pad : pad + str;
				break;
			}
			case 'f':
			case 'F':
			case 'e':
			case 'E':
			case 'g':
			case 'G':
			case 'a':
			case 'A':
			{
				snprintf(spec, sizeof(spec), "%s%c", hostSpec.c_str(), conv);
				snprintf(buffer, sizeof(buffer), spec, args.NextDouble());
				out += buffer;
				break;
			}
			case 'n':
				args.NextU32(); // never write through it
				break;
			default:
				out.append(start, p + 1);
				break;
			}
		}

		return out;
	}

	// Copies like MSVC's _snprintf: no terminator when truncated, returns -1 then.
	int32_t StoreMsvcN(char* dest, uint32_t count, const std::string& text)
	{
		if (text.size() > count)
		{
			memcpy(dest, text.data(), count);
			return -1;
		}

		memcpy(dest, text.data(), text.size());
		if (text.size() < count)
			dest[text.size()] = '\0';
		return int32_t(text.size());
	}
}

// int sprintf(char* buffer, const char* format, ...)
PPC_FUNC(__imp__sprintf)
{
	ArgSlots args{ &ctx, base, 2 };
	const std::string text = Format(base, reinterpret_cast<const char*>(base + ctx.r4.u32), args);
	memcpy(base + ctx.r3.u32, text.c_str(), text.size() + 1);
	ctx.r3.s64 = int32_t(text.size());
}

// int _snprintf(char* buffer, size_t count, const char* format, ...)
PPC_FUNC(__imp___snprintf)
{
	ArgSlots args{ &ctx, base, 3 };
	const std::string text = Format(base, reinterpret_cast<const char*>(base + ctx.r5.u32), args);
	ctx.r3.s64 = StoreMsvcN(reinterpret_cast<char*>(base + ctx.r3.u32), ctx.r4.u32, text);
}

// int _vsnprintf(char* buffer, size_t count, const char* format, va_list args)
PPC_FUNC(__imp___vsnprintf)
{
	ArgSlots args{ &ctx, base, 0, ctx.r6.u32 };
	const std::string text = Format(base, reinterpret_cast<const char*>(base + ctx.r5.u32), args);
	ctx.r3.s64 = StoreMsvcN(reinterpret_cast<char*>(base + ctx.r3.u32), ctx.r4.u32, text);
}

// uint32_t DbgPrint(const char* format, ...)
PPC_FUNC(__imp__DbgPrint)
{
	ArgSlots args{ &ctx, base, 1 };
	std::string text = Format(base, reinterpret_cast<const char*>(base + ctx.r3.u32), args);
	while (!text.empty() && text.back() == '\n')
		text.pop_back();
	fmt::println(stderr, "[guest] {}", text);
	ctx.r3.u64 = 0;
}
