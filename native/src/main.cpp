#include "native_tiles.hpp"
#include <osg/Notify>
#include <csignal>
#include <iostream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted=0;
void interrupt(int) {interrupted=1;}
std::uint64_t integer(const std::string& value) {
    if(value.empty() || value.find_first_not_of("0123456789")!=std::string::npos)
        throw std::runtime_error("参数必须是非负整数: "+value);
    return std::stoull(value);
}
}
int runMain(int argc,char** argv) {
    bool events=false;
    try {
        if(argc==2 && std::string(argv[1])=="--capabilities") {
            nt::configureRuntime(argv[0]);std::cout<<nt::capabilities().dump()<<'\n';return 0;
        }
        if(argc<3) {
            std::cout<<"用法: osgb-tiles INPUT OUTPUT [--threads N] [--memory-budget MIB] [--srs ENU:纬度,经度,高度] [--origin x,y,z] [--geometry raw|meshopt|draco] [--draco-speed 0|5|10] [--texture-format original|uastc|etc1s] [--texture-pixels] [--rebuild-top] [--top-seal-distance METERS] [--preview] [--json-events] [--cancel-file PATH] [--checkpoint-dir PATH] [--pause-file PATH] [--resume]\n"
                     <<"默认: threads=2, geometry=meshopt, memory-budget=0；--capabilities 输出机器可读契约。\n";
            return argc==2 && std::string(argv[1])=="--help"?0:2;
        }
        nt::Options options;options.input=nt::fs::u8path(argv[1]);options.output=nt::fs::u8path(argv[2]);nt::fs::path cancelFile;
        for(int i=3;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--texture-pixels") {options.preserveEncoded=false;continue;}
            if(arg=="--rebuild-top") {options.rebuildTop=true;continue;}
            if(arg=="--preview") {options.preview=true;continue;}
            if(arg=="--split-paged-lod") {options.splitPagedLod=true;continue;}
            if(arg=="--quantize-u16") {options.quantizeGeometry=true;continue;}
            if(arg=="--json-events") {events=true;continue;}
            if(arg=="--resume") {options.resume=true;continue;}
            if(i+1>=argc) throw std::runtime_error("参数缺少取值: "+arg);
            std::string value=argv[++i];
            if(arg=="--threads") {
                auto count=integer(value);if(count<1 || count>64) throw std::runtime_error("threads 必须为 1..64");
                options.threads=static_cast<unsigned>(count);
            }
            else if(arg=="--memory-budget") {
                auto mib=integer(value);
                if(mib>std::numeric_limits<std::uint64_t>::max()/(1ull<<20)) throw std::runtime_error("内存预算过大: "+value);
                options.memoryBudgetBytes=mib*(1ull<<20);
            }
            else if(arg=="--srs") options.srs=value;
            else if(arg=="--origin") options.origin=value;
            else if(arg=="--cancel-file") cancelFile=nt::fs::u8path(value);
            else if(arg=="--checkpoint-dir") options.checkpointDir=nt::fs::u8path(value);
            else if(arg=="--pause-file") options.pauseFile=nt::fs::u8path(value);
            else if(arg=="--texture-format") {
                if(value!="original" && value!="uastc" && value!="etc1s") throw std::runtime_error("texture-format 必须为 original/uastc/etc1s");
                options.textureFormat=value;
            }
            else if(arg=="--top-seal-distance") {
                double distance=0;try {distance=std::stod(value);} catch(const std::exception&) {throw std::runtime_error("top-seal-distance 必须为 0..2 的米数: "+value);}
                if(!(distance>=0) || distance>2.) throw std::runtime_error("top-seal-distance 必须为 0..2 的米数: "+value);
                options.topSealDistance=distance;
            }
            else if(arg=="--draco-speed") {
                auto speed=integer(value);if(speed!=0 && speed!=5 && speed!=10) throw std::runtime_error("draco-speed 必须为 0/5/10");
                options.dracoSpeed=static_cast<unsigned>(speed);
            }
            else if(arg=="--geometry") {
                if(value!="raw" && value!="meshopt" && value!="draco") throw std::runtime_error("geometry 必须为 raw/meshopt/draco");
                options.compressGeometry=value=="meshopt";options.dracoGeometry=value=="draco";
            }
            else throw std::runtime_error("未知参数: "+arg);
        }
        std::signal(SIGINT,interrupt);std::signal(SIGTERM,interrupt);
        options.checkCancelled=[&] {
            if(interrupted || (!cancelFile.empty() && nt::fs::exists(cancelFile))) throw std::runtime_error("转换已取消");
        };
        if(events) options.event=[](const nt::Json& event) {std::cout<<event.dump()<<'\n'<<std::flush;};
        nt::configureRuntime(argv[0]);osg::setNotifyLevel(osg::WARN);
        auto report=nt::convert(options);report.erase("records");
        if(events) std::cout<<nt::Json({{"type","result"},{"report",report}}).dump()<<'\n';
        else std::cout<<report.dump(2)<<'\n';
        return report.at("status")=="paused"?75:0;
    } catch(const std::exception& error) {
        if(events) std::cout<<nt::Json({{"type","log"},{"level","error"},{"message",error.what()}}).dump()<<'\n';
        std::cerr<<"转换失败: "<<error.what()<<'\n';return interrupted?130:1;
    }
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> values;std::vector<char*> pointers;
    for(int i=0;i<argc;++i) {
        int size=WideCharToMultiByte(CP_UTF8,0,argv[i],-1,nullptr,0,nullptr,nullptr);
        std::string value(size,'\0');WideCharToMultiByte(CP_UTF8,0,argv[i],-1,value.data(),size,nullptr,nullptr);
        value.resize(size-1);values.push_back(std::move(value));
    }
    for(auto& value:values) pointers.push_back(value.data());
    return runMain(argc,pointers.data());
}
#else
int main(int argc,char** argv) {return runMain(argc,argv);}
#endif
