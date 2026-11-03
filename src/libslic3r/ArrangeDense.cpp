#include "ArrangeDense.hpp"

#include "ClipperUtils.hpp"
#include "Geometry/ConvexHull.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "TriangleMeshSlicer.hpp"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <map>
#include <random>
#include <set>

namespace Slic3r { namespace arrangement {

namespace {

constexpr int kMaxBeds = 36; // MAX_NUM_PLATES in libnest2d/common.hpp

using Intervals = std::vector<std::pair<int, int>>; // filled cells [a, b] of one row

// An item's footprint (rotated, inflated by its spacing) on the packing grid.
struct Mask
{
    int                    w = 0, h = 0;
    std::vector<Intervals> rows;
    long long              cells = 0;
    Point                  bbmin;      // scaled bbox min of the inflated, rotated outline
    double                 angle = 0.;
};

// Marks every cell of the w x h grid (cell size c, grid origin o) that the shape touches,
// so two shapes whose cells don't overlap can't overlap either. Edges mark the cells they
// pass through, and cells whose centre is inside are filled by an even-odd scanline.
void rasterize(const ExPolygons &shape, const Point &o, coord_t c, int w, int h, std::vector<uint8_t> &out)
{
    out.assign(size_t(w) * size_t(h), 0);
    std::vector<std::vector<double>> xs(h);
    auto edge = [&](const Point &p0, const Point &p1) {
        const double x0 = double(p0.x() - o.x()) / c, y0 = double(p0.y() - o.y()) / c;
        const double x1 = double(p1.x() - o.x()) / c, y1 = double(p1.y() - o.y()) / c;
        const double ylo = std::min(y0, y1), yhi = std::max(y0, y1);
        const bool   flat = yhi - ylo < 1e-12;
        auto x_at = [&](double y) { return x0 + (x1 - x0) * (y - y0) / (y1 - y0); };
        for (int r = std::max(0, int(std::floor(ylo))), r1 = std::min(h - 1, int(std::floor(yhi))); r <= r1; ++r) {
            double xa = x0, xb = x1;
            if (!flat) {
                xa = x_at(std::max(ylo, double(r)));
                xb = x_at(std::min(yhi, double(r + 1)));
            }
            const int c0 = std::max(0, int(std::floor(std::min(xa, xb))));
            const int c1 = std::min(w - 1, int(std::floor(std::max(xa, xb))));
            for (int k = c0; k <= c1; ++k) out[size_t(r) * w + k] = 1;
        }
        if (!flat) // crossings of the row centres r + 0.5 in [ylo, yhi)
            for (int r = std::max(0, int(std::ceil(ylo - 0.5))), r1 = std::min(h - 1, int(std::ceil(yhi - 0.5)) - 1); r <= r1; ++r)
                xs[r].push_back(x_at(r + 0.5));
    };
    auto poly = [&](const Polygon &p) {
        for (size_t i = 0; i < p.points.size(); ++i) edge(p.points[i], p.points[(i + 1) % p.points.size()]);
    };
    for (const ExPolygon &e : shape) {
        poly(e.contour);
        for (const Polygon &hole : e.holes) poly(hole);
    }
    for (int r = 0; r < h; ++r) {
        std::sort(xs[r].begin(), xs[r].end());
        for (size_t i = 0; i + 1 < xs[r].size(); i += 2) {
            const int c0 = std::max(0, int(std::ceil(xs[r][i] - 0.5)));
            const int c1 = std::min(w - 1, int(std::floor(xs[r][i + 1] - 0.5)));
            for (int k = c0; k <= c1; ++k) out[size_t(r) * w + k] = 1;
        }
    }
}

Mask make_mask(const ExPolygons &outline, double angle, coord_t infl, coord_t c)
{
    ExPolygons s = outline;
    for (ExPolygon &e : s) e.rotate(angle);
    s = offset_ex(s, float(infl));
    const BoundingBox bb = get_extents(s);
    Mask m;
    m.bbmin = bb.min;
    m.angle = angle;
    m.w     = std::max(1, int(std::ceil(double(bb.size().x()) / c)));
    m.h     = std::max(1, int(std::ceil(double(bb.size().y()) / c)));
    std::vector<uint8_t> px;
    rasterize(s, bb.min, c, m.w, m.h, px);
    m.rows.resize(m.h);
    for (int y = 0; y < m.h; ++y)
        for (int x = 0; x < m.w; ++x)
            if (px[size_t(y) * m.w + x]) {
                int b = x;
                while (b + 1 < m.w && px[size_t(y) * m.w + b + 1]) ++b;
                m.rows[y].emplace_back(x, b);
                m.cells += b - x + 1;
                x = b;
            }
    return m;
}

// Occupancy grid. run[y][x] > 0: free cells starting at x; <= 0: occupied, and the next
// free cell is x - run[y][x]. That lets the search jump over obstacles instead of
// stepping one cell at a time.
struct Grid
{
    int                  w = 0, h = 0;
    std::vector<uint8_t> occ;
    std::vector<int>     run;

