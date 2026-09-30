#pragma once

#include "FilamentColorLibrary.hpp"

#include <string>
#include <vector>

namespace Slic3r
{

/**
 * @brief One spool as reported by a Spoolman server (GET /api/v1/spool).
 */
struct SpoolmanSpool
{
    int id { 0 };
    int filamentId { 0 };
    std::string filamentName; // Spoolman's filament "name", usually the colour/product name.
    std::string vendor;
    std::string material;
    std::string colorName;    // Extra field "color_name"/"colour_name" when set, else the filament name.
    FilamentColor color;      // Empty when the filament has no colour in Spoolman.
    double remainingWeight { -1.0 }; // grams, negative when unknown
    double initialWeight { -1.0 };   // grams, negative when unknown
    double diameter { 0.0 };
    std::string location;
    std::string lotNr;
    std::string comment;
    std::string lastUsed;     // ISO 8601 timestamp from Spoolman, empty when never used.
    bool archived { false };

    /**
     * @brief Human readable one-line label, e.g. "#12 Polymaker PolyTerra PLA - Charcoal Black".
     */
    std::string DisplayName() const;

    /**
     * @brief Lower-cased text used for filtering spools by a search string.
     */
    std::string SearchText() const;
};

/**
 * @brief Normalizes a user-entered Spoolman address.
 *
 * Trims whitespace, adds "http://" when no scheme is given and strips trailing
 * slashes and a trailing "/api/v1". Returns an empty string for empty input.
 */
std::string NormalizeSpoolmanUrl(const std::string& url);

/**
 * @brief Parses the JSON body of GET /api/v1/spool.
 *
 * Spools are returned sorted by id. Returns false and fills @p error when the
 * body is not a JSON array of spools.
 */
bool ParseSpoolmanSpools(const std::string& body, std::vector<SpoolmanSpool>& spools, std::string& error);

/**
 * @brief Returns true when every whitespace separated word of @p filter occurs in the spool's search text.
 */
bool SpoolmanSpoolMatches(const SpoolmanSpool& spool, const std::string& filter);

/**
 * @brief Returns true when a Spoolman material belongs to a slicer filament type.
 *
 * Both are split into upper-cased alphanumeric words and every word of @p filamentType
 * must appear in @p material: "PLA" matches "PLA", "PLA+" and "Silk PLA", "PLA-CF"
 * matches "PLA CF" but not plain "PLA". An empty filament type matches everything;
 * a spool without a material only matches an empty filament type.
 */
bool SpoolmanMaterialMatches(const std::string& material, const std::string& filamentType);

enum class SpoolmanSortOrder
{
    Id,          // ascending spool number
    RecentlyUsed // most recently used first; never used spools follow, newest (highest id) first
};

/**
 * @brief Sorts spools in place.
 */
void SortSpoolmanSpools(std::vector<SpoolmanSpool>& spools, SpoolmanSortOrder order);

/**
 * @brief Turns a colour name into something safe for file names and G-code comments.
 *
 * Removes characters that are invalid in file names on any platform, collapses
 * whitespace and trims the result.
 */
std::string SanitizeFilamentColourName(const std::string& name);

} // namespace Slic3r
