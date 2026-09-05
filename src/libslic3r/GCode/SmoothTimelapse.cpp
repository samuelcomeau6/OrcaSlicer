///|/ Smooth (anchored) timelapse support. See SmoothTimelapse.hpp.
///|/
#include "SmoothTimelapse.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include <boost/log/trivial.hpp>

#include "../ExtrusionEntityCollection.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"

namespace Slic3r {

// ----------------------------------------------------------------------------
// Tunables. Deliberately compile-time: they describe the shape of the search,
// not a user preference.
// ----------------------------------------------------------------------------
namespace {

// XY bin size of the coverage grid [mm]. Two millimetres is fine enough that
// the anchor lands on a specific wall, coarse enough that a wall wandering by
// a fraction of a line width still scores as "the same place".
constexpr double GRID_MM = 2.;
// Distance between samples taken along an extrusion [mm]. Half a cell, so no
// cell a path crosses can be missed.
constexpr double SAMPLE_MM = GRID_MM * 0.5;
// How many grid cells survive into the dynamic program.
constexpr size_t MAX_CANDIDATES = 64;
// Cost of moving the anchor, per mm of XY drift between two layers. The speed
// score is normalised to [0, 1], so 0.02 means "moving the anchor 50 mm has to
// buy at least a full slowest-to-fastest improvement in print speed".
constexpr double DRIFT_COST_PER_MM = 0.02;
// Flat cost of moving the anchor at all - stops it dithering between two
// equally good neighbouring cells.
constexpr double SWITCH_COST = 0.05;
// Cost of keeping an anchor on a layer that has no material there. Higher than
// any speed score, so the plan only does this when it has no choice.
constexpr double ABSENT_COST = 5.;
// Layers closer together than this are treated as the same layer [mm].
constexpr double Z_TOLERANCE = 1e-4;

inline int64_t cell_key(int32_t x, int32_t y) { return (int64_t(x) << 32) | (int64_t(uint32_t(y))); }
inline int32_t cell_of(double v) { return int32_t(std::floor(v / GRID_MM)); }

struct CellAgg
{
    uint32_t layers{0};      // number of distinct layers with material here
    int32_t  last_layer{-1}; // dedup helper for `layers`
    float    min_speed{std::numeric_limits<float>::max()};
    double   sx{0.};
    double   sy{0.};
    uint32_t n{0};
};

// Walk an extrusion tree down to the ExtrusionPaths it is built from.
template<typename Fn> void visit_paths(const ExtrusionEntity *ee, Fn &&fn)
{
    if (ee == nullptr)
        return;
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(ee)) {
        for (const ExtrusionEntity *e : coll->entities)
            visit_paths(e, fn);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(ee)) {
        for (const ExtrusionPath &p : loop->paths)
            fn(p);
    } else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath *>(ee)) {
        for (const ExtrusionPath &p : mp->paths)
            fn(p);
    } else if (const auto *p = dynamic_cast<const ExtrusionPath *>(ee)) {
        fn(*p);
    }
}

// The nominal print speed the G-code generator will use for `role`. This
// mirrors the role dispatch in GCode::_extrude; it does not need to be exact,
// only to rank one place in the print against another.
//
// Roles the frame may not be taken on return 0 and are dropped by the caller,
// so an outer wall can never become an anchor even if it is the slowest thing
// on the layer.
class SpeedModel
{
public:
    SpeedModel(const Print &print, const PrintRegionConfig &region, const PrintObjectConfig &object)
    {
        const size_t i = get_config_idx(print.config(), ConfigFlowDomain::Process, 0);
        auto at = [i](const ConfigOptionFloats &o) { return o.values.empty() ? 0. : o.get_at(i); };

        m_inner_wall   = at(region.inner_wall_speed);
        m_sparse       = at(region.sparse_infill_speed);
        m_solid        = at(region.internal_solid_infill_speed);
        m_top          = at(region.top_surface_speed);
        m_ironing      = at(region.ironing_speed);
        m_gap          = at(region.gap_infill_speed);
        m_support      = at(object.support_speed);
        m_support_intf = at(object.support_interface_speed);
        m_bottom       = at(print.config().initial_layer_infill_speed);
    }