    void init(int w_, int h_)
    {
        w = w_; h = h_;
        occ.assign(size_t(w) * h, 0);
        run.resize(size_t(w) * h);
        for (int y = 0; y < h; ++y) rebuild_row(y);
    }
    void rebuild_row(int y)
    {
        int *R = &run[size_t(y) * w];
        const uint8_t *O = &occ[size_t(y) * w];
        for (int x = w - 1; x >= 0; --x)
            if (O[x]) R[x] = x + 1 < w && R[x + 1] <= 0 ? R[x + 1] - 1 : -1;
            else      R[x] = 1 + (x + 1 < w && R[x + 1] > 0 ? R[x + 1] : 0);
    }
    void block(const std::vector<uint8_t> &px)
    {
        for (size_t i = 0; i < occ.size(); ++i) occ[i] |= px[i];
        for (int y = 0; y < h; ++y) rebuild_row(y);
    }
    void put(const Mask &m, int x, int y)
    {
        for (int i = 0; i < m.h; ++i) {
            for (auto [a, b] : m.rows[i]) std::memset(&occ[size_t(y + i) * w + x + a], 1, size_t(b - a + 1));
            rebuild_row(y + i);
        }
    }
    // -1 if the mask fits at (x, y), otherwise the next x worth trying in this row.
    int test(const Mask &m, int x, int y) const
    {
        for (int i = 0; i < m.h; ++i) {
            const int *R = &run[size_t(y + i) * w];
            for (auto [a, b] : m.rows[i]) {
                const int cx = x + a, v = R[cx];
                if (v >= b - a + 1) continue;
                if (v <= 0) return x - v;
                const int e = cx + v;
                return e >= w ? INT_MAX : x + v - R[e];
            }
        }
        return -1;
    }
    // Lowest, then leftmost position whose right edge stays within xlimit.
    bool find(const Mask &m, int xlimit, int &ox, int &oy) const
    {
        for (int y = 0; y + m.h <= h; ++y)
            for (int x = 0; x + m.w <= xlimit;) {
                const int t = test(m, x, y);
                if (t < 0) { ox = x; oy = y; return true; }
                x = t;
            }
        return false;
    }
};

struct Bin
{
    Grid           g;        // fixed obstacles (also "avoid if possible" regions) + packed items
    Grid           relaxed;  // same without the avoid-if-possible regions; empty when there are none
    std::set<int>  temp_types, extruders, tpu;
    bool           used = false;
};

struct Placement { int bin = -1, x = 0, y = 0, mask = -1, rank = -1; };

struct Result
{
    std::vector<Placement> at;
    int       on_first = 0, bins = 0;
    long long area_first = 0, pile = 0;
    int       pile_w = 0, pile_h = 0;
    bool      relaxed_used = false;
};

bool better(const Result &a, const Result &b)
{
    if (a.on_first != b.on_first) return a.on_first > b.on_first;
    if (a.area_first != b.area_first) return a.area_first > b.area_first;
    if (a.bins != b.bins) return a.bins < b.bins;
    return a.pile < b.pile;
}

// Angle that turns the shape so its minimum-area bounding rectangle is axis aligned,
// plus that rectangle's size at that angle.
double min_area_angle(const ExPolygons &outline, double &rw, double &rh)
{
    Points pts;
    for (const ExPolygon &e : outline) append(pts, e.contour.points);
    const Polygon hull = Geometry::convex_hull(pts);
    double best = std::numeric_limits<double>::max(), angle = 0.;
    rw = rh = 0.;
    for (size_t i = 0; i < hull.points.size(); ++i) {
        const Vec2d d = (hull.points[(i + 1) % hull.points.size()] - hull.points[i]).cast<double>();
        if (d.squaredNorm() < 1.) continue;
        const double a = -std::atan2(d.y(), d.x()), ca = std::cos(a), sa = std::sin(a);
        double x0 = DBL_MAX, x1 = -DBL_MAX, y0 = DBL_MAX, y1 = -DBL_MAX;
        for (const Point &p : hull.points) {
            const double x = ca * p.x() - sa * p.y(), y = sa * p.x() + ca * p.y();
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
        if ((x1 - x0) * (y1 - y0) < best) { best = (x1 - x0) * (y1 - y0); angle = a; rw = x1 - x0; rh = y1 - y0; }
    }
    return angle;
}

bool quarter_turn(double a) { return std::abs(std::remainder(a, PI / 2)) < 1e-3; }

std::vector<double> candidate_angles(const ExPolygons &outline, const ArrangeParams &params)
{
    double rw, rh;
    const double r = min_area_angle(outline, rw, rh);
    if (params.allow_rotations) {
        std::vector<double> out{0., PI / 2, PI, 3 * PI / 2};
        if (!quarter_turn(r))
            for (int k = 0; k < 4; ++k) out.push_back(r + k * PI / 2);
        return out;
    }
    if (params.align_to_y_axis) {
        // long side along Y when the shape is clearly longer one way
        if (rw > rh * 1.1) return {r + PI / 2, r + 3 * PI / 2};
        if (rh > rw * 1.1) return {r, r + PI};
    }
    return {0.};
}

// The usable cells of the bed rectangle: cells not fully inside the bed polygon are blocked.
std::vector<uint8_t> outside_bed(const Polygon &bed, const Point &o, coord_t c, int w, int h)
{
    const Polygon box({o, Point(o.x() + coord_t(w) * c, o.y()), Point(o.x() + coord_t(w) * c, o.y() + coord_t(h) * c), Point(o.x(), o.y() + coord_t(h) * c)});
    std::vector<uint8_t> px;
    rasterize(diff_ex(Polygons{box}, Polygons{bed}), o, c, w, h, px);
    return px;
}

} // namespace

ExPolygons instance_outline(const ModelInstance &instance)
{
    Geometry::Transformation t = instance.get_transformation();
    t.set_offset(Vec3d(0., 0., instance.get_offset(Z)));
    Polygons all;
    for (const ModelVolume *v : instance.get_object()->volumes)
        if (v->is_model_part())
            append(all, project_mesh(v->mesh().its, t.get_matrix() * v->get_matrix(), [] {}));
    return union_ex(all);
}

bool arrange_dense(ArrangePolygons &items, const ArrangePolygons &fixed, const Points &bedpts, const ArrangeParams &params, double time_limit)
{
    if (items.empty() || bedpts.size() < 3) return true;
    const Polygon     bed(bedpts);
    const BoundingBox bedbb = bed.bounding_box();
    // ~0.5 mm cells; coarser on very large beds to bound the grid size
    const coord_t c  = std::max<coord_t>(scaled(0.5), std::max(bedbb.size().x(), bedbb.size().y()) / 800);
    const int     W  = int(bedbb.size().x() / c), H = int(bedbb.size().y() / c);
    const Point   o  = bedbb.min;
    const coord_t min_infl = scaled(1.); // MIN_SEPARATION in libnest2d/nester.hpp
    if (W < 1 || H < 1) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(time_limit);
    auto stop = [&] { return (params.stopcondition && params.stopcondition()) || std::chrono::steady_clock::now() > deadline; };

    // Base grids: bed edge and fixed items; "avoid if possible" regions only in hard_soft.
    auto base_grid = [&](int bin, bool with_soft) {
        Grid g;
        g.init(W, H);
        g.block(outside_bed(bed, o, c, W, H));
        // overflow beds borrow the first bed's excluded regions when they have none listed
        bool own = std::any_of(fixed.begin(), fixed.end(), [&](const ArrangePolygon &f) { return f.bed_idx == bin; });
        for (const ArrangePolygon &f : fixed) {
            if (own ? f.bed_idx != bin : (f.bed_idx != 0 || !f.is_virt_object || f.is_wipe_tower)) continue;
            if (f.is_extrusion_cali_object && !with_soft) continue;
            ExPolygons s = offset_ex(f.transformed_poly(), float(std::max(f.inflation, min_infl)));
            std::vector<uint8_t> px;
            rasterize(s, o, c, W, H, px);
            g.block(px);
        }
        return g;
    };
    const bool has_soft = std::any_of(fixed.begin(), fixed.end(), [](const ArrangePolygon &f) { return f.is_extrusion_cali_object && f.bed_idx == 0; });

    // Masks, shared between identical items.
    std::vector<Mask>             masks;
    std::vector<std::vector<int>> item_masks(items.size());
    std::map<std::string, int>    mask_index;
    for (size_t i = 0; i < items.size(); ++i) {
        if (stop()) return false;
        const ArrangePolygon &ap      = items[i];
        const ExPolygons      outline = ap.outline.empty() ? ExPolygons{ap.poly} : ap.outline;
        const coord_t         infl    = std::max(ap.inflation, min_infl);
        std::string           key(reinterpret_cast<const char *>(&infl), sizeof(infl));
        for (const ExPolygon &e : outline) {
            key.append(reinterpret_cast<const char *>(e.contour.points.data()), e.contour.points.size() * sizeof(Point));
            for (const Polygon &hole : e.holes) key.append(reinterpret_cast<const char *>(hole.points.data()), hole.points.size() * sizeof(Point));
            key.push_back('|');
        }
        std::vector<double> angles = candidate_angles(outline, params);
        auto add = [&](double a) {
            std::string k = key;
            k.append(reinterpret_cast<const char *>(&a), sizeof(a));
            auto it = mask_index.find(k);
            if (it == mask_index.end()) {
                it = mask_index.emplace(k, int(masks.size())).first;
                masks.push_back(make_mask(outline, a, infl, c));
            }
            item_masks[i].push_back(it->second);
        };
        for (double a : angles) add(a);
        // too big as it is: let it turn to whatever fits, like the old arrange did
        if (std::none_of(item_masks[i].begin(), item_masks[i].end(), [&](int m) { return masks[m].w <= W && masks[m].h <= H; })) {
            double rw, rh;
            const double r = min_area_angle(outline, rw, rh);
            for (int k = 0; k < 4; ++k) add(r + k * PI / 2);
        }
    }

    const Grid first_hard = base_grid(0, false);
    const Grid first      = has_soft ? base_grid(0, true) : first_hard;
    const Grid other      = base_grid(1, false);

    auto compatible = [&](const Bin &b, const ArrangePolygon &ap) {
        if (!b.used) return true;
        std::vector<int> types(b.temp_types.begin(), b.temp_types.end());
        types.push_back(ap.filament_temp_type);
        if (!Print::is_filaments_compatible(types)) return false;
        std::set<int> tpu = b.tpu;
        for (const auto &[id, type] : ap.extrude_id_filament_types)
            if (type == "TPU") tpu.insert(id);
        if (tpu.size() > 1) return false;
        if (!params.allow_multi_materials_on_same_plate)
            for (const auto &kv : ap.extrude_id_filament_types)
                if (!b.extruders.count(kv.first)) return false;
        return true;
    };

    // One packing pass: items in the given order, each at the best position over its angles.
    auto pack = [&](const std::vector<int> &order, bool top_key, int xlimit) {
        Result res;
        res.at.resize(items.size());
        std::vector<Bin> bins(1);
        bins[0].g = first;
        if (has_soft) bins[0].relaxed = first_hard;
        const long long K = 4LL * W + 4;
        int rank = 0;
        for (int idx : order) {
            if (stop()) break;
            const ArrangePolygon &ap = items[idx];
            for (size_t b = 0; b <= bins.size() && b < size_t(kMaxBeds); ++b) {
                const bool fresh = b == bins.size();
                if (fresh) {
                    bins.emplace_back();
                    bins.back().g = other;
                }
                Bin &bin = bins[b];
                if (!compatible(bin, ap)) continue;
                long long best = LLONG_MAX;
                Placement p;
                bool via_relaxed = false;
                for (int pass = 0; pass < 2 && best == LLONG_MAX; ++pass) {
                    const Grid &g = pass == 0 ? bin.g : bin.relaxed;
                    if (pass == 1 && g.w == 0) break;
                    for (int mi : item_masks[idx]) {
                        const Mask &m = masks[mi];
                        int x, y;
                        if (m.w > (b == 0 ? xlimit : W) || m.h > H || !g.find(m, b == 0 ? xlimit : W, x, y)) continue;
                        const long long key = top_key ? (y + m.h) * K + (x + m.w) : y * K + x;
                        if (key < best) { best = key; p = {int(b), x, y, mi, rank}; via_relaxed = pass == 1; }
                    }
                }
                if (best == LLONG_MAX) {
                    if (fresh) { bins.pop_back(); break; } // doesn't fit an empty bed: fits nowhere
                    continue;
                }
                const Mask &m = masks[p.mask];
                bin.g.put(m, p.x, p.y);
                if (bin.relaxed.w) bin.relaxed.put(m, p.x, p.y);
                res.relaxed_used |= via_relaxed;
                bin.used = true;
                bin.temp_types.insert(ap.filament_temp_type);
                for (const auto &[id, type] : ap.extrude_id_filament_types) {
                    bin.extruders.insert(id);
                    if (type == "TPU") bin.tpu.insert(id);
                }
                res.at[idx] = p;
                ++rank;
                break;
            }
        }
        int x0 = INT_MAX, y0 = INT_MAX, x1 = 0, y1 = 0;
        for (const Placement &p : res.at) {
            res.bins = std::max(res.bins, p.bin + 1);
            if (p.bin != 0) continue;
            const Mask &m = masks[p.mask];
            ++res.on_first;
            res.area_first += m.cells;
            x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
            x1 = std::max(x1, p.x + m.w); y1 = std::max(y1, p.y + m.h);
        }
        if (res.on_first) {
            res.pile_w = x1 - x0; res.pile_h = y1 - y0;
            res.pile   = (long long) res.pile_w * res.pile_h;
        }
        return res;
    };

    // Orderings: biggest first by a few measures, then shuffled variants of the first.
    const size_t n = items.size();
    auto sorted_by = [&](auto measure) {
        std::vector<int> order(n);
        for (size_t i = 0; i < n; ++i) order[i] = int(i);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
            if (items[a].priority != items[b].priority) return items[a].priority > items[b].priority;
            return measure(a) > measure(b);
        });
        return order;
    };
    auto m0 = [&](int i) -> const Mask & { return masks[item_masks[i].front()]; };
    struct Variant { std::vector<int> order; bool top_key; };
    std::vector<Variant> variants;
    variants.push_back({sorted_by([&](int i) { return m0(i).cells; }), true});
    variants.push_back({variants[0].order, false});
    variants.push_back({sorted_by([&](int i) { return std::max(m0(i).w, m0(i).h); }), true});
    variants.push_back({sorted_by([&](int i) { return m0(i).h; }), true});
    variants.push_back({sorted_by([&](int i) { return m0(i).w; }), true});
    std::mt19937 rng(20261101);
    for (int k = 0; k < 7 && n > 2; ++k) {
        std::vector<int> order = variants[0].order;
        for (size_t i = 0; i + 1 < n; ++i)
            if (rng() % 3 == 0 && items[order[i]].priority == items[order[i + 1]].priority) std::swap(order[i], order[i + 1]);
        variants.push_back({std::move(order), rng() % 2 == 0});
    }

