// Ips3: the title module as the kernel describes it to the game.
#pragma once

#include <cstddef>
#include <cstdint>

struct Image;

namespace Module
{
	// Copies the XEX header into guest memory, builds the loader entry the game reaches through
	// XexExecutableModuleHandle, and points every data (variable) import at host-provided
	// storage. Call after the image is in guest memory.
	void Init(const uint8_t* xex, size_t xexSize, const Image& image);

	// Guest address of the title's loader entry (its module handle).
	uint32_t Handle();

	// Guest address of the XEX header copy.
	uint32_t XexHeader();
}
