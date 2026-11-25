#include "orthophoto.hpp"
#include "source_attributes.hpp"
#include <osg/AlphaFunc>
#include <osg/TriangleIndexFunctor>
#include <proj.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <map>
#include <set>

namespace nt {
namespace {
constexpr double resolution = 0.05;
struct Source {
    std::string uri;
    const Json* record;
    osg::BoundingBoxd bounds;
};

using Projection=std::unique_ptr<PJ,decltype(&proj_destroy)>;
Projection fromEcef(const char* target) {
    Projection raw(proj_create_crs_to_crs(nullptr,"EPSG:4978",target,nullptr),proj_destroy);
    Projection normalized(raw ? proj_normalize_for_visualization(nullptr,raw.get()) : nullptr,proj_destroy);
    if (!normalized) throw std::runtime_error("Cannot create orthophoto coordinate operation");
    return normalized;
}

osg::Vec3d project(PJ* operation,const osg::Vec3d& ecef) {
    auto point=proj_trans(operation,PJ_FWD,proj_coord(ecef.x(),ecef.y(),ecef.z(),0));
    if (!std::isfinite(point.xyz.x) || !std::isfinite(point.xyz.y) || !std::isfinite(point.xyz.z))
        throw std::runtime_error("Invalid orthophoto coordinate");
    return {point.xyz.x,point.xyz.y,point.xyz.z};
}

int gridEpsg(const SpatialReference& spatial,const osg::Vec3d& center) {
    if (spatial.localOnly()) return 0;
    auto geographic=fromEcef("EPSG:4979");
    const auto site=project(geographic.get(),center*spatial.placement());
    const double lon=site.x(),lat=site.y();
    if (lat>=84) return 5041; // WGS 84 / UPS North (E,N)
    if (lat < -80) return 5042;
    int zone=std::clamp(int(std::floor((lon+180)/6))+1,1,60);
    if (lat>=56 && lat<64 && lon>=3 && lon<12) zone=32;
    if (lat>=72 && lon>=0 && lon<42) zone=lon<9 ? 31 : lon<21 ? 33 : lon<33 ? 35 : 37;
    return (lat>=0 ? 32600 : 32700)+zone;
}

osg::Vec3d rasterVertex(const GeometryInput& mesh,const osg::Array& array,unsigned i,
                        const SpatialReference& spatial,PJ* projection) {
    auto source=osg::Vec3d(attributeComponent(array,3ULL*i),attributeComponent(array,3ULL*i+1),attributeComponent(array,3ULL*i+2))*mesh.matrix;
    if (!projection) return source;
    return project(projection,spatial.toLocal(source)*spatial.placement());
}

void writeSidecars(const fs::path& output,double west,double north,int epsg) {
    // World files locate the center of the first pixel; GeoTIFF stores its outer edge.
    std::ostringstream world;
    world.imbue(std::locale::classic());
    world << std::setprecision(std::numeric_limits<double>::max_digits10)
          << resolution << "\n0\n0\n" << -resolution << '\n'
          << west+resolution/2 << '\n' << north-resolution/2 << '\n';
    const auto transform=world.str();
    atomicWrite(output/orthoWorldPath,Bytes(transform.begin(),transform.end()));

    std::string wkt=R"(LOCAL_CS["Model-local grid",LOCAL_DATUM["Model datum",10000],UNIT["metre",1],AXIS["Easting",EAST],AXIS["Northing",NORTH]])";
    if (epsg) {
        Projection crs(proj_create(nullptr,("EPSG:"+std::to_string(epsg)).c_str()),proj_destroy);
        const char* definition=crs ? proj_as_wkt(nullptr,crs.get(),PJ_WKT1_GDAL,nullptr) : nullptr;
        if (!definition) throw std::runtime_error("Cannot export the orthophoto projection");
        wkt=definition;
    }
    wkt+='\n';
    atomicWrite(output/orthoPrjPath,Bytes(wkt.begin(),wkt.end()));
}

int dimension(double span) {
    double pixels = std::ceil(span/resolution);
    if (!std::isfinite(pixels) || pixels >= std::numeric_limits<int>::max()) throw std::runtime_error("Orthophoto exceeds the rasterizer dimension limit");
    return std::max(1,static_cast<int>(pixels));
}

struct Strip {
    double west,north;
    int width,first,rows;
    Bytes rgba;
    std::vector<double> depth;
};

osg::Vec4d attribute(const osg::Array* array,unsigned vertex,unsigned primitive,osg::Vec4d value) {
    if (!array) return value;
    const unsigned index = array->getBinding()==osg::Array::BIND_OVERALL ? 0 :
        array->getBinding()==osg::Array::BIND_PER_PRIMITIVE_SET ? primitive : vertex;
    const auto size = array->getDataSize();
    for (int c=0;c<size;++c) value[c]=attributeComponent(*array,std::size_t(index)*size+c);
    return value;
}

struct DrawTriangle {
    Strip* strip;
    const std::vector<osg::Vec3d>* positions;
    const GeometryInput* mesh;
    const osg::Texture* texture;
    unsigned primitive;
    void operator()(unsigned a,unsigned b,unsigned c) const {
        const auto& p=positions->at(a); const auto& q=positions->at(b); const auto& r=positions->at(c);
        double dx=q.x()-p.x(),dy=q.y()-p.y(),ex=r.x()-p.x(),ey=r.y()-p.y();
        double area=dx*ey-dy*ex;
        if (area==0) return;
        auto state=mesh->state.get();
        if (area>0 && state && (state->getMode(GL_CULL_FACE)&osg::StateAttribute::ON)) return;
        int left=std::max(0,int(std::floor(std::min({p.x(),q.x(),r.x()}))));
        int right=std::min(strip->width-1,int(std::ceil(std::max({p.x(),q.x(),r.x()}))));
        int top=std::max(strip->first,int(std::floor(std::min({p.y(),q.y(),r.y()}))));
        int bottom=std::min(strip->first+strip->rows-1,int(std::ceil(std::max({p.y(),q.y(),r.y()}))));
        if (top>bottom || left>right) return;
        auto colors=mesh->geometry->getColorArray(); auto coords=mesh->geometry->getTexCoordArray(0);
        osg::Vec4d white(1,1,1,1);
        auto ca=attribute(colors,a,primitive,white), cb=attribute(colors,b,primitive,white), cc=attribute(colors,c,primitive,white);
        auto ua=attribute(coords,a,primitive,{}), ub=attribute(coords,b,primitive,{}), uc=attribute(coords,c,primitive,{});
        auto diffuse=diffuseColor(state);
        auto alpha=state && (state->getMode(GL_ALPHA_TEST)&osg::StateAttribute::ON) ? dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC)) : nullptr;
        bool blend=state && (state->getMode(GL_BLEND)&osg::StateAttribute::ON);
        for (int y=top;y<=bottom;++y) for (int x=left;x<=right;++x) {
            double rx=x+.5-p.x(),ry=y+.5-p.y();
            double v=(rx*ey-ry*ex)/area,w=(dx*ry-dy*rx)/area,u=1-v-w;
            if (std::min({u,v,w}) < -1e-10) continue;
            auto i=std::size_t(y-strip->first)*strip->width+x;
            double z=u*p.z()+v*q.z()+w*r.z();
            if (z<=strip->depth[i]) continue;
            auto color=ca*u+cb*v+cc*w;
            if (texture) {
                auto uv=ua*u+ub*v+uc*w;
                auto texel=sampleSourceTexture(*texture,uv.x(),uv.y());
                for (int k=0;k<4;++k) color[k]*=texel[k];
            }
            if (diffuse) for (int k=0;k<4;++k) color[k]*=(*diffuse)[k];
            if (alpha && (color.a()<alpha->getReferenceValue() || (alpha->getFunction()==osg::AlphaFunc::GREATER && color.a()==alpha->getReferenceValue()))) continue;
            if (!blend || alpha) color.a()=1;
            if (color.a()<=0) continue;
            strip->depth[i]=z;
            for (int k=0;k<4;++k) strip->rgba[i*4+k]=static_cast<unsigned char>(std::lround(std::clamp(color[k],0.,1.)*255));
        }
    }
};

