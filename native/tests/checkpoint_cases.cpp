#include "native_tiles.hpp"
#include "checkpoint.hpp"
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/Texture2D>
#include <osgDB/WriteFile>
#include <osgDB/ReadFile>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace nt;
namespace {
void require(bool value,const std::string& message) {if(!value) throw std::runtime_error(message);}
void nodeFile(osg::Node* node,const fs::path& file,bool external=false) {
    fs::create_directories(file.parent_path());
    osg::ref_ptr<osgDB::Options> writeOptions=new osgDB::Options(external?"WriteImageHint=UseExternal":"WriteImageHint=IncludeFile Compressor=zlib");
    require(osgDB::writeNodeFile(*node,file.u8string(),writeOptions.get()),"fixture OSGB 写入失败");
}
osg::ref_ptr<osg::Geode> mesh(osg::Image* image=nullptr,unsigned count=600) {
    auto g=new osg::Geometry;auto points=new osg::Vec3Array;auto uv=new osg::Vec2Array;
    for(unsigned i=0;i<count;++i) {points->push_back({float(i%20)*0.25f,float(i/20)*0.25f,0});uv->push_back({float(i%2),float((i/2)%2)});}
    g->setVertexArray(points);g->setTexCoordArray(0,uv);g->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,count));
    auto geode=new osg::Geode;geode->addDrawable(g);
    if(image) geode->getOrCreateStateSet()->setTextureAttributeAndModes(0,new osg::Texture2D(image),osg::StateAttribute::ON);
    return geode;
}
osg::ref_ptr<osg::PagedLOD> reference(const std::string& file) {
    auto p=new osg::PagedLOD;p->setFileName(0,file);p->setRange(0,0,1e9);return p;
}
void fixture(const fs::path& directory,bool processFixture=false) {
    fs::create_directories(directory/"external");
    auto image=new osg::Image;image->allocateImage(2,2,1,GL_RGBA,GL_UNSIGNED_BYTE);
    const unsigned char pixels[]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,0,96};
    std::memcpy(image->data(),pixels,sizeof(pixels));image->setFileName((directory/"external/original.png").u8string());
    require(osgDB::writeImageFile(*image,image->getFileName()),"fixture PNG 写入失败");
    auto root=new osg::Group;root->addChild(mesh(image).get());
    for(double offset:{17.5,35.25}) {
        auto matrix=new osg::MatrixTransform;matrix->setMatrix(osg::Matrixd::translate(offset,4,-2));
        matrix->addChild(reference("leaf.osgb").get());root->addChild(matrix);
    }
    nodeFile(root,directory/"source/a.osgb");nodeFile(mesh(image,processFixture?120000:600).get(),directory/"source/leaf.osgb",true);
    nodeFile(reference("middle.osgb").get(),directory/"source/b.osgb");
    nodeFile(reference("leaf.osgb").get(),directory/"source/middle.osgb");
    std::string xml="<ModelMetadata><SRS>EPSG:32651</SRS><SRSOrigin>500000,0,10</SRSOrigin></ModelMetadata>";
    atomicWrite(directory/"source/metadata.xml",Bytes(xml.begin(),xml.end()));
}
std::map<std::string,std::string> assets(const fs::path& directory) {
    std::map<std::string,std::string> result;
    for(const auto& item:fs::recursive_directory_iterator(directory)) if(item.is_regular_file()) {
        auto name=item.path().lexically_relative(directory).generic_u8string();
        if(name!="conversion-report.json") result[name]=fileSha256(item.path());
    }
    return result;
}
Json jsonFile(const fs::path& file) {return Json::parse(readBytes(file));}
void missingExternalTexture(const fs::path& temp) {
    auto image=new osg::Image;
    image->allocateImage(1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,1);
    image->setFileName((temp/"missing.png").u8string());
    image->setWriteHint(osg::Image::EXTERNAL_FILE);
    auto node=mesh(image,3);
    auto state=node->getOrCreateStateSet();
    state->setTextureMode(0,GL_TEXTURE_2D,osg::StateAttribute::OFF);
    Options o;o.srs="ENU:0,0";o.input=temp/"disabled.osgb";o.output=temp/"baseline";
    nodeFile(node,o.input,true);
    auto baseline=convert(o);
    require(baseline.at("imageReferences")==0,"关闭且缺失的贴图不参与绘制");
    o.output=temp/"resumed";o.checkpointDir=temp/"checkpoint";o.pauseFile=temp/"pause";
    o.event=[&](const Json& event) {
        if(event.value("stage","")=="convert" && event.at("done")==1) atomicWrite(o.pauseFile,{});
    };
    require(convert(o).at("status")=="paused","关闭的缺图应允许在文件完成后暂停");
    fs::remove(o.pauseFile);o.event={};o.resume=true;
    auto resumed=convert(o);
    require(resumed.at("filesReused")==1 && assets(o.output)==assets(temp/"baseline"),"续转后应复用同一份无贴图 GLB");
    o.resume=false;o.input=temp/"enabled.osgb";o.output=temp/"invalid";
    state->setTextureMode(0,GL_TEXTURE_2D,osg::StateAttribute::ON);
    nodeFile(node,o.input,true);
    std::string error;
    try {convert(o);} catch(const std::exception& e) {error=e.what();}
    require(error.find("enabled.osgb")!=std::string::npos && !fs::exists(o.output) && !fs::exists(o.checkpointDir),
            "启用的贴图缺失必须报出源文件并阻止发布");
}
void rejected(const Options& options,const std::string& reason) {
    bool failed=false;try {convert(options);} catch(const std::exception&) {failed=true;}
    require(failed,"必须拒绝 "+reason);require(!fs::exists(options.output/"tileset.json"),"拒绝时不能发布 "+reason);
    require(fs::exists(options.checkpointDir/"identity.json"),"验证失败必须保留检查点 "+reason);
}
}
int main(int argc,char** argv) {
    try {
        if(argc==3 && std::string(argv[1])=="--fixture") {fixture(fs::absolute(fs::u8path(argv[2])),true);return 0;}
        auto temp=fs::current_path()/("checkpoint-data-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fixture(temp);unsigned checks=0;
        missingExternalTexture(temp/"missing-texture");++checks;
        require(sha256("")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" &&
                sha256("abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
                sha256(std::string(1000000,'a'))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","FIPS SHA-256 向量");checks++;
        Options baseline;baseline.input=temp/"source";baseline.output=temp/"baseline";baseline.threads=1;
        auto full=convert(baseline);auto expected=assets(baseline.output);
        require(full["files"]==4 && full["glbs"]==4 && full["records"].size()==6 && full["originalEncodedImages"]==4,"fixture 应覆盖引用闭包、原图和矩阵实例");checks++;
        require(full["orthophoto"]["status"]=="complete","Expected a completed orthophoto");
        for(const std::string target:{"orthophoto.tif","orthophoto.prj"}) {
            auto degraded=baseline;
            degraded.checkpointDir=temp/("checkpoint-"+target);
            degraded.output=temp/("out-"+target);
            bool warned=false;
            degraded.event=[&](const Json& event) {
                if(event.value("type","")=="log" && event.value("level","")=="warn") warned=true;
                // Fail during raster output, or block the PRJ after the TIFF has been written.
                bool block=target=="orthophoto.prj" && event.value("stage","")=="orthophoto";
                if(block) for(const auto& item:fs::directory_iterator(degraded.checkpointDir))
                    if(item.path().filename().string().find(".orthophoto.building-")==0)
                        fs::create_directories(item.path()/"orthophoto"/target);
                if(target=="orthophoto.tif" && event.value("stage","")=="orthophoto")
                    throw std::runtime_error("Injected TIFF write failure");
            };
            auto report=convert(degraded);
            require(warned && report.at("status")=="complete" && report.at("orthophoto").at("status")=="failed" &&
                    !report.at("orthophoto").at("error").get<std::string>().empty() && !report.at("orthophoto").contains("uri"),"Missing orthophoto failure details");
            auto primary=expected;
            for(const auto* name:{"orthophoto.tif","orthophoto.tfw","orthophoto.prj"}) primary.erase(std::string("orthophoto/")+name);
            require(assets(degraded.output)==primary && !fs::exists(degraded.output/"orthophoto"),"Optional export failure changed the tileset output");
            ++checks;
        }
        unsigned sequence=0;
        auto pause=[&](unsigned boundary=1,unsigned threads=1,std::uint64_t budget=0) {
            Options options=baseline;auto name=std::to_string(sequence++);
            options.output=temp/("out-"+name);options.checkpointDir=temp/("checkpoint-"+name);options.pauseFile=temp/("pause-"+name);
            options.threads=threads;options.memoryBudgetBytes=budget;
            auto token=options.pauseFile;
            options.event=[token,boundary](const Json& event) {
                if(event.value("stage","")=="convert" && event.at("done").get<unsigned>()>=boundary) atomicWrite(token,{});
            };
            auto paused=convert(options);
            require(paused["status"]=="paused" && paused["filesCompleted"].get<unsigned>()>=boundary,"实际文件边界暂停");
            require(!fs::exists(options.output) && !fs::exists(options.checkpointDir/"work/tileset.json"),"暂停不得发布入口");
            fs::remove(options.pauseFile);options.event={};options.resume=true;return options;
        };
        auto resumedOptions=pause(3);std::map<std::string,fs::file_time_type> times;
        require(jsonFile(resumedOptions.checkpointDir/"identity.json")["payload"]["inputs"].contains(fs::canonical(temp/"external/original.png").u8string()),"身份必须包含输入目录外实际读取的原图");checks++;
        for(const auto& item:fs::recursive_directory_iterator(resumedOptions.checkpointDir/"work")) if(item.is_regular_file())
            times[item.path().lexically_relative(resumedOptions.checkpointDir/"work").generic_u8string()]=fs::last_write_time(item.path());
        unsigned partial=0,finished=0;
        for(const auto& item:fs::directory_iterator(resumedOptions.checkpointDir/"entries")) {
            auto entry=jsonFile(item.path())["payload"];if(entry["complete"]==true) ++finished;else ++partial;
        }
        require(partial>0 && finished>0,"检查点必须同时覆盖本地已完成父节点与已完成子树");checks++;
        auto resumed=convert(resumedOptions);
        require(resumed["status"]=="complete" && resumed["filesReused"]==3 && resumed["filesCompleted"]==6,"续转必须复用三个持久化文件实例");
        require(assets(resumedOptions.output)==expected && resumed["records"]==full["records"],"续转与完整转换逐资产字节/业务审计一致");
        require(!fs::exists(resumedOptions.checkpointDir),"成功后检查点清理");checks++;
        for(const auto& item:times) require(fs::last_write_time(resumedOptions.output/item.first)==item.second,"续转不得重写已完成内容: "+item.first);checks++;
        for(const auto& row:resumed["records"]) for(const auto& content:row["content"]) for(const auto& image:content["images"])
            require(readBytes(resumedOptions.output/image["uri"].get<std::string>())==readBytes(temp/"external/original.png"),"原编码图片逐字节保留");checks++;
        auto repeated=pause();auto repeatedToken=repeated.pauseFile;
        repeated.event=[repeatedToken](const Json& event) {if(event.value("stage","")=="convert" && event.at("done").get<unsigned>()>=2) atomicWrite(repeatedToken,{});};
        auto again=convert(repeated);require(again["status"]=="paused" && again["filesCompleted"]==2 && again["filesReused"]==1,"连续暂停不能丢失之前的完成文件");
        fs::remove(repeated.pauseFile);repeated.event={};auto third=convert(repeated);
        require(third["filesReused"]==2 && assets(repeated.output)==expected,"多次恢复结果一致");checks++;
        auto parallel=pause(1,2,1);auto parallelResult=convert(parallel);
        require(parallelResult["threads"]==2 && parallelResult["memoryBudgetBytes"]==1 && parallelResult["memoryThrottled"]==true && assets(parallel.output)==expected,"续转保留线程与预算门控");checks++;
        auto noBudget=pause(1,2,0);auto noBudgetResult=convert(noBudget);
        require(noBudgetResult["threads"]==2 && assets(noBudget.output)==expected,"并行根共享引用完整性");checks++;
        Options raw=baseline;raw.compressGeometry=false;raw.output=temp/"raw-baseline";auto rawResult=convert(raw);
        raw.checkpointDir=temp/"raw-checkpoint";raw.output=temp/"raw-resume";raw.pauseFile=temp/"raw-pause";
        auto rawToken=raw.pauseFile;raw.event=[rawToken](const Json& event) {if(event.value("stage","")=="convert" && event.at("done").get<unsigned>()>=2) atomicWrite(rawToken,{});};
        require(convert(raw)["status"]=="paused","raw 暂停");raw.event={};raw.resume=true;fs::remove(raw.pauseFile);
        auto rawResumed=convert(raw);require(rawResumed["records"]==rawResult["records"] && assets(raw.output)==assets(temp/"raw-baseline"),"raw 恢复逐文件一致");checks++;
        auto changed=pause();auto metadata=temp/"source/metadata.xml";auto bytes=readBytes(metadata);auto timestamp=fs::last_write_time(metadata);
        auto different=bytes;auto offset=std::string(bytes.begin(),bytes.end()).find("500000");different.at(offset+5)='1';
        atomicWrite(metadata,different);fs::last_write_time(metadata,timestamp);
        require(fs::file_size(metadata)==bytes.size() && fs::last_write_time(metadata)==timestamp,"同长度同 mtime 输入变更 fixture");
        rejected(changed,"同长度同 mtime 输入变化");atomicWrite(metadata,bytes);fs::last_write_time(metadata,timestamp);
        require(convert(changed)["filesReused"]==1,"恢复正确输入后检查点仍可用");checks++;
        auto geometryChanged=pause();auto leaf=temp/"source/leaf.osgb";auto leafBytes=readBytes(leaf);auto leafTime=fs::last_write_time(leaf);
        auto loaded=osgDB::readRefNodeFile(leaf.u8string());require(loaded.valid(),"读取真实源几何 fixture");
        auto positions=dynamic_cast<osg::Vec3Array*>(loaded->asGeode()->getDrawable(0)->asGeometry()->getVertexArray());
        require(positions && !positions->empty(),"源几何 fixture 必须有顶点");(*positions)[0].x()+=0.125f;
        nodeFile(loaded.get(),leaf,true);require(fs::file_size(leaf)==leafBytes.size(),"有效 OSGB 几何变更保持相同长度");
        fs::last_write_time(leaf,leafTime);rejected(geometryChanged,"有效 OSGB 同长度同 mtime 几何变化");
        atomicWrite(leaf,leafBytes);fs::last_write_time(leaf,leafTime);
        require(convert(geometryChanged)["filesReused"]==1,"源几何还原后可复用原检查点");checks++;
        auto external=pause();auto picture=temp/"external/original.png";auto original=readBytes(picture);
        auto copy=original;copy.back()^=1;atomicWrite(picture,copy);rejected(external,"外部原图变化");atomicWrite(picture,original);
        require(convert(external)["filesReused"]==1,"外部原图恢复后可续转");checks++;
        auto parameter=pause();
        for(unsigned option=0;option<7;++option) {
            auto altered=parameter;
            switch(option) {
            case 0: altered.compressGeometry=false;break;case 1: altered.preserveEncoded=false;break;
            case 2: altered.threads=2;break;case 3: altered.memoryBudgetBytes=1;break;
            case 4: altered.srs="EPSG:32650";break;case 5: altered.origin="500000,0,11";break;
            case 6: altered.output=temp/"different-output";break;
            }
            rejected(altered,"有效选项变化");checks++;
        }
        require(convert(parameter)["filesReused"]==1,"参数验证拒绝不能破坏旧检查点");checks++;
        auto inspectCorruption=[&](const auto& mutate,const std::string& message) {
            auto options=pause();mutate(options.checkpointDir);rejected(options,message);++checks;
        };
        inspectCorruption([](const fs::path& checkpoint) {
            for(const auto& file:fs::recursive_directory_iterator(checkpoint/"work")) if(file.path().extension()==".glb") {
                auto bytes=readBytes(file.path());bytes.back()^=1;atomicWrite(file.path(),bytes);return;
            }
            throw std::runtime_error("缺少 GLB fixture");
        },"GLB 损坏");
        inspectCorruption([](const fs::path& checkpoint) {
            auto file=fs::directory_iterator(checkpoint/"entries")->path();auto value=jsonFile(file);value["payload"]["complete"]=true;atomicJson(file,value);
        },"记录校验和损坏");
        inspectCorruption([](const fs::path& checkpoint) {fs::remove(fs::directory_iterator(checkpoint/"entries")->path());},"丢失记录");
        auto indexLost=pause(5);bool indexRemoved=false;
        for(const auto& item:fs::directory_iterator(indexLost.checkpointDir/"entries")) {
            auto entry=jsonFile(item.path())["payload"];
            if(entry["complete"]==false && entry["assets"].empty()) {fs::remove(item.path());indexRemoved=true;break;}
        }
        require(indexRemoved,"应有无本地资产的未完成索引记录");rejected(indexLost,"无资产索引记录丢失");checks++;
        inspectCorruption([](const fs::path& checkpoint) {
            fs::remove_all(checkpoint/"work");fs::create_directory(checkpoint/"work");
            fs::remove_all(checkpoint/"entries");fs::create_directory(checkpoint/"entries");
        },"全部工作文件和记录丢失，不能伪装空检查点");
        inspectCorruption([](const fs::path& checkpoint) {fs::remove(checkpoint/"state.json");},"总账丢失");
        inspectCorruption([](const fs::path& checkpoint) {atomicWrite(checkpoint/"work/content/old.glb",Bytes{1,2,3});},"额外旧资产");
        inspectCorruption([](const fs::path& checkpoint) {
            auto file=checkpoint/"identity.json";auto value=jsonFile(file);value["payload"]["converter"]["binary"]["sha256"]=std::string(64,'0');
            value["sha256"]=sha256(value["payload"].dump());atomicJson(file,value);
        },"不同转换器身份");
        inspectCorruption([](const fs::path& checkpoint) {
            auto file=fs::directory_iterator(checkpoint/"work/textures")->path();fs::remove(file);
        },"缺失原图资产");
#ifndef _WIN32
        inspectCorruption([&](const fs::path& checkpoint) {fs::create_symlink(temp/"external/original.png",checkpoint/"work/textures/foreign.png");},"符号链接资产");
#endif
        auto canceled=pause();atomicWrite(canceled.pauseFile,{});canceled.checkCancelled=[] {throw std::runtime_error("cancel");};
        bool failed=false;try {convert(canceled);} catch(const std::exception&) {failed=true;}
        require(failed && !fs::exists(canceled.checkpointDir) && !fs::exists(canceled.output),"已暂停任务取消优先于暂停且清理");checks++;
        Options active=baseline;active.output=temp/"active-output";active.checkpointDir=temp/"active-checkpoint";active.pauseFile=temp/"active-pause";bool cancel=false;
        active.event=[&](const Json& event) {if(event.value("stage","")=="convert" && event.at("done").get<unsigned>()>=1) {cancel=true;atomicWrite(active.pauseFile,{});}};
        active.checkCancelled=[&] {if(cancel) throw std::runtime_error("cancel");};failed=false;
        try {convert(active);} catch(const std::exception&) {failed=true;}
        require(failed && !fs::exists(active.checkpointDir) && !fs::exists(active.output),"运行中真正取消必须清理");checks++;
        Options changedParent=baseline;changedParent.output=temp/"changed-parent-output";changedParent.checkpointDir=temp/"changed-parent-checkpoint";
        changedParent.event=[&](const Json& event) {
            if(event.value("stage","")=="convert" && event.at("done")==2) for(const auto& item:fs::directory_iterator(changedParent.checkpointDir/"entries")) {
                auto record=jsonFile(item.path())["payload"]["prepared"]["record"];
                if(fs::path(record["source"].get<std::string>()).filename()=="a.osgb") {
                    auto file=changedParent.checkpointDir/"work"/record["content"][0]["uri"].get<std::string>();
                    auto data=readBytes(file);data.back()^=1;atomicWrite(file,data);
                }
            }
        };failed=false;try {convert(changedParent);} catch(const std::exception&) {failed=true;}
        require(failed && !fs::exists(changedParent.output) && !fs::exists(changedParent.checkpointDir),"子树完成不能重新登记已损坏父资产的哈希");checks++;
        auto atPublish=pause();atPublish.event=[&](const Json& event) {if(event.value("stage","")=="finalize") atomicWrite(metadata,different);};failed=false;
        try {convert(atPublish);} catch(const std::exception&) {failed=true;}
        atomicWrite(metadata,bytes);
        require(failed && !fs::exists(atPublish.output) && !fs::exists(atPublish.checkpointDir),"发布前源内容变化必须失败清理");checks++;
        auto damagedAtPublish=pause();damagedAtPublish.event=[&](const Json& event) {
            if(event.value("stage","")=="finalize") for(const auto& item:fs::directory_iterator(damagedAtPublish.checkpointDir/"work/content")) {
                auto data=readBytes(item.path());data.back()^=1;atomicWrite(item.path(),data);break;
            }
        };failed=false;
        try {convert(damagedAtPublish);} catch(const std::exception&) {failed=true;}
        require(failed && !fs::exists(damagedAtPublish.output) && !fs::exists(damagedAtPublish.checkpointDir),"发布前资产损坏必须失败清理");checks++;
        auto bad=mesh();auto badColors=new osg::Vec4Array;badColors->push_back({1,1,1,1});badColors->push_back({1,1,1,1});badColors->setBinding(osg::Array::BIND_PER_VERTEX);
        bad->getDrawable(0)->asGeometry()->setColorArray(badColors);nodeFile(bad.get(),temp/"bad/source.osgb");
        Options badOptions;badOptions.input=temp/"bad/source.osgb";badOptions.output=temp/"bad/output";badOptions.checkpointDir=temp/"bad/checkpoint";badOptions.srs="ENU:0,0,0";
        failed=false;try {convert(badOptions);} catch(const std::exception&) {failed=true;}
        require(failed && !fs::exists(badOptions.output) && !fs::exists(badOptions.checkpointDir),"正常转换异常不能保留可冒充暂停的检查点");checks++;
        Options immediate=baseline;immediate.output=temp/"immediate-out";immediate.checkpointDir=temp/"immediate-checkpoint";immediate.pauseFile=temp/"immediate-pause";
        atomicWrite(immediate.pauseFile,{});auto zero=convert(immediate);
        require(zero["status"]=="paused" && zero["filesCompleted"]==0,"已有暂停令牌应在第一个文件前停下");
        immediate.resume=true;fs::remove(immediate.pauseFile);auto zeroResume=convert(immediate);
        require(zeroResume["filesReused"]==0 && assets(immediate.output)==expected,"零进度检查点可恢复");checks++;
        auto occupied=pause();fs::create_directories(occupied.output);atomicWrite(occupied.output/"existing",Bytes{7});
        rejected(occupied,"非空输出");require(readBytes(occupied.output/"existing")==Bytes{7},"不能覆盖已有输出");checks++;
        auto fresh=pause();fresh.resume=false;rejected(fresh,"非空检查点未指定 resume");checks++;
        auto missing=pause();fs::remove_all(missing.checkpointDir);failed=false;try {convert(missing);} catch(const std::exception&) {failed=true;}
        require(failed && !fs::exists(missing.output),"resume 不能隐式重跑");checks++;
        auto finalize=pause(1);finalize.event=[&](const Json& event) {if(event.value("stage","")=="finalize") atomicWrite(finalize.pauseFile,{});};
        auto finalPause=convert(finalize);require(finalPause["status"]=="paused" && finalPause["filesCompleted"]==6 && !fs::exists(finalize.output),"发布边界暂停不能发布");
        fs::remove(finalize.pauseFile);finalize.event={};require(convert(finalize)["filesReused"]==6 && assets(finalize.output)==expected,"全文件完成后暂停恢复仍不重跑");checks++;
        {
            Checkpoint owner(temp/"lock-test",Json{{"task","test"}},false);failed=false;
            try {Checkpoint competitor(temp/"lock-test",Json{{"task","test"}},true);} catch(const std::exception&) {failed=true;}
            require(failed,"同一检查点必须排他锁定");owner.preserve();
        }
        {Checkpoint reopened(temp/"lock-test",Json{{"task","test"}},true);}
        require(!fs::exists(temp/"lock-test"),"释放锁后可恢复并清理");checks++;
        Options emptyDest=baseline;emptyDest.output=temp/"empty-existing-out";
        emptyDest.checkpointDir=temp/"empty-existing-checkpoint";emptyDest.pauseFile=temp/"empty-existing-pause";
        fs::create_directories(emptyDest.output);
        auto emptyToken=emptyDest.pauseFile;
        emptyDest.event=[emptyToken](const Json& event) {
            if(event.value("stage","")=="convert" && event.at("done").get<unsigned>()>=1) atomicWrite(emptyToken,{});
        };
        auto emptyPause=convert(emptyDest);
        require(emptyPause["status"]=="paused" && assets(emptyDest.output).empty(),"空目标目录暂停前不能发布");
        fs::remove(emptyDest.pauseFile);emptyDest.event={};emptyDest.resume=true;
        auto emptyResumed=convert(emptyDest);
        require(emptyResumed["status"]=="complete" && assets(emptyDest.output)==expected,"预建空目录检查点发布必须成功");checks++;
        Options trailingSep=baseline;trailingSep.output=temp/"trailing-sep-out/";
        trailingSep.checkpointDir=temp/"trailing-sep-checkpoint";trailingSep.pauseFile=temp/"trailing-sep-pause";
        auto trailingToken=trailingSep.pauseFile;
        trailingSep.event=[trailingToken](const Json& event) {
            if(event.value("stage","")=="convert" && event.at("done").get<unsigned>()>=1) atomicWrite(trailingToken,{});
        };
        auto trailingPause=convert(trailingSep);
        require(trailingPause["status"]=="paused" && !fs::exists(temp/"trailing-sep-out"),"尾斜杠输出暂停不得发布");
        fs::remove(trailingSep.pauseFile);trailingSep.event={};trailingSep.resume=true;
        auto trailingResumed=convert(trailingSep);
        require(trailingResumed["status"]=="complete" && assets(temp/"trailing-sep-out")==expected,"尾斜杠输出路径发布必须成功");checks++;
        std::cout<<"通过 "<<checks<<" 项检查点回归；真实 fixture: "<<temp<<'\n';return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
