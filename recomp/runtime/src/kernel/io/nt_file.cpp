// Ips3: the NT file API (NtCreateFile, NtReadFile, NtQueryDirectoryFile, ...).
//
// UnleashedRecomp hooks the Win32-style file functions inside Sonic Unleashed itself; Bayonetta's
// C runtime calls the kernel directly, so files are served here, at the NT level. Requests
// complete synchronously: the status block is filled and the event, if any, signalled before
// returning. Paths inside a mounted disc image are served from the image, read-only.
#include <stdafx.h>

#include "disc_image.h"
#include "file_system.h"

#include <kernel/function.h>
#include <kernel/heap.h>
#include <kernel/memory.h>
#include <kernel/xam.h>
#include <kernel/xdm.h>
#include <ips3_gpu.h>
#include <cpu/guest_thread.h>

#include <unordered_set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATUS_PENDING                 0x00000103
#define STATUS_NO_MORE_FILES           0x80000006
#define STATUS_INVALID_HANDLE          0xC0000008
#define STATUS_INVALID_PARAMETER       0xC000000D
#define STATUS_NO_SUCH_FILE            0xC000000F
#define STATUS_END_OF_FILE             0xC0000011
#define STATUS_ACCESS_DENIED           0xC0000022
#define STATUS_BUFFER_TOO_SMALL        0xC0000023
#define STATUS_OBJECT_NAME_NOT_FOUND   0xC0000034
#define STATUS_OBJECT_NAME_COLLISION   0xC0000035
#define STATUS_OBJECT_PATH_NOT_FOUND   0xC000003A
#define STATUS_FILE_IS_A_DIRECTORY     0xC00000BA
#define STATUS_NOT_A_DIRECTORY         0xC0000103
#define STATUS_INVALID_INFO_CLASS      0xC0000003

#define FILE_ATTRIBUTE_READONLY 0x00000001

// CreateDisposition
#define FILE_SUPERSEDE    0
#define FILE_OPEN         1
#define FILE_CREATE       2
#define FILE_OPEN_IF      3
#define FILE_OVERWRITE    4
#define FILE_OVERWRITE_IF 5

// CreateOptions
#define FILE_DIRECTORY_FILE     0x00000001
#define FILE_NON_DIRECTORY_FILE 0x00000040
#define FILE_DELETE_ON_CLOSE    0x00001000

// IO_STATUS_BLOCK.Information after a create
#define FILE_SUPERSEDED  0
#define FILE_OPENED      1
#define FILE_CREATED     2
#define FILE_OVERWRITTEN 3
#define FILE_EXISTS      4

struct Event;
uint32_t NtSetEvent(Event* handle, uint32_t* previousState);

namespace
{
	constexpr int64_t FILETIME_EPOCH_DIFFERENCE = 116444736000000000LL;

	struct DirectoryEntry
	{
		std::string name;
		struct stat info;
	};

	struct NtFile final : KernelObject
	{
		std::string guestPath;
		std::filesystem::path hostPath;
		int fd = -1;
		std::shared_ptr<DiscImage::Disc> disc; // set for files inside a disc image
		const DiscImage::Entry* discEntry = nullptr;
		bool directory = false;
		bool deleteOnClose = false;
		// Opened without FILE_SYNCHRONOUS_IO_(NON)ALERT: XAPI's ReadFile/WriteFile with an
		// OVERLAPPED expect such I/O to be reported as pending.
		bool asynchronous = false;
		std::mutex mutex;
		uint64_t position = 0;

		std::vector<DirectoryEntry> entries;
		size_t nextEntry = 0;
		bool listed = false;

		~NtFile() override
		{
			if (fd >= 0 && disc == nullptr)
				close(fd);

			if (deleteOnClose)
			{
				std::error_code ec;
				std::filesystem::remove(hostPath, ec);
			}
		}
	};

	// Handles NtDuplicateObject gave out on top of the original.
	std::mutex g_duplicateMutex;
	std::unordered_map<uint32_t, uint32_t> g_duplicates;

	std::mutex g_fileHandlesMutex;
	std::unordered_set<uint32_t> g_fileHandles;

	NtFile* GetFile(uint32_t handle)
	{
		std::lock_guard lock(g_fileHandlesMutex);
		return g_fileHandles.contains(handle) ? GetKernelObject<NtFile>(handle) : nullptr;
	}

	std::string GuestString(const XANSI_STRING* str)
	{
		if (str == nullptr || str->Buffer.get() == nullptr)
			return {};

		return std::string(str->Buffer.get(), str->Length);
	}

