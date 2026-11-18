#pragma once
#include "native_tiles.hpp"
#include <array>

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

// 轻量预览复用 Top 的 QEM，但保留原纹理 UV，不进入远景层的 1024 烘焙流程。
// 每次只处理一个源文件，调用方可在返回后释放该文件的完整 OSG 场景。
struct PreviewMaterial {
    osg::ref_ptr<osg::StateSet> state;
    osg::ref_ptr<osg::Image> image;
    osg::ref_ptr<osg::Texture> texture;
};
struct PreviewFace {
    std::array<osg::Vec3d,3> positions;
    std::array<osg::Vec2d,3> uv;
    std::array<osg::Vec4d,3> colors;
    unsigned material=0;
};
struct PreviewMesh {
    std::vector<PreviewFace> faces;
    std::vector<PreviewMaterial> materials;
    std::uint64_t inputTriangles=0;
    bool hasColors=false;
    Json simplification=Json::object();
};
PreviewMesh simplifyPreview(const std::vector<GeometryInput>& inputs,std::size_t target,
                            const Options& options,const SpatialReference* spatial=nullptr);
}
