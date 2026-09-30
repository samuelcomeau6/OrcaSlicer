#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Spoolman.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Trimmed-down response of GET /api/v1/spool from Spoolman 0.22.
const char* SpoolsJson = R"([
  {
    "id": 31,
    "remaining_weight": 120.5,
    "initial_weight": 1000,
    "location": "Dry box 2",
    "last_used": "2026-09-20T08:30:00Z",
    "lot_nr": "L-77",
    "archived": false,
    "filament": {
      "id": 4,
      "name": "Silk Rainbow",
      "material": "PLA",
      "diameter": 1.75,
      "multi_color_hexes": "FF0000,00ff00,0000FF",
      "multi_color_direction": "longitudinal",
      "vendor": { "id": 2, "name": "Eryone" }
    }
  },
  {
    "id": 12,
    "remaining_weight": 742,
    "filament": {
      "id": 1,
      "name": "PolyTerra PLA",
      "material": "PLA",
      "weight": 1000,
      "color_hex": "333333",
      "vendor": { "id": 1, "name": "Polymaker" },
      "extra": { "color_name": "\"Charcoal Black\"" }
    }
  },
  { "id": 40, "filament": { "id": 9, "name": "Mystery", "vendor": null, "color_hex": null } },
  { "name": "not a spool" }
])";

} // namespace

TEST_CASE("Spoolman spool list is parsed", "[Spoolman]")
{
    std::vector<SpoolmanSpool> spools;
    std::string error;
    REQUIRE(ParseSpoolmanSpools(SpoolsJson, spools, error));
    REQUIRE(spools.size() == 3);

    SECTION("spools are sorted by id and invalid entries are skipped")
    {
        CHECK(spools[0].id == 12);
        CHECK(spools[1].id == 31);
        CHECK(spools[2].id == 40);
    }

    SECTION("single colour spool with a colour name extra field")
    {
        const SpoolmanSpool& spool = spools[0];
        CHECK(spool.vendor == "Polymaker");
        CHECK(spool.colorName == "Charcoal Black");
        REQUIRE(spool.color.colors.size() == 1);
        CHECK(spool.color.colors[0] == "#333333");
        CHECK_THAT(spool.remainingWeight, WithinAbs(742.0, 1e-9));
        // Falls back to the filament's net weight when the spool has no initial weight.
        CHECK_THAT(spool.initialWeight, WithinAbs(1000.0, 1e-9));
        CHECK(spool.DisplayName() == "#12 Polymaker PolyTerra PLA - Charcoal Black");
    }

    SECTION("multi colour spool maps longitudinal to a gradient and uses the name as colour name")
    {
        const SpoolmanSpool& spool = spools[1];
        REQUIRE(spool.color.colors.size() == 3);
        CHECK(spool.color.colors[1] == "#00FF00");
        CHECK(spool.color.mode == FilamentColorMode::Gradient);
        CHECK(spool.colorName == "Silk Rainbow");
        CHECK(spool.location == "Dry box 2");
        CHECK(spool.lastUsed == "2026-09-20T08:30:00Z");
        CHECK(spool.DisplayName() == "#31 Eryone Silk Rainbow PLA");
    }

    SECTION("spool without colour or vendor")
    {
        const SpoolmanSpool& spool = spools[2];
        CHECK(spool.color.Empty());
        CHECK(spool.vendor.empty());
        CHECK(spool.remainingWeight < 0.0);
    }
}

