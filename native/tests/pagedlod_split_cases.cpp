#include "native_tiles.hpp"
#include <osg/Geode>
#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osgDB/WriteFile>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace nt;
namespace {
void require(bool value,const std::string& message) {if(!value) throw std::runtime_error(message);}
osg::ref_ptr<osg::Geode> triangle(float x) {
    auto geometry=new osg::Geometry;auto positions=new osg::Vec3Array;
    positions->push_back({x,0,0});positions->push_back({x+1,0,0});positions->push_back({x,1,0});geometry->setVertexArray(positions);
    auto uv=new osg::Vec2Array;uv->push_back({0,0});uv->push_back({1,0});uv->push_back({0,1});geometry->setTexCoordArray(0,uv);
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));auto node=new osg::Geode;node->addDrawable(geometry);return node;
}
void write(osg::Node* node,const fs::path& path) {if(!osgDB::writeNodeFile(*node,path.u8string())) throw std::runtime_error("写入夹具失败");}
Json glb(const fs::path& path,Bytes& bin) {
    auto bytes=readBytes(path);std::uint32_t size=0;std::memcpy(&size,bytes.data()+12,4);
    bin=Bytes(bytes.begin()+28+size,bytes.end());return Json::parse(bytes.begin()+20,bytes.begin()+20+size);
}
std::vector<std::array<double,3>> world(const fs::path& path) {
    Bytes bin;auto doc=glb(path,bin);std::vector<std::array<double,3>> out;
    for(const auto& node:doc["nodes"]) {
        auto matrix=node.value("matrix",std::vector<double>{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1});
        for(const auto& primitive:doc["meshes"][node["mesh"].get<unsigned>()]["primitives"]) {
            const auto& accessor=doc["accessors"][primitive["attributes"]["POSITION"].get<unsigned>()];
            const auto& meta=doc["bufferViews"][accessor["bufferView"].get<unsigned>()];auto stride=meta.value("byteStride",12);
            for(unsigned i=0;i<accessor["count"];++i) {
                float point[3];std::memcpy(point,bin.data()+meta.value("byteOffset",0)+accessor.value("byteOffset",0)+i*stride,12);
                double x=point[0]*matrix[0]+point[1]*matrix[4]+point[2]*matrix[8]+matrix[12];
                double y=point[0]*matrix[1]+point[1]*matrix[5]+point[2]*matrix[9]+matrix[13];
                double z=point[0]*matrix[2]+point[1]*matrix[6]+point[2]*matrix[10]+matrix[14];
                out.push_back({x,-z,y});
            }
        }
    }
    std::sort(out.begin(),out.end());return out;
}
}
int main() {
    try {
        auto root=fs::temp_directory_path()/("pagedlod-split-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(root);
        auto a=root/"a.osgb",b=root/"b.osgb",leafA=root/"leaf-a.osgb",leafB=root/"leaf-b.osgb";
        write(triangle(10),leafA);write(triangle(20),leafB);
        auto pa=new osg::PagedLOD;pa->addChild(triangle(0),0,100);pa->setFileName(1,"leaf-a.osgb");pa->setRange(1,100,1e9);
        auto pb=new osg::PagedLOD;pb->addChild(triangle(100),0,200);pb->setFileName(1,"leaf-b.osgb");pb->setRange(1,200,1e9);
        auto group=new osg::Group;group->addChild(pa);group->addChild(pb);write(group,a);
        auto proxy=new osg::ProxyNode;proxy->setFileName(0,a.filename().string());write(proxy,b);
        Json reports;
        for(bool split:{false,true}) {
            Options options;options.input=b;options.output=root/(split?"split":"baseline");options.compressGeometry=false;options.splitPagedLod=split;options.srs="ENU:31.2,121.4,10";
            reports[split?"split":"baseline"]=convert(options);
        }
        const auto& plain=reports["baseline"],&split=reports["split"];
        require(plain["trianglesAllLods"]==4 && split["trianglesAllLods"]==4,"独立分支必须保留全部几何且不重复文件级粗层");
        require(split["glbs"]==4 && plain["glbs"]==3,"两个粗层分支与两个叶子各输出一次");
        auto plainTile=Json::parse(readBytes(root/"baseline/tileset.json")),splitTile=Json::parse(readBytes(root/"split/tileset.json"));
        require(plainTile["root"]["boundingVolume"]==splitTile["root"]["boundingVolume"],"拆分改变整体包围体");
        const Json* proxyRoot=nullptr;
        std::function<void(const Json&)> find=[&](const Json& node) {
            if(node.contains("children") && node["children"].size()==2 && !node.contains("content")) proxyRoot=&node;
            if(node.contains("children")) for(const auto& child:node["children"]) {
                require(child.contains("content"),"主入口不得包含会提前停止SSE遍历的内部空节点");
                find(child);
            }
        };find(splitTile["root"]);require(proxyRoot && (*proxyRoot)["refine"]=="REPLACE","空分组父节点下的分页节点独立替换");
        for(const auto& child:(*proxyRoot)["children"]) {
            require(child.contains("content") && child.contains("children"),"分支应同时拥有粗层回退和细层");
            require((*proxyRoot)["geometricError"].get<double>()>=child["geometricError"].get<double>(),"结构父节点不得提前停止细化");
        }
        std::vector<std::array<double,3>> actual,expected,rootActual;
        for(const auto& record:plain["records"]) if(record["source"]==a.u8string()) for(const auto& content:record["content"]) {auto values=world(root/"baseline"/content["uri"].get<std::string>());expected.insert(expected.end(),values.begin(),values.end());}
        for(const auto& record:split["records"]) if(record["source"]==a.u8string()) for(const auto& content:record["content"]) {
            auto uri=content["uri"].get<std::string>();auto values=world(root/"split"/uri);
            if(uri.find("-split-")==std::string::npos) rootActual.insert(rootActual.end(),values.begin(),values.end());
            else actual.insert(actual.end(),values.begin(),values.end());
        }
        require(rootActual.empty(),"不得重复文件级粗层回退");
        require(actual==expected,"分支粗层角点与原文件不一致");
        // Pause after the split parent has saved its prepared tree. Its external
        // children were already converted; resume must restore their audit rows.
        Options checkpointOptions;checkpointOptions.input=b;checkpointOptions.output=root/"resumed";
        checkpointOptions.srs="ENU:31.2,121.4,10";checkpointOptions.compressGeometry=false;checkpointOptions.splitPagedLod=true;
        checkpointOptions.checkpointDir=root/"checkpoint";checkpointOptions.pauseFile=root/"pause";
        checkpointOptions.event=[&](const Json& event) {
            if(event.value("type","")=="progress" && event.value("stage","")=="convert" && event.value("done",0U)==3)
                atomicWrite(checkpointOptions.pauseFile,Bytes{1});
        };
        require(convert(checkpointOptions)["status"]=="paused","必须在拆分父节点准备完成后暂停");
        fs::remove(checkpointOptions.pauseFile);checkpointOptions.resume=true;checkpointOptions.event={};
        auto resumed=convert(checkpointOptions);
        require(resumed["files"]==split["files"] && resumed["glbs"]==split["glbs"] && resumed["trianglesAllLods"]==split["trianglesAllLods"],"恢复必须保留依赖记录与全部几何");
        require(Json::parse(readBytes(root/"resumed/tileset.json"))==splitTile,"恢复改变了拆分后的入口树");
        for(const auto& row:split["records"]) for(const auto& content:row["content"]) {
            auto uri=content["uri"].get<std::string>();
            require(readBytes(root/"split"/uri)==readBytes(root/"resumed"/uri),"恢复改写了已完成GLB");
        }
        auto terminal=new osg::Group;
        for(float x:{0.0f,10.0f}) {auto paged=new osg::PagedLOD;paged->addChild(triangle(x),0,100);terminal->addChild(paged);}
        write(terminal,root/"terminal.osgb");
        Options terminalOptions;terminalOptions.input=root/"terminal.osgb";terminalOptions.output=root/"terminal-split";terminalOptions.srs="ENU:31.2,121.4,10";terminalOptions.compressGeometry=false;terminalOptions.splitPagedLod=true;
        auto terminalReport=convert(terminalOptions);
        require(terminalReport["glbs"]==1 && terminalReport["trianglesAllLods"]==2,"终端常驻 PagedLOD 不应被拆分或丢失");
        require(terminalReport["records"][0]["content"][0]["uri"].get<std::string>().find("-split-")==std::string::npos,"无外部分页的终端组必须保持原文件输出");
        fs::remove_all(root);
        std::cout<<"通过并列 PagedLOD 拆分：两个独立分支，粗层不重复，全部 12 个角点与包围体一致\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
