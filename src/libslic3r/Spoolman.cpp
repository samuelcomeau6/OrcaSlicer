#include "Spoolman.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

namespace Slic3r
{
namespace
{

using json = nlohmann::json;

std::string TrimCopy(const std::string& value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string JsonString(const json& object, const char* key)
{
    const json::const_iterator it = object.find(key);
    if (it == object.end() || !it->is_string())
        return {};
    return it->get<std::string>();
}

double JsonNumber(const json& object, const char* key, double fallback)
{
    const json::const_iterator it = object.find(key);
    if (it == object.end() || !it->is_number())
        return fallback;
    return it->get<double>();
}

int JsonInt(const json& object, const char* key)
{
    const json::const_iterator it = object.find(key);
    if (it == object.end() || !it->is_number_integer())
        return 0;
    return it->get<int>();
}

// Spoolman stores custom "extra" fields as JSON-encoded strings, so a text field
// holding Galaxy Black arrives as "\"Galaxy Black\"".
std::string ExtraText(const json& object, const char* key)
{
    const json::const_iterator extra = object.find("extra");
    if (extra == object.end() || !extra->is_object())
        return {};

    const json::const_iterator it = extra->find(key);
    if (it == extra->end() || !it->is_string())
        return {};

    const std::string raw = it->get<std::string>();
    const json decoded = json::parse(raw, nullptr, false);
    if (!decoded.is_discarded() && decoded.is_string())
        return TrimCopy(decoded.get<std::string>());
    return TrimCopy(raw);
}

FilamentColor ParseFilamentColor(const json& filament)
{
    std::vector<std::string> colors;
    const std::string multi = JsonString(filament, "multi_color_hexes");
    if (!multi.empty())
    {
        std::stringstream stream(multi);
        std::string token;
        while (std::getline(stream, token, ','))
        {
            const std::string normalized = NormalizeFilamentHexColor(token);
            if (!normalized.empty())
                colors.emplace_back(normalized);
        }
    }

    if (colors.empty())
    {
        const std::string normalized = NormalizeFilamentHexColor(JsonString(filament, "color_hex"));
        if (!normalized.empty())
            colors.emplace_back(normalized);
    }

    if (colors.empty())
        return {};

    // "longitudinal" means the colour changes along the strand (a gradient), "coaxial" means
    // the colours run side by side, which maps onto the segment display.
    const FilamentColorMode mode = JsonString(filament, "multi_color_direction") == "longitudinal" ?
        FilamentColorMode::Gradient : FilamentColorMode::Segment;
    return FilamentColor::FromColors(colors, mode);
}

bool ParseSpool(const json& item, SpoolmanSpool& spool)
{
    if (!item.is_object())
        return false;

    spool.id = JsonInt(item, "id");
    if (spool.id <= 0)
        return false;

    spool.remainingWeight = JsonNumber(item, "remaining_weight", -1.0);
    spool.initialWeight = JsonNumber(item, "initial_weight", -1.0);
    spool.location = TrimCopy(JsonString(item, "location"));
    spool.lotNr = TrimCopy(JsonString(item, "lot_nr"));
    spool.comment = TrimCopy(JsonString(item, "comment"));
    spool.lastUsed = TrimCopy(JsonString(item, "last_used"));
    const json::const_iterator archived = item.find("archived");
    spool.archived = archived != item.end() && archived->is_boolean() && archived->get<bool>();

    const json::const_iterator filament = item.find("filament");
    if (filament != item.end() && filament->is_object())
    {
        spool.filamentId = JsonInt(*filament, "id");
        spool.filamentName = TrimCopy(JsonString(*filament, "name"));
        spool.material = TrimCopy(JsonString(*filament, "material"));
        spool.diameter = JsonNumber(*filament, "diameter", 0.0);
        spool.color = ParseFilamentColor(*filament);
        if (spool.initialWeight < 0.0)
            spool.initialWeight = JsonNumber(*filament, "weight", -1.0);

        const json::const_iterator vendor = filament->find("vendor");
        if (vendor != filament->end() && vendor->is_object())
            spool.vendor = TrimCopy(JsonString(*vendor, "name"));

        spool.colorName = ExtraText(*filament, "color_name");
        if (spool.colorName.empty())
            spool.colorName = ExtraText(*filament, "colour_name");
    }

    // A spool-level override wins over the filament.
    const std::string spoolColorName = ExtraText(item, "color_name");
    if (!spoolColorName.empty())
        spool.colorName = spoolColorName;
    if (spool.colorName.empty())
        spool.colorName = spool.filamentName;

    return true;
}

} // namespace

std::string SpoolmanSpool::DisplayName() const
{
    std::string name = "#" + std::to_string(id);
    for (const std::string* part : { &vendor, &filamentName })
        if (!part->empty())
            name += " " + *part;
    if (!material.empty() && filamentName.find(material) == std::string::npos)
        name += " " + material;
    if (!colorName.empty() && colorName != filamentName)
        name += " - " + colorName;
    return name;
}

std::string SpoolmanSpool::SearchText() const
{
    std::string text = "#" + std::to_string(id);
    for (const std::string* part : { &vendor, &filamentName, &material, &colorName, &location, &lotNr, &comment })
        if (!part->empty())
            text += " " + *part;
    for (const std::string& hex : color.colors)
        text += " " + hex;
    return ToLowerAscii(text);
}

std::string NormalizeSpoolmanUrl(const std::string& url)
{
    std::string value = TrimCopy(url);
    if (value.empty())
        return {};

    if (value.find("://") == std::string::npos)
        value = "http://" + value;

    while (!value.empty() && value.back() == '/')
        value.pop_back();

    const std::string apiSuffix = "/api/v1";
    if (value.size() > apiSuffix.size() &&
        ToLowerAscii(value.substr(value.size() - apiSuffix.size())) == apiSuffix)
        value.erase(value.size() - apiSuffix.size());

    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}

bool ParseSpoolmanSpools(const std::string& body, std::vector<SpoolmanSpool>& spools, std::string& error)
{
    spools.clear();
    const json root = json::parse(body, nullptr, false);
    if (root.is_discarded())
    {
        error = "Spoolman returned invalid JSON";
        return false;
    }
    if (!root.is_array())
    {
        error = "Spoolman returned an unexpected response";
        return false;
    }

    for (const json& item : root)
    {
        SpoolmanSpool spool;
        if (ParseSpool(item, spool))
            spools.emplace_back(std::move(spool));
    }

    std::sort(spools.begin(), spools.end(),
              [](const SpoolmanSpool& left, const SpoolmanSpool& right) { return left.id < right.id; });
    return true;
}

bool SpoolmanSpoolMatches(const SpoolmanSpool& spool, const std::string& filter)
{
    const std::string text = spool.SearchText();
    std::stringstream stream(ToLowerAscii(filter));
    std::string word;
    while (stream >> word)
        if (text.find(word) == std::string::npos)
            return false;
    return true;
}

namespace
{

std::vector<std::string> MaterialWords(const std::string& value)
{
    std::vector<std::string> words;
    std::string word;
    for (const char ch : value)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch))
        {
            word += static_cast<char>(std::toupper(uch));
        }
        else if (!word.empty())
        {
            words.emplace_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty())
        words.emplace_back(std::move(word));
    return words;
}

} // namespace

