#include "native_tiles.hpp"
#include "checkpoint.hpp"
#include "product_metadata.hpp"
#include <osgDB/Registry>
#include <osg/Version>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#include <link.h>
#endif

namespace nt {
Json capabilities() {
    Options defaults;
    return {{"name","osgb-tiles"},{"version",kReleaseVersion},{"protocolVersion",1},
        {"jsonEvents",true},{"cancelFile",true},{"pauseResume",true},{"rebuildTop",true},{"topSealDistance",true},{"topAlgorithm","ancestor-rebuild-v1"},
        {"preview",true},{"previewAlgorithm","mesh-preview-v2"},{"previewDefaults",{{"streamingLod",true},{"geometry","meshopt+quantized"},{"texture","original"}}},
        {"geometryFormats",{"raw","meshopt","draco"}},{"textureFormats",{"original","uastc","etc1s"}},{"embedTextures",true},
        {"dracoSpeeds",{0,5,10}},{"codecs",{{"draco","1.5.7"},{"basisUniversal","1.16.4"}}},
        {"defaults",{{"threads",defaults.threads},{"geometry",defaults.compressGeometry?"meshopt":"raw"},
                     {"memoryBudgetMiB",0},{"texturePixels",false},{"embedTextures",false},{"rebuildTop",false},{"preview",false},{"topSealDistance",2.0},{"textureFormat","original"},{"dracoSpeed",5}}}};
}
namespace {
fs::path executablePath(const fs::path& executable) {
    fs::path actual;
#ifdef _WIN32
    std::wstring path(32768,L'\0');auto size=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(size>0 && size<path.size()) {path.resize(size);actual=path;}
#elif defined(__APPLE__)
    std::uint32_t size=0;_NSGetExecutablePath(nullptr,&size);std::string path(size,'\0');
    if(_NSGetExecutablePath(path.data(),&size)==0) actual=fs::canonical(fs::u8path(path.c_str()));
#elif defined(__linux__)
    std::string path(4096,'\0');auto size=readlink("/proc/self/exe",path.data(),path.size());
    if(size>0 && static_cast<std::size_t>(size)<path.size()) {path.resize(size);actual=fs::u8path(path);}
#endif
    if(actual.empty() && !executable.empty()) actual=fs::absolute(executable);
    if(actual.empty()) throw std::runtime_error("无法确定转换器二进制身份");
    return actual;
}
Json loadedModules() {
    std::vector<fs::path> paths;
#ifdef __APPLE__
    for(std::uint32_t i=0;i<_dyld_image_count();++i) paths.push_back(fs::u8path(_dyld_get_image_name(i)));
#elif defined(__linux__)
    dl_iterate_phdr([](dl_phdr_info* info,std::size_t,void* data) {
        if(info->dlpi_name && *info->dlpi_name) static_cast<std::vector<fs::path>*>(data)->push_back(fs::u8path(info->dlpi_name));
        return 0;
    },&paths);
#elif defined(_WIN32)
    DWORD needed=0;auto process=GetCurrentProcess();
    if(!EnumProcessModules(process,nullptr,0,&needed)) throw std::runtime_error("无法枚举转换器运行库");
    std::vector<HMODULE> modules(needed/sizeof(HMODULE));
    if(!EnumProcessModules(process,modules.data(),needed,&needed)) throw std::runtime_error("无法读取转换器运行库");
    for(auto module:modules) {
        std::wstring name(32768,L'\0');auto size=GetModuleFileNameW(module,name.data(),static_cast<DWORD>(name.size()));
        if(!size || size>=name.size()) throw std::runtime_error("无法确定转换器运行库路径");
        name.resize(size);paths.emplace_back(name);
    }
#endif
    // OS shared-cache / virtual modules have no disk file. Hash every loaded
    // on-disk module, including development SDK libraries and OSG plugins.
    std::set<fs::path> seen;std::map<std::string,Json> sorted;
    for(const auto& path:paths) if(fs::is_regular_file(path)) {
        auto canonical=fs::canonical(path);if(!seen.insert(canonical).second) continue;
        auto value=fileIdentity(canonical);auto name=canonical.filename().u8string();
        sorted[name+value.at("sha256").get<std::string>()]={{"name",name},{"content",value}};
    }
    Json result=Json::array();for(const auto& item:sorted) result.push_back(item.second);return result;
}
}
Json checkpointConverterIdentity() {
    auto actual=executablePath({});
    Json identity={{"version",kReleaseVersion},{"binary",fileIdentity(actual)},{"osg",osgGetVersion()},{"runtimeModules",loadedModules()}};
    auto manifest=actual.parent_path()/"native-runtime.json";
    if(fs::is_regular_file(manifest)) {
        auto runtime=Json::parse(readBytes(manifest));identity["runtimeManifest"]=fileIdentity(manifest);
        for(auto it=runtime.at("files").begin();it!=runtime.at("files").end();++it)
            identity["runtimeFiles"][it.key()]=fileIdentity(actual.parent_path().parent_path()/fs::u8path(it.key()));
    }
    return identity;
}
void configureRuntime(const fs::path& executable) {
    auto actual=executablePath(executable);
    auto manifest=actual.parent_path()/"native-runtime.json";
    if(!fs::is_regular_file(manifest)) return; // 构建目录用于开发；分发包必须带清单。
    std::ifstream input(manifest);auto runtime=Json::parse(input);
    if(runtime.at("protocolVersion")!=1) throw std::runtime_error("不支持的 native 运行时清单");
    auto base=actual.parent_path();auto registry=osgDB::Registry::instance();
    osgDB::FilePathList directories;
    for(const auto& relative:runtime.at("pluginDirectories")) {
        auto dir=(base/fs::u8path(relative.get<std::string>())).lexically_normal().make_preferred();
        directories.push_back(dir.u8string());
    }
    registry->setLibraryFilePathList(directories);
    // 主动加载随包插件；不让开发机插件把缺失资源掩盖成可用。
    for(const auto& relative:runtime.at("plugins")) {
        auto plugin=(base/fs::u8path(relative.get<std::string>())).lexically_normal().make_preferred();
        if(!fs::is_regular_file(plugin) || registry->loadLibrary(plugin.u8string())==osgDB::Registry::NOT_LOADED)
            throw std::runtime_error("无法加载随包 OSG 插件: "+plugin.u8string());
    }
    for(const auto* extension:{"osgb","png","jpg"})
        if(!registry->getReaderWriterForExtension(extension)) throw std::runtime_error(std::string("缺少必需 OSG 读取器: ")+extension);
}
}
