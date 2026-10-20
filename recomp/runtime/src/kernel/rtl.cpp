// Ips3: kernel runtime-library helpers UnleashedRecomp leaves as stubs (strings, time fields,
// memory patterns, status codes), plus kernel calls that need no work on a host.
#include <stdafx.h>

#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/xdm.h>

namespace
{
	struct XUNICODE_STRING
	{
		be<uint16_t> Length;        // bytes
		be<uint16_t> MaximumLength; // bytes
		xpointer<be<uint16_t>> Buffer;
	};

	constexpr int64_t TICKS_PER_SECOND = 10000000;
	constexpr int64_t TICKS_PER_DAY = TICKS_PER_SECOND * 86400;

	// Days since 1601-01-01 for a proleptic Gregorian date, and back (Howard Hinnant's
	// civil-from-days, shifted from the Unix epoch).
	constexpr int64_t DAYS_1601_TO_1970 = 134774;

	int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
	{
		y -= m <= 2;
		const int64_t era = (y >= 0 ? y : y - 399) / 400;
		const unsigned yoe = unsigned(y - era * 400);
		const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
		const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		return era * 146097 + int64_t(doe) - 719468 + DAYS_1601_TO_1970;
	}

	void CivilFromDays(int64_t days, int64_t& y, unsigned& m, unsigned& d)
	{
		const int64_t z = days - DAYS_1601_TO_1970 + 719468;
		const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
		const unsigned doe = unsigned(z - era * 146097);
		const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
		const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		const unsigned mp = (5 * doy + 2) / 153;
		d = doy - (153 * mp + 2) / 5 + 1;
		m = mp < 10 ? mp + 3 : mp - 9;
		y = int64_t(yoe) + era * 400 + (m <= 2);
	}
}

// void RtlTimeToTimeFields(const LARGE_INTEGER* time, TIME_FIELDS* fields)
void RtlTimeToTimeFields(be<uint64_t>* time, XTIME_FIELDS* fields)
{
	const int64_t ticks = int64_t(uint64_t(*time));
	const int64_t days = ticks / TICKS_PER_DAY;
	int64_t rest = ticks % TICKS_PER_DAY;

	int64_t year;
	unsigned month, day;
	CivilFromDays(days, year, month, day);

	fields->Year = uint16_t(year);
	fields->Month = uint16_t(month);
	fields->Day = uint16_t(day);
	fields->Hour = uint16_t(rest / (TICKS_PER_SECOND * 3600));
	rest %= TICKS_PER_SECOND * 3600;
	fields->Minute = uint16_t(rest / (TICKS_PER_SECOND * 60));
	rest %= TICKS_PER_SECOND * 60;
	fields->Second = uint16_t(rest / TICKS_PER_SECOND);
	fields->Milliseconds = uint16_t((rest % TICKS_PER_SECOND) / 10000);
	fields->Weekday = uint16_t((days + 1) % 7); // 1601-01-01 was a Monday
}

// BOOLEAN RtlTimeFieldsToTime(const TIME_FIELDS* fields, LARGE_INTEGER* time)
uint32_t RtlTimeFieldsToTime(XTIME_FIELDS* fields, be<uint64_t>* time)
{
	const unsigned month = fields->Month, day = fields->Day;
	if (month < 1 || month > 12 || day < 1 || day > 31 || fields->Hour > 23 || fields->Minute > 59 || fields->Second > 59 || fields->Milliseconds > 999)
		return false;

	const int64_t days = DaysFromCivil(fields->Year, month, day);
	const int64_t seconds = int64_t(fields->Hour) * 3600 + int64_t(fields->Minute) * 60 + fields->Second;
	*time = uint64_t(days * TICKS_PER_DAY + seconds * TICKS_PER_SECOND + int64_t(fields->Milliseconds) * 10000);
	return true;
}

// void RtlInitUnicodeString(UNICODE_STRING* destination, const wchar_t* source)
void RtlInitUnicodeString(XUNICODE_STRING* destination, be<uint16_t>* source)
{
	size_t length = 0;
	if (source != nullptr)
	{
		while (source[length] != 0)
			length++;
	}

	destination->Length = uint16_t(length * 2);
	destination->MaximumLength = source ? uint16_t(length * 2 + 2) : 0;
	destination->Buffer = source;
}

