#pragma once
#include "native_tiles.hpp"

namespace nt {
// Only resident source content is loaded. The original JSON subtree stays attached.
struct TopSource {
    Json tile;
    std::vector<GeometryInput> meshes;
};
struct TopResult {
    Json children=Json::array();
    Json audit=Json::array();
    double error=0;
};
TopResult rebuildTop(std::vector<TopSource> sources,const Options& options,
                     const SpatialReference* spatial=nullptr);
}
