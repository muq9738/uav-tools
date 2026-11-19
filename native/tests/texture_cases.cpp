#include "native_tiles.hpp"
#include <osg/Geode>
#include <osg/Texture2D>
#include <osgDB/WriteFile>
#include <osgDB/ReadFile>
#include <png.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif

using namespace nt;
namespace {
void require(bool ok,const std::string& message) {if(!ok) throw std::runtime_error(message);}
osg::ref_ptr<osg::Geode> mesh() {
    auto geometry=new osg::Geometry;auto points=new osg::Vec3Array;
    points->push_back({0,0,0});points->push_back({1,0,0});points->push_back({0,1,0});geometry->setVertexArray(points);
    auto uv=new osg::Vec2Array;uv->push_back({0,0});uv->push_back({1,0});uv->push_back({0,1});geometry->setTexCoordArray(0,uv);
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));auto node=new osg::Geode;node->addDrawable(geometry);return node;
}
osg::ref_ptr<osg::Image> raw(unsigned width,unsigned height,GLenum format,unsigned packing=1) {
    auto image=new osg::Image;image->allocateImage(width,height,1,format,GL_UNSIGNED_BYTE,packing);image->setWriteHint(osg::Image::STORE_INLINE);
    std::memset(image->data(),0,image->getTotalSizeInBytesIncludingMipmaps());return image;
}
void write(osg::Node* node,const fs::path& file,const char* hint="IncludeData") {
    osg::ref_ptr<osgDB::Options> options=new osgDB::Options(std::string("WriteImageHint=")+hint);
    require(osgDB::writeNodeFile(*node,file.u8string(),options.get()),"无法生成夹具: "+file.u8string());
}
osg::ref_ptr<osg::Texture2D> texture(osg::Image* image) {
    auto value=new osg::Texture2D(image);value->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
    value->setWrap(osg::Texture::WRAP_T,osg::Texture::MIRROR);value->setFilter(osg::Texture::MIN_FILTER,osg::Texture::NEAREST);
    value->setFilter(osg::Texture::MAG_FILTER,osg::Texture::NEAREST);return value;
}
Bytes pngPixels(const fs::path& file) {
    auto bytes=readBytes(file);png_image image{};image.version=PNG_IMAGE_VERSION;
    require(png_image_begin_read_from_memory(&image,bytes.data(),bytes.size()),"PNG独立解码初始化失败");image.format=PNG_FORMAT_RGBA;
    Bytes values(PNG_IMAGE_SIZE(image));bool ok=png_image_finish_read(&image,nullptr,values.data(),0,nullptr);png_image_free(&image);require(ok,"PNG独立解码失败");return values;
}
Json makeFixtures(const fs::path& root) {
    fs::create_directories(root);Json cases=Json::array();
    auto add=[&](const std::string& name,osg::Image* image,const Bytes& expected,bool inherited=false,bool enabled=true) {
        auto node=mesh();auto tex=texture(image);
        if(image && image->isMipmap()) tex->setFilter(osg::Texture::MIN_FILTER,osg::Texture::NEAREST_MIPMAP_NEAREST);
        osg::ref_ptr<osg::Node> result=node;
        auto state=node->getOrCreateStateSet();
        if(inherited) {auto group=new osg::Group;group->addChild(node);result=group;state=group->getOrCreateStateSet();}
        state->setTextureAttributeAndModes(0,tex,enabled?osg::StateAttribute::ON:osg::StateAttribute::OFF);
        state->setMode(GL_BLEND,osg::StateAttribute::ON);write(result,root/(name+".osgb"));
        cases.push_back({{"name",name},{"file",name+".osgb"},{"expectTexture",enabled},{"expectedRgba",expected},{"expectedWidth",image?image->s():0},{"expectedHeight",image?image->t():0},{"inherited",inherited}});
    };
    const Bytes rgba={255,0,0,0,0,255,0,64,0,0,255,128,255,255,0,255};
    auto image=raw(2,2,GL_RGBA);std::memcpy(image->data(),rgba.data(),rgba.size());add("raw-rgba-alpha",image,rgba);
    add("parent-texture",image,rgba,true);
    auto bgra=raw(2,2,GL_BGRA);
    for(unsigned i=0;i<4;++i) for(unsigned c=0;c<4;++c) bgra->data()[i*4+c]=rgba[i*4+(c==0?2:c==2?0:c)];
    add("raw-bgra",bgra,rgba);
    auto gray=raw(2,2,GL_LUMINANCE);const unsigned char grayValues[]={0,64,128,255};std::memcpy(gray->data(),grayValues,4);Bytes grayExpected;
    for(auto value:grayValues) grayExpected.insert(grayExpected.end(),{value,value,value,255});add("raw-gray",gray,grayExpected);
    auto grayAlpha=raw(2,2,GL_LUMINANCE_ALPHA);const unsigned char ga[]={0,0,64,64,128,128,255,255};std::memcpy(grayAlpha->data(),ga,8);Bytes gaExpected;
    for(unsigned i=0;i<4;++i) gaExpected.insert(gaExpected.end(),{ga[i*2],ga[i*2],ga[i*2],ga[i*2+1]});add("raw-gray-alpha",grayAlpha,gaExpected);
    const Bytes rgbValues={255,0,0,0,255,0,0,0,255,255,255,0,0,255,255,255,0,255};
    auto packed=raw(3,2,GL_RGB,4);Bytes packedExpected;
    for(unsigned y=0;y<2;++y) {
        for(unsigned x=0;x<3;++x) {auto pixel=packed->data(x,y);std::memcpy(pixel,rgbValues.data()+(y*3+x)*3,3);packedExpected.insert(packedExpected.end(),{pixel[0],pixel[1],pixel[2],255});}
        packed->data(0,y)[9]=41+y;packed->data(0,y)[10]=51+y;packed->data(0,y)[11]=61+y;
    }
    add("rgb-row-padding",packed,packedExpected);
    auto length=new osg::Image;auto rowData=new unsigned char[24];std::memset(rowData,87,24);
    length->setImage(2,2,1,GL_RGB,GL_RGB,GL_UNSIGNED_BYTE,rowData,osg::Image::USE_NEW_DELETE,4,3);length->setWriteHint(osg::Image::STORE_INLINE);Bytes lengthExpected;
    for(unsigned y=0;y<2;++y) for(unsigned x=0;x<2;++x) {auto pixel=length->data(x,y);std::memcpy(pixel,rgbValues.data()+(y*3+x)*3,3);lengthExpected.insert(lengthExpected.end(),{pixel[0],pixel[1],pixel[2],255});}
    add("rgb-row-length",length,lengthExpected);
    // 开启且没有图片，与显式关闭但同样没有图片，必须有不同结果。
    auto missing=mesh();missing->getOrCreateStateSet()->setTextureAttributeAndModes(0,texture(nullptr),osg::StateAttribute::ON);write(missing,root/"missing-image.osgb");
    cases.push_back({{"name","missing-image"},{"file","missing-image.osgb"},{"expectFailure",true}});
    add("disabled-missing-image",nullptr,{},false,false);
    const unsigned char dxtBlock[]={0,0,255,255,255,255,255,255};
    auto dxt=new osg::Image;auto blocks=new unsigned char[8];std::memcpy(blocks,dxtBlock,8);
    dxt->setImage(4,4,1,GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,GL_UNSIGNED_BYTE,blocks,osg::Image::USE_NEW_DELETE,1);dxt->setWriteHint(osg::Image::STORE_INLINE);
    add("dxt1-transparent-index",dxt,Bytes(4*4*4,0));cases.back()["expectFailure"]=true;
    auto largeDxt=new osg::Image;auto largeBlocks=new unsigned char[513*8];
    for(unsigned i=0;i<513;++i) std::memcpy(largeBlocks+i*8,dxtBlock,8);
    largeDxt->setImage(2052,4,1,GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,GL_UNSIGNED_BYTE,largeBlocks,osg::Image::USE_NEW_DELETE,1);largeDxt->setWriteHint(osg::Image::STORE_INLINE);
    add("dxt1-size-over-2048",largeDxt,{});cases.back()["expectFailure"]=true;
    // 自定义mip不能被base-level比较冒充采样等价。
    auto mip=new osg::Image;auto mipBytes=new unsigned char[20];std::memcpy(mipBytes,rgba.data(),16);mipBytes[16]=17;mipBytes[17]=23;mipBytes[18]=31;mipBytes[19]=255;
    mip->setImage(2,2,1,GL_RGBA,GL_RGBA,GL_UNSIGNED_BYTE,mipBytes,osg::Image::USE_NEW_DELETE,1);mip->setMipmapLevels({16});mip->setWriteHint(osg::Image::STORE_INLINE);
    add("custom-mip-chain",mip,rgba);cases.back()["requiresMipAudit"]=true;cases.back()["expectFailure"]=true;
    // 外部文件路径故意不存在，验证OSG读取失败是否穿透到发布结果。
    auto external=raw(2,2,GL_RGBA);std::memcpy(external->data(),rgba.data(),rgba.size());external->setWriteHint(osg::Image::EXTERNAL_FILE);external->setFileName((root/"absent.png").u8string());
    auto externalNode=mesh();externalNode->getOrCreateStateSet()->setTextureAttributeAndModes(0,texture(external),osg::StateAttribute::ON);write(externalNode,root/"external-missing.osgb","UseExternal");
    cases.push_back({{"name","external-missing"},{"file","external-missing.osgb"},{"expectFailure",true}});
    atomicJson(root/"cases.json",cases);return cases;
}
}
int main(int argc,char** argv) {
    try {
        fs::path root=argc==3 && std::string(argv[1])=="--fixtures"?fs::u8path(argv[2]):fs::current_path()/("texture-cases-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        auto cases=makeFixtures(root);if(argc==3) {std::cout<<root<<'\n';return 0;}
        unsigned passed=0;Json observed=Json::array();
        for(const auto& item:cases) {
            Options options;options.input=root/item["file"].get<std::string>();options.output=root/("out-"+item["name"].get<std::string>());options.srs="ENU:31.2,121.4,10";
            bool failed=false;Json report;std::string error;
            if(item.value("requiresMipAudit",false)) {
                auto node=osgDB::readRefNodeFile(options.input.u8string());
                auto sourceTexture=dynamic_cast<osg::Texture*>(node->getStateSet()->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
                require(sourceTexture && sourceTexture->getImage(0)->getNumMipmapLevels()==2 && sourceTexture->getFilter(osg::Texture::MIN_FILTER)==osg::Texture::NEAREST_MIPMAP_NEAREST,"夹具必须在序列化后保留并实际启用两级mip");
            }
            try {report=convert(options);} catch(const std::exception& e) {failed=true;error=e.what();}
            observed.push_back({{"name",item["name"]},{"failed",failed},{"error",error}});
            require(failed==item.value("expectFailure",false),item["name"].get<std::string>()+": 意外的转换状态: "+error);
            if(failed) {
                require(!fs::exists(options.output),"失败后不能发布输出");
                require(error.find(item["file"].get<std::string>())!=std::string::npos,"失败必须标出源OSGB文件");
                if(item.value("requiresMipAudit",false)) require(error.find("mip")!=std::string::npos,"必须解释不能保留源mip");
                ++passed;continue;
            }
            if(!item.value("expectTexture",true)) {require(report["imageReferences"]==0,"关闭纹理不能重新启用");++passed;continue;}
            require(report["imageReferences"]==1,"纹理引用丢失");auto image=report["records"][0]["content"][0]["images"][0];
            auto glb=readBytes(options.output/report["records"][0]["content"][0]["uri"].get<std::string>());std::uint32_t jsonSize=0;std::memcpy(&jsonSize,glb.data()+12,4);
            auto doc=Json::parse(glb.begin()+20,glb.begin()+20+jsonSize);auto sampler=doc["samplers"][0];
            require(sampler["minFilter"]==GL_NEAREST && sampler["magFilter"]==GL_NEAREST && sampler["wrapS"]==GL_CLAMP_TO_EDGE && sampler["wrapT"]==GL_MIRRORED_REPEAT,"不能覆盖源过滤或边界模式");
            require(doc["materials"][0]["alphaMode"]=="BLEND","不能丢弃透明混合状态");
            auto actual=pngPixels(options.output/image["uri"].get<std::string>());
            require(actual==item["expectedRgba"].get<Bytes>(),item["name"].get<std::string>()+": 逐像素RGBA不一致");++passed;
        }
        atomicJson(root/"native-observed.json",observed);
        std::cout<<"通过 "<<passed<<" 项纹理边界；夹具: "<<root<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
