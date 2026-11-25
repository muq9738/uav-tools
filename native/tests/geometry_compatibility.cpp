// 图元与顶点属性兼容回归：自行生成 OSGB 夹具，端到端检查 GLB 的角点、索引与颜色。
// 仅从输入语义推导期望值，不复用 glb.cpp 的内部排列算法。
#include "native_tiles.hpp"
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osgDB/WriteFile>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nt;
namespace {
fs::path root;

[[noreturn]] void fail(const std::string& message) {throw std::runtime_error(message);}
void require(bool ok,const std::string& message) {if(!ok) fail(message);}
std::string number(double value) {
    std::string text=std::to_string(value);return text;
}
std::string label(const std::string& what,std::size_t index) {return what+"["+std::to_string(index)+"]";}

// ---------- GLB 读取（独立于转换器实现） ----------
struct Glb {
    Json doc;
    Bytes bytes;
    // 访问器逐元素读取；压缩视图只解码一次，避免 O(n^2)。
    mutable std::map<unsigned,Bytes> views;
};
Glb loadGlb(const fs::path& file) {
    Glb glb;glb.bytes=readBytes(file);
    require(glb.bytes.size()>20,"GLB 过短: "+file.u8string());
    std::uint32_t jsonLength=0;std::memcpy(&jsonLength,glb.bytes.data()+12,4);
    require(glb.bytes.size()>=28+jsonLength,"GLB JSON 块越界: "+file.u8string());
    glb.doc=Json::parse(glb.bytes.begin()+20,glb.bytes.begin()+20+jsonLength);
    return glb;
}
Bytes viewBytes(const Glb& glb,unsigned viewIndex) {
    auto cached=glb.views.find(viewIndex);
    if(cached!=glb.views.end()) return cached->second;
    const auto& view=glb.doc["bufferViews"].at(viewIndex);
    std::uint32_t jsonLength=0;std::memcpy(&jsonLength,glb.bytes.data()+12,4);
    std::size_t bin=28+jsonLength;
    auto store=[&](Bytes value) {glb.views[viewIndex]=value;return value;};
    if(view.contains("extensions") && view["extensions"].contains("EXT_meshopt_compression")) {
        const auto& ext=view["extensions"]["EXT_meshopt_compression"];
        auto start=bin+ext.value("byteOffset",0U);auto length=ext["byteLength"].get<std::size_t>();
        require(start+length<=glb.bytes.size(),"压缩视图越界");
        Bytes encoded(glb.bytes.begin()+start,glb.bytes.begin()+start+length);
        return store(decodeMeshopt(encoded,ext["count"],ext["byteStride"],ext["mode"]!="ATTRIBUTES"));
    }
    auto start=bin+view.value("byteOffset",0U);auto length=view["byteLength"].get<std::size_t>();
    require(start+length<=glb.bytes.size(),"原始视图越界");
    return store(Bytes(glb.bytes.begin()+start,glb.bytes.begin()+start+length));
}
Bytes elementBytes(const Glb& glb,unsigned accessor,unsigned index,unsigned elementSize) {
    const auto& a=glb.doc["accessors"].at(accessor);
    const auto& view=glb.doc["bufferViews"].at(a["bufferView"].get<unsigned>());
    auto data=viewBytes(glb,a["bufferView"].get<unsigned>());auto stride=view.value("byteStride",elementSize);
    auto offset=a.value("byteOffset",0U)+index*stride;
    require(offset+elementSize<=data.size(),"访问器元素越界");
    return Bytes(data.begin()+offset,data.begin()+offset+elementSize);
}
unsigned accessorWidth(const Json& accessor) {
    auto type=accessor["type"].get<std::string>();
    if(type=="SCALAR") return 1;
    if(type=="VEC2") return 2;
    if(type=="VEC3") return 3;
    require(type=="VEC4","未知访问器类型: "+type);return 4;
}
std::vector<float> accessorFloats(const Glb& glb,unsigned accessor,unsigned index) {
    const auto& a=glb.doc["accessors"].at(accessor);
    require(a["componentType"]==5126,"属性必须以 FLOAT 写出，实际 "+std::to_string(a["componentType"].get<unsigned>()));
    unsigned width=accessorWidth(a);auto raw=elementBytes(glb,accessor,index,width*4);
    std::vector<float> value(width);std::memcpy(value.data(),raw.data(),width*4);return value;
}
std::vector<std::uint32_t> accessorIndices(const Glb& glb,unsigned accessor) {
    const auto& a=glb.doc["accessors"].at(accessor);
    auto count=a["count"].get<unsigned>();unsigned type=a["componentType"].get<unsigned>();
    require(type==5123 || type==5125,"索引分量类型非法: "+std::to_string(type));
    unsigned elementSize=type==5123?2:4;auto values=std::vector<std::uint32_t>(count);
    for(unsigned i=0;i<count;++i) {
        auto raw=elementBytes(glb,accessor,i,elementSize);
        if(type==5123) {std::uint16_t v=0;std::memcpy(&v,raw.data(),2);values[i]=v;}
        else std::memcpy(&values[i],raw.data(),4);
    }
    return values;
}
struct PrimitiveView {
    unsigned mode=4;
    std::vector<std::uint32_t> indices;
    std::vector<std::vector<float>> positions,normals,colors,uvs;
};
std::vector<PrimitiveView> readPrimitives(const Glb& glb) {
    std::vector<PrimitiveView> result;
    for(const auto& mesh:glb.doc["meshes"])
        for(const auto& primitive:mesh["primitives"]) {
            PrimitiveView view;
            view.mode=primitive.value("mode",4U);
            view.indices=accessorIndices(glb,primitive["indices"].get<unsigned>());
            const auto& attributes=primitive["attributes"];
            auto colors=attributes.contains("COLOR_0")?attributes["COLOR_0"].get<unsigned>():0;
            auto uvs=attributes.contains("TEXCOORD_0")?attributes["TEXCOORD_0"].get<unsigned>():0;
            for(auto index:view.indices) {
                view.positions.push_back(accessorFloats(glb,attributes["POSITION"].get<unsigned>(),index));
                if(attributes.contains("NORMAL")) view.normals.push_back(accessorFloats(glb,attributes["NORMAL"].get<unsigned>(),index));
                if(colors) view.colors.push_back(accessorFloats(glb,colors,index));
                if(uvs) view.uvs.push_back(accessorFloats(glb,uvs,index));
            }
            result.push_back(std::move(view));
        }
    return result;
}

// ---------- 夹具与转换 ----------
void writeNode(osg::Node* node,const fs::path& file,bool compressed=false) {
    fs::create_directories(file.parent_path());
    osg::ref_ptr<osgDB::Options> options=new osgDB::Options(std::string("WriteImageHint=IncludeFile")+(compressed?" Compressor=zlib":""));
    require(osgDB::writeNodeFile(*node,file.u8string(),options.get()),"不能写出 OSGB 夹具: "+file.u8string());
}
osg::ref_ptr<osg::Vec3Array> grid(unsigned count) {
    auto values=new osg::Vec3Array;
    for(unsigned i=0;i<count;++i) values->push_back({float(i%256),float(i%4+i/256),-0.5f*float(i%256)});
    return values;
}
std::array<double,3> gridPoint(unsigned i) {return {double(i%256),double(i%4+i/256),-0.5*double(i%256)};}
float normalizedByte(unsigned value) {return static_cast<float>(value/255.0);}
float normalizedShort(unsigned value) {return static_cast<float>(value/65535.0);}
Json audit=Json::array();

struct Converted {
    Json report;
    Glb glb;
    std::vector<PrimitiveView> primitives;
};
Options baseOptions(const fs::path& input,const fs::path& output) {
    Options options;options.input=input;options.output=output;
    options.compressGeometry=false;options.srs="ENU:32.427247976,120.6706181";options.origin="1,2,30.665";
    return options;
}
Converted convertFixture(const std::string& name,osg::Node* node,bool compressedSource=false,bool compressedGeometry=false) {
    auto file=root/(name+".osgb");writeNode(node,file,compressedSource);
    auto options=baseOptions(file,root/("out-"+name));options.compressGeometry=compressedGeometry;
    Converted converted;converted.report=convert(options);
    const auto& content=converted.report["records"][0]["content"][0];
    converted.glb=loadGlb(root/("out-"+name)/content["uri"].get<std::string>());
    converted.primitives=readPrimitives(converted.glb);
    require(!converted.primitives.empty(),name+": 没有写出 primitive");
    Json row={{"case",name},{"triangles",converted.report["trianglesAllLods"]},{"vertices",converted.report["verticesAllLods"]},
              {"primitives",Json::array()}};
    for(const auto& primitive:converted.primitives) {
        Json item={{"mode",primitive.mode},{"indices",primitive.indices},{"positions",primitive.positions}};
        if(!primitive.normals.empty()) item["normals"]=primitive.normals;
        if(!primitive.colors.empty()) item["colors"]=primitive.colors;
        row["primitives"].push_back(std::move(item));
    }
    audit.push_back(std::move(row));
    return converted;
}

// ---------- 断言 ----------
void requireIndices(const PrimitiveView& primitive,const std::vector<std::uint32_t>& expected,const std::string& what) {
    require(primitive.indices.size()==expected.size(),
        what+": 索引数量 "+std::to_string(primitive.indices.size())+" != "+std::to_string(expected.size()));
    for(std::size_t i=0;i<expected.size();++i)
        if(primitive.indices[i]!=expected[i])
            fail(label(what+": 索引",i)+" "+std::to_string(primitive.indices[i])+" != "+std::to_string(expected[i]));
}
void requireCorners(const PrimitiveView& primitive,const std::vector<std::uint32_t>& expectedIndices,
                    const std::vector<std::array<double,3>>& expectedPositions,const std::string& what) {
    requireIndices(primitive,expectedIndices,what);
    require(primitive.positions.size()==expectedPositions.size(),what+": 角点位置数量不匹配");
    for(std::size_t corner=0;corner<expectedPositions.size();++corner) {
        const auto& actual=primitive.positions[corner];const auto& expected=expectedPositions[corner];
        for(unsigned c=0;c<3;++c) if(std::abs(actual[c]-expected[c])>1e-6)
            fail(label(what+": 角点位置",corner)+" 分量 "+std::to_string(c)+" = "+number(actual[c])+
                 " != "+number(expected[c])+" (索引 "+std::to_string(primitive.indices[corner])+")");
    }
}
void requireColors(const PrimitiveView& primitive,const std::vector<std::vector<float>>& expected,const std::string& what) {
    require(primitive.colors.size()==expected.size(),what+": 颜色角点数量不匹配");
    for(std::size_t corner=0;corner<expected.size();++corner) {
        require(!primitive.colors[corner].empty(),label(what+": 颜色缺失",corner));
        require(primitive.colors[corner].size()==expected[corner].size(),
            label(what+": 颜色分量数",corner)+" "+std::to_string(primitive.colors[corner].size())+" != "+std::to_string(expected[corner].size()));
        for(unsigned c=0;c<expected[corner].size();++c) {
            auto actual=primitive.colors[corner][c];auto want=expected[corner][c];
            if(std::abs(actual-want)>1e-7f)
                fail(label(what+": 颜色",corner)+" 分量 "+std::to_string(c)+" = "+number(actual)+" != "+number(want));
        }
    }
}
void requireUvs(const PrimitiveView& primitive,const std::vector<std::vector<float>>& expected,const std::string& what) {
    require(primitive.uvs.size()==expected.size(),what+": UV 角点数量不匹配");
    for(std::size_t corner=0;corner<expected.size();++corner) {
        require(primitive.uvs[corner].size()==expected[corner].size(),label(what+": UV 分量数",corner));
        for(unsigned c=0;c<expected[corner].size();++c)
            if(std::abs(primitive.uvs[corner][c]-expected[corner][c])>1e-7f)
                fail(label(what+": UV",corner)+" 分量 "+std::to_string(c)+" = "+number(primitive.uvs[corner][c])+
                     " != "+number(expected[corner][c]));
    }
}
std::vector<std::vector<float>> repeatedColor(const std::vector<float>& color,std::size_t corners) {
    return std::vector<std::vector<float>>(corners,color);
}
// 由源顶点号推出期望角点位置，避免复用转换器的置换结果。
std::vector<std::array<double,3>> cornersOf(const std::vector<unsigned>& sourceIndices) {
    std::vector<std::array<double,3>> result;
    for(auto index:sourceIndices) result.push_back(gridPoint(index));
    return result;
}
osg::ref_ptr<osg::Geode> geode(osg::Geometry* geometry) {auto node=new osg::Geode;node->addDrawable(geometry);return node;}
osg::ref_ptr<osg::Geometry> geometryOf(unsigned vertices,osg::PrimitiveSet* primitive) {
    auto geometry=new osg::Geometry;geometry->setVertexArray(grid(vertices));geometry->addPrimitiveSet(primitive);
    return geometry;
}

// ---------- 1. TRIANGLE_FAN ----------
void fanCases() {
    // 圆盘形 7 顶点扇面：中心 0 号，独立期望 5 个三角形。
    const float disks[7][3]={{0,0,0},{1,0,0},{1,1,0},{0,1,0},{-1,1,0},{-1,0,0},{-1,-1,0}};
    auto disk=[&] {auto values=new osg::Vec3Array;for(const auto& p:disks) values->push_back({p[0],p[1],p[2]});return values;};
    auto fan=new osg::Geometry;fan->setVertexArray(disk());fan->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN,0,7));
    auto fanResult=convertFixture("fan-array",geode(fan));
    require(fanResult.primitives.size()==1,"fan-array: 单个 primitive set 只能产出一个 primitive");
    require(fanResult.primitives[0].mode==4,"fan-array: 扇面必须写为 TRIANGLES");
    require(fanResult.report["trianglesAllLods"]==5,"fan-array: 7 顶点扇面应为 5 个三角形，实际 "+fanResult.report["trianglesAllLods"].dump());
    std::vector<std::array<double,3>> fanCorners;
    const unsigned fanSource[15]={0,1,2,0,2,3,0,3,4,0,4,5,0,5,6};
    for(auto index:fanSource) fanCorners.push_back({disks[index][0],disks[index][1],disks[index][2]});
    requireCorners(fanResult.primitives[0],{0,1,2,0,2,3,0,3,4,0,4,5,0,5,6},fanCorners,"fan-array");

    // 索引乱序扇面：锚点是 run 的第一个索引（3 号），不是顶点数组 0 号。
    auto elements=new osg::Geometry;elements->setVertexArray(disk());
    auto indices=new osg::DrawElementsUInt(GL_TRIANGLE_FAN);
    for(unsigned index:{3U,0U,5U,2U,6U,1U,4U}) indices->push_back(index);
    elements->addPrimitiveSet(indices);
    auto elementResult=convertFixture("fan-elements",geode(elements));
    require(elementResult.primitives.size()==1 && elementResult.primitives[0].mode==4,"fan-elements: 必须写为单个 TRIANGLES primitive");
    require(elementResult.report["trianglesAllLods"]==5,"fan-elements: 7 角点扇面应为 5 个三角形");
    std::vector<std::array<double,3>> elementCorners;
    const unsigned elementSource[15]={3,0,5,3,5,2,3,2,6,3,6,1,3,1,4};
    for(auto index:elementSource) elementCorners.push_back({disks[index][0],disks[index][1],disks[index][2]});
    requireCorners(elementResult.primitives[0],{0,1,2,0,2,3,0,3,4,0,4,5,0,5,6},elementCorners,"fan-elements");

    // 分段扇面：每段重新以本段起点为中心，段间不得桥接。
    auto segments=new osg::Geometry;segments->setVertexArray(grid(10));
    auto lengths=new osg::DrawArrayLengths(GL_TRIANGLE_FAN,1);lengths->push_back(3);lengths->push_back(3);lengths->push_back(3);
    segments->addPrimitiveSet(lengths);
    auto segmentResult=convertFixture("fan-segments",geode(segments));
    require(segmentResult.primitives.size()==3,"fan-segments: 三个分段必须各自成型");
    require(segmentResult.report["trianglesAllLods"]==3,"fan-segments: 三段各 1 个三角形");
    const unsigned segmentSource[3][3]={{1,2,3},{4,5,6},{7,8,9}};
    for(unsigned segment=0;segment<3;++segment) {
        require(segmentResult.primitives[segment].mode==4,"fan-segments: 分段 mode 必须是 TRIANGLES");
        // 索引槽位是全几何的首次访问顺序，分段 k 的角点落在 3k..3k+2。
        requireCorners(segmentResult.primitives[segment],{3*segment,3*segment+1,3*segment+2},
            cornersOf({segmentSource[segment][0],segmentSource[segment][1],segmentSource[segment][2]}),
            "fan-segments["+std::to_string(segment)+"]");
    }

    // DrawArrays 非零 first 的扇面。
    auto offset=new osg::Geometry;offset->setVertexArray(grid(7));offset->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN,2,4));
    auto offsetResult=convertFixture("fan-first",geode(offset));
    require(offsetResult.report["trianglesAllLods"]==2,"fan-first: 4 角点扇面应为 2 个三角形");
    requireCorners(offsetResult.primitives[0],{0,1,2,0,2,3},cornersOf({2,3,4,2,4,5}),"fan-first");
    const auto& positionAccessor=offsetResult.glb.doc["accessors"][offsetResult.glb.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<unsigned>()];
    require(positionAccessor["count"]==7,"fan-first: 未引用顶点必须保留");

    // 扇面首三角形退化（中心重复）：删除退化面，后续面缠绕不变。
    auto degenerate=new osg::Geometry;degenerate->setVertexArray(grid(6));
    auto degenerateIndices=new osg::DrawElementsUInt(GL_TRIANGLE_FAN);
    for(unsigned index:{1U,1U,2U,3U}) degenerateIndices->push_back(index);degenerate->addPrimitiveSet(degenerateIndices);
    auto degenerateResult=convertFixture("fan-degenerate",geode(degenerate));
    require(degenerateResult.report["trianglesAllLods"]==1,"fan-degenerate: 4 角点扇面去掉 1 个退化面后只剩 1 个三角形");
    requireCorners(degenerateResult.primitives[0],{0,1,2},cornersOf({1,2,3}),"fan-degenerate");
}

