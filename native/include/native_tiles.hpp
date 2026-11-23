#pragma once
#include <nlohmann/json.hpp>
#include <osg/BoundingBox>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Matrixd>
#include <osg/StateSet>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nt {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Bytes = std::vector<unsigned char>;

struct EncodedImage : osg::Referenced {
    Bytes bytes;
    std::string mime;
    // 原文件图片按顶行在前编码，OSG 通常按底行在前采样。
    bool flipV = true;
};
void installImageCapture();
void uninstallImageCapture();
unsigned validateImageForEncoding(const osg::Image& image);
void copyImageRgba8(const osg::Image& image, unsigned char* destination, std::size_t rowStrideBytes=0);
osg::ref_ptr<EncodedImage> encodeImage(const osg::Image& image);
osg::ref_ptr<EncodedImage> encodePreviewResizedImage(const osg::Image& image, unsigned maxDimension, unsigned jpegQuality=80);
std::uint64_t residentBytes();
std::string contentKey(const Bytes& bytes);
Bytes readBytes(const fs::path& file);
void atomicWrite(const fs::path& file, const Bytes& bytes);
void atomicJson(const fs::path& file, const Json& value);
// TRIANGLES may cyclically rotate each face; vertex values, winding and face order stay exact.
Bytes encodeMeshopt(const void* data, std::size_t length, std::size_t stride, bool indices, bool triangles=false);
Bytes decodeMeshopt(const Bytes& encoded, std::size_t count, std::size_t stride, bool indices);

struct GeometryInput {
    osg::ref_ptr<osg::Geometry> geometry;
    osg::ref_ptr<osg::StateSet> state;
    osg::Matrixd matrix;
};
struct ContentResult {
    osg::BoundingBoxd bounds;
    std::uint64_t vertices=0, triangles=0, images=0, bytes=0;
    double coordinateRoundingError=0;
    double quantizationErrorMeters=0;
    std::uint64_t originalFloatBytes=0;
    std::uint64_t geometryRawBytes=0, geometryStoredBytes=0, compressedViews=0;
    Json imageAudit=Json::array();
    Json dracoAudit=Json::object();
};
class SpatialReference {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    SpatialReference(const std::string& srs, const osg::Vec3d& origin);
    ~SpatialReference();
    bool projected() const;
    const osg::Matrixd& placement() const;
    osg::Vec3d toLocal(const osg::Vec3d& source) const;
    osg::Vec3d normalToLocal(const osg::Vec3d& point, const osg::Vec3d& normal) const;
};
struct Options;
ContentResult writeGlb(const std::vector<GeometryInput>& meshes, const fs::path& output,
                       const fs::path& textureDir, bool preserveEncoded=true,
                       const SpatialReference* spatial=nullptr, bool compressGeometry=false,
                       bool quantizeGeometry=false,const Options* codecOptions=nullptr);

struct Options {
    fs::path input;
    fs::path output;
    std::string srs;
    std::string origin;
    unsigned threads=2;
    // 0 表示不限；非 0 时只在驻留内存低于该值时提交下一个独立根块。
    std::uint64_t memoryBudgetBytes=0;
    bool preserveEncoded=true;
    bool compressGeometry=true;
    bool dracoGeometry=false;
    unsigned dracoSpeed=5;
    std::string textureFormat="original";
    // 将贴图直接内嵌在 GLB bufferView 中，避免浏览器额外发起外置贴图 HTTP 请求，提升首帧加载速度。
    bool embedTextures=false;
    // 实验：使用 KHR_mesh_quantization；非默认路径，误差单独报告。
    bool quantizeGeometry=false;
    // 独立轻量预览；不改变主产物的编码、误差与 sourceExact 语义。
    bool preview=false;
    // 仅供 preview.cpp 的 GLB 写出副本设置；主产物即使开启 preview 也不读取它。
    bool previewEncoding=false;
    // 仅供 preview.cpp 非叶子瓦片限制远中景内嵌纹理最大边长（0 表示保持原始分辨率，叶子层恒为 0）。
    unsigned previewMaxTextureSize=0;
    unsigned previewJpegQuality=80;
    // 新增有损远景祖先层；原始源内容全部保留。
    bool rebuildTop=false;
    // Boundary weld tolerance for rebuilt ancestors, in metres. 0 disables
    // sealing. Clamped by the CLI to 0..2.
    double topSealDistance=2.;
    // 实验：恢复同文件并列 PagedLOD 的独立细化单元；默认保持现有文件级树。
    bool splitPagedLod=false;
    // 仅显式启用时持久化文件边界；恢复必须完整验证身份与资产。
    fs::path checkpointDir;
    // 合作暂停令牌放在检查点和输出目录之外。convert 返回 status=paused。
    fs::path pauseFile;
    bool resume=false;
    // 事件可能由工作线程发出；转换器内部串行调用。total=0 表示扫描总量未知。
    std::function<void(const Json&)> event;
    // 原生验证驱动的只读观察点：同次生成的 Top、在有损编码前。CLI/App 不设置。
    std::function<void(const std::string&, const std::vector<GeometryInput>&)> observeTopGeometry;
    // 在源文件处理与发布边界检查，抛异常后由输出事务清理暂存。
    std::function<void()> checkCancelled;
};
void configureRuntime(const fs::path& executable);
Json capabilities();
Json convert(const Options& options);
osg::Matrixd enuTransform(double latitude, double longitude, double height,
                          const osg::Vec3d& origin);
Json boundingBoxJson(const osg::BoundingBoxd& box);
}