    // Returns 0 for anything the frame may not be taken on.
    double speed(ExtrusionRole role) const
    {
        if (!smooth_timelapse_role_eligible(role))
            return 0.;
        switch (role) {
        case erPerimeter: return m_inner_wall;
        case erInternalInfill: return m_sparse;
        case erSolidInfill: return m_solid;
        case erTopSolidInfill: return m_top;
        case erIroning: return m_ironing;
        case erBottomSurface: return m_bottom;
        case erGapFill: return m_gap;
        case erSupportMaterial: return m_support;
        case erSupportMaterialInterface: return m_support_intf;
        default: return 0.;
        }
    }

private:
    double m_inner_wall{0}, m_sparse{0}, m_solid{0}, m_top{0};
    double m_ironing{0}, m_gap{0}, m_support{0}, m_support_intf{0}, m_bottom{0};
};

} // namespace

// ----------------------------------------------------------------------------
// Planning
// ----------------------------------------------------------------------------

bool SmoothTimelapsePlanner::plan(const Print &print)
{
    m_layers.clear();
    m_valid       = false;
    m_prime_tower = false;
    m_max_drift   = 0.;
    m_coverage    = 0.;

    // A prime tower answers the question outright: it is printed at the same XY
    // on every layer and it is there to be sacrificed.
    if (print.has_wipe_tower()) {
        const PrintConfig &cfg   = print.config();
        const int          plate = print.get_plate_index();
        const double       x     = cfg.wipe_tower_x.get_at(plate);
        const double       y     = cfg.wipe_tower_y.get_at(plate);
        const double       w     = cfg.prime_tower_width.value;
        const double       d     = print.wipe_tower_data(print.extruders().size()).depth;
        const double       a     = Geometry::deg2rad(double(cfg.wipe_tower_rotation_angle.value));
        // Centre of the tower footprint, rotated about the tower origin. The
        // tower is emitted in plate coordinates, so the plate origin applies
        // here the way it does in WipeTowerIntegration.
        const Vec3d  plate_origin = print.get_plate_origin();
        const double lx           = 0.5 * w;
        const double ly           = 0.5 * (d > 0. ? d : w);
        m_fixed_anchor = Vec2d(x + lx * std::cos(a) - ly * std::sin(a), y + lx * std::sin(a) + ly * std::cos(a)) +
                         Vec2d(plate_origin.x(), plate_origin.y());
        m_prime_tower  = true;
        m_valid        = true;
        m_coverage     = 1.;
        BOOST_LOG_TRIVIAL(info) << "Smooth timelapse: anchored on the prime tower at " << m_fixed_anchor.x() << ", "
                                << m_fixed_anchor.y();
        return true;
    }

    // ---- Layer table -------------------------------------------------------
    std::vector<double> zs;
    for (const PrintObject *object : print.objects()) {
        for (const Layer *l : object->layers())
            zs.emplace_back(l->print_z);
        for (const SupportLayer *l : object->support_layers())
            zs.emplace_back(l->print_z);
    }
    if (zs.empty())
        return false;
    std::sort(zs.begin(), zs.end());
    zs.erase(std::unique(zs.begin(), zs.end(), [](double a, double b) { return std::abs(a - b) < Z_TOLERANCE; }), zs.end());

    auto layer_index = [&zs](double z) -> int {
        auto it = std::lower_bound(zs.begin(), zs.end(), z - Z_TOLERANCE);
        if (it == zs.end())
            return int(zs.size()) - 1;
        return int(it - zs.begin());
    };
    const size_t n_layers = zs.size();

    // ---- Geometry walk -----------------------------------------------------
    // `sink(layer_index, x_mm, y_mm, speed_mm_s)` receives every sample of every
    // eligible extrusion. Two passes share this walk: the first builds the
    // global coverage map, the second records per-layer detail for the
    // surviving candidates only.
    auto walk = [&print, &layer_index](auto &&sink) {
        for (const PrintObject *object : print.objects()) {
            const PrintObjectConfig &object_cfg = object->config();
            for (const PrintInstance &instance : object->instances()) {
                const Vec2d shift = unscale(instance.shift.x(), instance.shift.y());

                auto sample_path = [&sink, &shift](const ExtrusionPath &path, int li, double speed) {
                    if (speed <= 0. || path.polyline.size() < 2)
                        return;
                    const Points &pts = path.polyline.points;
                    for (size_t i = 1; i < pts.size(); ++i) {
                        const Vec2d  a   = unscale(pts[i - 1].x(), pts[i - 1].y()) + shift;
                        const Vec2d  b   = unscale(pts[i].x(), pts[i].y()) + shift;
                        const double len = (b - a).norm();
                        const int    n   = std::max(1, int(std::ceil(len / SAMPLE_MM)));
                        for (int s = 0; s <= n; ++s) {
                            const Vec2d p = a + (b - a) * (double(s) / double(n));
                            sink(li, p.x(), p.y(), speed);
                        }
                    }
                };

                for (const Layer *layer : object->layers()) {
                    const int li = layer_index(layer->print_z);
                    for (const LayerRegion *region : layer->regions()) {
                        const SpeedModel model(print, region->region().config(), object_cfg);
                        auto             emit = [&](const ExtrusionPath &p) { sample_path(p, li, model.speed(p.role())); };
                        visit_paths(&region->perimeters, emit);
                        visit_paths(&region->fills, emit);
                    }
                }
                for (const SupportLayer *layer : object->support_layers()) {
                    const int li = layer_index(layer->print_z);
                    // Support has no region config; the object config carries its speeds.
                    const SpeedModel model(print, print.default_region_config(), object_cfg);
                    auto             emit = [&](const ExtrusionPath &p) { sample_path(p, li, model.speed(p.role())); };
                    visit_paths(&layer->support_fills, emit);
                }
            }
        }
    };

    // ---- Pass A: global coverage -------------------------------------------
    std::unordered_map<int64_t, CellAgg> cells;
    cells.reserve(4096);
    walk([&cells](int li, double x, double y, double speed) {
        CellAgg &c = cells[cell_key(cell_of(x), cell_of(y))];
        if (c.last_layer != li) {
            c.last_layer = li;
            ++c.layers;
        }
        c.min_speed = std::min(c.min_speed, float(speed));
        c.sx += x;
        c.sy += y;
        ++c.n;
    });
    if (cells.empty())
        return false;

    // ---- Pass B: shortlist -------------------------------------------------
    struct Cand
    {
        int64_t  key;
        uint32_t layers;
        float    min_speed;
        Vec2d    centre; // centre of mass of everything printed in the cell
    };
    std::vector<Cand> ranked;
    ranked.reserve(cells.size());
    for (const auto &kv : cells)
        ranked.push_back(Cand{kv.first, kv.second.layers, kv.second.min_speed,
                              Vec2d(kv.second.sx / kv.second.n, kv.second.sy / kv.second.n)});
    std::sort(ranked.begin(), ranked.end(), [](const Cand &a, const Cand &b) {
        if (a.layers != b.layers)
            return a.layers > b.layers;
        return a.min_speed < b.min_speed;
    });

    // Keep the shortlist spread out, so a print whose anchor has to drift still
    // has somewhere to drift to instead of 64 neighbouring cells in one corner.
    std::vector<Cand> cand;
    cand.reserve(MAX_CANDIDATES);
    const double min_sep = 4. * GRID_MM;
    for (int relax = 0; relax < 2 && cand.size() < MAX_CANDIDATES; ++relax) {
        for (const Cand &c : ranked) {
            if (cand.size() >= MAX_CANDIDATES)
                break;
            bool taken = false;
            for (const Cand &k : cand) {
                if (k.key == c.key || (relax == 0 && (k.centre - c.centre).norm() < min_sep)) {
                    taken = true;
                    break;
                }
            }
            if (!taken)
                cand.push_back(c);
        }
    }
    const size_t K = cand.size();
    if (K == 0)
        return false;

    // ---- Pass C: per-layer detail for the shortlist ------------------------
    // For each (layer, candidate) keep the slowest speed seen, and the sampled
    // point nearest the cell's centre of mass. That point is on a real
    // extrusion of that layer, and picking the same relative spot on every
    // layer keeps the anchor as still as the geometry allows.
    std::unordered_map<int64_t, uint32_t> cand_index;
    for (uint32_t k = 0; k < K; ++k)
        cand_index.emplace(cand[k].key, k);

    std::vector<uint8_t> present(n_layers * K, 0);
    std::vector<float>   speed(n_layers * K, 0.f);
    std::vector<Vec2d>   point(n_layers * K, Vec2d::Zero());
    std::vector<double>  point_d2(n_layers * K, std::numeric_limits<double>::max());
    walk([&](int li, double x, double y, double sp) {
        auto it = cand_index.find(cell_key(cell_of(x), cell_of(y)));
        if (it == cand_index.end())
            return;
        const uint32_t k = it->second;
        const size_t   i = size_t(li) * K + k;
        if (!present[i] || sp < speed[i])
            speed[i] = float(sp);
        present[i]      = 1;
        const Vec2d  p  = Vec2d(x, y);
        const double d2 = (p - cand[k].centre).squaredNorm();
        if (d2 < point_d2[i]) {
            point_d2[i] = d2;
            point[i]    = p;
        }
    });

    // Normalise the speed score across everything that survived.
    float vmin = std::numeric_limits<float>::max(), vmax = 0.f;
    for (size_t i = 0; i < speed.size(); ++i)
        if (present[i]) {
            vmin = std::min(vmin, speed[i]);
            vmax = std::max(vmax, speed[i]);
        }
    const float vspan = std::max(1e-3f, vmax - vmin);

    // ---- Dynamic program over the layers -----------------------------------
    std::vector<double>   dp(K, 0.), dp_prev(K, 0.);
    std::vector<uint16_t> back(n_layers * K, 0);
    for (size_t li = 0; li < n_layers; ++li) {
        for (size_t k = 0; k < K; ++k) {
            const size_t i    = li * K + k;
            const double here = present[i] ? double((speed[i] - vmin) / vspan) : ABSENT_COST;
            if (li == 0) {
                dp[k]   = here;
                back[i] = uint16_t(k);
                continue;
            }
            double best = std::numeric_limits<double>::max();
            size_t arg  = k;
            for (size_t j = 0; j < K; ++j) {
                double c = dp_prev[j];
                if (j != k)
                    c += SWITCH_COST + DRIFT_COST_PER_MM * (cand[k].centre - cand[j].centre).norm();
                if (c < best) {
                    best = c;
                    arg  = j;
                }
            }
            dp[k]   = here + best;
            back[i] = uint16_t(arg);
        }
        dp_prev = dp;
    }

    size_t k = 0;
    for (size_t j = 1; j < K; ++j)
        if (dp_prev[j] < dp_prev[k])
            k = j;

    std::vector<size_t> chosen(n_layers, 0);
    for (size_t li = n_layers; li-- > 0;) {
        chosen[li] = k;
        k          = back[li * K + k];
    }

    // ---- Materialise the anchors ------------------------------------------
    m_layers.resize(n_layers);
    Vec2d  last     = cand[chosen.front()].centre;
    size_t with_mat = 0;
    for (size_t li = 0; li < n_layers; ++li) {
        const size_t i = li * K + chosen[li];
        Vec2d        pos;
        if (present[i]) {
            pos = point[i];
            ++with_mat;
        } else {
            // Nothing eligible is printed at the anchor on this layer - hold the
            // last one rather than jumping to the cell centre.
            pos = last;
        }
        m_max_drift  = std::max(m_max_drift, (pos - last).norm());
        last         = pos;
        m_layers[li] = PlannedLayer{zs[li], pos};
    }
    m_coverage = double(with_mat) / double(n_layers);
    m_valid    = true;

    BOOST_LOG_TRIVIAL(info) << "Smooth timelapse: planned " << n_layers << " layers from " << K << " candidate spots, coverage "
                            << int(m_coverage * 100.) << "%, max drift " << m_max_drift << " mm";
    return true;
}

