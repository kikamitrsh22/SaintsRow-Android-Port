// xexdump: decrypt + decompress an XEX2 and write the raw PE image (as loaded at image base).
// Usage: xexdump <input.xex> <output.bin>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <image.h>
#include <xex.h>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("Usage: xexdump <input.xex> <output.bin>\n");
        return 1;
    }

    std::ifstream in(argv[1], std::ios::binary);
    if (!in)
    {
        std::printf("Cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    Image image = Xex2LoadImage(file.data(), file.size());
    if (image.sections.empty())
    {
        std::printf("Failed to load XEX image\n");
        return 1;
    }

    std::printf("base=0x%zX size=0x%X entry=0x%zX\n", image.base, image.size, image.entry_point);
    for (const auto& s : image.sections)
        std::printf("  %-10s 0x%08zX size=0x%08X flags=%u\n", s.name.c_str(), s.base, s.size, static_cast<unsigned>(s.flags));

    // Rebuild the flat image from the section pointers (they point into the decrypted buffer).
    std::vector<uint8_t> out(image.size, 0);
    const auto& first = *image.sections.begin();
    const size_t headerSize = first.base - image.base;
    std::memcpy(out.data(), first.data - headerSize, headerSize);
    for (const auto& s : image.sections)
    {
        const size_t off = s.base - image.base;
        if (s.data != nullptr && off + s.size <= out.size())
            std::memcpy(out.data() + off, s.data, s.size);
    }

    FILE* f = std::fopen(argv[2], "wb");
    if (f == nullptr)
    {
        std::perror("fopen");
        return 1;
    }
    const size_t written = std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
    if (written != out.size())
    {
        std::perror("fwrite");
        return 1;
    }
    std::printf("Wrote %zu bytes to %s\n", written, argv[2]);
    return 0;
}
