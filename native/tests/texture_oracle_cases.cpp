// OSGB 图片兼容回归：每个夹具都带一个不经过 OSG 图片插件的独立 oracle。
//
// 反循环原则（本文件判断对错的唯一依据）：
//   1. 原始像素（RGBA/RGB/BGR/BGRA/灰度/灰度 alpha/packing/rowLength）：
//      作者按 GL 内存约定构造 osg::Image，期望画面由本文件的通道映射独立推导，
//      不读取任何 OSG 插件解码结果。文件层用例中 .osgb 保存的是未编码原始像素
//      （夹具回读自检已确认逐字节存在），所以期望画面仍然独立。
//   2. 原始 JPEG/PNG 编码：真相是容器字节本身（由本文件的 zlib/turbojpeg 生成）。
//      断言输出图片与源容器逐字节相同，并用独立解码核对画面；不以 OSG 插件
//      解码或编码后的像素作为 oracle。
//   3. 方向：期望按 GL 约定（内存行 0 = 图片底部）从作者图片采样；实际按 GLB 自己
//      声明的采样器与 KHR_texture_transform 采样，二者逐像素比较。翻转标志是否自洽
//      由像素数据判定，不依据 osg::Image::getOrigin() 的名字。
//
// 事实依据（本机实测，2026-11，见 .agents-debug）：
//   macOS 上 OSG 的 png/jpg 插件是 ImageIO，读取时会把 alpha=0 的 RGB 清零
//   （预乘后无法还原），写入时会做一次反预乘并放大低 alpha 像素，另有 ±1 色彩空间
//   往返。因此涉及“像素值”的断言必须绕开该插件（原则 1），涉及“原字节”的断言
//   只比较字节（原则 2）。同一次实测也确认：.osgb 的 WriteImageHint=IncludeData
//   在本机构建下保存的是未编码原始像素，ContextCapture 生成的瓦片则内嵌 JPEG。

#include "native_tiles.hpp"
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/Texture2D>
#include <osgDB/Options>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <png.h>

#ifndef GL_MIRRORED_REPEAT
#define GL_MIRRORED_REPEAT 0x8370
#endif
#include <turbojpeg.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace nt;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string quad(const unsigned char* p) {
    return "(" + std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "," + std::to_string(p[3]) + ")";
}

// ---------------------------------------------------------------- 图片与容器

// 行 0 = 图片顶部（文件/显示顺序），每像素 RGBA。
struct Picture {
    unsigned width = 0, height = 0;
    Bytes rgba;
    unsigned char* at(unsigned x, unsigned y) { return &rgba[(std::size_t(y) * width + x) * 4]; }
    const unsigned char* at(unsigned x, unsigned y) const { return &rgba[(std::size_t(y) * width + x) * 4]; }
};
Picture makePicture(unsigned width, unsigned height, Bytes rgba) {
    Picture picture; picture.width = width; picture.height = height; picture.rgba = std::move(rgba);
    require(picture.rgba.size() == std::size_t(width) * height * 4, "作者图片缓冲区尺寸不符");
    return picture;
}
bool samePicture(const Picture& a, const Picture& b) {
    return a.width == b.width && a.height == b.height && a.rgba == b.rgba;
}
Picture flipVertical(const Picture& source) {
    Picture result; result.width = source.width; result.height = source.height; result.rgba.resize(source.rgba.size());
    for (unsigned y = 0; y < source.height; ++y)
        std::copy_n(source.at(0, source.height - 1 - y), source.width * 4, result.at(0, y));
    return result;
}
Picture flipHorizontal(const Picture& source) {
    Picture result; result.width = source.width; result.height = source.height; result.rgba.resize(source.rgba.size());
    for (unsigned y = 0; y < source.height; ++y) for (unsigned x = 0; x < source.width; ++x)
        std::copy_n(source.at(source.width - 1 - x, y), 4, result.at(x, y));
    return result;
}

Picture decodePngOwn(const Bytes& bytes, const std::string& where) {
    png_image image{}; image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()))
        throw std::runtime_error(where + ": libpng 无法初始化解码: " + image.message);
    image.format = PNG_FORMAT_RGBA;
    Picture picture; picture.width = image.width; picture.height = image.height;
    picture.rgba.resize(PNG_IMAGE_SIZE(image));
    bool ok = png_image_finish_read(&image, nullptr, picture.rgba.data(), 0, nullptr) != 0;
    std::string message = image.message; png_image_free(&image);
    require(ok, where + ": libpng 解码失败: " + message);
    return picture;
}
Picture decodeJpegOwn(const Bytes& bytes, const std::string& where) {
    tjhandle handle = tjInitDecompress();
    require(handle != nullptr, "turbojpeg 初始化解码器失败");
    int width = 0, height = 0, subsampling = 0, colorspace = 0;
    if (tjDecompressHeader3(handle, bytes.data(), bytes.size(), &width, &height, &subsampling, &colorspace) != 0) {
        std::string error = tjGetErrorStr2(handle); tjDestroy(handle);
        throw std::runtime_error(where + ": turbojpeg 无法解析头: " + error);
    }
    Picture picture; picture.width = unsigned(width); picture.height = unsigned(height);
    picture.rgba.resize(std::size_t(width) * height * 4);
    int status = tjDecompress2(handle, bytes.data(), bytes.size(), picture.rgba.data(), width, 0, height, TJPF_RGBA, 0);
    std::string error = status ? tjGetErrorStr2(handle) : ""; tjDestroy(handle);
    require(status == 0, where + ": turbojpeg 解码失败: " + error);
    return picture;
}
bool looksPng(const Bytes& bytes) {
    return bytes.size() > 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
}
bool looksJpeg(const Bytes& bytes) {
    return bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
}
Picture decodeContainer(const Bytes& bytes, const std::string& where) {
    if (looksPng(bytes)) return decodePngOwn(bytes, where);
    if (looksJpeg(bytes)) return decodeJpegOwn(bytes, where);
    throw std::runtime_error(where + ": 既不是 PNG 也不是 JPEG");
}

void appendBe32(Bytes& out, std::uint32_t value) {
    out.push_back(value >> 24); out.push_back(value >> 16); out.push_back(value >> 8); out.push_back(value);
}
void appendChunk(Bytes& out, const char type[4], const Bytes& payload) {
    appendBe32(out, std::uint32_t(payload.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), payload.begin(), payload.end());
    uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
    // zlib's crc32(crc, Z_NULL, len) returns 0, so an empty payload whose
    // data() is null would reset the chunk CRC to 0 (IEND must be 0xAE426082).
    if (!payload.empty()) crc = crc32(crc, payload.data(), uLong(payload.size()));
    appendBe32(out, std::uint32_t(crc));
}
Bytes deflateBytes(const Bytes& raw) {
    uLongf bound = compressBound(uLong(raw.size()));
    Bytes out(bound);
    require(compress2(out.data(), &bound, raw.data(), uLong(raw.size()), 9) == Z_OK, "zlib 压缩失败");
    out.resize(bound); return out;
}
// 自写 PNG（filter 0）。行 0 = picture 第 0 行（顶部）。exactSize 非 0 时用 tEXt 补到
// 精确字节数，便于逐字节替换模板 OSGB 的内嵌图片槽位。
Bytes encodePngOwn(const Picture& picture, std::size_t exactSize = 0) {
    require(picture.width && picture.height, "作者图片为空");
    Bytes raw; raw.reserve(std::size_t(picture.height) * (picture.width * 4 + 1));
    for (unsigned y = 0; y < picture.height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), picture.at(0, y), picture.at(0, y) + picture.width * 4);
    }
    Bytes ihdr;
    appendBe32(ihdr, picture.width); appendBe32(ihdr, picture.height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    Bytes skeleton = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    appendChunk(skeleton, "IHDR", ihdr);
    Bytes fixed;
    appendChunk(fixed, "IDAT", deflateBytes(raw));
    appendChunk(fixed, "IEND", {});
    Bytes out = skeleton;
    if (exactSize) {
        // tEXt 块开销 = 12（长度/类型/CRC）+ 关键字 "note" + 结尾 0
        std::size_t base = skeleton.size() + fixed.size() + 12 + 5;
        require(exactSize >= base, "目标字节数过小，无法用 tEXt 补齐 PNG");
        Bytes text = {'n', 'o', 't', 'e', 0};
        text.resize(text.size() + (exactSize - base), 'P');
        appendChunk(out, "tEXt", text);
    }
    out.insert(out.end(), fixed.begin(), fixed.end());
    require(!exactSize || out.size() == exactSize, "PNG 补齐后字节数不符");
    return out;
}
// 自写 JPEG（turbojpeg 4:4:4）。exactSize 非 0 时在 SOI 后插入 COM 段补齐。
Bytes encodeJpegOwn(const Picture& picture, std::size_t exactSize = 0) {
    require(picture.width && picture.height, "作者图片为空");
    Bytes rgb(std::size_t(picture.width) * picture.height * 3);
    for (std::size_t i = 0; i < std::size_t(picture.width) * picture.height; ++i) {
        rgb[i * 3] = picture.rgba[i * 4]; rgb[i * 3 + 1] = picture.rgba[i * 4 + 1]; rgb[i * 3 + 2] = picture.rgba[i * 4 + 2];
    }
    tjhandle handle = tjInitCompress();
    require(handle != nullptr, "turbojpeg 初始化编码器失败");
    unsigned char* data = nullptr; unsigned long size = 0;
    int status = tjCompress2(handle, rgb.data(), int(picture.width), 0, int(picture.height), TJPF_RGB, &data, &size,
                             TJSAMP_444, 92, 0);
    std::string error = status ? tjGetErrorStr2(handle) : "";
    Bytes out;
    if (!status && data) out.assign(data, data + size);
    tjFree(data); tjDestroy(handle);
    require(status == 0, "turbojpeg 编码失败: " + error);
    if (!exactSize) return out;
    require(exactSize >= out.size() + 5 && exactSize - out.size() - 4 <= 65533,
            "目标字节数无法用 COM 段补齐 JPEG");
    Bytes padded = {0xFF, 0xD8, 0xFF, 0xFE};
    std::uint16_t length = std::uint16_t(exactSize - out.size() - 4 + 2);
    padded.push_back(length >> 8); padded.push_back(length & 0xFF);
    padded.resize(padded.size() + (exactSize - out.size() - 4), 'J');
    padded.insert(padded.end(), out.begin() + 2, out.end());
    require(padded.size() == exactSize, "JPEG 补齐后字节数不符");
    return padded;
}

