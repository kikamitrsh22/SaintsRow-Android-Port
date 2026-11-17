#pragma once

// Asks the OS to clock the CPU for the game's frame (Android Performance Hint; no-op elsewhere).
namespace perf
{
    // Folder with optional switch files (Android: the user data folder).
    void SetConfigDir(const char* dir);
    // Called on each guest thread when it starts (guestId 1 = the game's main thread).
    void RegisterCurrentThread(unsigned guestId);
    // True if the switch file `name` exists in the config folder (for A/B tests).
    bool Switch(const char* name);
    // Called once per presented frame.
    void OnFrame();
}