// NTSTATUS RtlUnicodeStringToAnsiString(ANSI_STRING* destination, const UNICODE_STRING* source,
//     BOOLEAN allocateDestination)
uint32_t RtlUnicodeStringToAnsiString(XANSI_STRING* destination, XUNICODE_STRING* source, uint32_t allocate)
{
	const uint32_t length = source->Length / 2;

	if (allocate & 0xFF)
	{
		void* buffer = g_userHeap.AllocPhysical(length + 1, 16);
		destination->Buffer = static_cast<char*>(buffer);
		destination->MaximumLength = uint16_t(length + 1);
	}
	else if (destination->MaximumLength < length + 1)
	{
		return 0x80000005; // STATUS_BUFFER_OVERFLOW
	}

	char* out = destination->Buffer.get();
	const be<uint16_t>* in = source->Buffer.get();
	for (uint32_t i = 0; i < length; i++)
	{
		const uint16_t c = in[i];
		out[i] = c < 0x100 ? char(c) : '?';
	}
	out[length] = '\0';
	destination->Length = uint16_t(length);
	return STATUS_SUCCESS;
}

// void RtlFreeAnsiString(ANSI_STRING* string)
void RtlFreeAnsiString(XANSI_STRING* string)
{
	if (string->Buffer.get() != nullptr)
		g_userHeap.Free(string->Buffer.get());

	string->Buffer = nullptr;
	string->Length = 0;
	string->MaximumLength = 0;
}

// int RtlCompareStringN(const char* a, uint32_t aLength, const char* b, uint32_t bLength,
//     BOOLEAN caseInsensitive)
// A length of -1 means the string is null-terminated.
int32_t RtlCompareStringN(const char* a, uint32_t aLength, const char* b, uint32_t bLength, uint32_t caseInsensitive)
{
	const size_t lengthA = aLength == 0xFFFFFFFF ? strlen(a) : aLength;
	const size_t lengthB = bLength == 0xFFFFFFFF ? strlen(b) : bLength;
	const size_t count = std::min(lengthA, lengthB);

	for (size_t i = 0; i < count; i++)
	{
		int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
		if (caseInsensitive & 0xFF)
		{
			ca = toupper(ca);
			cb = toupper(cb);
		}

		if (ca != cb)
			return ca - cb;
	}

	return int32_t(lengthA) - int32_t(lengthB);
}

// wchar_t RtlUpcaseUnicodeChar(wchar_t c)
uint32_t RtlUpcaseUnicodeChar(uint32_t c)
{
	c &= 0xFFFF;
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 'A';
	if ((c >= 0xE0 && c <= 0xFE && c != 0xF7))
		return c - 0x20;
	return c;
}

// void RtlFillMemoryUlong(void* destination, size_t length, uint32_t pattern)
void RtlFillMemoryUlong(be<uint32_t>* destination, uint32_t length, uint32_t pattern)
{
	for (uint32_t i = 0; i < length / 4; i++)
		destination[i] = pattern;
}

// size_t RtlCompareMemoryUlong(const void* source, size_t length, uint32_t pattern)
// Returns how many leading bytes match the repeated pattern.
uint32_t RtlCompareMemoryUlong(be<uint32_t>* source, uint32_t length, uint32_t pattern)
{
	uint32_t i = 0;
	while (i < length / 4 && source[i] == pattern)
		i++;
	return i * 4;
}

// uint32_t RtlNtStatusToDosError(NTSTATUS status)
uint32_t RtlNtStatusToDosError(uint32_t status)
{
	switch (status)
	{
	case 0x00000000: return 0;    // ERROR_SUCCESS
	case 0x00000102: return 258;  // WAIT_TIMEOUT
	case 0x00000103: return 997;  // ERROR_IO_PENDING
	case 0x80000005: return 234;  // ERROR_MORE_DATA
	case 0x80000006: return 18;   // ERROR_NO_MORE_FILES
	case 0xC0000002: return 1;    // ERROR_INVALID_FUNCTION
	case 0xC0000003: return 87;   // ERROR_INVALID_PARAMETER
	case 0xC0000008: return 6;    // ERROR_INVALID_HANDLE
	case 0xC000000D: return 87;   // ERROR_INVALID_PARAMETER
	case 0xC000000F: return 2;    // ERROR_FILE_NOT_FOUND
	case 0xC0000011: return 38;   // ERROR_HANDLE_EOF
	case 0xC0000017: return 8;    // ERROR_NOT_ENOUGH_MEMORY
	case 0xC0000022: return 5;    // ERROR_ACCESS_DENIED
	case 0xC0000023: return 122;  // ERROR_INSUFFICIENT_BUFFER
	case 0xC0000034: return 2;    // ERROR_FILE_NOT_FOUND
	case 0xC0000035: return 183;  // ERROR_ALREADY_EXISTS
	case 0xC000003A: return 3;    // ERROR_PATH_NOT_FOUND
	case 0xC000007F: return 112;  // ERROR_DISK_FULL
	case 0xC00000BA: return 5;    // ERROR_ACCESS_DENIED
	case 0xC0000103: return 267;  // ERROR_DIRECTORY
	case 0xC0000135: return 126;  // ERROR_MOD_NOT_FOUND
	case 0xC000007A: return 127;  // ERROR_PROC_NOT_FOUND
	default:
		if ((status & 0xFFFF0000) == 0x80070000 || (status & 0xFFFF0000) == 0xC0070000)
			return status & 0xFFFF; // a Win32 error wrapped as a status
		return 317; // ERROR_MR_MID_NOT_FOUND
	}
}