bool SpoolmanMaterialMatches(const std::string& material, const std::string& filamentType)
{
    const std::vector<std::string> typeWords = MaterialWords(filamentType);
    if (typeWords.empty())
        return true;

    const std::vector<std::string> materialWords = MaterialWords(material);
    return std::all_of(typeWords.begin(), typeWords.end(), [&materialWords](const std::string& word)
    {
        return std::find(materialWords.begin(), materialWords.end(), word) != materialWords.end();
    });
}

void SortSpoolmanSpools(std::vector<SpoolmanSpool>& spools, SpoolmanSortOrder order)
{
    if (order == SpoolmanSortOrder::Id)
    {
        std::sort(spools.begin(), spools.end(),
                  [](const SpoolmanSpool& left, const SpoolmanSpool& right) { return left.id < right.id; });
        return;
    }

    // Spoolman serializes timestamps as ISO 8601 in UTC, so they order correctly as strings.
    std::sort(spools.begin(), spools.end(), [](const SpoolmanSpool& left, const SpoolmanSpool& right)
    {
        if (left.lastUsed.empty() != right.lastUsed.empty())
            return !left.lastUsed.empty();
        if (left.lastUsed != right.lastUsed)
            return left.lastUsed > right.lastUsed;
        return left.id > right.id;
    });
}

std::string SanitizeFilamentColourName(const std::string& name)
{
    std::string result;
    result.reserve(name.size());
    bool pendingSpace = false;
    for (const char ch : name)
    {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isspace(uch))
        {
            pendingSpace = !result.empty();
            continue;
        }
        if (uch < 0x20 || std::strchr("<>:\"/\\|?*;", ch) != nullptr)
            continue;
        if (pendingSpace)
            result += ' ';
        pendingSpace = false;
        result += ch;
    }
    return result;
}

} // namespace Slic3r