    std::vector<Result> results(variants.size());
    tbb::parallel_for(size_t(0), variants.size(), [&](size_t i) { results[i] = pack(variants[i].order, variants[i].top_key, W); });
    if (stop()) return false;
    size_t best = 0;
    for (size_t i = 1; i < results.size(); ++i)
        if (better(results[i], results[best])) best = i;
    Result chosen = results[best];

    // Everything fits: look for a squarer pile by packing into narrower strips.
    if (chosen.on_first == int(n) && n > 1) {
        long long area = 0;
        for (const Placement &p : chosen.at) area += masks[p.mask].cells;
        std::vector<int> limits;
        for (double l = std::sqrt(double(area) * 1.1); l < W; l *= 1.12) limits.push_back(int(l));
        std::vector<std::pair<int, int>> jobs; // (limit, variant)
        for (int l : limits)
            for (int v = 0; v < 4; ++v) jobs.emplace_back(l, v);
        std::vector<Result> narrow(jobs.size());
        tbb::parallel_for(size_t(0), jobs.size(), [&](size_t i) {
            narrow[i] = pack(variants[jobs[i].second].order, variants[jobs[i].second].top_key, jobs[i].first);
        });
        // out of time here: the cut-short strips can't hold everything, so `chosen` stays
        if (params.stopcondition && params.stopcondition()) return false;
        auto squareness = [](const Result &r) { return std::make_pair(std::max(r.pile_w, r.pile_h), r.pile); };
        for (const Result &r : narrow)
            if (r.on_first == int(n) && squareness(r) < squareness(chosen)) chosen = r;
    }

