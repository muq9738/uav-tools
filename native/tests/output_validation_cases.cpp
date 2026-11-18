#include "native_tiles.hpp"
#include "output_validation.hpp"
#include <osg/Geode>
#include <osg/Geometry>
#include <osg/PagedLOD>
#include <osg/Texture2D>
#include <osgDB/WriteFile>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <vector>

using namespace nt;

namespace {
void require(bool condition, const std::string& message) {
    if(!condition) throw std::runtime_error(message);
}

osg::ref_ptr<osg::Geode> triangle() {
    auto geometry = new osg::Geometry;
    auto positions = new osg::Vec3Array;
    positions->push_back({0, 0, 0});
    positions->push_back({1, 0, 0});
    positions->push_back({0, 1, 0});
    geometry->setVertexArray(positions);
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
    auto node = new osg::Geode;
    node->addDrawable(geometry);
    return node;
}

void writeNode(osg::Node* node, const fs::path& path) {
    fs::create_directories(path.parent_path());
    osg::ref_ptr<osgDB::Options> options = new osgDB::Options("WriteImageHint=IncludeFile");
    require(osgDB::writeNodeFile(*node, path.string(), options.get()), "fixture 写入失败");
}

Options options(const fs::path& input, const fs::path& output) {
    Options value;
    value.input = input;
    value.output = output;
    value.compressGeometry = false;
    value.srs = "ENU:32.427247976,120.6706181";
    value.origin = "1,2,30.665";
    return value;
}

std::map<std::string, std::string> snapshot(const fs::path& root) {
    std::map<std::string, std::string> result;
    for(const auto& entry : fs::recursive_directory_iterator(root)) {
        if(!entry.is_regular_file()) continue;
        auto relative = fs::relative(entry.path(), root).generic_u8string();
        result[relative] = contentKey(readBytes(entry.path())) + ":" +
                           std::to_string(fs::file_size(entry.path()));
    }
    return result;
}

fs::path firstFile(const fs::path& root, const std::function<bool(const fs::path&)>& predicate) {
    std::vector<fs::path> matches;
    for(const auto& entry : fs::recursive_directory_iterator(root))
        if(entry.is_regular_file() && predicate(entry.path())) matches.push_back(entry.path());
    std::sort(matches.begin(), matches.end());
    require(!matches.empty(), "夹具未找到目标文件");
    return matches.front();
}

std::string expectFailure(const std::function<void()>& action) {
    try {
        action();
    } catch(const std::exception& error) {
        return error.what();
    }
    throw std::runtime_error("预期校验失败，但校验通过了");
}

void rewriteJsonFile(const fs::path& file, const std::function<void(Json&)>& mutate) {
    auto document = Json::parse(readBytes(file));
    mutate(document);
    auto text = document.dump(2);
    atomicWrite(file, Bytes(text.begin(), text.end()));
}

void rewriteGlb(const fs::path& file, const std::function<void(Json&)>& mutate) {
    auto bytes = readBytes(file);
    require(bytes.size() >= 20, "夹具 GLB 头不完整");
    std::uint32_t jsonLength = 0;
    std::memcpy(&jsonLength, bytes.data() + 12, 4);
    require(20 + jsonLength <= bytes.size(), "夹具 GLB JSON chunk 越界");
    auto document = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + jsonLength);
    mutate(document);
    std::string json = document.dump();
    while(json.size() % 4) json += ' ';
    const std::size_t binHeader = 20 + jsonLength;
    Bytes binary;
    if(binHeader + 8 <= bytes.size()) {
        std::uint32_t binLength = 0;
        std::memcpy(&binLength, bytes.data() + binHeader, 4);
        require(binHeader + 8 + binLength <= bytes.size(), "夹具 GLB BIN chunk 越界");
        binary.assign(bytes.begin() + binHeader + 8, bytes.begin() + binHeader + 8 + binLength);
    }
    const auto appendU32 = [](Bytes& output, std::uint32_t value) {
        for(unsigned i = 0; i < 4; ++i) output.push_back(static_cast<unsigned char>(value >> (i * 8)));
    };
    Bytes output;
    appendU32(output, 0x46546c67u);
    appendU32(output, 2u);
    appendU32(output, static_cast<std::uint32_t>(12 + 8 + json.size() + 8 + binary.size()));
    appendU32(output, static_cast<std::uint32_t>(json.size()));
    appendU32(output, 0x4e4f534au);
    output.insert(output.end(), json.begin(), json.end());
    appendU32(output, static_cast<std::uint32_t>(binary.size()));
    appendU32(output, 0x004e4942u);
    output.insert(output.end(), binary.begin(), binary.end());
    atomicWrite(file, output);
}

