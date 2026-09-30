#pragma once

#include "Http.hpp"
#include "libslic3r/Spoolman.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Slic3r
{

class AppConfig;

/**
 * @brief Minimal client for the Spoolman REST API (https://github.com/Donkie/Spoolman).
 *
 * Requests run on a background thread; the callbacks are invoked on that thread,
 * so GUI code must marshal back to the main thread before touching widgets.
 */
class SpoolmanClient
{
public:
    // AppConfig key holding the user-entered Spoolman address. Empty disables the integration.
    static constexpr const char* ConfigKey = "spoolman_url";

    using SpoolsFn = std::function<void(std::vector<SpoolmanSpool> spools)>;
    using InfoFn = std::function<void(std::string version)>;
    using ErrorFn = std::function<void(std::string error)>;

    /**
     * @brief Returns the normalized Spoolman base URL from the app config, or an empty string.
     */
    static std::string ConfiguredUrl(const AppConfig& config);

    /**
     * @brief Fetches all non-archived spools.
     */
    static Http::Ptr FetchSpools(const std::string& baseUrl, SpoolsFn onSpools, ErrorFn onError);

    /**
     * @brief Queries /api/v1/info to verify the address points at a Spoolman server.
     */
    static Http::Ptr FetchInfo(const std::string& baseUrl, InfoFn onInfo, ErrorFn onError);
};

} // namespace Slic3r