// ---------- 2. DrawArrays 非零 first/count、strip 展开 ----------
void drawArraysCases() {
    auto triangles=geometryOf(9,new osg::DrawArrays(GL_TRIANGLES,2,6));
    auto triangleResult=convertFixture("draw-range-triangles",geode(triangles));
    require(triangleResult.report["trianglesAllLods"]==2,"draw-range-triangles: 6 角点应为 2 个三角形");
    requireCorners(triangleResult.primitives[0],{0,1,2,3,4,5},cornersOf({2,3,4,5,6,7}),"draw-range-triangles");
    const auto& accessor=triangleResult.glb.doc["accessors"][triangleResult.glb.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<unsigned>()];
    require(accessor["count"]==9,"draw-range-triangles: 必须保留未引用顶点");
    // 未引用槽位顺序：先出现被绘制顶点，再按源顺序补齐，因此末两位是源 0、1 号。
    auto slot=[](const Converted& converted,unsigned index) {
        return accessorFloats(converted.glb,converted.glb.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<unsigned>(),index);
    };
    std::vector<unsigned> slotOrder={2,3,4,5,6,7,0,1,8};
    for(unsigned i=0;i<9;++i) {
        auto actual=slot(triangleResult,i);auto expected=gridPoint(slotOrder[i]);
        for(unsigned c=0;c<3;++c) require(std::abs(actual[c]-expected[c])<1e-6,label("draw-range-triangles: 存储槽位",i)+" 保留失败");
    }

    auto strip=geometryOf(8,new osg::DrawArrays(GL_TRIANGLE_STRIP,3,4));
    auto stripResult=convertFixture("draw-range-strip",geode(strip));
    require(stripResult.report["trianglesAllLods"]==2,"draw-range-strip: 4 角点条带应为 2 个三角形");
    requireCorners(stripResult.primitives[0],{0,1,2,2,1,3},cornersOf({3,4,5,5,4,6}),"draw-range-strip");

    // 分段条带的第一段只有 2 角点（无三角形）：后续段的顶点槽位不得错位。
    auto emptyRun=new osg::Geometry;emptyRun->setVertexArray(grid(8));
    auto lengths=new osg::DrawArrayLengths(GL_TRIANGLE_STRIP,2);lengths->push_back(2);lengths->push_back(4);
    emptyRun->addPrimitiveSet(lengths);
    auto emptyResult=convertFixture("strip-empty-run",geode(emptyRun));
    require(emptyResult.primitives.size()==1,"strip-empty-run: 只有产生三角形的分段才能成 primitive");
    require(emptyResult.report["trianglesAllLods"]==2,"strip-empty-run: 第二段 4 角点应为 2 个三角形");
    requireCorners(emptyResult.primitives[0],{2,3,4,4,3,5},cornersOf({4,5,6,6,5,7}),"strip-empty-run");
}

