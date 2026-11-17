// Maps Xbox 360 paths (game:\, d:\, \Device\Cdrom0\, cache:\ ...) to host paths.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "kernel/kernel.h"

namespace vfs
{
    void Init(const std::filesystem::path& gameDir, const std::filesystem::path& userDir);
    // Host folder for saves (content created with XamContentCreate): <userDir>/save.
    std::filesystem::path SaveDir();

    // Dynamic devices such as "save:" created by XamContentCreate.
    void AddMount(const std::string& rootName, const std::filesystem::path& hostDir);
    void RemoveMount(const std::string& rootName);

    // Translates a guest path to a host path. Returns false if the device is unknown.
    bool Translate(const std::string& guestPath, std::filesystem::path& out, bool* writable = nullptr);

    struct File : kernel::Object
    {
        std::filesystem::path path;
        std::FILE* fp = nullptr;
        bool isDirectory = false;
        uint64_t position = 0;
        // directory enumeration state
        std::vector<std::filesystem::directory_entry> entries;
        size_t enumIndex = 0;
        bool enumStarted = false;

        bool IsWaitable() const override { return false; }
        ~File() override { if (fp) std::fclose(fp); }
    };
}