// Nothing to do on a host: there are no kernel APCs to hold off, no L2 cache lines to lock, and
// FPU exceptions stay masked.
void KeEnterCriticalRegion() {}
void KeLeaveCriticalRegion() {}
uint32_t KeLockL2(uint32_t, uint32_t) { return STATUS_SUCCESS; }
void KeUnlockL2(uint32_t) {}
uint32_t KeEnableFpuExceptions(uint32_t) { return 0; }
uint32_t KeQueryBasePriorityThread(uint32_t) { return 0; }
void ObReferenceObject(uint32_t) {}
void ObDereferenceObject(uint32_t) {}
uint32_t ObIsTitleObject(uint32_t) { return true; }
void ExRegisterTitleTerminateNotification(uint32_t, uint32_t) {}

void DbgBreakPoint()
{
	fmt::println(stderr, "[guest] DbgBreakPoint");
}

void HalReturnToFirmware(uint32_t routine)
{
	fmt::println(stderr, "[guest] HalReturnToFirmware({}): the title asked to exit", routine);
	std::_Exit(0);
}

void RtlRaiseException(uint32_t record)
{
	auto* fields = static_cast<be<uint32_t>*>(g_memory.Translate(record));
	fmt::println(stderr, "[guest] RtlRaiseException code {:08X} flags {:08X} address {:08X}", uint32_t(fields[0]), uint32_t(fields[1]), uint32_t(fields[3]));
	std::abort();
}

void RtlUnwind(uint32_t targetFrame, uint32_t targetIp, uint32_t record, uint32_t returnValue)
{
	fmt::println(stderr, "[guest] RtlUnwind to {:08X}: structured exception unwinding is not supported", targetIp);
	std::abort();
}

GUEST_FUNCTION_HOOK(__imp__RtlTimeToTimeFields, RtlTimeToTimeFields);
GUEST_FUNCTION_HOOK(__imp__RtlTimeFieldsToTime, RtlTimeFieldsToTime);
GUEST_FUNCTION_HOOK(__imp__RtlInitUnicodeString, RtlInitUnicodeString);
GUEST_FUNCTION_HOOK(__imp__RtlUnicodeStringToAnsiString, RtlUnicodeStringToAnsiString);
GUEST_FUNCTION_HOOK(__imp__RtlFreeAnsiString, RtlFreeAnsiString);
GUEST_FUNCTION_HOOK(__imp__RtlCompareStringN, RtlCompareStringN);
GUEST_FUNCTION_HOOK(__imp__RtlUpcaseUnicodeChar, RtlUpcaseUnicodeChar);
GUEST_FUNCTION_HOOK(__imp__RtlFillMemoryUlong, RtlFillMemoryUlong);
GUEST_FUNCTION_HOOK(__imp__RtlCompareMemoryUlong, RtlCompareMemoryUlong);
GUEST_FUNCTION_HOOK(__imp__RtlNtStatusToDosError, RtlNtStatusToDosError);
GUEST_FUNCTION_HOOK(__imp__KeEnterCriticalRegion, KeEnterCriticalRegion);
GUEST_FUNCTION_HOOK(__imp__KeLeaveCriticalRegion, KeLeaveCriticalRegion);
GUEST_FUNCTION_HOOK(__imp__KeLockL2, KeLockL2);
GUEST_FUNCTION_HOOK(__imp__KeUnlockL2, KeUnlockL2);
GUEST_FUNCTION_HOOK(__imp__KeEnableFpuExceptions, KeEnableFpuExceptions);
GUEST_FUNCTION_HOOK(__imp__KeQueryBasePriorityThread, KeQueryBasePriorityThread);
GUEST_FUNCTION_HOOK(__imp__ObReferenceObject, ObReferenceObject);
GUEST_FUNCTION_HOOK(__imp__ObDereferenceObject, ObDereferenceObject);
GUEST_FUNCTION_HOOK(__imp__ObIsTitleObject, ObIsTitleObject);
GUEST_FUNCTION_HOOK(__imp__ExRegisterTitleTerminateNotification, ExRegisterTitleTerminateNotification);
GUEST_FUNCTION_HOOK(__imp__DbgBreakPoint, DbgBreakPoint);
GUEST_FUNCTION_HOOK(__imp__HalReturnToFirmware, HalReturnToFirmware);
GUEST_FUNCTION_HOOK(__imp__RtlRaiseException, RtlRaiseException);
GUEST_FUNCTION_HOOK(__imp__RtlUnwind, RtlUnwind);