// ---------- 3. 混合 primitive ----------
void mixedPrimitiveCase() {
    auto geometry=new osg::Geometry;geometry->setVertexArray(grid(15));
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    auto strip=new osg::DrawElementsUInt(GL_TRIANGLE_STRIP);for(unsigned index:{3U,4U,5U,6U}) strip->push_back(index);
    geometry->addPrimitiveSet(strip);
    auto fan=new osg::DrawArrayLengths(GL_TRIANGLE_FAN,7);fan->push_back(4);fan->push_back(4);geometry->addPrimitiveSet(fan);
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_LINES,0,4));
    auto colors=new osg::Vec4Array;
    colors->push_back({1,0,0,1});colors->push_back({0,1,0,1});colors->push_back({0,0,1,1});colors->push_back({1,1,0,0.5f});
    colors->setBinding(osg::Array::BIND_PER_PRIMITIVE_SET);geometry->setColorArray(colors);
    auto result=convertFixture("mixed-primitives",geode(geometry));
    require(result.primitives.size()==5,"mixed-primitives: 应产出 5 个 primitive（分段扇面各一），实际 "+std::to_string(result.primitives.size()));
    require(result.report["trianglesAllLods"]==7,"mixed-primitives: 三角形总数应为 1+2+2+2");
    const unsigned expectedModes[5]={4,4,4,4,1};
    const std::vector<std::uint32_t> expectedIndices[5]={{0,1,2},{3,4,5,5,4,6},{7,8,9,7,9,10},{11,12,13,11,13,14},{0,1,2,3}};
    const unsigned expectedSource[5][6]={{0,1,2},{3,4,5,5,4,6},{7,8,9,7,9,10},{11,12,13,11,13,14},{0,1,2,3}};
    const std::vector<float> expectedColors[4]={{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,0,0.5f}};
    const unsigned colorOfPrimitive[5]={0,1,2,2,3};
    for(unsigned p=0;p<5;++p) {
        const auto& primitive=result.primitives[p];auto name="mixed-primitives["+std::to_string(p)+"]";
        require(primitive.mode==expectedModes[p],name+": mode "+std::to_string(primitive.mode)+" != "+std::to_string(expectedModes[p]));
        std::vector<unsigned> sources(expectedSource[p],expectedSource[p]+expectedIndices[p].size());
        std::vector<std::array<double,3>> positions;for(auto index:sources) positions.push_back(gridPoint(index));
        requireCorners(primitive,expectedIndices[p],positions,name);
        requireColors(primitive,repeatedColor(expectedColors[colorOfPrimitive[p]],primitive.indices.size()),name);
    }
}

