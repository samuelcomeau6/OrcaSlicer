///|/ Anchor timelapse support. See AnchorTimelapse.hpp.
///|/
#include "AnchorTimelapse.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

#include <boost/log/trivial.hpp>

#include "../ExtrusionEntityCollection.hpp"
#include "../GCodeReader.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "GCodeProcessor.hpp"

namespace Slic3r {

// ----------------------------------------------------------------------------
// Tunables. Deliberately compile-time: they describe the shape of the search,
// not a user preference.
//
// The costs are all in millimetres of anchor movement, which is the only thing
// the plan trades away: ABSENT_COST is "I would move the anchor this far rather
// than fire one frame where there is no material".
// ----------------------------------------------------------------------------
namespace {

// XY bin size of the coverage grid [mm]. Two millimetres is fine enough that
// the anchor lands on a specific wall, coarse enough that a wall wandering by
// a fraction of a line width still scores as "the same place".
constexpr double GRID_MM = 2.;
// Distance between samples taken along an extrusion [mm]. Half a cell, so no
// cell a path crosses can be missed.
constexpr double SAMPLE_MM = GRID_MM * 0.5;
// How many grid cells survive into the dynamic program. The shortlist is filled
// in two tiers: first spots spread at least MIN_SEPARATION_MM apart, so a plan
// that has to cross the plate has somewhere to cross to, then the best of what
// is left regardless of separation, which is what gives the anchor a neighbour
// one cell away to step onto.
constexpr size_t MAX_CANDIDATES = 192;
constexpr double MIN_SEPARATION_MM = 4. * GRID_MM;
// Cost of moving the anchor, per mm of XY drift between two layers.
constexpr double DRIFT_COST_PER_MM = 1.;
// Flat cost of moving the anchor at all - stops it dithering between two
// equally good neighbouring cells.
constexpr double SWITCH_COST = 0.05;
// The anchor may not step further than this between two layers while it still
// has material under it. It only gets to cross the plate on a layer where its
// own spot has run out, which is what turns a teleport into a creep.
constexpr double MAX_STEP_MM = 5.;
// Cost of keeping the anchor on a layer that has no material there.
constexpr double ABSENT_COST = 5.;
// Layers closer together than this are treated as the same layer [mm].
constexpr double Z_TOLERANCE = 1e-4;

inline int64_t cell_key(int32_t x, int32_t y) { return (int64_t(x) << 32) | (int64_t(uint32_t(y))); }
inline int32_t cell_of(double v) { return int32_t(std::floor(v / GRID_MM)); }

struct CellAgg
{
    uint32_t layers{0};      // number of distinct layers with eligible material here
    int32_t  last_layer{-1}; // dedup helper for `layers`
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

} // namespace

// ----------------------------------------------------------------------------
// Planning
// ----------------------------------------------------------------------------

bool AnchorTimelapsePlanner::plan(const Print &print)
{
    m_layers.clear();
    m_valid       = false;
    m_prime_tower = false;
    m_max_drift   = 0.;
    m_total_drift = 0.;
    m_coverage    = 0.;

    // A real multi-filament prime tower answers the question outright: it is
    // printed at the same XY on every layer and it is there to be sacrificed.
    //
    // print.has_wipe_tower() is also true for a single-filament print that only
    // asks for a tower to service a "smooth" timelapse - there is no tool-change
    // tower in that case and the G-code generator never builds a
    // WipeTowerIntegration for it, so the frame could never be fired there.
    // Require more than one extruder so those prints fall through to the grid
    // search below instead of anchoring on a tower that will not be printed.
    if (print.has_wipe_tower() && print.extruders().size() > 1) {
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
        BOOST_LOG_TRIVIAL(info) << "Anchor timelapse: anchored on the prime tower at " << m_fixed_anchor.x() << ", "
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
    // `fn(layer_index, path, instance_shift)` receives every extrusion of the
    // print. Cheap: it visits paths, it does not touch their points.
    auto for_each_path = [&print, &layer_index](auto &&fn) {
        for (const PrintObject *object : print.objects()) {
            for (const PrintInstance &instance : object->instances()) {
                const Vec2d shift = unscale(instance.shift.x(), instance.shift.y());
                for (const Layer *layer : object->layers()) {
                    const int li = layer_index(layer->print_z);
                    for (const LayerRegion *region : layer->regions()) {
                        auto emit = [&fn, li, &shift](const ExtrusionPath &p) { fn(li, p, shift); };
                        visit_paths(&region->perimeters, emit);
                        visit_paths(&region->fills, emit);
                    }
                }
                for (const SupportLayer *layer : object->support_layers()) {
                    const int li   = layer_index(layer->print_z);
                    auto      emit = [&fn, li, &shift](const ExtrusionPath &p) { fn(li, p, shift); };
                    visit_paths(&layer->support_fills, emit);
                }
            }
        }
    };

    // The best line each layer has to offer. An outer wall only becomes an
    // anchor on a layer that prints nothing better, so this gate has to be
    // settled for the whole layer before any of its extrusions can be scored.
    std::vector<AnchorTimelapsePriority> layer_best(n_layers, AnchorTimelapsePriority::Forbidden);
    for_each_path([&layer_best](int li, const ExtrusionPath &path, const Vec2d &) {
        if (path.polyline.size() < 2)
            return;
        const AnchorTimelapsePriority prio = anchor_timelapse_role_priority(path.role());
        if (prio > layer_best[li])
            layer_best[li] = prio;
    });

    // `sink(layer_index, x_mm, y_mm)` receives every sample of every eligible
    // extrusion. Two passes share this walk: the first builds the global
    // coverage map, the second records per-layer detail for the surviving
    // candidates only.
    auto walk = [&for_each_path, &layer_best](auto &&sink) {
        for_each_path([&sink, &layer_best](int li, const ExtrusionPath &path, const Vec2d &shift) {
            if (path.polyline.size() < 2 || !anchor_timelapse_allowed(layer_best[li], anchor_timelapse_role_priority(path.role())))
                return;
            const Points &pts = path.polyline.points;
            for (size_t i = 1; i < pts.size(); ++i) {
                const Vec2d  a   = unscale(pts[i - 1].x(), pts[i - 1].y()) + shift;
                const Vec2d  b   = unscale(pts[i].x(), pts[i].y()) + shift;
                const double len = (b - a).norm();
                const int    n   = std::max(1, int(std::ceil(len / SAMPLE_MM)));
                for (int s = 0; s <= n; ++s) {
                    const Vec2d p = a + (b - a) * (double(s) / double(n));
                    sink(li, p.x(), p.y());
                }
            }
        });
    };

    // ---- Pass A: global coverage -------------------------------------------
    std::unordered_map<int64_t, CellAgg> cells;
    cells.reserve(4096);
    walk([&cells](int li, double x, double y) {
        CellAgg &c = cells[cell_key(cell_of(x), cell_of(y))];
        if (c.last_layer != li) {
            c.last_layer = li;
            ++c.layers;
        }
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
        Vec2d    centre; // centre of mass of everything printed in the cell
    };
    std::vector<Cand> ranked;
    ranked.reserve(cells.size());
    for (const auto &kv : cells)
        ranked.push_back(Cand{kv.first, kv.second.layers, Vec2d(kv.second.sx / kv.second.n, kv.second.sy / kv.second.n)});
    std::sort(ranked.begin(), ranked.end(), [](const Cand &a, const Cand &b) {
        if (a.layers != b.layers)
            return a.layers > b.layers;
        return a.key < b.key; // deterministic
    });

    // First tier: keep the shortlist spread out, so a print whose anchor has to
    // cross the plate has somewhere to cross to instead of a clump of
    // neighbouring cells in one corner. Second tier: fill the rest with the best
    // of what is left, which is where the anchor's own neighbours come from.
    std::vector<Cand> cand;
    cand.reserve(MAX_CANDIDATES);
    for (int tier = 0; tier < 2 && cand.size() < MAX_CANDIDATES; ++tier) {
        for (const Cand &c : ranked) {
            if (cand.size() >= MAX_CANDIDATES)
                break;
            bool taken = false;
            for (const Cand &k : cand) {
                if (k.key == c.key || (tier == 0 && (k.centre - c.centre).norm() < MIN_SEPARATION_MM)) {
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
    // For each (layer, candidate) record whether it has eligible material, and
    // the sampled point nearest the cell's centre of mass. That point is on a
    // real extrusion of that layer, and picking the same relative spot on every
    // layer keeps the anchor as still as the geometry allows.
    std::unordered_map<int64_t, uint32_t> cand_index;
    for (uint32_t k = 0; k < K; ++k)
        cand_index.emplace(cand[k].key, k);

    std::vector<uint8_t> present(n_layers * K, 0);
    std::vector<Vec2d>   point(n_layers * K, Vec2d::Zero());
    std::vector<double>  point_d2(n_layers * K, std::numeric_limits<double>::max());
    walk([&](int li, double x, double y) {
        auto it = cand_index.find(cell_key(cell_of(x), cell_of(y)));
        if (it == cand_index.end())
            return;
        const size_t i = size_t(li) * K + it->second;
        present[i]     = 1;
        const Vec2d  p  = Vec2d(x, y);
        const double d2 = (p - cand[it->second].centre).squaredNorm();
        if (d2 < point_d2[i]) {
            point_d2[i] = d2;
            point[i]    = p;
        }
    });

    // Order the shortlist by the longest unbroken stretch of layers it can hold
    // the anchor for. Nothing below depends on the order except the tie-break in
    // the dynamic program - and with no speed term left, ties are the normal
    // case: every spot that has material scores exactly the same. Resolving them
    // towards the tallest column is what keeps the anchor still.
    {
        std::vector<uint32_t> run(K, 0);
        for (size_t k = 0; k < K; ++k) {
            uint32_t cur = 0;
            for (size_t li = 0; li < n_layers; ++li) {
                cur      = present[li * K + k] ? cur + 1 : 0;
                run[k] = std::max(run[k], cur);
            }
        }
        std::vector<size_t> order(K);
        std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&run, &cand](size_t a, size_t b) {
            if (run[a] != run[b])
                return run[a] > run[b];
            if (cand[a].layers != cand[b].layers)
                return cand[a].layers > cand[b].layers;
            return cand[a].key < cand[b].key;
        });

        std::vector<Cand>    cand2(K);
        std::vector<uint8_t> present2(n_layers * K, 0);
        std::vector<Vec2d>   point2(n_layers * K, Vec2d::Zero());
        for (size_t k = 0; k < K; ++k) {
            const size_t src = order[k];
            cand2[k]         = cand[src];
            for (size_t li = 0; li < n_layers; ++li) {
                present2[li * K + k] = present[li * K + src];
                point2[li * K + k]   = point[li * K + src];
            }
        }
        cand    = std::move(cand2);
        present = std::move(present2);
        point   = std::move(point2);
    }

    // ---- Dynamic program over the layers -----------------------------------
    // The only things that cost anything are moving the anchor and firing a
    // frame where there is no material, so the plan that comes out is the
    // stillest one that stays on the print.
    std::vector<double> dist(K * K);
    for (size_t j = 0; j < K; ++j)
        for (size_t k = 0; k < K; ++k)
            dist[j * K + k] = (cand[j].centre - cand[k].centre).norm();

    std::vector<double>   dp(K, 0.), dp_prev(K, 0.);
    std::vector<uint16_t> back(n_layers * K, 0);
    for (size_t li = 0; li < n_layers; ++li) {
        for (size_t k = 0; k < K; ++k) {
            const size_t i    = li * K + k;
            const double here = present[i] ? 0. : ABSENT_COST;
            if (li == 0) {
                dp[k]   = here;
                back[i] = uint16_t(k);
                continue;
            }
            // Staying put is always available and always free, so `best` is
            // never left unset.
            double best = dp_prev[k];
            size_t arg  = k;
            for (size_t j = 0; j < K; ++j) {
                if (j == k)
                    continue;
                const double d = dist[j * K + k];
                // A step longer than the cap is only on the table when staying
                // is not: the anchor's own spot has run out of material.
                if (d > MAX_STEP_MM && present[li * K + j])
                    continue;
                const double c = dp_prev[j] + SWITCH_COST + DRIFT_COST_PER_MM * d;
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
        const double step = (pos - last).norm();
        m_max_drift       = std::max(m_max_drift, step);
        m_total_drift += step;
        last         = pos;
        m_layers[li] = PlannedLayer{zs[li], pos};
    }
    m_coverage = double(with_mat) / double(n_layers);
    m_valid    = true;

    BOOST_LOG_TRIVIAL(info) << "Anchor timelapse: planned " << n_layers << " layers from " << K << " candidate spots, coverage "
                            << int(m_coverage * 100.) << "%, max step " << m_max_drift << " mm, total travel " << m_total_drift
                            << " mm";
    return true;
}

bool AnchorTimelapsePlanner::anchor_for_print_z(double print_z, Vec2d &out) const
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
// Splicing the block into a finished layer's G-code
// ----------------------------------------------------------------------------

std::string anchor_timelapse_insert_block(const std::string &layer_gcode,
                                          const Vec2d       &start_xy,
                                          const Vec2d       &anchor,
                                          const std::string &block)
{
    if (block.empty())
        return layer_gcode;

    // Walk the layer's moves, tracking the toolhead XY and the feature it is
    // laying down, and remember the line boundary at which it comes closest to
    // the anchor while extruding a line the frame is allowed on. The block goes
    // there: no line is touched and nothing extra is travelled - the nozzle is
    // already as near the anchor as this layer's toolpath ever gets.
    //
    // Boundaries are kept per priority, because whether an outer wall may be
    // used is only known once the whole layer has been read.
    const std::string &role_tag = GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Role);

    GCodeReader reader;
    reader.x() = float(start_xy.x());
    reader.y() = float(start_xy.y());

    constexpr size_t N_PRIO  = size_t(AnchorTimelapsePriority::Interior) + 1;
    double           best_d2[N_PRIO];
    size_t           best_off[N_PRIO];
    for (size_t i = 0; i < N_PRIO; ++i) {
        best_d2[i]  = std::numeric_limits<double>::max();
        best_off[i] = 0;
    }

    AnchorTimelapsePriority prio      = AnchorTimelapsePriority::Forbidden;
    bool                    extruding = false;
    auto cb = [&extruding](GCodeReader &r, const GCodeReader::GCodeLine &l) { extruding = l.extruding(r); };

    const char *const base = layer_gcode.c_str();
    const char       *p    = base;
    const char *const end  = base + layer_gcode.size();
    GCodeReader::GCodeLine gline;
    while (p < end) {
        gline.reset();
        extruding = false;
        // parse_line runs the callback before it applies the move, so `extruding`
        // is decided against the position the line starts from, while reader
        // holds the position it ends at once the call returns.
        const char *next = reader.parse_line(p, end, gline, cb);
        if (next <= p) // defensive: never seen, but do not spin
            break;
        const std::string_view comment = gline.comment();
        if (comment.size() > role_tag.size() && comment.substr(0, role_tag.size()) == role_tag)
            prio = anchor_timelapse_role_priority(ExtrusionEntity::string_to_role(comment.substr(role_tag.size())));
        else if (extruding && prio != AnchorTimelapsePriority::Forbidden) {
            const double d2 = (Vec2d(double(reader.x()), double(reader.y())) - anchor).squaredNorm();
            const size_t t  = size_t(prio);
            if (d2 < best_d2[t]) {
                best_d2[t]  = d2;
                best_off[t] = size_t(next - base);
            }
        }
        p = next;
    }

    // Best tier the layer actually offered. When it offered none - a layer that
    // is all bridge, or has no extrusion at all - the block goes at the end of
    // the layer, which is where traditional timelapse puts it.
    size_t off = layer_gcode.size();
    for (size_t t = N_PRIO; t-- > size_t(AnchorTimelapsePriority::ExternalWall);)
        if (best_d2[t] != std::numeric_limits<double>::max()) {
            off = best_off[t];
            break;
        }

    std::string out;
    out.reserve(layer_gcode.size() + block.size() + 1);
    out.append(layer_gcode, 0, off);
    if (off != 0 && out.back() != '\n')
        out.push_back('\n');
    out.append(block);
    out.append(layer_gcode, off, std::string::npos);
    return out;
}

} // namespace Slic3r
