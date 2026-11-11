#pragma once
// Bridge between the display/input options and the app's key/value config (src/app/config.*),
// so these modules do not depend on how the launcher stores settings.
#include <functional>
#include <string>

namespace supersaga {

struct SettingsIO {
    std::function<std::string(const std::string& key, const std::string& fallback)> get;
    std::function<void(const std::string& key, const std::string& value)> set;
    std::function<void()> save;  // persist to disk
};

}  // namespace supersaga
