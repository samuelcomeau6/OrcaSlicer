///|/ Anchor timelapse support.
///|/
///|/ Picks, for every printed layer, the XY position at which the timelapse
///|/ G-code block should be fired, and provides the helper that splices the
///|/ block into a finished layer's G-code at the line where the nozzle passes
///|/ that position. The layer's G-code is never otherwise altered and no move
///|/ is added.
///|/
#ifndef slic3r_GCode_AnchorTimelapse_hpp_
#define slic3r_GCode_AnchorTimelapse_hpp_

#include <cstdint>
#include <string>
#include <vector>

#include "../libslic3r.h"
#include "../ExtrusionEntity.hpp"
#include "../Point.hpp"

namespace Slic3r {

class Print;

// What the frame may be fired on, in order of preference.
//
// Firing the frame parks the nozzle on the print for as long as the camera
// needs, so it is taken on the least conspicuous line the layer has to offer.
// Anything printed over air is out - bridges and overhangs are slow, and
// stopping on one is how you get a droop - and so is a travel move, where the
// nozzle is not on the print at all. An outer wall is what the print is judged
// on, so it is a fallback: see anchor_timelapse_allowed().
enum class AnchorTimelapsePriority : uint8_t
{
    Forbidden    = 0, // never: printed over air, or not part of the object
    ExternalWall = 1, // only on a layer that prints nothing better
    Interior     = 2, // anything that is neither an outer wall nor printed over air
};

inline AnchorTimelapsePriority anchor_timelapse_role_priority(ExtrusionRole role)
{
    switch (role) {
    // Printed over air.
    case erOverhangPerimeter:
    case erBridgeInfill:
    case erInternalBridgeInfill:
    // There on one layer and gone on the next, or not a toolpath of the print
    // at all. The prime tower is deliberately not in this list: it is printed at
    // a fixed XY on every layer and it is sacrificial, which is exactly what an
    // anchor wants - see AnchorTimelapsePlanner::plan().
    case erNone:
    case erSkirt:
    case erBrim:
    case erCustom:
    case erMixed: return AnchorTimelapsePriority::Forbidden;
    case erExternalPerimeter: return AnchorTimelapsePriority::ExternalWall;
    default: return AnchorTimelapsePriority::Interior;
    }
}

// Whether a line of `prio` may take the frame on a layer whose best line
// anywhere is `best_on_layer`. A lower priority is only allowed when the layer
// prints nothing better: an outer wall is an anchor exactly on those layers
// that are all outer wall.
inline bool anchor_timelapse_allowed(AnchorTimelapsePriority best_on_layer, AnchorTimelapsePriority prio)
{
    return prio != AnchorTimelapsePriority::Forbidden && prio >= best_on_layer;
}

// Plans one XY "anchor" per printed layer: the spot at which the timelapse
// G-code block is fired so the camera sees the toolhead in (very nearly) the
// same place on every frame.
//
// Selection rules, in order:
//   * A prime tower, when the print has a real multi-filament one that reaches
//     the top of the print, is the anchor straight away. It is printed at a
//     fixed XY on every layer and it is sacrificial, so no search is needed or
//     wanted. A tower that stops early - the print finishes in one colour, so
//     there are no more tool changes to service - is not used at all: the layers
//     above it would have no anchor.
//   * Otherwise the eligible extrusions of the whole print are binned into a
//     coarse XY grid, and a dynamic program over the layers picks one cell per
//     layer. Its objective is the distance the anchor moves, plus a penalty for
//     any layer that has no eligible material at the anchor. Nothing else scores
//     - every spot that has material is as good as any other - so the plan the
//     search returns is the stillest one the geometry permits.
//   * The anchor may only step further than a few millimetres between layers on
//     a layer where its own spot has run out of material. It creeps along the
//     part; it crosses the plate only when the column it was on ends.
//
// A cell's anchor is one point, fixed for the whole print, that lies on an
// eligible extrusion. The plan keeps the anchor on cells that have material on
// the layer being printed, so the nozzle passes within a cell of it; the splice
// then fires the frame at the toolpath's closest approach.
//
// Nothing here runs unless anchor timelapse is actually selected; the planning
// happens once, at G-code export time, off the slicing path.
class AnchorTimelapsePlanner
{
public:
    // Plan the anchors for this print. Returns false when no anchor could be
    // planned (nothing eligible is extruded).
    bool plan(const Print &print);

    bool   valid() const { return m_valid; }
    bool   anchored_on_prime_tower() const { return m_prime_tower; }
    size_t planned_layer_count() const { return m_layers.size(); }
    // Largest XY step the anchor takes between two consecutive layers [mm].
    double max_drift() const { return m_max_drift; }
    // Total XY distance the anchor travels over the whole print [mm].
    double total_drift() const { return m_total_drift; }
    // Fraction of layers that actually have eligible material at their anchor.
    double coverage() const { return m_coverage; }

    // Anchor for the layer printed at print_z. Falls back to the nearest
    // planned layer, so raft / support-only / custom-gcode layers still get one.
    bool anchor_for_print_z(double print_z, Vec2d &out) const;

private:
    struct PlannedLayer
    {
        double print_z;
        Vec2d  anchor;
    };

    std::vector<PlannedLayer> m_layers; // sorted by print_z
    Vec2d                     m_fixed_anchor{Vec2d::Zero()};
    bool                      m_valid{false};
    bool                      m_prime_tower{false};
    double                    m_max_drift{0.};
    double                    m_total_drift{0.};
    double                    m_coverage{0.};
};

// Splice `block` into one layer's G-code at the line boundary where the toolhead
// is closest to `anchor` while extruding a line the frame is allowed to be taken
// on (both in G-code XY; `start_xy` is the toolhead position before the first
// line). No existing line is modified and no move is added - the block simply
// lands between the two G-code lines that bracket the moment the nozzle passes
// the anchor. Returns `layer_gcode` unchanged when `block` is empty.
std::string anchor_timelapse_insert_block(const std::string &layer_gcode,
                                          const Vec2d       &start_xy,
                                          const Vec2d       &anchor,
                                          const std::string &block);

} // namespace Slic3r

#endif // slic3r_GCode_AnchorTimelapse_hpp_
