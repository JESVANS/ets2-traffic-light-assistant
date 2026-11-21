#pragma once

#include "types.hpp"
#include <string>

namespace tlhud {

const char* GetDefaultIniContent();
bool LoadConfig(const std::wstring& path, PluginConfig& out);
std::string SanitizeVehicleId(const std::string& raw);
bool LoadVehicleHudOverrides(const std::wstring& path,
                             const std::string& vehicle_id,
                             HudConfig& hud);
bool SaveConfigValue(const std::wstring& path,
                     const std::string& section,
                     const std::string& key,
                     const std::string& value);

} // namespace tlhud
