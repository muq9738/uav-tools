#include "output_validation.hpp"
#include "codecs.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace nt {
namespace {
constexpr std::uint32_t kGlbMagic = 0x46546c67u;
constexpr std::uint32_t kGlbVersion = 2u;
constexpr std::uint32_t kJsonChunk = 0x4e4f534au;
constexpr std::uint32_t kBinChunk = 0x004e4942u;

struct ValidationContext {
    fs::path stagingRoot;
    std::set<std::string> manifests, glbs;
    std::map<std::string, std::string> imageMimes;
    std::map<unsigned, Bytes> decodedViews;
    bool preview=false;
    OutputValidationStats stats;
};

[[noreturn]] void fail(const fs::path& file, const std::string& reason) {
    throw std::runtime_error("发布前校验失败 [" + file.u8string() + "]: " + reason);
}

std::uint32_t readU32(const unsigned char* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

double finiteValue(const Json& value, const fs::path& file, const std::string& label) {
    if(!value.is_number()) fail(file, label + " 必须是数值");
    double result = value.get<double>();
    if(!std::isfinite(result)) fail(file, label + " 不是有限数值");
    return result;
}

std::uint64_t integerValue(const Json& value, const fs::path& file, const std::string& label) {
    if(value.is_number_unsigned()) return value.get<std::uint64_t>();
    if(value.is_number_integer()) {
        auto signedValue = value.get<std::int64_t>();
        if(signedValue < 0) fail(file, label + " 不能为负");
        return static_cast<std::uint64_t>(signedValue);
    }
    fail(file, label + " 必须是整数");
}

const Json& member(const Json& object, const char* key, const fs::path& file, const std::string& label) {
    if(!object.is_object()) fail(file, label + " 所在值不是对象");
    auto found = object.find(key);
    if(found == object.end()) fail(file, label + " 缺失");
    return *found;
}

const Json& objectMember(const Json& object, const char* key, const fs::path& file, const std::string& label) {
    const auto& value = member(object, key, file, label);
    if(!value.is_object()) fail(file, label + " 必须是对象");
    return value;
}

const Json& arrayMember(const Json& object, const char* key, const fs::path& file, const std::string& label) {
    const auto& value = member(object, key, file, label);
    if(!value.is_array()) fail(file, label + " 必须是数组");
    return value;
}

std::string stringMember(const Json& object, const char* key, const fs::path& file, const std::string& label) {
    const auto& value = member(object, key, file, label);
    if(!value.is_string()) fail(file, label + " 必须是字符串");
    return value.get<std::string>();
}

bool rangeWithin(std::uint64_t offset, std::uint64_t length, std::uint64_t limit) {
    return offset <= limit && length <= limit - offset;
}

bool productFits(std::uint64_t a, std::uint64_t b) {
    return b == 0 || a <= std::numeric_limits<std::uint64_t>::max() / b;
}

unsigned componentSize(std::uint64_t type) {
    switch(type) {
        case 5120: case 5121: return 1;
        case 5122: case 5123: return 2;
        case 5125: case 5126: return 4;
        default: return 0;
    }
}

unsigned componentCount(const std::string& type) {
    if(type == "SCALAR") return 1;
    if(type == "VEC2") return 2;
    if(type == "VEC3") return 3;
    if(type == "VEC4") return 4;
    return 0;
}

bool isWithin(const fs::path& root, const fs::path& candidate) {
    auto rootPart = root.begin();
    auto candidatePart = candidate.begin();
    for(; rootPart != root.end(); ++rootPart, ++candidatePart) {
        if(candidatePart == candidate.end() || *rootPart != *candidatePart) return false;
    }
    return true;
}

std::string lowerExtension(const fs::path& file) {
    auto text = file.extension().u8string();
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

Json parseJsonFile(const fs::path& file, const std::string& label) {
    Bytes bytes;
    try {
        bytes = readBytes(file);
    } catch(const std::exception& error) {
        fail(file, label + " 读取失败: " + error.what());
    }
    try {
        return Json::parse(bytes.begin(), bytes.end());
    } catch(const std::exception& error) {
        fail(file, label + " 不是有效 JSON: " + error.what());
    }
}

// 只接受本工程写出的本地相对 URI；真实路径必须留在暂存目录内。
fs::path resolveLocalUri(ValidationContext& context, const std::string& uri, const fs::path& base,
                         const fs::path& document, const std::string& label) {
    if(uri.empty()) fail(document, label + " uri 为空");
    if(uri.find("://") != std::string::npos || uri.rfind("//", 0) == 0 || uri.front() == '/')
        fail(document, label + " 必须是本地相对 URI: " + uri);
    for(char unsupported : {'\\', '?', '#', '%'})
        if(uri.find(unsupported) != std::string::npos)
            fail(document, label + " URI 形式不受支持: " + uri);
    fs::path relative = fs::u8path(uri);
    if(relative.is_absolute() || relative.has_root_name())
        fail(document, label + " 不能是绝对路径: " + uri);
    fs::path resolved = (base / relative).lexically_normal();
    std::error_code error;
    if(!fs::is_regular_file(resolved, error))
        fail(document, label + " 引用的本地资产不存在: " + resolved.u8string());
    auto size = fs::file_size(resolved, error);
    if(error || size == 0)
        fail(document, label + " 引用的本地资产为空: " + resolved.u8string());
    if(!context.stagingRoot.empty()) {
        auto canonical = fs::canonical(resolved, error);
        if(error) fail(document, label + " 引用的本地资产无法解析: " + resolved.u8string());
        if(!isWithin(context.stagingRoot, canonical))
            fail(document, label + " 引用的本地资产逃出暂存目录: " + canonical.u8string());
    }
    return resolved;
}

void collectExtensions(const Json& value, std::set<std::string>& names) {
    if(value.is_object()) {
        for(auto item = value.begin(); item != value.end(); ++item) {
            if(item.key() == "extensions" && item.value().is_object())
                for(auto extension = item.value().begin(); extension != item.value().end(); ++extension)
                    names.insert(extension.key());
            collectExtensions(item.value(), names);
        }
    } else if(value.is_array()) {
        for(const auto& item : value) collectExtensions(item, names);
    }
}

void checkImageFile(const fs::path& file, const std::string& mime, const fs::path& document,
                    const std::string& label, ValidationContext& context) {
    if(mime != "image/png" && mime != "image/jpeg" && mime != "image/ktx2")
        fail(document, label + " 不支持的纹理 mimeType: " + mime);
    std::error_code error;
    auto canonical = fs::canonical(file, error);
    if(error) fail(document, label + " 图片无法解析: " + file.u8string());
    auto previous = context.imageMimes.find(canonical.u8string());
    if(previous != context.imageMimes.end()) {
        if(previous->second != mime) fail(document, label + " 同一图片被声明为不同 mimeType: " + file.u8string());
        return;
    }
    std::ifstream input(file, std::ios::binary);
    unsigned char header[8] = {};
    input.read(reinterpret_cast<char*>(header), sizeof(header));
    auto count = input.gcount();
    bool png = count >= 8 && header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4e &&
               header[3] == 0x47 && header[4] == 0x0d && header[5] == 0x0a && header[6] == 0x1a && header[7] == 0x0a;
    bool jpeg = count >= 3 && header[0] == 0xff && header[1] == 0xd8 && header[2] == 0xff;
    if((mime == "image/png" && !png) || (mime == "image/jpeg" && !jpeg))
        fail(document, label + " 图片签名与 mimeType 不一致: " + file.u8string());
    if(mime=="image/ktx2") {
        try {inspectBasis(readBytes(file),true);} catch(const std::exception& error) {fail(document,label+" KTX2 校验失败: "+error.what());}
    }
    context.imageMimes.emplace(canonical.u8string(), mime);
    context.stats.images++;
}

void validateGltfDocument(const Json& document, const fs::path& file, std::uint64_t binLength,
                          ValidationContext& context);

void validateGlb(const fs::path& path, const fs::path& document, ValidationContext& context) {
    std::error_code error;
    auto canonical = fs::canonical(path, error);
    if(error) fail(document, "内容 GLB 无法解析: " + path.u8string());
    if(!context.glbs.insert(canonical.u8string()).second) return;
    auto fileSize = fs::file_size(path, error);
    if(error || fileSize < 12) fail(path, "GLB 文件过短，头部不完整");
    std::ifstream input(path, std::ios::binary);
    if(!input) fail(path, "无法读取 GLB");
    unsigned char header[12] = {};
    if(!input.read(reinterpret_cast<char*>(header), sizeof(header))) fail(path, "GLB 头部读取失败");
    if(readU32(header) != kGlbMagic) fail(path, "GLB 魔数不是 glTF");
    if(readU32(header + 4) != kGlbVersion) fail(path, "GLB 版本不是 2");
    if(static_cast<std::uint64_t>(readU32(header + 8)) != fileSize)
        fail(path, "GLB 声明长度与文件大小不一致，文件被截断或未完成写入");
    unsigned char chunk[8] = {};
    if(!input.read(reinterpret_cast<char*>(chunk), sizeof(chunk))) fail(path, "GLB 缺少 JSON chunk 头");
    auto jsonLength = readU32(chunk);
    if(readU32(chunk + 4) != kJsonChunk) fail(path, "GLB 第一个 chunk 不是 JSON");
    if(jsonLength == 0 || jsonLength % 4 != 0) fail(path, "GLB JSON chunk 长度非法");
    if(!rangeWithin(20, jsonLength, fileSize)) fail(path, "GLB JSON chunk 超出文件范围");
    Bytes jsonBytes(jsonLength);
    if(!input.read(reinterpret_cast<char*>(jsonBytes.data()), static_cast<std::streamsize>(jsonLength)))
        fail(path, "GLB JSON chunk 读取失败");
    Json gltf;
    try {
        gltf = Json::parse(jsonBytes.begin(), jsonBytes.end());
    } catch(const std::exception& parseError) {
        fail(path, std::string("GLB JSON chunk 无法解析: ") + parseError.what());
    }
    auto binHeaderOffset = static_cast<std::uint64_t>(20) + jsonLength;
    if(!rangeWithin(binHeaderOffset, 8, fileSize)) fail(path, "GLB 缺少 BIN chunk 头");
    input.seekg(static_cast<std::streamoff>(binHeaderOffset));
    if(!input.read(reinterpret_cast<char*>(chunk), sizeof(chunk))) fail(path, "GLB BIN chunk 头读取失败");
    auto binLength = readU32(chunk);
    if(readU32(chunk + 4) != kBinChunk) fail(path, "GLB 第二个 chunk 不是 BIN");
    if(binLength % 4 != 0) fail(path, "GLB BIN chunk 长度未 4 字节对齐");
    if(!rangeWithin(binHeaderOffset + 8, binLength, fileSize) || binHeaderOffset + 8 + binLength != fileSize)
        fail(path, "GLB 除 JSON/BIN chunk 外还有未声明数据");
    context.stats.contents++;
    validateGltfDocument(gltf, path, binLength, context);
}

void validateGltfDocument(const Json& document, const fs::path& file, std::uint64_t binLength,
                          ValidationContext& context) {
    if(!document.is_object()) fail(file, "GLB JSON 根不是对象");
    const auto& asset = objectMember(document, "asset", file, "asset");
    if(stringMember(asset, "version", file, "asset.version") != "2.0")
        fail(file, "asset.version 不是 2.0");
    const auto& buffers = arrayMember(document, "buffers", file, "buffers");
    if(buffers.empty() || buffers.size() > 2) fail(file, "buffers 数量必须是当前格式的 1 或 2");
    std::uint64_t buffer0 = 0, buffer1 = 0;
    for(std::size_t i = 0; i < buffers.size(); ++i) {
        const auto& buffer = buffers[i];
        if(!buffer.is_object()) fail(file, "buffers[] 不是对象");
        if(buffer.contains("uri")) fail(file, "GLB buffer 不允许外部 uri");
        auto length = integerValue(member(buffer, "byteLength", file, "buffer.byteLength"), file, "buffer.byteLength");
        if(i == 0) {
            buffer0 = length;
            if(length > binLength) fail(file, "buffer 0 超出 BIN chunk");
            if(binLength - length >= 4) fail(file, "BIN chunk 比 buffer 0 多出超过 3 字节填充");
        } else {
            buffer1 = length;
            const auto& extensions = objectMember(buffer, "extensions", file, "buffer.extensions");
            const auto& meshopt = objectMember(extensions, "EXT_meshopt_compression", file,
                                               "buffer.extensions.EXT_meshopt_compression");
            const auto& fallback = member(meshopt, "fallback", file, "EXT_meshopt_compression.fallback");
            if(!fallback.is_boolean() || !fallback.get<bool>())
                fail(file, "虚拟 buffer 缺少 fallback:true");
        }
    }
    const auto& views = arrayMember(document, "bufferViews", file, "bufferViews");
    if(views.empty()) fail(file, "bufferViews 为空");
    std::uint64_t compressedViews = 0;
    Bytes previewGlbBytes;
    std::size_t previewBinOffset = 0;
    if(context.preview) {
        previewGlbBytes = readBytes(file);
        previewBinOffset = 28 + readU32(previewGlbBytes.data() + 12);
    }
    for(const auto& view : views) {
        if(!view.is_object()) fail(file, "bufferViews[] 不是对象");
        auto bufferIndex = integerValue(member(view, "buffer", file, "bufferView.buffer"), file, "bufferView.buffer");
        auto offset = view.contains("byteOffset")
                          ? integerValue(view.at("byteOffset"), file, "bufferView.byteOffset") : 0;
        auto length = integerValue(member(view, "byteLength", file, "bufferView.byteLength"), file,
                                   "bufferView.byteLength");
        if(length == 0) fail(file, "bufferView.byteLength 为 0");
        if(view.contains("byteStride") &&
           integerValue(view.at("byteStride"), file, "bufferView.byteStride") == 0)
            fail(file, "bufferView.byteStride 为 0");
        bool compressed = false;
        if(view.contains("extensions")) {
            const auto& extensions = view.at("extensions");
            if(!extensions.is_object()) fail(file, "bufferView.extensions 必须是对象");
            compressed = extensions.contains("EXT_meshopt_compression");
        }
        if(compressed) {
            if(bufferIndex != 1) fail(file, "压缩视图必须引用虚拟 buffer 1");
            if(!rangeWithin(offset, length, buffer1)) fail(file, "压缩视图虚拟范围超出 buffer 1");
            const auto& extension = view.at("extensions").at("EXT_meshopt_compression");
            if(!extension.is_object()) fail(file, "EXT_meshopt_compression 必须是对象");
            auto physicalBuffer = integerValue(member(extension, "buffer", file, "compressed.buffer"), file,
                                               "compressed.buffer");
            auto physicalOffset = integerValue(member(extension, "byteOffset", file, "compressed.byteOffset"), file,
                                               "compressed.byteOffset");
            auto physicalLength = integerValue(member(extension, "byteLength", file, "compressed.byteLength"), file,
                                               "compressed.byteLength");
            if(physicalBuffer != 0) fail(file, "压缩视图扩展必须引用 buffer 0");
            if(!rangeWithin(physicalOffset, physicalLength, buffer0))
                fail(file, "压缩视图物理范围超出 buffer 0");
            auto stride = integerValue(member(extension, "byteStride", file, "compressed.byteStride"), file,
                                       "compressed.byteStride");
            auto count = integerValue(member(extension, "count", file, "compressed.count"), file, "compressed.count");
            if(stride == 0 || count == 0) fail(file, "压缩视图 byteStride/count 必须大于 0");
            if(!productFits(count, stride) || count * stride != length)
                fail(file, "压缩视图 count × byteStride 与虚拟 byteLength 不一致");
            auto mode = stringMember(extension, "mode", file, "compressed.mode");
            if(mode != "ATTRIBUTES" && mode != "INDICES" && mode != "TRIANGLES") fail(file, "压缩视图 mode 不受支持: " + mode);
            if(mode == "ATTRIBUTES" ? (stride % 4 != 0 || stride > 256) : (stride != 2 && stride != 4))
                fail(file, "压缩视图 byteStride 不符合 mode");
            if(mode == "TRIANGLES" && count % 3 != 0) fail(file, "TRIANGLES 压缩视图 count 必须为 3 的倍数");
            auto filter = stringMember(extension, "filter", file, "compressed.filter");
            if(filter != "NONE") fail(file, "压缩视图 filter 必须是 NONE: " + filter);
            ++compressedViews;
            if(context.preview) {
                if(!rangeWithin(previewBinOffset+physicalOffset,physicalLength,previewGlbBytes.size())) fail(file,"预览 Meshopt 物理数据超出 BIN");
                Bytes encoded(previewGlbBytes.begin()+previewBinOffset+physicalOffset,previewGlbBytes.begin()+previewBinOffset+physicalOffset+physicalLength);
                try {
                    auto decoded=decodeMeshopt(encoded,count,stride,mode!="ATTRIBUTES");
                    if(decoded.size()!=length) fail(file,"预览 Meshopt 解码长度与虚拟视图不一致");
                    context.decodedViews[static_cast<unsigned>(&view-&views[0])]=std::move(decoded);
                } catch(const std::exception& error) {fail(file,"预览 Meshopt 解码失败: "+std::string(error.what()));}
            }
        } else {
            if(bufferIndex != 0) fail(file, "原始 bufferView 必须引用 buffer 0");
            if(!rangeWithin(offset, length, buffer0)) fail(file, "bufferView 范围超出 buffer 0");
        }
    }
    const auto& accessors = arrayMember(document, "accessors", file, "accessors");
    if(accessors.empty()) fail(file, "accessors 为空");
    std::set<unsigned> dracoAccessors;
    auto extensions=document.value("extensionsRequired",Json::array());
    if(std::find(extensions.begin(),extensions.end(),"KHR_draco_mesh_compression")!=extensions.end()) {
        auto bytes=readBytes(file);auto begin=28+readU32(bytes.data()+12);
        try {dracoAccessors=validateDraco(document,Bytes(bytes.begin()+begin,bytes.end()));}
        catch(const std::exception& error) {fail(file,std::string("Draco 校验失败: ")+error.what());}
    }
    for(std::size_t accessorIndex=0;accessorIndex<accessors.size();++accessorIndex) {
        const auto& accessor=accessors[accessorIndex];
        if(!accessor.is_object()) fail(file, "accessors[] 不是对象");
        if(accessor.contains("sparse")) fail(file, "accessor.sparse 未生成，拒绝跳过");
        if(!accessor.contains("bufferView") && dracoAccessors.count(accessorIndex)) continue;
        auto viewIndex = integerValue(member(accessor, "bufferView", file, "accessor.bufferView"), file,
                                      "accessor.bufferView");
        if(viewIndex >= static_cast<std::uint64_t>(views.size()))
            fail(file, "accessor.bufferView 索引越界");
        const auto& view = views[static_cast<std::size_t>(viewIndex)];
        auto viewLength = integerValue(member(view, "byteLength", file, "bufferView.byteLength"), file,
                                       "bufferView.byteLength");
        auto componentType = integerValue(member(accessor, "componentType", file, "accessor.componentType"), file,
                                          "accessor.componentType");
        auto size = componentSize(componentType);
        if(size == 0) fail(file, "accessor.componentType 不受支持");
        auto type = stringMember(accessor, "type", file, "accessor.type");
        auto components = componentCount(type);
        if(components == 0) fail(file, "accessor.type 不受支持: " + type);
        auto count = integerValue(member(accessor, "count", file, "accessor.count"), file, "accessor.count");
        if(count == 0) fail(file, "accessor.count 为 0");
        auto offset = accessor.contains("byteOffset")
                          ? integerValue(accessor.at("byteOffset"), file, "accessor.byteOffset") : 0;
        auto stride = accessor.contains("byteStride")
                          ? integerValue(accessor.at("byteStride"), file, "accessor.byteStride") : 0;
        auto elementSize = static_cast<std::uint64_t>(components) * size;
        if(stride == 0 && view.contains("byteStride"))
            stride = integerValue(view.at("byteStride"), file, "bufferView.byteStride");
        if(stride == 0) stride = elementSize;
        if(stride < elementSize) fail(file, "accessor.byteStride 小于元素大小");
        std::uint64_t span = 0;
        if(count > 1) {
            if(!productFits(count - 1, stride)) fail(file, "accessor 范围溢出");
            span = (count - 1) * stride;
        }
        if(!rangeWithin(offset, span, viewLength)) fail(file, "accessor 范围超出 bufferView");
        if(!rangeWithin(offset + span, elementSize, viewLength))
            fail(file, "accessor 范围超出 bufferView");
        bool hasMin = accessor.contains("min"), hasMax = accessor.contains("max");
        if(hasMin != hasMax) fail(file, "accessor.min/max 必须成对");
        if(hasMin) {
            const auto& minimum = accessor.at("min");
            const auto& maximum = accessor.at("max");
            if(!minimum.is_array() || !maximum.is_array() || minimum.size() != components ||
               maximum.size() != components)
                fail(file, "accessor.min/max 分量数与 type 不一致");
            for(unsigned c = 0; c < components; ++c) {
                auto label = std::to_string(c);
                auto low = finiteValue(minimum[c], file, "accessor.min[" + label + "]");
                auto high = finiteValue(maximum[c], file, "accessor.max[" + label + "]");
                if(low > high) fail(file, "accessor.min 大于 accessor.max");
            }
        }
    }
    const auto& materials = arrayMember(document, "materials", file, "materials");
    const auto requiredExtensions = document.value("extensionsRequired", Json::array());
    const bool meshQuantization = std::find(requiredExtensions.begin(), requiredExtensions.end(), "KHR_mesh_quantization") != requiredExtensions.end();
    if(context.preview) {
        if(std::find(requiredExtensions.begin(),requiredExtensions.end(),"EXT_meshopt_compression")==requiredExtensions.end()) fail(file,"预览必须实际使用 EXT_meshopt_compression");
        if(!meshQuantization) fail(file,"预览必须实际使用 KHR_mesh_quantization");
        if(std::find(requiredExtensions.begin(),requiredExtensions.end(),"KHR_materials_unlit")==requiredExtensions.end()) fail(file,"预览材质必须声明 KHR_materials_unlit");
        if(std::find(requiredExtensions.begin(),requiredExtensions.end(),"KHR_draco_mesh_compression")!=requiredExtensions.end()) fail(file,"预览不允许使用 Draco");
    }
    const auto normalizedQuantized = [meshQuantization](const Json& accessor) {
        return meshQuantization && accessor.value("normalized", false);
    };
    if(materials.empty()) fail(file, "materials 为空");
    const auto& meshes = arrayMember(document, "meshes", file, "meshes");
    if(meshes.empty()) fail(file, "meshes 为空");
    static const std::set<std::string> allowedAttributes = {"POSITION", "TEXCOORD_0", "NORMAL", "COLOR_0"};
    for(const auto& mesh : meshes) {
        if(!mesh.is_object()) fail(file, "meshes[] 不是对象");
        const auto& primitives = arrayMember(mesh, "primitives", file, "mesh.primitives");
        if(primitives.empty()) fail(file, "mesh.primitives 为空");
        for(const auto& primitive : primitives) {
            if(!primitive.is_object()) fail(file, "primitive 不是对象");
            const auto& attributes = objectMember(primitive, "attributes", file, "primitive.attributes");
            if(!attributes.contains("POSITION")) fail(file, "primitive 缺少 POSITION 属性");
            for(auto attribute = attributes.begin(); attribute != attributes.end(); ++attribute) {
                if(!allowedAttributes.count(attribute.key()))
                    fail(file, "primitive 属性不在当前格式内: " + attribute.key());
                if(context.preview && attribute.key()=="NORMAL") fail(file,"预览 unlit 几何必须省略 NORMAL");
                auto index = integerValue(attribute.value(), file, "primitive.attributes." + attribute.key());
                if(index >= static_cast<std::uint64_t>(accessors.size()))
                    fail(file, "primitive 属性 accessor 索引越界");
                const auto& accessor = accessors[static_cast<std::size_t>(index)];
                if(accessor.contains("bufferView")) {
                    const auto& attributeView = views[accessor.at("bufferView").get<std::size_t>()];
                    if(attributeView.contains("extensions") && attributeView.at("extensions").contains("EXT_meshopt_compression") &&
                       attributeView.at("extensions").at("EXT_meshopt_compression").at("mode") != "ATTRIBUTES")
                        fail(file, "属性必须引用 ATTRIBUTES 压缩视图");
                }
                auto type = stringMember(accessor, "type", file, "accessor.type");
                auto componentType = integerValue(member(accessor, "componentType", file, "accessor.componentType"),
                                                  file, "accessor.componentType");
                if(context.preview && attribute.key()=="POSITION" && !(componentType==5123 && normalizedQuantized(accessor)))
                    fail(file,"预览 POSITION 必须为 KHR_mesh_quantization 归一化 USHORT");
                if(attribute.key() == "POSITION") {
                    if(type != "VEC3" || (componentType != 5126 && !(componentType == 5123 && normalizedQuantized(accessor))))
                        fail(file, "POSITION 必须是 VEC3/FLOAT 或声明 KHR_mesh_quantization 的归一化 USHORT");
                    if(!accessor.contains("min") || !accessor.contains("max"))
                        fail(file, "POSITION accessor 缺少有限 min/max");
                } else if(attribute.key() == "TEXCOORD_0") {
                    if(type != "VEC2" || (componentType != 5126 && !(componentType == 5123 && normalizedQuantized(accessor))))
                        fail(file, "TEXCOORD_0 必须是 VEC2/FLOAT 或 KHR 归一化 USHORT");
                } else if(attribute.key() == "NORMAL") {
                    if(type != "VEC3" || componentType != 5126) fail(file, "NORMAL 必须是 VEC3/FLOAT");
                } else if(attribute.key() == "COLOR_0") {
                    if((type != "VEC3" && type != "VEC4") || componentType != 5126)
                        fail(file, "COLOR_0 必须是 VEC3/VEC4/FLOAT");
                }
            }
            if(!primitive.contains("indices")) fail(file, "primitive 缺少 indices");
            auto indicesIndex = integerValue(primitive.at("indices"), file, "primitive.indices");
            if(indicesIndex >= static_cast<std::uint64_t>(accessors.size()))
                fail(file, "primitive.indices 索引越界");
            const auto& indices = accessors[static_cast<std::size_t>(indicesIndex)];
            auto componentType = integerValue(member(indices, "componentType", file, "accessor.componentType"),
                                              file, "accessor.componentType");
            if(componentType != 5121 && componentType != 5123 && componentType != 5125)
                fail(file, "索引 accessor 必须是无符号整数");
            if(stringMember(indices, "type", file, "accessor.type") != "SCALAR")
                fail(file, "索引 accessor 类型必须是 SCALAR");
            auto indexCount = integerValue(member(indices, "count", file, "accessor.count"), file, "accessor.count");
            auto mode = primitive.contains("mode")
                            ? integerValue(primitive.at("mode"), file, "primitive.mode") : 4;
            if(mode > 6) fail(file, "primitive.mode 不受支持");
            if(mode == 4 && indexCount % 3 != 0) fail(file, "三角形索引数量不是 3 的倍数");
            const auto indexView = indices.contains("bufferView")?views[indices.at("bufferView").get<std::size_t>()]:Json::object();
            if(indexView.contains("extensions") && indexView.at("extensions").contains("EXT_meshopt_compression")) {
                const auto& compression = indexView.at("extensions").at("EXT_meshopt_compression");
                if(compression.at("mode") == "ATTRIBUTES") fail(file, "索引不能引用 ATTRIBUTES 压缩视图");
                if(compression.at("byteStride").get<unsigned>() != componentSize(componentType))
                    fail(file, "索引压缩 byteStride 与 accessor 分量大小不一致");
                if(compression.at("mode") == "TRIANGLES" && (indices.value("byteOffset", 0ULL) != 0 ||
                   compression.at("count").get<std::uint64_t>() != indexCount))
                    fail(file, "TRIANGLES 索引 accessor 必须完整引用压缩视图");
                if(compression.at("mode") == "TRIANGLES" && mode != 4)
                    fail(file, "TRIANGLES 压缩只能用于三角形 primitive，不能轮转点或线");
            }
            auto material = integerValue(member(primitive, "material", file, "primitive.material"), file,
                                         "primitive.material");
            if(material >= static_cast<std::uint64_t>(materials.size()))
                fail(file, "primitive.material 索引越界");
            if(context.preview) {
                const auto& materialJson=materials[static_cast<std::size_t>(material)];
                if(!materialJson.contains("extensions") || !materialJson.at("extensions").contains("KHR_materials_unlit")) fail(file,"预览 primitive 材质不是 unlit");
                auto positionIndex=attributes.at("POSITION").get<std::size_t>();auto vertexCount=accessors[positionIndex].at("count").get<std::uint64_t>();
                if(indices.contains("bufferView")) {
                    auto viewIndex=indices.at("bufferView").get<unsigned>();auto found=context.decodedViews.find(viewIndex);
                    if(found==context.decodedViews.end()) fail(file,"预览索引缺少 Meshopt 解码视图");
                    auto size=componentSize(componentType);auto offset=indices.value("byteOffset",0ULL);
                    for(std::uint64_t i=0;i<indexCount;++i) {
                        auto at=offset+i*size;if(!rangeWithin(at,size,found->second.size())) fail(file,"预览解码索引越界");
                        std::uint64_t value=0;for(unsigned b=0;b<size;++b) value|=std::uint64_t(found->second[at+b])<<(8*b);
                        if(value>=vertexCount) fail(file,"预览解码索引超出 POSITION 顶点数");
                    }
                }
                for(const char* name:{"POSITION","TEXCOORD_0"}) if(attributes.contains(name)) {
                    const auto& accessor=accessors[attributes.at(name).get<std::size_t>()];if(!accessor.contains("bufferView")) continue;
                    auto found=context.decodedViews.find(accessor.at("bufferView").get<unsigned>());if(found==context.decodedViews.end()) fail(file,"预览属性缺少 Meshopt 解码视图");
                    auto components=componentCount(accessor.at("type").get<std::string>());auto count=accessor.at("count").get<std::uint64_t>();auto view=views[accessor.at("bufferView").get<std::size_t>()];
                    auto ctype=accessor.at("componentType").get<std::uint64_t>();auto csize=componentSize(ctype);
                    auto stride=view.value("byteStride",components*csize);auto offset=accessor.value("byteOffset",0ULL);
                    for(std::uint64_t i=0;i<count*components;++i) {
                        auto at=offset+(i/components)*stride+(i%components)*csize;if(!rangeWithin(at,csize,found->second.size())) fail(file,"预览解码属性越界");
                        if(ctype==5126) {
                            auto value=readU32(found->second.data()+at);float number=0;std::memcpy(&number,&value,4);if(!std::isfinite(number)) fail(file,"预览解码属性包含非有限值");
                        }
                    }
                }
            }
        }
    }
    const auto& nodes = arrayMember(document, "nodes", file, "nodes");
    if(nodes.empty()) fail(file, "nodes 为空");
    for(const auto& node : nodes) {
        if(!node.is_object()) fail(file, "nodes[] 不是对象");
        if(node.contains("mesh")) {
            auto mesh = integerValue(node.at("mesh"), file, "node.mesh");
            if(mesh >= static_cast<std::uint64_t>(meshes.size())) fail(file, "node.mesh 索引越界");
        }
        if(node.contains("matrix")) {
            const auto& matrix = node.at("matrix");
            if(!matrix.is_array() || matrix.size() != 16) fail(file, "node.matrix 不是 16 个数值");
            for(std::size_t i = 0; i < matrix.size(); ++i)
                finiteValue(matrix[i], file, "node.matrix[" + std::to_string(i) + "]");
        }
    }
    const auto& scenes = arrayMember(document, "scenes", file, "scenes");
    if(scenes.empty()) fail(file, "scenes 为空");
    const auto& sceneNodes = arrayMember(scenes[0], "nodes", file, "scenes[0].nodes");
    for(const auto& node : sceneNodes) {
        auto index = integerValue(node, file, "scenes[0].nodes[]");
        if(index >= static_cast<std::uint64_t>(nodes.size())) fail(file, "scenes[0].nodes 索引越界");
    }
    if(integerValue(member(document, "scene", file, "scene"), file, "scene") != 0)
        fail(file, "scene 必须为 0");
    std::size_t imageTotal = 0;
    if(document.contains("images")) {
        const auto& images = arrayMember(document, "images", file, "images");
        if(images.empty()) fail(file, "images 为空");
        imageTotal = images.size();
        for(const auto& image : images) {
            if(!image.is_object()) fail(file, "images[] 不是对象");
            auto mime = stringMember(image, "mimeType", file, "image.mimeType");
            if(mime != "image/png" && mime != "image/jpeg" && mime!="image/ktx2")
                fail(file, "image.mimeType 不受支持: " + mime);
            if(image.contains("bufferView")) {
                if(image.contains("uri")) fail(file, "image 不能同时包含 uri 和 bufferView");
                auto viewIndex = integerValue(image.at("bufferView"), file, "image.bufferView");
                if(viewIndex >= static_cast<std::uint64_t>(views.size()))
                    fail(file, "image.bufferView 索引越界");
                const auto& view = views[static_cast<std::size_t>(viewIndex)];
                if(view.contains("extensions") && view.at("extensions").contains("EXT_meshopt_compression"))
                    fail(file, "内嵌纹理 bufferView 不能使用 EXT_meshopt_compression");
                auto offset = view.contains("byteOffset")
                                  ? integerValue(view.at("byteOffset"), file, "bufferView.byteOffset") : 0;
                auto length = integerValue(member(view, "byteLength", file, "bufferView.byteLength"),
                                           file, "bufferView.byteLength");
                if(previewGlbBytes.empty()) {
                    previewGlbBytes = readBytes(file);
                    previewBinOffset = 28 + readU32(previewGlbBytes.data() + 12);
                }
                if(!rangeWithin(previewBinOffset + offset, length, previewGlbBytes.size()))
                    fail(file, "内嵌纹理数据超出 BIN chunk");
                const unsigned char* header = previewGlbBytes.data() + previewBinOffset + offset;
                bool png = length >= 8 && header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4e &&
                           header[3] == 0x47 && header[4] == 0x0d && header[5] == 0x0a && header[6] == 0x1a && header[7] == 0x0a;
                bool jpeg = length >= 3 && header[0] == 0xff && header[1] == 0xd8 && header[2] == 0xff;
                if((mime == "image/png" && !png) || (mime == "image/jpeg" && !jpeg))
                    fail(file, "内嵌图片签名与 mimeType 不一致");
                if(mime == "image/ktx2") {
                    try { inspectBasis(Bytes(header, header + length), true); }
                    catch(const std::exception& error) { fail(file, std::string("内嵌 KTX2 校验失败: ") + error.what()); }
                }
                context.stats.images++;
            } else {
                if(context.preview) fail(file, "预览纹理必须内嵌在 GLB bufferView 中");
                auto uri = stringMember(image, "uri", file, "image.uri");
                auto imageFile = resolveLocalUri(context, uri, file.parent_path(), file, "image.uri");
                checkImageFile(imageFile, mime, file, "image.uri", context);
            }
        }
    }
    std::size_t textureTotal = 0;
    if(document.contains("textures")) {
        const auto& textures = arrayMember(document, "textures", file, "textures");
        textureTotal = textures.size();
        if(textures.empty()) fail(file, "textures 为空");
        const auto& samplers = arrayMember(document, "samplers", file, "samplers");
        for(const auto& texture : textures) {
            if(!texture.is_object()) fail(file, "textures[] 不是对象");
            const bool basis=texture.contains("extensions") && texture.at("extensions").contains("KHR_texture_basisu");
            auto source = basis?integerValue(texture.at("extensions").at("KHR_texture_basisu").at("source"),file,"basis.source"):
                integerValue(member(texture, "source", file, "texture.source"), file, "texture.source");
            if(basis && document.at("images").at(source).at("mimeType")!="image/ktx2") fail(file,"Basis texture must reference KTX2");
            auto sampler = integerValue(member(texture, "sampler", file, "texture.sampler"), file, "texture.sampler");
            if(source >= imageTotal) fail(file, "texture.source 索引越界");
            if(sampler >= static_cast<std::uint64_t>(samplers.size()))
                fail(file, "texture.sampler 索引越界");
        }
    }
    for(const auto& material : materials) {
        if(!material.is_object()) fail(file, "materials[] 不是对象");
        if(!material.contains("pbrMetallicRoughness")) continue;
        const auto& pbr = objectMember(material, "pbrMetallicRoughness", file, "material.pbrMetallicRoughness");
        if(!pbr.contains("baseColorTexture")) continue;
        const auto& texture = objectMember(pbr, "baseColorTexture", file, "material.baseColorTexture");
        auto index = integerValue(member(texture, "index", file, "baseColorTexture.index"), file, "baseColorTexture.index");
        if(index >= textureTotal) fail(file, "baseColorTexture.index 索引越界");
    }
    std::set<std::string> used;
    collectExtensions(document, used);
    std::set<std::string> declared, required;
    if(document.contains("extensionsUsed")) {
        const auto& list = arrayMember(document, "extensionsUsed", file, "extensionsUsed");
        for(const auto& name : list) {
            if(!name.is_string()) fail(file, "extensionsUsed[] 不是字符串");
            declared.insert(name.get<std::string>());
        }
    }
    if(document.contains("extensionsRequired")) {
        const auto& list = arrayMember(document, "extensionsRequired", file, "extensionsRequired");
        for(const auto& name : list) {
            if(!name.is_string()) fail(file, "extensionsRequired[] 不是字符串");
            required.insert(name.get<std::string>());
        }
    }
    for(const auto& name : used)
        if(!declared.count(name)) fail(file, "扩展 " + name + " 未在 extensionsUsed 声明");
    for(const auto& name : required)
        if(!declared.count(name)) fail(file, "extensionsRequired 中的扩展未在 extensionsUsed 声明: " + name);
    if(used.count("KHR_texture_basisu") && !required.count("KHR_texture_basisu")) fail(file,"Basis texture extension must be required");
    if(compressedViews > 0) {
        if(!declared.count("EXT_meshopt_compression"))
            fail(file, "压缩视图未声明 EXT_meshopt_compression");
        if(!required.count("EXT_meshopt_compression"))
            fail(file, "压缩视图必须把 EXT_meshopt_compression 声明为 required");
        if(buffers.size() != 2) fail(file, "压缩视图缺少虚拟 buffer 1");
    } else if(buffers.size() == 2) {
        fail(file, "虚拟 buffer 1 没有对应压缩视图");
    }
}

void validateTile(const Json& tile, const fs::path& base, const fs::path& document,
                  ValidationContext& context) {
    if(!tile.is_object()) fail(document, "瓦片不是对象");
    context.stats.tiles++;
    const auto& volume = objectMember(tile, "boundingVolume", document, "boundingVolume");
    const auto& box = arrayMember(volume, "box", document, "boundingVolume.box");
    if(box.size() != 12) fail(document, "boundingVolume.box 不是 12 个数值");
    for(std::size_t i = 0; i < box.size(); ++i)
        finiteValue(box[i], document, "boundingVolume.box[" + std::to_string(i) + "]");
    for(auto item = volume.begin(); item != volume.end(); ++item)
        if(item.key() != "box")
            fail(document, "boundingVolume 只支持 box，发现 " + item.key());
    auto error = finiteValue(member(tile, "geometricError", document, "geometricError"), document,
                             "geometricError");
    if(error < 0) fail(document, "geometricError 不能为负");
    if(tile.contains("implicitTiling")) fail(document, "当前格式不支持 implicitTiling");
    if(tile.contains("contents")) fail(document, "当前格式不支持 contents 多内容数组");
    bool hasContent = false;
    if(tile.contains("content")) {
        hasContent = true;
        const auto& content = member(tile, "content", document, "content");
        if(!content.is_object()) fail(document, "content 不是对象");
        auto uri = stringMember(content, "uri", document, "content.uri");
        auto contentFile = resolveLocalUri(context, uri, base, document, "content.uri");
        if(lowerExtension(contentFile) != ".glb")
            fail(document, "content.uri 必须是当前格式的 GLB: " + uri);
        validateGlb(contentFile, document, context);
    }
    bool hasChildren = false;
    if(tile.contains("children")) {
        const auto& children = member(tile, "children", document, "children");
        if(!children.is_array()) fail(document, "children 不是数组");
        hasChildren = !children.empty();
        for(const auto& child : children) validateTile(child, base, document, context);
    }
    if(!hasContent && !hasChildren) fail(document, "瓦片既无内容也无子瓦片，内容被跳过");
}

void validateManifest(const Json& document, const fs::path& documentPath, ValidationContext& context) {
    context.stats.tilesets++;
    if(!document.is_object()) fail(documentPath, "tileset 根不是对象");
    const auto& asset = objectMember(document, "asset", documentPath, "asset");
    if(stringMember(asset, "version", documentPath, "asset.version") != "1.1")
        fail(documentPath, "asset.version 不是 1.1");
    const auto& root = member(document, "root", documentPath, "root");
    validateTile(root, documentPath.parent_path(), documentPath, context);
    if(context.preview && root.value("geometricError", 0.0) <= 0.0)
        fail(documentPath, "预览根 geometricError 必须为正，否则 Cesium 会跳过整棵子树");
}

void validateManifestFile(const fs::path& file, ValidationContext& context, const std::string& label) {
    std::error_code error;
    if(!fs::is_regular_file(file, error)) fail(file, label + " 不存在");
    auto canonical = fs::canonical(file, error);
    if(error) fail(file, label + " 无法解析");
    if(!context.manifests.insert(canonical.u8string()).second) return;
    auto document = parseJsonFile(canonical, label);
    validateManifest(document, canonical, context);
}
}

void validateTilesetDocument(const Json& document, const fs::path& documentPath) {
    ValidationContext context;
    validateManifest(document, documentPath, context);
}

OutputValidationStats validateStagedOutput(const fs::path& staging) {
    std::error_code error;
    if(!fs::is_directory(staging, error)) fail(staging, "暂存目录不存在或不是目录");
    ValidationContext context;
    context.stagingRoot = fs::canonical(staging, error);
    if(error) fail(staging, "暂存目录无法解析");
    validateManifestFile(context.stagingRoot / "tileset.json", context, "主入口");
    auto mainDocument=parseJsonFile(context.stagingRoot/"tileset.json","主入口");
    if(mainDocument.value("root",Json::object()).value("geometricError",0.0)<=0.0)
        fail(context.stagingRoot/"tileset.json","主入口根 geometricError 必须为正，否则 Cesium 会跳过整棵子树");
    fs::path reportPath = context.stagingRoot / "conversion-report.json";
    if(!fs::is_regular_file(reportPath, error)) fail(reportPath, "缺少转换报告");
    auto report = parseJsonFile(reportPath, "转换报告");
    if(!report.is_object()) fail(reportPath, "转换报告不是对象");
    const auto& records = arrayMember(report, "records", reportPath, "records");
    for(const auto& record : records) {
        if(!record.is_object()) fail(reportPath, "records[] 不是对象");
        auto tilesetUri = stringMember(record, "tileset", reportPath, "records[].tileset");
        auto manifest = resolveLocalUri(context, tilesetUri, context.stagingRoot, reportPath,
                                       "records[].tileset");
        if(lowerExtension(manifest) != ".json") fail(reportPath, "records[].tileset 必须是 JSON 清单");
        validateManifestFile(manifest, context, "分文件清单");
        if(!record.contains("content")) continue;
        const auto& contents = record.at("content");
        if(!contents.is_array()) fail(reportPath, "records[].content 不是数组");
        for(const auto& content : contents) {
            if(!content.is_object()) fail(reportPath, "records[].content[] 不是对象");
            auto uri = stringMember(content, "uri", reportPath, "records[].content[].uri");
            auto contentFile = resolveLocalUri(context, uri, context.stagingRoot, reportPath,
                                               "records[].content[].uri");
            if(lowerExtension(contentFile) != ".glb")
                fail(reportPath, "记录内容必须是当前格式的 GLB: " + uri);
            validateGlb(contentFile, reportPath, context);
            if(!content.contains("images")) continue;
            const auto& images = content.at("images");
            if(!images.is_array()) fail(reportPath, "records[].content[].images 不是数组");
            for(const auto& image : images) {
                if(!image.is_object()) fail(reportPath, "records[].content[].images[] 不是对象");
                auto imageUri = stringMember(image, "uri", reportPath, "records[].content[].images[].uri");
                resolveLocalUri(context, imageUri, context.stagingRoot, reportPath,
                                "records[].content[].images[].uri");
            }
        }
    }
    if(report.contains("preview")) {
        const auto& summary=objectMember(report,"preview",reportPath,"preview");
        if(stringMember(summary,"status",reportPath,"preview.status")!="complete" ||
           stringMember(summary,"entry",reportPath,"preview.entry")!="preview/tileset.json" ||
           stringMember(summary,"algorithm",reportPath,"preview.algorithm")!="mesh-preview-v2" ||
           stringMember(summary,"geometryEncoding",reportPath,"preview.geometryEncoding")!="EXT_meshopt_compression+KHR_mesh_quantization" ||
           stringMember(summary,"textureFormat",reportPath,"preview.textureFormat")!="original") fail(reportPath,"预览摘要与 mesh-preview-v2 契约不一致");
        ValidationContext preview;preview.preview=true;preview.stagingRoot=context.stagingRoot;
        validateManifestFile(preview.stagingRoot/"preview/tileset.json",preview,"轻量预览入口");
        auto previewReportPath=preview.stagingRoot/"preview/preview-report.json";
        if(!fs::is_regular_file(previewReportPath,error)) fail(previewReportPath,"缺少轻量预览详细报告");
        auto previewReport=parseJsonFile(previewReportPath,"轻量预览详细报告");
        if(stringMember(previewReport,"status",previewReportPath,"preview-report.status")!="complete" ||
           stringMember(previewReport,"algorithm",previewReportPath,"preview-report.algorithm")!="mesh-preview-v2") fail(previewReportPath,"轻量预览详细报告状态或算法不匹配");
        if(integerValue(member(summary,"triangles",reportPath,"preview.triangles"),reportPath,"preview.triangles")!=
           integerValue(member(previewReport,"triangles",previewReportPath,"preview-report.triangles"),previewReportPath,"preview-report.triangles")) fail(reportPath,"主报告与预览报告的三角形数不一致");
        context.stats.tilesets+=preview.stats.tilesets;context.stats.tiles+=preview.stats.tiles;
        context.stats.contents+=preview.stats.contents;context.stats.images+=preview.stats.images;
    } else if(fs::exists(context.stagingRoot/"preview")) fail(reportPath,"存在 preview 目录但主报告没有完整预览摘要");
    return context.stats;
}
}