// ---------- 4. 颜色：byte/float × RGB/RGBA × binding ----------
void colorCases() {
    // per-vertex byte RGB
    auto byteRgb=new osg::Geometry;byteRgb->setVertexArray(grid(3));byteRgb->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    auto rgb=new osg::Vec3ubArray;rgb->push_back({0,64,128});rgb->push_back({255,128,0});rgb->push_back({32,200,255});
    rgb->setBinding(osg::Array::BIND_PER_VERTEX);byteRgb->setColorArray(rgb);
    auto byteRgbResult=convertFixture("color-byte-rgb",geode(byteRgb));
    {
        std::vector<std::vector<float>> expected={{{normalizedByte(0),normalizedByte(64),normalizedByte(128)}},
            {{normalizedByte(255),normalizedByte(128),normalizedByte(0)}},{{normalizedByte(32),normalizedByte(200),normalizedByte(255)}}};
        requireColors(byteRgbResult.primitives[0],expected,"color-byte-rgb");
        const auto& accessor=byteRgbResult.glb.doc["accessors"][byteRgbResult.glb.doc["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"].get<unsigned>()];
        require(accessor["type"]=="VEC3","color-byte-rgb: RGB 源不能扩成 VEC4");
    }
    // per-vertex byte RGBA
    auto byteRgba=new osg::Geometry;byteRgba->setVertexArray(grid(3));byteRgba->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    auto rgba=new osg::Vec4ubArray;rgba->push_back({10,20,30,40});rgba->push_back({200,100,50,25});rgba->push_back({1,2,3,4});
    byteRgba->setColorArray(rgba);
    auto byteRgbaResult=convertFixture("color-byte-rgba",geode(byteRgba));
    {
        std::vector<std::vector<float>> expected={{{normalizedByte(10),normalizedByte(20),normalizedByte(30),normalizedByte(40)}},
            {{normalizedByte(200),normalizedByte(100),normalizedByte(50),normalizedByte(25)}},{{normalizedByte(1),normalizedByte(2),normalizedByte(3),normalizedByte(4)}}};
        requireColors(byteRgbaResult.primitives[0],expected,"color-byte-rgba");
        const auto& accessor=byteRgbaResult.glb.doc["accessors"][byteRgbaResult.glb.doc["meshes"][0]["primitives"][0]["attributes"]["COLOR_0"].get<unsigned>()];
        require(accessor["type"]=="VEC4","color-byte-rgba: RGBA 源必须保持 VEC4");
    }
    // per-vertex float RGB / RGBA
    auto floatRgb=new osg::Geometry;floatRgb->setVertexArray(grid(3));floatRgb->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    auto frgb=new osg::Vec3Array;frgb->push_back({0.5f,0.25f,1.0f});frgb->push_back({0.125f,0.75f,0.0f});frgb->push_back({1.0f,1.0f,0.5f});
    frgb->setBinding(osg::Array::BIND_PER_VERTEX);floatRgb->setColorArray(frgb);
    auto floatRgbResult=convertFixture("color-float-rgb",geode(floatRgb));
    requireColors(floatRgbResult.primitives[0],{{{0.5f,0.25f,1.0f}},{{0.125f,0.75f,0.0f}},{{1.0f,1.0f,0.5f}}},"color-float-rgb");
    auto floatRgba=new osg::Geometry;floatRgba->setVertexArray(grid(3));floatRgba->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    auto frgba=new osg::Vec4Array;frgba->push_back({0.5f,0.25f,1.0f,0.75f});frgba->push_back({0.125f,0.75f,0.0f,0.5f});frgba->push_back({1.0f,1.0f,0.5f,0.0f});
    frgba->setBinding(osg::Array::BIND_PER_VERTEX);floatRgba->setColorArray(frgba);
    auto floatRgbaResult=convertFixture("color-float-rgba",geode(floatRgba));
    requireColors(floatRgbaResult.primitives[0],{{{0.5f,0.25f,1.0f,0.75f}},{{0.125f,0.75f,0.0f,0.5f}},{{1.0f,1.0f,0.5f,0.0f}}},"color-float-rgba");
    // BIND_OVERALL：单个颜色广播到所有角点。
    auto overall=new osg::Geometry;overall->setVertexArray(grid(4));auto overallStrip=new osg::DrawElementsUInt(GL_TRIANGLE_STRIP);
    for(unsigned index:{0U,1U,2U,3U}) overallStrip->push_back(index);overall->addPrimitiveSet(overallStrip);
    auto overallColors=new osg::Vec4ubArray;overallColors->push_back({255,0,128,64});
    overallColors->setBinding(osg::Array::BIND_OVERALL);overall->setColorArray(overallColors);
    auto overallResult=convertFixture("color-overall",geode(overall));
    require(overallResult.report["trianglesAllLods"]==2,"color-overall: 4 角点条带应为 2 个三角形");
    requireColors(overallResult.primitives[0],
        repeatedColor({1.0f,0.0f,normalizedByte(128),normalizedByte(64)},6),"color-overall");
    // BIND_PER_PRIMITIVE_SET：每个 primitive set 使用自己的颜色。
    auto perSet=new osg::Geometry;perSet->setVertexArray(grid(6));
    perSet->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    perSet->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,3,3));
    auto setColors=new osg::Vec3Array;setColors->push_back({0.25f,0.5f,0.75f});setColors->push_back({0.9f,0.1f,0.3f});
    setColors->setBinding(osg::Array::BIND_PER_PRIMITIVE_SET);perSet->setColorArray(setColors);
    auto perSetResult=convertFixture("color-per-set",geode(perSet));
    require(perSetResult.primitives.size()==2,"color-per-set: 两个 primitive set 必须各自成型");
    requireColors(perSetResult.primitives[0],repeatedColor({0.25f,0.5f,0.75f},3),"color-per-set[0]");
    requireColors(perSetResult.primitives[1],repeatedColor({0.9f,0.1f,0.3f},3),"color-per-set[1]");
    requireCorners(perSetResult.primitives[1],{3,4,5},cornersOf({3,4,5}),"color-per-set[1]");
    // 归一化 ushort 颜色。
    auto shortColors=new osg::Geometry;shortColors->setVertexArray(grid(3));shortColors->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    auto us=new osg::Vec4usArray;us->push_back({65535,0,0,65535});us->push_back({0,65535,0,32768});us->push_back({0,0,65535,0});
    us->setBinding(osg::Array::BIND_PER_VERTEX);shortColors->setColorArray(us);
    auto shortResult=convertFixture("color-ushort",geode(shortColors));
    requireColors(shortResult.primitives[0],{{{1.0f,0.0f,0.0f,1.0f}},{{0.0f,1.0f,0.0f,normalizedShort(32768)}},{{0.0f,0.0f,1.0f,0.0f}}},"color-ushort");
    // 数量与绑定不匹配必须失败，不得删属性后发布。
    auto badCount=new osg::Geometry;badCount->setVertexArray(grid(6));badCount->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,6));
    auto shortColors2=new osg::Vec4Array;shortColors2->push_back({1,1,1,1});
    badCount->setColorArray(shortColors2);
    auto badFile=root/"color-bad-count.osgb";writeNode(geode(badCount),badFile);
    bool failed=false;
    try {convert(baseOptions(badFile,root/"out-color-bad-count"));} catch(const std::exception&) {failed=true;}
    require(failed && !fs::exists(root/"out-color-bad-count"),"color-bad-count: 属性数量错误必须失败且不发布");
}

