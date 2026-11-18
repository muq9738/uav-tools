#pragma once
#include "native_tiles.hpp"

namespace nt {
// 显示前沿中的一个常驻内容。只保存轻量描述；源网格在处理该空间块时才读取。
struct PreviewSource {
    fs::path file;
    osg::Matrixd matrix=osg::Matrixd::identity();
    osg::BoundingBoxd bounds;
    std::uint64_t inputTriangles=0;
};
// options.output 是独立的 preview 暂存目录。返回 preview-report.json 的完整内容。
Json buildPreview(const std::vector<PreviewSource>& sources,const Options& options,
                  const SpatialReference* spatial,const osg::Matrixd& placement);
}