TEST_CASE("Spoolman parse errors are reported", "[Spoolman]")
{
    std::vector<SpoolmanSpool> spools;
    std::string error;
    CHECK_FALSE(ParseSpoolmanSpools("<html>nope</html>", spools, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(ParseSpoolmanSpools(R"({"detail":"Not Found"})", spools, error));
    CHECK(ParseSpoolmanSpools("[]", spools, error));
    CHECK(spools.empty());
}

TEST_CASE("Spoolman search matches every word", "[Spoolman]")
{
    std::vector<SpoolmanSpool> spools;
    std::string error;
    REQUIRE(ParseSpoolmanSpools(SpoolsJson, spools, error));
    const SpoolmanSpool& polyterra = spools[0];

    CHECK(SpoolmanSpoolMatches(polyterra, ""));
    CHECK(SpoolmanSpoolMatches(polyterra, "charcoal PLA"));
    CHECK(SpoolmanSpoolMatches(polyterra, "#12"));
    CHECK(SpoolmanSpoolMatches(polyterra, "  POLYMAKER  "));
    CHECK_FALSE(SpoolmanSpoolMatches(polyterra, "charcoal petg"));
}

TEST_CASE("Spoolman materials are matched to filament types", "[Spoolman]")
{
    CHECK(SpoolmanMaterialMatches("PLA", "PLA"));
    CHECK(SpoolmanMaterialMatches("pla+", "PLA"));
    CHECK(SpoolmanMaterialMatches("Silk PLA", "PLA"));
    CHECK(SpoolmanMaterialMatches("PLA CF", "PLA-CF"));
    CHECK_FALSE(SpoolmanMaterialMatches("PLA", "PLA-CF"));
    CHECK_FALSE(SpoolmanMaterialMatches("PETG", "PLA"));
    CHECK_FALSE(SpoolmanMaterialMatches("", "PLA"));
    CHECK(SpoolmanMaterialMatches("", ""));
    CHECK(SpoolmanMaterialMatches("PETG", ""));
}

TEST_CASE("Spoolman spools can be sorted by number or recent use", "[Spoolman]")
{
    auto make = [](int id, const std::string& lastUsed) {
        SpoolmanSpool spool;
        spool.id = id;
        spool.lastUsed = lastUsed;
        return spool;
    };
    std::vector<SpoolmanSpool> spools { make(5, ""), make(2, "2026-09-01T10:00:00Z"), make(9, ""),
                                        make(7, "2026-09-20T08:30:00Z"), make(1, "2026-03-15T12:00:00Z") };

    SECTION("by number")
    {
        SortSpoolmanSpools(spools, SpoolmanSortOrder::Id);
        CHECK(spools[0].id == 1);
        CHECK(spools[1].id == 2);
        CHECK(spools[2].id == 5);
        CHECK(spools[3].id == 7);
        CHECK(spools[4].id == 9);
    }

    SECTION("most recently used first, never used spools last and newest first")
    {
        SortSpoolmanSpools(spools, SpoolmanSortOrder::RecentlyUsed);
        CHECK(spools[0].id == 7);
        CHECK(spools[1].id == 2);
        CHECK(spools[2].id == 1);
        CHECK(spools[3].id == 9);
        CHECK(spools[4].id == 5);
    }
}

TEST_CASE("Spoolman addresses are normalized", "[Spoolman]")
{
    CHECK(NormalizeSpoolmanUrl("") == "");
    CHECK(NormalizeSpoolmanUrl("   ") == "");
    CHECK(NormalizeSpoolmanUrl("192.168.1.20:7912") == "http://192.168.1.20:7912");
    CHECK(NormalizeSpoolmanUrl(" https://spoolman.lan/ ") == "https://spoolman.lan");
    CHECK(NormalizeSpoolmanUrl("http://host:7912/api/v1/") == "http://host:7912");
    CHECK(NormalizeSpoolmanUrl("http://host/spoolman/API/V1") == "http://host/spoolman");
}

TEST_CASE("Filament colour names are made file name safe", "[Spoolman]")
{
    CHECK(SanitizeFilamentColourName("  Galaxy   Black ") == "Galaxy Black");
    CHECK(SanitizeFilamentColourName("Red/Blue: \"Dual\"?") == "RedBlue Dual");
    CHECK(SanitizeFilamentColourName("a|b;c\t\nd") == "abc d");
    CHECK(SanitizeFilamentColourName("") == "");
}