// ---------- 4b. UV binding ----------
void uvBindingCases() {
    auto geometry=new osg::Geometry;geometry->setVertexArray(grid(6));
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,3,3));
    // BIND_PER_PRIMITIVE_SET：每个 primitive set 用自己的 UV，不能全部取 0 号元素。
    auto perSet=new osg::Vec2Array;perSet->push_back({0.125f,0.25f});perSet->push_back({0.75f,0.5f});
    perSet->setBinding(osg::Array::BIND_PER_PRIMITIVE_SET);
    auto perSetGeometry=new osg::Geometry(*geometry,osg::CopyOp::DEEP_COPY_ALL);
    // OSG 的 setTexCoordArray 默认把 binding 重置为 UNDEFINED，必须显式声明 per-set。
    perSetGeometry->setTexCoordArray(0,perSet,osg::Array::BIND_PER_PRIMITIVE_SET);
    auto perSetResult=convertFixture("uv-per-set",geode(perSetGeometry));
    require(perSetResult.primitives.size()==2,"uv-per-set: 两个 primitive set 必须各自成型");
    requireUvs(perSetResult.primitives[0],repeatedColor({0.125f,0.25f},3),"uv-per-set[0]");
    requireUvs(perSetResult.primitives[1],repeatedColor({0.75f,0.5f},3),"uv-per-set[1]");
    // BIND_OVERALL：单个 UV 广播到两个 primitive set。
    auto overall=new osg::Vec2Array;overall->push_back({0.5f,0.625f});
    overall->setBinding(osg::Array::BIND_OVERALL);
    auto overallGeometry=new osg::Geometry(*geometry,osg::CopyOp::DEEP_COPY_ALL);
    overallGeometry->setTexCoordArray(0,overall,osg::Array::BIND_OVERALL);
    auto overallResult=convertFixture("uv-overall",geode(overallGeometry));
    for(unsigned p=0;p<overallResult.primitives.size();++p)
        requireUvs(overallResult.primitives[p],repeatedColor({0.5f,0.625f},3),"uv-overall["+std::to_string(p)+"]");
    // BIND_PER_VERTEX：角点各自取自己的 UV。
    auto perVertex=new osg::Vec2Array;
    for(unsigned i=0;i<6;++i) perVertex->push_back({float(i)/8.0f,1.0f-float(i)/16.0f});
    perVertex->setBinding(osg::Array::BIND_PER_VERTEX);
    auto perVertexGeometry=new osg::Geometry(*geometry,osg::CopyOp::DEEP_COPY_ALL);
    perVertexGeometry->setTexCoordArray(0,perVertex);
    auto perVertexResult=convertFixture("uv-per-vertex",geode(perVertexGeometry));
    for(unsigned p=0;p<perVertexResult.primitives.size();++p) {
        std::vector<std::vector<float>> expected;
        for(std::size_t corner=0;corner<perVertexResult.primitives[p].indices.size();++corner) {
            auto source=perVertexResult.primitives[p].indices[corner];
            expected.push_back({float(source)/8.0f,1.0f-float(source)/16.0f});
        }
        requireUvs(perVertexResult.primitives[p],expected,"uv-per-vertex["+std::to_string(p)+"]");
    }
}

