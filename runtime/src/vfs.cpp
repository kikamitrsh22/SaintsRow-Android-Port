#include "vfs.h"
#include "guest.h"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace vfs
{
    static std::filesystem::path s_game;
    static std::filesystem::path s_user;

    void Init(const std::filesystem::path& gameDir, const std::filesystem::path& userDir)
    {
        s_game = gameDir;
        s_user = userDir;
        std::error_code ec;
        std::filesystem::create_directories(s_user / "cache", ec);
        std::filesystem::create_directories(s_user / "save", ec);
        LOG("vfs: game=%s user=%s", s_game.string().c_str(), s_user.string().c_str());
    }

    std::filesystem::path SaveDir() { return s_user / "save"; }

    static std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    }

    static bool StartsWith(const std::string& s, const char* prefix, size_t& len)
    {
        len = std::strlen(prefix);
        return s.size() >= len && s.compare(0, len, prefix) == 0;
    }

    static std::mutex s_mountLock;
    static std::vector<std::pair<std::string, std::filesystem::path>> s_mounts; // lower-case "name:" -> dir

    void AddMount(const std::string& rootName, const std::filesystem::path& hostDir)
    {
        std::lock_guard lock(s_mountLock);
        std::string key = Lower(rootName);
        if (key.empty() || key.back() != ':') key.push_back(':');
        for (auto& m : s_mounts)
            if (m.first == key) { m.second = hostDir; return; }
        s_mounts.emplace_back(key, hostDir);
        LOG("vfs: mount %s -> %s", key.c_str(), hostDir.string().c_str());
    }

    void RemoveMount(const std::string& rootName)
    {
        std::lock_guard lock(s_mountLock);
        std::string key = Lower(rootName);
        if (key.empty() || key.back() != ':') key.push_back(':');
        std::erase_if(s_mounts, [&](auto& m) { return m.first == key; });
    }

    bool Translate(const std::string& guestPath, std::filesystem::path& out, bool* writable)
    {
        std::string p = guestPath;
        std::replace(p.begin(), p.end(), '/', '\\');
        std::string lower = Lower(p);

        {
            std::lock_guard lock(s_mountLock);
            for (auto& [key, dir] : s_mounts)
            {
                if (lower.compare(0, key.size(), key) != 0) continue;
                std::string rest = p.substr(key.size());
                while (!rest.empty() && rest.front() == '\\') rest.erase(rest.begin());
                std::replace(rest.begin(), rest.end(), '\\', '/');
                out = rest.empty() ? dir : dir / std::filesystem::path(rest);
                if (writable) *writable = true;
                return true;
            }
        }

        struct Mount { const char* prefix; int root; bool writable; };
        static const Mount kMounts[] = {
            { "\\device\\cdrom0\\", 0, false },
            { "\\device\\cdrom0", 0, false },
            { "\\device\\harddisk0\\partition1\\", 2, true },
            { "\\systemroot\\", 0, false },
            { "game:\\", 0, false },
            { "game:", 0, false },
            { "d:\\", 0, false },
            { "d:", 0, false },
            { "cache:\\", 1, true },
            { "cache:", 1, true },
            { "z:\\", 1, true },
            { "save:\\", 2, true },
            { "save:", 2, true },
            { "\\??\\", -1, false },
        };

        for (const auto& m : kMounts)
        {
            size_t len;
            if (!StartsWith(lower, m.prefix, len))
                continue;
            if (m.root == -1)
                return Translate(p.substr(len), out, writable);

            std::string rest = p.substr(len);
            while (!rest.empty() && rest.front() == '\\') rest.erase(rest.begin());
            std::filesystem::path base = m.root == 0 ? s_game : m.root == 1 ? s_user / "cache" : s_user / "save";
            std::replace(rest.begin(), rest.end(), '\\', '/');
            out = rest.empty() ? base : base / std::filesystem::path(rest);
            if (writable) *writable = m.writable;
            return true;
        }

        // Relative path (no device): treat as relative to the game folder.
        if (lower.find(':') == std::string::npos && (lower.empty() || lower[0] != '\\'))
        {
            std::replace(p.begin(), p.end(), '\\', '/');
            out = s_game / std::filesystem::path(p);
            if (writable) *writable = false;
            return true;
        }

        return false;
    }
}
