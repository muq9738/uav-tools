#include "native_tiles.hpp"
#include "output_validation.hpp"
#include "codecs.hpp"
#include "top.hpp"
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/Texture2D>
#include <osgDB/WriteFile>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
using namespace nt;
namespace {
void require(bool value,const std::string& message) {if(!value) throw std::runtime_error(message);}
void writeNode(osg::Node* node,const fs::path& file) {
    fs::create_directories(file.parent_path());osg::ref_ptr<osgDB::Options> options=new osgDB::Options("WriteImageHint=IncludeFile Compressor=zlib");
    require(osgDB::writeNodeFile(*node,file.u8string(),options.get()),"写入预览夹具失败");
}
osg::ref_ptr<osg::Geode> grid(unsigned divisions=8,bool repeat=false) {
    auto geometry=new osg::Geometry;auto positions=new osg::Vec3Array;auto uv=new osg::Vec2Array;
    for(unsigned y=0;y<=divisions;++y) for(unsigned x=0;x<=divisions;++x) {
        positions->push_back({float(x)*8/divisions,float(y)*8/divisions,0});uv->push_back({float(x)/divisions,float(y)/divisions});
    }
    auto indices=new osg::DrawElementsUInt(GL_TRIANGLES);
    for(unsigned y=0;y<divisions;++y) for(unsigned x=0;x<divisions;++x) {
        unsigned a=y*(divisions+1)+x,b=a+1,c=a+divisions+1,d=c+1;for(auto v:{a,b,d,a,d,c}) indices->push_back(v);
    }
    geometry->setVertexArray(positions);geometry->setTexCoordArray(0,uv);geometry->addPrimitiveSet(indices);
    auto image=new osg::Image;image->allocateImage(8,8,1,GL_RGBA,GL_UNSIGNED_BYTE,1);image->setFileName(repeat?"repeat.png":"atlas.png");
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {auto p=image->data(x,y);p[0]=x*31;p[1]=y*31;p[2]=90;p[3]=80+x*20;}
    auto texture=new osg::Texture2D(image);texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
    texture->setWrap(osg::Texture::WRAP_S,repeat?osg::Texture::REPEAT:osg::Texture::CLAMP_TO_EDGE);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
    geometry->getOrCreateStateSet()->setTextureAttributeAndModes(0,texture,osg::StateAttribute::ON);geometry->getOrCreateStateSet()->setMode(GL_BLEND,osg::StateAttribute::ON);
    auto geode=new osg::Geode;geode->addDrawable(geometry);return geode;
}
void fixture(const fs::path& directory) {
    for(unsigned i=0;i<2;++i) {
        auto transform=new osg::MatrixTransform;transform->setMatrix(osg::Matrixd::scale(1.5,2,1)*osg::Matrixd::translate(double(i)*20,3,2));
        auto root=new osg::Group;root->addChild(grid().get());
        if(i==0) {auto reference=new osg::PagedLOD;reference->setFileName(0,"leaf.osgb");reference->setRange(0,0,1e9);root->addChild(reference);}
        transform->addChild(root);writeNode(transform,directory/("root"+std::to_string(i)+".osgb"));
    }
    writeNode(grid(12,true).get(),directory/"leaf.osgb");
}
Options options(const fs::path& input,const fs::path& output) {Options o;o.input=input;o.output=output;o.srs="ENU:0,0";o.threads=1;return o;}
std::map<std::string,std::string> mainAssets(const fs::path& root) {
    std::map<std::string,std::string> result;
    for(const auto& item:fs::recursive_directory_iterator(root)) if(item.is_regular_file()) {
        auto relative=item.path().lexically_relative(root);auto name=relative.generic_u8string();auto first=relative.begin()->string();
        if(name=="tileset.json" || first=="tiles" || first=="content" || first=="textures") result[name]=contentKey(readBytes(item.path()));
    }
    return result;
}
std::map<std::string,std::string> previewAssets(const fs::path& root) {
    std::map<std::string,std::string> result;for(const auto& item:fs::recursive_directory_iterator(root/"preview")) if(item.is_regular_file() && item.path().filename()!="preview-report.json")
        result[item.path().lexically_relative(root).generic_u8string()]=contentKey(readBytes(item.path()));return result;
}
Json gltf(const fs::path& file) {auto bytes=readBytes(file);std::uint32_t length=0;std::memcpy(&length,bytes.data()+12,4);return Json::parse(bytes.begin()+20,bytes.begin()+20+length);}
}
int main() {
    try {
        auto root=fs::temp_directory_path()/("preview-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fixture(root/"input");
        auto baseOptions=options(root/"input",root/"baseline");auto baseline=convert(baseOptions);auto expected=mainAssets(root/"baseline");
        auto enabled=baseOptions;enabled.output=root/"preview-output";enabled.preview=true;auto report=convert(enabled);
        require(report["sourceExact"]==true && report["sourceContentsExact"]==true,"预览不得改变主产物 sourceExact");
        require(report["preview"]["status"]=="complete" && report["preview"]["algorithm"]=="mesh-preview-v1" && report["preview"]["glbs"]==1,"预览摘要");
        require(mainAssets(enabled.output)==expected,"开关预览前后主 tileset/content/textures/tiles 必须逐字节一致");
        require(report["preview"]["triangles"].get<std::uint64_t>()==256,"预览只取显示前沿，不能合并分页 leaf LOD");
        auto detail=Json::parse(readBytes(enabled.output/"preview/preview-report.json"));
        require(detail["atlasPages"]==1 && detail["textureFormat"]=="uastc" && detail["normals"]=="omitted","图集/UASTC/unlit 报告");
        auto previewGlb=enabled.output/"preview/content/preview-0.glb";auto doc=gltf(previewGlb);
        require(doc.contains("extensionsRequired"),"预览扩展声明");
        for(const auto& mesh:doc["meshes"]) for(const auto& primitive:mesh["primitives"]) {
            require(!primitive["attributes"].contains("NORMAL"),"unlit 预览省略法线");
            require(doc["accessors"][primitive["attributes"]["POSITION"].get<unsigned>()]["componentType"]==5126,"位置 FLOAT");
            require(doc["accessors"][primitive["attributes"]["TEXCOORD_0"].get<unsigned>()]["componentType"]==5126,"UV FLOAT");
            require(doc["materials"][primitive["material"].get<unsigned>()]["extensions"].contains("KHR_materials_unlit"),"unlit 材质");
        }
        for(const auto& image:doc["images"]) {
            require(image["mimeType"]=="image/ktx2","预览图片必须为 KTX2");auto file=enabled.output/"preview/content"/image["uri"].get<std::string>();
            auto audit=inspectBasis(readBytes(file),true);require(audit["format"]=="uastc" && audit["mipLevels"].get<unsigned>()>1 && audit["hasAlpha"]==true && audit["sRGB"]==true,"UASTC mip/alpha/sRGB");
            require(audit["width"]<=4096 && audit["height"]<=4096,"图集页上限");
        }
        auto simpleGeometry=grid(16);std::vector<GeometryInput> simpleInputs={{simpleGeometry->getDrawable(0)->asGeometry(),simpleGeometry->getDrawable(0)->asGeometry()->getOrCreateStateSet(),osg::Matrixd::identity()}};
        auto simplified=simplifyPreview(simpleInputs,32,enabled,nullptr);require(simplified.faces.size()<=32 && simplified.inputTriangles==512,"预览 QEM 三角形预算");
        for(const auto& face:simplified.faces) for(const auto& uv:face.uv) require(std::isfinite(uv.x()) && std::isfinite(uv.y()),"QEM 后 UV 必须有限");
        auto resumedOptions=enabled;resumedOptions.output=root/"resumed";resumedOptions.checkpointDir=root/"checkpoint";resumedOptions.pauseFile=root/"pause";
        resumedOptions.event=[&](const Json& event) {if(event.value("stage","")=="preview" && event.value("done",0)>=1) atomicWrite(resumedOptions.pauseFile,{});};
        auto paused=convert(resumedOptions);require(paused["status"]=="paused","预览阶段合作暂停");
        require(!fs::exists(resumedOptions.output) && !fs::exists(resumedOptions.checkpointDir/"work/preview") && !fs::exists(resumedOptions.checkpointDir/"work/.preview-building"),"暂停清理预览临时资产");
        fs::remove(resumedOptions.pauseFile);resumedOptions.event={};resumedOptions.resume=true;resumedOptions.preview=false;bool rejected=false;
        try {convert(resumedOptions);} catch(const std::exception& error) {rejected=std::string(error.what()).find("身份不匹配")!=std::string::npos;}
        require(rejected,"预览配置必须进入 checkpoint identity");resumedOptions.preview=true;auto resumed=convert(resumedOptions);
        require(resumed["filesReused"]==baseline["records"].size(),"续转复用主产物文件记录");require(mainAssets(resumedOptions.output)==expected,"续转主产物一致");
        require(previewAssets(resumedOptions.output)==previewAssets(enabled.output),"续转预览逐字节一致，不能混入旧配置");
        require(!fs::exists(resumedOptions.checkpointDir),"发布后检查点清理");
        fs::remove(enabled.output/"preview/textures"/fs::path(doc["images"][0]["uri"].get<std::string>()).filename());
        bool validationFailed=false;try {validateStagedOutput(enabled.output);} catch(...) {validationFailed=true;}
        require(validationFailed,"预览 KTX2 缺失必须被发布前校验拒绝");
        atomicJson(root/"test-result.json",{{"passed",true},{"previewTriangles",report["preview"]["triangles"]},{"qemTriangles",simplified.faces.size()}});
        std::cout<<"Preview native checks passed: "<<root<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
