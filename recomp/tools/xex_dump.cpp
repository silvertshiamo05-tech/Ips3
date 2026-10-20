// Loads an Xbox 360 XEX with XenonRecomp's loader (decrypts + decompresses), prints its
// sections with a rough code census, and optionally writes the flat image (loaded at
// image.base) for the Python analysis tools.
//
//   xex_dump default.xex [image.bin]

#include <image.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

static uint32_t be32(const uint8_t* p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: %s default.xex [image.bin]\n", argv[0]);
		return 1;
	}

	std::ifstream in(argv[1], std::ios::binary);
	std::vector<uint8_t> xex((std::istreambuf_iterator<char>(in)), {});
	Image image = Image::ParseImage(xex.data(), xex.size());

	if (!image.data)
	{
		std::fprintf(stderr, "could not load %s\n", argv[1]);
		return 1;
	}

	std::printf("base %zx, size %x, entry %zx, %zu sections\n", image.base, image.size, image.entry_point, image.sections.size());

	for (const Section& s : image.sections)
	{
		const size_t offset = s.data - image.data.get();
		const uint32_t limit = offset < image.size ? std::min<uint32_t>(s.size, image.size - uint32_t(offset)) : 0;
		size_t prologues = 0, returns = 0;

		for (uint32_t i = 0; i + 4 <= limit; i += 4)
		{
			const uint32_t w = be32(s.data + i);
			prologues += w == 0x7D8802A6; // mflr r12
			returns += w == 0x4E800020;   // blr
		}

		std::printf("%-10s %08zx-%08zx  flags %x  mflr r12: %zu  blr: %zu\n",
			s.name.c_str(), s.base, s.base + s.size, unsigned(s.flags), prologues, returns);
	}

	if (argc > 2)
	{
		std::FILE* out = std::fopen(argv[2], "wb");
		std::fwrite(image.data.get(), 1, image.size, out);
		std::fclose(out);
	}

	return 0;
}
