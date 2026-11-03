#include <catch2/catch.hpp>

#include <cstdlib>
#include <iostream>

#include "libslic3r/ArrangeDense.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/SVG.hpp"

using namespace Slic3r;
using namespace Slic3r::arrangement;

static ExPolygon shape(std::initializer_list<Vec2d> pts)
{
    Polygon p;
    for (const Vec2d &v : pts) p.points.emplace_back(scaled(v.x()), scaled(v.y()));
    if (!p.is_counter_clockwise()) p.reverse();
    return ExPolygon(p);
}

static ArrangePolygon item(const ExPolygon &outline, double gap_mm = 1.)
{
    ArrangePolygon ap;
    ap.outline = {outline};
    ap.poly.contour = Geometry::convex_hull(outline.contour.points);
    ap.inflation = scaled(gap_mm);
    ap.bed_idx = 0; // as the GUI hands them over; the old arrange skips -1
    ap.extrude_id_filament_types = {{1, "PLA"}};
    return ap;
}

static Points bed(double w, double h) { return {Point(0, 0), Point(scaled(w), 0), Point(scaled(w), scaled(h)), Point(0, scaled(h))}; }

static ExPolygons placed(const ArrangePolygon &ap)
{
    ExPolygons out = ap.outline.empty() ? ExPolygons{ap.poly} : ap.outline;
    for (ExPolygon &e : out) { e.rotate(ap.rotation); e.translate(ap.translation.x(), ap.translation.y()); }
    return out;
}

static int on_first(const ArrangePolygons &items)
{
    return int(std::count_if(items.begin(), items.end(), [](const ArrangePolygon &ap) { return ap.bed_idx == 0; }));
}

// Every pair on bed 0 keeps the gap, everything stays on the bed.
static void check_layout(const ArrangePolygons &items, const Points &bedpts, const std::string &svg_name)
{
    std::vector<ExPolygons> grown;
    for (const ArrangePolygon &ap : items)
        grown.push_back(ap.bed_idx == 0 ? offset_ex(placed(ap), float(ap.inflation) - float(scaled(0.01))) : ExPolygons{});
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].bed_idx != 0) continue;
        REQUIRE(area(diff_ex(placed(items[i]), Polygons{Polygon(bedpts)})) < 1.);
        for (size_t j = i + 1; j < items.size(); ++j)
            if (items[j].bed_idx == 0) REQUIRE(area(intersection_ex(grown[i], grown[j])) < 1.);
    }
    if (const char *dir = std::getenv("ARRANGE_SVG")) {
        BoundingBox bb(bedpts);
        SVG svg(std::string(dir) + "/" + svg_name + ".svg", bb);
        svg.draw_outline(Polygon(bedpts), "black");
        for (const ArrangePolygon &ap : items)
            if (ap.bed_idx == 0) svg.draw(placed(ap), "steelblue", 0.7f);
    }
}

TEST_CASE("Dense arrange: no overlaps, inside the bed, rotations used", "[ArrangeDense]")
{
    ArrangePolygons items;
    for (int i = 0; i < 40; ++i) {
        const double a = 20 + (i * 7) % 30, b = 10 + (i * 11) % 25;
        items.push_back(item(shape({{0, 0}, {a, 0}, {a, b * 0.4}, {a * 0.4, b * 0.4}, {a * 0.4, b}, {0, b}}))); // L shapes
    }
    ArrangeParams params;
    params.allow_rotations = true;
    arrange_dense(items, {}, bed(256, 256), params);
    REQUIRE(on_first(items) > 0);
    check_layout(items, bed(256, 256), "dense_L");
    REQUIRE(std::any_of(items.begin(), items.end(), [](const ArrangePolygon &ap) { return std::abs(ap.rotation) > 1e-6; }));
}

TEST_CASE("Dense arrange: small parts go into a frame's opening", "[ArrangeDense]")
{
    ExPolygon frame = shape({{0, 0}, {100, 0}, {100, 100}, {0, 100}});
    Polygon   hole  = shape({{10, 10}, {90, 10}, {90, 90}, {10, 90}}).contour;
    hole.reverse();
    frame.holes.push_back(hole);
    ArrangePolygons items{item(frame)};
    for (int i = 0; i < 9; ++i) items.push_back(item(shape({{0, 0}, {20, 0}, {20, 20}, {0, 20}})));
    ArrangeParams params;
    arrange_dense(items, {}, bed(105, 105), params); // no room beside the frame
    check_layout(items, bed(105, 105), "dense_frame");
    REQUIRE(on_first(items) == int(items.size()));
}

TEST_CASE("Dense arrange: fixed items are kept clear", "[ArrangeDense]")
{
    ArrangePolygon tower = item(shape({{0, 0}, {40, 0}, {40, 40}, {0, 40}}));
    tower.translation = Vec2crd(scaled(108.), scaled(108.));
    tower.bed_idx = 0;
    tower.is_virt_object = true;
    ArrangePolygons items;
    for (int i = 0; i < 30; ++i) items.push_back(item(shape({{0, 0}, {30, 0}, {30, 30}, {0, 30}})));
    ArrangeParams params;
    arrange_dense(items, {tower}, bed(256, 256), params);
    items.push_back(tower);
    check_layout(items, bed(256, 256), "dense_fixed");
}

TEST_CASE("Dense arrange fits more than the old arrange", "[ArrangeDense]")
{
    struct Case { std::string name; ExPolygon s; int n; };
    std::vector<Case> cases{
        {"L", shape({{0, 0}, {60, 0}, {60, 15}, {15, 15}, {15, 60}, {0, 60}}), 40},
        {"triangle", shape({{0, 0}, {70, 0}, {0, 45}}), 60},
        {"T", shape({{0, 0}, {50, 0}, {50, 12}, {31, 12}, {31, 45}, {19, 45}, {19, 12}, {0, 12}}), 60},
        {"plate", shape({{0, 0}, {120.3, 0}, {120.3, 66.3}, {0, 66.3}}), 12},
    };
    for (const Case &c : cases)
        for (bool rot : {false, true}) {
            ArrangeParams params;
            params.allow_rotations = rot;
            ArrangePolygons old_items(c.n, item(c.s)), new_items(c.n, item(c.s));
            arrange(old_items, {}, bed(256, 256), params);
            arrange_dense(new_items, {}, bed(256, 256), params);
            check_layout(new_items, bed(256, 256), "dense_" + c.name + (rot ? "_rot" : ""));
            std::cout << c.name << (rot ? " rotations" : " fixed    ") << ": old " << on_first(old_items) << ", dense " << on_first(new_items)
                      << " of " << c.n << std::endl;
            REQUIRE(on_first(new_items) >= on_first(old_items));
        }
}

TEST_CASE("Dense arrange: out of time leaves the items alone", "[ArrangeDense]")
{
    ArrangePolygons items;
    for (int i = 0; i < 40; ++i) items.push_back(item(shape({{0, 0}, {20, 0}, {20, 10}, {0, 10}})));
    const ArrangePolygons before = items;
    ArrangeParams params;
    REQUIRE_FALSE(arrange_dense(items, {}, bed(256, 256), params, 0.));
    for (size_t i = 0; i < items.size(); ++i) {
        REQUIRE(items[i].translation == before[i].translation);
        REQUIRE(items[i].bed_idx == before[i].bed_idx);
    }
    REQUIRE(arrange_dense(items, {}, bed(256, 256), params));
}
