// Portable start-up shared by every platform's entry point (platform/*.mm, platform/android.cpp).
#pragma once

#include <string>
#include <vector>

struct AppConfig
{
    std::string hostExeDir; // directory holding the host executables (kpintro next to kingpin)
    std::string gameDir;    // installed game files (main/, kingpin.exe, ...)
    std::string discDir;    // disc contents (D:\)
    std::string cuePath;    // optional: CD image .cue for the music tracks
    std::vector<std::string> args;
};

bool appInit(const AppConfig& cfg); // loads the executable; false on failure
void appStartGuest();               // starts the guest main thread (returns at once)
void hostExit(int code);
