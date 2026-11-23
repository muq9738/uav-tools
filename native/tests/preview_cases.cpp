#include "native_tiles.hpp"
#include "output_validation.hpp"
#include "top.hpp"
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/Texture2D>
#include <osgDB/WriteFile>
#include <algorithm>
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
osg::ref_ptr<osg::Geode> grid(unsigned divisions=8,bool repeat=false,float uvScale=1.f) {
    auto geometry=new osg::Geometry;auto positions=new osg::Vec3Array;auto uv=new osg::Vec2Array;auto normals=new osg::Vec3Array;
    for(unsigned y=0;y<=divisions;++y) for(unsigned x=0;x<=divisions;++x) {
        positions->push_back({float(x)*8/divisions,float(y)*8/divisions,0});
        uv->push_back({float(x)*uvScale/divisions,float(y)*uvScale/divisions});
        normals->push_back({0,0,1});
    }
    auto indices=new osg::DrawElementsUInt(GL_TRIANGLES);
    for(unsigned y=0;y<divisions;++y) for(unsigned x=0;x<divisions;++x) {
        unsigned a=y*(divisions+1)+x,b=a+1,c=a+divisions+1,d=c+1;for(auto v:{a,b,d,a,d,c}) indices->push_back(v);
    }
    geometry->setVertexArray(positions);geometry->setTexCoordArray(0,uv);
    geometry->setNormalArray(normals,osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(indices);
    auto image=new osg::Image;image->allocateImage(8,8,1,GL_RGBA,GL_UNSIGNED_BYTE,1);image->setFileName(repeat?"repeat.png":"atlas.png");
    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {auto p=image->data(x,y);p[0]=x*31;p[1]=y*31;p[2]=90;p[3]=80+x*20;}
    auto texture=new osg::Texture2D(image);texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
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
    writeNode(grid(12,true,2.5f).get(),directory/"leaf.osgb");
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
bool hasExt(const Json& doc,const char* name) {
    if(!doc.contains("extensionsRequired")) return false;
    const auto& req=doc["extensionsRequired"];
    return std::find(req.begin(),req.end(),name)!=req.end();
}
}
int main(int argc,char** argv) {
    try {
        if(argc>=3 && std::string(argv[1])=="--fixture") {
            auto directory=fs::absolute(fs::u8path(argv[2]));unsigned roots=argc>=4?static_cast<unsigned>(std::stoul(argv[3])):4;
            unsigned divisions=argc>=5?static_cast<unsigned>(std::stoul(argv[4])):64;
            for(unsigned i=0;i<roots;++i) {
                auto transform=new osg::MatrixTransform;
                transform->setMatrix(osg::Matrixd::translate(double(i%4)*8.5,double(i/4)*8.5,0));
                auto node=new osg::Group;node->addChild(grid(divisions).get());transform->addChild(node);
                writeNode(transform,directory/("block"+std::to_string(i)+".osgb"));
            }
            std::cout<<"Preview fixture: "<<directory<<" roots="<<roots<<" divisions="<<divisions<<" triangles="<<roots*divisions*divisions*2<<'\n';
            return 0;
        }
        auto root=fs::temp_directory_path()/("preview-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fixture(root/"input");
        auto baseOptions=options(root/"input",root/"baseline");auto baseline=convert(baseOptions);auto expected=mainAssets(root/"baseline");
        auto enabled=baseOptions;enabled.output=root/"preview-output";enabled.preview=true;auto report=convert(enabled);
        require(report["sourceExact"]==true && report["sourceContentsExact"]==true,"预览不得改变主产物 sourceExact");
        require(report["preview"]["status"]=="complete" && report["preview"]["algorithm"]=="mesh-preview-v2" && report["preview"]["streamingLod"]==true,"预览摘要");
        require(report["preview"]["geometryEncoding"]=="EXT_meshopt_compression+KHR_mesh_quantization" && report["preview"]["textureFormat"]=="original","预览编码摘要");
        require(mainAssets(enabled.output)==expected,"开关预览前后主 tileset/content/textures/tiles 必须逐字节一致");
        const auto mainManifest=Json::parse(readBytes(enabled.output/"tileset.json"));
        const auto generator=mainManifest.at("asset").at("generator").get<std::string>();
        const auto previewManifest=Json::parse(readBytes(enabled.output/"preview/tileset.json"));
        require(previewManifest.at("asset")==mainManifest.at("asset"),"Preview tileset must use the same asset metadata as the main tileset");
        auto previewTris=report["preview"]["triangles"].get<std::uint64_t>();
        require(previewTris>0 && previewTris<416,"预览叶层三角面应通过锁边 QEM 简化显著低于源面数 416");
        require(report["preview"]["glbs"].get<std::uint64_t>()==3,"2 层树保留根概览与叶层，共 3 个流式瓦片");
        auto detail=Json::parse(readBytes(enabled.output/"preview/preview-report.json"));
        require(detail["streamingLod"]==true && detail["textureFormat"]=="original" && detail["normals"]=="omitted","流式 LOD / 原图 / unlit 报告");
        require(detail["texturesEmbedded"]==true && detail["borderLocked"]==true,"预览纹理内嵌且启用锁边简化");
        require(detail["sourceLeafTriangles"].get<std::uint64_t>()==416,"源叶层三角面应记录为 416");
        require(detail["simplificationRatio"].get<double>()<0.75,"预览简化率应低于 75%");
        require(!fs::exists(enabled.output/"preview/textures"),"内嵌纹理预览不应写出外部 preview/textures 目录");
        bool sawQuantizedUv=false,sawFloatUv=false;
        for(unsigned i=0;i<3;++i) {
            auto doc=gltf(enabled.output/"preview/content"/("tile-"+std::to_string(i)+".glb"));
            require(doc.at("asset")==Json{{"version","2.0"},{"generator",generator}},"Preview GLBs must retain the glTF version and share the asset generator");
            require(hasExt(doc,"EXT_meshopt_compression") && hasExt(doc,"KHR_mesh_quantization") && hasExt(doc,"KHR_materials_unlit"),"预览扩展声明");
            for(const auto& mesh:doc["meshes"]) for(const auto& primitive:mesh["primitives"]) {
                require(!primitive["attributes"].contains("NORMAL"),"unlit 预览省略法线");
                auto& pos=doc["accessors"][primitive["attributes"]["POSITION"].get<unsigned>()];
                require(pos["componentType"]==5123 && pos.value("normalized",false),"位置必须归一化 USHORT 量化");
                auto& uv=doc["accessors"][primitive["attributes"]["TEXCOORD_0"].get<unsigned>()];
                if(uv["componentType"]==5123 && uv.value("normalized",false)) sawQuantizedUv=true;
                if(uv["componentType"]==5126) sawFloatUv=true;
                require(doc["materials"][primitive["material"].get<unsigned>()]["extensions"].contains("KHR_materials_unlit"),"unlit 材质");
            }
            for(const auto& sampler:doc["samplers"]) {
                require(sampler["minFilter"]==GL_LINEAR_MIPMAP_LINEAR && sampler["magFilter"]==GL_LINEAR,"预览采样器启用三线性 mipmap");
            }
            for(const auto& image:doc["images"]) {
                require(image["mimeType"]=="image/png" || image["mimeType"]=="image/jpeg","预览纹理原样透传 PNG/JPEG");
                require(image.contains("bufferView") && !image.contains("uri"),"预览纹理必须内嵌在 GLB bufferView 中");
            }
        }
        require(sawQuantizedUv && sawFloatUv,"[0,1] UV 应 U16 量化，越界重复 UV 应安全回退 FLOAT");
        // 四级金字塔抽层验证：保留顶层(d0)、偶数层(d2)与叶层(d3)，跳过冗余奇数中间层(d1)。
        auto pyramid=root/"pyramid";
        {
            writeNode(grid(12).get(),pyramid/"input/d3.osgb");
            auto d2=new osg::Group;d2->addChild(grid(8).get());auto p2=new osg::PagedLOD;p2->setFileName(0,"d3.osgb");p2->setRange(0,0,1e9);d2->addChild(p2);
            writeNode(d2,pyramid/"input/d2.osgb");
            auto d1=new osg::Group;d1->addChild(grid(6).get());auto p1=new osg::PagedLOD;p1->setFileName(0,"d2.osgb");p1->setRange(0,0,1e9);d1->addChild(p1);
            writeNode(d1,pyramid/"input/d1.osgb");
            auto d0=new osg::Group;d0->addChild(grid(4).get());auto p0=new osg::PagedLOD;p0->setFileName(0,"d1.osgb");p0->setRange(0,0,1e9);d0->addChild(p0);
            writeNode(d0,pyramid/"input/d0.osgb");
        }
        auto p1Opt=options(pyramid/"input",pyramid/"out-t1");p1Opt.preview=true;p1Opt.threads=1;
        auto p4Opt=options(pyramid/"input",pyramid/"out-t4");p4Opt.preview=true;p4Opt.threads=4;
        convert(p1Opt);convert(p4Opt);
        auto pyrDetail=Json::parse(readBytes(p1Opt.output/"preview/preview-report.json"));
        require(pyrDetail["sourceTiles"]==4 && pyrDetail["prunedTiles"]==1 && pyrDetail["glbs"]==3,"四级金字塔应跳过奇数中间层 d1，保留 d0/d2/d3");
        require(previewAssets(p1Opt.output)==previewAssets(p4Opt.output),"多线程预览必须与单线程逐字节一致");
        // 暂停与续转检查点一致性。
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
        // 破坏预览 GLB 内嵌图片魔数必须被发布前校验拒绝。
        auto firstGlb=enabled.output/"preview/content/tile-0.glb";
        auto glbBytes=readBytes(firstGlb);
        auto firstDoc=gltf(firstGlb);
        std::uint32_t jlen=0;std::memcpy(&jlen,glbBytes.data()+12,4);
        std::size_t imgOffset=28+jlen+firstDoc["bufferViews"][firstDoc["images"][0]["bufferView"].get<std::size_t>()].value("byteOffset",std::size_t(0));
        glbBytes[imgOffset]^=0xff;
        atomicWrite(firstGlb,glbBytes);
        bool validationFailed=false;try {validateStagedOutput(enabled.output);} catch(...) {validationFailed=true;}
        require(validationFailed,"预览内嵌纹理损坏必须被发布前校验拒绝");
        atomicJson(root/"test-result.json",{{"passed",true},{"previewTriangles",report["preview"]["triangles"]}});
        std::cout<<"Preview native checks passed: "<<root<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
