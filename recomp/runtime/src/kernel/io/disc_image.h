// Ips3: read-only access to an Xbox 360 disc image (XDVDFS), so the game can run straight from
// the .iso without extracting it first.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace DiscImage
{
	struct Entry
	{
		std::string name;     // as stored on the disc
		uint64_t offset = 0;  // byte offset of the data in the image file
		uint64_t size = 0;
		bool directory = false;
		std::vector<std::unique_ptr<Entry>> children;
	};

	struct Disc
	{
		std::filesystem::path path;
		int fd = -1;
		Entry root;
	};

	// Opens and indexes an image. Null if the file is not an XDVDFS image.
	std::shared_ptr<Disc> Open(const std::filesystem::path& path);
	// Why the last Open failed, in words for the player.
	std::string LastError();

	// Finds the disc mounted at `hostPath` or one of its parents (paths below an image name
	// files inside it) and the entry for the rest of the path, matched without regard to case.
	// Returns null when `hostPath` is not inside a mounted image.
	std::shared_ptr<Disc> Lookup(const std::filesystem::path& hostPath, const Entry** entry);

	// Makes `path` available to Lookup.
	void Mount(std::shared_ptr<Disc> disc);

	bool IsImage(const std::filesystem::path& path);
}