	// Full guest path named by an OBJECT_ATTRIBUTES, relative to RootDirectory when it is set.
	std::string GuestPath(const XOBJECT_ATTRIBUTES* attributes)
	{
		std::string name = GuestString(attributes->Name.get());

		if (attributes->RootDirectory != 0 && attributes->RootDirectory != 0xFFFFFFFD)
		{
			if (auto* root = GetFile(attributes->RootDirectory))
			{
				std::string base = root->guestPath;
				if (!base.empty() && base.back() != '\\')
					base += '\\';
				name = base + name;
			}
		}

		return name;
	}

	// A stat for a disc entry: the image has no times, the entry offset stands in for an inode.
	struct stat DiscStat(const DiscImage::Entry& entry)
	{
		struct stat info{};
		info.st_mode = entry.directory ? (S_IFDIR | 0555) : (S_IFREG | 0444);
		info.st_size = off_t(entry.size);
		info.st_ino = ino_t(entry.offset);
		info.st_nlink = 1;
		return info;
	}

	// stat() that also sees inside mounted disc images.
	bool StatPath(const std::filesystem::path& path, struct stat& info, std::shared_ptr<DiscImage::Disc>* discOut = nullptr, const DiscImage::Entry** entryOut = nullptr)
	{
		const DiscImage::Entry* entry = nullptr;
		if (auto disc = DiscImage::Lookup(path, &entry))
		{
			if (discOut != nullptr)
				*discOut = disc;
			if (entryOut != nullptr)
				*entryOut = entry;
			if (entry == nullptr)
				return false;
			info = DiscStat(*entry);
			return true;
		}

		return stat(path.c_str(), &info) == 0;
	}

	uint64_t FileTime(const struct timespec& time)
	{
		return uint64_t(time.tv_sec) * 10000000ull + uint64_t(time.tv_nsec) / 100 + FILETIME_EPOCH_DIFFERENCE;
	}

	const struct timespec& ModifiedTime(const struct stat& info)
	{
#ifdef __APPLE__
		return info.st_mtimespec;
#else
		return info.st_mtim;
#endif
	}

