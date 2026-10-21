// Ips3: XDVDFS reader.
//
// An Xbox 360 game disc is an XDVDFS volume (2 KiB sectors, little-endian) that starts at a
// fixed offset that depends on the disc format: 0 for rebuilt images, then the XGD1, XGD2 and XGD3
// game partition offsets. Its volume descriptor is at sector 32 of the partition; each directory
// is a binary tree of entries
//     u16 left, u16 right (subtree offsets in 4-byte units), u32 sector, u32 size,
//     u8 attributes, u8 name length, name
// padded to 4 bytes.
#include <stdafx.h>

#include "disc_image.h"

#include <unordered_set>

#include <fcntl.h>
#include <unistd.h>

namespace
{
	constexpr uint64_t SECTOR_SIZE = 0x800;
	constexpr char MAGIC[] = "MICROSOFT*XBOX*MEDIA";
	constexpr uint64_t PARTITION_OFFSETS[] = { 0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000, 0x18300000 };

	std::mutex g_mutex;
	std::vector<std::shared_ptr<DiscImage::Disc>> g_discs;

	bool ReadAt(int fd, uint64_t offset, void* buffer, size_t size)
	{
		size_t done = 0;
		while (done < size)
		{
			const ssize_t count = pread(fd, static_cast<uint8_t*>(buffer) + done, size - done, off_t(offset + done));
			if (count <= 0)
				return false;
			done += size_t(count);
		}
		return true;
	}

	uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
	uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

	bool EqualsNoCase(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() && strncasecmp(a.data(), b.data(), a.size()) == 0;
	}

	// Reads the directory table at `sector` into `parent`'s children, recursing into
	// subdirectories. `depth` guards against corrupt images that loop.
	bool ReadDirectory(int fd, uint64_t partition, uint32_t sector, uint32_t size, DiscImage::Entry& parent, int depth)
	{
		if (size == 0)
			return true;
		if (depth > 64 || size > 64 * 1024 * 1024)
			return false;

		std::vector<uint8_t> table(size);
		if (!ReadAt(fd, partition + uint64_t(sector) * SECTOR_SIZE, table.data(), size))
			return false;

		std::vector<uint32_t> pending{ 0 };
		std::unordered_set<uint32_t> visited;

		while (!pending.empty())
		{
			const uint32_t offset = pending.back();
			pending.pop_back();

			if (offset + 14 > size || !visited.insert(offset).second)
				continue;

			const uint8_t* node = table.data() + offset;
			const uint16_t left = U16(node);
			const uint16_t right = U16(node + 2);
			if (left == 0xFFFF && right == 0xFFFF)
				continue; // sector padding

			const uint8_t nameLength = node[13];
			if (offset + 14 + nameLength > size)
				continue;

			auto entry = std::make_unique<DiscImage::Entry>();
			entry->name.assign(reinterpret_cast<const char*>(node + 14), nameLength);
			entry->directory = (node[12] & 0x10) != 0;
			const uint32_t entrySector = U32(node + 4);
			const uint32_t entrySize = U32(node + 8);

			if (entry->directory)
			{
				if (!ReadDirectory(fd, partition, entrySector, entrySize, *entry, depth + 1))
					return false;
			}
			else
			{
				entry->offset = partition + uint64_t(entrySector) * SECTOR_SIZE;
				entry->size = entrySize;
			}

			parent.children.push_back(std::move(entry));

			if (left != 0)
				pending.push_back(uint32_t(left) * 4);
			if (right != 0)
				pending.push_back(uint32_t(right) * 4);
		}

		std::sort(parent.children.begin(), parent.children.end(), [](auto& a, auto& b) { return a->name < b->name; });
		return true;
	}

	const DiscImage::Entry* Find(const DiscImage::Entry& root, const std::filesystem::path& relative)
	{
		const DiscImage::Entry* entry = &root;
		for (const auto& part : relative)
		{
			const std::string name = (const char*)part.u8string().c_str();
			if (name.empty() || name == "." || name == "/")
				continue;

			const DiscImage::Entry* next = nullptr;
			for (const auto& child : entry->children)
			{
				if (EqualsNoCase(child->name, name))
				{
					next = child.get();
					break;
				}
			}

			if (next == nullptr)
				return nullptr;
			entry = next;
		}

		return entry;
	}
}

std::shared_ptr<DiscImage::Disc> DiscImage::Open(const std::filesystem::path& path)
{
	const int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return nullptr;

	for (uint64_t partition : PARTITION_OFFSETS)
	{
		uint8_t descriptor[SECTOR_SIZE];
		if (!ReadAt(fd, partition + 32 * SECTOR_SIZE, descriptor, sizeof(descriptor)))
			continue;

		if (memcmp(descriptor, MAGIC, 20) != 0 || memcmp(descriptor + 0x7EC, MAGIC, 20) != 0)
			continue;

		auto disc = std::make_shared<Disc>();
		disc->path = path;
		disc->fd = fd;
		disc->root.directory = true;

		if (!ReadDirectory(fd, partition, U32(descriptor + 20), U32(descriptor + 24), disc->root, 0))
		{
			fmt::println(stderr, "[disc] {}: corrupt directory tree", path.string());
			break;
		}

		fmt::println(stderr, "[disc] {}: XDVDFS at 0x{:X}, {} root entries", path.string(), partition, disc->root.children.size());
		return disc;
	}

	close(fd);
	return nullptr;
}

std::shared_ptr<DiscImage::Disc> DiscImage::Lookup(const std::filesystem::path& hostPath, const Entry** entry)
{
	std::lock_guard lock(g_mutex);

	for (const auto& disc : g_discs)
	{
		auto [discEnd, pathIt] = std::mismatch(disc->path.begin(), disc->path.end(), hostPath.begin(), hostPath.end());
		if (discEnd != disc->path.end())
			continue;

		std::filesystem::path relative;
		for (; pathIt != hostPath.end(); ++pathIt)
			relative /= *pathIt;

		*entry = Find(disc->root, relative);

		// Debugging aid: IPS3_HIDE=path1;path2 makes those disc paths (case-insensitive,
		// '/'-separated, relative to the disc root) appear missing.
		if (*entry != nullptr)
		{
			if (const char* hidden = getenv("IPS3_HIDE"))
			{
				const std::string rel = (const char*)relative.u8string().c_str();
				std::string_view list = hidden;
				while (!list.empty())
				{
					const size_t end = std::min(list.find(';'), list.size());
					const std::string_view item = list.substr(0, end);
					if (EqualsNoCase(item, rel))
						*entry = nullptr;
					list.remove_prefix(std::min(end + 1, list.size()));
				}
			}
		}
		return disc;
	}

	return nullptr;
}

void DiscImage::Mount(std::shared_ptr<Disc> disc)
{
	std::lock_guard lock(g_mutex);
	g_discs.push_back(std::move(disc));
}

bool DiscImage::IsImage(const std::filesystem::path& path)
{
	std::error_code ec;
	return std::filesystem::is_regular_file(path, ec);
}
