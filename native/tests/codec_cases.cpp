#include "native_tiles.hpp"
#include "codecs.hpp"
#include "output_validation.hpp"
#include <osg/Geode>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osgDB/WriteFile>
#include <draco/compression/decode.h>
#include <draco/attributes/attribute_quantization_transform.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace nt;
namespace {
void require(bool c,const std::string& s) {if(!c) throw std::runtime_error(s);}
unsigned u32(const Bytes& b,unsigned p) {require(p+4<=b.size(),"u32 range");return unsigned(b[p])|(unsigned(b[p+1])<<8)|(unsigned(b[p+2])<<16)|(unsigned(b[p+3])<<24);}
osg::ref_ptr<osg::Geode> fixture() {
    auto g=new osg::Geometry;auto p=new osg::Vec3Array;auto uv=new osg::Vec2Array;auto n=new osg::Vec3Array;auto c=new osg::Vec4Array;
    for(auto v:{osg::Vec3(0,0,0),osg::Vec3(9.12345,0,0),osg::Vec3(9.12345,7.22345,0),osg::Vec3(0,7.22345,0),osg::Vec3(0,0,0),osg::Vec3(3.14159,2.11111,0)}) {
        p->push_back(v);uv->push_back({v.x()/9.12345f,v.y()/7.22345f});n->push_back({0,0,1});c->push_back({.7,.5,.3,1});
    }
    // Vertex 4 shares the complete quantized key with vertex 0, while its
    // source UV differs. Its error must not be hidden by matching vertex 0.
    (*uv)[4].x()=0.00012f;
    auto i=new osg::DrawElementsUInt(GL_TRIANGLES);for(unsigned v:{0,1,2,0,2,3,4,4,1}) i->push_back(v);
    g->setVertexArray(p);g->setTexCoordArray(0,uv);g->setNormalArray(n,osg::Array::BIND_PER_VERTEX);g->setColorArray(c,osg::Array::BIND_PER_VERTEX);g->addPrimitiveSet(i);
    auto image=new osg::Image;image->allocateImage(6,10,1,GL_RGBA,GL_UNSIGNED_BYTE,1);image->setFileName("rgba.png");
    for(unsigned y=0;y<10;++y) for(unsigned x=0;x<6;++x) {auto p=image->data(x,y);p[0]=x*40;p[1]=y*25;p[2]=80;p[3]=64+x*30;}
    auto tex=new osg::Texture2D(image);tex->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);tex->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
    tex->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);tex->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
    auto geode=new osg::Geode;geode->addDrawable(g);geode->getOrCreateStateSet()->setTextureAttributeAndModes(0,tex,osg::StateAttribute::ON);geode->getOrCreateStateSet()->setMode(GL_BLEND,osg::StateAttribute::ON);return geode;
}
void verifyDraco(const fs::path& path) {
    auto b=readBytes(path);auto j=Json::parse(b.begin()+20,b.begin()+20+u32(b,12));auto bin=28+u32(b,12);
    require(j["extensionsRequired"].end()!=std::find(j["extensionsRequired"].begin(),j["extensionsRequired"].end(),"KHR_draco_mesh_compression"),"required Draco extension");
    for(const auto& mesh:j["meshes"]) for(const auto& primitive:mesh["primitives"]) {
        auto& extension=primitive["extensions"]["KHR_draco_mesh_compression"];auto& view=j["bufferViews"][extension["bufferView"].get<unsigned>()];
        draco::Decoder decoder;decoder.SetSkipAttributeTransform(draco::GeometryAttribute::POSITION);decoder.SetSkipAttributeTransform(draco::GeometryAttribute::TEX_COORD);
        draco::DecoderBuffer buffer;buffer.Init(reinterpret_cast<const char*>(b.data()+bin+view["byteOffset"].get<unsigned>()),view["byteLength"].get<unsigned>());
        auto decoded=decoder.DecodeMeshFromBuffer(&buffer);require(decoded.ok(),"independent decoder");auto& geometry=*decoded.value();
        require(geometry.num_points()==6 && geometry.num_faces()==3,"retain duplicate, unreferenced and degenerate content");
        for(auto name:{"POSITION","TEXCOORD_0"}) {
            auto a=geometry.GetAttributeByUniqueId(extension["attributes"][name].get<unsigned>());draco::AttributeQuantizationTransform transform;
            require(a && transform.InitFromAttribute(*a),"quantization metadata");require(transform.quantization_bits()==(std::string(name)=="POSITION"?14:12),"target quantization bits");
        }
        // Sequential must preserve the explicit repeated-index degenerate face.
        auto f=geometry.face(draco::FaceIndex(2));require(f[0]==f[1] && f[2]!=f[0],"degenerate triangle identity");
    }
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3 && std::string(argv[1])=="--reader-fixture") {
            auto group=new osg::Group;
            for(unsigned i=0;i<2;++i) {
                auto geode=fixture();auto second=new osg::DrawElementsUInt(GL_TRIANGLES);for(unsigned v:{0,1,3}) second->push_back(v);
                geode->getDrawable(0)->asGeometry()->addPrimitiveSet(second);group->addChild(geode);
            }
            auto file=fs::absolute(argv[2]);fs::create_directories(file.parent_path());
            osg::ref_ptr<osgDB::Options> write=new osgDB::Options("WriteImageHint=IncludeFile Compressor=zlib");
            require(osgDB::writeNodeFile(*group,file.u8string(),write),"multi-primitive reader fixture");return 0;
        }
        auto root=argc==3&&std::string(argv[1])=="--root"?fs::absolute(argv[2]):fs::current_path()/("codec-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root/"input");osg::ref_ptr<osgDB::Options> write=new osgDB::Options("WriteImageHint=IncludeFile Compressor=zlib");
        require(osgDB::writeNodeFile(*fixture(),(root/"input/model.osgb").u8string(),write),"real OSGB fixture");
        Options o;o.input=root/"input";o.srs="ENU:0,0";o.threads=1;o.compressGeometry=false;Json results=Json::array();
        for(unsigned speed:{0,5,10}) {
            o.output=root/("draco-"+std::to_string(speed));o.dracoGeometry=true;o.dracoSpeed=speed;auto report=convert(o);
            require(report["geometryEncoding"]=="KHR_draco_mesh_compression" && report["lossyGeometry"]==true,"Draco report");
            for(const auto& row:report["records"]) for(const auto& content:row["content"]) {
                verifyDraco(o.output/content["uri"].get<std::string>());require(content["draco"]["primitives"][0]["method"]=="sequential","identity-safe method");
                require(content["quantizationErrorMeters"].get<double>()>0 && content["quantizationErrorMeters"].get<double>()<.001,"actual world quantization error");
                require(content["draco"]["primitives"][0]["maxUvError"].get<double>()>=0.000119,
                        "UV error must include every source point in a colliding grid key");
            }
            results.push_back({{"dracoSpeed",speed},{"report",report}});
        }
        // Identical primitives (instanced geometry) must share one Draco stream:
        // fewer stored payloads, unchanged decoded coverage.
        {
            auto group=new osg::Group;for(unsigned i=0;i<2;++i) group->addChild(fixture());
            fs::create_directories(root/"input-reuse");
            require(osgDB::writeNodeFile(*group,(root/"input-reuse/model.osgb").u8string(),write),"instanced fixture");
            Options reuseOptions=o;reuseOptions.input=root/"input-reuse";reuseOptions.output=root/"draco-reuse";
            reuseOptions.dracoGeometry=true;reuseOptions.dracoSpeed=5;
            auto report=convert(reuseOptions);
            std::size_t primitives=0,reused=0;std::set<unsigned> streams;
            for(const auto& row:report["records"]) for(const auto& content:row["content"]) {
                for(const auto& prim:content["draco"]["primitives"]) {++primitives;if(prim.value("streamReused",false)) ++reused;}
                auto bytes=readBytes(reuseOptions.output/content["uri"].get<std::string>());
                auto doc=Json::parse(bytes.begin()+20,bytes.begin()+20+u32(bytes,12));
                for(const auto& mesh:doc["meshes"]) for(const auto& prim:mesh["primitives"])
                    streams.insert(prim["extensions"]["KHR_draco_mesh_compression"]["bufferView"].get<unsigned>());
            }
            require(primitives>=2 && reused>=1,"identical primitives must reuse a Draco stream");
            require(streams.size()<primitives,"shared streams reduce stored Draco payloads");
        }
        {
            Options embedOptions=o;embedOptions.dracoGeometry=true;embedOptions.compressGeometry=false;embedOptions.embedTextures=true;embedOptions.textureFormat="original";
            embedOptions.output=root/"draco-embed-textures";
            auto report=convert(embedOptions);
            require(report["status"]=="complete" && report["embedTextures"]==true,"draco + embedTextures report");
            require(!fs::exists(embedOptions.output/"textures") || fs::is_empty(embedOptions.output/"textures"),"embedTextures must not emit external texture files");
            bool sawEmbeddedImage=false;
            for(const auto& row:report["records"]) for(const auto& content:row["content"]) {
                auto bytes=readBytes(embedOptions.output/content["uri"].get<std::string>());
                auto doc=Json::parse(bytes.begin()+20,bytes.begin()+20+u32(bytes,12));
                if(doc.contains("images")) for(const auto& img:doc["images"]) {
                    require(img.contains("bufferView") && !img.contains("uri"),"embedded image must use bufferView without uri");
                    sawEmbeddedImage=true;
                }
            }
            require(sawEmbeddedImage,"expected at least one embedded image inside Draco GLB");
        }
        {
            // Full compat relay profile: Draco + embedTextures + rebuildTop + preview on opaque multi-block roots.
            fs::create_directories(root/"input-relay");
            for(unsigned b=0;b<2;++b) {
                auto g=new osg::Geometry;auto p=new osg::Vec3Array;auto uv=new osg::Vec2Array;
                for(unsigned y=0;y<=4;++y) for(unsigned x=0;x<=4;++x) {
                    p->push_back({float(x)*2.0f,float(y)*2.0f,0.0f});uv->push_back({float(x)/4.0f,float(y)/4.0f});
                }
                auto idx=new osg::DrawElementsUInt(GL_TRIANGLES);
                for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) {
                    unsigned a=y*5+x,c=a+1,d=a+5,e=d+1;for(auto v:{a,c,e,a,e,d}) idx->push_back(v);
                }
                g->setVertexArray(p);g->setTexCoordArray(0,uv);g->addPrimitiveSet(idx);
                auto image=new osg::Image;image->allocateImage(8,8,1,GL_RGBA,GL_UNSIGNED_BYTE,1);image->setFileName("opaque.png");
                for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {auto px=image->data(x,y);px[0]=static_cast<unsigned char>(20+x*25);px[1]=static_cast<unsigned char>(30+y*25);px[2]=140;px[3]=255;}
                auto tex=new osg::Texture2D(image);tex->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);tex->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
                tex->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);tex->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
                auto geode=new osg::Geode;geode->addDrawable(g);geode->getOrCreateStateSet()->setTextureAttributeAndModes(0,tex,osg::StateAttribute::ON);
                auto mt=new osg::MatrixTransform;mt->setMatrix(osg::Matrixd::translate(double(b)*12.0,0.0,0.0));mt->addChild(geode);
                require(osgDB::writeNodeFile(*mt,(root/"input-relay"/("block"+std::to_string(b)+".osgb")).u8string(),write),"relay block fixture");
            }
            Options relay=o;relay.input=root/"input-relay";relay.output=root/"relay-full";
            relay.dracoGeometry=true;relay.compressGeometry=false;relay.embedTextures=true;relay.rebuildTop=true;relay.preview=true;relay.textureFormat="original";
            auto report=convert(relay);
            require(report["status"]=="complete" && report["embedTextures"]==true && report["rebuildTop"]==true && report["topGlbs"].get<unsigned>()>=1,"relay full report");
            require(report.contains("preview") && report["preview"]["status"]=="complete","relay preview report");
            require(!fs::exists(relay.output/"textures") || fs::is_empty(relay.output/"textures"),"relay must not emit external textures");
            auto mainTileset=Json::parse(readBytes(relay.output/"tileset.json"));
            auto previewTileset=Json::parse(readBytes(relay.output/"preview/tileset.json"));
            require(mainTileset["root"]["geometricError"].get<double>()>=2000.0,"main root geometricError must guarantee first-frame traversal");
            require(previewTileset["root"]["geometricError"].get<double>()>=2000.0,"preview root geometricError must guarantee first-frame traversal");
            auto topGlbBytes=readBytes(relay.output/report["topAncestors"][0]["content"].get<std::string>());
            auto topDoc=Json::parse(topGlbBytes.begin()+20,topGlbBytes.begin()+20+u32(topGlbBytes,12));
            require(topDoc.contains("images") && !topDoc["images"].empty(),"top GLB must contain embedded atlas");
            require(topDoc["images"][0].contains("bufferView") && !topDoc["images"][0].contains("uri"),"top GLB atlas must be embedded");
            require(topDoc["images"][0]["mimeType"]=="image/jpeg" || topDoc["images"][0]["mimeType"]=="image/png","top GLB atlas mimeType");
            if(report["topAncestors"][0]["bake"]["textureEncoding"]=="JPEG RGB") {
                require(topDoc["images"][0]["mimeType"]=="image/jpeg","opaque embedded top atlas should encode as JPEG");
            }
        }
        o.dracoGeometry=false;
        for(auto format:{"uastc","etc1s"}) {
            o.textureFormat=format;o.output=root/format;auto report=convert(o);require(report["preserveEncodedTextures"]==false && report["lossyTextures"]==true,"Basis loss report");
            for(const auto& file:fs::directory_iterator(o.output/"textures")) {
                auto b=readBytes(file.path());const unsigned char magic[]={0xab,0x4b,0x54,0x58,0x20,0x32,0x30,0xbb,0x0d,0x0a,0x1a,0x0a};
                require(b.size()>104 && std::memcmp(b.data(),magic,12)==0,"KTX2 magic");
                require(u32(b,12)==0 && u32(b,20)==8 && u32(b,24)==12 && u32(b,40)==4,"Basis KTX2 dimensions/mip count");
                require(u32(b,44)==(std::string(format)=="uastc"?2U:1U),"Zstd or BasisLZ scheme");
                unsigned dfd=u32(b,48);require(b.at(dfd+14)==2,"DFD sRGB transfer");
                auto decoded=inspectBasis(b);require(decoded["format"]==format && decoded["hasAlpha"]==true,"real compressed blocks and alpha");
                require(decoded["decodedAlphaMin"].get<unsigned>()<100 && decoded["decodedAlphaMax"].get<unsigned>()>150,"alpha values survived transcoding");
                results.push_back({{"format",format},{"inspection",decoded}});
            }
        }
        // Publication validation must reject corrupt compressed payloads and
        // extension declarations instead of trusting a completed encoder call.
        fs::copy(root/"uastc",root/"corrupt-basis",fs::copy_options::recursive);
        auto texture=fs::directory_iterator(root/"corrupt-basis/textures")->path();auto corrupt=readBytes(texture);corrupt[0]^=1;atomicWrite(texture,corrupt);
        bool refused=false;try {validateStagedOutput(root/"corrupt-basis");} catch(const std::exception&) {refused=true;}require(refused,"corrupt KTX2 must fail publication");
        fs::copy(root/"uastc",root/"missing-basis-extension",fs::copy_options::recursive);
        auto glb=fs::directory_iterator(root/"missing-basis-extension/content")->path();auto glbBytes=readBytes(glb);auto doc=Json::parse(glbBytes.begin()+20,glbBytes.begin()+20+u32(glbBytes,12));
        auto& required=doc["extensionsRequired"];required.erase(std::remove(required.begin(),required.end(),"KHR_texture_basisu"),required.end());
        auto text=doc.dump();while(text.size()%4) text+=' ';Bytes rewritten;auto put=[&](unsigned v) {for(unsigned k=0;k<4;++k) rewritten.push_back((v>>(k*8))&255);};
        unsigned binStart=20+u32(glbBytes,12);put(0x46546c67);put(2);put(20+text.size()+glbBytes.size()-binStart);put(text.size());put(0x4e4f534a);
        rewritten.insert(rewritten.end(),text.begin(),text.end());rewritten.insert(rewritten.end(),glbBytes.begin()+binStart,glbBytes.end());atomicWrite(glb,rewritten);
        refused=false;try {validateStagedOutput(root/"missing-basis-extension");} catch(const std::exception&) {refused=true;}require(refused,"missing required Basis extension must fail");
        // All three encoding choices are part of the checkpoint identity.
        o.textureFormat="uastc";o.dracoGeometry=true;o.dracoSpeed=5;o.output=root/"resumed";o.checkpointDir=root/"checkpoint";o.pauseFile=root/"pause";
        o.event=[&](const Json& e){if(e.value("stage","")=="convert" && e.at("done").get<unsigned>()>=1) atomicWrite(o.pauseFile,{});};
        auto paused=convert(o);require(paused["status"]=="paused","codec pause");fs::remove(o.pauseFile);o.event={};o.resume=true;
        auto reject=[&] {bool fail=false;try {convert(o);}catch(const std::exception& e){fail=std::string(e.what()).find("身份不匹配")!=std::string::npos;}require(fail,"codec identity must reject changed parameters");};
        o.dracoSpeed=0;reject();o.dracoSpeed=5;o.textureFormat="etc1s";reject();o.textureFormat="uastc";o.dracoGeometry=false;reject();o.dracoGeometry=true;
        auto resumed=convert(o);require(resumed["status"]=="complete" && resumed["filesReused"]==1,"codec true resume");
        atomicJson(root/"test-result.json",{{"passed",true},{"cases",results}});std::cout<<"Native codec OSGB checks passed: "<<root<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