void render(Strip& strip,const GeometryInput& mesh,const SpatialReference& spatial,PJ* projection) {
    auto array=mesh.geometry->getVertexArray();
    std::vector<osg::Vec3d> positions(array->getNumElements());
    for (unsigned i=0;i<positions.size();++i) {
        auto p=rasterVertex(mesh,*array,i,spatial,projection);
        positions[i].set((p.x()-strip.west)/resolution,(strip.north-p.y())/resolution,p.z());
    }
    osg::TriangleIndexFunctor<DrawTriangle> draw;
    draw.strip=&strip; draw.mesh=&mesh; draw.positions=&positions; draw.texture=activeBaseColorTexture(mesh.state);
    for (unsigned i=0;i<mesh.geometry->getNumPrimitiveSets();++i) {
        draw.primitive=i;
        mesh.geometry->getPrimitiveSet(i)->accept(draw);
    }
}
}

Json buildOrthophoto(const Json& root,const Json& records,const osg::BoundingBoxd& bounds,
                     const SpatialReference& spatial,const Options& options,const OrthoMeshReader& read) {
    const int epsg=gridEpsg(spatial,bounds.center());
    Projection projection(nullptr,proj_destroy);
    if (epsg) projection=fromEcef(("EPSG:"+std::to_string(epsg)).c_str());
    osg::BoundingBoxd extent;
    std::map<std::string,const Json*> ledger;
    for (const auto& row:records) for (const auto& content:row.at("content")) ledger.emplace(content.at("uri").get<std::string>(),&row);
    std::vector<Source> sources;
    std::set<std::string> seen;
    auto visit=[&](auto&& self,const Json& tile)->void {
        if (tile.contains("children") && !tile.at("children").empty()) {
            for (const auto& child:tile.at("children")) self(self,child);
        } else if (tile.contains("content")) {
            auto uri=tile.at("content").at("uri").get<std::string>();
            if (!seen.insert(uri).second) return;
            Source source{uri,ledger.at(uri),{}};
            if (options.checkCancelled) options.checkCancelled();
            for (const auto& mesh:read(*source.record,uri)) {
                auto array=mesh.geometry->getVertexArray();
                for (unsigned i=0;i<array->getNumElements();++i)
                    source.bounds.expandBy(rasterVertex(mesh,*array,i,spatial,projection.get()));
            }
            extent.expandBy(source.bounds);
            sources.push_back(source);
        }
    };
    visit(visit,root);
    if (!extent.valid()) throw std::runtime_error("No vertices in the orthophoto extent");
    const int width=dimension(extent.xMax()-extent.xMin()),height=dimension(extent.yMax()-extent.yMin());
    // Only the current band and its depth buffer are resident.
    auto budget=options.memoryBudgetBytes ? std::min<std::uint64_t>(64ULL<<20,options.memoryBudgetBytes/4) : 64ULL<<20;
    const int band=static_cast<int>(std::clamp<std::uint64_t>(budget/(sizeof(double)+4)/width,1,1024));
    fs::create_directories((options.output/orthoTiffPath).parent_path());
    writeOrthophotoTiff(options.output/orthoTiffPath,width,height,extent.xMin(),extent.yMax(),resolution,epsg,[&](const auto& writeRow) {
        for (int first=0;first<height;) {
            if (options.checkCancelled) options.checkCancelled();
            int rows=std::min(band,height-first);
            Strip strip{extent.xMin(),extent.yMax(),width,first,rows,{},{}};
            std::size_t samples=std::size_t(rows)*width;
            strip.rgba.resize(samples*4); strip.depth.assign(samples,-std::numeric_limits<double>::infinity());
            if (options.event) options.event({{"type","progress"},{"stage","orthophoto"},{"done",first},{"total",height},{"unit","rows"}});
            for (const auto& source:sources) {
                if (source.bounds.yMax()<strip.north-(first+rows)*resolution || source.bounds.yMin()>strip.north-first*resolution) continue;
                if (options.checkCancelled) options.checkCancelled();
                for (const auto& mesh:read(*source.record,source.uri)) render(strip,mesh,spatial,projection.get());
            }
            for (int y=0;y<rows;++y) writeRow(strip.rgba.data()+std::size_t(y)*width*4);
            first+=rows;
        }
    });
    if (options.event) options.event({{"type","progress"},{"stage","orthophoto"},{"done",height},{"total",height},{"unit","rows"}});
    if (options.checkCancelled) options.checkCancelled();
    writeSidecars(options.output,extent.xMin(),extent.yMax(),epsg);
    return {{"uri",orthoTiffPath},{"worldFileUri",orthoWorldPath},{"projectionUri",orthoPrjPath},
            {"width",width},{"height",height},{"metersPerPixel",resolution},
            {"crs",epsg ? "EPSG:"+std::to_string(epsg) : "LOCAL"},{"upperLeftEdge",{extent.xMin(),extent.yMax()}}};
}
}
