#include "native_tiles.hpp"
#include "output_validation.hpp"
#include "checkpoint.hpp"
#include "top.hpp"
#include "preview.hpp"
#include "product_metadata.hpp"
#include <osg/Geode>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osgDB/ReadFile>
#include <osgDB/FileUtils>
#include <osgDB/Registry>
#include <libxml/parser.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <deque>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/stat.h>
#include <unistd.h>
#elif defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <wchar.h>
#endif

namespace nt {
// 仅用于内存预算门控与报告；采样失败返回 0，不能把 0 当成真实占用。
std::uint64_t residentBytes() {
#ifdef __APPLE__
    mach_task_basic_info info{};mach_msg_type_number_t count=MACH_TASK_BASIC_INFO_COUNT;
    if(task_info(mach_task_self(),MACH_TASK_BASIC_INFO,reinterpret_cast<task_info_t>(&info),&count)!=KERN_SUCCESS) return 0;
    return info.resident_size;
#elif defined(__linux__)
    std::ifstream in("/proc/self/statm");std::uint64_t pages=0,resident=0;
    if(!(in>>pages>>resident)) return 0;
    return resident*static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS info{};
    if(!GetProcessMemoryInfo(GetCurrentProcess(),&info,sizeof(info))) return 0;
    return info.WorkingSetSize;
#else
    return 0;
#endif
}
namespace {
// 峰值只在调度点采样会错过任务中峰值；后台按固定间隔取最大值，报告值不依赖采样时机。
class ResidentSampler {
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> peak_{0};
    std::thread worker_;
public:
    ResidentSampler() {
        peak_.store(residentBytes());
        worker_=std::thread([this] {
            while(!stop_.load(std::memory_order_relaxed)) {
                auto value=residentBytes();auto current=peak_.load(std::memory_order_relaxed);
                while(value>current && !peak_.compare_exchange_weak(current,value,std::memory_order_relaxed)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });
    }
    ~ResidentSampler() {stop_.store(true);if(worker_.joinable()) worker_.join();}
    std::uint64_t peak() const {return peak_.load(std::memory_order_relaxed);}
};
}
osg::Matrixd enuTransform(double latitude,double longitude,double height,const osg::Vec3d& origin) {
    if(!std::isfinite(latitude)||!std::isfinite(longitude)||!std::isfinite(height)||std::abs(latitude)>90||std::abs(longitude)>180)
        throw std::runtime_error("无效的 ENU 经纬度或高度");
    constexpr double pi=3.14159265358979323846, a=6378137.0, e2=6.6943799901413165e-3;
    double lat=latitude*pi/180,lon=longitude*pi/180,s=std::sin(lat),c=std::cos(lat),sl=std::sin(lon),cl=std::cos(lon);
    double n=a/std::sqrt(1-e2*s*s);
    osg::Matrixd enu(-sl,cl,0,0, -s*cl,-s*sl,c,0, c*cl,c*sl,s,0,
                    (n+height)*c*cl,(n+height)*c*sl,(n*(1-e2)+height)*s,1);
    return osg::Matrixd::translate(origin)*enu;
}
namespace {
struct External {fs::path file;osg::Matrixd matrix;};
struct Pending {
    Json json=Json::object();osg::BoundingBoxd bounds;double error=0;
    std::vector<External> external;
    // Split branches are materialized before the parent is checkpointed. Restore
    // their source records too, without adding their tiles a second time.
    std::vector<External> dependencies;
};
struct FileResult {fs::path file;osg::BoundingBoxd bounds;double error=0;Json root;};
fs::path stripTrailingSeparator(fs::path p) {
    while(!p.empty() && p.filename().empty() && p!=p.root_path()) {
        auto parent=p.parent_path();
        if(parent==p) break;
        p=std::move(parent);
    }
    return p;
}
bool sameVolume(const fs::path& a,const fs::path& b) {
#ifdef _WIN32
    auto volumeOf=[](const fs::path& p)->std::wstring {
        std::wstring abs=fs::absolute(p).native();
        std::vector<wchar_t> buf(std::max<std::size_t>(abs.size()+64,512),L'\0');
        if(GetVolumePathNameW(abs.c_str(),buf.data(),static_cast<DWORD>(buf.size()))) return std::wstring(buf.data());
        return fs::absolute(p).root_name().native();
    };
    auto va=volumeOf(a),vb=volumeOf(b);
    if(va.empty() || vb.empty()) return false;
    return _wcsicmp(va.c_str(),vb.c_str())==0;
#else
    struct stat sa{},sb{};
    std::error_code ec;
    auto pa=fs::exists(a,ec)?a:a.parent_path();
    auto pb=fs::exists(b,ec)?b:b.parent_path();
    if(pa.empty() || pb.empty()) return false;
    if(::stat(pa.c_str(),&sa)!=0 || ::stat(pb.c_str(),&sb)!=0) return false;
    return sa.st_dev==sb.st_dev;
#endif
}
bool isRetryableWindowsFsError(const std::error_code& error) {
#ifdef _WIN32
    return error.value()==ERROR_ACCESS_DENIED || error.value()==ERROR_SHARING_VIOLATION;
#else
    (void)error;
    return false;
#endif
}
bool isCrossDeviceOrWindowsDirRenameError(const std::error_code& error) {
    if(error==std::errc::cross_device_link) return true;
#ifdef _WIN32
    return error.value()==ERROR_NOT_SAME_DEVICE || error.value()==ERROR_ACCESS_DENIED;
#else
    return false;
#endif
}
template <typename Op>
void retryWindowsFsOp(Op&& op,std::error_code& error,const std::function<void()>& check) {
    op(error);
    for(int attempt=0;attempt<20 && isRetryableWindowsFsError(error);++attempt) {
        if(check) check();
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(250,50*(attempt+1))));
        error.clear();
        op(error);
    }
}
void renameWithRetry(const fs::path& from,const fs::path& to,std::error_code& error,const std::function<void()>& check={}) {
    retryWindowsFsOp([&](std::error_code& ec) {fs::rename(from,to,ec);},error,check);
}
void removeEmptyDirWithRetry(const fs::path& dir,std::error_code& error,const std::function<void()>& check={}) {
    retryWindowsFsOp([&](std::error_code& ec) {fs::remove(dir,ec);},error,check);
}
void publishIntoExistingEmptyDir(const fs::path& source,const fs::path& destination,const std::function<void()>& check) {
    if(!fs::is_directory(destination) || !fs::is_empty(destination))
        throw std::runtime_error("发布前输出目录被占用，拒绝覆盖");
    std::vector<fs::path> entries;
    bool hasReport=false,hasTileset=false;
    for(const auto& item:fs::directory_iterator(source)) {
        auto name=item.path().filename().u8string();
        if(name=="tileset.json") {hasTileset=true;continue;}
        if(name=="conversion-report.json") {hasReport=true;continue;}
        entries.push_back(item.path().filename());
    }
    std::sort(entries.begin(),entries.end());
    if(hasReport) entries.push_back(fs::u8path("conversion-report.json"));
    if(hasTileset) entries.push_back(fs::u8path("tileset.json"));
    std::vector<fs::path> moved;
    try {
        for(const auto& name:entries) {
            if(check) check();
            std::error_code error;
            renameWithRetry(source/name,destination/name,error,check);
            if(error) throw fs::filesystem_error("无法发布转换结果",source/name,destination/name,error);
            moved.push_back(name);
        }
    } catch(...) {
        // tileset.json 最后移入；出错时回滚已移入条目，避免输出目录残留半成品。
        for(auto it=moved.rbegin();it!=moved.rend();++it) {
            std::error_code undoErr;
            fs::rename(destination/(*it),source/(*it),undoErr);
            if(undoErr) fs::remove_all(destination/(*it),undoErr);
        }
        throw;
    }
}
// 仅持有本次创建的暂存目录；失败清理自身产物，完成后整体发布，允许原路径直接重跑。
class OutputTransaction {
    fs::path destination_,staging_,publication_;bool committed_=false,keep_=false;
    void checkDestination() {
        if(fs::exists(destination_) && (!fs::is_directory(destination_) || !fs::is_empty(destination_)))
            throw std::runtime_error("发布前输出目录被占用，拒绝覆盖");
    }
    bool publishStagedDir(const fs::path& staged,const std::function<void()>& check,std::error_code& error) {
        if(fs::exists(destination_)) {
            std::error_code rmError;
            removeEmptyDirWithRetry(destination_,rmError,check);
            if(rmError) {
                if(isRetryableWindowsFsError(rmError) && fs::is_directory(destination_) && fs::is_empty(destination_)) {
                    publishIntoExistingEmptyDir(staged,destination_,check);
                    return true;
                }
                error=rmError;
                return false;
            }
        }
        renameWithRetry(staged,destination_,error,check);
        return !error;
    }
public:
    explicit OutputTransaction(const fs::path& destination,const fs::path& retained={}):destination_(stripTrailingSeparator(destination)) {
        auto parent=destination_.parent_path().empty()?fs::path("."):destination_.parent_path();
        fs::create_directories(parent);
        auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        publication_=parent/fs::u8path("."+destination_.filename().u8string()+".building-"+std::to_string(stamp));
        staging_=retained.empty()?publication_:retained;
        if(retained.empty() && !fs::create_directory(staging_)) throw std::runtime_error("无法创建独立转换暂存目录");
    }
    ~OutputTransaction() {
        std::error_code error;
        if(!committed_ && !keep_) fs::remove_all(staging_,error);
        if(publication_!=staging_) fs::remove_all(publication_,error);
    }
    const fs::path& staging() const {return staging_;}
    void preserve() {keep_=true;}
    void commit(const std::function<void()>& check={},const std::function<void()>& beforePublish={}) {
        if(check) check();if(beforePublish) beforePublish();if(check) check();checkDestination();
        const auto parent=destination_.parent_path().empty()?fs::path("."):destination_.parent_path();
        const bool onSameVolume=sameVolume(staging_,parent);
        std::error_code error;
        if(onSameVolume && publishStagedDir(staging_,check,error)) {
            std::error_code cleanupError;
            fs::remove_all(staging_,cleanupError);
            committed_=true;
            return;
        }
        if(!onSameVolume || (staging_!=publication_ && isCrossDeviceOrWindowsDirRenameError(error))) {
            if(!fs::create_directory(publication_)) throw std::runtime_error("无法创建发布暂存目录");
            for(const auto& file:fs::recursive_directory_iterator(staging_)) {
                if(check) check();
                auto target=publication_/file.path().lexically_relative(staging_);
                if(file.is_directory()) fs::create_directories(target);
                else {fs::create_directories(target.parent_path());fs::copy_file(file.path(),target);}
            }
            if(check) check();if(beforePublish) beforePublish();if(check) check();checkDestination();
            std::error_code publishError;
            if(!publishStagedDir(publication_,check,publishError)) {
                throw fs::filesystem_error("无法发布转换结果",publication_,destination_,publishError);
            }
            std::error_code cleanupError;
            fs::remove_all(staging_,cleanupError);
            committed_=true;
            return;
        }
        throw fs::filesystem_error("无法发布转换结果",staging_,destination_,error);
    }
};
struct PauseRequested {};
struct ResumeState {
    Checkpoint& checkpoint;const Options& options;const Json& inputs;
    std::atomic<bool> paused{false};std::mutex mutex;std::set<std::string> reused;
    ResumeState(Checkpoint& c,const Options& o,const Json& i):checkpoint(c),options(o),inputs(i) {}
    void check() {
        if(options.checkCancelled) options.checkCancelled();
        if(!options.pauseFile.empty() && fs::exists(options.pauseFile)) paused.store(true);
        if(paused.load()) throw PauseRequested{};
    }
    void checkSource(const fs::path& file) {
        auto found=inputs.find(file.u8string());
        if(found==inputs.end() || fileIdentity(file)!=*found) throw std::runtime_error("检查点输入内容发生变化: "+file.u8string());
    }
    void reusedFile(const std::string& key) {
        if(checkpoint.wasResumed(key)) {std::lock_guard<std::mutex> lock(mutex);reused.insert(key);}
    }
};
Json savedBounds(const osg::BoundingBoxd& bounds) {
    if(!bounds.valid()) return nullptr;
    return {bounds.xMin(),bounds.yMin(),bounds.zMin(),bounds.xMax(),bounds.yMax(),bounds.zMax()};
}
osg::BoundingBoxd restoredBounds(const Json& value) {
    if(value.is_null()) return {};
    auto b=value.get<std::vector<double>>();if(b.size()!=6) throw std::runtime_error("检查点包围盒错误");
    return {b[0],b[1],b[2],b[3],b[4],b[5]};
}
std::vector<std::string> recordAssets(const Json& record,bool complete) {
    std::vector<std::string> assets;
    for(const auto& content:record.at("content")) {
        assets.push_back(content.at("uri"));
        for(const auto& image:content.at("images")) if(image.contains("uri")) assets.push_back(image.at("uri"));
    }
    if(complete) assets.push_back(record.at("tileset"));
    return assets;
}
osg::ref_ptr<osg::StateSet> inherited(osg::StateSet* parent,osg::StateSet* local) {
    auto result=new osg::StateSet;if(parent) result->merge(*parent);if(local) result->merge(*local);return result;
}
std::string fileKey(const fs::path& file) {
    auto s=file.generic_u8string();return contentKey(Bytes(s.begin(),s.end()));
}
fs::path resolveReference(const fs::path& source,const std::string& db,const std::string& name) {
    std::string normalized=name,normalizedDb=db;
    std::replace(normalized.begin(),normalized.end(),'\\','/');std::replace(normalizedDb.begin(),normalizedDb.end(),'\\','/');
    fs::path ref=fs::u8path(normalized);
    std::vector<fs::path> candidates;
    if(ref.is_absolute()) candidates.push_back(ref);
    else {
        if(!normalizedDb.empty()) {fs::path d=fs::u8path(normalizedDb);candidates.push_back((d.is_absolute()?d:source.parent_path()/d)/ref);}
        candidates.push_back(source.parent_path()/ref);
    }
    for(const auto& candidate:candidates) if(fs::is_regular_file(candidate)) return fs::canonical(candidate);
    throw std::runtime_error("缺失 OSGB 分页引用: "+source.u8string()+" -> "+name);
}
// OSG 序列化会省略外部孩子，并把常驻孩子压缩到数组前部；完整保留预载孩子时才按原槽位跳过。
bool compactExternalChildren(unsigned children,unsigned files) {return children<files;}
struct Engine {
    const Options& options;SpatialReference spatial;std::map<std::string,FileResult> cache;std::set<fs::path> active;
    Json records=Json::array();ResumeState* resume=nullptr;
    Engine(const Options& value,const osg::Vec3d& origin,ResumeState* state=nullptr):options(value),spatial(value.srs,origin),resume(state) {}
    std::string instanceKey(const fs::path& file,const osg::Matrixd& matrix) const {
        if(matrix.isIdentity()) return fileKey(file);
        const auto* p=reinterpret_cast<const unsigned char*>(matrix.ptr());
        return fileKey(file)+"-"+contentKey(Bytes(p,p+16*sizeof(double)));
    }
    // 一个源文件的常驻几何构成一个 REPLACE 单元，分页分支不能各自降低细化阈值。
    void collect(osg::Node* node,const osg::Matrixd& parent,osg::StateSet* previous,const fs::path& source,
                 std::vector<GeometryInput>& meshes,Pending& out) {
        if(node->getNodeMask()==0) return;
        osg::Matrixd matrix=parent;
        if(auto transform=dynamic_cast<osg::Transform*>(node)) {
            if(transform->getReferenceFrame()!=osg::Transform::RELATIVE_RF) throw std::runtime_error("不支持绝对参考系节点");
            if(!transform->computeLocalToWorldMatrix(matrix,nullptr)) throw std::runtime_error("无法求节点局部矩阵");
        }
        auto state=inherited(previous,node->getStateSet());
        if(auto geode=dynamic_cast<osg::Geode*>(node)) {
            for(unsigned i=0;i<geode->getNumDrawables();++i) {
                auto g=geode->getDrawable(i)->asGeometry();if(!g) throw std::runtime_error("OSGB 包含非 Geometry Drawable");
                meshes.push_back({g,inherited(state.get(),g->getStateSet()),matrix});
            }
            return;
        }
        if(auto g=dynamic_cast<osg::Geometry*>(node)) {meshes.push_back({g,state,matrix});return;}
        if(auto paged=dynamic_cast<osg::PagedLOD*>(node)) {
            unsigned resident=0;
            for(unsigned i=0;i<paged->getNumFileNames();++i) if(!paged->getFileName(i).empty())
                out.external.push_back({resolveReference(source,paged->getDatabasePath(),paged->getFileName(i)),matrix});
            bool compact=compactExternalChildren(paged->getNumChildren(),paged->getNumFileNames());
            for(unsigned i=0;i<paged->getNumChildren();++i) {
                // 已预载的外部细层仍按文件引用处理，不能同时并入粗层几何。
                if(!compact && i<paged->getNumFileNames() && !paged->getFileName(i).empty()) continue;
                if(++resident>1) throw std::runtime_error("多个无文件引用的常驻 LOD 分支尚未映射: "+source.u8string());
                collect(paged->getChild(i),matrix,state.get(),source,meshes,out);
            }
            return;
        }
        if(auto proxy=dynamic_cast<osg::ProxyNode*>(node)) {
            for(unsigned i=0;i<proxy->getNumFileNames();++i) if(!proxy->getFileName(i).empty())
                out.external.push_back({resolveReference(source,proxy->getDatabasePath(),proxy->getFileName(i)),matrix});
            bool compact=compactExternalChildren(proxy->getNumChildren(),proxy->getNumFileNames());
            for(unsigned i=0;i<proxy->getNumChildren();++i)
                if(compact || i>=proxy->getNumFileNames() || proxy->getFileName(i).empty()) collect(proxy->getChild(i),matrix,state.get(),source,meshes,out);
            return;
        }
        if(auto lod=dynamic_cast<osg::LOD*>(node)) if(lod->getNumChildren()>1) throw std::runtime_error("多个常驻 LOD 分支需要显式范围映射");
        if(auto group=node->asGroup()) for(unsigned i=0;i<group->getNumChildren();++i)
            collect(group->getChild(i),matrix,state.get(),source,meshes,out);
    }
    Json finish(Pending& tile) {
        Json children=Json::array();double childError=0;std::set<std::string> references;
        for(const auto& external:tile.external) {
            if(!references.insert(instanceKey(external.file,external.matrix)).second) continue;
            auto child=convertFile(external.file,external.matrix);
            children.push_back(child.root);
            tile.bounds.expandBy(child.bounds);childError=std::max(childError,child.error);
        }
        if(!tile.bounds.valid()) throw std::runtime_error("节点既无有效几何也无可达分页内容");
        double error=tile.error;
        if(!children.empty()) {
            double extent=std::max({tile.bounds.xMax()-tile.bounds.xMin(),tile.bounds.yMax()-tile.bounds.yMin(),tile.bounds.zMax()-tile.bounds.zMin()});
            // 仅按源文件层级递推，纯索引文件也继承下一层误差；无人工分组层。
            error=childError>0?childError*2.0:extent/50.0;
        }
        tile.json["boundingVolume"]=boundingBoxJson(tile.bounds);tile.json["geometricError"]=error;tile.json["refine"]=tile.json.value("refine","REPLACE");
        tile.error=error;
        if(!children.empty()) tile.json["children"]=children;
        return tile.json;
    }
    FileResult convertFile(fs::path file,const osg::Matrixd& sourceTransform=osg::Matrixd::identity()) {
        if(options.checkCancelled) options.checkCancelled();
        file=fs::canonical(file);auto key=instanceKey(file,sourceTransform);
        if(active.count(file)) throw std::runtime_error("检测到循环 OSGB 引用: "+file.u8string());
        if(auto it=cache.find(key);it!=cache.end()) return it->second;
        if(resume) resume->check();
        auto entryLock=resume?resume->checkpoint.lockEntry(key):std::unique_lock<std::mutex>();
        if(resume) resume->check();
        active.insert(file);Pending pending;Json contents=Json::array(),saved;
        fs::path manifest=options.output/"tiles"/(key+".json");
        const auto matrixValues=std::vector<double>(sourceTransform.ptr(),sourceTransform.ptr()+16);
        if(resume) saved=resume->checkpoint.entry(key);
        if(!saved.is_null()) {
            const auto& prepared=saved.at("prepared");const auto& record=prepared.at("record");
            if(record.at("source")!=file.u8string() || record.at("sourceTransform")!=matrixValues) throw std::runtime_error("检查点文件实例键冲突");
            pending.json=prepared.at("json");pending.bounds=restoredBounds(prepared.at("bounds"));pending.error=prepared.value("error",0.0);
            for(const auto& ref:prepared.value("dependencies",Json::array())) {
                auto values=ref.at("matrix").get<std::vector<double>>();
                if(values.size()!=16) throw std::runtime_error("检查点依赖矩阵错误");
                pending.dependencies.push_back({fs::u8path(ref.at("file").get<std::string>()),osg::Matrixd(values.data())});
            }
            for(const auto& ref:prepared.at("external")) {
                auto matrix=ref.at("matrix").get<std::vector<double>>();if(matrix.size()!=16) throw std::runtime_error("检查点矩阵错误");
                pending.external.push_back({fs::u8path(ref.at("file").get<std::string>()),osg::Matrixd(matrix.data())});
            }
            contents=record.at("content");resume->reusedFile(key);
        } else {
            if(resume) resume->checkSource(file);
            osg::ref_ptr<osgDB::Options> readOptions=new osgDB::Options;
            readOptions->setObjectCacheHint(osgDB::Options::CACHE_NONE);
            auto node=osgDB::readRefNodeFile(file.u8string(),readOptions.get());
            if(!node) throw std::runtime_error("OSGB 读取失败: "+file.u8string());
            std::vector<GeometryInput> meshes;
            if(options.splitPagedLod) {
                auto group=node->asGroup();auto state=group?inherited(nullptr,group->getStateSet()):osg::ref_ptr<osg::StateSet>();
                std::vector<osg::PagedLOD*> branches;
                if(group) for(unsigned i=0;i<group->getNumChildren();++i) {
                    auto paged=dynamic_cast<osg::PagedLOD*>(group->getChild(i));if(paged) {
                        bool hasExternal=false;
                        for(unsigned child=0;child<paged->getNumFileNames();++child) hasExternal=hasExternal||!paged->getFileName(child).empty();
                        if(!hasExternal) {branches.clear();break;}
                        branches.push_back(paged);
                    }
                }
                if(dynamic_cast<osg::Transform*>(node.get())) branches.clear();
                if(group && branches.size()==group->getNumChildren() && branches.size()>1) {
                    // A contentless structural parent groups independent REPLACE branches.
                    // Each branch owns its coarse fallback exactly once; duplicating the
                    // file-level coarse mesh wastes storage and pins both copies in Cesium.
                    Json children=Json::array();
                    std::vector<std::pair<Pending,std::vector<GeometryInput>>> parts(branches.size());
                    // Complete dependencies before writing any parent assets. Child
                    // checkpoint commits must not observe unregistered parent GLBs.
                    for(std::size_t i=0;i<branches.size();++i) {
                        collect(branches[i],sourceTransform,state.get(),file,parts[i].second,parts[i].first);
                        for(const auto& ref:parts[i].first.external) convertFile(ref.file,ref.matrix);
                    }
                    for(std::size_t i=0;i<branches.size();++i) {
                        auto& part=parts[i].first;auto& partMeshes=parts[i].second;
                        if(!partMeshes.empty()) {
                            auto name=key+"-split-"+std::to_string(i)+".glb";ContentResult result;
                            try {result=writeGlb(partMeshes,options.output/"content"/name,options.output/"textures",options.preserveEncoded,spatial.projected()?&spatial:nullptr,options.compressGeometry,options.quantizeGeometry,&options);}
                            catch(const std::exception& error) {throw std::runtime_error("内容转换失败 ["+file.u8string()+" 分支 "+std::to_string(i)+"]: "+error.what());}
                            part.bounds=result.bounds;part.json["content"]={{"uri","../content/"+name}};
                            contents.push_back({{"uri","content/"+name},{"vertices",result.vertices},{"triangles",result.triangles},
                                {"bytes",result.bytes},{"coordinateRoundingErrorMeters",result.coordinateRoundingError},
                                {"geometryRawBytes",result.geometryRawBytes},{"geometryStoredBytes",result.geometryStoredBytes},
                                {"compressedViews",result.compressedViews},{"images",result.imageAudit},
                                {"originalFloatBytes",result.originalFloatBytes},{"quantizationErrorMeters",result.quantizationErrorMeters},{"draco",result.dracoAudit}});
                        }
                        auto child=finish(part);pending.bounds.expandBy(part.bounds);
                        pending.dependencies.insert(pending.dependencies.end(),part.external.begin(),part.external.end());
                        pending.error=std::max(pending.error,part.error);children.push_back(child);
                    }
                    pending.json["children"]=children;
                } else collect(node.get(),sourceTransform,nullptr,file,meshes,pending);
            } else collect(node.get(),sourceTransform,nullptr,file,meshes,pending);
            if(!meshes.empty()) {
                auto name=key+".glb";
                ContentResult result;
                try {result=writeGlb(meshes,options.output/"content"/name,options.output/"textures",options.preserveEncoded,spatial.projected()?&spatial:nullptr,options.compressGeometry,options.quantizeGeometry,&options);}
                catch(const std::exception& error) {throw std::runtime_error("内容转换失败 ["+file.u8string()+"]: "+error.what());}
                pending.bounds=result.bounds;pending.json["content"]={{"uri","../content/"+name}};
                contents.push_back({{"uri","content/"+name},{"vertices",result.vertices},{"triangles",result.triangles},
                    {"bytes",result.bytes},{"coordinateRoundingErrorMeters",result.coordinateRoundingError},
                    {"geometryRawBytes",result.geometryRawBytes},{"geometryStoredBytes",result.geometryStoredBytes},
                    {"compressedViews",result.compressedViews},{"images",result.imageAudit},
                    {"originalFloatBytes",result.originalFloatBytes},{"quantizationErrorMeters",result.quantizationErrorMeters},{"draco",result.dracoAudit}});
            }
        } // 在进入子文件前释放源节点；跨文件矩阵在投影前应用，避免误把 ENU 结果乘 UTM 矩阵。
        Json record={{"source",file.u8string()},{"sourceBytes",fs::file_size(file)},
                     {"sourceTransform",matrixValues},{"tileset","tiles/"+manifest.filename().u8string()},{"content",contents}};
        if(resume && saved.is_null()) {
            resume->checkSource(file);Json references=Json::array();
            for(const auto& ref:pending.external) references.push_back({{"file",ref.file.u8string()},
                {"matrix",std::vector<double>(ref.matrix.ptr(),ref.matrix.ptr()+16)}});
            Json dependencies=Json::array();
            for(const auto& ref:pending.dependencies) dependencies.push_back({{"file",ref.file.u8string()},
                {"matrix",std::vector<double>(ref.matrix.ptr(),ref.matrix.ptr()+16)}});
            saved={{"complete",false},{"prepared",{{"record",record},{"json",pending.json},
                {"bounds",savedBounds(pending.bounds)},{"error",pending.error},{"external",references},{"dependencies",dependencies}}}};
            resume->checkpoint.save(key,saved,recordAssets(record,false));
            if(options.event) options.event({{"type","file-complete"}});
        }
        if(resume) resume->check();
        for(const auto& dependency:pending.dependencies) convertFile(dependency.file,dependency.matrix);
        Json root=finish(pending);double error=root["geometricError"].get<double>();
        Json tile={{"asset",{{"version","1.1"},{"generator",kAssetGenerator}}},{"geometricError",error},{"root",root}};
        if(resume && saved.at("complete").get<bool>()) {
            if(Json::parse(readBytes(manifest))!=tile) throw std::runtime_error("检查点引用子树与已完成清单不一致");
        } else {
            atomicJson(manifest,tile);
            if(resume) {saved["complete"]=true;resume->checkpoint.save(key,saved,recordAssets(record,true));}
        }
        records.push_back(record);
        FileResult result{manifest,pending.bounds,error,root};cache[key]=result;active.erase(file);
        if(options.checkCancelled) options.checkCancelled();
        if(!resume && options.event) options.event({{"type","file-complete"}});
        if(resume) resume->check();
        return result;
    }
};
// Observe actual external reads, including images outside the source directory.
// Embedded image streams are already covered by the containing OSGB digest.
struct InputReads final : osgDB::ReadFileCallback {
    Json inputs=Json::object();std::function<void()> checkCancelled;
    explicit InputReads(std::function<void()> check):checkCancelled(std::move(check)) {}
    fs::path before(const std::string& name,const osgDB::Options* options) {
        if(checkCancelled) checkCancelled();
        auto resolved=osgDB::findDataFile(name,options);
        if(resolved.empty()) throw std::runtime_error("无法绑定检查点外部输入: "+name);
        auto file=fs::canonical(fs::u8path(resolved));auto identity=fileIdentity(file);
        auto found=inputs.find(file.u8string());
        if(found!=inputs.end() && *found!=identity) throw std::runtime_error("扫描期间输入发生变化: "+file.u8string());
        inputs[file.u8string()]=identity;return file;
    }
    void after(const fs::path& file) {
        if(fileIdentity(file)!=inputs.at(file.u8string())) throw std::runtime_error("读取期间输入发生变化: "+file.u8string());
    }
    osgDB::ReaderWriter::ReadResult readNode(const std::string& name,const osgDB::Options* options) override {
        auto file=before(name,options);auto result=osgDB::Registry::instance()->readNodeImplementation(name,options);after(file);return result;
    }
    osgDB::ReaderWriter::ReadResult readImage(const std::string& name,const osgDB::Options* options) override {
        auto file=before(name,options);auto result=osgDB::Registry::instance()->readImageImplementation(name,options);after(file);return result;
    }
    osgDB::ReaderWriter::ReadResult readObject(const std::string& name,const osgDB::Options* options) override {
        auto file=before(name,options);auto result=osgDB::Registry::instance()->readObjectImplementation(name,options);after(file);return result;
    }
    osgDB::ReaderWriter::ReadResult readHeightField(const std::string& name,const osgDB::Options* options) override {
        auto file=before(name,options);auto result=osgDB::Registry::instance()->readHeightFieldImplementation(name,options);after(file);return result;
    }
};
Json inputIdentity(const Options& options,const osg::Vec3d& origin,const std::vector<fs::path>& entries,const fs::path& metadata) {
    osg::ref_ptr<InputReads> reads=new InputReads(options.checkCancelled);
    Engine collector(options,origin);std::map<fs::path,unsigned> visited;
    const auto walk=[&](auto&& self,const fs::path& file)->void {
        auto& mark=visited[file];if(mark==1) throw std::runtime_error("检查点扫描检测到循环引用: "+file.u8string());
        if(mark==2) return;mark=1;Pending pending;
        {
            osg::ref_ptr<osgDB::Options> readOptions=new osgDB::Options;
            readOptions->setObjectCacheHint(osgDB::Options::CACHE_NONE);readOptions->setReadFileCallback(reads.get());
            auto node=osgDB::readRefNodeFile(file.u8string(),readOptions.get());
            if(!node) throw std::runtime_error("检查点扫描无法读取: "+file.u8string());
            std::vector<GeometryInput> meshes;collector.collect(node.get(),osg::Matrixd::identity(),nullptr,file,meshes,pending);
        }
        for(const auto& ref:pending.external) self(self,ref.file);mark=2;
    };
    for(const auto& file:entries) walk(walk,file);
    if(fs::exists(metadata)) reads->inputs[fs::canonical(metadata).u8string()]=fileIdentity(metadata);
    return reads->inputs;
}
std::vector<double> csv(const std::string& value) {
    std::stringstream stream(value);std::string part;std::vector<double> values;
    while(std::getline(stream,part,',')) {std::size_t consumed=0;double v=std::stod(part,&consumed);
        if(!std::isfinite(v)||part.find_first_not_of(" \t\r\n",consumed)!=std::string::npos) throw std::runtime_error("无效坐标数值");values.push_back(v);}
    return values;
}
std::pair<std::string,std::string> readMetadata(const fs::path& path) {
    auto bytes=readBytes(path);
    std::unique_ptr<xmlDoc,decltype(&xmlFreeDoc)> doc(xmlReadMemory(reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),path.u8string().c_str(),nullptr,XML_PARSE_NONET),xmlFreeDoc);
    if(!doc) throw std::runtime_error("metadata.xml 不是有效 XML: "+path.u8string());
    auto root=xmlDocGetRootElement(doc.get());if(!root) throw std::runtime_error("metadata.xml 没有根节点");
    auto field=[&](const char* name) {
        std::string result;bool found=false;
        for(auto child=root->children;child;child=child->next) if(child->type==XML_ELEMENT_NODE && xmlStrEqual(child->name,BAD_CAST name)) {
            if(found) throw std::runtime_error(std::string("重复的坐标元数据: ")+name);found=true;
            std::unique_ptr<xmlChar,decltype(xmlFree)> value(xmlNodeGetContent(child),xmlFree);
            if(value) result=reinterpret_cast<const char*>(value.get());
        }
        auto start=result.find_first_not_of(" \t\r\n"),end=result.find_last_not_of(" \t\r\n");
        return start==std::string::npos?std::string():result.substr(start,end-start+1);
    };
    return {field("SRS"),field("SRSOrigin")};
}
std::vector<fs::path> roots(const fs::path& input) {
    if(fs::is_regular_file(input)) return {fs::canonical(input)};
    auto lower=[](std::string s) {std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return std::tolower(c);});return s;};
    auto directory=fs::exists(input/"Data")?input/"Data":(fs::exists(input/"data")?input/"data":input);
    std::vector<fs::path> result;
    for(const auto& e:fs::directory_iterator(directory))
        if(e.is_regular_file() && lower(e.path().extension().u8string())==".osgb") result.push_back(fs::canonical(e.path()));
    if(result.empty()) for(const auto& e:fs::directory_iterator(directory)) if(e.is_directory()) {
        std::vector<fs::path> matches;
        for(const auto& f:fs::directory_iterator(e.path()))
            if(f.is_regular_file() && lower(f.path().extension().u8string())==".osgb" && lower(f.path().stem().u8string())==lower(e.path().filename().u8string()))
                matches.push_back(fs::canonical(f.path()));
        if(matches.size()>1) throw std::runtime_error("块目录含多个仅大小写不同的入口: "+e.path().u8string());
        if(!matches.empty()) result.push_back(matches[0]);
    }
    std::sort(result.begin(),result.end());
    if(result.empty()) throw std::runtime_error("未找到索引 OSGB 或 Data/分块名/分块名.osgb");
    if(result.size()==1) return result;
    // 目录中的子瓦片不能同时成为独立根；按实际引用闭包去重，而不是按文件名猜层级。
    std::set<fs::path> candidates(result.begin(),result.end()),referenced;
    std::map<fs::path,unsigned> visited;
    const auto walk=[&](auto&& self,const fs::path& file)->void {
        auto& mark=visited[file];
        if(mark==1) throw std::runtime_error("根发现时检测到循环引用: "+file.u8string());
        if(mark==2) return;mark=1;
        std::vector<fs::path> refs;
        {
            osg::ref_ptr<osgDB::Options> options=new osgDB::Options;options->setObjectCacheHint(osgDB::Options::CACHE_NONE);
            auto node=osgDB::readRefNodeFile(file.u8string(),options.get());
            if(!node) throw std::runtime_error("根发现时无法读取: "+file.u8string());
            const auto collect=[&](auto&& recurse,osg::Node* n)->void {
                if(auto paged=dynamic_cast<osg::PagedLOD*>(n)) {
                    for(unsigned i=0;i<paged->getNumFileNames();++i) if(!paged->getFileName(i).empty()) refs.push_back(resolveReference(file,paged->getDatabasePath(),paged->getFileName(i)));
                    bool compact=compactExternalChildren(paged->getNumChildren(),paged->getNumFileNames());
                    for(unsigned i=0;i<paged->getNumChildren();++i) if(compact || i>=paged->getNumFileNames() || paged->getFileName(i).empty()) recurse(recurse,paged->getChild(i));
                    return;
                }
                if(auto proxy=dynamic_cast<osg::ProxyNode*>(n)) {
                    for(unsigned i=0;i<proxy->getNumFileNames();++i) if(!proxy->getFileName(i).empty()) refs.push_back(resolveReference(file,proxy->getDatabasePath(),proxy->getFileName(i)));
                    bool compact=compactExternalChildren(proxy->getNumChildren(),proxy->getNumFileNames());
                    for(unsigned i=0;i<proxy->getNumChildren();++i) if(compact || i>=proxy->getNumFileNames() || proxy->getFileName(i).empty()) recurse(recurse,proxy->getChild(i));
                    return;
                }
                if(auto group=n->asGroup()) for(unsigned i=0;i<group->getNumChildren();++i) recurse(recurse,group->getChild(i));
            };
            collect(collect,node.get());
        }
        for(const auto& ref:refs) {if(candidates.count(ref)) referenced.insert(ref);self(self,ref);}
        mark=2;
    };
    for(const auto& candidate:result) walk(walk,candidate);
    result.erase(std::remove_if(result.begin(),result.end(),[&](const auto& p){return referenced.count(p);}),result.end());
    if(result.empty()) throw std::runtime_error("OSGB 目录没有无环的根入口");
    return result;
}
}
Json convert(const Options& requested) {
    Options options=requested;
    std::mutex eventMutex;std::uint64_t processed=0;
    auto emit=[&](const Json& event) {
        if(!requested.event) return;
        std::lock_guard<std::mutex> lock(eventMutex);requested.event(event);
    };
    options.event=[&](const Json&) {
        std::lock_guard<std::mutex> lock(eventMutex);++processed;
        if(requested.event) requested.event({{"type","progress"},{"stage","convert"},{"done",processed},{"total",0},{"unit","files"}});
    };
    if(options.checkpointDir.empty() && (options.resume || !options.pauseFile.empty())) throw std::runtime_error("--resume/--pause-file 需要 --checkpoint-dir");
    if(options.checkpointDir.empty() && options.checkCancelled) options.checkCancelled();
    emit({{"type","progress"},{"stage","scan"},{"done",0},{"total",0},{"unit","files"}});
    options.input=fs::canonical(options.input);options.output=stripTrailingSeparator(fs::absolute(options.output).lexically_normal());
    if(!options.checkpointDir.empty()) {
        options.output=stripTrailingSeparator(fs::weakly_canonical(options.output));
        if(fs::is_symlink(fs::symlink_status(options.checkpointDir))) throw std::runtime_error("检查点目录不能是符号链接");
        options.checkpointDir=fs::weakly_canonical(fs::absolute(options.checkpointDir));
        if(pathContains(options.checkpointDir,options.output) || pathContains(options.output,options.checkpointDir) || pathContains(options.checkpointDir,options.input))
            throw std::runtime_error("检查点与输出必须互不包含，且不能包含输入");
        if(!options.pauseFile.empty()) {
            options.pauseFile=fs::weakly_canonical(fs::absolute(options.pauseFile));
            if(pathContains(options.checkpointDir,options.pauseFile) || pathContains(options.output,options.pauseFile)) throw std::runtime_error("暂停令牌必须位于检查点和输出目录之外");
        }
    }
    if(options.threads<1 || options.threads>64) throw std::runtime_error("threads 必须为 1..64");
    if(options.dracoSpeed!=0 && options.dracoSpeed!=5 && options.dracoSpeed!=10) throw std::runtime_error("Draco speed 必须为 0/5/10");
    if(options.textureFormat!="original" && options.textureFormat!="uastc" && options.textureFormat!="etc1s") throw std::runtime_error("无效的 texture format");
    if(options.dracoGeometry && options.quantizeGeometry) throw std::runtime_error("Draco 与实验 U16 量化不可组合");
    if(fs::exists(options.output) && (!fs::is_directory(options.output)||!fs::is_empty(options.output))) throw std::runtime_error("输出目录必须不存在或为空，拒绝覆盖");
    fs::path inputDir=fs::is_directory(options.input)?options.input:options.input.parent_path();
    auto metadata=inputDir/"metadata.xml";
    if(!fs::exists(metadata)) {
        auto name=inputDir.filename().u8string();std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return std::tolower(c);});
        if(name=="data") metadata=inputDir.parent_path()/"metadata.xml";
    }
    Json metadataIdentity;
    if(fs::exists(metadata)) {
        if(!options.checkpointDir.empty()) metadataIdentity=fileIdentity(metadata);
        auto [srs,originText]=readMetadata(metadata);
        if(options.srs.empty()) options.srs=srs;
        if(options.origin.empty()) options.origin=originText;
    }
    auto origin=csv(options.origin.empty()?"0,0,0":options.origin);if(origin.size()!=3) throw std::runtime_error("SRSOrigin 必须有三个分量");
    osg::Vec3d originVector(origin[0],origin[1],origin[2]);
    SpatialReference spatial(options.srs,originVector);auto placement=spatial.placement();
    auto entrypoints=roots(options.input);installImageCapture();
    struct CaptureCleanup { ~CaptureCleanup() { uninstallImageCapture(); } } captureCleanup;
    std::unique_ptr<Checkpoint> checkpoint;std::unique_ptr<ResumeState> resume;Json inputs;
    if(!options.checkpointDir.empty()) {
        // Resume validation is read-only. Once accepted, cancellation owns and
        // cleans the checkpoint, including a cancel token present at startup.
        auto scanOptions=options;if(options.resume) scanOptions.checkCancelled={};
        inputs=inputIdentity(scanOptions,originVector,entrypoints,metadata);
        for(auto it=inputs.begin();it!=inputs.end();++it) {
            auto source=fs::u8path(it.key());
            if(pathContains(options.checkpointDir,source) || pathContains(options.output,source)) throw std::runtime_error("检查点/输出不能包含源引用资源");
        }
        if(metadataIdentity!=(fs::exists(metadata)?fileIdentity(metadata):Json())) throw std::runtime_error("扫描期间坐标元数据改变");
        Json entries=Json::array();for(const auto& entry:entrypoints) entries.push_back(entry.u8string());
        Json identity={{"input",options.input.u8string()},{"output",options.output.u8string()},
            {"entrypoints",entries},{"inputs",inputs},{"converter",checkpointConverterIdentity()},
            {"effectiveOptions",{{"srs",options.srs},{"origin",origin},{"preserveEncoded",options.preserveEncoded},
                {"compressGeometry",options.compressGeometry},{"dracoGeometry",options.dracoGeometry},{"dracoSpeed",options.dracoSpeed},{"textureFormat",options.textureFormat},{"embedTextures",options.embedTextures},{"threads",options.threads},{"quantizeGeometry",options.quantizeGeometry},{"splitPagedLod",options.splitPagedLod},{"rebuildTop",options.rebuildTop},{"topSealDistance",options.topSealDistance},{"topAlgorithm","ancestor-rebuild-v1"},{"preview",options.preview},{"previewAlgorithm","mesh-preview-v2"},{"memoryBudgetBytes",options.memoryBudgetBytes}}}};
        checkpoint=std::make_unique<Checkpoint>(options.checkpointDir,identity,options.resume);
        resume=std::make_unique<ResumeState>(*checkpoint,options,inputs);processed=checkpoint->filesCompleted();
        if(options.checkCancelled) options.checkCancelled();
        emit({{"type","progress"},{"stage","convert"},{"done",processed},{"total",0},{"unit","files"}});
    }
    OutputTransaction transaction(options.output,checkpoint?checkpoint->work():fs::path());options.output=transaction.staging();
    auto verifyCurrentInputs=[&] {
        if(!checkpoint) return;
        verifyInputIdentity(inputs,options.checkCancelled);
        if(metadataIdentity!=(fs::exists(metadata)?fileIdentity(metadata):Json()) || roots(options.input)!=entrypoints)
            throw std::runtime_error("转换期间输入入口或坐标元数据发生变化");
    };
    const auto requestedThreads=options.threads;
    options.threads=std::min({options.threads,std::max(1U,std::thread::hardware_concurrency()),static_cast<unsigned>(entrypoints.size())});
    auto start=std::chrono::steady_clock::now();Json children=Json::array(),records=Json::array();osg::BoundingBoxd bounds;double error=0;
    // 并发只发生在独立根块；每块内部按引用逐文件释放。预算门控只在驻留内存低于预算时提交下一块，
    // 超出就先等最早一块结束，避免把并行峰值交给操作系统去挡。
    const auto initialResident=residentBytes();ResidentSampler sampler;bool throttled=false;
    Json derivedAssets=Json::object();
    const auto topStaging=options.output/".top-building";
    const auto previewStaging=options.output/".preview-building";
    try {
    std::deque<std::future<std::pair<FileResult,Json>>> jobs;bool stopping=false;std::exception_ptr failure;
    for(std::size_t next=0;next<entrypoints.size()||!jobs.empty();) {
        try {
            if(resume) resume->check();
            while(!stopping && next<entrypoints.size() && jobs.size()<options.threads) {
                if(options.memoryBudgetBytes && !jobs.empty() && residentBytes()>=options.memoryBudgetBytes) {throttled=true;break;}
                jobs.push_back(std::async(std::launch::async,[&options,&originVector,&entrypoints,&resume,index=next] {
                    Engine engine(options,originVector,resume.get());auto result=engine.convertFile(entrypoints[index]);return std::make_pair(result,engine.records);
                }));
                ++next;
            }
        } catch(const PauseRequested&) {stopping=true;}
          catch(...) {stopping=true;failure=std::current_exception();}
        if(jobs.empty()) break;
        try {
            auto [result,audit]=jobs.front().get();
            bounds.expandBy(result.bounds);error=std::max(error,result.error);children.push_back(result.root);
            for(auto& row:audit) records.push_back(std::move(row));
        } catch(const PauseRequested&) {stopping=true;}
          catch(...) {stopping=true;if(!failure) failure=std::current_exception();}
        jobs.pop_front();
    }
    if(failure) std::rethrow_exception(failure);
    if(options.checkCancelled) options.checkCancelled();
    if(resume) resume->check();
    emit({{"type","progress"},{"stage","finalize"},{"done",processed},{"total",processed},{"unit","files"}});
    // 顶层根是无内容容器；保持 >= 2000 误差（与 rebuildTop 及外部查看器习惯一致），
    // 确保单层或小场景在任意初始视距下首帧都能立即遍历到首个有效内容层。
    if(bounds.valid()) {
        double extent=std::max({bounds.xMax()-bounds.xMin(),bounds.yMax()-bounds.yMin(),bounds.zMax()-bounds.zMin()});
        error=std::max({error,extent/50.,2000.});
    } else if(!(error>0)) {
        error=2000.;
    }
    Json root={{"boundingVolume",boundingBoxJson(bounds)},{"transform",std::vector<double>(placement.ptr(),placement.ptr()+16)},
               {"geometricError",error},{"refine","REPLACE"},{"children",children}};
    // 前端只需一份层级清单，去除每个源文件一次外部 JSON 往返；内容选择规则保持不变。
    const auto relocateUris=[&](auto&& self,Json& tile)->void {
        if(tile.contains("content")) {
            auto& uri=tile["content"]["uri"];auto value=uri.get<std::string>();
            if(value.rfind("../content/",0)==0) uri=value.substr(3);
        }
        if(tile.contains("children")) {
            for(auto& child:tile["children"]) self(self,child);
            if(options.splitPagedLod) {
                // Cesium 1.136 canTraverse applies SSE even to empty content. An
                // internal empty grouping can stop refining and render a hole.
                // All internal coordinates are already in the same local frame;
                // splice out these structural groups instead of assigning fake errors.
                Json children=Json::array();
                for(auto& child:tile["children"]) {
                    if(!child.contains("content") && !child.contains("transform") && child.contains("children"))
                        for(auto& descendant:child["children"]) children.push_back(std::move(descendant));
                    else children.push_back(std::move(child));
                }
                tile["children"]=std::move(children);
            }
        }
    };
    relocateUris(relocateUris,root);
    Json previewSourceRoot;
    if(options.preview) previewSourceRoot=root;
    Json topAudit=Json::array();
    if(options.rebuildTop) {
        emit({{"type","progress"},{"stage","rebuild-top"},{"done",0},{"total",0},{"unit","ancestors"}});
        if(options.splitPagedLod) throw std::runtime_error("Top rebuild cannot be combined with experimental split-paged-lod");
        std::map<std::string,const Json*> sourceByContent;
        for(const auto& row:records) for(const auto& content:row.at("content")) sourceByContent[content.at("uri").get<std::string>()]=&row;
        Engine reader(options,originVector);std::vector<TopSource> sources;
        auto frontier=[&](auto&& self,const Json& tile)->void {
            if(!tile.contains("content")) {
                if(tile.contains("children")) for(const auto& child:tile["children"]) self(self,child);
                return;
            }
            const auto uri=tile.at("content").at("uri").get<std::string>();auto found=sourceByContent.find(uri);
            if(found==sourceByContent.end()) throw std::runtime_error("Top source content missing from source ledger: "+uri);
            const auto& row=*found->second;auto file=fs::u8path(row.at("source").get<std::string>());
            if(resume) resume->checkSource(file);
            osg::ref_ptr<osgDB::Options> readOptions=new osgDB::Options;readOptions->setObjectCacheHint(osgDB::Options::CACHE_NONE);
            auto node=osgDB::readRefNodeFile(file.u8string(),readOptions);if(!node) throw std::runtime_error("Top source read failed: "+file.u8string());
            auto matrix=row.at("sourceTransform").get<std::vector<double>>();if(matrix.size()!=16) throw std::runtime_error("Top source matrix invalid");
            Pending pending;TopSource source;source.tile=tile;
            reader.collect(node,osg::Matrixd(matrix.data()),nullptr,file,source.meshes,pending);
            if(source.meshes.empty()) throw std::runtime_error("Top resident source is empty: "+file.u8string());
            sources.push_back(std::move(source));
        };
        for(const auto& child:root["children"]) frontier(frontier,child);
        auto topOptions=options;topOptions.event=emit;topOptions.output=topStaging;
        topOptions.checkCancelled=[&] {if(options.checkCancelled) options.checkCancelled();if(resume) resume->check();};
        auto top=nt::rebuildTop(std::move(sources),topOptions,spatial.projected()?&spatial:nullptr);
        // Derived assets are disposable until the entire Top tree is ready.
        // Record exact identities for final checkpoint validation, then move.
        if(fs::exists(topStaging)) for(const auto& item:fs::recursive_directory_iterator(topStaging)) if(item.is_regular_file()) {
            auto relative=item.path().lexically_relative(topStaging);auto destination=options.output/relative;
            if(fs::exists(destination)) {
                if(readBytes(destination)!=readBytes(item.path())) throw std::runtime_error("Top asset collides with source asset");
            } else {
                derivedAssets[relative.generic_u8string()]=fileIdentity(item.path());
                fs::create_directories(destination.parent_path());
                std::error_code renameError;
                renameWithRetry(item.path(),destination,renameError);
                if(renameError) throw fs::filesystem_error("无法发布顶层重构资产",item.path(),destination,renameError);
            }
        }
        fs::remove_all(topStaging);
        root["children"]=top.children;topAudit=top.audit;error=std::max(2000.,top.error);
        for(const auto& child:top.children) {
            auto box=child.at("boundingVolume").at("box").get<std::vector<double>>();
            for(int x:{-1,1}) for(int y:{-1,1}) for(int z:{-1,1}) bounds.expandBy(osg::Vec3d(
                box[0]+x*box[3]+y*box[6]+z*box[9],box[1]+x*box[4]+y*box[7]+z*box[10],box[2]+x*box[5]+y*box[8]+z*box[11]));
        }
        root["boundingVolume"]=boundingBoxJson(bounds);
        root["geometricError"]=error;
        emit({{"type","progress"},{"stage","rebuild-top"},{"done",topAudit.size()},{"total",topAudit.size()},{"unit","ancestors"}});
    }
    Json previewReport;
    if(options.preview) {
        auto previewOptions=options;previewOptions.output=previewStaging;previewOptions.event=emit;
        previewOptions.threads=std::min(requestedThreads,std::max(1U,std::thread::hardware_concurrency()));
        previewOptions.checkCancelled=[&] {if(options.checkCancelled) options.checkCancelled();if(resume) resume->check();};
        auto checkSource=[&](const fs::path& file) {if(resume) resume->checkSource(file);};
        previewReport=buildPreview(previewSourceRoot,records,previewOptions,spatial.projected()?&spatial:nullptr,placement,checkSource);
        if(fs::exists(options.output/"preview")) throw std::runtime_error("预览暂存资产与已有目录冲突");
        for(const auto& item:fs::recursive_directory_iterator(previewStaging)) if(item.is_regular_file()) {
            auto relative=item.path().lexically_relative(previewStaging);derivedAssets[(fs::path("preview")/relative).generic_u8string()]=fileIdentity(item.path());
        }
        std::error_code previewRenameError;
        renameWithRetry(previewStaging,options.output/"preview",previewRenameError);
        if(previewRenameError) throw fs::filesystem_error("无法发布轻量预览目录",previewStaging,options.output/"preview",previewRenameError);
        auto previewGlbs=previewReport.at("glbs").get<std::uint64_t>();
        emit({{"type","progress"},{"stage","preview"},{"done",previewGlbs},{"total",previewGlbs},{"unit","files"}});
    }
    std::uint64_t glbs=0,vertices=0,triangles=0,imageCount=0,originals=0;std::set<std::string> files,textureFiles,contentsSeen;
    std::uint64_t geometryOriginalFloatBytes=0;double quantizationErrorMeters=0;
    // Deduplicated texture residency tiers and Draco stream-reuse counters for
    // the App report (a texture referenced by several contents is counted once).
    double ktxMipBytes=0,ktxRgba8Bytes=0;std::map<std::string,unsigned> ktxTiers;std::set<std::string> ktxSeen;
    std::uint64_t dracoPrimitives=0,dracoReused=0;
    double coordinateRoundingError=0;std::uint64_t geometryRawBytes=0,geometryStoredBytes=0,compressedViews=0;
    for(const auto& row:records) {
      files.insert(row["source"].get<std::string>());
      for(const auto& content:row["content"]) {
        if(!contentsSeen.insert(content["uri"].get<std::string>()).second) continue;
        glbs++;vertices+=content["vertices"].get<std::uint64_t>();triangles+=content["triangles"].get<std::uint64_t>();
        coordinateRoundingError=std::max(coordinateRoundingError,content["coordinateRoundingErrorMeters"].get<double>());
        quantizationErrorMeters=std::max(quantizationErrorMeters,content.value("quantizationErrorMeters",0.0));
        geometryOriginalFloatBytes+=content.value("originalFloatBytes",content["geometryRawBytes"].get<std::uint64_t>());
        geometryRawBytes+=content["geometryRawBytes"].get<std::uint64_t>();geometryStoredBytes+=content["geometryStoredBytes"].get<std::uint64_t>();compressedViews+=content["compressedViews"].get<std::uint64_t>();
        for(const auto& image:content["images"]) {
            imageCount++;originals+=image["originalEncoded"].get<bool>();
            const auto uri=image.value("uri",std::string());
            const auto key=!uri.empty()?uri:image.value("contentKey",std::string());
            if(!key.empty()) textureFiles.insert(key);
            if(image.contains("basis") && image["basis"].contains("gpuEstimate") && (key.empty() || ktxSeen.insert(key).second)) {
                const auto& g=image["basis"]["gpuEstimate"];
                ktxMipBytes+=g["withMipChainBytes"].get<double>();ktxRgba8Bytes+=g["rgba8WithMipChainBytes"].get<double>();
                ktxTiers[g["tier"].get<std::string>()]++;
            }
        }
        if(content.contains("draco") && content["draco"].contains("primitives"))
            for(const auto& prim:content["draco"]["primitives"]) {++dracoPrimitives;if(prim.value("streamReused",false)) ++dracoReused;}
      }
    }
    const auto sourceGlbs=glbs;
    for(const auto& top:topAudit) {
        ++glbs;vertices+=top.at("vertices").get<std::uint64_t>();triangles+=top.at("outputTriangles").get<std::uint64_t>();
        geometryRawBytes+=top.at("geometryRawBytes").get<std::uint64_t>();geometryStoredBytes+=top.at("geometryStoredBytes").get<std::uint64_t>();
        geometryOriginalFloatBytes+=top.at("originalFloatBytes").get<std::uint64_t>();compressedViews+=top.at("compressedViews").get<std::uint64_t>();
        coordinateRoundingError=std::max(coordinateRoundingError,top.at("coordinateRoundingErrorMeters").get<double>());
        if(!top.at("draco").empty()) quantizationErrorMeters=std::max(quantizationErrorMeters,top.at("draco").at("maxWorldPositionErrorMeters").get<double>());
        for(const auto& image:top.at("images")) {
            ++imageCount;
            const auto key=image.contains("uri")?image.at("uri").get<std::string>():image.value("contentKey",std::string());
            if(!key.empty()) textureFiles.insert(key);
        }
    }
    Json report={{"status","complete"},{"converter",capabilities()},{"preserveEncodedTextures",options.preserveEncoded && options.textureFormat=="original"},{"format","3D Tiles 1.1 / glTF 2.0"},{"srs",options.srs},{"srsOrigin",origin},
        {"files",files.size()},{"glbs",glbs},{"sourceGlbs",sourceGlbs},{"verticesAllLods",vertices},{"trianglesAllLods",triangles},
        {"imageReferences",imageCount},{"uniqueImages",textureFiles.size()},{"originalEncodedImages",originals},
        {"coordinateRoundingErrorMeters",coordinateRoundingError},
        {"geometryEncoding",options.dracoGeometry?"KHR_draco_mesh_compression":options.quantizeGeometry?(options.compressGeometry?"EXT_meshopt_compression+KHR_mesh_quantization":"KHR_mesh_quantization"):(options.compressGeometry?"EXT_meshopt_compression":"raw")},
        {"geometryOriginalFloatBytes",geometryOriginalFloatBytes},
        {"quantizationErrorMeters",quantizationErrorMeters},
        {"geometryRawBytes",geometryRawBytes},{"geometryStoredBytes",geometryStoredBytes},{"compressedViews",compressedViews},
        {"elapsedSeconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count()},
        {"threads",options.threads},{"requestedThreads",requestedThreads},
        {"memoryBudgetBytes",options.memoryBudgetBytes},{"peakResidentBytes",sampler.peak()},
        {"splitPagedLod",options.splitPagedLod},{"quantizeGeometry",options.quantizeGeometry},
        {"rebuildTop",options.rebuildTop},{"topSealDistance",options.topSealDistance},{"topAlgorithm","ancestor-rebuild-v1"},{"topAncestors",topAudit},
        {"textureFormat",options.textureFormat},{"embedTextures",options.embedTextures},{"dracoSpeed",options.dracoSpeed},
        {"textureResidency",{{"withMipChainBytes",ktxMipBytes},{"rgba8WithMipChainBytes",ktxRgba8Bytes},
            {"savedVsRgba8",ktxRgba8Bytes>0?1.-ktxMipBytes/ktxRgba8Bytes:0.},{"tiers",ktxTiers},
            {"basis","format arithmetic over unique textures; consumer/GPU picks the real upload format"}}},
        {"dracoStreams",{{"primitives",dracoPrimitives},{"reused",dracoReused}}},{"lossyGeometry",options.dracoGeometry || options.quantizeGeometry || !topAudit.empty()},{"lossyTextures",options.textureFormat!="original" || !topAudit.empty()},
        {"sourceExact",!options.dracoGeometry && !options.quantizeGeometry && options.textureFormat=="original" && topAudit.empty()},
        {"sourceContentsExact",!options.dracoGeometry && !options.quantizeGeometry && options.textureFormat=="original"},
        {"topGlbs",topAudit.size()},{"topLossy",options.rebuildTop && !topAudit.empty()},
        {"memoryThrottled",throttled},{"memoryResidentSampled",initialResident>0},{"records",records}};
    if(options.preview) report["preview"]={
        {"status","complete"},{"entry","preview/tileset.json"},{"algorithm","mesh-preview-v2"},
        {"streamingLod",true},{"triangles",previewReport.at("triangles")},{"glbs",previewReport.at("glbs")},
        {"overviewTriangles",previewReport.at("overviewTriangles")},
        {"geometryEncoding","EXT_meshopt_compression+KHR_mesh_quantization"},{"textureFormat","original"}};
    if(checkpoint) {
        report["checkpointDir"]=checkpoint->directory().u8string();report["filesCompleted"]=checkpoint->filesCompleted();
        report["filesReused"]=resume->reused.size();
    }
    atomicJson(options.output/"conversion-report.json",report);
    atomicJson(options.output/"tileset.json",{{"asset",{{"version","1.1"},{"generator",kAssetGenerator}}},{"geometricError",error},{"root",root}});
    if(options.checkCancelled) options.checkCancelled();
    // 发布前只读完整性校验；暂停与续转也不能绕过资产门禁。
    validateStagedOutput(transaction.staging());
    report["elapsedSeconds"]=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    report["peakResidentBytes"]=sampler.peak();
    atomicJson(options.output/"conversion-report.json",report);
    transaction.commit([&] {if(options.checkCancelled) options.checkCancelled();if(resume) resume->check();},
                       [&] {verifyCurrentInputs();if(checkpoint) checkpoint->verifyAssets(true,derivedAssets);});
    emit({{"type","progress"},{"stage","done"},{"done",processed},{"total",processed},{"unit","files"}});
    return report;
    } catch(const PauseRequested&) {
        if(options.checkCancelled) options.checkCancelled();
        verifyCurrentInputs();
        fs::remove(options.output/"tileset.json");fs::remove(options.output/"conversion-report.json");
        fs::remove_all(topStaging);fs::remove_all(previewStaging);fs::remove_all(options.output/"preview");
        for(auto it=derivedAssets.begin();it!=derivedAssets.end();++it) fs::remove(options.output/fs::u8path(it.key()));
        checkpoint->verifyAssets();auto completed=checkpoint->filesCompleted();
        Json report={{"status","paused"},{"checkpointDir",checkpoint->directory().u8string()},{"filesCompleted",completed},
                     {"filesReused",resume->reused.size()},{"converter",capabilities()}};
        emit({{"type","progress"},{"stage","paused"},{"done",completed},{"total",0},{"unit","files"}});
        checkpoint->preserve();transaction.preserve();return report;
    }
}
}
