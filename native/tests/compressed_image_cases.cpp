#include "compressed_image.hpp"
#include "native_tiles.hpp"
#include <osg/Geode>
#include <osg/Texture2D>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace nt;
using namespace nt::experimental;
namespace {
unsigned checks=0;
void require(bool ok,const std::string& message) {
    if(!ok) throw std::runtime_error(message);
    ++checks;
}
template<class F> void rejects(F fn,const std::string& diagnostic) {
    try {fn();} catch(const std::runtime_error& error) {
        require(std::string(error.what()).find(diagnostic)!=std::string::npos,
                "wrong diagnostic: "+std::string(error.what())+"; expected "+diagnostic);return;
    }
    throw std::runtime_error("expected rejection: "+diagnostic);
}
std::uint64_t unsignedNumber(const Json& value,std::uint64_t maximum) {
    if(!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>()<0) || value.get<std::uint64_t>()>maximum)
        throw std::runtime_error("request integer out of range");
    return value.get<std::uint64_t>();
}
Bytes byteArray(const Json& values) {
    if(!values.is_array()) throw std::runtime_error("request bytes must be an array");
    Bytes result;result.reserve(values.size());
    for(const auto& value:values) result.push_back(unsignedNumber(value,255));
    return result;
}
BcImage decodeRequest(const Json& request) {
    const auto data=byteArray(request.at("data"));std::vector<std::size_t> offsets;
    if(request.contains("mipOffsets")) {
        if(!request["mipOffsets"].is_array()) throw std::runtime_error("mipOffsets must be an array");
        for(const auto& offset:request["mipOffsets"]) offsets.push_back(unsignedNumber(offset,std::numeric_limits<std::size_t>::max()));
    }
    const auto origin=request.value("origin",std::string("bottom-left"));
    if(origin!="bottom-left" && origin!="top-left") throw std::runtime_error("request origin is invalid");
    constexpr auto max=std::numeric_limits<std::uint32_t>::max();
    return decodeBcCandidate(unsignedNumber(request.at("glFormat"),max),unsignedNumber(request.at("width"),max),
        unsignedNumber(request.at("height"),max),data.data(),data.size(),offsets,
        origin=="bottom-left"?ImageOrigin::BottomLeft:ImageOrigin::TopLeft);
}
Json describe(const BcImage& image) {
    const char* alpha=image.format.alpha==BcAlpha::Opaque?"opaque":image.format.alpha==BcAlpha::PunchThrough?
        "punch-through":image.format.alpha==BcAlpha::Explicit4?"explicit-4":"interpolated-8";
    Json result={{"policy",bcCandidatePolicy},{"gpuEquivalent",false},{"glFormat",static_cast<std::uint32_t>(image.format.format)},
        {"format",image.format.name},{"colorEncoding",image.format.colorEncoding==ColorEncoding::Srgb?"srgb":"linear"},
        {"alpha",alpha},{"origin",image.origin==ImageOrigin::TopLeft?"top-left":"bottom-left"},{"levels",Json::array()}};
    for(const auto& level:image.levels) result["levels"].push_back({{"width",level.width},{"height",level.height},
        {"sourceOffset",level.sourceOffset},{"sourceBytes",level.sourceBytes},{"rgba",level.rgba}});
    return result;
}
osg::ref_ptr<osg::Image> osgImage(std::uint32_t format,unsigned width,unsigned height,const Bytes& data,GLenum type=GL_UNSIGNED_BYTE,unsigned depth=1) {
    auto image=new osg::Image;auto copy=new unsigned char[data.size()];std::copy(data.begin(),data.end(),copy);
    image->setImage(width,height,depth,format,format,type,copy,osg::Image::USE_NEW_DELETE,1);
    image->setWriteHint(osg::Image::STORE_INLINE);return image;
}
const std::array<Bytes,4> colors={Bytes{255,0,0,255},Bytes{0,255,0,255},Bytes{0,0,255,255},Bytes{255,255,255,255}};
const std::array<std::uint16_t,4> colorEndpoints={0xf800,0x07e0,0x001f,0xffff};
Bytes solidBlock(std::uint32_t format,unsigned color) {
    Bytes block;
    if(bcFormatInfo(format).blockBytes==16) block=Bytes(8,255); // both BC2/3 give alpha=255
    const auto endpoint=colorEndpoints.at(color);
    block.insert(block.end(),{static_cast<unsigned char>(endpoint),static_cast<unsigned char>(endpoint>>8),0,0,0,0,0,0});
    return block;
}
Json vectors() {
    auto result=Json::parse(readBytes(TEST_BC_VECTORS));
    // Independent endpoint-only expectations exercise 2D block addressing and cropped blocks.
    for(const auto dimensions:{std::pair<unsigned,unsigned>{5,3},{3,5},{5,7},{1,7},{7,1},{2052,1}}) {
        const auto width=dimensions.first,height=dimensions.second;Bytes data,expected;
        const unsigned columns=(width+3)/4,rows=(height+3)/4;
        for(unsigned by=0;by<rows;++by) for(unsigned bx=0;bx<columns;++bx) {
            auto block=solidBlock(0x83f1,(by*columns+bx)%4);data.insert(data.end(),block.begin(),block.end());
        }
        for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
            const auto& color=colors[((y/4)*columns+x/4)%4];expected.insert(expected.end(),color.begin(),color.end());
        }
        result["cases"].push_back({{"name","bc1-edge-"+std::to_string(width)+"x"+std::to_string(height)},
            {"basis","Independent endpoint colors in raster block order; outside texels ignored"},
            {"glFormat",0x83f1},{"width",width},{"height",height},{"data",data},{"expectedRgba",expected}});
    }
    result["mipCases"]=Json::array();
    for(auto format:{0x83f0U,0x83f1U,0x83f2U,0x83f3U,0x8c4cU,0x8c4dU,0x8c4eU,0x8c4fU}) {
        Bytes data;Json offsets=Json::array(),levels=Json::array();
        const std::array<unsigned,3> widths={7,3,1},heights={5,2,1},blockCounts={4,1,1};
        for(unsigned i=0;i<3;++i) {
            const auto offset=data.size();if(i) offsets.push_back(offset);
            const auto block=solidBlock(format,i);Bytes expected;
            for(unsigned b=0;b<blockCounts[i];++b) data.insert(data.end(),block.begin(),block.end());
            for(unsigned p=0;p<widths[i]*heights[i];++p) expected.insert(expected.end(),colors[i].begin(),colors[i].end());
            levels.push_back({{"width",widths[i]},{"height",heights[i]},{"sourceOffset",offset},
                {"sourceBytes",blockCounts[i]*block.size()},{"rgba",expected}});
        }
        result["mipCases"].push_back({{"name","custom-mip-"+std::to_string(format)},
            {"basis","Independent solid red/green/blue at 7x5, 3x2, 1x1; no generated mip"},
            {"glFormat",format},{"width",7},{"height",5},{"origin","top-left"},
            {"data",data},{"mipOffsets",offsets},{"expectedLevels",levels}});
    }
    return result;
}
void fixedVectors(const Json& corpus) {
    require(corpus["policy"]==bcCandidatePolicy && corpus["gpuEquivalent"]==false,"fixture policy");
    for(const auto& item:corpus["mipCases"]) {
        const auto result=describe(decodeRequest(item));
        require(result["levels"]==item["expectedLevels"] && result["origin"]=="top-left","exported mip vector independent expectation");
    }
    for(const auto& item:corpus["cases"]) {
        const auto name=item["name"].get<std::string>();const auto input=byteArray(item["data"]);
        const auto decoded=decodeRequest(item);const auto& level=decoded.levels.at(0);
        require(level.rgba==byteArray(item["expectedRgba"]),name+": independent RGBA expectation");
        require(level.width==item["width"] && level.height==item["height"] && level.sourceOffset==0 && level.sourceBytes==input.size(),name+": dimensions/source range");
        auto image=osgImage(item["glFormat"],item["width"],item["height"],input);
        if(item.value("origin",std::string())=="top-left") image->setOrigin(osg::Image::TOP_LEFT);
        const auto adapted=decodeOsgBcCandidate(*image,input.size());
        require(adapted.levels[0].rgba==level.rgba && adapted.origin==decoded.origin,name+": OSG adapter");
        require(Bytes(image->data(),image->data()+input.size())==input,name+": source bytes changed");
        rejects([&]{encodeImage(*image);},"GPU");
        // Run the same bytes in linear/sRGB counterparts: encoded values and alpha
        // must remain identical, with only the explicit encoding tag changing.
        auto alternate=item;const auto format=item["glFormat"].get<std::uint32_t>();
        alternate["glFormat"]=format<0x8c4c?format+0x8c4c-0x83f0:format-0x8c4c+0x83f0;
        const auto other=decodeRequest(alternate);
        require(decoded.format.colorEncoding!=other.format.colorEncoding && decoded.format.alpha==other.format.alpha && other.levels[0].rgba==level.rgba,name+": sRGB metadata, no gamma transform");
        // Deliberately unaligned source pointer must decode identically.
        Bytes unaligned{99};unaligned.insert(unaligned.end(),input.begin(),input.end());
        require(decodeBcCandidate(format,level.width,level.height,unaligned.data()+1,input.size()).levels[0].rgba==level.rgba,name+": unaligned input");
    }
}
void edgesAndMips() {
    for(auto format:{0x83f0U,0x83f1U,0x83f2U,0x83f3U,0x8c4cU,0x8c4dU,0x8c4eU,0x8c4fU}) {
        for(unsigned height=1;height<=9;++height) for(unsigned width=1;width<=9;++width) {
            Bytes data,expected;
            const unsigned columns=(width+3)/4,rows=(height+3)/4;
            for(unsigned i=0;i<columns*rows;++i) {auto block=solidBlock(format,i%4);data.insert(data.end(),block.begin(),block.end());}
            for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
                const auto& color=colors[((y/4)*columns+x/4)%4];expected.insert(expected.end(),color.begin(),color.end());
            }
            const auto before=data;auto output=decodeBcCandidate(format,width,height,data.data(),data.size());
            require(output.levels[0].rgba==expected && data==before,"block cropping/order/source immutability");
        }
        // Custom mip levels have unrelated colors, so silently regenerating or
        // dropping lower levels cannot satisfy these expectations. NPOT halves floor.
        Bytes data;std::vector<std::size_t> offsets;unsigned width=7,height=5;
        const std::array<std::pair<unsigned,unsigned>,3> sizes={{{7,5},{3,2},{1,1}}};
        for(unsigned i=0;i<sizes.size();++i) {
            if(i) offsets.push_back(data.size());
            const auto block=solidBlock(format,i);
            const auto size=bcLevelByteSize(format,sizes[i].first,sizes[i].second);
            for(std::size_t j=0;j<size;j+=block.size()) data.insert(data.end(),block.begin(),block.end());
        }
        const auto before=data;auto full=decodeBcCandidate(format,width,height,data.data(),data.size(),offsets,ImageOrigin::TopLeft);
        require(full.levels.size()==3 && full.origin==ImageOrigin::TopLeft && data==before,"full custom mip/origin/source");
        for(unsigned i=0;i<sizes.size();++i) {
            const auto& level=full.levels[i];Bytes expected;
            for(unsigned j=0;j<sizes[i].first*sizes[i].second;++j) expected.insert(expected.end(),colors[i].begin(),colors[i].end());
            require(level.width==sizes[i].first && level.height==sizes[i].second && level.sourceOffset==(i?offsets[i-1]:0) && level.rgba==expected,"custom mip exact pixels and range");
        }
        auto image=osgImage(format,width,height,data);image->setMipmapLevels({static_cast<unsigned>(offsets[0]),static_cast<unsigned>(offsets[1])});
        require(decodeOsgBcCandidate(*image,data.size()).levels[2].rgba==full.levels[2].rgba,"OSG full mip table");
        rejects([&]{encodeImage(*image);},"mip");
        const auto partial=decodeBcCandidate(format,width,height,data.data(),offsets.back(),{offsets.front()});
        require(partial.levels.size()==2 && partial.levels[1].rgba==full.levels[1].rgba,"explicit partial mip chain");
        const auto base=decodeBcCandidate(format,width,height,data.data(),offsets.front());
        require(base.levels.size()==1 && base.levels[0].rgba==full.levels[0].rgba,"base only without invented mip");
    }
}
void invalidInputs() {
    const Bytes data(32,0);auto decode=[&](unsigned format,unsigned w,unsigned h,std::size_t size,const std::vector<std::size_t>& offsets=std::vector<std::size_t>{}) {
        return decodeBcCandidate(format,w,h,data.data(),size,offsets);
    };
    for(auto format:{0U,0x1907U,0x84edU,0x8c48U,0x8dbbU,0x83f4U}) rejects([&]{decode(format,4,4,8);},"unsupported GL");
    rejects([&]{decode(0x83f1,0,4,8);},"nonzero");rejects([&]{decode(0x83f1,4,0,8);},"nonzero");
    rejects([&]{decodeBcCandidate(0x83f1,4,4,nullptr,8);},"null");
    rejects([&]{decode(0x83f1,4,4,7);},"truncated");rejects([&]{decode(0x83f3,4,4,15);},"truncated");
    rejects([&]{decode(0x83f1,4,4,9);},"trailing");rejects([&]{decode(0x83f1,4,4,16);},"missing mip");
    for(auto offset:{0U,7U,9U,99U}) rejects([&]{decode(0x83f1,4,4,16,{offset});},"contiguous");
    rejects([&]{decode(0x83f1,4,4,15,{8});},"truncated");
    rejects([&]{decode(0x83f1,4,4,24,{8,8});},"contiguous");
    rejects([&]{decode(0x83f1,1,1,16,{8});},"excess mip");
    rejects([&]{decode(0x83f1,4,4,32,{8,16,24});},"excess mip");
    constexpr auto max=std::numeric_limits<std::uint32_t>::max();
    rejects([&]{decode(0x83f1,max,max,data.size());},"overflow");
    rejects([&]{bcLevelByteSize(0x83f3,max,max);},"overflow");
    rejects([&]{decodeBcCandidate(0x83f1,4,4,data.data(),8,{},static_cast<ImageOrigin>(99));},"origin");
    const auto block=solidBlock(0x83f1,0);auto image=osgImage(0x83f1,4,4,block);
    rejects([&]{decodeOsgBcCandidate(*image,7);},"truncated");
    image->setInternalTextureFormat(0x8c4d);rejects([&]{decodeOsgBcCandidate(*image,8);},"internal format");image->setInternalTextureFormat(0x83f1);
    auto wrongType=osgImage(0x83f1,4,4,block,GL_UNSIGNED_SHORT);rejects([&]{decodeOsgBcCandidate(*wrongType,8);},"data type");
    auto volume=osgImage(0x83f1,4,4,data,GL_UNSIGNED_BYTE,2);rejects([&]{decodeOsgBcCandidate(*volume,data.size());},"2D image");
    image->setRowLength(5);rejects([&]{decodeOsgBcCandidate(*image,8);},"block rows");image->setRowLength(0);
    image->setPacking(3);rejects([&]{decodeOsgBcCandidate(*image,8);},"block rows");
    for(auto packing:{1U,2U,4U,8U}) {
        image->setPacking(packing);image->setRowLength(4);
        require(decodeOsgBcCandidate(*image,8).levels[0].rgba[0]==255,"packing must not pad compressed rows");
    }
    image->setMipmapLevels({7});rejects([&]{decodeOsgBcCandidate(*image,8);},"contiguous");
    auto empty=new osg::Image;osg::ref_ptr<osg::Image> owned=empty;rejects([&]{decodeOsgBcCandidate(*owned,0);},"2D image");
    Json request={{"glFormat",0x83f1},{"width",1},{"height",1},{"data",block}};
    for(const Json value:{Json(-1),Json(256),Json(0.5),Json(true)}) {request["data"][0]=value;rejects([&]{decodeRequest(request);},"out of range");}
    request["data"]=block;request["width"]=-1;rejects([&]{decodeRequest(request);},"out of range");
}
osg::ref_ptr<osg::Geode> texturedMesh(osg::Image* image) {
    auto geometry=new osg::Geometry;auto positions=new osg::Vec3Array;
    positions->push_back({0,0,0});positions->push_back({1,0,0});positions->push_back({0,1,0});geometry->setVertexArray(positions);
    auto uv=new osg::Vec2Array;uv->push_back({0,0});uv->push_back({1,0});uv->push_back({0,1});geometry->setTexCoordArray(0,uv);
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));auto node=new osg::Geode;node->addDrawable(geometry);
    auto texture=new osg::Texture2D(image);texture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
    node->getOrCreateStateSet()->setTextureAttributeAndModes(0,texture,osg::StateAttribute::ON);return node;
}
void serializationAndPassthrough(const fs::path& root) {
    fs::create_directories(root);
    // OSG round-trip uses aligned base size because this SDK's computed compressed
    // sizes for NPOT/small mips are not a trustworthy allocation-length oracle.
    unsigned roundtrips=0;Json sdkAudit=Json::array();
    for(auto format:{0x83f0U,0x83f1U,0x83f2U,0x83f3U,0x8c4cU,0x8c4dU,0x8c4eU,0x8c4fU}) {
        auto block=solidBlock(format,0);auto image=osgImage(format,4,4,block);auto node=texturedMesh(image);
        GeometryInput geometry{node->getDrawable(0)->asGeometry(),node->getStateSet(),osg::Matrixd::identity()};
        const auto direct=root/(std::to_string(format)+"-direct.glb");
        rejects([&]{writeGlb({geometry},direct,root/"direct-textures");},"GPU");
        require(!fs::exists(direct),"default in-memory compressed input must not publish");
        const auto computed=image->getTotalSizeInBytesIncludingMipmaps();
        sdkAudit.push_back({{"glFormat",format},{"actualAllocatedBytes",block.size()},{"osgComputedBytes",computed},
            {"osgIsCompressed",image->isCompressed()},{"serializationTested",computed==block.size()}});
        if(computed!=block.size()) {
            if(format<0x8c4c) throw std::runtime_error("OSG linear BC format size regression");
            std::cout<<"OSG serialization unavailable for GL "<<format<<": computed "<<computed<<", actual "<<block.size()<<" bytes; only explicit-buffer candidate verified.\n";
            continue;
        }
        const auto input=root/(std::to_string(format)+".osgb");osg::ref_ptr<osgDB::Options> writeOptions=new osgDB::Options("WriteImageHint=IncludeData");
        require(osgDB::writeNodeFile(*node,input.u8string(),writeOptions),"OSGB compressed fixture write");
        auto loaded=osgDB::readRefNodeFile(input.u8string());require(loaded.valid(),"OSGB compressed fixture read");
        auto texture=dynamic_cast<osg::Texture*>(loaded->getStateSet()->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
        require(texture && texture->getImage(0) && texture->getImage(0)->getPixelFormat()==format,"OSGB compressed format survives");
        const auto readImage=texture->getImage(0);
        require(readImage->getTotalSizeInBytesIncludingMipmaps()==block.size(),"aligned fixture computed size");
        require(Bytes(readImage->data(),readImage->data()+block.size())==block,"OSGB compressed bytes survive");
        require(decodeOsgBcCandidate(*readImage,block.size()).levels[0].rgba==decodeOsgBcCandidate(*image,block.size()).levels[0].rgba,"OSGB candidate pixels survive");
        Options options;options.input=input;options.output=root/(std::to_string(format)+"-out");options.srs="ENU:31,121,0";
        rejects([&]{convert(options);},"GPU");require(!fs::exists(options.output),"default compressed failure must not publish");++roundtrips;
    }
    atomicJson(root/"osg-compressed-serialization.json",sdkAudit);
    std::cout<<"OSGB format round-trips verified: "<<roundtrips<<"/8; SDK limitations recorded separately.\n";
    for(const std::string extension:{"png","jpg"}) {
        osg::ref_ptr<osg::Image> image=new osg::Image;image->allocateImage(2,2,1,GL_RGB,GL_UNSIGNED_BYTE,1);
        const Bytes pixels={255,0,0,0,255,0,0,0,255,255,255,255};std::memcpy(image->data(),pixels.data(),pixels.size());
        const auto file=root/("original."+extension);image->setFileName(file.u8string());
        require(osgDB::writeImageFile(*image,file.u8string()),"encoded fixture write "+extension);
        const auto original=readBytes(file);auto node=texturedMesh(image);
        const auto input=root/(extension+".osgb");osg::ref_ptr<osgDB::Options> writeOptions=new osgDB::Options("WriteImageHint=IncludeFile");
        require(osgDB::writeNodeFile(*node,input.u8string(),writeOptions),"encoded OSGB fixture write "+extension);
        Options options;options.input=input;options.output=root/(extension+"-out");options.srs="ENU:31,121,0";
        auto report=convert(options);require(report["originalEncodedImages"]==1,"ordinary passthrough "+extension);
        const auto uri=report["records"][0]["content"][0]["images"][0]["uri"].get<std::string>();
        require(readBytes(options.output/uri)==original,"original encoded bytes differ: "+extension);
    }
}
}
int main(int argc,char** argv) {
    try {
        if(argc==2 && std::string(argv[1])=="--vectors") {std::cout<<vectors().dump(2)<<'\n';return 0;}
        if(argc==3 && std::string(argv[1])=="--decode") {
            Json request;
            if(std::string(argv[2])=="-") std::cin>>request;else request=Json::parse(readBytes(fs::u8path(argv[2])));
            std::cout<<describe(decodeRequest(request)).dump(2)<<'\n';return 0;
        }
        if(argc!=1) throw std::runtime_error("usage: osgb-compressed-image-tests [--vectors | --decode request.json|-]");
        const auto corpus=vectors();fixedVectors(corpus);edgesAndMips();invalidInputs();
        const auto root=fs::current_path()/("compressed-image-cases-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        serializationAndPassthrough(root);
        std::cout<<"Passed "<<checks<<" checks, "<<corpus["cases"].size()<<" base + "<<corpus["mipCases"].size()<<" mip public-spec/policy vectors; GPU equivalence NOT established. Fixtures: "<<root<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
