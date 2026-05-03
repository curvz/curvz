#pragma once
#include "GlyphObject.hpp"
#include <string>
#include <vector>

namespace Curvz {

struct Layer {
    std::string              name    = "Layer 1";
    bool                     visible = true;
    bool                     locked  = false;
    double                   opacity = 1.0;
    std::vector<GlyphObject> objects;
};

} // namespace Curvz