// ---------- 5. 共享属性数组不得被重复变换 ----------
void sharedArrayCases() {
    // 5.1 同一 geometry 的多个 primitive set 共享 per-vertex 法线：投影 + 旋转矩阵下只能变换一次。
    auto geometry=new osg::Geometry;geometry->setVertexArray(grid(9));
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    auto strip=new osg::DrawElementsUInt(GL_TRIANGLE_STRIP);for(unsigned index:{3U,4U,5U,6U}) strip->push_back(index);geometry->addPrimitiveSet(strip);
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_FAN,6,3));
    auto normals=new osg::Vec3Array;
    for(unsigned i=0;i<9;++i) normals->push_back({0.6f,0.0f,0.8f});
    normals->setBinding(osg::Array::BIND_PER_VERTEX);geometry->setNormalArray(normals);
    auto rotation=osg::Matrixd::rotate(0.7,osg::Vec3d(0,0,1))*osg::Matrixd::translate(11.5,-3.25,7.0);
    auto transform=new osg::MatrixTransform(rotation);transform->addChild(geode(geometry));
    auto file=root/"shared-primitive-sets.osgb";writeNode(transform,file);
    auto options=baseOptions(file,root/"out-shared-primitive-sets");options.srs="EPSG:32651";options.origin="500000,0,10";
    auto report=convert(options);
    auto glb=loadGlb(root/"out-shared-primitive-sets"/report["records"][0]["content"][0]["uri"].get<std::string>());
    auto primitives=readPrimitives(glb);
    require(primitives.size()==3,"shared-primitive-sets: 三个 primitive set 必须各自成型");
    SpatialReference spatial("EPSG:32651",{500000,0,10});
    osg::Matrixd inverse;require(inverse.invert(rotation),"共享数组用例矩阵可逆");osg::Matrixd inverseTranspose;inverseTranspose.transpose(inverse);
    std::vector<std::array<double,3>> expectedNormals;
    for(unsigned i=0;i<9;++i) {
        auto point=osg::Vec3d(gridPoint(i)[0],gridPoint(i)[1],gridPoint(i)[2])*rotation;
        auto transformed=osg::Matrixd::transform3x3(osg::Vec3d(0.6,0,0.8),inverseTranspose);
        auto local=spatial.normalToLocal(point,transformed);local.normalize();
        expectedNormals.push_back({local.x(),local.y(),local.z()});
    }
    for(unsigned p=0;p<primitives.size();++p) {
        require(primitives[p].normals.size()==primitives[p].indices.size(),"shared-primitive-sets: 法线缺失");
        for(std::size_t corner=0;corner<primitives[p].indices.size();++corner) {
            auto source=primitives[p].indices[corner];
            require(source<expectedNormals.size(),"shared-primitive-sets: 索引越界");
            const auto& actual=primitives[p].normals[corner];const auto& expected=expectedNormals[source];
            double length=0;for(unsigned c=0;c<3;++c) length+=double(actual[c])*actual[c];
            if(std::abs(std::sqrt(length)-1.0)>1e-5)
                fail("shared-primitive-sets: 法线未归一化，可能被重复变换 ("+number(std::sqrt(length))+")");
            for(unsigned c=0;c<3;++c) if(std::abs(actual[c]-expected[c])>1e-5)
                fail("shared-primitive-sets: primitive "+std::to_string(p)+" 角点 "+std::to_string(corner)+
                     " 法线分量 "+std::to_string(c)+" = "+number(actual[c])+" != "+number(expected[c])+"（共享数组被重复变换）");
        }
    }
    audit.push_back({{"case","shared-primitive-sets"},{"normals",[&] {Json rows=Json::array();for(const auto& p:primitives) rows.push_back(p.normals);return rows;}()}});

    // 5.2 DAG：两个 GeometryInput 共享同一个 osg::Geometry 实例，属性必须各自只变换一次，源数组不得被改写。
    auto shared=new osg::Geometry;auto positions=new osg::Vec3Array;
    for(unsigned i=0;i<6;++i) positions->push_back({float(i)*1.5f,float(i%3),-0.25f*float(i)});
    shared->setVertexArray(positions);
    shared->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,6));
    auto sharedNormals=new osg::Vec3Array;for(unsigned i=0;i<6;++i) sharedNormals->push_back({0.0f,0.6f,0.8f});
    sharedNormals->setBinding(osg::Array::BIND_PER_VERTEX);shared->setNormalArray(sharedNormals);
    auto sharedColors=new osg::Vec4Array;for(unsigned i=0;i<6;++i) sharedColors->push_back({0.1f*float(i),0.2f,0.3f,1.0f});
    sharedColors->setBinding(osg::Array::BIND_PER_VERTEX);shared->setColorArray(sharedColors);
    auto snapshot=[](const osg::Array* array) {
        auto bytes=static_cast<std::size_t>(array->getTotalDataSize());auto data=static_cast<const unsigned char*>(array->getDataPointer());
        return Bytes(data,data+bytes);
    };
    auto positionBefore=snapshot(positions),normalBefore=snapshot(sharedNormals),colorBefore=snapshot(sharedColors);
    auto matrix=osg::Matrixd::rotate(0.35,osg::Vec3d(0,0,1))*osg::Matrixd::translate(4.5,6.25,-1.5);
    auto state=new osg::StateSet;
    std::vector<GeometryInput> duplicated{{shared,state,matrix},{shared,state,matrix}};
    std::vector<GeometryInput> single{{shared,state,matrix}};
    writeGlb(duplicated,root/"shared-dag.glb",root/"shared-textures",true,&spatial,false);
    writeGlb(single,root/"shared-single.glb",root/"shared-textures",true,&spatial,false);
    auto duplicatedGlb=loadGlb(root/"shared-dag.glb");auto singleGlb=loadGlb(root/"shared-single.glb");
    auto duplicatedPrimitives=readPrimitives(duplicatedGlb);auto singlePrimitives=readPrimitives(singleGlb);
    require(duplicatedPrimitives.size()==2,"shared-dag: 两个输入必须各写一份 primitive");
    require(singlePrimitives.size()==1,"shared-single: 单个输入只写一份 primitive");
    require(duplicatedPrimitives[0].positions==singlePrimitives[0].positions,"shared-dag: 第一份位置与单份转换不一致");
    require(duplicatedPrimitives[1].positions==singlePrimitives[0].positions,"shared-dag: 共享数组的第二次转换改写了位置");
    require(duplicatedPrimitives[1].normals==singlePrimitives[0].normals,"shared-dag: 共享法线被重复变换");
    require(duplicatedPrimitives[1].colors==singlePrimitives[0].colors,"shared-dag: 共享颜色被改写");
    require(snapshot(positions)==positionBefore,"shared-dag: 源位置数组被就地修改");
    require(snapshot(sharedNormals)==normalBefore,"shared-dag: 源法线数组被就地修改");
    require(snapshot(sharedColors)==colorBefore,"shared-dag: 源颜色数组被就地修改");
    // 同一共享 geometry 配两个不同矩阵：每份都必须按原始源数据独立变换，不能承接上一份的结果。
    auto second=osg::Matrixd::rotate(-0.9,osg::Vec3d(0,0,1))*osg::Matrixd::scale(2.0,0.5,1.0)*osg::Matrixd::translate(-8.0,2.5,3.0);
    std::vector<GeometryInput> different{{shared,state,matrix},{shared,state,second}};
    writeGlb(different,root/"shared-dag-two-matrices.glb",root/"shared-textures",true,&spatial,false);
    std::vector<GeometryInput> secondOnly{{shared,state,second}};
    writeGlb(secondOnly,root/"shared-second-single.glb",root/"shared-textures",true,&spatial,false);
    auto differentPrimitives=readPrimitives(loadGlb(root/"shared-dag-two-matrices.glb"));
    auto secondBaseline=readPrimitives(loadGlb(root/"shared-second-single.glb"));
    require(differentPrimitives.size()==2 && secondBaseline.size()==1,"shared-dag-two-matrices: primitive 数量");
    require(differentPrimitives[0].positions==singlePrimitives[0].positions,"shared-dag-two-matrices: 第一份被第二份的矩阵污染");
    require(differentPrimitives[1].positions==secondBaseline[0].positions,"shared-dag-two-matrices: 第二份必须等于单份基线位置");
    require(differentPrimitives[1].normals==secondBaseline[0].normals,"shared-dag-two-matrices: 第二份必须等于单份基线法线");
    require(differentPrimitives[1].colors==singlePrimitives[0].colors,"shared-dag-two-matrices: 共享颜色跨矩阵应保持一致");
    require(differentPrimitives[0].normals!=differentPrimitives[1].normals,"shared-dag-two-matrices: 不同矩阵的法线不应相同（用例有效性）");
    audit.push_back({{"case","shared-dag"},{"positions",duplicatedPrimitives[1].positions},{"normals",duplicatedPrimitives[1].normals}});
    audit.push_back({{"case","shared-dag-two-matrices"},{"positions",differentPrimitives[1].positions},{"normals",differentPrimitives[1].normals}});
}

