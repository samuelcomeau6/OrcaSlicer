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

#include <string>
#include <vector>

#include "../libslic3r.h"
#include "../ExtrusionEntity.hpp"
#include "../Point.hpp"

namespace Slic3r {

class Print;

// Which extrusion roles the anchor is allowed to sit on.
//
// Outer perimeters are excluded: they are what the print is judged on, and the
// pause artefact belongs anywhere else. Anything printed over air is excluded
// too - bridge speeds are slow, which would otherwise make bridges attractive
// anchors, and stopping on one is how you get a droop.
inline bool anchor_timelapse_role_eligible(ExtrusionRole role)
{
    switch (role) {
    case erPerimeter:
    case erInternalInfill:
    case erSolidInfill:
    case erTopSolidInfill:
    case erBottomSurface:
    case erIroning:
    case erGapFill:
    case erSupportMaterial:
    case erSupportMaterialInterface: return true;
    default: return false;
    }
}

// Plans one XY "anchor" per printed layer: the spot at which the timelapse
// G-code block is fired so the camera sees the toolhead in (very nearly) the
// same place on every frame.
//
// Selection rules, in order:
//   * A prime tower, when the print has a real multi-filament one, is the anchor
//     straight away. It is printed at a fixed XY on every layer and it is
//     sacrificial, so no search is needed or wanted.
//   * Otherwise the eligible extrusions of the whole print are binned into a
//     coarse XY grid and searched for a column of material that exists on as
//     many layers as possible and is printed slowly where it exists. Travel
//     moves are never sampled - only extrusions contribute, both to the
//     coverage and to the speed score.
//   * Where no single column spans the whole print (tapering or organic parts),
//     the anchor is allowed to drift: a dynamic program over the layers trades
//     the slowness of the chosen spot against the XY distance the anchor moves
//     from one layer to the next, so the anchor creeps along the part instead
//     of jumping around it.
//
// Every grid-search anchor is a point that actually lies on an extrusion of its
// own layer, so the nozzle is guaranteed to pass through it while that layer is
// printed.
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
    double                    m_coverage{0.};
};

// Splice `block` into one layer's G-code at the line boundary where the toolhead
// is closest to `anchor` (both in G-code XY; `start_xy` is the toolhead position
// before the first line). No existing line is modified and no move is added -
// the block simply lands between the two G-code lines that bracket the moment
// the nozzle passes the anchor. Returns `layer_gcode` unchanged when `block` is
// empty.
std::string anchor_timelapse_insert_block(const std::string &layer_gcode,
                                          const Vec2d       &start_xy,
                                          const Vec2d       &anchor,
                                          const std::string &block);

} // namespace Slic3r

#endif // slic3r_GCode_AnchorTimelapse_hpp_
