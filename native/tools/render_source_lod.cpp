#include "native_tiles.hpp"
#include <osgViewer/Viewer>
#include <osg/GL>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <chrono>
#include <iostream>
#include <thread>

using namespace nt;
int main(int argc,char** argv) {
    try {
        if(argc!=5) {std::cerr<<"用法: render_source_lod BLOCK.osgb compare-config.json SRS OUTPUT.png\n";return 2;}
        auto config=Json::parse(readBytes(argv[2]));
        auto frame=config["frame"],manifest=config["manifest"];
        SpatialReference spatial(argv[3],{0,0,0});osg::Matrixd inverse=spatial.placement();if(!inverse.invert(inverse)) throw std::runtime_error("空间基准不可逆");
        auto vector=[](const Json& a) {return osg::Vec3d(a[0].get<double>(),a[1].get<double>(),a[2].get<double>());};
        auto origin=vector(frame["originWorld"]),east=vector(frame["localAxesInWorld"]["east"]),north=vector(frame["localAxesInWorld"]["north"]),up=vector(frame["localAxesInWorld"]["up"]);
        auto target=vector(manifest["camera"]["target"]);double radius=manifest["camera"]["radius"].get<double>()*.55,pitch=.55;
        auto local=target+osg::Vec3d(std::cos(pitch)*radius,0,std::sin(pitch)*radius);
        auto world=origin+east*local.x()+north*local.y()+up*local.z();
        auto eye=world*inverse,center=(origin+target)*inverse;
        auto sourceUp=osg::Matrixd::transform3x3(up,inverse);sourceUp.normalize();
        osg::ref_ptr<osgDB::Options> options=new osgDB::Options;options->setObjectCacheHint(osgDB::Options::CACHE_NONE);
        auto scene=osgDB::readRefNodeFile(fs::u8path(argv[1]).u8string(),options.get());if(!scene) throw std::runtime_error("源 OSGB 读取失败");
        auto sceneSphere=scene->getBound();
        center=sceneSphere.center();
        eye=center+osg::Vec3d(std::cos(pitch)*sceneSphere.radius()*.55,0,std::sin(pitch)*sceneSphere.radius()*.55);
        sourceUp={0,0,1};
        std::cerr<<"source eye "<<eye.x()<<","<<eye.y()<<","<<eye.z()<<" center "<<center.x()<<","<<center.y()<<","<<center.z()<<" scene center "<<sceneSphere.center().x()<<","<<sceneSphere.center().y()<<","<<sceneSphere.center().z()<<" radius "<<sceneSphere.radius()<<'\n';
        osgViewer::Viewer viewer;
        viewer.setThreadingModel(osgViewer::ViewerBase::SingleThreaded);
        viewer.setUpViewInWindow(0,0,1920,851);
        viewer.setSceneData(scene);
        auto camera=viewer.getCamera();camera->setProjectionMatrixAsPerspective(55,1920.0/851.0,.01,manifest["camera"]["radius"].get<double>()*100);
        camera->setViewMatrixAsLookAt(eye,center,sourceUp);camera->setClearColor({.035,.07,.1,1});
        viewer.realize();
        auto pager=viewer.getDatabasePager();
        for(unsigned i=0;i<900;++i) {
            viewer.frame();
            if(i>30 && pager->getFileRequestListSize()==0 && pager->getDataToCompileListSize()==0 && pager->getDataToMergeListSize()==0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        for(unsigned i=0;i<10;++i) viewer.frame();
        Json loaded=Json::array();
        std::function<void(osg::Node*,std::string)> audit=[&](osg::Node* node,std::string path) {
            if(auto paged=dynamic_cast<osg::PagedLOD*>(node);paged && paged->getNumFileNames()) {
                Json names=Json::array();for(unsigned i=0;i<paged->getNumFileNames();++i) names.push_back(paged->getFileName(i));
                Json ranges=Json::array();for(const auto& range:paged->getRangeList()) ranges.push_back({range.first,range.second});
                loaded.push_back({{"path",path},{"fileNames",names},{"residentChildren",paged->getNumChildren()},
                    {"rangeMode",static_cast<int>(paged->getRangeMode())},{"radius",paged->getRadius()},
                    {"ranges",ranges}});
            }
            if(auto group=node->asGroup()) for(unsigned i=0;i<group->getNumChildren();++i) audit(group->getChild(i),path+"/"+std::to_string(i));
        };
        audit(scene.get(),"root");
        atomicJson(fs::u8path(argv[4]).parent_path()/"source-osg-loaded.json",loaded);
        osg::ref_ptr<osg::Image> image=new osg::Image;
        image->allocateImage(1920,851,1,GL_RGBA,GL_UNSIGNED_BYTE);
        camera->getGraphicsContext()->makeCurrent();
        glReadBuffer(GL_BACK);
        glReadPixels(0,0,1920,851,GL_RGBA,GL_UNSIGNED_BYTE,image->data());
        image->setOrigin(osg::Image::BOTTOM_LEFT);
        if(!image->valid()) throw std::runtime_error("OSG 未产生有效截图");
        if(!osgDB::writeImageFile(*image,fs::u8path(argv[4]).u8string())) throw std::runtime_error("截图写出失败");
        std::cout<<Json({{"status","captured"},{"pendingRequests",pager->getFileRequestListSize()},{"width",image->s()},{"height",image->t()},{"source",fs::u8path(argv[1]).u8string()},{"output",fs::u8path(argv[4]).u8string()}}).dump()<<'\n';
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