// ---------- 6. 65535 / 65536 索引边界 ----------
void indexBoundaryCases() {
    auto indexed=[](unsigned vertices,const std::vector<unsigned>& ids,unsigned mode=GL_TRIANGLES) {
        auto geometry=new osg::Geometry;geometry->setVertexArray(grid(vertices));
        auto elements=new osg::DrawElementsUInt(mode);for(auto id:ids) elements->push_back(id);
        geometry->addPrimitiveSet(elements);return geometry;
    };
    // 65535 顶点、最大索引 65534：仍是 UNSIGNED_SHORT。
    std::vector<unsigned> shortIds;
    for(unsigned i=0;i<65535;++i) shortIds.push_back(i);
    auto shortResult=convertFixture("index-65534",geode(indexed(65535,shortIds)),false,true);
    require(shortResult.report["trianglesAllLods"]==21845,"index-65534: 65535 索引应为 21845 个三角形");
    require(shortResult.report["compressedViews"].get<unsigned>()>0,"index-65534: 该用例必须真的走 meshopt 压缩路径");
    const auto& shortAccessor=shortResult.glb.doc["accessors"][shortResult.glb.doc["meshes"][0]["primitives"][0]["indices"].get<unsigned>()];
    require(shortAccessor["componentType"]==5123,"index-65534: 最大索引 65534 必须保持 UNSIGNED_SHORT");
    require(shortAccessor["count"]==65535,"index-65534: 索引数量必须是 65535");
    auto shortIndices=accessorIndices(shortResult.glb,shortResult.glb.doc["meshes"][0]["primitives"][0]["indices"].get<unsigned>());
    require(shortIndices.front()==0 && shortIndices[65534]==65534,"index-65534: 索引值不能被截断");

    // 65536 顶点、含索引 65535：必须升为 UNSIGNED_INT。
    auto intIds=shortIds;intIds.push_back(0);intIds.push_back(1);intIds.push_back(65535);
    auto intResult=convertFixture("index-65535",geode(indexed(65536,intIds)),false,true);
    require(intResult.report["trianglesAllLods"]==21846,"index-65535: 追加一个三角形");
    require(intResult.report["compressedViews"].get<unsigned>()>0,"index-65535: 该用例必须真的走 meshopt 压缩路径");
    const auto& intAccessor=intResult.glb.doc["accessors"][intResult.glb.doc["meshes"][0]["primitives"][0]["indices"].get<unsigned>()];
    require(intAccessor["componentType"]==5125,"index-65535: 索引 65535 必须升为 UNSIGNED_INT");
    auto intIndices=accessorIndices(intResult.glb,intResult.glb.doc["meshes"][0]["primitives"][0]["indices"].get<unsigned>());
    require(intIndices.size()==65538,"index-65535: 索引数量");
    // 存储槽位仍与源顶点号一致；每个面只允许循环起点，禁止倒面或换面序。
    for(unsigned face=0;face<intIds.size();face+=3) {
        bool same=false;
        for(unsigned r=0;r<3;++r)
            same|=intIds[face]==intIndices[face+r] && intIds[face+1]==intIndices[face+(1+r)%3] && intIds[face+2]==intIndices[face+(2+r)%3];
        require(same,"index-65535: 有向三角形序列改变");
    }
    std::vector<unsigned> decodedIds(intIndices.begin(),intIndices.end());
    requireCorners(intResult.primitives[0],intIndices,cornersOf(decodedIds),"index-65535");
    // 索引 65535 指向的顶点也必须能取到原始坐标。
    auto lastPosition=accessorFloats(intResult.glb,intResult.glb.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<unsigned>(),65535);
    auto expected=gridPoint(65535);
    for(unsigned c=0;c<3;++c) require(std::abs(lastPosition[c]-expected[c])<1e-6,"index-65535: 65536 号顶点坐标被改写");

    // 顶点数 65536 但最大索引 65534：类型只看索引，未引用顶点仍保留。
    auto unreferenced=convertFixture("index-unreferenced",geode(indexed(65536,shortIds)));
    const auto& unreferencedAccessor=unreferenced.glb.doc["accessors"][unreferenced.glb.doc["meshes"][0]["primitives"][0]["indices"].get<unsigned>()];
    require(unreferencedAccessor["componentType"]==5123,"index-unreferenced: 最大索引 65534 不应被迫升位");
    const auto& unreferencedPosition=unreferenced.glb.doc["accessors"][unreferenced.glb.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<unsigned>()];
    require(unreferencedPosition["count"]==65536,"index-unreferenced: 未引用顶点必须保留");
    auto retained=accessorFloats(unreferenced.glb,unreferenced.glb.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<unsigned>(),65535);
    for(unsigned c=0;c<3;++c) require(std::abs(retained[c]-expected[c])<1e-6,"index-unreferenced: 未引用顶点坐标被改写");
}

