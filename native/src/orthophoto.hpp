#pragma once
#include "native_tiles.hpp"

namespace nt {
using OrthoRowProducer = std::function<void(const std::function<void(unsigned char*)>&)>;
void writeOrthophotoTiff(const fs::path& file,int width,int height,double west,double north,
                         double resolution,int epsg,const OrthoRowProducer& produce);
using OrthoMeshReader = std::function<std::vector<GeometryInput>(const Json& record, const std::string& uri)>;
Json buildOrthophoto(const Json& root, const Json& records, const osg::BoundingBoxd& bounds,
                     const SpatialReference& spatial, const Options& options, const OrthoMeshReader& read);
}