// 自写 24 位 BMP（底行在前），用于覆盖“非 PNG/JPEG 容器”这一边界。
Bytes encodeBmpOwn(const Picture& picture) {
    unsigned width = picture.width, height = picture.height;
    std::size_t stride = (std::size_t(width) * 3 + 3) / 4 * 4;
    std::size_t pixels = stride * height;
    Bytes out(14 + 40 + pixels, 0);
    auto put32 = [&](std::size_t at, std::uint32_t value) { std::memcpy(&out[at], &value, 4); };
    auto put16 = [&](std::size_t at, std::uint16_t value) { std::memcpy(&out[at], &value, 2); };
    out[0] = 'B'; out[1] = 'M';
    put32(2, std::uint32_t(out.size())); put32(10, 54);
    put32(14, 40); put32(18, width); put32(22, height);
    put16(26, 1); put16(28, 24); put32(34, std::uint32_t(pixels)); put32(38, 2835); put32(42, 2835);
    for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
        const unsigned char* p = picture.at(x, height - 1 - y);  // BMP 底行在前
        unsigned char* target = &out[54 + y * stride + x * 3];
        target[0] = p[2]; target[1] = p[1]; target[2] = p[0];
    }
    return out;
}

// ---------------------------------------------------------------- GL 内存布局

struct Layout {
    unsigned width = 0, height = 0;
    GLenum format = GL_RGBA;
    unsigned packing = 1;
    unsigned rowLength = 0;
    int origin = osg::Image::BOTTOM_LEFT;
};
unsigned channelsOf(GLenum format) {
    switch (format) {
        case GL_RGB: case GL_BGR: return 3;
        case GL_RGBA: case GL_BGRA: return 4;
        case GL_LUMINANCE: case GL_ALPHA: return 1;
        case GL_LUMINANCE_ALPHA: return 2;
        default: throw std::runtime_error("本文件不覆盖该格式: " + std::to_string(format));
    }
}
std::size_t strideOf(const Layout& layout) {
    std::size_t raw = std::size_t(layout.rowLength ? layout.rowLength : layout.width) * channelsOf(layout.format);
    if (layout.packing > 1) raw = (raw + layout.packing - 1) / layout.packing * layout.packing;
    return raw;
}
// 期望画面：按明确通道语义逐像素映射，alpha 缺省 255。
// 入参 source 是 GL 内存顺序（行 0 = 图片底部），返回值是显示顺序（行 0 = 图片顶部）。
Picture expectedFromSource(const Layout& layout, const Bytes& source) {
    unsigned channels = channelsOf(layout.format);
    require(source.size() == std::size_t(layout.width) * layout.height * channels,
            "源通道字节数不符: " + std::to_string(source.size()));
    Bytes rgba(std::size_t(layout.width) * layout.height * 4);
    for (std::size_t i = 0; i < std::size_t(layout.width) * layout.height; ++i) {
        const unsigned char* p = &source[i * channels];
        std::size_t row = i / layout.width, column = i % layout.width;
        unsigned char* o = &rgba[((std::size_t(layout.height) - 1 - row) * layout.width + column) * 4];
        switch (layout.format) {
            case GL_RGBA: std::copy_n(p, 4, o); break;
            case GL_BGRA: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = p[3]; break;
            case GL_RGB: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255; break;
            case GL_BGR: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;
            case GL_LUMINANCE: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;
            case GL_LUMINANCE_ALPHA: o[0] = o[1] = o[2] = p[0]; o[3] = p[1]; break;
            case GL_ALPHA: o[0] = o[1] = o[2] = 255; o[3] = p[0]; break;
            default: throw std::runtime_error("未知格式");
        }
    }
    return makePicture(layout.width, layout.height, rgba);
}
// 按布局构造 osg::Image；行填充写哨兵，误把 padding 当颜色会立刻暴露。
osg::ref_ptr<osg::Image> buildImage(const Layout& layout, const Bytes& source) {
    unsigned channels = channelsOf(layout.format);
    std::size_t stride = strideOf(layout);
    std::size_t total = stride * layout.height;
    auto buffer = new unsigned char[total];
    std::memset(buffer, 0x7B, total);
    for (unsigned y = 0; y < layout.height; ++y)
        std::copy_n(&source[std::size_t(y) * layout.width * channels], layout.width * channels, buffer + y * stride);
    osg::ref_ptr<osg::Image> image = new osg::Image;
    image->setImage(int(layout.width), int(layout.height), 1, layout.format, layout.format, GL_UNSIGNED_BYTE, buffer,
                    osg::Image::USE_NEW_DELETE, int(layout.packing), int(layout.rowLength));
    image->setOrigin(osg::Image::Origin(layout.origin));
    image->setWriteHint(osg::Image::STORE_INLINE);
    require(std::size_t(image->getRowStepInBytes()) == stride,
            "OSG 行步长 " + std::to_string(image->getRowStepInBytes()) + " 与作者假定 " + std::to_string(stride) + " 不一致");
    // 行步长可能大于行宽（rowLength/packing），分配尺寸要用 imageStep。
    require(std::size_t(image->getImageStepInBytes()) == total, "OSG 单层字节数 " +
            std::to_string(image->getImageStepInBytes()) + " 与作者假定 " + std::to_string(total) + " 不一致");
    for (unsigned y = 0; y < layout.height; ++y) for (unsigned x = 0; x < layout.width; ++x)
        require(image->data(int(x), int(y)) == buffer + y * stride + std::size_t(x) * channels,
                "OSG 像素寻址与作者布局不一致");
    return image;
}
Bytes memoryImage(const Layout& layout, const Bytes& source) {
    std::size_t stride = strideOf(layout);
    unsigned channels = channelsOf(layout.format);
    Bytes memory(stride * layout.height, 0);
    for (unsigned y = 0; y < layout.height; ++y)
        std::copy_n(&source[std::size_t(y) * layout.width * channels], layout.width * channels, memory.begin() + y * stride);
    return memory;
}

// ---------------------------------------------------------------- glTF 输出