bool SmoothTimelapsePlanner::anchor_for_print_z(double print_z, Vec2d &out) const
{
    if (!m_valid)
        return false;
    if (m_prime_tower) {
        out = m_fixed_anchor;
        return true;
    }
    if (m_layers.empty())
        return false;
    auto it = std::lower_bound(m_layers.begin(), m_layers.end(), print_z - Z_TOLERANCE,
                               [](const PlannedLayer &l, double z) { return l.print_z < z; });
    if (it == m_layers.end())
        it = std::prev(m_layers.end());
    out = it->anchor;
    return true;
}

// ----------------------------------------------------------------------------
// Breaking an extrusion open at the anchor
// ----------------------------------------------------------------------------

bool smooth_timelapse_split_polyline(const Polyline &src,
                                     const Vec2d    &origin,
                                     const Vec2d    &anchor,
                                     double          capture_mm,
                                     Polyline       &first,
                                     Polyline       &second)
{
    const Points &pts = src.points;
    if (pts.size() < 2)
        return false;

    // Work in scaled coordinates, the same ones the emitted moves are built from.
    const Vec2d  target      = (anchor - origin) / SCALING_FACTOR;
    const double capture2    = (capture_mm / SCALING_FACTOR) * (capture_mm / SCALING_FACTOR);
    // Do not leave a stub shorter than this on either side of the cut.
    const double min_stub    = 0.2 / SCALING_FACTOR;

    size_t best_i  = 0;
    double best_t  = 0.;
    double best_d2 = std::numeric_limits<double>::max();
    for (size_t i = 1; i < pts.size(); ++i) {
        const Vec2d  a  = pts[i - 1].cast<double>();
        const Vec2d  b  = pts[i].cast<double>();
        const Vec2d  d  = b - a;
        const double l2 = d.squaredNorm();
        double       t  = 0.;
        if (l2 > 0.)
            t = std::max(0., std::min(1., (target - a).dot(d) / l2));
        const double d2 = (a + d * t - target).squaredNorm();
        if (d2 < best_d2) {
            best_d2 = d2;
            best_i  = i;
            best_t  = t;
        }
    }
    if (best_d2 > capture2)
        return false;

    const Vec2d a   = pts[best_i - 1].cast<double>();
    const Vec2d b   = pts[best_i].cast<double>();
    const Vec2d cut = a + (b - a) * best_t;

    // Length up to and after the cut, so neither half comes out as a stub.
    double before = 0.;
    for (size_t i = 1; i < best_i; ++i)
        before += (pts[i] - pts[i - 1]).cast<double>().norm();
    before += (cut - a).norm();
    double after = (b - cut).norm();
    for (size_t i = best_i + 1; i < pts.size(); ++i)
        after += (pts[i] - pts[i - 1]).cast<double>().norm();
    if (before < min_stub || after < min_stub)
        return false;

    const Point cut_pt(coord_t(std::lround(cut.x())), coord_t(std::lround(cut.y())));

    first.points.assign(pts.begin(), pts.begin() + best_i);
    if (first.points.empty() || first.points.back() != cut_pt)
        first.points.emplace_back(cut_pt);

    second.points.clear();
    second.points.emplace_back(cut_pt);
    for (size_t i = best_i; i < pts.size(); ++i)
        if (pts[i] != cut_pt)
            second.points.emplace_back(pts[i]);

    return first.points.size() >= 2 && second.points.size() >= 2;
}

} // namespace Slic3r
