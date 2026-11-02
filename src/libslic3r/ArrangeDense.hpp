#ifndef slic3r_ArrangeDense_hpp_
#define slic3r_ArrangeDense_hpp_

#include "Arrange.hpp"

namespace Slic3r {

class ModelInstance;

namespace arrangement {

// Packs the items as tightly as possible. Works on each item's real outline
// (ArrangePolygon::outline, the convex hull when empty), so parts can nest into
// each other's recesses and holes. With params.allow_rotations every item may also
// turn by 90, 180 and 270 degrees and to its minimum-area angle. Several orderings
// are packed and the one that fits the most onto the first bed wins; when everything
// fits, the pile is kept close to square and centred on params.align_center.
//
// Same contract as arrange(): fixed items (with bed_idx) are obstacles, results go to
// translation / rotation / bed_idx (0 = first bed, 1.. = overflow beds, -1 = fits
// nowhere). Not meant for sequential printing.
void arrange_dense(ArrangePolygons &items, const ArrangePolygons &fixed, const Points &bed, const ArrangeParams &params);

// The instance's real footprint: every model part projected onto the bed, in the same
// frame as ArrangePolygon::poly (instance transformation without the XY offset).
ExPolygons instance_outline(const ModelInstance &instance);

}} // namespace Slic3r::arrangement

#endif