Json glbJson(const fs::path& file) {
    Bytes bytes = readBytes(file);
    require(bytes.size() > 20, "GLB 太短: " + file.u8string());
    std::uint32_t jsonLength = 0;
    std::memcpy(&jsonLength, bytes.data() + 12, 4);
    require(bytes.size() >= 20 + std::size_t(jsonLength), "GLB JSON 长度越界");
    return Json::parse(bytes.begin() + 20, bytes.begin() + 20 + jsonLength);
}
struct Transform {
    double scaleU = 1, scaleV = 1, offsetU = 0, offsetV = 0;
    bool present = false;
};
struct OutputTexture {
    Picture picture;
    Transform transform;
    Json sampler;
    std::string mime;
    std::string uri;
    fs::path file;
    bool extensionDeclared = false;
    std::string alphaMode;
};
OutputTexture loadOutputTexture(const fs::path& glb) {
    Json doc = glbJson(glb);
    require(doc["materials"].is_array() && !doc["materials"].empty(), "GLB 没有材质: " + glb.u8string());
    // 真实瓦片可能有多材质，取第一个带纹理的材质。
    const Json* chosen = nullptr;
    for (const auto& candidate : doc["materials"]) {
        if (!candidate.contains("pbrMetallicRoughness")) continue;
        if (!candidate["pbrMetallicRoughness"].contains("baseColorTexture")) continue;
        chosen = &candidate; break;
    }
    require(chosen != nullptr, "GLB 中没有带 baseColorTexture 的材质: " + glb.u8string());
    const Json& material = *chosen;
    const Json& info = material["pbrMetallicRoughness"]["baseColorTexture"];
    const Json& texture = doc["textures"][info["index"].get<unsigned>()];
    require(texture.contains("source"), "纹理没有图片");
    OutputTexture result;
    const Json& image = doc["images"][texture["source"].get<unsigned>()];
    result.uri = image["uri"];
    result.mime = image["mimeType"];
    result.file = (glb.parent_path() / fs::u8path(result.uri)).lexically_normal();
    require(fs::is_regular_file(result.file), "输出图片不存在: " + result.file.u8string());
    result.picture = decodeContainer(readBytes(result.file), "输出图片");
    result.alphaMode = material.value("alphaMode", "OPAQUE");
    if (texture.contains("sampler")) result.sampler = doc["samplers"][texture["sampler"].get<unsigned>()];
    if (info.contains("extensions") && info["extensions"].contains("KHR_texture_transform")) {
        const Json& extension = info["extensions"]["KHR_texture_transform"];
        result.transform.present = true;
        if (extension.contains("scale")) { result.transform.scaleU = extension["scale"][0]; result.transform.scaleV = extension["scale"][1]; }
        if (extension.contains("offset")) { result.transform.offsetU = extension["offset"][0]; result.transform.offsetV = extension["offset"][1]; }
        require(extension.value("texCoord", 0) == 0, "纹理变换不应切换 UV 通道");
        require(!extension.contains("matrix"), "纹理变换不应使用 matrix");
        result.extensionDeclared = doc["extensionsRequired"].is_array() &&
            std::find(doc["extensionsRequired"].begin(), doc["extensionsRequired"].end(), "KHR_texture_transform") !=
            doc["extensionsRequired"].end();
        require(result.extensionDeclared, "使用 KHR_texture_transform 时必须列入 extensionsRequired");
    }
    return result;
}

unsigned wrapIndex(long value, unsigned size, int wrap) {
    long period = long(size);
    if (wrap == GL_REPEAT) { value %= period; if (value < 0) value += period; return unsigned(value); }
    if (wrap == GL_MIRRORED_REPEAT) {
        long m = ((value % (2 * period)) + 2 * period) % (2 * period);
        return unsigned(m < period ? m : 2 * period - 1 - m);
    }
    return unsigned(std::min<long>(std::max<long>(value, 0), period - 1));
}
// 输出侧：glTF 约定（解码图片行 0 = 顶部，v=0 指顶部）+ GLB 自己声明的变换与边界模式。
std::array<unsigned char, 4> sampleOutput(const OutputTexture& texture, double u, double v) {
    int wrapS = texture.sampler.is_null() ? GL_CLAMP_TO_EDGE : texture.sampler.value("wrapS", GL_CLAMP_TO_EDGE);
    int wrapT = texture.sampler.is_null() ? GL_CLAMP_TO_EDGE : texture.sampler.value("wrapT", GL_CLAMP_TO_EDGE);
    double uu = u * texture.transform.scaleU + texture.transform.offsetU;
    double vv = v * texture.transform.scaleV + texture.transform.offsetV;
    long x = long(std::floor(uu * texture.picture.width));
    long y = long(std::floor(vv * texture.picture.height));
    const unsigned char* p = texture.picture.at(wrapIndex(x, texture.picture.width, wrapS),
                                                wrapIndex(y, texture.picture.height, wrapT));
    return {p[0], p[1], p[2], p[3]};
}
// 源侧：OSG/GL 约定（v=0 → 内存行 0 → 图片行 H-1）。
std::array<unsigned char, 4> sampleSource(const Picture& picture, double u, double v) {
    unsigned x = wrapIndex(long(std::floor(u * picture.width)), picture.width, GL_CLAMP_TO_EDGE);
    unsigned memoryRow = wrapIndex(long(std::floor(v * picture.height)), picture.height, GL_CLAMP_TO_EDGE);
    const unsigned char* p = picture.at(x, picture.height - 1 - memoryRow);
    return {p[0], p[1], p[2], p[3]};
}
void requireRenderEquivalent(const Picture& source, const OutputTexture& output, const std::string& name) {
    // 夹具自检：两轴都不对称，否则翻转/转置错误可能被对称内容掩盖。
    require(!samePicture(source, flipVertical(source)), name + ": 夹具必须垂直不对称，否则方向断言无效");
    require(!samePicture(source, flipHorizontal(source)), name + ": 夹具必须水平不对称，否则方向断言无效");
    require(output.picture.width == source.width && output.picture.height == source.height,
            name + ": 输出尺寸 " + std::to_string(output.picture.width) + "x" + std::to_string(output.picture.height) +
            " 与源 " + std::to_string(source.width) + "x" + std::to_string(source.height) + " 不一致");
    for (unsigned y = 0; y < source.height; ++y) for (unsigned x = 0; x < source.width; ++x) {
        // 该图片点的中心 UV；v 以图片底部为零点（OSG 采样约定）。
        double u = (x + 0.5) / source.width;
        double v = ((source.height - 1 - y) + 0.5) / source.height;
        auto expected = sampleSource(source, u, v);
        const unsigned char* own = source.at(x, y);
        require(expected[0] == own[0] && expected[1] == own[1] && expected[2] == own[2] && expected[3] == own[3],
                name + ": oracle 自检失败（源采样应等于图片本身）");
        auto actual = sampleOutput(output, u, v);
        if (actual != expected)
            throw std::runtime_error(name + ": 渲染不等价。图片(" + std::to_string(x) + "," + std::to_string(y) +
                ") uv=(" + std::to_string(u) + "," + std::to_string(v) + ") 期望 " +
                quad(expected.data()) + " 实际 " + quad(actual.data()) +
                "；纹理变换" + (output.transform.present ? "已声明" : "未声明") + " mime=" + output.mime +
                " alphaMode=" + output.alphaMode);
    }
}

// ---------------------------------------------------------------- 场景构造

osg::ref_ptr<osg::Geode> planarMesh(bool withUv = true) {
    auto geometry = new osg::Geometry;
    auto points = new osg::Vec3Array;
    points->push_back({0, 0, 0}); points->push_back({1, 0, 0}); points->push_back({0, 1, 0});
    geometry->setVertexArray(points);
    if (withUv) {
        auto uv = new osg::Vec2Array;
        uv->push_back({0.25, 0.25}); uv->push_back({0.75, 0.25}); uv->push_back({0.25, 0.75});
        geometry->setTexCoordArray(0, uv);
    }
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
    auto geode = new osg::Geode; geode->addDrawable(geometry);
    return geode;
}
osg::ref_ptr<osg::Texture2D> nearestTexture(osg::Image* image) {
    auto texture = new osg::Texture2D(image);
    texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    texture->setWrap(osg::Texture::WRAP_T, osg::Texture::MIRROR);
    texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
    texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
    return texture;
}
void writeNodeOrFail(osg::Node* node, const fs::path& file, const char* hint = "IncludeData") {
    fs::create_directories(file.parent_path());
    osg::ref_ptr<osgDB::Options> options = new osgDB::Options(std::string("WriteImageHint=") + hint);
    require(osgDB::writeNodeFile(*node, file.u8string(), options.get()), "无法生成夹具: " + file.u8string());
}
osg::Texture* findTexture(osg::Node* node) {
    if (!node) return nullptr;
    if (auto state = node->getStateSet())
        if (auto texture = dynamic_cast<osg::Texture*>(state->getTextureAttribute(0, osg::StateAttribute::TEXTURE)))
            return texture;
    if (auto geode = node->asGeode())
        if (auto texture = dynamic_cast<osg::Texture*>(geode->getOrCreateStateSet()->getTextureAttribute(0, osg::StateAttribute::TEXTURE)))
            return texture;
    if (auto group = node->asGroup())
        for (unsigned i = 0; i < group->getNumChildren(); ++i)
            if (auto texture = findTexture(group->getChild(i))) return texture;
    return nullptr;
}
osg::ref_ptr<osg::Image> readBackImage(const fs::path& file) {
    auto node = osgDB::readRefNodeFile(file.u8string());
    require(node.valid(), "夹具自检: 无法回读 " + file.u8string());
    auto texture = findTexture(node.get());
    require(texture != nullptr, "夹具自检: " + file.u8string() + " 中没有纹理");
    return texture->getImage(0);
}
// 夹具自检：原始像素必须逐字节存在于 .osgb 中，否则夹具可能被悄悄压平而让用例假通过。
void requireRawPixelsInFile(const fs::path& file, const Layout& layout, const Bytes& source) {
    Bytes bytes = readBytes(file);
    Bytes memory = memoryImage(layout, source);
    require(std::search(bytes.begin(), bytes.end(), memory.begin(), memory.end()) != bytes.end(),
            "夹具自检: .osgb 中找不到作者原始像素，布局被压平: " + file.u8string());
}

