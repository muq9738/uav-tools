#pragma once
#include "native_tiles.hpp"

namespace nt {
using OrthoMeshReader = std::function<std::vector<GeometryInput>(const Json& record, const std::string& uri)>;
Json buildOrthophoto(const Json& root, const Json& records, const osg::BoundingBoxd& bounds,
                     const SpatialReference& spatial, const Options& options, const OrthoMeshReader& read);
}
