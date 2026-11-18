#include "native_tiles.hpp"
#include "checkpoint.hpp"
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/Texture2D>
#include <osgDB/WriteFile>
#include <osgDB/ReadFile>
#include <png.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace nt;
namespace {
void require(bool c,const std::string& message) {if(!c) throw std::runtime_error(message);}
void nodeFile(osg::Node* n,const fs::path& path) {
    fs::create_directories(path.parent_path());osg::ref_ptr<osgDB::Options> o=new osgDB::Options("WriteImageHint=IncludeFile Compressor=zlib");
    require(osgDB::writeNodeFile(*n,path.u8string(),o),"write OSGB fixture");
}
osg::ref_ptr<osg::Geode> grid(unsigned divisions=8) {
    auto g=new osg::Geometry;auto p=new osg::Vec3Array;auto uv=new osg::Vec2Array;
    for(unsigned y=0;y<=divisions;++y) for(unsigned x=0;x<=divisions;++x) {
        p->push_back({float(x)*8/divisions,float(y)*8/divisions,0});uv->push_back({float(x)/divisions,float(y)/divisions});
    }
    auto indices=new osg::DrawElementsUInt(GL_TRIANGLES);
    for(unsigned y=0;y<divisions;++y) for(unsigned x=0;x<divisions;++x) {
        unsigned a=y*(divisions+1)+x,b=a+1,c=a+divisions+1,d=c+1;for(auto v:{a,b,d,a,d,c}) indices->push_back(v);
    }
    g->setVertexArray(p);g->setTexCoordArray(0,uv);g->addPrimitiveSet(indices);
    auto image=new osg::Image;image->allocateImage(4,4,1,GL_RGBA,GL_UNSIGNED_BYTE,1);image->setFileName("grid.png");
    for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) {auto c=image->data(x,y);c[0]=x*64;c[1]=y*64;c[2]=80;c[3]=80+x*40;}
    auto texture=new osg::Texture2D(image);texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
    texture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
    g->getOrCreateStateSet()->setTextureAttributeAndModes(0,texture,osg::StateAttribute::ON);g->getOrCreateStateSet()->setMode(GL_BLEND,osg::StateAttribute::ON);
    auto geode=new osg::Geode;geode->addDrawable(g);return geode;
}
void fixture(const fs::path& directory,unsigned count=2,bool coincident=false) {    for(unsigned i=0;i<count;++i) {
        auto transform=new osg::MatrixTransform;transform->setMatrix(osg::Matrixd::scale(1.5,2,1)*osg::Matrixd::translate(coincident?0:double(i)*20,3,2));
        auto root=new osg::Group;root->addChild(grid().get());
        if(i==0) {auto ref=new osg::PagedLOD;ref->setFileName(0,"leaf.osgb");ref->setRange(0,0,1e9);root->addChild(ref);}
        transform->addChild(root);nodeFile(transform,directory/("root"+std::to_string(i)+".osgb"));
    }
    nodeFile(grid(12).get(),directory/"leaf.osgb");
}
Json jsonFile(const fs::path& p) {return Json::parse(readBytes(p));}Json gltf(const Bytes& b) {unsigned length=0;std::memcpy(&length,b.data()+12,4);return Json::parse(b.begin()+20,b.begin()+20+length);}
std::vector<double> attribute(const Bytes& bytes,const Json& doc,unsigned ai) {
    auto& a=doc["accessors"][ai];auto& view=doc["bufferViews"][a["bufferView"].get<unsigned>()];require(!view.contains("extensions"),"oracle requires raw GLB");
    unsigned jsonLength=0;std::memcpy(&jsonLength,bytes.data()+12,4);unsigned offset=28+jsonLength+view.value("byteOffset",0U)+a.value("byteOffset",0U);
    unsigned width=a["type"]=="VEC4"?4:a["type"]=="VEC3"?3:a["type"]=="VEC2"?2:1;
    unsigned size=a["componentType"]==5126||a["componentType"]==5125?4:2;unsigned stride=view.value("byteStride",width*size);
    std::vector<double> result;for(unsigned i=0;i<a["count"].get<unsigned>();++i) for(unsigned k=0;k<width;++k) {
        auto p=bytes.data()+offset+i*stride+k*size;require(p+size<=bytes.data()+bytes.size(),"accessor range");
        if(a["componentType"]==5126) {float v;std::memcpy(&v,p,4);result.push_back(v);}else {unsigned v=0;std::memcpy(&v,p,size);result.push_back(v);}
    }
    return result;
}
std::set<std::string> uris(const Json& tile) {
    std::set<std::string> out;if(tile.contains("content")) out.insert(tile["content"]["uri"].get<std::string>());
    if(tile.contains("children")) for(const auto& c:tile["children"]) {auto sub=uris(c);out.insert(sub.begin(),sub.end());}return out;
}
void auditParent(const fs::path& output,const Json& row) {
    auto bytes=readBytes(output/row["content"].get<std::string>());auto doc=gltf(bytes);unsigned triangles=0,measurements=0;
    osg::Matrixd yToZ(1,0,0,0,0,0,1,0,0,-1,0,0,0,0,0,1);
    for(const auto& image:doc["images"]) {
        auto uri=image["uri"].get<std::string>();auto png=readBytes(output/"content"/uri);png_image decoded{};decoded.version=PNG_IMAGE_VERSION;
        require(png_image_begin_read_from_memory(&decoded,png.data(),png.size())!=0,"decode atlas");
        require(decoded.width==1024 && decoded.height==1024,"atlas dimensions");decoded.format=PNG_FORMAT_RGBA;Bytes rgba(PNG_IMAGE_SIZE(decoded));
        require(png_image_finish_read(&decoded,nullptr,rgba.data(),0,nullptr)!=0,"read atlas pixels");
        bool alpha=false;for(unsigned i=3;i<rgba.size();i+=4) if(rgba[i]>0 && rgba[i]<255) alpha=true;
        require(alpha,"atlas must retain source alpha");png_image_free(&decoded);
    }
    for(const auto& node:doc["nodes"]) {
        auto matrix=node["matrix"].get<std::vector<double>>();osg::Matrixd transform(matrix.data());
        for(const auto& primitive:doc["meshes"][node["mesh"].get<unsigned>()]["primitives"]) {
            require(doc["materials"][primitive["material"].get<unsigned>()]["alphaMode"]=="BLEND","parent alpha mode");
            auto p=attribute(bytes,doc,primitive["attributes"]["POSITION"].get<unsigned>()),uv=attribute(bytes,doc,primitive["attributes"]["TEXCOORD_0"].get<unsigned>());
            auto indices=attribute(bytes,doc,primitive["indices"].get<unsigned>());triangles+=indices.size()/3;
            for(auto v:uv) require(std::isfinite(v)&&v>=0&&v<=1,"atlas UV bounds");
            for(unsigned i=0;i<indices.size();i+=3) {
                osg::Vec3d corners[3];for(unsigned k=0;k<3;++k) {unsigned v=indices[i+k];corners[k]=osg::Vec3d(p[v*3],p[v*3+1],p[v*3+2])*transform*yToZ;
                    require(std::abs(corners[k].z()-2)<1e-6,"Top world local z: transform exactly once");}
                require(((corners[1]-corners[0])^(corners[2]-corners[0])).z()>0,"QEM must not invert winding");++measurements;
            }
        }
    }
    require(triangles==row["outputTriangles"],"independent parent face count");require(measurements>0,"no parent drawing");
    require(row["bake"]["bakedPixels"].get<unsigned>()>0,"no projected pixels");require(row["bake"]["unmatchedPixels"]==0,"flat fixture must project every texel");
    require(row["bake"]["maxNormalProjectionDistanceMeters"].get<double>()<1e-8,"flat fixture surface projection");
}
}
int main(int argc,char** argv) {
    try {
        if(argc>=5 && std::string(argv[1])=="--index") {
            auto root=new osg::Group;
            for(int i=3;i<argc;++i) {auto p=new osg::PagedLOD;p->setFileName(0,fs::absolute(argv[i]).u8string());p->setRange(0,0,1e9);root->addChild(p);}
            nodeFile(root,fs::absolute(argv[2]));return 0;
        }
        if(argc==3 && std::string(argv[1])=="--fixture") {fixture(fs::absolute(argv[2]));return 0;}
        auto root=argc==3 && std::string(argv[1])=="--root"?fs::absolute(argv[2]):fs::current_path()/("top-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fixture(root/"input");Options o;o.input=root/"input";o.output=root/"baseline";o.srs="ENU:0,0";o.compressGeometry=false;o.threads=1;
        auto baseline=convert(o);o.output=root/"rebuilt";o.rebuildTop=true;
        unsigned observed=0;
        o.observeTopGeometry=[&](const std::string& name,const std::vector<GeometryInput>& meshes) {
            ++observed;writeGlb(meshes,root/"observed/content"/name,root/"observed/textures",false);
        };
        auto top=convert(o);o.observeTopGeometry={};
        require(observed==1 && readBytes(root/"observed/content/top-0.glb")==readBytes(o.output/"content/top-0.glb"),
                "pre-encoding Top observer must capture exactly the published raw geometry");
        require(top["topGlbs"]==1 && top["topLossy"]==true,"two roots must create one ancestor");
        require(baseline["sourceExact"]==true && top["sourceExact"]==false && top["sourceContentsExact"]==true,
                "lossy ancestors must not label the whole output source-exact");
        auto before=uris(jsonFile(root/"baseline/tileset.json")["root"]),after=uris(jsonFile(o.output/"tileset.json")["root"]);
        for(const auto& uri:before) {require(after.count(uri),"lost original content");require(readBytes(root/"baseline"/uri)==readBytes(o.output/uri),"source content changed");}
        require(after.size()==before.size()+1,"ancestor/source content coverage");
        for(const auto& image:fs::directory_iterator(root/"baseline/textures")) require(readBytes(image.path())==readBytes(o.output/"textures"/image.path().filename()),"original texture changed");
        auto row=top["topAncestors"][0];require(row["inputTriangles"]==256 && row["targetTriangles"]==128,"OSGB average face budget");
        require(row["outputTriangles"].get<unsigned>()<=128 && row["collapses"].get<unsigned>()>0,"actual constrained QEM reduction");auditParent(o.output,row);
        // Pause at start of Top after all sources have durable records. Reject
        // changing the switch, then rebuild derived assets on a true resume.
        o.output=root/"resumed";o.checkpointDir=root/"checkpoint";o.pauseFile=root/"pause";
        o.event=[&](const Json& e) {if(e.value("stage","")=="rebuild-top") atomicWrite(o.pauseFile,{});};
        auto paused=convert(o);require(paused["status"]=="paused","Top stage pause");
        require(!fs::exists(o.checkpointDir/"work/.top-building"),"pause must clean derived staging");
        fs::remove(o.pauseFile);o.event={};o.resume=true;o.rebuildTop=false;bool rejected=false;
        try {convert(o);} catch(const std::exception& e) {rejected=std::string(e.what()).find("身份不匹配")!=std::string::npos;}
        require(rejected,"Top parameter identity mismatch must reject resume");o.rebuildTop=true;
        auto resumed=convert(o);require(resumed["status"]=="complete" && resumed["filesReused"]==baseline["records"].size(),"resume source reuse");
        for(const auto& uri:after) require(readBytes(root/"rebuilt"/uri)==readBytes(o.output/uri),"resumed derived/source asset mismatch");
        require(!fs::exists(o.checkpointDir),"successful publication cleans checkpoint");
        o.checkpointDir.clear();o.pauseFile.clear();o.resume=false;o.input=root/"many";fixture(o.input,6);o.output=root/"many-out";
        auto many=convert(o);require(many["topGlbs"].get<unsigned>()>=2,"spatial grouping of >4 roots");
        o.output=root/"many-resumed";o.checkpointDir=root/"many-checkpoint";o.pauseFile=root/"many-pause";
        o.event=[&](const Json& e) {if(e.value("stage","")=="rebuild-top" && e.at("done").get<unsigned>()>=1) atomicWrite(o.pauseFile,{});};
        auto midTop=convert(o);require(midTop["status"]=="paused","pause after an actual derived ancestor");
        require(!fs::exists(o.checkpointDir/"work/.top-building"),"discard partial Top assets on pause");
        fs::remove(o.pauseFile);o.event={};o.resume=true;auto manyResumed=convert(o);
        auto expectedMany=uris(jsonFile(root/"many-out/tileset.json")["root"]);
        for(const auto& uri:expectedMany) require(readBytes(root/"many-out"/uri)==readBytes(o.output/uri),"mid-Top resume asset equality");
        require(manyResumed["topGlbs"]==many["topGlbs"],"mid-Top resume regenerates all ancestors");
        o.resume=false;o.checkpointDir.clear();o.pauseFile.clear();
        o.input=root/"coincident";fixture(o.input,5,true);o.output=root/"coincident-out";auto degenerate=convert(o);
        require(degenerate["topGlbs"]==1 && degenerate["topAncestors"][0]["inputCount"]==5,"degenerate group must merge directly");
        // Two blocks 0.5 m apart: their facing boundary chains must be sealed
        // before QEM, otherwise the gap survives every ancestor level.
        auto seamFixture=[&](const fs::path& directory,double offset) {
            for(unsigned i=0;i<2;++i) {auto transform=new osg::MatrixTransform;transform->setMatrix(osg::Matrixd::translate(double(i)*offset,0,0));
                auto seamRoot=new osg::Group;seamRoot->addChild(grid().get());transform->addChild(seamRoot);nodeFile(transform,directory/("root"+std::to_string(i)+".osgb"));}
        };
        o.input=root/"seam";seamFixture(o.input,8.02);o.output=root/"seam-out";auto sealed=convert(o);
        require(sealed["topGlbs"].get<unsigned>()>=1,"seam fixture must create an ancestor");
        auto sealRow=sealed["topAncestors"][0]["boundarySeal"];
        require(sealRow["pairsInRange"].get<unsigned>()>=1,"boundary chains within the tolerance must be considered");
        require(sealRow["mergePairs"].get<unsigned>()>=1,"a 0.02 m crack must be sealed");
        require(sealRow["welds"].get<unsigned>()>0,"sealing must weld vertices");
        require(sealRow["maxMergeGapMeters"].get<double>()<=sealRow["sealDistanceMeters"].get<double>()+1e-9,"seal gap stays within the tolerance");
        require(sealRow["sealDistanceMeters"].get<double>()==2.,"default tolerance matches the reference 2 m");
        // 0.5 m is inside the reference radius, so parity welds it too.
        o.input=root/"wide-seam";seamFixture(o.input,8.5);o.output=root/"wide-seam-out";auto wide=convert(o);
        auto wideRow=wide["topAncestors"][0]["boundarySeal"];
        require(wideRow["welds"].get<unsigned>()>0,"default 2 m tolerance must weld a 0.5 m gap");
        // Disabling the weld must leave both blocks separate.
        o.topSealDistance=0.;o.output=root/"wide-seam-off";auto off=convert(o);
        auto offRow=off["topAncestors"][0]["boundarySeal"];
        require(offRow["welds"].get<unsigned>()==0 && offRow["pairsInRange"].get<unsigned>()==0,"zero tolerance disables sealing");
        o.topSealDistance=2.;
        // A pure index root should expose its resident frontier, preserving leaves.
        auto index=new osg::Group;for(unsigned i=0;i<2;++i) {auto p=new osg::PagedLOD;p->setFileName(0,"root"+std::to_string(i)+".osgb");p->setRange(0,0,1e9);index->addChild(p);}nodeFile(index,root/"input/index.osgb");
        o.input=root/"input/index.osgb";o.output=root/"index-out";auto indexed=convert(o);require(indexed["topGlbs"]==1,"pure index frontier");
        fixture(root/"unequal");auto smaller=new osg::MatrixTransform;
        smaller->setMatrix(osg::Matrixd::scale(1.5,2,1)*osg::Matrixd::translate(20,3,2));smaller->addChild(grid(4));nodeFile(smaller,root/"unequal/root1.osgb");
        o.input=root/"unequal";o.output=root/"unequal-out";auto unequal=convert(o);
        require(unequal["topAncestors"][0]["inputTriangles"]==160 && unequal["topAncestors"][0]["targetTriangles"]==80,"unequal input average, excluding paged descendants");
        fixture(root/"colored");
        for(unsigned i=0;i<2;++i) {
            auto file=root/"colored"/("root"+std::to_string(i)+".osgb");auto node=osgDB::readRefNodeFile(file.u8string());
            auto geode=dynamic_cast<osg::Geode*>(node->asGroup()->getChild(0)->asGroup()->getChild(0));require(geode,"colored fixture hierarchy");
            auto colors=new osg::Vec4Array;colors->push_back(i==0?osg::Vec4(1,0,0,1):osg::Vec4(0,0,1,1));
            geode->getDrawable(0)->asGeometry()->setColorArray(colors,osg::Array::BIND_OVERALL);nodeFile(node,file);
        }
        o.input=root/"colored";o.output=root/"colored-out";auto colored=convert(o);auto parent=colored["topAncestors"][0];
        auto coloredBytes=readBytes(o.output/parent["content"].get<std::string>());auto coloredDoc=gltf(coloredBytes);std::set<std::string> palette;
        for(const auto& mesh:coloredDoc["meshes"]) for(const auto& primitive:mesh["primitives"]) {
            auto c=attribute(coloredBytes,coloredDoc,primitive["attributes"]["COLOR_0"].get<unsigned>());
            for(unsigned i=0;i<c.size();i+=4) {
                require(c[i+1]==0 && c[i+3]==1 && ((c[i]==1 && c[i+2]==0)||(c[i]==0 && c[i+2]==1)),"material/color boundary must remain distinct");
                palette.insert(c[i]==1?"red":"blue");
            }
        }
        require(palette==std::set<std::string>({"red","blue"}),"both material color partitions survive");
        // Failure and cancellation own and clean all unpublished assets.
        o.input=root/"input";o.output=root/"cancel-out";o.event={};o.checkCancelled=[&] {if(fs::exists(o.output.parent_path()/"cancel")) throw std::runtime_error("cancelled");};
        o.event=[&](const Json& e) {if(e.value("stage","")=="rebuild-top") atomicWrite(root/"cancel",{});};rejected=false;
        try {convert(o);} catch(const std::exception&) {rejected=true;}require(rejected&&!fs::exists(o.output),"Top cancellation cleanup");
        atomicJson(root/"test-result.json",{{"passed",true},{"baselineFiles",baseline["files"]},{"originalContents",before.size()},{"top",top["topAncestors"]},{"manyAncestors",many["topGlbs"]},{"coincidentAncestors",degenerate["topGlbs"]},{"resumeFilesReused",resumed["filesReused"]}});
        std::cout<<"Top native OSGB checks passed: "<<root<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
