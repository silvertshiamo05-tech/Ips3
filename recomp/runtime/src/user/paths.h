// Where the runtime keeps per-user data. UnleashedRecomp derives these from the OS user
// directory; here the host decides (the iOS app passes its Documents directory).
#pragma once

#include <filesystem>

const std::filesystem::path& GetGamePath();
std::filesystem::path GetSavePath(bool checkForMods);