    // Move each bed's pile towards params.align_center, as far as the fixed items allow.
    for (int b = 0; b < chosen.bins; ++b) {
        const Grid obstacles = b == 0 ? (chosen.relaxed_used ? first_hard : first) : other;
        int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
        for (const Placement &p : chosen.at)
            if (p.bin == b) {
                x0 = std::min(x0, p.x); y0 = std::min(y0, p.y);
                x1 = std::max(x1, p.x + masks[p.mask].w); y1 = std::max(y1, p.y + masks[p.mask].h);
            }
        if (x0 == INT_MAX) continue;
        const int dx = std::clamp(int(std::lround(params.align_center.x() * W - 0.5 * (x0 + x1))), -x0, W - x1);
        const int dy = std::clamp(int(std::lround(params.align_center.y() * H - 0.5 * (y0 + y1))), -y0, H - y1);
        int sx = 0, sy = 0;
        double best_d = std::hypot(dx, dy);
        for (int i = 8; i >= 0; --i)
            for (int j = 8; j >= 0; --j) {
                const int tx = dx * i / 8, ty = dy * j / 8;
                const double d = std::hypot(dx - tx, dy - ty);
                if (d >= best_d) continue;
                bool ok = true;
                for (const Placement &p : chosen.at)
                    if (p.bin == b && obstacles.test(masks[p.mask], p.x + tx, p.y + ty) >= 0) { ok = false; break; }
                if (ok) { best_d = d; sx = tx; sy = ty; }
            }
        for (Placement &p : chosen.at)
            if (p.bin == b) { p.x += sx; p.y += sy; }
    }

    for (size_t i = 0; i < n; ++i) {
        const Placement &p  = chosen.at[i];
        ArrangePolygon  &ap = items[i];
        ap.bed_idx = p.bin;
        if (p.bin < 0) continue;
        const Mask &m  = masks[p.mask];
        ap.rotation    = m.angle;
        ap.translation = Vec2crd(o.x() + coord_t(p.x) * c - m.bbmin.x(), o.y() + coord_t(p.y) * c - m.bbmin.y());
        ap.itemid      = p.rank;
        if (params.progressind) params.progressind(unsigned(p.rank + 1), ap.name);
    }
    return true;
}

}} // namespace Slic3r::arrangement
