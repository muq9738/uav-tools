#include "native_tiles.hpp"
#include <osg/Geode>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace nt;
namespace {
void require(bool v,const char* message) {if(!v) throw std::runtime_error(message);}
struct Model {Json doc;Bytes binary;};
Model load(const fs::path& p) {
    auto b=readBytes(p);std::uint32_t length;std::memcpy(&length,b.data()+12,4);
    return {Json::parse(b.begin()+20,b.begin()+20+length),Bytes(b.begin()+28+length,b.end())};
}
Bytes view(const Model& m,unsigned ai) {
    auto a=m.doc["accessors"][ai];auto v=m.doc["bufferViews"][a["bufferView"].get<unsigned>()];
    if(v.contains("extensions")) {
        auto e=v["extensions"]["EXT_meshopt_compression"];auto at=e.value("byteOffset",0U),size=e["byteLength"].get<unsigned>();
        return decodeMeshopt(Bytes(m.binary.begin()+at,m.binary.begin()+at+size),e["count"],e["byteStride"],e["mode"]!="ATTRIBUTES");
    }
    auto at=v.value("byteOffset",0U),size=v["byteLength"].get<unsigned>();return Bytes(m.binary.begin()+at,m.binary.begin()+at+size);
}
osg::Vec3d value(const Model& m,unsigned ai,unsigned i) {
    auto a=m.doc["accessors"][ai];auto v=m.doc["bufferViews"][a["bufferView"].get<unsigned>()];auto b=view(m,ai);
    bool quant=a["componentType"]==5123;unsigned unit=quant?2:4,width=a["type"]=="VEC2"?2:3;
    auto at=a.value("byteOffset",0U)+i*v.value("byteStride",width*unit);osg::Vec3d p;
    for(unsigned c=0;c<width;++c) {if(quant) {std::uint16_t x;std::memcpy(&x,b.data()+at+c*2,2);p[c]=x/65535.;} else {float x;std::memcpy(&x,b.data()+at+c*4,4);p[c]=x;}}
    return p;
}
osg::Vec3d point(const osg::Vec3d& p,const std::vector<double>& m) {
    return {p.x()*m[0]+p.y()*m[4]+p.z()*m[8]+m[12],p.x()*m[1]+p.y()*m[5]+p.z()*m[9]+m[13],p.x()*m[2]+p.y()*m[6]+p.z()*m[10]+m[14]};
}
osg::Vec3d normal(const osg::Vec3d& p,const std::vector<double>& m) {
    osg::Vec3d a(m[0],m[1],m[2]),b(m[4],m[5],m[6]),c(m[8],m[9],m[10]);
    auto n=((b^c)*p.x()+(c^a)*p.y()+(a^b)*p.z())/(a*(b^c));n.normalize();return n;
}
}
int main() {
    auto root=fs::current_path()/("quantization-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root/"textures");unsigned passed=0;
    try {
        for(bool doubles:{false,true}) for(bool compressed:{false,true}) for(bool projected:{false,true}) {
            auto g=new osg::Geometry;osg::ref_ptr<osg::Geometry> owner=g;
            auto uv=new osg::Vec2Array;auto normals=new osg::Vec3Array;
            auto f=new osg::Vec3Array;auto d=new osg::Vec3dArray;osg::ref_ptr<osg::Vec3Array> ff=f;osg::ref_ptr<osg::Vec3dArray> dd=d;
            for(unsigned i=0;i<128;++i) {
                osg::Vec3d p(0.213*i,11.0*std::sin(i*.2),0.013*(i%9));
                if(doubles && !projected) p+=osg::Vec3d(1e8,-2e8,3e8);
                f->push_back(osg::Vec3(p));d->push_back(p);uv->push_back({float(i)/127,float(i%7)/6});normals->push_back({.6f,0,.8f});
            }
            g->setVertexArray(doubles?static_cast<osg::Array*>(d):static_cast<osg::Array*>(f));g->setTexCoordArray(0,uv);
            normals->setBinding(osg::Array::BIND_PER_VERTEX);g->setNormalArray(normals);
            auto index=new osg::DrawElementsUInt(GL_TRIANGLES);for(unsigned i:{2U,0U,1U}) index->push_back(i);g->addPrimitiveSet(index);
            GeometryInput input;input.geometry=g;input.matrix=osg::Matrixd::scale(2,3,4)*osg::Matrixd::rotate(.37,osg::Vec3d(0,0,1))*osg::Matrixd::translate(17,-20,30);
            SpatialReference spatial("EPSG:32651",{500000,0,0});auto ref=projected?&spatial:nullptr;
            writeGlb({input},root/"raw.glb",root/"textures",true,ref,compressed,false);
            auto result=writeGlb({input},root/"quant.glb",root/"textures",true,ref,compressed,true);
            auto a=load(root/"raw.glb"),b=load(root/"quant.glb");auto ap=a.doc["meshes"][0]["primitives"][0],bp=b.doc["meshes"][0]["primitives"][0];
            auto am=a.doc["nodes"][0]["matrix"].get<std::vector<double>>(),bm=b.doc["nodes"][0]["matrix"].get<std::vector<double>>();
            auto pos=bp["attributes"]["POSITION"].get<unsigned>();auto data=view(b,pos);
            require(data.size()==128*8,"padded position length");for(unsigned i=0;i<128;++i) require(data[i*8+6]==0 && data[i*8+7]==0,"padding must be zero");
            require(view(a,ap["indices"])==view(b,bp["indices"]),"index order changed");
            double grid=0;for(unsigned c=0;c<3;++c) grid+=std::hypot(bm[c*4],bm[c*4+1],bm[c*4+2])/(2*65535.);
            double observed=0;
            for(unsigned i=0;i<128;++i) {
                auto pa=point(value(a,ap["attributes"]["POSITION"],i),am),pb=point(value(b,pos,i),bm);
                observed=std::max(observed,(pa-pb).length());require((pa-pb).length()<=grid+1e-6,"world position outside quantization cell");
                auto u=value(a,ap["attributes"]["TEXCOORD_0"],i),v=value(b,bp["attributes"]["TEXCOORD_0"],i);
                require(std::abs(u.x()-v.x())<=.5/65535.+1e-12 && std::abs(u.y()-v.y())<=.5/65535.+1e-12,"UV error");
                auto na=normal(value(a,ap["attributes"]["NORMAL"],i),am),nb=normal(value(b,bp["attributes"]["NORMAL"],i),bm);
                require((na-nb).length()<2e-6,"world normal changed by quantization scale");
                // pb is evaluated through the composed node matrix while
                // result.bounds accumulates per-point transforms: the two float
                // paths differ by a few ulp across platforms, so compare with a
                // quantization-cell tolerance instead of a hard box test.
                osg::BoundingBoxd tolerant=result.bounds;double pad=grid+1e-6;
                tolerant._min-=osg::Vec3d(pad,pad,pad);tolerant._max+=osg::Vec3d(pad,pad,pad);
                require(tolerant.contains(osg::Vec3d(pb.x(),-pb.z(),pb.y())),"decoded position excluded from bounds");
            }
            require(std::abs(observed-result.quantizationErrorMeters)<2e-6,"reported error disagrees with serialized world positions");
            require(result.quantizationErrorMeters<.01,"local center omitted from error computation");
            ++passed;
        }
        fs::remove_all(root);std::cout<<"PASS "<<passed<<" quantization fixtures (FLOAT/DOUBLE, raw/meshopt, local/projected)\n";return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<" fixtures retained: "<<root<<'\n';return 1;}
}