	uint32_t Attributes(const struct stat& info)
	{
		return S_ISDIR(info.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
	}

	uint64_t AllocationSize(uint64_t size)
	{
		return (size + 0xFFF) & ~uint64_t(0xFFF);
	}

	void Complete(XIO_STATUS_BLOCK* ioStatus, uint32_t status, uint32_t information, uint32_t event)
	{
		if (ioStatus != nullptr)
		{
			ioStatus->Status = status;
			ioStatus->Information = information;
		}

		if (event != 0)
			NtSetEvent(GetKernelObject<Event>(event), nullptr);
	}

	// The I/O has completed (status block and event set). On an asynchronous file, a caller with
	// an OVERLAPPED (passed as the APC context) is told it is pending, as the kernel would, and
	// reads the result from the OVERLAPPED; XAPI turns that into ERROR_IO_PENDING.
	uint32_t Result(const NtFile* file, uint32_t status, uint32_t apcContext)
	{
		constexpr uint32_t STATUS_PENDING_ = 0x103;
		return (file->asynchronous && apcContext != 0 && status == STATUS_SUCCESS) ? STATUS_PENDING_ : status;
	}

	// Case-insensitive match with '*' and '?'.
	bool MatchMask(std::string_view mask, std::string_view name)
	{
		size_t m = 0, n = 0, star = std::string_view::npos, resume = 0;
		while (n < name.size())
		{
			if (m < mask.size() && (mask[m] == '?' || tolower((unsigned char)mask[m]) == tolower((unsigned char)name[n])))
			{
				m++;
				n++;
			}
			else if (m < mask.size() && mask[m] == '*')
			{
				star = m++;
				resume = n;
			}
			else if (star != std::string_view::npos)
			{
				m = star + 1;
				n = ++resume;
			}
			else
			{
				return false;
			}
		}

		while (m < mask.size() && mask[m] == '*')
			m++;

		return m == mask.size();
	}

	struct XFILE_NETWORK_OPEN_INFORMATION
	{
		be<uint64_t> CreationTime;
		be<uint64_t> LastAccessTime;
		be<uint64_t> LastWriteTime;
		be<uint64_t> ChangeTime;
		be<uint64_t> AllocationSize;
		be<uint64_t> EndOfFile;
		be<uint32_t> FileAttributes;
		be<uint32_t> Reserved;
	};
	static_assert(sizeof(XFILE_NETWORK_OPEN_INFORMATION) == 56);

	void FillNetworkOpenInfo(XFILE_NETWORK_OPEN_INFORMATION* out, const struct stat& info)
	{
		const uint64_t time = FileTime(ModifiedTime(info));
		out->CreationTime = time;
		out->LastAccessTime = time;
		out->LastWriteTime = time;
		out->ChangeTime = time;
		out->AllocationSize = S_ISDIR(info.st_mode) ? 0 : AllocationSize(info.st_size);
		out->EndOfFile = S_ISDIR(info.st_mode) ? 0 : uint64_t(info.st_size);
		out->FileAttributes = Attributes(info);
		out->Reserved = 0;
	}

	// pread on the file, or on its slice of the disc image.
	ssize_t ReadAt(NtFile* file, void* buffer, size_t length, uint64_t offset)
	{
		if (file->disc != nullptr)
		{
			if (offset >= file->discEntry->size)
				return 0;
			length = std::min<uint64_t>(length, file->discEntry->size - offset);
			offset += file->discEntry->offset;
		}

		// The GPU may be watching the destination for CPU writes; a system call writing a
		// watched (read-only) page would fail instead of faulting.
		ips3_memory_will_write(g_memory.MapVirtual(buffer), uint32_t(length));
		return pread(file->fd, buffer, length, off_t(offset));
	}

	// fstat that understands disc entries.
	bool StatFile(NtFile* file, struct stat& info)
	{
		if (file->disc != nullptr)
		{
			info = DiscStat(*file->discEntry);
			return true;
		}

		return file->fd >= 0 ? fstat(file->fd, &info) == 0 : stat(file->hostPath.c_str(), &info) == 0;
	}

	// Byte offset of a read or write: the explicit offset, or the file pointer when the caller
	// passes none (or FILE_USE_FILE_POINTER_POSITION).
	uint64_t RequestOffset(NtFile* file, const be<uint64_t>* byteOffset)
	{
		if (byteOffset == nullptr)
			return file->position;

		const uint64_t offset = *byteOffset;
		return offset == 0xFFFFFFFFFFFFFFFEull ? file->position : offset;
	}
}

// Called by NtClose before it destroys an object. True while duplicates are still open.
bool ReleaseDuplicatedHandle(uint32_t handle)
{
	{
		std::lock_guard lock(g_duplicateMutex);
		auto it = g_duplicates.find(handle);
		if (it != g_duplicates.end())
		{
			if (--it->second == 0)
				g_duplicates.erase(it);
			return true;
		}
	}

	std::lock_guard lock(g_fileHandlesMutex);
	g_fileHandles.erase(handle);
	return false;
}

// NTSTATUS NtCreateFile(HANDLE* handle, ACCESS_MASK access, OBJECT_ATTRIBUTES* attributes,
//     IO_STATUS_BLOCK* ioStatus, LARGE_INTEGER* allocationSize, uint32_t fileAttributes,
//     uint32_t shareAccess, uint32_t createDisposition, uint32_t createOptions)
uint32_t NtCreateFile(be<uint32_t>* handle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes, XIO_STATUS_BLOCK* ioStatus,
	be<uint64_t>* allocationSize, uint32_t fileAttributes, uint32_t shareAccess, uint32_t createDisposition, uint32_t createOptions)
{
	if (handle == nullptr || attributes == nullptr)
		return STATUS_INVALID_PARAMETER;

	*handle = 0;

	const std::string guestPath = GuestPath(attributes);
	const std::filesystem::path hostPath = FileSystem::ResolvePath(guestPath, false);

	struct stat info{};
	std::shared_ptr<DiscImage::Disc> disc;
	const DiscImage::Entry* discEntry = nullptr;
	const bool exists = StatPath(hostPath, info, &disc, &discEntry);
	const bool isDirectory = exists && S_ISDIR(info.st_mode);

	auto fail = [&](uint32_t status)
	{
		fmt::println(stderr, "[file] open {} -> {:08X}", guestPath, status);
		Complete(ioStatus, status, 0, 0);
		return status;
	};

	uint32_t information = FILE_OPENED;
	int flags = 0;

	switch (createDisposition)
	{
	case FILE_OPEN:
		if (!exists)
			return fail(std::filesystem::exists(hostPath.parent_path()) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND);
		break;
	case FILE_CREATE:
		if (exists)
			return fail(STATUS_OBJECT_NAME_COLLISION);
		flags = O_CREAT | O_EXCL;
		information = FILE_CREATED;
		break;
	case FILE_OPEN_IF:
		flags = O_CREAT;
		information = exists ? FILE_OPENED : FILE_CREATED;
		break;
	case FILE_OVERWRITE:
		if (!exists)
			return fail(STATUS_OBJECT_NAME_NOT_FOUND);
		flags = O_TRUNC;
		information = FILE_OVERWRITTEN;
		break;
	case FILE_SUPERSEDE:
	case FILE_OVERWRITE_IF:
		flags = O_CREAT | O_TRUNC;
		information = exists ? (createDisposition == FILE_SUPERSEDE ? FILE_SUPERSEDED : FILE_OVERWRITTEN) : FILE_CREATED;
		break;
	default:
		return fail(STATUS_INVALID_PARAMETER);
	}

	// The disc is read-only: anything that would create or truncate fails.
	if (disc != nullptr && (flags & (O_CREAT | O_TRUNC | O_EXCL)) != 0 && (!exists || (flags & O_TRUNC)))
		return fail(STATUS_ACCESS_DENIED);

	if ((createOptions & FILE_NON_DIRECTORY_FILE) && isDirectory)
		return fail(STATUS_FILE_IS_A_DIRECTORY);

	if ((createOptions & FILE_DIRECTORY_FILE) && exists && !isDirectory)
		return fail(STATUS_NOT_A_DIRECTORY);

	auto* file = CreateKernelObject<NtFile>();
	file->guestPath = guestPath;
	file->hostPath = hostPath;
	file->deleteOnClose = disc == nullptr && (createOptions & FILE_DELETE_ON_CLOSE) != 0;
	file->asynchronous = (createOptions & 0x30) == 0; // FILE_SYNCHRONOUS_IO_ALERT | _NONALERT

	if (disc != nullptr)
	{
		file->disc = disc;
		file->discEntry = discEntry;
		file->directory = discEntry->directory;
		file->fd = discEntry->directory ? -1 : disc->fd;
	}
	else if (isDirectory || (createOptions & FILE_DIRECTORY_FILE))
	{
		if (!exists)
		{
			std::error_code ec;
			if (!std::filesystem::create_directories(hostPath, ec))
			{
				DestroyKernelObject(file);
				return fail(STATUS_OBJECT_PATH_NOT_FOUND);
			}
		}

		file->directory = true;
	}
	else
	{
		// Writes are allowed wherever the host allows them; the disc root is simply read-only
		// on most setups.
		const bool wantsWrite = (desiredAccess & (0x40000000 | 0x2 | 0x4 | 0x10000000)) != 0 || (flags & (O_CREAT | O_TRUNC)) != 0;
		file->fd = open(hostPath.c_str(), (wantsWrite ? O_RDWR : O_RDONLY) | flags, 0644);
		if (file->fd < 0 && wantsWrite && !(flags & (O_CREAT | O_TRUNC)))
			file->fd = open(hostPath.c_str(), O_RDONLY);

		if (file->fd < 0)
		{
			const int error = errno;
			DestroyKernelObject(file);
			return fail(error == ENOENT ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_ACCESS_DENIED);
		}
	}

	const uint32_t guestHandle = GetKernelHandle(file);
	if (getenv("IPS3_TRACE_FILES"))
		fmt::println(stderr, "[file] open {:08X} {} access {:X} disposition {} options {:X}", guestHandle, guestPath, desiredAccess, createDisposition, createOptions);
	{
		std::lock_guard lock(g_fileHandlesMutex);
		g_fileHandles.insert(guestHandle);
	}

	*handle = guestHandle;
	Complete(ioStatus, STATUS_SUCCESS, information, 0);
	return STATUS_SUCCESS;
}

// NTSTATUS NtOpenFile(HANDLE* handle, ACCESS_MASK access, OBJECT_ATTRIBUTES* attributes,
//     IO_STATUS_BLOCK* ioStatus, uint32_t shareAccess, uint32_t openOptions)
uint32_t NtOpenFile(be<uint32_t>* handle, uint32_t desiredAccess, XOBJECT_ATTRIBUTES* attributes, XIO_STATUS_BLOCK* ioStatus, uint32_t shareAccess, uint32_t openOptions)
{
	return NtCreateFile(handle, desiredAccess, attributes, ioStatus, nullptr, 0, shareAccess, FILE_OPEN, openOptions);
}

// NTSTATUS NtReadFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apcRoutine, void* apcContext,
//     IO_STATUS_BLOCK* ioStatus, void* buffer, uint32_t length, LARGE_INTEGER* byteOffset)
uint32_t NtReadFile(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
	uint8_t* buffer, uint32_t length, be<uint64_t>* byteOffset)
{
	NtFile* file = GetFile(handle);
	if (file == nullptr || file->fd < 0)
		return STATUS_INVALID_HANDLE;

	if (apcRoutine != 0)
		LOG_UTILITY("APC completion routines are not delivered");

	std::lock_guard lock(file->mutex);
	const uint64_t offset = RequestOffset(file, byteOffset);

	size_t total = 0;
	while (total < length)
	{
		const ssize_t count = ReadAt(file, buffer + total, length - total, offset + total);
		if (count <= 0)
			break;
		total += size_t(count);
	}

	file->position = offset + total;

	const uint32_t status = (total == 0 && length != 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
	if (getenv("IPS3_TRACE_FILES"))
		fmt::println(stderr, "[file] read {:08X} @{:X} +{:X} -> {:X} event {:08X} apc {:08X} status {:08X}", handle, offset, length, total, event, apcRoutine, status);
	Complete(ioStatus, status, uint32_t(total), event);
	return Result(file, status, apcContext);
}

// NTSTATUS NtReadFileScatter(HANDLE file, HANDLE event, PIO_APC_ROUTINE apcRoutine, void* apcContext,
//     IO_STATUS_BLOCK* ioStatus, FILE_SEGMENT_ELEMENT* segments, uint32_t length, LARGE_INTEGER* byteOffset)
// Each segment element is the guest address of one 4 KiB page.
uint32_t NtReadFileScatter(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
	be<uint32_t>* segments, uint32_t length, be<uint64_t>* byteOffset)
{
	NtFile* file = GetFile(handle);
	if (file == nullptr || file->fd < 0 || segments == nullptr)
		return STATUS_INVALID_HANDLE;

	std::lock_guard lock(file->mutex);
	const uint64_t offset = RequestOffset(file, byteOffset);

	size_t total = 0;
	for (size_t i = 0; total < length; i++)
	{
		const size_t chunk = std::min<size_t>(0x1000, length - total);
		auto* page = static_cast<uint8_t*>(g_memory.Translate(segments[i]));
		const ssize_t count = ReadAt(file, page, chunk, offset + total);
		if (count <= 0)
			break;
		total += size_t(count);
		if (size_t(count) < chunk)
			break;
	}

	file->position = offset + total;

	const uint32_t status = (total == 0 && length != 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
	Complete(ioStatus, status, uint32_t(total), event);
	return Result(file, status, apcContext);
}

// NTSTATUS NtWriteFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apcRoutine, void* apcContext,
//     IO_STATUS_BLOCK* ioStatus, const void* buffer, uint32_t length, LARGE_INTEGER* byteOffset)
uint32_t NtWriteFile(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
	const uint8_t* buffer, uint32_t length, be<uint64_t>* byteOffset)
{
	NtFile* file = GetFile(handle);
	if (file == nullptr || file->fd < 0)
		return STATUS_INVALID_HANDLE;

	if (file->disc != nullptr)
		return STATUS_ACCESS_DENIED;

	std::lock_guard lock(file->mutex);
	const uint64_t offset = RequestOffset(file, byteOffset);

	size_t total = 0;
	while (total < length)
	{
		const ssize_t count = pwrite(file->fd, buffer + total, length - total, off_t(offset + total));
		if (count <= 0)
			break;
		total += size_t(count);
	}

	file->position = offset + total;

	const uint32_t status = total == length ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
	Complete(ioStatus, status, uint32_t(total), event);
	return Result(file, status, apcContext);
}

// FILE_INFORMATION_CLASS values the 360 uses.
enum : uint32_t
{
	XFileBasicInformation = 4,
	XFileStandardInformation = 5,
	XFileInternalInformation = 6,
	XFileRenameInformation = 10,
	XFileDispositionInformation = 13,
	XFilePositionInformation = 14,
	XFileAlignmentInformation = 17,
	XFileAllocationInformation = 19,
	XFileEndOfFileInformation = 20,
	XFileNetworkOpenInformation = 34,
};

// NTSTATUS NtQueryInformationFile(HANDLE file, IO_STATUS_BLOCK* ioStatus, void* info, uint32_t length,
//     FILE_INFORMATION_CLASS infoClass)
uint32_t NtQueryInformationFile(uint32_t handle, XIO_STATUS_BLOCK* ioStatus, uint8_t* out, uint32_t length, uint32_t infoClass)
{
	NtFile* file = GetFile(handle);
	if (file == nullptr)
		return STATUS_INVALID_HANDLE;

	struct stat info{};
	if (!StatFile(file, info))
		return STATUS_INVALID_HANDLE;

	auto finish = [&](uint32_t size)
	{
		Complete(ioStatus, STATUS_SUCCESS, size, 0);
		return STATUS_SUCCESS;
	};

	auto words = reinterpret_cast<be<uint64_t>*>(out);
	switch (infoClass)
	{
	case XFileBasicInformation:
	{
		if (length < 40) return STATUS_BUFFER_TOO_SMALL;
		const uint64_t time = FileTime(ModifiedTime(info));
		words[0] = time;
		words[1] = time;
		words[2] = time;
		words[3] = time;
		*reinterpret_cast<be<uint32_t>*>(out + 32) = Attributes(info);
		*reinterpret_cast<be<uint32_t>*>(out + 36) = 0;
		return finish(40);
	}
	case XFileStandardInformation:
	{
		if (length < 24) return STATUS_BUFFER_TOO_SMALL;
		const uint64_t size = S_ISDIR(info.st_mode) ? 0 : uint64_t(info.st_size);
		words[0] = AllocationSize(size);
		words[1] = size;
		*reinterpret_cast<be<uint32_t>*>(out + 16) = 1;
		out[20] = file->deleteOnClose;
		out[21] = S_ISDIR(info.st_mode);
		out[22] = 0;
		out[23] = 0;
		return finish(24);
	}
	case XFileInternalInformation:
		if (length < 8) return STATUS_BUFFER_TOO_SMALL;
		words[0] = uint64_t(info.st_ino);
		return finish(8);
	case XFilePositionInformation:
	{
		if (length < 8) return STATUS_BUFFER_TOO_SMALL;
		std::lock_guard lock(file->mutex);
		words[0] = file->position;
		return finish(8);
	}
	case XFileAlignmentInformation:
		if (length < 4) return STATUS_BUFFER_TOO_SMALL;
		*reinterpret_cast<be<uint32_t>*>(out) = 0;
		return finish(4);
	case XFileNetworkOpenInformation:
		if (length < sizeof(XFILE_NETWORK_OPEN_INFORMATION)) return STATUS_BUFFER_TOO_SMALL;
		FillNetworkOpenInfo(reinterpret_cast<XFILE_NETWORK_OPEN_INFORMATION*>(out), info);
		return finish(sizeof(XFILE_NETWORK_OPEN_INFORMATION));
	default:
		LOGF_UTILITY("class {}", infoClass);
		return STATUS_INVALID_INFO_CLASS;
	}
}

// NTSTATUS NtSetInformationFile(HANDLE file, IO_STATUS_BLOCK* ioStatus, void* info, uint32_t length,
//     FILE_INFORMATION_CLASS infoClass)
uint32_t NtSetInformationFile(uint32_t handle, XIO_STATUS_BLOCK* ioStatus, uint8_t* in, uint32_t length, uint32_t infoClass)
{
	NtFile* file = GetFile(handle);
	if (file == nullptr)
		return STATUS_INVALID_HANDLE;

	auto finish = [&](uint32_t size)
	{
		Complete(ioStatus, STATUS_SUCCESS, size, 0);
		return STATUS_SUCCESS;
	};

	std::lock_guard lock(file->mutex);

	if (file->disc != nullptr && infoClass != XFilePositionInformation && infoClass != XFileBasicInformation)
		return STATUS_ACCESS_DENIED;

	switch (infoClass)
	{
	case XFileBasicInformation:
		return finish(40);
	case XFileDispositionInformation:
		file->deleteOnClose = in[0] != 0;
		return finish(1);
	case XFilePositionInformation:
		file->position = *reinterpret_cast<be<uint64_t>*>(in);
		return finish(8);
	case XFileAllocationInformation:
		return finish(8);
	case XFileEndOfFileInformation:
		if (file->fd < 0 || ftruncate(file->fd, off_t(uint64_t(*reinterpret_cast<be<uint64_t>*>(in)))) != 0)
			return STATUS_ACCESS_DENIED;
		return finish(8);
	case XFileRenameInformation:
	{
		// { BOOLEAN ReplaceIfExists; HANDLE RootDirectory; ANSI_STRING FileName; }
		auto* name = reinterpret_cast<XANSI_STRING*>(in + 8);
		XOBJECT_ATTRIBUTES target{};
		target.RootDirectory = *reinterpret_cast<be<uint32_t>*>(in + 4);
		target.Name = name;
		const auto newPath = FileSystem::ResolvePath(GuestPath(&target), false);

		std::error_code ec;
		std::filesystem::rename(file->hostPath, newPath, ec);
		if (ec)
			return STATUS_ACCESS_DENIED;

		file->hostPath = newPath;
		file->guestPath = GuestPath(&target);
		return finish(0);
	}
	default:
		LOGF_UTILITY("class {}", infoClass);
		return STATUS_INVALID_INFO_CLASS;
	}
}

// NTSTATUS NtQueryFullAttributesFile(OBJECT_ATTRIBUTES* attributes, FILE_NETWORK_OPEN_INFORMATION* info)
uint32_t NtQueryFullAttributesFile(XOBJECT_ATTRIBUTES* attributes, XFILE_NETWORK_OPEN_INFORMATION* out)
{
	const std::string guestPath = GuestPath(attributes);
	const auto hostPath = FileSystem::ResolvePath(guestPath, false);

	struct stat info{};
	if (!StatPath(hostPath, info))
	{
		fmt::println(stderr, "[file] attributes {} -> not found", guestPath);
		return STATUS_OBJECT_NAME_NOT_FOUND;
	}

	FillNetworkOpenInfo(out, info);
	return STATUS_SUCCESS;
}

// NTSTATUS NtQueryVolumeInformationFile(HANDLE file, IO_STATUS_BLOCK* ioStatus, void* info,
//     uint32_t length, FS_INFORMATION_CLASS infoClass)
uint32_t NtQueryVolumeInformationFile(uint32_t handle, XIO_STATUS_BLOCK* ioStatus, uint8_t* out, uint32_t length, uint32_t infoClass)
{
	NtFile* file = GetFile(handle);
	if (file == nullptr)
		return STATUS_INVALID_HANDLE;

	memset(out, 0, length);
	uint32_t size = 0;

	switch (infoClass)
	{
	case 1: // FileFsVolumeInformation: creation time, serial, label length, supports objects, label
		if (length < 24) return STATUS_BUFFER_TOO_SMALL;
		*reinterpret_cast<be<uint32_t>*>(out + 8) = 0x12345678;
		*reinterpret_cast<be<uint32_t>*>(out + 12) = 0;
		size = 24;
		break;
	case 3: // FileFsSizeInformation: total units, free units, sectors per unit, bytes per sector
	{
		if (length < 24) return STATUS_BUFFER_TOO_SMALL;
		std::error_code ec;
		const auto space = std::filesystem::space(file->hostPath, ec);
		const uint64_t unit = 0x10000;
		*reinterpret_cast<be<uint64_t>*>(out) = ec ? 0x10000 : space.capacity / unit;
		*reinterpret_cast<be<uint64_t>*>(out + 8) = ec ? 0x8000 : space.available / unit;
		*reinterpret_cast<be<uint32_t>*>(out + 16) = unit / 0x200;
		*reinterpret_cast<be<uint32_t>*>(out + 20) = 0x200;
		size = 24;
		break;
	}
	case 4: // FileFsDeviceInformation: device type, characteristics
		if (length < 8) return STATUS_BUFFER_TOO_SMALL;
		*reinterpret_cast<be<uint32_t>*>(out) = 0x7; // FILE_DEVICE_DISK
		size = 8;
		break;
	case 5: // FileFsAttributeInformation: attributes, max name length, name length, name
	{
		constexpr std::string_view fsName = "FATX";
		if (length < 12 + fsName.size()) return STATUS_BUFFER_TOO_SMALL;
		*reinterpret_cast<be<uint32_t>*>(out) = 0x2; // FILE_CASE_PRESERVED_NAMES
		*reinterpret_cast<be<uint32_t>*>(out + 4) = 42;
		*reinterpret_cast<be<uint32_t>*>(out + 8) = uint32_t(fsName.size());
		memcpy(out + 12, fsName.data(), fsName.size());
		size = 12 + uint32_t(fsName.size());
		break;
	}
	default:
		LOGF_UTILITY("class {}", infoClass);
		return STATUS_INVALID_INFO_CLASS;
	}

	Complete(ioStatus, STATUS_SUCCESS, size, 0);
	return STATUS_SUCCESS;
}

// NTSTATUS NtQueryDirectoryFile(HANDLE file, HANDLE event, PIO_APC_ROUTINE apcRoutine, void* apcContext,
//     IO_STATUS_BLOCK* ioStatus, FILE_DIRECTORY_INFORMATION* info, uint32_t length, ANSI_STRING* mask,
//     BOOLEAN restartScan)
// Returns one entry per call, which is all the 360's own callers ask for.
uint32_t NtQueryDirectoryFile(uint32_t handle, uint32_t event, uint32_t apcRoutine, uint32_t apcContext, XIO_STATUS_BLOCK* ioStatus,
	uint8_t* out, uint32_t length, XANSI_STRING* mask, uint32_t restartScan)
{
	NtFile* file = GetFile(handle);
	if (file == nullptr || !file->directory)
		return STATUS_INVALID_HANDLE;

	std::lock_guard lock(file->mutex);

	if (!file->listed || (restartScan & 0xFF))
	{
		const std::string pattern = mask != nullptr ? GuestString(mask) : "*";
		file->entries.clear();
		file->nextEntry = 0;
		file->listed = true;

		if (file->disc != nullptr)
		{
			for (const auto& child : file->discEntry->children)
			{
				if (MatchMask(pattern.empty() ? "*" : pattern, child->name))
					file->entries.push_back({ child->name, DiscStat(*child) });
			}
		}
		else if (DIR* dir = opendir(file->hostPath.c_str()))
		{
			while (dirent* entry = readdir(dir))
			{
				const std::string_view name = entry->d_name;
				if (name == "." || name == ".." || !MatchMask(pattern.empty() ? "*" : pattern, name))
					continue;

				DirectoryEntry item{ std::string(name), {} };
				if (stat((file->hostPath / item.name).c_str(), &item.info) == 0)
					file->entries.push_back(std::move(item));
			}
			closedir(dir);
		}

		std::sort(file->entries.begin(), file->entries.end(), [](auto& a, auto& b) { return a.name < b.name; });

		if (file->entries.empty())
		{
			Complete(ioStatus, STATUS_NO_SUCH_FILE, 0, event);
			return STATUS_NO_SUCH_FILE;
		}
	}

	if (file->nextEntry >= file->entries.size())
	{
		Complete(ioStatus, STATUS_NO_MORE_FILES, 0, event);
		return STATUS_NO_MORE_FILES;
	}

	const DirectoryEntry& entry = file->entries[file->nextEntry];
	constexpr uint32_t nameOffset = 64;
	if (length < nameOffset + entry.name.size())
		return STATUS_BUFFER_TOO_SMALL;

	file->nextEntry++;

	// FILE_DIRECTORY_INFORMATION
	memset(out, 0, nameOffset);
	auto* fields = reinterpret_cast<be<uint64_t>*>(out);
	const uint64_t time = FileTime(ModifiedTime(entry.info));
	const uint64_t size = S_ISDIR(entry.info.st_mode) ? 0 : uint64_t(entry.info.st_size);
	*reinterpret_cast<be<uint32_t>*>(out + 0) = 0; // NextEntryOffset: last entry
	*reinterpret_cast<be<uint32_t>*>(out + 4) = uint32_t(file->nextEntry - 1);
	fields[1] = time;
	fields[2] = time;
	fields[3] = time;
	fields[4] = time;
	fields[5] = size;
	fields[6] = AllocationSize(size);
	*reinterpret_cast<be<uint32_t>*>(out + 56) = Attributes(entry.info);
	*reinterpret_cast<be<uint32_t>*>(out + 60) = uint32_t(entry.name.size());
	memcpy(out + nameOffset, entry.name.data(), entry.name.size());

	Complete(ioStatus, STATUS_SUCCESS, nameOffset + uint32_t(entry.name.size()), event);
	return STATUS_SUCCESS;
}

// NTSTATUS NtFlushBuffersFile(HANDLE file, IO_STATUS_BLOCK* ioStatus)
uint32_t NtFlushBuffersFile(uint32_t handle, XIO_STATUS_BLOCK* ioStatus)
{
	if (NtFile* file = GetFile(handle); file != nullptr && file->fd >= 0)
		fsync(file->fd);

	Complete(ioStatus, STATUS_SUCCESS, 0, 0);
	return STATUS_SUCCESS;
}

// NTSTATUS NtDuplicateObject(HANDLE source, HANDLE* target, uint32_t options)
// Hands back the same handle and counts it, so the object lives until the last NtClose.
uint32_t NtDuplicateObject(uint32_t source, be<uint32_t>* target, uint32_t options)
{
	if (target != nullptr)
		*target = source;

	if (IsKernelObject(source) && source != CURRENT_THREAD_HANDLE && source != GUEST_INVALID_HANDLE_VALUE)
	{
		std::lock_guard lock(g_duplicateMutex);
		g_duplicates[source]++;
	}

	// DUPLICATE_CLOSE_SOURCE
	if (options & 1)
		ReleaseDuplicatedHandle(source);

	return STATUS_SUCCESS;
}

// NTSTATUS ObCreateSymbolicLink(ANSI_STRING* link, ANSI_STRING* target)
// "\??\name:" -> a device path: registers "name" as a root for path resolution.
uint32_t ObCreateSymbolicLink(XANSI_STRING* link, XANSI_STRING* target)
{
	std::string name = GuestString(link);
	const std::string device = GuestString(target);

	if (name.starts_with("\\??\\"))
		name = name.substr(4);
	if (!name.empty() && name.back() == ':')
		name.pop_back();

	const auto hostPath = FileSystem::ResolvePath(device, false);
	fmt::println(stderr, "[file] link {}: -> {} ({})", name, device, (const char*)hostPath.u8string().c_str());
	XamRootCreate(name, (const char*)hostPath.u8string().c_str());
	return STATUS_SUCCESS;
}

// NTSTATUS ObDeleteSymbolicLink(ANSI_STRING* link)
uint32_t ObDeleteSymbolicLink(XANSI_STRING* link)
{
	return STATUS_SUCCESS;
}

GUEST_FUNCTION_HOOK(__imp__NtCreateFile, NtCreateFile);
GUEST_FUNCTION_HOOK(__imp__NtOpenFile, NtOpenFile);
GUEST_FUNCTION_HOOK(__imp__NtReadFile, NtReadFile);
GUEST_FUNCTION_HOOK(__imp__NtReadFileScatter, NtReadFileScatter);
GUEST_FUNCTION_HOOK(__imp__NtWriteFile, NtWriteFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryInformationFile, NtQueryInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtSetInformationFile, NtSetInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryFullAttributesFile, NtQueryFullAttributesFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryVolumeInformationFile, NtQueryVolumeInformationFile);
GUEST_FUNCTION_HOOK(__imp__NtQueryDirectoryFile, NtQueryDirectoryFile);
GUEST_FUNCTION_HOOK(__imp__NtFlushBuffersFile, NtFlushBuffersFile);
GUEST_FUNCTION_HOOK(__imp__NtDuplicateObject, NtDuplicateObject);
GUEST_FUNCTION_HOOK(__imp__ObCreateSymbolicLink, ObCreateSymbolicLink);
GUEST_FUNCTION_HOOK(__imp__ObDeleteSymbolicLink, ObDeleteSymbolicLink);