// ---------------------------------------------------------------- 结果记录

enum class Status { required, refused, knownFail };
struct Tally {
    unsigned passed = 0, refused = 0, knownFail = 0;
    std::vector<std::string> notes;
};
struct FileCase {
    std::string name;
    std::string note;
    bool expectFailure = false;
    std::string reason;
    unsigned expectImages = 1;
    bool expectOriginalEncoded = false;
    bool hasBytes = false;
    Bytes expectBytes;
    std::string expectMime;
    bool hasPicture = false;
    Picture expectPicture;
    // 采样器与混合状态只在作者自建的夹具上断言；模板瓦片的采样器由原文件决定。
    bool checkSampler = true;
    bool checkBlend = true;
    // 采样器不是 NEAREST 时无法逐像素比较采样结果，改为核对输出行序与声明翻转的一致性。
    bool comparePixels = true;
    // 非 PNG/JPEG 容器的边界：只要求“逐字节透传或明确拒绝”，不允许静默换容器。
    bool passthroughOrRefusal = false;
    Status status = Status::required;
    std::string statusNote;
};

// 转换并按独立 oracle 核对；required 用例不通过直接抛异常结束。
void runFileCase(FileCase test, const fs::path& root, Tally& tally, Json& manifest, Json& observed) {
    require(test.hasPicture || test.expectFailure || test.expectImages == 0 || test.passthroughOrRefusal,
            test.name + ": 用例必须给出期望画面、期望失败、期望无图片或透传/拒绝不变量");
    Options options;
    options.input = root / test.name / (test.name + ".osgb");
    options.output = root / ("out-" + test.name);
    options.srs = "ENU:31.2,121.4,10";
    fs::remove_all(options.output);
    require(fs::is_regular_file(options.input), "夹具缺失: " + options.input.u8string());
    bool failed = false; std::string error; Json report;
    try { report = convert(options); } catch (const std::exception& e) { failed = true; error = e.what(); }
    observed.push_back({{"name", test.name}, {"note", test.note}, {"failed", failed}, {"error", error}});
    manifest.push_back({{"name", test.name}, {"note", test.note}, {"expectFailure", test.expectFailure},
        {"reason", test.reason}, {"expectOriginalEncoded", test.expectOriginalEncoded},
        {"expectMime", test.expectMime}, {"hasPicture", test.hasPicture},
        {"expectPicture", test.hasPicture ? Json{{"width", test.expectPicture.width},
            {"height", test.expectPicture.height},
            // 小图直接记录像素便于复核；大图只记尺寸，避免清单膨胀。
            {"rgba", test.expectPicture.rgba.size() <= 4096 ? Json(test.expectPicture.rgba) : Json(nullptr)}} : Json(nullptr)},
        {"status", int(test.status)}, {"statusNote", test.statusNote}});

    auto reject = [&](const std::string& message) {
        if (test.status == Status::knownFail) {
            ++tally.knownFail;
            std::cout << "[已知失败] " << test.name << ": " << message << "  ← 原因: " << test.statusNote << '\n';
            tally.notes.push_back("已知失败 " + test.name + ": " + test.statusNote);
            return;
        }
        std::cout << "[FAIL] " << test.name << ": " << message << '\n';
        throw std::runtime_error(test.name + ": " + message);
    };
    if (test.passthroughOrRefusal) {
        if (failed) {
            ++tally.passed;
            std::cout << "[ ok ] " << test.name << "  明确拒绝: " << error << '\n';
            return;
        }
        try {
            fs::path glb;
            for (const auto& record : report["records"]) for (const auto& content : record["content"])
                if (!content["images"].empty()) glb = options.output / content["uri"].get<std::string>();
            require(!glb.empty(), "应有带图片的 GLB 输出");
            auto output = loadOutputTexture(glb);
            Bytes actual = readBytes(output.file);
            require(actual == test.expectBytes, "静默替换了原容器：输出 " + std::to_string(actual.size()) +
                " 字节，源 " + std::to_string(test.expectBytes.size()) + " 字节（既未透传也未明确拒绝）");
        } catch (const std::exception& e) { reject(e.what()); return; }
        ++tally.passed;
        std::cout << "[ ok ] " << test.name << "  原容器逐字节透传\n";
        return;
    }
    if (test.expectFailure) {
        if (!failed) { reject("预期失败但转换成功"); return; }
        if (fs::exists(options.output)) { reject("失败后仍发布了输出目录"); return; }
        std::string reason = test.reason.empty() ? "" : test.reason;
        if (!reason.empty() && error.find(reason) == std::string::npos) {
            reject("失败原因未说明: " + error + "（期望包含 " + reason + "）"); return;
        }
        require(error.find(test.name + ".osgb") != std::string::npos || error.find(options.input.filename().u8string()) != std::string::npos,
                "失败必须标出源 OSGB 文件: " + error);
        if (test.status == Status::refused) {
            ++tally.refused;
            std::cout << "[拒绝] " << test.name << ": 明确拒绝（" << reason << "）—— 未建立等价输出，不计为兼容完成\n";
            tally.notes.push_back("拒绝 " + test.name + ": " + reason);
        } else {
            ++tally.passed;
            std::cout << "[ ok ] " << test.name << "  明确失败: " << error << '\n';
        }
        return;
    }
    if (failed) { reject("预期成功但失败: " + error); return; }
    try {
        if (test.expectImages == 0) {
            require(report["imageReferences"] == 0, "关闭纹理后不应产生图片引用");
            require(report["uniqueImages"] == 0, "关闭纹理后不应写出图片文件");
            ++tally.passed;
            std::cout << "[ ok ] " << test.name << '\n';
            return;
        }
        require(report["imageReferences"] == 1, "应有 1 个图片引用，实际 " + report["imageReferences"].dump());
        // 入口文件可能同时带分页子瓦片，按内容逐个找带图片的那个 GLB。
        Json audit;
        fs::path glb;
        unsigned seen = 0;
        for (const auto& record : report["records"]) for (const auto& content : record["content"]) {
            if (content["images"].empty()) continue;
            ++seen;
            audit = content["images"][0];
            glb = options.output / content["uri"].get<std::string>();
        }
        require(seen == 1, "应恰好有 1 个带图片的 GLB 内容，实际 " + std::to_string(seen));
        require(audit.is_object(), "报告缺少图片审计");
        auto output = loadOutputTexture(glb);
        if (test.hasBytes) {
            Bytes actual = readBytes(output.file);
            require(actual == test.expectBytes, "原编码未逐字节透传：输出 " + std::to_string(actual.size()) +
                " 字节，源 " + std::to_string(test.expectBytes.size()) + " 字节");
            require(!test.expectMime.empty() && output.mime == test.expectMime, "mime 与源容器不一致: " + output.mime);
            require(audit["originalEncoded"].get<bool>(), "审计未标记为原编码透传");
            require(audit["bytes"].get<std::size_t>() == test.expectBytes.size(), "审计记录的图片字节数不符");
        } else {
            require(audit["originalEncoded"].get<bool>() == test.expectOriginalEncoded,
                    "审计的原编码标记不符");
            require(output.mime == "image/png", "原始像素必须无损写成 PNG，实际 " + output.mime);
        }
        if (test.checkBlend) require(output.alphaMode == "BLEND", "不能丢弃透明混合状态");
        if (test.checkSampler) {
            require(output.sampler.value("magFilter", 0) == int(GL_NEAREST) &&
                    output.sampler.value("minFilter", 0) == int(GL_NEAREST), "采样过滤被改写");
            require(output.sampler.value("wrapS", 0) == int(GL_CLAMP_TO_EDGE), "wrapS 被改写");
            require(output.sampler.value("wrapT", 0) == int(GL_MIRRORED_REPEAT), "wrapT 被改写");
        }
        if (test.comparePixels) {
            requireRenderEquivalent(test.expectPicture, output, test.name);
        } else {
            // PNG/JPEG 容器一律顶行在前（格式定义），因此必须声明垂直翻转；否则渲染会上下颠倒。
            require(output.transform.present && output.transform.scaleV == -1 && output.transform.offsetV == 1,
                    std::string("容器字节顶行在前时必须声明垂直翻转，实际 ") +
                    (output.transform.present ? "已声明但不是 (scale=-1, offset=1)" : "未声明"));
            require(samePicture(output.picture, test.expectPicture),
                    "输出图片行序必须与源容器一致（顶行在前）");
        }
    } catch (const std::exception& e) { reject(e.what()); return; }
    ++tally.passed;
    std::cout << "[ ok ] " << test.name << (test.note.empty() ? "" : "  (" + test.note + ")") << '\n';
}

