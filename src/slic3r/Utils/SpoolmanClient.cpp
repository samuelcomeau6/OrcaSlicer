#include "SpoolmanClient.hpp"

#include "libslic3r/AppConfig.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

namespace Slic3r
{
namespace
{

std::string DescribeError(const std::string& body, const std::string& error, unsigned status)
{
    if (!error.empty())
        return error;
    std::string message = "HTTP " + std::to_string(status);
    if (!body.empty())
        message += ": " + body.substr(0, 200);
    return message;
}

Http MakeRequest(const std::string& url)
{
    Http http = Http::get(url);
    http.timeout_connect(5)
        .timeout_max(30)
        // Large inventories easily exceed the 5 MB default.
        .size_limit(64 * 1024 * 1024)
        .header("Accept", "application/json");
    return http;
}

} // namespace

std::string SpoolmanClient::ConfiguredUrl(const AppConfig& config)
{
    return NormalizeSpoolmanUrl(config.get(ConfigKey));
}

Http::Ptr SpoolmanClient::FetchSpools(const std::string& baseUrl, SpoolsFn onSpools, ErrorFn onError)
{
    const std::string url = baseUrl + "/api/v1/spool?allow_archived=false";
    BOOST_LOG_TRIVIAL(info) << "Spoolman: fetching spools from " << url;

    Http http = MakeRequest(url);
    http.on_complete([onSpools, onError](std::string body, unsigned)
        {
            std::vector<SpoolmanSpool> spools;
            std::string error;
            if (!ParseSpoolmanSpools(body, spools, error))
            {
                BOOST_LOG_TRIVIAL(warning) << "Spoolman: " << error;
                onError(error);
                return;
            }
            onSpools(std::move(spools));
        })
        .on_error([onError](std::string body, std::string error, unsigned status)
        {
            const std::string message = DescribeError(body, error, status);
            BOOST_LOG_TRIVIAL(warning) << "Spoolman: fetching spools failed: " << message;
            onError(message);
        });
    return http.perform();
}

Http::Ptr SpoolmanClient::FetchInfo(const std::string& baseUrl, InfoFn onInfo, ErrorFn onError)
{
    Http http = MakeRequest(baseUrl + "/api/v1/info");
    http.on_complete([onInfo, onError](std::string body, unsigned)
        {
            const nlohmann::json root = nlohmann::json::parse(body, nullptr, false);
            if (root.is_discarded() || !root.is_object() || !root.contains("version"))
            {
                onError("The server answered, but it does not look like Spoolman");
                return;
            }
            onInfo(root["version"].is_string() ? root["version"].get<std::string>() : std::string());
        })
        .on_error([onError](std::string body, std::string error, unsigned status)
        {
            onError(DescribeError(body, error, status));
        });
    return http.perform();
}

} // namespace Slic3r
