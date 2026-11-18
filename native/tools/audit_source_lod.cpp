#include "native_tiles.hpp"
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osgDB/ReadFile>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace nt;
namespace {
std::string fileKey(const fs::path& file) {
    auto name=file.generic_u8string();return contentKey(Bytes(name.begin(),name.end()));
}
void inspect(osg::Node* node,const fs::path& file,const osg::Matrixd& parent,const SpatialReference& spatial,Json& rows) {
    osg::Matrixd matrix=parent;
    if(auto transform=dynamic_cast<osg::Transform*>(node)) {
        if(transform->getReferenceFrame()!=osg::Transform::RELATIVE_RF) throw std::runtime_error("绝对参考系");
        if(!transform->computeLocalToWorldMatrix(matrix,nullptr)) throw std::runtime_error("无法计算矩阵");
    }
    if(auto paged=dynamic_cast<osg::PagedLOD*>(node)) {
        const auto& ranges=paged->getRangeList();
        bool compact=paged->getNumChildren()<paged->getNumFileNames();
        for(unsigned i=0;i<paged->getNumFileNames();++i) if(!paged->getFileName(i).empty()) {
            unsigned rangeIndex=(paged->getNumChildren()==1 && paged->getNumFileNames()==2)?i+1:i;
            double coarseMax=ranges.empty()?0:ranges.front().second;
            double threshold=coarseMax;
            auto center=spatial.toLocal(paged->getCenter()*matrix);
            rows.push_back({{"source",file.u8string()},{"key",fileKey(file)},
                {"external",paged->getFileName(i)},
                {"debug",{{"children",paged->getNumChildren()},{"files",paged->getNumFileNames()},{"ranges",ranges.size()},{"compact",compact}}},{"rangeMode",static_cast<int>(paged->getRangeMode())},
                {"center",{center.x(),center.y(),center.z()}},{"radius",paged->getRadius()},
                {"pixelThreshold",threshold},
                {"coarseMax",coarseMax},
                {"matrix",std::vector<double>(matrix.ptr(),matrix.ptr()+16)}});
        }
    }
    if(auto proxy=dynamic_cast<osg::ProxyNode*>(node)) {
        for(unsigned i=0;i<proxy->getNumFileNames();++i) if(!proxy->getFileName(i).empty())
            rows.push_back({{"source",file.u8string()},{"key",fileKey(file)},{"external",proxy->getFileName(i)},{"proxy",true}});
    }
    if(auto group=node->asGroup()) for(unsigned i=0;i<group->getNumChildren();++i)
        inspect(group->getChild(i),file,matrix,spatial,rows);
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=4) {std::cerr<<"用法: osgb-source-lod-audit INPUT EPSG SRSOrigin\n";return 2;}
        fs::path input=fs::canonical(fs::u8path(argv[1]));
        auto origin=std::string(argv[3]);double x=0,y=0,z=0;
        if(std::sscanf(origin.c_str(),"%lf,%lf,%lf",&x,&y,&z)!=3) throw std::runtime_error("origin 格式错误");
        SpatialReference spatial(argv[2],{x,y,z});
        fs::path directory=fs::is_regular_file(input)?input.parent_path():(fs::exists(input/"Data")?input/"Data":input);
        Json rows=Json::array();std::set<fs::path> visited;
        for(const auto& entry:fs::recursive_directory_iterator(directory)) {
            if(!entry.is_regular_file() || entry.path().extension()!=".osgb") continue;
            auto file=fs::canonical(entry.path());if(!visited.insert(file).second) continue;
            osg::ref_ptr<osgDB::Options> options=new osgDB::Options;options->setObjectCacheHint(osgDB::Options::CACHE_NONE);
            auto node=osgDB::readRefNodeFile(file.u8string(),options.get());
            if(!node) throw std::runtime_error("无法读取 "+file.u8string());
            inspect(node.get(),file,osg::Matrixd::identity(),spatial,rows);
        }
        std::cout<<Json({{"input",input.u8string()},{"srs",argv[2]},{"origin",{x,y,z}},{"sourceFiles",visited.size()},{"pagedLods",rows}}).dump()<<'\n';
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