// ---------------------------------------------------------------- 模板内嵌原编码

struct EmbeddedTemplate {
    Bytes bytes;
    std::size_t payloadStart = 0, payloadLength = 0;
    std::size_t nameStart = 0, nameLength = 0;
};
EmbeddedTemplate loadTemplate(const fs::path& file) {
    EmbeddedTemplate result;
    result.bytes = readBytes(file);
    const Bytes& bytes = result.bytes;
    for (std::size_t i = 0; i + 2 < bytes.size(); ++i)
        if (bytes[i] == 0xFF && bytes[i + 1] == 0xD8 && bytes[i + 2] == 0xFF) { result.payloadStart = i; break; }
    require(result.payloadStart != 0, "模板 OSGB 中没有内嵌 JPEG: " + file.u8string());
    for (std::size_t i = bytes.size() - 2; i > result.payloadStart; --i)
        if (bytes[i] == 0xFF && bytes[i + 1] == 0xD9) { result.payloadLength = i + 2 - result.payloadStart; break; }
    require(result.payloadLength > 4, "模板 OSGB 内嵌 JPEG 没有结束标记");
    // 内嵌图片名不紧贴数据（中间还有长度/类型字段），在数据之前找最后一个以 .jpg 结尾的可打印串。
    std::size_t bestStart = 0, bestLength = 0;
    for (std::size_t i = 0; i < result.payloadStart;) {
        if (bytes[i] < 0x20 || bytes[i] >= 0x7F) { ++i; continue; }
        std::size_t start = i;
        while (i < result.payloadStart && bytes[i] >= 0x20 && bytes[i] < 0x7F) ++i;
        std::string candidate(bytes.begin() + start, bytes.begin() + i);
        if (candidate.size() > 4 && candidate.compare(candidate.size() - 4, 4, ".jpg") == 0) {
            bestStart = start; bestLength = candidate.size();
        }
    }
    require(bestLength > 0, "模板 OSGB 中找不到内嵌 .jpg 图片名: " + file.u8string());
    result.nameStart = bestStart; result.nameLength = bestLength;
    return result;
}
Bytes patchContainerInto(const EmbeddedTemplate& tpl, const Bytes& container, bool toPng) {
    require(container.size() == tpl.payloadLength, "作者容器长度 " + std::to_string(container.size()) +
            " 必须等于模板内嵌长度 " + std::to_string(tpl.payloadLength));
    Bytes out = tpl.bytes;
    std::copy(container.begin(), container.end(), out.begin() + tpl.payloadStart);
    if (toPng) std::copy_n("png", 3, out.begin() + tpl.nameStart + tpl.nameLength - 3);
    return out;
}
}