// ---------- 7. 必须显式失败的非法图元 ----------
void rejectionCases() {
    struct Case {std::string name;osg::ref_ptr<osg::Geometry> geometry;};
    auto partial=new osg::Geometry;partial->setVertexArray(grid(4));partial->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,4));
    auto quads=new osg::Geometry;quads->setVertexArray(grid(4));quads->addPrimitiveSet(new osg::DrawArrays(GL_QUADS,0,4));
    auto outOfRange=new osg::Geometry;outOfRange->setVertexArray(grid(4));auto bad=new osg::DrawElementsUInt(GL_TRIANGLES);
    for(unsigned index:{0U,1U,99U}) bad->push_back(index);outOfRange->addPrimitiveSet(bad);
    auto negativeFirst=new osg::Geometry;negativeFirst->setVertexArray(grid(4));negativeFirst->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,-1,3));
    auto beyondArray=new osg::Geometry;beyondArray->setVertexArray(grid(4));beyondArray->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLE_STRIP,2,4));
    auto segmentedTriangles=new osg::Geometry;segmentedTriangles->setVertexArray(grid(7));
    auto raggedLengths=new osg::DrawArrayLengths(GL_TRIANGLES,0);raggedLengths->push_back(3);raggedLengths->push_back(4);
    segmentedTriangles->addPrimitiveSet(raggedLengths);
    const Case cases[]={{"reject-partial-triangle",partial},{"reject-quads",quads},{"reject-index-range",outOfRange},
        {"reject-negative-first",negativeFirst},{"reject-beyond-array",beyondArray},
        {"reject-ragged-triangle-run",segmentedTriangles}};
    for(const auto& item:cases) {
        auto file=root/(item.name+".osgb");writeNode(geode(item.geometry),file);
        bool failed=false;
        try {convert(baseOptions(file,root/("out-"+item.name)));} catch(const std::exception&) {failed=true;}
        require(failed && !fs::exists(root/("out-"+item.name)),item.name+": 必须显式失败且不发布输出");
        audit.push_back({{"case",item.name},{"expectFailure",true},{"failed",failed}});
    }
}
}
int main(int argc,char** argv) {
    try {
        root=argc==3 && std::string(argv[1])=="--output"?fs::u8path(argv[2]):
            fs::current_path()/("geometry-compat-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        fanCases();
        drawArraysCases();
        mixedPrimitiveCase();
        colorCases();
        uvBindingCases();
        sharedArrayCases();
        indexBoundaryCases();
        rejectionCases();
        atomicJson(root/"observed.json",audit);
        std::cout<<"通过图元/属性回归（扇面、DrawArrays 子区间、混合 primitive、颜色/UV binding、共享数组、65535 边界、非法图元）；证据: "<<root.u8string()<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
