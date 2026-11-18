#include "native_tiles.hpp"
#include <osg/Geode>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osgDB/WriteFile>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

using namespace nt;
namespace {
void require(bool condition,const std::string& message) {if(!condition) throw std::runtime_error(message);}
osg::ref_ptr<osg::Geode> triangle(float offset=0) {
    auto geometry=new osg::Geometry;auto positions=new osg::Vec3Array;
    positions->push_back({offset,0,0});positions->push_back({offset+1,0,0});positions->push_back({offset,1,0});
    geometry->setVertexArray(positions);geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    auto geode=new osg::Geode;geode->addDrawable(geometry);return geode;
}
void writeNode(osg::Node* node,const fs::path& path) {
    fs::create_directories(path.parent_path());osg::ref_ptr<osgDB::Options> options=new osgDB::Options("WriteImageHint=IncludeFile");
    require(osgDB::writeNodeFile(*node,path.u8string(),options.get()),"fixture 写入失败: "+path.u8string());
}
Options options(const fs::path& input,const fs::path& output) {
    Options result;result.input=input;result.output=output;result.compressGeometry=false;result.threads=1;
    result.srs="ENU:0,0";result.origin="0,0,0";return result;
}
osg::ref_ptr<osg::PagedLOD> external(const std::string& name) {
    auto paged=new osg::PagedLOD;paged->setFileName(0,name);paged->setRange(0,0,1e9);return paged;
}
Json gltf(const fs::path& file,const Bytes* bytes=nullptr) {
    Bytes local;if(!bytes) {local=readBytes(file);bytes=&local;}
    require(bytes->size()>=20,"GLB 头不完整");std::uint32_t length=0;std::memcpy(&length,bytes->data()+12,4);
    require(bytes->size()>=20+length,"GLB JSON 长度");return Json::parse(bytes->begin()+20,bytes->begin()+20+length);
}
Bytes viewBytes(const Bytes& bytes,const Json& view) {
    std::uint32_t jsonLength=0;std::memcpy(&jsonLength,bytes.data()+12,4);std::size_t bin=28+jsonLength;
    require(view["buffer"]==0 && !view.contains("extensions"),"本回归使用独立 raw GLB 解码");
    auto start=bin+view.value("byteOffset",0U);auto length=view["byteLength"].get<std::size_t>();
    require(start+length<=bytes.size(),"GLB bufferView 越界");return Bytes(bytes.begin()+start,bytes.begin()+start+length);
}
std::string pointKey(const osg::Vec3d& point) {
    auto rounded=[](double value) {return static_cast<long long>(std::llround(value*1000000.0));};
    return std::to_string(rounded(point.x()))+","+std::to_string(rounded(point.y()))+","+std::to_string(rounded(point.z()));
}
std::multiset<std::string> drawnCorners(const fs::path& file) {
    auto bytes=readBytes(file);auto doc=gltf(file,&bytes);std::multiset<std::string> result;
    osg::Matrixd yToZ(1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,0,0,1);
    for(const auto& node:doc["nodes"]) {
        auto values=node["matrix"].get<std::vector<double>>();osg::Matrixd matrix(values.data());
        for(const auto& primitive:doc["meshes"][node["mesh"].get<unsigned>()]["primitives"]) {
            require(primitive["mode"]==4,"本回归只接收三角形");const auto& position=doc["accessors"][primitive["attributes"]["POSITION"].get<unsigned>()];
            require(position["componentType"]==5126 && position["type"]=="VEC3","POSITION 类型");
            const auto& positionView=doc["bufferViews"][position["bufferView"].get<unsigned>()];auto positions=viewBytes(bytes,positionView);
            const auto& indices=doc["accessors"][primitive["indices"].get<unsigned>()];auto indexBytes=viewBytes(bytes,doc["bufferViews"][indices["bufferView"].get<unsigned>()]);
            require(indices["count"].get<unsigned>()%3==0,"索引不是完整三角形");unsigned indexSize=indices["componentType"]==5123?2:4;
            require(indexSize==2 || indices["componentType"]==5125,"索引类型");
            for(unsigned corner=0;corner<indices["count"].get<unsigned>();++corner) {
                unsigned index=0;std::memcpy(&index,indexBytes.data()+indices.value("byteOffset",0U)+corner*indexSize,indexSize);
                require(index<position["count"].get<unsigned>(),"绘制索引越界");float xyz[3];
                std::memcpy(xyz,positions.data()+position.value("byteOffset",0U)+index*positionView.value("byteStride",12U),12);
                result.insert(pointKey(osg::Vec3d(xyz[0],xyz[1],xyz[2])*matrix*yToZ));
            }
        }
    }
    return result;
}
struct Audit {std::set<fs::path> uniqueContents;std::multiset<std::string> corners;std::size_t nodes=0,contentOccurrences=0;};
Audit audit(const fs::path& output) {
    auto manifest=Json::parse(readBytes(output/"tileset.json"));Audit result;
    const auto walk=[&](auto&& self,const Json& tile)->void {
        result.nodes++;bool content=false;
        if(tile.contains("content")) {
            content=true;auto uri=tile["content"]["uri"].get<std::string>();require(uri.find(':')==std::string::npos,"内容必须是本地 URI");
            auto file=fs::canonical(output/fs::u8path(uri));require(fs::is_regular_file(file),"内容引用不存在: "+uri);
            result.uniqueContents.insert(file);result.contentOccurrences++;auto corners=drawnCorners(file);result.corners.insert(corners.begin(),corners.end());
        }
        if(tile.contains("children")) for(const auto& child:tile["children"]) self(self,child);
        require(content || tile.contains("children"),"最终树存在空叶节点");
    };
    walk(walk,manifest["root"]);return result;
}
std::multiset<std::string> expectedCorners(const std::vector<double>& offsets) {
    std::multiset<std::string> result;
    for(double offset:offsets) for(const auto& point:{osg::Vec3d(offset,0,0),osg::Vec3d(offset+1,0,0),osg::Vec3d(offset,1,0)}) result.insert(pointKey(point));
    return result;
}
Audit expectDrawing(const Json& report,const fs::path& output,const std::vector<double>& offsets) {
    require(report["status"]=="complete","转换未成功");auto result=audit(output);require(result.corners==expectedCorners(offsets),"最终树实际绘制内容不符");
    std::set<fs::path> reported;
    for(const auto& row:report["records"]) for(const auto& content:row["content"]) reported.insert(fs::canonical(output/content["uri"].get<std::string>()));
    require(result.uniqueContents==reported,"报告内容与最终树引用集合不一致");
    std::set<fs::path> files;for(const auto& entry:fs::directory_iterator(output/"content")) if(entry.path().extension()==".glb") files.insert(fs::canonical(entry.path()));
    require(files==result.uniqueContents,"存在未被最终树引用的 GLB");return result;
}
std::size_t topRoots(const fs::path& output) {return Json::parse(readBytes(output/"tileset.json"))["root"]["children"].size();}
void pagedSlotsAndProxy(const fs::path& temp) {
    auto input=temp/"input";writeNode(triangle(20),input/"slot-one.osgb");writeNode(triangle(40),input/"slot-zero.osgb");
    writeNode(triangle(50),input/"both-zero.osgb");writeNode(triangle(60),input/"both-one.osgb");writeNode(triangle(90),input/"proxy.osgb");writeNode(triangle(100),input/"nested.osgb");
    auto root=new osg::Group;
    auto residentZero=new osg::PagedLOD;residentZero->addChild(triangle(10).get(),0,128);residentZero->setFileName(1,"slot-one.osgb");residentZero->setRange(1,128,1e9);root->addChild(residentZero);
    auto externalZero=new osg::PagedLOD;externalZero->addChild(triangle(999).get(),0,128);externalZero->setFileName(0,"slot-zero.osgb");
    auto residentOne=new osg::Group;residentOne->addChild(triangle(30));residentOne->addChild(external("nested.osgb"));externalZero->addChild(residentOne,128,1e9);root->addChild(externalZero);
    auto both=new osg::PagedLOD;both->addChild(triangle(999).get(),0,128);both->setFileName(0,"both-zero.osgb");both->setFileName(1,"both-one.osgb");both->setRange(1,128,1e9);root->addChild(both);
    auto proxy=new osg::ProxyNode;proxy->addChild(triangle(999).get(),"proxy.osgb");proxy->addChild(triangle(80).get());root->addChild(proxy);
    root->addChild(triangle(70));writeNode(root,input/"root.osgb");auto output=temp/"output";auto report=convert(options(input,output));
    require(report["files"]==7 && report["trianglesAllLods"]==10 && topRoots(output)==1,"PagedLOD/Proxy 引用数量或压缩常驻闭包");expectDrawing(report,output,{10,20,30,40,50,60,70,80,90,100});
    for(const auto& row:report["records"]) if(fs::path(row["source"].get<std::string>()).filename()=="root.osgb") {
        require(row["content"].size()==1,"根常驻内容应为一个单元");auto doc=gltf(output/row["content"][0]["uri"].get<std::string>());
        require(doc["meshes"].size()==1 && doc["meshes"][0]["primitives"].size()==4,"普通常驻与分页分支混合时丢失几何");
        return;
    }
    throw std::runtime_error("缺少根文件记录");
}
void sharedSameMatrixClosure(const fs::path& temp) {
    auto input=temp/"input";writeNode(triangle(10),input/"deep/leaf.osgb");writeNode(external("deep/leaf.osgb"),input/"left.osgb");writeNode(external("deep/leaf.osgb"),input/"right.osgb");
    auto root=new osg::Group;root->addChild(triangle(0));root->addChild(external("left.osgb"));root->addChild(external("right.osgb"));writeNode(root,input/"root.osgb");
    auto output=temp/"output";auto report=convert(options(input,output));require(report["files"]==4 && topRoots(output)==1,"间接共享引用闭包或根去重错误");
    auto drawing=expectDrawing(report,output,{0,10,10});require(drawing.uniqueContents.size()==2 && drawing.contentOccurrences==3,"同矩阵共享引用未复用内容");
}
void sharedDifferentMatrices(const fs::path& temp) {
    auto input=temp/"input";writeNode(triangle(10),input/"leaf.osgb");auto root=new osg::Group;root->addChild(triangle(0));
    for(double x:{100.0,300.0}) {auto transform=new osg::MatrixTransform;transform->setMatrix(osg::Matrixd::translate(x,0,0));transform->addChild(external("leaf.osgb"));root->addChild(transform);}
    writeNode(root,input/"root.osgb");auto output=temp/"output";auto report=convert(options(input,output));
    require(report["files"]==2 && report["records"].size()==3,"不同矩阵共享引用被错误合并");expectDrawing(report,output,{0,110,310});
}
void relativeDatabaseUtf8(const fs::path& temp) {
    auto input=temp/"input";writeNode(triangle(10),input/fs::u8path(u8"数据库/瓦片 ①/细层.OSGB"));
    auto paged=external(fs::u8path(u8"细层.OSGB").u8string());paged->setDatabasePath(fs::u8path(u8"../数据库\\瓦片 ①").u8string());
    auto root=new osg::Group;root->addChild(triangle(0));root->addChild(paged);writeNode(root,input/fs::u8path(u8"索引/索引.osgb"));
    auto output=temp/"output";auto report=convert(options(input/fs::u8path(u8"索引/索引.osgb"),output));
    require(report["files"]==2,"相对 databasePath 或 UTF-8 引用失败");expectDrawing(report,output,{0,10});
}
bool caseSensitive(const fs::path& directory) {
    fs::create_directories(directory);atomicWrite(directory/"CaseProbe",Bytes{1});return !fs::exists(directory/"caseprobe");
}
void casePaths(const fs::path& temp) {
    auto input=temp/"input";writeNode(triangle(10),input/"CaseLeaf.OSGB");auto root=new osg::Group;root->addChild(triangle(0));root->addChild(external("caseleaf.osgb"));writeNode(root,input/"Root.osgb");
    auto output=temp/"output";
    if(caseSensitive(temp)) {
        bool failed=false;try {convert(options(input,output));} catch(...) {failed=true;}
        require(failed && !fs::exists(output),"大小写敏感文件系统接受了错误引用大小写");
        auto distinct=temp/"distinct";writeNode(triangle(10),distinct/"CaseLeaf.OSGB");writeNode(triangle(20),distinct/"caseleaf.osgb");
        auto distinctRoot=new osg::Group;distinctRoot->addChild(triangle(0));distinctRoot->addChild(external("CaseLeaf.OSGB"));distinctRoot->addChild(external("caseleaf.osgb"));writeNode(distinctRoot,distinct/"Root.osgb");
        auto distinctOutput=temp/"distinct-output";auto report=convert(options(distinct,distinctOutput));
        require(report["files"]==3 && topRoots(distinctOutput)==1,"大小写不同的实际文件被合并");expectDrawing(report,distinctOutput,{0,10,20});
    } else {
        auto report=convert(options(input,output));require(report["files"]==2 && topRoots(output)==1,"大小写别名制造了重复根或源文件");expectDrawing(report,output,{0,10});
    }
}
void nodeMask(const fs::path& temp) {
    auto input=temp/"input";writeNode(triangle(20),input/"visible.osgb");writeNode(triangle(900),input/"hidden.osgb");auto root=new osg::Group;root->addChild(triangle(0));
    auto hidden=new osg::Group;hidden->setNodeMask(0);hidden->addChild(triangle(900));hidden->addChild(external("hidden.osgb"));root->addChild(hidden);
    auto visible=new osg::Group;visible->setNodeMask(2);visible->addChild(triangle(10));visible->addChild(external("visible.osgb"));root->addChild(visible);writeNode(root,input/"root.osgb");
    auto output=temp/"output";auto report=convert(options(input,output));require(report["files"]==2 && topRoots(output)==1,"NodeMask 改变了有效引用闭包");expectDrawing(report,output,{0,10,20});
    auto hiddenOnly=new osg::Group;hiddenOnly->setNodeMask(0);hiddenOnly->addChild(triangle(0));writeNode(hiddenOnly,temp/"hidden.osgb");
    bool failed=false;try {convert(options(temp/"hidden.osgb",temp/"hidden-output"));} catch(...) {failed=true;}
    require(failed && !fs::exists(temp/"hidden-output"),"全掩码输入不能发布空成功产物");
}
void badParentValidChild(const fs::path& temp) {
    auto input=temp/"input";writeNode(triangle(10),input/"valid.osgb");auto parent=triangle(0);auto geometry=parent->getDrawable(0)->asGeometry();
    auto colors=new osg::Vec4Array;colors->push_back({1,1,1,1});colors->push_back({1,1,1,1});colors->setBinding(osg::Array::BIND_PER_VERTEX);geometry->setColorArray(colors);
    auto paged=new osg::PagedLOD;paged->addChild(parent,0,128);paged->setFileName(1,"valid.osgb");paged->setRange(1,128,1e9);writeNode(paged,input/"root.osgb");
    bool failed=false;std::string message;try {convert(options(input/"root.osgb",temp/"output"));} catch(const std::exception& error) {failed=true;message=error.what();}
    require(failed && message.find("内容转换失败")!=std::string::npos && !fs::exists(temp/"output"),"坏父内容被有效孩子掩盖");
}
}
int main() {
    fs::path temp=fs::current_path()/("reference-data-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(temp);
    const std::vector<std::pair<std::string,std::function<void(const fs::path&)>>> cases={
        {"paged-slots-proxy",pagedSlotsAndProxy},{"shared-same-matrix-closure",sharedSameMatrixClosure},
        {"shared-different-matrices",sharedDifferentMatrices},{"relative-database-utf8",relativeDatabaseUtf8},{"case-paths",casePaths},
        {"node-mask",nodeMask},{"bad-parent-valid-child",badParentValidChild}};
    unsigned failed=0;
    for(const auto& item:cases) try {item.second(temp/item.first);std::cout<<"PASS "<<item.first<<'\n';} catch(const std::exception& error) {failed++;std::cerr<<"FAIL "<<item.first<<": "<<error.what()<<'\n';}
    std::cout<<"引用回归: "<<cases.size()-failed<<" 通过, "<<failed<<" 失败; 测试数据: "<<temp<<'\n';return failed?1:0;
}