int main(int argc, char** argv) {
    try {
        // 默认写到系统临时目录，避免在源码/构建目录留下产物；--out 可指定位置以便取证。
        fs::path root = fs::temp_directory_path() / ("texture-oracle-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        bool strict = false;
        for (int i = 1; i < argc; ++i) {
            std::string argument = argv[i];
            if (argument == "--out" && i + 1 < argc) root = fs::u8path(argv[++i]);
            else if (argument == "--strict") strict = true;
            else throw std::runtime_error("未知参数: " + argument);
        }
        fs::remove_all(root);
        fs::create_directories(root);
        Tally tally;
        Json manifest = Json::array(), observed = Json::array();

        // ============ oracle 反向自检：故意造错误，必须被发现 ============
        // 若比较逻辑本身失效（例如永远通过），下面的用例会失败，从而避免“全绿但无意义”。
        {
            Picture truth = makePicture(3, 2, Bytes{
                10, 20, 30, 0, 40, 50, 60, 128, 70, 80, 90, 255,
                100, 110, 120, 64, 130, 140, 150, 200, 160, 170, 180, 1});
            auto expectCaught = [&](const std::string& label, const Picture& candidate, bool flip) {
                OutputTexture wrong;
                wrong.picture = candidate;
                wrong.mime = "image/png";
                wrong.sampler = Json{{"wrapS", GL_CLAMP_TO_EDGE}, {"wrapT", GL_MIRRORED_REPEAT},
                                     {"magFilter", GL_NEAREST}, {"minFilter", GL_NEAREST}};
                wrong.transform.present = flip;
                if (flip) { wrong.transform.scaleV = -1; wrong.transform.offsetV = 1; }
                bool caught = false; std::string message;
                try { requireRenderEquivalent(truth, wrong, "oracle 自检"); }
                catch (const std::exception& e) { caught = true; message = e.what(); }
                require(caught, "oracle 自检失败：" + label + " 未被发现，比较逻辑可能失效");
                ++tally.passed;
                std::cout << "[ ok ] oracle/" << label << "  错误已被发现\n";
                observed.push_back({{"layer", "oracle"}, {"name", label}, {"result", "caught"}, {"detection", message}});
            };
            // 原始像素路径的 PNG 行序等于 GL 内存序（图片行序的垂直翻转），因此
            // “行序未翻转却不声明翻转”和“行序已翻转却声明翻转”都是错误。
            expectCaught("行序未翻转且未声明翻转", truth, false);
            expectCaught("行序已翻转却声明翻转", flipVertical(truth), true);
            Picture zeroedAlpha = truth;
            for (std::size_t i = 0; i < zeroedAlpha.rgba.size(); i += 4)
                if (zeroedAlpha.rgba[i + 3] == 0) { zeroedAlpha.rgba[i] = zeroedAlpha.rgba[i + 1] = zeroedAlpha.rgba[i + 2] = 0; }
            expectCaught("全透明像素被清成黑色", zeroedAlpha, false);
            Picture premultiplied = truth;
            for (std::size_t i = 0; i < premultiplied.rgba.size(); i += 4) {
                unsigned alpha = premultiplied.rgba[i + 3];
                for (unsigned c = 0; c < 3; ++c)
                    premultiplied.rgba[i + c] = static_cast<unsigned char>(premultiplied.rgba[i + c] * alpha / 255);
            }
            expectCaught("alpha 被预乘", premultiplied, false);
            Picture swapped = truth;
            for (std::size_t i = 0; i < swapped.rgba.size(); i += 4) std::swap(swapped.rgba[i], swapped.rgba[i + 2]);
            expectCaught("红蓝通道被交换", swapped, false);
        }

        // ============ 第一层：writeGlb 直调，完全不经过 OSG 图片插件 ============
        struct PixelCase { std::string name; Layout layout; Bytes source; std::string note; };
        std::vector<PixelCase> pixelCases;
        {
            Layout layout; layout.width = 3; layout.height = 3; layout.format = GL_RGBA;
            pixelCases.push_back({"rgba-alpha-extremes", layout,
                {200, 10, 20, 0, 130, 140, 150, 1, 1, 2, 3, 255,
                 40, 50, 60, 64, 255, 255, 255, 128, 7, 8, 9, 200,
                 0, 0, 0, 255, 90, 91, 92, 128, 5, 6, 7, 1},
                "alpha 0/1/64/128/200/255，全透明像素保留 RGB"});
            Layout rgbOdd; rgbOdd.width = 5; rgbOdd.height = 3; rgbOdd.format = GL_RGB;
            Bytes rgbOddSource;
            for (unsigned y = 0; y < 3; ++y) for (unsigned x = 0; x < 5; ++x)
                rgbOddSource.insert(rgbOddSource.end(), {static_cast<unsigned char>(10 + x * 3 + y),
                    static_cast<unsigned char>(200 - x * 7), static_cast<unsigned char>(x * 11 + y * 40)});
            pixelCases.push_back({"rgb-odd-width", rgbOdd, rgbOddSource, "5x3 RGB packing=1"});
            Layout packed = rgbOdd; packed.height = 2; packed.packing = 4;
            Bytes packedSource;
            for (unsigned y = 0; y < 2; ++y) for (unsigned x = 0; x < 5; ++x)
                packedSource.insert(packedSource.end(), {static_cast<unsigned char>(x * 30 + y),
                    static_cast<unsigned char>(255 - x * 40), static_cast<unsigned char>(x * 5 + y * 100)});
            pixelCases.push_back({"rgb-packing4-odd", packed, packedSource, "行填充不得被当成颜色"});
            Layout bgr; bgr.width = 3; bgr.height = 2; bgr.format = GL_BGR;
            pixelCases.push_back({"bgr", bgr, {1, 2, 3, 40, 50, 60, 70, 80, 90, 255, 128, 64, 9, 8, 7, 11, 22, 33}, "通道序"});
            Layout bgra = bgr; bgra.format = GL_BGRA;
            pixelCases.push_back({"bgra", bgra, {1, 2, 3, 0, 40, 50, 60, 64, 70, 80, 90, 128,
                                                255, 128, 64, 255, 9, 8, 7, 200, 11, 22, 33, 1}, "通道序 + alpha"});
            Layout luma; luma.width = 4; luma.height = 2; luma.format = GL_LUMINANCE;
            pixelCases.push_back({"luminance", luma, {0, 17, 128, 255, 3, 90, 200, 41}, "灰度"});
            Layout lumaAlpha = luma; lumaAlpha.format = GL_LUMINANCE_ALPHA;
            pixelCases.push_back({"luminance-alpha", lumaAlpha, {0, 0, 64, 1, 128, 64, 255, 128,
                                                                9, 200, 41, 255, 200, 3, 7, 99}, "灰度 alpha 逐像素映射"});
            Layout alphaOnly; alphaOnly.width = 3; alphaOnly.height = 2; alphaOnly.format = GL_ALPHA;
            pixelCases.push_back({"alpha-only", alphaOnly, {0, 64, 128, 200, 255, 1}, "纯 alpha"});
            Layout rowLength; rowLength.width = 3; rowLength.height = 2; rowLength.format = GL_RGB; rowLength.rowLength = 7;
            pixelCases.push_back({"rgb-row-length", rowLength,
                {255, 1, 1, 2, 255, 2, 3, 3, 255, 9, 9, 9, 4, 4, 4, 9, 9, 9}, "rowLength=7 宽度=3"});
            Layout tall; tall.width = 3; tall.height = 5; tall.format = GL_RGBA;
            Bytes tallSource;
            for (unsigned y = 0; y < 5; ++y) for (unsigned x = 0; x < 3; ++x)
                tallSource.insert(tallSource.end(), {static_cast<unsigned char>(20 * x + y),
                    static_cast<unsigned char>(10 + 30 * y), static_cast<unsigned char>(200 - x),
                    static_cast<unsigned char>(y % 2 ? 128 : 255)});
            pixelCases.push_back({"rgba-3x5", tall, tallSource, "非方形，可发现转置"});
            // origin 只是存储顺序元数据；OSG 采样始终是 t=0 → 内存行 0，因此带 TOP_LEFT 标记的
            // 同样内存字节必须渲染成同一画面。
            Layout topLeft; topLeft.width = 3; topLeft.height = 2; topLeft.format = GL_RGBA;
            topLeft.origin = osg::Image::TOP_LEFT;
            pixelCases.push_back({"rgba-origin-top-left", topLeft,
                {30, 60, 90, 0, 120, 150, 180, 64, 210, 240, 15, 255,
                 45, 75, 105, 128, 135, 165, 195, 200, 225, 255, 30, 1},
                "origin=TOP_LEFT 不改变渲染结果"});
        }
        for (const auto& test : pixelCases) {
            auto image = buildImage(test.layout, test.source);
            auto geode = planarMesh();
            auto state = geode->getOrCreateStateSet();
            state->setTextureAttributeAndModes(0, nearestTexture(image.get()), osg::StateAttribute::ON);
            state->setMode(GL_BLEND, osg::StateAttribute::ON);
            fs::path directory = root / ("pixels-" + test.name);
            fs::path glb = directory / "content" / (test.name + ".glb");
            fs::create_directories(glb.parent_path());
            GeometryInput input; input.geometry = geode->getDrawable(0)->asGeometry(); input.state = state;
            ContentResult result = writeGlb({input}, glb, directory / "textures", true, nullptr, false);
            require(result.images == 1, test.name + ": 应写出 1 张图片");
            require(result.imageAudit[0]["originalEncoded"].get<bool>() == false,
                    test.name + ": 原始像素不应被当成原编码图片");
            auto output = loadOutputTexture(glb);
            require(output.mime == "image/png", test.name + ": 原始像素必须无损写成 PNG，实际 " + output.mime);
            require(output.picture.width == test.layout.width && output.picture.height == test.layout.height,
                    test.name + ": 输出图片尺寸不符");
            require(output.alphaMode == "BLEND", test.name + ": 不能丢弃透明混合状态");
            require(output.sampler.value("magFilter", 0) == int(GL_NEAREST) &&
                    output.sampler.value("minFilter", 0) == int(GL_NEAREST), test.name + ": 采样过滤被改写");
            Picture expected = expectedFromSource(test.layout, test.source);
            requireRenderEquivalent(expected, output, test.name);
            manifest.push_back({{"layer", "pixels"}, {"name", test.name}, {"note", test.note},
                {"layout", {{"width", test.layout.width}, {"height", test.layout.height},
                            {"format", test.layout.format}, {"packing", test.layout.packing},
                            {"rowLength", test.layout.rowLength}, {"origin", test.layout.origin}}},
                {"expectedRgba", expected.rgba}});
            observed.push_back({{"layer", "pixels"}, {"name", test.name}, {"result", "passed"},
                                {"mime", output.mime}, {"flip", output.transform.present}});
            ++tally.passed;
            std::cout << "[ ok ] pixels/" << test.name << (test.note.empty() ? "" : "  (" + test.note + ")") << '\n';
        }

        // ============ 第一层：明确拒绝的格式（拒绝不等于兼容完成） ============
        struct RefusalCase {
            std::string name, reason, note;
            std::function<osg::ref_ptr<osg::Image>()> build;
        };
        std::vector<RefusalCase> refusals = {
            {"custom-mip-chain", "mip", "内嵌自定义 mip 链，当前 JPEG/PNG 路径无法等价保留", [] {
                auto image = new osg::Image;
                auto bytes = new unsigned char[20];
                for (int i = 0; i < 20; ++i) bytes[i] = static_cast<unsigned char>(i * 11);
                image->setImage(2, 2, 1, GL_RGBA, GL_RGBA, GL_UNSIGNED_BYTE, bytes, osg::Image::USE_NEW_DELETE, 1);
                image->setMipmapLevels({16});
                image->setWriteHint(osg::Image::STORE_INLINE);
                image->setFileName("fixture-custom-mip.png");
                return osg::ref_ptr<osg::Image>(image);
            }},
            {"dxt1-compressed", "拒绝有损替代", "BC1/DXT1 压缩纹理尚无独立 oracle 的等价输出", [] {
                auto image = new osg::Image;
                auto blocks = new unsigned char[8];
                std::memset(blocks, 0, 8); blocks[4] = 255;
                image->setImage(4, 4, 1, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT,
                                GL_UNSIGNED_BYTE, blocks, osg::Image::USE_NEW_DELETE, 1);
                image->setWriteHint(osg::Image::STORE_INLINE);
                image->setFileName("fixture-dxt1.dds");
                return osg::ref_ptr<osg::Image>(image);
            }},
            {"rgb-float-pixels", "拒绝有损替代", "16 位/浮点原始像素尚无逐像素等价映射", [] {
                auto image = new osg::Image;
                image->allocateImage(2, 2, 1, GL_RGB, GL_FLOAT, 1);
                std::memset(image->data(), 0, image->getTotalSizeInBytes());
                image->setWriteHint(osg::Image::STORE_INLINE);
                image->setFileName("fixture-float.tif");
                return osg::ref_ptr<osg::Image>(image);
            }}};
        for (auto& test : refusals) {
            auto image = test.build();
            auto geode = planarMesh();
            geode->getOrCreateStateSet()->setTextureAttributeAndModes(0, nearestTexture(image.get()), osg::StateAttribute::ON);
            fs::path directory = root / ("refuse-" + test.name);
            fs::create_directories(directory);
            GeometryInput input; input.geometry = geode->getDrawable(0)->asGeometry();
            input.state = geode->getOrCreateStateSet();
            bool threw = false; std::string error;
            try { writeGlb({input}, directory / (test.name + ".glb"), directory, true, nullptr, false); }
            catch (const std::exception& e) { threw = true; error = e.what(); }
            require(threw, test.name + ": 未建立等价输出时必须明确拒绝，而不是静默替代");
            require(error.find(test.reason) != std::string::npos,
                    test.name + ": 拒绝原因未说明，实际: " + error);
            require(!fs::exists(directory / (test.name + ".glb")), test.name + ": 拒绝后不应写出 GLB");
            manifest.push_back({{"layer", "pixels"}, {"name", test.name}, {"note", test.note},
                                {"refused", true}, {"reason", error}});
            observed.push_back({{"layer", "pixels"}, {"name", test.name}, {"result", "refused"}, {"error", error}});
            ++tally.refused;
            tally.notes.push_back("拒绝 " + test.name + ": " + test.note);
            std::cout << "[拒绝] pixels/" << test.name << ": " << error
                      << "  —— 未建立等价输出，不计为兼容完成\n";
        }

        // ============ 第二层：convert() 文件级 ============
        const fs::path templateFile = fs::u8path(TEST_OSGB_TEMPLATE);
        require(fs::is_regular_file(templateFile), "找不到夹具模板: " + templateFile.u8string());
        EmbeddedTemplate embedded = loadTemplate(templateFile);
        const std::string siblingName = "Tile_+000_+000_L16_0.osgb";
        require(std::string(embedded.bytes.begin(), embedded.bytes.end()).find(siblingName) != std::string::npos,
                "模板 OSGB 中缺少预期的分页引用 " + siblingName);

        // 作者原编码图片：块状图案，两轴不对称，含全透明与半透明像素。
        Picture authored;
        {
            const unsigned width = 256, height = 128;
            Bytes rgba(std::size_t(width) * height * 4);
            const unsigned char palette[8][3] = {{200, 10, 20}, {130, 140, 150}, {1, 2, 3}, {255, 255, 255},
                                                 {0, 0, 0}, {40, 50, 60}, {7, 8, 9}, {90, 91, 92}};
            const unsigned char alphas[8] = {0, 1, 64, 128, 255, 128, 64, 1};
            for (unsigned y = 0; y < height; ++y) for (unsigned x = 0; x < width; ++x) {
                unsigned blockX = x / 16, blockY = y / 16;
                const unsigned char* color = palette[(blockX * 3 + blockY * 5) % 8];
                unsigned char* pixel = &rgba[(std::size_t(y) * width + x) * 4];
                pixel[0] = color[0]; pixel[1] = color[1]; pixel[2] = color[2];
                pixel[3] = alphas[(blockX + blockY * 2) % 8];
            }
            authored = makePicture(width, height, rgba);
        }
        Bytes authoredPng = encodePngOwn(authored, embedded.payloadLength);
        Bytes authoredJpeg = encodeJpegOwn(authored, embedded.payloadLength);
        require(samePicture(decodePngOwn(authoredPng, "作者 PNG 自检"), authored),
                "作者 PNG 自检失败：独立解码与作者图片不一致");
        {
            Picture jpeg = decodeJpegOwn(authoredJpeg, "作者 JPEG 自检");
            require(jpeg.width == authored.width && jpeg.height == authored.height, "作者 JPEG 自检失败：尺寸不符");
        }

        // --- 原编码透传：模板内嵌（readImage(stream) 捕获路径） ---
        for (bool asPng : {true, false}) {
            std::string name = asPng ? "embedded-png" : "embedded-jpeg";
            fs::path directory = root / name;
            fs::create_directories(directory);
            Bytes container = asPng ? authoredPng : authoredJpeg;
            atomicWrite(directory / (name + ".osgb"), patchContainerInto(embedded, container, asPng));
            writeNodeOrFail(planarMesh(false), directory / siblingName);
            Bytes found = readBytes(directory / (name + ".osgb"));
            require(std::search(found.begin(), found.end(), container.begin(), container.end()) != found.end(),
                    "夹具自检: 补丁后 .osgb 中找不到作者容器字节");
            FileCase test;
            test.name = name;
            test.note = asPng ? "内嵌作者 PNG 原字节透传" : "内嵌作者 JPEG 原字节透传";
            test.hasBytes = true; test.expectBytes = container;
            test.expectMime = asPng ? "image/png" : "image/jpeg";
            test.expectOriginalEncoded = true;
            test.expectPicture = asPng ? authored : decodeJpegOwn(container, "作者 JPEG");
            test.hasPicture = true;
            // 模板瓦片的采样器与混合状态来自原文件（LINEAR + mip），无法用 NEAREST 逐像素采样，
            // 改为核对“容器字节顶行在前 → 必须声明垂直翻转”。
            test.checkSampler = false;
            test.checkBlend = false;
            test.comparePixels = false;
            runFileCase(test, root, tally, manifest, observed);
        }
        // --- 原编码透传：外部引用（readImage(name) 捕获路径） ---
        for (bool asPng : {true, false}) {
            std::string name = asPng ? "external-png" : "external-jpeg";
            fs::path directory = root / name;
            fs::create_directories(directory);
            Bytes container = asPng ? encodePngOwn(authored) : encodeJpegOwn(authored);
            fs::path textureFile = directory / (asPng ? "authored.png" : "authored.jpg");
            atomicWrite(textureFile, container);
            auto image = new osg::Image;
            image->allocateImage(int(authored.width), int(authored.height), 1, GL_RGBA, GL_UNSIGNED_BYTE, 1);
            std::memcpy(image->data(), authored.rgba.data(), authored.rgba.size());
            image->setFileName(fs::absolute(textureFile).u8string());
            image->setWriteHint(osg::Image::EXTERNAL_FILE);
            auto geode = planarMesh();
            geode->getOrCreateStateSet()->setTextureAttributeAndModes(0, nearestTexture(image), osg::StateAttribute::ON);
            geode->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
            writeNodeOrFail(geode, directory / (name + ".osgb"), "External");
            FileCase test;
            test.name = name;
            test.note = "外部引用作者原编码文件逐字节透传";
            test.hasBytes = true; test.expectBytes = container;
            test.expectMime = asPng ? "image/png" : "image/jpeg";
            test.expectOriginalEncoded = true;
            test.expectPicture = asPng ? authored : decodeJpegOwn(container, "作者 JPEG");
            test.hasPicture = true;
            runFileCase(test, root, tally, manifest, observed);
        }

        // --- 原编码透传：外部引用使用相对文件名（真实 OSGB 的常见写法） ---
        {
            std::string name = "external-relative-name";
            fs::path directory = root / name;
            fs::create_directories(directory);
            Bytes container = encodePngOwn(authored);
            atomicWrite(directory / "relative.png", container);
            auto image = new osg::Image;
            image->allocateImage(int(authored.width), int(authored.height), 1, GL_RGBA, GL_UNSIGNED_BYTE, 1);
            std::memcpy(image->data(), authored.rgba.data(), authored.rgba.size());
            // 只写文件名，不带目录：必须按 .osgb 所在目录解析，否则会退回重新编码。
            image->setFileName("relative.png");
            image->setWriteHint(osg::Image::EXTERNAL_FILE);
            auto geode = planarMesh();
            geode->getOrCreateStateSet()->setTextureAttributeAndModes(0, nearestTexture(image), osg::StateAttribute::ON);
            geode->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
            writeNodeOrFail(geode, directory / (name + ".osgb"), "External");
            FileCase test;
            test.name = name;
            test.note = "外部引用相对文件名仍应原字节透传";
            test.hasBytes = true; test.expectBytes = container;
            test.expectMime = "image/png";
            test.expectOriginalEncoded = true;
            test.expectPicture = authored;
            test.hasPicture = true;
            runFileCase(test, root, tally, manifest, observed);
        }

        // --- 非 PNG/JPEG 容器：必须透传或明确拒绝，不能静默换容器 ---
        {
            std::string name = "external-bmp-container";
            fs::path directory = root / name;
            fs::create_directories(directory);
            Bytes container = encodeBmpOwn(authored);
            atomicWrite(directory / "authored.bmp", container);
            auto image = new osg::Image;
            image->allocateImage(int(authored.width), int(authored.height), 1, GL_RGBA, GL_UNSIGNED_BYTE, 1);
            std::memcpy(image->data(), authored.rgba.data(), authored.rgba.size());
            image->setFileName(fs::absolute(directory / "authored.bmp").u8string());
            image->setWriteHint(osg::Image::EXTERNAL_FILE);
            auto geode = planarMesh();
            geode->getOrCreateStateSet()->setTextureAttributeAndModes(0, nearestTexture(image), osg::StateAttribute::ON);
            geode->getOrCreateStateSet()->setMode(GL_BLEND, osg::StateAttribute::ON);
            writeNodeOrFail(geode, directory / (name + ".osgb"), "External");
            FileCase test;
            test.name = name;
            test.note = "BMP 容器：要么透传，要么明确拒绝，不能静默换容器";
            test.passthroughOrRefusal = true;
            test.expectBytes = container;
            test.status = Status::required;
            runFileCase(test, root, tally, manifest, observed);
        }

        // --- 原始像素经 convert()：作者布局 → 期望画面 ---
        auto addRawFile = [&](const std::string& name, const Layout& layout, const Bytes& source,
                              const std::string& note, bool parentState = false) {
            fs::path directory = root / name;
            fs::create_directories(directory);
            auto image = buildImage(layout, source);
            auto geode = planarMesh();
            osg::ref_ptr<osg::Node> node = geode;
            if (parentState) {
                auto group = new osg::Group; group->addChild(geode); node = group;
                auto state = group->getOrCreateStateSet();
                state->setTextureAttributeAndModes(0, nearestTexture(image.get()), osg::StateAttribute::ON);
                state->setMode(GL_BLEND, osg::StateAttribute::ON);
            } else {
                auto state = geode->getOrCreateStateSet();
                state->setTextureAttributeAndModes(0, nearestTexture(image.get()), osg::StateAttribute::ON);
                state->setMode(GL_BLEND, osg::StateAttribute::ON);
            }
            fs::path file = directory / (name + ".osgb");
            writeNodeOrFail(node, file);
            requireRawPixelsInFile(file, layout, source);
            auto roundTrip = readBackImage(file);
            require(roundTrip.valid(), name + ": 夹具回读图片为空");
            require(roundTrip->getPixelFormat() == layout.format,
                    name + ": .osgb 往返改变了像素格式 " + std::to_string(roundTrip->getPixelFormat()));
            require(unsigned(roundTrip->getPacking()) == layout.packing, name + ": .osgb 往返改变了 packing");
            require(roundTrip->getOrigin() == osg::Image::Origin(layout.origin), name + ": .osgb 往返改变了 origin");
            FileCase test;
            test.name = name; test.note = note;
            test.expectPicture = expectedFromSource(layout, source);
            test.hasPicture = true;
            runFileCase(test, root, tally, manifest, observed);
        };
        {
            Layout layout; layout.width = 3; layout.height = 3; layout.format = GL_RGBA;
            Bytes source = {200, 10, 20, 0, 130, 140, 150, 1, 1, 2, 3, 255,
                            40, 50, 60, 64, 255, 255, 255, 128, 7, 8, 9, 200,
                            0, 0, 0, 255, 90, 91, 92, 128, 5, 6, 7, 1};
            addRawFile("rgba-alpha-extremes", layout, source, "原始 RGBA 经 convert() 保留 alpha");
            addRawFile("parent-state-texture", layout, source, "纹理只在父节点 StateSet", true);
            Layout bgra = layout; bgra.format = GL_BGRA;
            Bytes sourceBgra;
            for (std::size_t i = 0; i < source.size(); i += 4)
                sourceBgra.insert(sourceBgra.end(), {source[i + 2], source[i + 1], source[i], source[i + 3]});
            addRawFile("bgra", bgra, sourceBgra, "BGRA 通道序");
            Layout grayAlpha; grayAlpha.width = 4; grayAlpha.height = 2; grayAlpha.format = GL_LUMINANCE_ALPHA;
            addRawFile("luminance-alpha", grayAlpha,
                {0, 0, 64, 1, 128, 64, 255, 128, 9, 200, 41, 255, 200, 3, 7, 99}, "灰度 alpha");
            // 行填充（packing>1）只能在第一层直调 writeGlb 验证：实测 .osgb 序列化会按紧凑行
            // 写出像素、却仍记录 packing，往返后图像行步长与文档长度不一致（OSG 侧问题），
            // 因此文件层改用 packing=1 的奇数宽度。
            Layout oddWidth; oddWidth.width = 5; oddWidth.height = 3; oddWidth.format = GL_RGB;
            Bytes sourceOdd;
            for (unsigned y = 0; y < 3; ++y) for (unsigned x = 0; x < 5; ++x)
                sourceOdd.insert(sourceOdd.end(), {static_cast<unsigned char>(10 + x * 3 + y),
                    static_cast<unsigned char>(200 - x * 7), static_cast<unsigned char>(x * 11 + y * 40)});
            addRawFile("rgb-odd-width", oddWidth, sourceOdd, "奇数宽度 RGB");
        }

        // --- 状态语义：显式关闭纹理 ---
        {
            fs::path directory = root / "disabled-texture";
            fs::create_directories(directory);
            Layout layout; layout.width = 2; layout.height = 2; layout.format = GL_RGBA;
            Bytes source = {1, 2, 3, 255, 4, 5, 6, 128, 7, 8, 9, 0, 10, 11, 12, 64};
            auto image = buildImage(layout, source);
            auto geode = planarMesh();
            geode->getOrCreateStateSet()->setTextureAttributeAndModes(0, nearestTexture(image.get()),
                                                                     osg::StateAttribute::OFF);
            writeNodeOrFail(geode, directory / "disabled-texture.osgb");
            FileCase test; test.name = "disabled-texture";
            test.note = "显式关闭的有效纹理不得重新启用";
            test.expectImages = 0;
            runFileCase(test, root, tally, manifest, observed);
        }
        {
            // 规格要求：显式关闭纹理时不应因未使用的坏图片失败。
            fs::path directory = root / "disabled-broken-external";
            fs::create_directories(directory);
            auto image = new osg::Image;
            image->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, 1);
            std::memset(image->data(), 9, 16);
            image->setFileName(fs::absolute(directory / "absent.png").u8string());
            image->setWriteHint(osg::Image::EXTERNAL_FILE);
            auto geode = planarMesh();
            geode->getOrCreateStateSet()->setTextureAttributeAndModes(0, nearestTexture(image),
                                                                     osg::StateAttribute::OFF);
            writeNodeOrFail(geode, directory / "disabled-broken-external.osgb", "External");
            FileCase test; test.name = "disabled-broken-external";
            test.note = "关闭纹理且外部图片缺失";
            test.expectImages = 0;
            test.status = Status::required;
            runFileCase(test, root, tally, manifest, observed);
        }

        // --- 开启纹理但图片缺失/损坏必须失败 ---
        struct FailureCase { std::string name, note, reason; };
        auto addExternalFailure = [&](const FailureCase& specification, bool corrupt) {
            fs::path directory = root / specification.name;
            fs::create_directories(directory);
            fs::path textureFile = directory / "texture.png";
            if (corrupt) atomicWrite(textureFile, Bytes{0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'});
            auto image = new osg::Image;
            image->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE, 1);
            std::memset(image->data(), 6, 16);
            image->setFileName(fs::absolute(textureFile).u8string());
            image->setWriteHint(osg::Image::EXTERNAL_FILE);
            auto geode = planarMesh();
            geode->getOrCreateStateSet()->setTextureAttributeAndModes(0, nearestTexture(image), osg::StateAttribute::ON);
            writeNodeOrFail(geode, directory / (specification.name + ".osgb"), "External");
            FileCase test; test.name = specification.name; test.note = specification.note;
            test.expectFailure = true; test.reason = specification.reason;
            runFileCase(test, root, tally, manifest, observed);
        };
        addExternalFailure({"external-missing", "开启纹理但外部图片缺失", "材质引用了缺失纹理"}, false);
        addExternalFailure({"external-corrupt", "开启纹理但外部图片损坏", "材质引用了缺失纹理"}, true);
        {
            fs::path directory = root / "embedded-corrupt";
            fs::create_directories(directory);
            Bytes payload(embedded.payloadLength, 0);
            atomicWrite(directory / "embedded-corrupt.osgb", patchContainerInto(embedded, payload, false));
            writeNodeOrFail(planarMesh(false), directory / siblingName);
            FileCase test; test.name = "embedded-corrupt";
            test.note = "开启纹理但内嵌图片损坏";
            test.expectFailure = true; test.reason = "材质引用了缺失纹理";
            runFileCase(test, root, tally, manifest, observed);
        }

        atomicJson(root / "cases.json", manifest);
        atomicJson(root / "native-observed.json", observed);
        std::cout << "通过 " << tally.passed << " 项；明确拒绝 " << tally.refused
                  << " 项（未建立等价输出，不计为兼容完成）；已知失败 " << tally.knownFail
                  << " 项；夹具: " << root.u8string() << '\n';
        for (const auto& note : tally.notes) std::cout << "  - " << note << '\n';
        if (strict && tally.knownFail) { std::cerr << "严格模式：存在已知失败\n"; return 2; }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "纹理兼容回归失败: " << error.what() << '\n';
        return 1;
    }
}
