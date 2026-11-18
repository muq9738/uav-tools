#include "native_tiles.hpp"
#include "compressed_image.hpp"
#include <osgDB/FileNameUtils>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <osgDB/ReaderWriter>
#include <png.h>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace nt {
Bytes readBytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("无法读取文件: " + p.string());
    return Bytes(std::istreambuf_iterator<char>(in), {});
}
std::string contentKey(const Bytes& bytes) {
    // 仅作稳定文件命名；命中后仍逐字节核对，哈希碰撞不会合并不同图片。
    std::uint64_t a=14695981039346656037ULL, b=1099511628211ULL;
    for (auto c:bytes) { a=(a^c)*1099511628211ULL; b=(b^(c+17U))*14029467366897019727ULL; }
    std::ostringstream s; s<<std::hex<<std::setfill('0')<<std::setw(16)<<a<<std::setw(16)<<b;
    return s.str();
}
void atomicWrite(const fs::path& p,const Bytes& bytes) {
    static std::atomic<unsigned long> sequence{0};
    fs::create_directories(p.parent_path());
    fs::path temporary=p;temporary+=".tmp-"+std::to_string(sequence++);
    {
        std::ofstream out(temporary,std::ios::binary|std::ios::trunc);
        if(!out || !out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()))
            throw std::runtime_error("输出写入失败: "+temporary.string());
    }
    fs::rename(temporary,p);
}
void atomicJson(const fs::path& p,const Json& j) {
    auto s=j.dump(2); atomicWrite(p,Bytes(s.begin(),s.end()));
}
namespace {
std::string mime(const Bytes& b) {
    if(b.size()>3 && b[0]==255 && b[1]==216 && b[2]==255) return "image/jpeg";
    if(b.size()>8 && png_sig_cmp(b.data(),0,8)==0) return "image/png";
    return {};
}
// BMP 不属于当前输出合同；在插件选择处拒绝，避免各平台另载解码插件后静默转码。
class UnsupportedBmpReader final : public osgDB::ReaderWriter {
public:
    UnsupportedBmpReader() {supportsExtension("bmp","不支持 BMP 编码纹理");}
    ReadResult readImage(std::istream& s,const Options* =nullptr) const override {
        char head[2]={};s.read(head,2);s.clear();s.seekg(0);
        if(s.gcount()==2 && head[0]=='B' && head[1]=='M') return ReadResult("BMP 编码纹理尚未建立等价输出，拒绝静默转码");
        return ReadResult::FILE_NOT_HANDLED;
    }
    ReadResult readImage(const std::string& name,const Options* =nullptr) const override {
        if(osgDB::convertToLowerCase(osgDB::getFileExtension(name))=="bmp")
            return ReadResult("BMP 编码纹理尚未建立等价输出，拒绝静默转码");
        return ReadResult::FILE_NOT_HANDLED;
    }
};
// 包装正式图像插件的入口，捕获 OSGB 内嵌原编码，不重编解码后再猜测图片。
class CaptureReader final : public osgDB::ReaderWriter {
    osg::ref_ptr<osgDB::ReaderWriter> original_;
public:
    explicit CaptureReader(osgDB::ReaderWriter* rw):osgDB::ReaderWriter(*rw),original_(rw) {}
    bool acceptsExtension(const std::string& extension) const override { return original_->acceptsExtension(extension); }
    osgDB::ReaderWriter* original() const { return original_.get(); }
    WriteResult writeImage(const osg::Image& image,std::ostream& stream,const Options* options=nullptr) const override {
        return original_->writeImage(image,stream,options);
    }
    WriteResult writeImage(const osg::Image& image,const std::string& name,const Options* options=nullptr) const override {
        return original_->writeImage(image,name,options);
    }
    ReadResult readImage(std::istream& stream,const Options* options=nullptr) const override {
        Bytes bytes(std::istreambuf_iterator<char>(stream),{});
        std::string data(bytes.begin(),bytes.end()); std::istringstream copy(data);
        auto result=original_->readImage(copy,options);
        if(result.validImage()) capture(result.getImage(),bytes);
        return result;
    }
    ReadResult readImage(const std::string& name,const Options* options=nullptr) const override {
        auto result=original_->readImage(name,options);
        if(result.validImage()) {
            auto resolved=osgDB::findDataFile(name,options);
            if(!resolved.empty()) capture(result.getImage(),readBytes(fs::u8path(resolved)));
        }
        return result;
    }
    static void capture(osg::Image* image,const Bytes& bytes) {
        auto type=mime(bytes);
        // 此处只接收正式插件已成功读取的编码流；不能把未知容器伪装成原始像素。
        if(type.empty()) type="application/octet-stream";
        auto data=new EncodedImage; data->bytes=bytes; data->mime=type;
        data->flipV=image->getOrigin()==osg::Image::BOTTOM_LEFT;
        image->setUserData(data);
    }
};
}
void installImageCapture() {
    {
        auto registry=osgDB::Registry::instance();
        // 在任何工作线程启动前加载插件，避免并发修改 ReaderWriter 注册表。
        registry->getReaderWriterForExtension("osgb");
        registry->getReaderWriterForExtension("png");
        registry->getReaderWriterForExtension("jpg");
        registry->getReaderWriterForExtension("jpeg");
        // Some platforms ship an osgdb_bmp plugin whose priority would bypass
        // our rejection. Preload it, then replace every bmp-capable reader with
        // the rejecting one so behaviour is identical across platforms.
        registry->getReaderWriterForExtension("bmp");
        auto& readers=registry->getReaderWriterList();
        for(auto& reader:readers)
            if(reader->acceptsExtension("png") || reader->acceptsExtension("jpg") || reader->acceptsExtension("jpeg"))
                reader=new CaptureReader(reader.get());
        // Only replace dedicated bmp plugins. A multi-format plugin such as
        // ImageIO also handles png/jpeg and must not be swapped out, otherwise
        // png decoding breaks. The front reader still rejects bmp everywhere.
        for(auto& reader:readers)
            if(reader->acceptsExtension("bmp") && !reader->acceptsExtension("png") && !reader->acceptsExtension("jpg") && !reader->acceptsExtension("jpeg"))
                reader=new UnsupportedBmpReader;
        readers.insert(readers.begin(),new UnsupportedBmpReader);
    }
}
void uninstallImageCapture() {
    // 在 OSG 卸载插件动态库前释放包装器，避免原插件 vtable 已卸载后才析构。
    auto& readers=osgDB::Registry::instance()->getReaderWriterList();
    for(auto it=readers.begin();it!=readers.end();) {
        if(dynamic_cast<UnsupportedBmpReader*>(it->get())) {it=readers.erase(it);continue;}
        if(auto capture=dynamic_cast<CaptureReader*>(it->get())) *it=capture->original();
        ++it;
    }
}
experimental::BcImage experimental::decodeOsgBcCandidate(const osg::Image& im,std::size_t sourceByteLength) {
    if(im.s()<=0 || im.t()<=0 || im.r()!=1 || !im.data())
        throw std::runtime_error("BC candidate: source must be a nonempty 2D image");
    bcFormatInfo(im.getPixelFormat());
    if(im.getDataType()!=GL_UNSIGNED_BYTE || im.getInternalTextureFormat()!=static_cast<GLint>(im.getPixelFormat()))
        throw std::runtime_error("BC candidate: ambiguous OSG data type or internal format");
    if((im.getRowLength()!=0 && im.getRowLength()!=im.s()) ||
       (im.getPacking()!=1 && im.getPacking()!=2 && im.getPacking()!=4 && im.getPacking()!=8))
        throw std::runtime_error("BC candidate: only tightly packed block rows are supported");
    if(im.getOrigin()!=osg::Image::BOTTOM_LEFT && im.getOrigin()!=osg::Image::TOP_LEFT)
        throw std::runtime_error("BC candidate: unknown OSG image origin");
    const auto& offsets=im.getMipmapLevels();
    return decodeBcCandidate(im.getPixelFormat(),im.s(),im.t(),im.data(),sourceByteLength,
        std::vector<std::size_t>(offsets.begin(),offsets.end()),
        im.getOrigin()==osg::Image::TOP_LEFT?ImageOrigin::TopLeft:ImageOrigin::BottomLeft);
}
osg::ref_ptr<EncodedImage> encodeImage(const osg::Image& im) {
    if(im.s()<=0 || im.t()<=0 || im.r()!=1 || !im.data())
        throw std::runtime_error("纹理为空或不是二维图片: "+im.getFileName());
    if(im.isMipmap())
        throw std::runtime_error("源纹理包含内嵌 mip 链，PNG 编码不能等价保留，拒绝静默丢弃: "+im.getFileName());
    const auto format=im.getPixelFormat();
    // OSG 3.6.5 的 isCompressed() 未覆盖 EXT_texture_sRGB 的四个 S3TC 枚举。
    if(im.isCompressed() || (format>=0x8C4C && format<=0x8C4F))
        throw std::runtime_error("GPU 压缩纹理的 CPU 候选尚未证明采样等价，默认拒绝有损替代: "+im.getFileName());
    if(im.getDataType()!=GL_UNSIGNED_BYTE)
        throw std::runtime_error("原始纹理暂不支持此像素类型，拒绝有损替代: "+im.getFileName());
    unsigned channels=0;
    if(format==GL_RGB || format==GL_BGR) channels=3;
    if(format==GL_RGBA || format==GL_BGRA) channels=4;
    if(format==GL_LUMINANCE || format==GL_ALPHA) channels=1;
    if(format==GL_LUMINANCE_ALPHA) channels=2;
    if(!channels) throw std::runtime_error("未知纹理通道: "+std::to_string(format));
    Bytes pixels(static_cast<std::size_t>(im.s())*im.t()*4);
    for(int y=0;y<im.t();++y) for(int x=0;x<im.s();++x) {
        auto source=im.data(x,y);auto target=&pixels[(static_cast<std::size_t>(y)*im.s()+x)*4];
        if(channels>=3) {
            bool bgr=format==GL_BGR || format==GL_BGRA;
            target[0]=source[bgr?2:0];target[1]=source[1];target[2]=source[bgr?0:2];
            target[3]=channels==4?source[3]:255;
        } else {
            target[0]=target[1]=target[2]=format==GL_ALPHA?255:source[0];
            target[3]=format==GL_ALPHA?source[0]:(channels==2?source[1]:255);
        }
    }
    png_image image{};image.version=PNG_IMAGE_VERSION;image.width=im.s();image.height=im.t();image.format=PNG_FORMAT_RGBA;
    png_alloc_size_t length=0;
    if(!png_image_write_to_memory(&image,nullptr,&length,0,pixels.data(),0,nullptr))
        throw std::runtime_error("PNG 编码失败: "+std::string(image.message));
    auto result=new EncodedImage;result->bytes.resize(length);result->mime="image/png";result->flipV=false;
    if(!png_image_write_to_memory(&image,result->bytes.data(),&length,0,pixels.data(),0,nullptr)) {
        osg::ref_ptr<EncodedImage> cleanup=result;
        throw std::runtime_error("PNG 编码失败: "+std::string(image.message));
    }
    result->bytes.resize(length);return result;
}
}