bool replaceFirstContentUri(Json& node, const std::string& uri) {
    if(node.is_object()) {
        if(node.contains("content") && node["content"].is_object() &&
           node["content"].contains("uri") && node["content"]["uri"].is_string()) {
            node["content"]["uri"] = uri;
            return true;
        }
        for(auto& item : node.items())
            if(replaceFirstContentUri(item.value(), uri)) return true;
    } else if(node.is_array()) {
        for(auto& item : node)
            if(replaceFirstContentUri(item, uri)) return true;
    }
    return false;
}

bool removeExtensionName(Json& document, const char* key, const std::string& name) {
    if(!document.contains(key) || !document[key].is_array()) return false;
    Json kept = Json::array();
    bool removed = false;
    for(const auto& item : document[key]) {
        if(item.is_string() && item.get<std::string>() == name) removed = true;
        else kept.push_back(item);
    }
    document[key] = kept;
    return removed;
}

fs::path treeGlb(const fs::path& root) {
    return firstFile(root, [](const fs::path& file) {
        return file.extension() == ".glb" && file.parent_path().filename() == "content";
    });
}

fs::path texturedGlb(const fs::path& root) {
    std::vector<fs::path> candidates;
    for(const auto& entry : fs::recursive_directory_iterator(root))
        if(entry.is_regular_file() && entry.path().extension() == ".glb" &&
           entry.path().parent_path().filename() == "content")
            candidates.push_back(entry.path());
    std::sort(candidates.begin(), candidates.end());
    for(const auto& file : candidates) {
        auto bytes = readBytes(file);
        require(bytes.size() >= 20, "夹具 GLB 头不完整");
        std::uint32_t jsonLength = 0;
        std::memcpy(&jsonLength, bytes.data() + 12, 4);
        auto document = Json::parse(bytes.begin() + 20, bytes.begin() + 20 + jsonLength);
        if(document.contains("images") && !document["images"].empty()) return file;
    }
    throw std::runtime_error("夹具未找到带图片的 GLB");
}

fs::path textureFile(const fs::path& root) {
    return firstFile(root, [](const fs::path& file) {
        auto extension = file.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return (extension == ".png" || extension == ".jpg" || extension == ".jpeg") &&
               file.parent_path().filename() == "textures";
    });
}
}

