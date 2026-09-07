///|/ Smooth (anchored) timelapse support.
///|/
///|/ Picks, for every printed layer, the XY position at which the timelapse
///|/ G-code block should be fired, and provides the geometry helper that breaks
///|/ an extrusion open at that position while the layer is being written.
///|/
#ifndef slic3r_GCode_SmoothTimelapse_hpp_
#define slic3r_GCode_SmoothTimelapse_hpp_

#include <vector>

#include "../libslic3r.h"
#include "../ExtrusionEntity.hpp"
#include "../Point.hpp"
#include "../Polyline.hpp"

namespace Slic3r {

class Print;

// How close an extrusion has to pass to the anchor to be broken open there [mm].
// The anchor is always an actual point on an extrusion of that layer, so this is
// only slack for seam clipping, arc fitting and path simplification.
static constexpr double SMOOTH_TIMELAPSE_CAPTURE_MM = 0.6;

// Can the frame be taken in the middle of this kind of extrusion?
//
// Outer perimeters are excluded: they are what the print is judged on, and the
// pause artefact belongs anywhere else. Anything printed over air is excluded
// too - bridge speeds are slow, which would otherwise make bridges attractive
// anchors, and stopping on one is how you get a droop.
inline bool smooth_timelapse_role_eligible(ExtrusionRole role)
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
//   * A prime tower, when the print has one, is the anchor straight away. It is
//     printed at a fixed XY on every layer and it is sacrificial, so no search
//     is needed or wanted.
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
// Every anchor is a point that actually lies on an extrusion of its own layer,
// so the layer can be broken open exactly there.
//
// Nothing here runs unless smooth (anchored) timelapse is actually selected;
// the planning happens once, at G-code export time, off the slicing path.
class SmoothTimelapsePlanner
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

// Break `src` in two at the point closest to `anchor`, where `anchor` is in
// G-code XY and the polyline is in object coordinates offset by `origin`.
//
// Returns false - leaving `first` and `second` untouched - when no segment
// passes within `capture_mm` of the anchor, or when the cut would leave a
// stub too short to be worth emitting.
bool smooth_timelapse_split_polyline(const Polyline &src,
                                     const Vec2d    &origin,
                                     const Vec2d    &anchor,
                                     double          capture_mm,
                                     Polyline       &first,
                                     Polyline       &second);

} // namespace Slic3r

#endif // slic3r_GCode_SmoothTimelapse_hpp_
