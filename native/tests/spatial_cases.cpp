#include "native_tiles.hpp"
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osgDB/WriteFile>
#include <proj.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace nt;
namespace {
constexpr double pi=3.14159265358979323846,a=6378137.0,f=1/298.257223563,e2=f*(2-f),rad=pi/180;
double largestWorldError=0,largestFloatBound=0,largestReportedRounding=0;
void require(bool value,const std::string& message) {if(!value) throw std::runtime_error(message);}
void close(const osg::Vec3d& actual,const osg::Vec3d& expected,double tolerance,const std::string& message) {
    auto error=(actual-expected).length();
    std::ostringstream detail;detail<<std::setprecision(12)<<message<<" error="<<error<<" tolerance="<<tolerance;
    require(error<=tolerance,detail.str());
}
struct Frame {osg::Vec3d east,north,up,position;};
// 独立 WGS84 椭球/ENU 公式；不调用生产 enuTransform 或 PROJ。
Frame frame(double lat,double lon,double height) {
    double s=std::sin(lat*rad),c=std::cos(lat*rad),sl=std::sin(lon*rad),cl=std::cos(lon*rad);
    double n=a/std::sqrt(1-e2*s*s);
    return {{-sl,cl,0},{-s*cl,-s*sl,c},{c*cl,c*sl,s},{(n+height)*c*cl,(n+height)*c*sl,(n*(1-e2)+height)*s}};
}
osg::Vec3d inFrame(const Frame& basis,const osg::Vec3d& p) {return basis.position+basis.east*p.x()+basis.north*p.y()+basis.up*p.z();}
osg::Vec3d geographicNormal(double lat,double lon,double h,const osg::Vec3d& normal) {
    auto basis=frame(lat,lon,h);double s=std::sin(lat*rad),q=1-e2*s*s;
    double n=a/std::sqrt(q),m=a*(1-e2)/std::pow(q,1.5);
    auto result=basis.east*(normal.x()/((n+h)*std::cos(lat*rad)*rad))+basis.north*(normal.y()/((m+h)*rad))+basis.up*normal.z();
    result.normalize();return result;
}
// Snyder UTM 逆级数；夹具限定在 51 带中央经线/赤道附近 1 km，截断远小于纳米。
osg::Vec3d utmWorld(const osg::Vec3d& p) {
    require(std::abs(p.x()-500000)<1000&&std::abs(p.y())<1000,"independent UTM series fixture domain");
    constexpr double k=.9996;double ep=e2/(1-e2),e1=(1-std::sqrt(1-e2))/(1+std::sqrt(1-e2));
    double mu=(p.y()/k)/(a*(1-e2/4-3*e2*e2/64-5*e2*e2*e2/256-175*std::pow(e2,4)/16384));
    double phi=mu+(3*e1/2-27*std::pow(e1,3)/32)*std::sin(2*mu)+(21*e1*e1/16-55*std::pow(e1,4)/32)*std::sin(4*mu)
        +151*std::pow(e1,3)/96*std::sin(6*mu)+1097*std::pow(e1,4)/512*std::sin(8*mu);
    double s=std::sin(phi),c=std::cos(phi),t=std::tan(phi),n=a/std::sqrt(1-e2*s*s),r=a*(1-e2)/std::pow(1-e2*s*s,1.5);
    double d=(p.x()-500000)/(n*k),cc=ep*c*c,tt=t*t;
    double lat=phi-n*t/r*(d*d/2-(5+3*tt+10*cc-4*cc*cc-9*ep)*std::pow(d,4)/24
        +(61+90*tt+298*cc+45*tt*tt-252*ep-3*cc*cc)*std::pow(d,6)/720);
    double lon=123*rad+(d-(1+2*tt+cc)*std::pow(d,3)/6+(5-2*cc+28*tt-3*cc*cc+8*ep+24*tt*tt)*std::pow(d,5)/120)/c;
    return frame(lat/rad,lon/rad,p.z()).position;
}
Json jsonFile(const fs::path& path) {return Json::parse(readBytes(path));}
struct Model {Json doc;Bytes bytes;std::size_t bin;};
Model model(const fs::path& path) {
    auto bytes=readBytes(path);require(bytes.size()>=28,"GLB header");std::uint32_t n;std::memcpy(&n,bytes.data()+12,4);
    require(28+n<=bytes.size(),"GLB JSON bounds");auto doc=Json::parse(bytes.begin()+20,bytes.begin()+20+n);return {doc,std::move(bytes),28+n};
}
Bytes elements(const Model& m,unsigned accessor) {
    const auto& item=m.doc["accessors"][accessor];const auto& view=m.doc["bufferViews"][item["bufferView"].get<unsigned>()];Bytes data;
    if(view.contains("extensions")) {
        const auto& ext=view["extensions"]["EXT_meshopt_compression"];auto start=m.bin+ext.value("byteOffset",0U),size=ext["byteLength"].get<std::size_t>();
        require(start+size<=m.bytes.size(),"compressed view bounds");data=decodeMeshopt(Bytes(m.bytes.begin()+start,m.bytes.begin()+start+size),ext["count"],ext["byteStride"],ext["mode"]!="ATTRIBUTES");
    } else {auto start=m.bin+view.value("byteOffset",0U),size=view["byteLength"].get<std::size_t>();require(start+size<=m.bytes.size(),"view bounds");data=Bytes(m.bytes.begin()+start,m.bytes.begin()+start+size);}
    auto offset=item.value("byteOffset",0U);require(offset<=data.size(),"accessor bounds");return Bytes(data.begin()+offset,data.end());
}
osg::Vec3d vectorAt(const Bytes& bytes,unsigned i) {float p[3];require((i+1)*12<=bytes.size(),"VEC3 bounds");std::memcpy(p,bytes.data()+i*12,12);return {p[0],p[1],p[2]};}
osg::Vec3d pointMatrix(const osg::Vec3d& p,const std::vector<double>& m) {
    return {p.x()*m[0]+p.y()*m[4]+p.z()*m[8]+m[12],p.x()*m[1]+p.y()*m[5]+p.z()*m[9]+m[13],p.x()*m[2]+p.y()*m[6]+p.z()*m[10]+m[14]};
}
// 以三个行向量的余子式计算逆转置，独立于 OSG 的矩阵求逆与 transform3x3。
osg::Vec3d normalMatrix(const osg::Vec3d& n,const std::vector<double>& m) {
    osg::Vec3d r0(m[0],m[1],m[2]),r1(m[4],m[5],m[6]),r2(m[8],m[9],m[10]);double det=r0*(r1^r2);
    require(std::isfinite(det)&&det!=0,"normal matrix determinant");auto result=((r1^r2)*n.x()+(r2^r0)*n.y()+(r0^r1)*n.z())/det;result.normalize();return result;
}
osg::Vec3d yToZ(const osg::Vec3d& p) {return {p.x(),-p.z(),p.y()};}
struct Decoded {std::vector<osg::Vec3d> world,normals;double floatBound=0;};
Decoded decode(const Model& m,const std::vector<double>& placement) {
    Decoded out;
    for(const auto& node:m.doc["nodes"]) {
        auto matrix=node["matrix"].get<std::vector<double>>();
        for(const auto& primitive:m.doc["meshes"][node["mesh"].get<unsigned>()]["primitives"]) {
            const auto& attrs=primitive["attributes"];unsigned pos=attrs["POSITION"];auto values=elements(m,pos);
            for(unsigned i=0;i<m.doc["accessors"][pos]["count"].get<unsigned>();++i) {
                auto local=vectorAt(values,i);out.world.push_back(pointMatrix(yToZ(pointMatrix(local,matrix)),placement));
                // 每个 float 分量两侧 ULP 的最大半值，经节点线性部分传播后的 L1 上界。
                double bound=0;
                for(unsigned c=0;c<3;++c) {
                    float v=static_cast<float>(local[c]);double ulp=std::max(double(std::nextafter(v,std::numeric_limits<float>::infinity()))-v,double(v)-std::nextafter(v,-std::numeric_limits<float>::infinity()));
                    double scale=std::sqrt(matrix[c*4]*matrix[c*4]+matrix[c*4+1]*matrix[c*4+1]+matrix[c*4+2]*matrix[c*4+2]);bound+=ulp*.5*scale;
                }
                out.floatBound=std::max(out.floatBound,bound);
                if(attrs.contains("NORMAL")) out.normals.push_back(normalMatrix(yToZ(normalMatrix(vectorAt(elements(m,attrs["NORMAL"]),i),matrix)),placement));
            }
        }
    }
    return out;
}
void writeNode(osg::Node* node,const fs::path& path) {fs::create_directories(path.parent_path());require(osgDB::writeNodeFile(*node,path.string()),"write OSGB fixture");}
osg::ref_ptr<osg::Geode> geometry(const std::vector<osg::Vec3d>& points,const osg::Vec3& normal={0,0,1},bool doubles=true,bool attributes=true) {
    auto g=new osg::Geometry;
    if(doubles) {auto a=new osg::Vec3dArray;for(auto p:points) a->push_back(p);g->setVertexArray(a);}
    else {auto a=new osg::Vec3Array;for(auto p:points) a->push_back(osg::Vec3(p));g->setVertexArray(a);}
    auto indices=new osg::DrawElementsUInt(GL_TRIANGLES);for(unsigned i:{2U,0U,1U}) indices->push_back(i);g->addPrimitiveSet(indices);
    if(attributes) {
        auto normals=new osg::Vec3Array;auto uv=new osg::Vec2Array;auto color=new osg::Vec4Array;
        for(unsigned i=0;i<points.size();++i) {normals->push_back(normal);uv->push_back({i?std::nextafter(.125f*float(i),1.0f):-0.0f,1e-40f*float(i+1)});color->push_back({-0.0f,.25f,.5f,float(i+1)/float(points.size())});}
        normals->setBinding(osg::Array::BIND_PER_VERTEX);g->setNormalArray(normals);g->setTexCoordArray(0,uv);color->setBinding(osg::Array::BIND_PER_VERTEX);g->setColorArray(color);
    }
    auto geode=new osg::Geode;geode->addDrawable(g);return geode;
}
const std::array<unsigned,4> order={2,0,1,3};
void unchanged(const Model& m,osg::Geometry* source,bool normals,bool positions=false) {
    const auto& attrs=m.doc["meshes"][0]["primitives"][0]["attributes"];
    std::vector<std::pair<std::string,osg::Array*>> arrays={{"TEXCOORD_0",source->getTexCoordArray(0)},{"COLOR_0",source->getColorArray()}};
    if(normals) arrays.push_back({"NORMAL",source->getNormalArray()});if(positions) arrays.push_back({"POSITION",source->getVertexArray()});
    for(const auto& pair:arrays) {
        const auto* array=pair.second;auto data=elements(m,attrs[pair.first]);auto width=array->getDataSize()*sizeof(float);const auto* bytes=static_cast<const unsigned char*>(array->getDataPointer());
        auto count=source->getVertexArray()->getNumElements();require(data.size()==count*width,pair.first+" byte length");
        for(unsigned i=0;i<count;++i) {auto old=i<3?order[i]:i;require(std::memcmp(data.data()+i*width,bytes+old*width,width)==0,pair.first+" bit pattern, including signed zero/subnormal");}
    }
}
struct Output {Json report;Model glb;std::vector<double> placement;Decoded decoded;};
Output convertFixture(const fs::path& input,const fs::path& output,const std::string& srs,const std::string& origin,bool compressed=false) {
    Options o;o.input=input;o.output=output;o.srs=srs;o.origin=origin;o.compressGeometry=compressed;o.threads=1;auto report=convert(o);
    auto placement=jsonFile(output/"tileset.json")["root"]["transform"].get<std::vector<double>>();
    for(const auto& row:report["records"]) if(!row["content"].empty()) {auto m=model(output/row["content"][0]["uri"].get<std::string>());auto decoded=decode(m,placement);return {report,std::move(m),placement,std::move(decoded)};}
    throw std::runtime_error("missing content");
}
void verifyWorld(const Output& out,const std::vector<osg::Vec3d>& expected,double numerical=1e-8) {
    require(out.decoded.world.size()==expected.size(),"world vertex count");
    for(unsigned i=0;i<expected.size();++i) {
        auto old=i<3?order[i]:i;auto error=(out.decoded.world[i]-expected[old]).length();largestWorldError=std::max(largestWorldError,error);
        close(out.decoded.world[i],expected[old],out.decoded.floatBound+numerical,"independent world coordinates / up axis once");
    }
    double reported=out.report["coordinateRoundingErrorMeters"];
    require(std::isfinite(reported)&&reported>=0&&reported<=out.decoded.floatBound+numerical,"reported rounding <= independently derived FLOAT half-ULP bound");
    largestFloatBound=std::max(largestFloatBound,out.decoded.floatBound);largestReportedRounding=std::max(largestReportedRounding,reported);
}
}
int main() {
    fs::path temp=fs::current_path()/("spatial-data-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(temp);unsigned passed=0,failed=0;
    auto test=[&](const std::string& name,auto body) {try {body();++passed;std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}};
    const std::vector<osg::Vec3d> small={{.125,.25,.5},{1.25,-2.125,3.5},{-4.5,6.25,-.75},{.03125,-.0625,.125}};
    test("fixed PROJ 9.6.2",[&]{require(std::string(proj_info().version)=="9.6.2","tested PROJ version");});
    struct Site {double lat,lon,h;osg::Vec3d origin;const char* srs;const char* csv;};
    const Site sites[]={{0,0,0,{0,0,0},"ENU:0,0","0,0,0"},{32.427248,120.670618,-50,{17.5,-23.75,-30.665},"ENU:32.427248,120.670618,-50","17.5,-23.75,-30.665"},
        {-33.86,151.21,-125,{-12.5,8.75,23.125},"ENU:-33.86,151.21,-125","-12.5,8.75,23.125"},{89.999,-175,120,{4,-8,-150},"ENU:89.999,-175,120","4,-8,-150"}};
    for(unsigned i=0;i<4;++i) test("ENU independent ECEF/site "+std::to_string(i),[&,i]{
        auto node=geometry(small,{-0.0f,.6f,.8f},false);auto input=temp/("enu-"+std::to_string(i)+".osgb");writeNode(node,input);
        auto out=convertFixture(input,temp/("enu-out-"+std::to_string(i)),sites[i].srs,sites[i].csv);auto basis=frame(sites[i].lat,sites[i].lon,sites[i].h);
        std::vector<osg::Vec3d> expected;for(auto p:small) expected.push_back(inFrame(basis,p+sites[i].origin));verifyWorld(out,expected);unchanged(out.glb,node->getDrawable(0)->asGeometry(),true,true);
        close({out.placement[12],out.placement[13],out.placement[14]},inFrame(basis,sites[i].origin),2e-9,"ENU xyz origin and negative height");
    });
    const char* malformed[]={"ENU:0,0junk","ENU:0,0,","ENU:0,0,1m","ENU:0,,0","ENU:91,0","ENU:0,181","ENU:nan,0","ENU:0,0,inf"};
    for(auto srs:malformed) test(std::string("reject malformed ")+srs,[&,srs]{bool rejected=false;try {SpatialReference ref(srs,{0,0,0});}catch(...){rejected=true;}require(rejected,"malformed ENU accepted");});
    test("UTM meters/height/axis with independent inverse series",[&]{
        auto node=geometry(small);writeNode(node,temp/"utm.osgb");auto out=convertFixture(temp/"utm.osgb",temp/"utm-out","EPSG:32651","500000,0,-70");
        std::vector<osg::Vec3d> expected;for(auto p:small) expected.push_back(utmWorld(p+osg::Vec3d(500000,0,-70)));verifyWorld(out,expected);unchanged(out.glb,node->getDrawable(0)->asGeometry(),false);
    });
    test("projected official N,E axes normalized to E,N",[&]{
        auto ctx=proj_context_create();auto crs=proj_create(ctx,"+proj=utm +zone=51 +datum=WGS84 +units=m +axis=neu +type=crs");auto cs=proj_crs_get_coordinate_system(ctx,crs);
        const char* direction=nullptr;bool northFirst=proj_cs_get_axis_info(ctx,cs,0,nullptr,nullptr,&direction,nullptr,nullptr,nullptr,nullptr)&&std::string(direction)=="north";
        proj_destroy(cs);proj_destroy(crs);proj_context_destroy(ctx);require(northFirst,"fixture must actually declare official northing first");
        auto node=geometry(small);writeNode(node,temp/"ne.osgb");auto out=convertFixture(temp/"ne.osgb",temp/"ne-out","+proj=utm +zone=51 +datum=WGS84 +units=m +axis=neu +type=crs","500000,0,-70");
        std::vector<osg::Vec3d> expected;for(auto p:small) expected.push_back(utmWorld(p+osg::Vec3d(500000,0,-70)));verifyWorld(out,expected);
    });
    test("non-metre projected axes retain units, new ellipsoid z is metres",[&]{
        auto node=geometry(small);writeNode(node,temp/"feet.osgb");auto out=convertFixture(temp/"feet.osgb",temp/"feet-out","+proj=utm +zone=51 +datum=WGS84 +units=ft +type=crs","1640419.9475065616,0,-70");
        std::vector<osg::Vec3d> expected;for(auto p:small) expected.push_back(utmWorld({(p.x()+1640419.9475065616)*.3048,p.y()*.3048,p.z()-70}));verifyWorld(out,expected);
    });
    test("large Vec3d Web Mercator with independent analytic coordinates",[&]{
        double lon=172.25,lat=-41.5;osg::Vec3d origin(a*lon*rad,a*std::log(std::tan(pi/4+lat*rad/2)),-120.125);std::vector<osg::Vec3d> points;for(auto p:small) points.push_back(origin+p);
        auto node=geometry(points);writeNode(node,temp/"mercator.osgb");auto out=convertFixture(temp/"mercator.osgb",temp/"mercator-out","EPSG:3857","0,0,0");
        std::vector<osg::Vec3d> expected;for(auto p:points) expected.push_back(frame((2*std::atan(std::exp(p.y()/a))-pi/2)/rad,p.x()/a/rad,p.z()).position);verifyWorld(out,expected);
        require(out.report["coordinateRoundingErrorMeters"].get<double>()<1e-6,"Vec3d rounded before projection/centering");unchanged(out.glb,node->getDrawable(0)->asGeometry(),false);
    });
    test("2D BoundCRS promoted height and same-operation ENU origin",[&]{
        const std::string srs="+proj=longlat +ellps=WGS84 +towgs84=100,200,-300 +type=crs";osg::Vec3d origin(30,40,1000),shift(100,200,-300);
        SpatialReference ref(srs,origin);close(ref.toLocal({0,0,0}),{0,0,0},1e-8,"source origin must map to local zero after 3D datum transform");
        auto expected=frame(40,30,1000).position+shift;auto actual=osg::Vec3d(0,0,0)*ref.placement();close(actual,expected,1e-8,"placement origin uses transformed ellipsoidal height");
        auto points=small;for(auto& p:points) {p.x()*=1e-5;p.y()*=1e-5;}auto node=geometry(points);writeNode(node,temp/"bound.osgb");auto out=convertFixture(temp/"bound.osgb",temp/"bound-out",srs,"30,40,1000");
        std::vector<osg::Vec3d> world;for(auto p:points) world.push_back(frame(p.y()+40,p.x()+30,p.z()+1000).position+shift);verifyWorld(out,world);
    });
    for(double latitude:{-33.0,45.0,89.999}) test("geographic analytic normals lat="+std::to_string(latitude),[&,latitude]{
        auto points=small;for(auto& p:points) {p.x()*=1e-5;p.y()*=1e-5;}osg::Vec3 n(.6f,.8f,0);
        auto node=geometry(points,n);writeNode(node,temp/("geo-"+std::to_string(latitude)+".osgb"));std::string origin="42,"+std::to_string(latitude)+",-80";
        auto out=convertFixture(temp/("geo-"+std::to_string(latitude)+".osgb"),temp/("geo-out-"+std::to_string(latitude)),"EPSG:4326",origin);std::vector<osg::Vec3d> expected;
        for(auto p:points) expected.push_back(frame(p.y()+latitude,p.x()+42,p.z()-80).position);verifyWorld(out,expected);
        for(unsigned i=0;i<order.size();++i) {auto p=points[order[i]];close(out.decoded.normals[i],geographicNormal(p.y()+latitude,p.x()+42,p.z()-80,osg::Vec3d(n)),2e-7,"analytic geographic inverse-transpose normal");}
        unchanged(out.glb,node->getDrawable(0)->asGeometry(),false);
    });
    test("non-uniform matrix before geographic normal transform",[&]{
        auto points=small;for(auto& p:points) {p.x()*=1e-5;p.y()*=1e-5;}osg::Vec3 n(.3f,.4f,.8660254f);auto node=geometry(points,n);
        osg::Matrixd matrix(0,2,0,0,-3,0,0,0,0,0,.5,0,.0002,-.0003,12,1);auto transform=new osg::MatrixTransform(matrix);transform->addChild(node);writeNode(transform,temp/"normal-scale.osgb");
        auto out=convertFixture(temp/"normal-scale.osgb",temp/"normal-scale-out","EPSG:4979","42,45,-80");std::vector<osg::Vec3d> expected;
        for(auto p:points) expected.push_back(frame(45+2*p.x()-.0003,42-3*p.y()+.0002,-68+.5*p.z()).position);verifyWorld(out,expected);
        osg::Vec3d sourceNormal(-double(n.y())/3,double(n.x())/2,double(n.z())/.5);
        for(unsigned i=0;i<order.size();++i) {auto p=points[order[i]];close(out.decoded.normals[i],geographicNormal(45+2*p.x()-.0003,42-3*p.y()+.0002,-68+.5*p.z(),sourceNormal),2e-7,"nonuniform + rotation + projection inverse transpose");}
        unchanged(out.glb,node->getDrawable(0)->asGeometry(),false);
    });
    test("ENU non-uniform node normal and one up-axis conversion",[&]{
        osg::Vec3 n(.3f,.4f,.8660254f);auto node=geometry(small,n,false);
        osg::Matrixd matrix(0,2,0,0,-3,0,0,0,0,0,.5,0,40,-30,7,1);auto transform=new osg::MatrixTransform(matrix);transform->addChild(node);writeNode(transform,temp/"enu-scale.osgb");
        auto out=convertFixture(temp/"enu-scale.osgb",temp/"enu-scale-out","ENU:-33.86,151.21,-125","-12.5,8.75,23.125");auto basis=frame(-33.86,151.21,-125);std::vector<osg::Vec3d> expected;
        for(auto p:small) expected.push_back(inFrame(basis,{-3*p.y()+27.5,2*p.x()-21.25,.5*p.z()+30.125}));verifyWorld(out,expected);
        auto expectedNormal=basis.east*(-double(n.y())/3)+basis.north*(double(n.x())/2)+basis.up*(double(n.z())/.5);expectedNormal.normalize();
        for(auto value:out.decoded.normals) close(value,expectedNormal,1e-12,"client inverse transpose after nonuniform ENU matrix");
        unchanged(out.glb,node->getDrawable(0)->asGeometry(),true,true);
    });
    test("nested cross-file matrices precede projection",[&]{
        auto external=[&](const char* target){auto ref=new osg::PagedLOD;ref->setFileName(0,target);ref->setRange(0,0,1e9);return ref;};
        auto leaf=geometry(small);auto leafMatrix=new osg::MatrixTransform(osg::Matrixd::scale(.5,1.5,2)*osg::Matrixd::translate(3,-4,5));leafMatrix->addChild(leaf);writeNode(leafMatrix,temp/"nested/leaf.osgb");
        auto middle=new osg::MatrixTransform(osg::Matrixd(1,.25,0,0,0,2,0,0,0,0,.5,0,-10,12,-2,1));middle->addChild(external("leaf.osgb"));writeNode(middle,temp/"nested/middle.osgb");
        auto root=new osg::MatrixTransform(osg::Matrixd(0,2,0,0,-3,0,0,0,0,0,4,0,40,-30,7,1));root->addChild(external("middle.osgb"));writeNode(root,temp/"nested/root.osgb");
        auto out=convertFixture(temp/"nested/root.osgb",temp/"nested-out","EPSG:32651","500000,0,-70");require(out.report["files"]==3,"nested file count");std::vector<osg::Vec3d> expected;
        for(auto p:small) {osg::Vec3d q(.5*p.x()+3,1.5*p.y()-4,2*p.z()+5),r(q.x()-10,.25*q.x()+2*q.y()+12,.5*q.z()-2);expected.push_back(utmWorld({500040-3*r.y(),2*r.x()-30,4*r.z()-63}));}verifyWorld(out,expected);
    });
    test("large Vec3d ENU cancellation and independent rounding bounds",[&]{
        std::vector<osg::Vec3d> points;for(auto p:small) points.push_back(osg::Vec3d(1e12,-1e12,1e9)+p);auto node=geometry(points);
        auto transform=new osg::MatrixTransform(osg::Matrixd::translate(-1e12,1e12,-1e9));transform->addChild(node);writeNode(transform,temp/"large.osgb");
        auto out=convertFixture(temp/"large.osgb",temp/"large-out","ENU:-33.86,151.21,-125","-12.5,8.75,23.125");auto basis=frame(-33.86,151.21,-125);std::vector<osg::Vec3d> expected;
        for(auto p:small) expected.push_back(inFrame(basis,p+osg::Vec3d(-12.5,8.75,23.125)));verifyWorld(out,expected);unchanged(out.glb,node->getDrawable(0)->asGeometry(),true);
    });
    test("ECEF Vec3d millimetres survive local centering",[&]{
        auto base=frame(-33.86,151.21,-125).position;std::vector<osg::Vec3d> points;for(auto p:small) points.push_back(base+p*.001);
        auto node=geometry(points);writeNode(node,temp/"ecef.osgb");auto out=convertFixture(temp/"ecef.osgb",temp/"ecef-out","EPSG:4978","0,0,0");verifyWorld(out,points);
        require(out.report["coordinateRoundingErrorMeters"].get<double>()<1e-8,"ECEF must not be stored directly in float");
    });
    test("nonzero FLOAT rounding measured independently",[&]{
        std::vector<osg::Vec3d> points={{.123456789,.876543219,.234567891},{1.987654321,-2.345678912,3.456789123},{-4.567891234,6.789123456,-.891234567},{.03123456789,-.06789123456,.12987654321}};
        auto node=geometry(points);writeNode(node,temp/"round.osgb");auto out=convertFixture(temp/"round.osgb",temp/"round-out","ENU:0,0","0,0,0");std::vector<osg::Vec3d> expected;
        for(auto p:points) expected.push_back(inFrame(frame(0,0,0),p));verifyWorld(out,expected);double measured=0;
        auto nodeMatrix=out.glb.doc["nodes"][0]["matrix"].get<std::vector<double>>();auto accessor=out.glb.doc["meshes"][0]["primitives"][0]["attributes"]["POSITION"].get<unsigned>();auto stored=elements(out.glb,accessor);
        for(unsigned i=0;i<order.size();++i) measured=std::max(measured,(yToZ(pointMatrix(vectorAt(stored,i),nodeMatrix))-points[order[i]]).length());
        require(measured>0&&measured<1e-6,"exercise finite nonzero rounding");require(std::abs(measured-out.report["coordinateRoundingErrorMeters"].get<double>())<1e-15,"rounding report matches independently decoded local error");
    });
    test("3D geographic ellipsoid height feet retain declared units",[&]{
        auto points=small;for(auto& p:points) {p.x()*=1e-5;p.y()*=1e-5;}auto node=geometry(points);writeNode(node,temp/"height-feet.osgb");
        auto out=convertFixture(temp/"height-feet.osgb",temp/"height-feet-out","+proj=longlat +datum=WGS84 +vunits=ft +type=crs","42,45,-200");std::vector<osg::Vec3d> expected;
        for(auto p:points) expected.push_back(frame(45+p.y(),42+p.x(),(p.z()-200)*.3048).position);verifyWorld(out,expected);
    });
    test("raw and meshopt spatial/attribute bit patterns",[&]{
        std::vector<osg::Vec3d> points;for(unsigned i=0;i<600;++i) points.push_back({(int(i%20)-10)*1e-5,(int(i/20)-15)*1e-5,double(i%7)*.125});
        auto node=geometry(points,{.3f,.4f,.8660254f});writeNode(node,temp/"packed.osgb");
        auto raw=convertFixture(temp/"packed.osgb",temp/"packed-raw","EPSG:4979","42,-33,-80");auto packed=convertFixture(temp/"packed.osgb",temp/"packed-out","EPSG:4979","42,-33,-80",true);
        require(packed.report["compressedViews"].get<unsigned>()>0,"exercise actual meshopt compression");
        require(raw.glb.doc["nodes"]==packed.glb.doc["nodes"]&&raw.glb.doc["accessors"]==packed.glb.doc["accessors"],"codec preserves placement/accessors");
        for(unsigned i=0;i<raw.glb.doc["accessors"].size();++i) require(elements(raw.glb,i)==elements(packed.glb,i),"decoded POSITION/NORMAL/UV/COLOR/indices bit patterns");
        unchanged(packed.glb,node->getDrawable(0)->asGeometry(),false);std::vector<osg::Vec3d> expected;
        for(auto p:points) expected.push_back(frame(-33+p.y(),42+p.x(),p.z()-80).position);verifyWorld(raw,expected);verifyWorld(packed,expected);
        require(packed.report["coordinateRoundingErrorMeters"].get<double>()<1e-6,"no measurement tolerance relaxation");
    });
    test("required missing grid cannot silently fall back",[&]{
        auto node=geometry(small);writeNode(node,temp/"missing-grid.osgb");bool rejected=false;
        try {convertFixture(temp/"missing-grid.osgb",temp/"missing-grid-out","+proj=longlat +ellps=WGS84 +nadgrids=required-spatial-test.gsb +type=crs","42,45,-80");}
        catch(const std::exception& e){rejected=std::string(e.what()).find("PROJ")!=std::string::npos;}
        require(rejected&&!fs::exists(temp/"missing-grid-out"),"missing mandatory grid must fail and not publish");
    });
    test("geographic normals at exact poles are singular",[&]{
        SpatialReference ref("EPSG:4979",{42,90,-80});bool rejected=false;
        try {ref.normalToLocal({0,0,0},{0,0,1});}catch(const std::exception& e){rejected=std::string(e.what()).find("极点")!=std::string::npos;}
        require(rejected,"geographic pole Jacobian must fail explicitly");
    });
    test("reject non-finite origin",[&]{
        for(const char* srs:{"ENU:0,0","EPSG:32651"}) {bool rejected=false;try {SpatialReference ref(srs,{0,0,std::numeric_limits<double>::infinity()});}catch(const std::exception& e){rejected=std::string(e.what()).find("SRSOrigin")!=std::string::npos;}require(rejected,"non-finite origin accepted");}
    });
    const std::pair<const char*,const char*> unsupported[]={{"EPSG:4326+5773","不支持垂直"},{"EPSG:5703","不支持垂直"},{"EPSG:7912","动态 CRS"},
        {"+proj=longlat +ellps=clrk66 +type=crs","ballpark"},{"+proj=pipeline +step +proj=unitconvert +xy_in=deg +xy_out=rad","不是可识别的 CRS"},{"EPSG:999999","不是可识别的 CRS"}};
    for(unsigned i=0;i<6;++i) test(std::string("reject unsupported CRS/height ")+unsupported[i].first,[&,i]{
        auto node=geometry(small);auto input=temp/("unsupported-"+std::to_string(i)+".osgb"),output=temp/("unsupported-out-"+std::to_string(i));writeNode(node,input);bool rejected=false;
        try {convertFixture(input,output,unsupported[i].first,"30,40,100");}catch(const std::exception& e){rejected=std::string(e.what()).find(unsupported[i].second)!=std::string::npos;}
        require(rejected&&!fs::exists(output),"unsupported CRS/height must fail with specific cause and no publication");
    });
    for(const char* localSrs:{"LOCAL"," local ","LOCAL_CS[\"arbitrary\"]"}) test(std::string("accept local non-georeferenced SRS ")+localSrs,[&,localSrs]{
        SpatialReference ref(localSrs,{10,-20,5});
        require(!ref.projected(),"LOCAL SRS must not run PROJ projection");
        close(ref.toLocal({1,2,3}),{1,2,3},1e-12,"LOCAL toLocal is identity");
        close(ref.normalToLocal({1,2,3},{0,0,1}),{0,0,1},1e-12,"LOCAL normalToLocal is identity");
        auto node=geometry(small);auto safeName=std::string(localSrs[0]==' '?"l":(std::string(localSrs).size()>6?"w":"u"));
        auto input=temp/("local-"+safeName+".osgb"),output=temp/("local-out-"+safeName);writeNode(node,input);
        auto out=convertFixture(input,output,localSrs,"10,-20,5");
        std::vector<osg::Vec3d> expected;
        for(auto p:small) expected.push_back(inFrame(frame(0,0,0),p+osg::Vec3d(10,-20,5)));
        verifyWorld(out,expected);
    });
    std::cout<<"Max world error="<<largestWorldError<<" m, FLOAT bound="<<largestFloatBound<<" m, reported rounding="<<largestReportedRounding<<" m\n";
    std::cout<<"Spatial regression: "<<passed<<" passed, "<<failed<<" failed; fixtures: "<<temp<<'\n';return failed?1:0;
}