int main() {
    try {
        fs::path temp = fs::current_path() /
                        ("output-validation-data-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp);
        unsigned checks = 0;

        auto image = new osg::Image;
        image->allocateImage(2, 2, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        const unsigned char rgba[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 96};
        std::memcpy(image->data(), rgba, sizeof(rgba));
        image->setFileName((temp / "original.png").string());
        require(osgDB::writeImageFile(*image, image->getFileName()), "PNG fixture 输出");

        auto textured = triangle();
        auto geometry = textured->getDrawable(0)->asGeometry();
        auto uv = new osg::Vec2Array;
        uv->push_back({0, 0});
        uv->push_back({1, 0});
        uv->push_back({0, 1});
        geometry->setTexCoordArray(0, uv);
        auto texture = new osg::Texture2D(image);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        textured->getOrCreateStateSet()->setTextureAttributeAndModes(0, texture, osg::StateAttribute::ON);

        auto root = new osg::PagedLOD;
        root->setRangeMode(osg::LOD::PIXEL_SIZE_ON_SCREEN);
        root->addChild(triangle().get(), 0, 128);
        root->setFileName(1, "child.osgb");
        root->setRange(1, 128, 1e9);
        writeNode(textured.get(), temp / "tree/child.osgb");
        writeNode(root, temp / "tree/root.osgb");

        auto dense = new osg::Geode;
        auto denseGeometry = new osg::Geometry;
        auto densePositions = new osg::Vec3Array;
        for(unsigned i = 0; i < 600; ++i)
            densePositions->push_back({float(i % 20) * 0.25f, float(i / 20) * 0.25f, 0});
        denseGeometry->setVertexArray(densePositions);
        denseGeometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 600));
        dense->addDrawable(denseGeometry);
        writeNode(dense, temp / "dense.osgb");

        auto treeReport = convert(options(temp / "tree/root.osgb", temp / "out-tree"));
        require(treeReport["files"] == 2 && fs::exists(temp / "out-tree/tileset.json"), "基线转换");
        auto denseOptions = options(temp / "dense.osgb", temp / "out-dense");
        denseOptions.compressGeometry = true;
        auto denseReport = convert(denseOptions);
        require(denseReport["compressedViews"].get<unsigned>() > 0, "压缩夹具未产生压缩视图");

        // 成功校验只读：资产字节与大小不发生变化。
        auto before = snapshot(temp / "out-tree");
        auto stats = validateStagedOutput(temp / "out-tree");
        auto after = snapshot(temp / "out-tree");
        require(before == after, "成功校验改写了资产字节");
        require(stats.tilesets == 3 && stats.tiles == 6 && stats.contents == 2 && stats.images == 1,
                "成功校验统计与当前输出不符: tilesets=" + std::to_string(stats.tilesets) +
                    " tiles=" + std::to_string(stats.tiles) +
                    " contents=" + std::to_string(stats.contents) +
                    " images=" + std::to_string(stats.images));
        ++checks;

        const auto mutateCase = [&](const std::string& name, const std::function<void(const fs::path&)>& mutate,
                                    const std::string& expected) {
            fs::path target = temp / ("case-" + name);
            fs::remove_all(target);
            fs::copy(temp / "out-tree", target, fs::copy_options::recursive);
            mutate(target);
            auto message = expectFailure([&] { validateStagedOutput(target); });
            require(message.find(expected) != std::string::npos, name + " 失败原因不匹配: " + message);
            ++checks;
        };

        mutateCase("missing-glb", [](const fs::path& target) { fs::remove(treeGlb(target)); },
                   "引用的本地资产不存在");
        mutateCase("missing-image", [](const fs::path& target) { fs::remove(textureFile(target)); },
                   "引用的本地资产不存在");
        mutateCase("truncated-glb", [](const fs::path& target) {
            auto file = treeGlb(target);
            auto bytes = readBytes(file);
            bytes.resize(bytes.size() / 2);
            atomicWrite(file, bytes);
        }, "GLB 声明长度与文件大小不一致");
        mutateCase("out-of-range-view", [](const fs::path& target) {
            rewriteGlb(treeGlb(target), [](Json& document) {
                for(auto& view : document["bufferViews"]) {
                    if(view["buffer"] != 0 || view.contains("extensions")) continue;
                    auto length = view["byteLength"].get<std::uint64_t>();
                    auto buffer = document["buffers"][0]["byteLength"].get<std::uint64_t>();
                    view["byteLength"] = length + buffer + 4;
                    return;
                }
                throw std::runtime_error("夹具未找到原始 bufferView");
            });
        }, "bufferView 范围超出 buffer 0");
        mutateCase("wrong-content-uri", [](const fs::path& target) {
            rewriteJsonFile(target / "tileset.json", [](Json& document) {
                require(replaceFirstContentUri(document, "content/absent-fixture.glb"),
                        "夹具未找到 content.uri");
            });
        }, "引用的本地资产不存在");
        mutateCase("wrong-image-uri", [](const fs::path& target) {
            rewriteGlb(texturedGlb(target), [](Json& document) {
                require(document.contains("images"), "夹具 GLB 没有图片");
                document["images"][0]["uri"] = "../textures/absent-fixture.png";
            });
        }, "引用的本地资产不存在");
        mutateCase("same-image-conflicting-mime", [](const fs::path& target) {
            rewriteGlb(texturedGlb(target), [](Json& document) {
                auto duplicate = document["images"][0];
                duplicate["mimeType"] = duplicate["mimeType"] == "image/png" ? "image/jpeg" : "image/png";
                document["images"].push_back(duplicate);
            });
        }, "mimeType");
        mutateCase("invalid-material-texture-index", [](const fs::path& target) {
            rewriteGlb(texturedGlb(target), [](Json& document) {
                document["materials"][0]["pbrMetallicRoughness"]["baseColorTexture"]["index"] = document["textures"].size();
            });
        }, "baseColorTexture.index");
        mutateCase("invalid-error-type", [](const fs::path& target) {
            rewriteJsonFile(target / "tileset.json", [](Json& document) {
                document["root"]["geometricError"] = nullptr;
            });
        }, "geometricError 必须是数值");
        mutateCase("invalid-box-type", [](const fs::path& target) {
            rewriteJsonFile(target / "tileset.json", [](Json& document) {
                document["root"]["boundingVolume"]["box"][3] = nullptr;
            });
        }, "boundingVolume.box[3] 必须是数值");

        // 扩展声明与压缩视图必须一致，针对当前 meshopt 输出。
        const auto denseCase = [&](const std::string& name, const std::function<void(Json&)>& mutate,
                                   const std::string& expected) {
            fs::path target = temp / ("case-" + name);
            fs::remove_all(target);
            fs::copy(temp / "out-dense", target, fs::copy_options::recursive);
            rewriteGlb(treeGlb(target), mutate);
            auto message = expectFailure([&] { validateStagedOutput(target); });
            require(message.find(expected) != std::string::npos, name + " 失败原因不匹配: " + message);
            ++checks;
        };
        denseCase("missing-required-extension", [](Json& document) {
            require(removeExtensionName(document, "extensionsRequired", "EXT_meshopt_compression"),
                    "夹具缺少 required 扩展");
        }, "必须把 EXT_meshopt_compression 声明为 required");
        denseCase("missing-used-extension", [](Json& document) {
            require(removeExtensionName(document, "extensionsUsed", "EXT_meshopt_compression"),
                    "夹具缺少 used 扩展");
        }, "未在 extensionsUsed 声明");

        const auto triangleView = [](Json& document) -> Json& {
            auto& primitive = document["meshes"][0]["primitives"][0];
            auto& accessor = document["accessors"][primitive["indices"].get<unsigned>()];
            auto& view = document["bufferViews"][accessor["bufferView"].get<unsigned>()];
            require(view["extensions"]["EXT_meshopt_compression"]["mode"] == "TRIANGLES",
                    "夹具必须实际产生 TRIANGLES 压缩索引");
            return view;
        };
        denseCase("triangles-wrong-stride", [&](Json& document) {
            auto& e = triangleView(document)["extensions"]["EXT_meshopt_compression"];
            e["count"] = e["count"].get<unsigned>() * 2; e["byteStride"] = 1;
        }, "byteStride 不符合 mode");
        denseCase("triangles-incomplete-face", [&](Json& document) {
            auto& view = triangleView(document); auto& e = view["extensions"]["EXT_meshopt_compression"];
            e["count"] = e["count"].get<unsigned>() - 1;
            view["byteLength"] = e["count"].get<unsigned>() * e["byteStride"].get<unsigned>();
        }, "TRIANGLES 压缩视图 count 必须为 3 的倍数");
        for(unsigned mode : {0U, 1U, 2U, 3U, 5U, 6U})
            denseCase("triangles-non-triangle-" + std::to_string(mode), [&](Json& document) {
                triangleView(document);
                document["meshes"][0]["primitives"][0]["mode"] = mode;
            }, "TRIANGLES 压缩只能用于三角形 primitive");
        denseCase("triangles-accessor-type-mismatch", [&](Json& document) {
            triangleView(document);
            auto id = document["meshes"][0]["primitives"][0]["indices"].get<unsigned>();
            document["accessors"][id]["componentType"] = 5121;
        }, "索引压缩 byteStride 与 accessor 分量大小不一致");
        denseCase("triangles-partial-accessor", [&](Json& document) {
            triangleView(document);
            auto id = document["meshes"][0]["primitives"][0]["indices"].get<unsigned>();
            document["accessors"][id]["count"] = 597;
        }, "TRIANGLES 索引 accessor 必须完整引用压缩视图");
        denseCase("attributes-as-triangles", [](Json& document) {
            auto id = document["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<unsigned>();
            auto& view = document["bufferViews"][document["accessors"][id]["bufferView"].get<unsigned>()];
            auto& e = view["extensions"]["EXT_meshopt_compression"];
            e["mode"] = "TRIANGLES"; e["byteStride"] = 4;
            e["count"] = view["byteLength"].get<unsigned>() / 4;
        }, "属性必须引用 ATTRIBUTES 压缩视图");
        denseCase("indices-as-attributes", [&](Json& document) {
            auto& e = triangleView(document)["extensions"]["EXT_meshopt_compression"];
            e["mode"] = "ATTRIBUTES"; e["count"] = e["count"].get<unsigned>() / 2; e["byteStride"] = 4;
        }, "索引不能引用 ATTRIBUTES 压缩视图");

        // 直接构造非有限文档，覆盖 JSON 文本无法表达的 NaN/Inf 分支。
        Json synthetic;
        synthetic["asset"]["version"] = "1.1";
        synthetic["root"]["boundingVolume"]["box"] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
        synthetic["root"]["geometricError"] = std::numeric_limits<double>::infinity();
        synthetic["root"]["children"] = Json::array();
        auto syntheticMessage = expectFailure([&] {
            validateTilesetDocument(synthetic, temp / "synthetic/tileset.json");
        });
        require(syntheticMessage.find("不是有限数值") != std::string::npos,
                "直接文档 Inf 未命中有限性检查: " + syntheticMessage);
        Json nonFiniteBox;
        nonFiniteBox["asset"]["version"] = "1.1";
        nonFiniteBox["root"]["boundingVolume"]["box"] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
        nonFiniteBox["root"]["boundingVolume"]["box"][3] = std::numeric_limits<double>::quiet_NaN();
        nonFiniteBox["root"]["geometricError"] = 0;
        nonFiniteBox["root"]["children"] = Json::array();
        auto boxMessage = expectFailure([&] {
            validateTilesetDocument(nonFiniteBox, temp / "synthetic-box/tileset.json");
        });
        require(boxMessage.find("不是有限数值") != std::string::npos,
                "直接文档 NaN 未命中有限性检查: " + boxMessage);
        ++checks;

        // 发布门禁：在最后一次取消检查（tileset.json 已写出、尚未 commit）破坏暂存 GLB。
        Options injected = options(temp / "tree/root.osgb", temp / "out-injected");
        bool corrupted = false;
        injected.checkCancelled = [&] {
            if(corrupted) return;
            for(const auto& entry : fs::directory_iterator(temp)) {
                auto name = entry.path().filename().string();
                if(name.rfind(".out-injected.building-", 0) != 0) continue;
                if(!fs::exists(entry.path() / "tileset.json")) continue;
                std::vector<fs::path> glbs;
                for(const auto& file : fs::recursive_directory_iterator(entry.path()))
                    if(file.is_regular_file() && file.path().extension() == ".glb") glbs.push_back(file.path());
                std::sort(glbs.begin(), glbs.end());
                if(glbs.empty()) continue;
                fs::remove(glbs.front());
                corrupted = true;
                return;
            }
        };
        bool published = false;
        auto injectionMessage = expectFailure([&] {
            convert(injected);
            published = true;
        });
        require(corrupted, "故障注入未触及发布前暂存目录");
        require(!published && !fs::exists(temp / "out-injected"), "校验失败后仍然发布了输出");
        require(injectionMessage.find("发布前校验失败") != std::string::npos,
                "发布门禁未触发完整性校验: " + injectionMessage);
        for(const auto& entry : fs::directory_iterator(temp))
            require(entry.path().filename().string().rfind(".out-injected.building-", 0) != 0,
                    "失败暂存目录必须清理");
        ++checks;

        std::cout << "通过 " << checks << " 项发布前完整性校验回归；测试数据: " << temp << '\n';
        return 0;
    } catch(const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
