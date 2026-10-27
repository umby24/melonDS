#pragma once
// Configuration for our implementation of melonDS's Platform:: interface
// (mds_upstream/src/Platform.h). See melon_platform.cpp and emu_host.cpp.

#include <string>

namespace melon_platform {

// Directory that relative "local" paths (Platform::OpenLocalFile) resolve against.
// The host points this at the system/ dir.
void set_local_dir(const std::string& dir);

}  // namespace melon_platform
