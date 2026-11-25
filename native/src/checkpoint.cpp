#include "checkpoint.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nt {
namespace {
// Original implementation of SHA-256 from FIPS PUB 180-4, sections 4.1.2,
// 4.2.2, 5 and 6.2. No platform crypto library or third-party code required.
// https://doi.org/10.6028/NIST.FIPS.180-4
class Sha256 {
    std::array<std::uint32_t,8> state_{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    std::array<unsigned char,64> buffer_{};
    std::uint64_t size_=0;
    std::size_t used_=0;
    static std::uint32_t rotate(std::uint32_t x,unsigned n) {return (x>>n)|(x<<(32-n));}
    void block(const unsigned char* data) {
        static constexpr std::uint32_t k[]={
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        std::uint32_t w[64];
        for(unsigned i=0;i<16;++i) w[i]=(std::uint32_t(data[4*i])<<24)|(std::uint32_t(data[4*i+1])<<16)|(std::uint32_t(data[4*i+2])<<8)|data[4*i+3];
        for(unsigned i=16;i<64;++i) {
            auto a=w[i-15],b=w[i-2];
            w[i]=w[i-16]+(rotate(a,7)^rotate(a,18)^(a>>3))+w[i-7]+(rotate(b,17)^rotate(b,19)^(b>>10));
        }
        auto a=state_[0],b=state_[1],c=state_[2],d=state_[3],e=state_[4],f=state_[5],g=state_[6],h=state_[7];
        for(unsigned i=0;i<64;++i) {
            auto t1=h+(rotate(e,6)^rotate(e,11)^rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i];
            auto t2=(rotate(a,2)^rotate(a,13)^rotate(a,22))+((a&b)^(a&c)^(b&c));
            h=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        state_[0]+=a;state_[1]+=b;state_[2]+=c;state_[3]+=d;state_[4]+=e;state_[5]+=f;state_[6]+=g;state_[7]+=h;
    }
public:
    void update(const char* data,std::size_t size) {
        size_+=size;
        while(size) {
            auto count=std::min(size,buffer_.size()-used_);std::memcpy(buffer_.data()+used_,data,count);
            used_+=count;data+=count;size-=count;
            if(used_==buffer_.size()) {block(buffer_.data());used_=0;}
        }
    }
    std::string finish() {
        const auto bits=size_*8;buffer_[used_++]=0x80;
        if(used_>56) {std::fill(buffer_.begin()+used_,buffer_.end(),0);block(buffer_.data());used_=0;}
        std::fill(buffer_.begin()+used_,buffer_.begin()+56,0);
        for(unsigned i=0;i<8;++i) buffer_[63-i]=static_cast<unsigned char>(bits>>(8*i));
        block(buffer_.data());std::string result;static constexpr char hex[]="0123456789abcdef";
        for(auto word:state_) for(int i=7;i>=0;--i) result.push_back(hex[(word>>(i*4))&15]);
        return result;
    }
};
Json sealed(const Json& payload) {return {{"format",1},{"sha256",sha256(payload.dump())},{"payload",payload}};}
Json unseal(const fs::path& file) {
    if(!fs::is_regular_file(fs::symlink_status(file))) throw std::runtime_error("检查点记录缺失或不是普通文件: "+file.u8string());
    try {
        auto value=Json::parse(readBytes(file));
        if(value.at("format")!=1 || value.at("sha256")!=sha256(value.at("payload").dump())) throw std::runtime_error("校验和不匹配");
        return value.at("payload");
    } catch(const std::exception& e) {throw std::runtime_error("检查点记录损坏: "+file.u8string()+": "+e.what());}
}
bool validKey(const std::string& key) {
    return (key.size()==32 || (key.size()==65 && key[32]=='-')) && key.find_first_not_of("0123456789abcdef-")==std::string::npos;
}
fs::path assetPath(const fs::path& work,const std::string& name) {
    auto path=fs::u8path(name);
    if(path.is_absolute() || path.lexically_normal()!=path || path.empty() || *path.begin()=="..") throw std::runtime_error("非法检查点资产路径: "+name);
    auto first=path.begin()->string();
    if(name!="orthophoto.tif" && first!="content" && first!="textures" && first!="tiles" && first!="preview") throw std::runtime_error("未知检查点资产: "+name);
    return work/path;
}
}
std::string sha256(const std::string& bytes) {Sha256 hash;hash.update(bytes.data(),bytes.size());return hash.finish();}
std::string fileSha256(const fs::path& file) {
    std::ifstream input(file,std::ios::binary);if(!input) throw std::runtime_error("无法计算文件身份: "+file.u8string());
    Sha256 hash;std::array<char,65536> buffer;
    while(input) {input.read(buffer.data(),buffer.size());hash.update(buffer.data(),static_cast<std::size_t>(input.gcount()));}
    if(!input.eof()) throw std::runtime_error("读取文件身份失败: "+file.u8string());
    return hash.finish();
}
Json fileIdentity(const fs::path& file) {return {{"bytes",fs::file_size(file)},{"sha256",fileSha256(file)}};}
bool pathContains(const fs::path& parent,const fs::path& child) {
    auto p=parent.begin(),c=child.begin();for(;p!=parent.end();++p,++c) if(c==child.end() || *p!=*c) return false;return true;
}
void verifyInputIdentity(const Json& inputs,const std::function<void()>& checkCancelled) {
    for(auto it=inputs.begin();it!=inputs.end();++it) {
        if(checkCancelled) checkCancelled();
        auto file=fs::u8path(it.key());
        if(!fs::is_regular_file(file) || fileIdentity(file)!=it.value()) throw std::runtime_error("检查点输入内容发生变化: "+it.key());
    }
}
struct Checkpoint::Impl {
    fs::path directory;std::string identity;Json state;
    bool owned=false,keep=false;
    mutable std::mutex mutex;
    std::map<std::string,Json> entries;
    std::set<std::string> resumed;
    std::map<std::string,std::unique_ptr<std::mutex>> locks;
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
#else
    int handle=-1;
#endif
    ~Impl() {
        // Keep the lock until all owned assets have been removed. Never clean a
        // rejected resume; that checkpoint still belongs to its previous job.
        std::error_code error;
        if(owned && !keep) for(fs::directory_iterator it(directory,error),end;!error && it!=end;it.increment(error))
            if(it->path().filename()!=".lock") fs::remove_all(it->path(),error);
        // Unlink our lock while still holding it. Removing it after closing
        // could unlink a new owner's lock and permit concurrent writers.
        if(owned && !keep) fs::remove(directory/".lock",error);
#ifdef _WIN32
        if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if(handle>=0) close(handle);
#endif
        if(owned && !keep) fs::remove(directory,error);
    }
};
Checkpoint::Checkpoint(const fs::path& directory,const Json& identity,bool resume):impl_(new Impl) {
    auto& p=*impl_;p.directory=directory;p.identity=sha256(identity.dump());
    if(fs::is_symlink(fs::symlink_status(directory))) throw std::runtime_error("检查点目录不能是符号链接");
    if(resume && !fs::is_directory(directory)) throw std::runtime_error("续转检查点不存在");
    fs::create_directories(directory);
    if(fs::is_symlink(fs::symlink_status(directory/".lock"))) throw std::runtime_error("检查点锁不能是符号链接");
#ifdef _WIN32
    p.handle=CreateFileW((directory/".lock").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_DELETE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(p.handle==INVALID_HANDLE_VALUE) throw std::runtime_error("检查点已被占用或无法加锁");
#else
    p.handle=open((directory/".lock").c_str(),O_CREAT|O_RDWR|O_NOFOLLOW,0600);
    if(p.handle<0 || flock(p.handle,LOCK_EX|LOCK_NB)!=0) throw std::runtime_error("检查点已被占用或无法加锁");
    struct stat held{},named{};
    if(fstat(p.handle,&held)!=0 || lstat((directory/".lock").c_str(),&named)!=0 || held.st_dev!=named.st_dev || held.st_ino!=named.st_ino)
        throw std::runtime_error("检查点锁在接管时已被替换");
#endif
    if(!resume) {
        for(const auto& file:fs::directory_iterator(directory)) if(file.path().filename()!=".lock") throw std::runtime_error("检查点目录非空；续转必须显式使用 --resume");
        p.owned=true;fs::create_directory(work());fs::create_directory(directory/"entries");
        atomicJson(directory/"identity.json",sealed(identity));
        p.state={{"identity",p.identity},{"entries",Json::object()}};atomicJson(directory/"state.json",sealed(p.state));return;
    }
    if(unseal(directory/"identity.json")!=identity) throw std::runtime_error("检查点身份不匹配：输入、有效选项、输出路径或转换器已改变");
    p.state=unseal(directory/"state.json");
    if(p.state.at("identity")!=p.identity || !p.state.at("entries").is_object()) throw std::runtime_error("检查点总账身份或格式错误");
    if(!fs::is_directory(directory/"entries") || !fs::is_directory(work())) throw std::runtime_error("检查点缺少 entries/work");
    std::map<std::string,Json> assets;
    for(const auto& file:fs::recursive_directory_iterator(directory)) {
        if(fs::is_symlink(file.symlink_status()) || (!file.is_directory() && !file.is_regular_file())) throw std::runtime_error("检查点含非普通文件: "+file.path().u8string());
    }
    for(const auto& file:fs::directory_iterator(directory)) {
        auto name=file.path().filename().u8string();
        if(name!=".lock" && name!="identity.json" && name!="state.json" && name!="entries" && name!="work") throw std::runtime_error("检查点含未知记录: "+name);
    }
    for(const auto& file:fs::directory_iterator(directory/"entries")) {
        auto key=file.path().stem().u8string();
        if(file.path().extension()!=".json" || !validKey(key)) throw std::runtime_error("检查点含不完整记录");
        auto entry=unseal(file.path());
        if(!p.state.at("entries").contains(key) || p.state.at("entries").at(key)!=sha256(entry.dump())) throw std::runtime_error("检查点总账与文件记录不一致");
        if(entry.at("identity")!=p.identity || !entry.at("assets").is_object() || !entry.at("prepared").is_object() || !entry.at("complete").is_boolean()) throw std::runtime_error("检查点记录任务身份或格式错误");
        for(auto it=entry["assets"].begin();it!=entry["assets"].end();++it) {
            auto path=assetPath(work(),it.key());
            if(!fs::is_regular_file(path) || fileIdentity(path)!=it.value()) throw std::runtime_error("检查点资产损坏或缺失: "+it.key());
            auto inserted=assets.emplace(it.key(),it.value());
            if(!inserted.second && inserted.first->second!=it.value()) throw std::runtime_error("检查点资产身份冲突: "+it.key());
        }
        p.entries.emplace(key,std::move(entry));p.resumed.insert(key);
    }
    if(p.state.at("entries").size()!=p.entries.size()) throw std::runtime_error("检查点丢失文件记录");
    for(const auto& file:fs::recursive_directory_iterator(work())) if(file.is_regular_file()) {
        auto name=file.path().lexically_relative(work()).generic_u8string();
        if(!assets.count(name)) throw std::runtime_error("检查点含未登记资产: "+name);
    }
    p.owned=true;
}
Checkpoint::~Checkpoint()=default;
const fs::path& Checkpoint::directory() const {return impl_->directory;}
fs::path Checkpoint::work() const {return directory()/"work";}
std::unique_lock<std::mutex> Checkpoint::lockEntry(const std::string& key) {
    std::mutex* mutex;
    {std::lock_guard<std::mutex> guard(impl_->mutex);auto& lock=impl_->locks[key];if(!lock) lock=std::make_unique<std::mutex>();mutex=lock.get();}
    return std::unique_lock<std::mutex>(*mutex);
}
Json Checkpoint::entry(const std::string& key) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);auto it=impl_->entries.find(key);return it==impl_->entries.end()?Json():it->second;
}
void Checkpoint::save(const std::string& key,Json value,const std::vector<std::string>& assets) {
    if(!validKey(key)) throw std::runtime_error("非法检查点键");
    value["identity"]=impl_->identity;value["assets"]=Json::object();
    for(const auto& name:assets) value["assets"][name]=fileIdentity(assetPath(work(),name));
    std::lock_guard<std::mutex> guard(impl_->mutex);
    auto previous=impl_->entries.find(key);
    if(previous!=impl_->entries.end()) for(auto it=previous->second.at("assets").begin();it!=previous->second.at("assets").end();++it)
        if(!value["assets"].contains(it.key()) || value["assets"][it.key()]!=it.value()) throw std::runtime_error("已持久化资产在子树完成前改变: "+it.key());
    // Record then ledger: a crash between the two is rejected on resume. The
    // ledger detects deletion even for a prepared index with no local assets.
    atomicJson(directory()/"entries"/(key+".json"),sealed(value));
    impl_->state["entries"][key]=sha256(value.dump());atomicJson(directory()/"state.json",sealed(impl_->state));
    impl_->entries[key]=std::move(value);
}
std::uint64_t Checkpoint::filesCompleted() const {std::lock_guard<std::mutex> guard(impl_->mutex);return impl_->entries.size();}
bool Checkpoint::wasResumed(const std::string& key) const {return impl_->resumed.count(key)!=0;}
void Checkpoint::verifyAssets(bool finalized,const Json& derivedAssets) const {
    std::lock_guard<std::mutex> guard(impl_->mutex);std::set<std::string> expected;
    if(finalized) {expected.insert("tileset.json");expected.insert("conversion-report.json");}
    for(auto it=derivedAssets.begin();it!=derivedAssets.end();++it) {
        if(!finalized || fileIdentity(assetPath(work(),it.key()))!=it.value()) throw std::runtime_error("派生 Top 资产在发布前改变");
        expected.insert(it.key());
    }
    if(sha256(unseal(directory()/"identity.json").dump())!=impl_->identity || unseal(directory()/"state.json")!=impl_->state)
        throw std::runtime_error("检查点身份或总账在转换期间改变");
    if(static_cast<std::size_t>(std::distance(fs::directory_iterator(directory()/"entries"),fs::directory_iterator()))!=impl_->entries.size())
        throw std::runtime_error("检查点含未登记记录");
    for(const auto& item:impl_->entries) {
        if(unseal(directory()/"entries"/(item.first+".json"))!=item.second) throw std::runtime_error("检查点记录在转换期间改变");
        for(auto it=item.second.at("assets").begin();it!=item.second.at("assets").end();++it) {
            auto file=assetPath(work(),it.key());
            if(!fs::is_regular_file(fs::symlink_status(file)) || fileIdentity(file)!=it.value()) throw std::runtime_error("检查点资产在转换期间改变: "+it.key());
            expected.insert(it.key());
        }
    }
    for(const auto& file:fs::recursive_directory_iterator(work())) {
        if(fs::is_symlink(file.symlink_status())) throw std::runtime_error("检查点资产不能是符号链接");
        if(file.is_regular_file() && !expected.count(file.path().lexically_relative(work()).generic_u8string())) throw std::runtime_error("检查点含未登记资产");
    }
}
void Checkpoint::preserve() {impl_->keep=true;}
}
