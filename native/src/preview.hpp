#pragma once
#include "native_tiles.hpp"
#include <functional>

namespace nt {
// options.output 是独立的 preview 暂存目录。返回 preview-report.json 的完整内容。
Json buildPreview(const Json& mainRoot,const Json& records,const Options& options,
                  const SpatialReference* spatial,const osg::Matrixd& placement,
                  const std::function<void(const fs::path&)>& checkSource={});
}
