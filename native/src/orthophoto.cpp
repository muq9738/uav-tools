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
#include <atomic>
#include <future>
#include <cstring>
#include <osg/Texture2D>

namespace nt {
namespace {
constexpr double resolution = 0.05;
using Positions = std::vector<osg::Vec3d>;

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
    int width,first,rows;
    Bytes rgba;
    std::vector<double> depth;
};

struct Face {
    unsigned a,b,c,primitive;
};
struct FaceCollector {
    std::vector<Face>* faces;
    unsigned primitive;
    void operator()(unsigned a,unsigned b,unsigned c) const { faces->push_back({a,b,c,primitive}); }
};

struct Attribute {
    std::vector<osg::Vec4d> values;
    osg::Array::Binding binding=osg::Array::BIND_UNDEFINED;
    const osg::Array* source=nullptr;
    osg::Vec4d get(unsigned vertex,unsigned primitive,osg::Vec4d fallback) const {
        const auto index=binding==osg::Array::BIND_OVERALL ? 0 : binding==osg::Array::BIND_PER_PRIMITIVE_SET ? primitive : vertex;
        if (source) {
            for (int c=0;c<source->getDataSize();++c) fallback[c]=attributeComponent(*source,std::size_t(index)*source->getDataSize()+c);
        }
        return values.empty() ? fallback : values.at(index);
    }
};
Attribute prepareAttribute(const osg::Array* array,const osg::Vec4d& fallback,bool cache) {
    Attribute result;
    if (array) {
        result.binding=array->getBinding();
        if (!cache) { result.source=array; return result; }
        result.values.resize(array->getNumElements(),fallback);
        for (std::size_t i=0;i<result.values.size();++i) for (int c=0;c<array->getDataSize();++c)
            result.values[i][c]=attributeComponent(*array,i*array->getDataSize()+c);
    }
    return result;
}

struct RasterMesh {
    Positions positions;
    const osg::Geometry* sourceGeometry=nullptr;
    std::vector<Face> faces;
    Attribute colors,uv;
    osg::ref_ptr<const osg::Texture> texture;
    osg::Vec4 diffuse;
    bool tinted=false,cull=false,blend=false,alphaTest=false,strictAlpha=false;
    float alphaReference=0;
};
struct Source {
    std::string uri;
    const Json* record;
    osg::BoundingBoxd bounds;
    std::vector<RasterMesh> cached;
};

std::uint64_t faceLimit(const osg::Geometry& geometry) {
    std::uint64_t count=0;
    for (unsigned i=0;i<geometry.getNumPrimitiveSets();++i) count+=geometry.getPrimitiveSet(i)->getNumIndices();
    return count;
}
// Charge the independent raster payload before retaining it. Never pin the source
// scene, which can also own unrelated arrays, textures, or encoded image data.
std::uint64_t cacheCharge(const GeometryInput& mesh) {
    const auto& g=*mesh.geometry;
    auto count=[](const osg::Array* a)->std::uint64_t { return a ? a->getNumElements() : 0; };
    auto bytes=sizeof(RasterMesh)+count(g.getVertexArray())*sizeof(osg::Vec3d)+faceLimit(g)*sizeof(Face)+
        (count(g.getColorArray())+count(g.getTexCoordArray(0)))*sizeof(osg::Vec4d);
    if (auto tex=activeBaseColorTexture(mesh.state)) {
        auto image=tex->getImage(0);
        if (!image || !image->data() || !image->isDataContiguous() || image->isCompressed() || image->r()!=1)
            return std::numeric_limits<std::uint64_t>::max();
        bytes+=sizeof(osg::Texture2D)+sizeof(osg::Image)+std::uint64_t(image->getRowSizeInBytes())*image->t();
    }
    return bytes;
}
RasterMesh prepare(const GeometryInput& mesh,const SpatialReference& spatial,PJ* projection,bool cache) {
    RasterMesh result;
    auto array=mesh.geometry->getVertexArray();
    result.positions.resize(array->getNumElements());
    for (unsigned i=0;i<result.positions.size();++i) result.positions[i]=rasterVertex(mesh,*array,i,spatial,projection);
    result.colors=prepareAttribute(mesh.geometry->getColorArray(),{1,1,1,1},cache);
    result.uv=prepareAttribute(mesh.geometry->getTexCoordArray(0),{},cache);
    if (cache) {
        osg::TriangleIndexFunctor<FaceCollector> collect;
        collect.faces=&result.faces;
        result.faces.reserve(faceLimit(*mesh.geometry));
        for (unsigned i=0;i<mesh.geometry->getNumPrimitiveSets();++i) {
            collect.primitive=i;
            mesh.geometry->getPrimitiveSet(i)->accept(collect);
        }
    } else result.sourceGeometry=mesh.geometry.get();
    const auto* state=mesh.state.get();
    if (auto diffuse=diffuseColor(state)) { result.tinted=true; result.diffuse=*diffuse; }
    result.cull=state && (state->getMode(GL_CULL_FACE)&osg::StateAttribute::ON);
    result.blend=state && (state->getMode(GL_BLEND)&osg::StateAttribute::ON);
    if (state && (state->getMode(GL_ALPHA_TEST)&osg::StateAttribute::ON)) {
        if (auto alpha=dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC))) {
            result.alphaTest=true; result.alphaReference=alpha->getReferenceValue(); result.strictAlpha=alpha->getFunction()==osg::AlphaFunc::GREATER;
        }
    }
    result.texture=activeBaseColorTexture(state);
    if (result.texture && !result.texture->getImage(0)) throw std::runtime_error("Missing orthophoto texture image");
    if (cache && result.texture) {
        const auto& image=*result.texture->getImage(0);
        osg::ref_ptr<osg::Image> copy=new osg::Image;
        copy->allocateImage(image.s(),image.t(),1,image.getPixelFormat(),image.getDataType(),image.getPacking());
        std::memcpy(copy->data(),image.data(),std::size_t(image.getRowSizeInBytes())*image.t());
        osg::ref_ptr<osg::Texture2D> texture=new osg::Texture2D(copy);
        for (auto axis:{osg::Texture::WRAP_S,osg::Texture::WRAP_T}) texture->setWrap(axis,result.texture->getWrap(axis));
        result.texture=texture;
    }
    return result;
}

void toPixels(RasterMesh& mesh,const osg::BoundingBoxd& extent) {
    for (auto& p:mesh.positions) p.set((p.x()-extent.xMin())/resolution,(extent.yMax()-p.y())/resolution,p.z());
}

void draw(Strip& strip,const RasterMesh& mesh,const Face& face,int first,int end) {
    const auto& p=mesh.positions.at(face.a); const auto& q=mesh.positions.at(face.b); const auto& r=mesh.positions.at(face.c);
    double dx=q.x()-p.x(),dy=q.y()-p.y(),ex=r.x()-p.x(),ey=r.y()-p.y();
    double area=dx*ey-dy*ex;
    if (area==0 || (area>0 && mesh.cull)) return;
    int left=std::max(0,int(std::floor(std::min({p.x(),q.x(),r.x()}))));
    int right=std::min(strip.width-1,int(std::ceil(std::max({p.x(),q.x(),r.x()}))));
    int top=std::max(first,int(std::floor(std::min({p.y(),q.y(),r.y()}))));
    int bottom=std::min(end-1,int(std::ceil(std::max({p.y(),q.y(),r.y()}))));
    if (top>bottom || left>right) return;
    osg::Vec4d white(1,1,1,1);
    auto ca=mesh.colors.get(face.a,face.primitive,white), cb=mesh.colors.get(face.b,face.primitive,white), cc=mesh.colors.get(face.c,face.primitive,white);
    auto ua=mesh.uv.get(face.a,face.primitive,{}), ub=mesh.uv.get(face.b,face.primitive,{}), uc=mesh.uv.get(face.c,face.primitive,{});
    for (int y=top;y<=bottom;++y) for (int x=left;x<=right;++x) {
        double rx=x+.5-p.x(),ry=y+.5-p.y();
        double v=(rx*ey-ry*ex)/area,w=(dx*ry-dy*rx)/area,u=1-v-w;
        if (std::min({u,v,w}) < -1e-10) continue;
        auto i=std::size_t(y-strip.first)*strip.width+x;
        double z=u*p.z()+v*q.z()+w*r.z();
        if (z<=strip.depth[i]) continue;
        auto color=ca*u+cb*v+cc*w;
        if (mesh.texture) {
            auto uv=ua*u+ub*v+uc*w;
            auto texel=sampleSourceTexture(*mesh.texture,uv.x(),uv.y());
            for (int k=0;k<4;++k) color[k]*=texel[k];
        }
        if (mesh.tinted) for (int k=0;k<4;++k) color[k]*=mesh.diffuse[k];
        if (mesh.alphaTest && (color.a()<mesh.alphaReference || (mesh.strictAlpha && color.a()==mesh.alphaReference))) continue;
        if (!mesh.blend || mesh.alphaTest) color.a()=1;
        if (color.a()<=0) continue;
        strip.depth[i]=z;
        for (int k=0;k<4;++k) strip.rgba[i*4+k]=static_cast<unsigned char>(std::lround(std::clamp(color[k],0.,1.)*255));
    }
}

struct StreamFaces {
    Strip* strip;
    const RasterMesh* mesh;
    int first,end;
    unsigned primitive;
    void operator()(unsigned a,unsigned b,unsigned c) const { draw(*strip,*mesh,{a,b,c,primitive},first,end); }
};

// Each worker owns disjoint rows; every pixel sees meshes and faces in source order.
void render(Strip& strip,const std::vector<const RasterMesh*>& meshes,const Options& options) {
    if (meshes.empty()) return;
    const auto samples=std::uint64_t(strip.width)*strip.rows;
    const int workers=std::min<std::uint64_t>({options.threads,std::uint64_t(strip.rows),1+samples/(256*1024)});
    std::atomic<bool> stop{false};
    auto run=[&](int worker) {
        int first=strip.first+std::int64_t(strip.rows)*worker/workers;
        int end=strip.first+std::int64_t(strip.rows)*(worker+1)/workers;
        for (const auto* mesh:meshes) {
            if (stop.load(std::memory_order_relaxed)) return;
            if (worker==0 && options.checkCancelled) options.checkCancelled();
            if (mesh->sourceGeometry) {
                osg::TriangleIndexFunctor<StreamFaces> stream;
                stream.strip=&strip; stream.mesh=mesh; stream.first=first; stream.end=end;
                for (unsigned i=0;i<mesh->sourceGeometry->getNumPrimitiveSets();++i) {
                    stream.primitive=i;
                    mesh->sourceGeometry->getPrimitiveSet(i)->accept(stream);
                }
            } else for (const auto& face:mesh->faces) draw(strip,*mesh,face,first,end);
        }
    };
    std::vector<std::future<void>> jobs;
    for (int i=1;i<workers;++i) jobs.push_back(std::async(std::launch::async,run,i));
    try {
        run(0);
        for (auto& job:jobs) job.get();
    } catch (...) {
        stop.store(true,std::memory_order_relaxed);
        throw;
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
    const auto budget=options.memoryBudgetBytes ? std::min<std::uint64_t>(64ULL<<20,options.memoryBudgetBytes/4) : 64ULL<<20;
    auto cacheLeft=budget/2;
    std::set<std::string> seen;
    auto visit=[&](auto&& self,const Json& tile)->void {
        if (tile.contains("children") && !tile.at("children").empty()) {
            for (const auto& child:tile.at("children")) self(self,child);
        } else if (tile.contains("content")) {
            auto uri=tile.at("content").at("uri").get<std::string>();
            if (!seen.insert(uri).second) return;
            Source source{uri,ledger.at(uri),{},{}};
            if (options.checkCancelled) options.checkCancelled();
            auto meshes=read(*source.record,uri);
            auto remaining=cacheLeft;
            const bool fits=std::all_of(meshes.begin(),meshes.end(),[&](const GeometryInput& mesh) {
                auto charge=cacheCharge(mesh);
                if (charge>remaining) return false;
                remaining-=charge; return true;
            });
            if (fits) { cacheLeft=remaining; source.cached.reserve(meshes.size()); }
            for (const auto& mesh:meshes) {
                if (fits) {
                    source.cached.push_back(prepare(mesh,spatial,projection.get(),true));
                    for (const auto& p:source.cached.back().positions) source.bounds.expandBy(p);
                } else {
                    auto array=mesh.geometry->getVertexArray();
                    for (unsigned i=0;i<array->getNumElements();++i)
                        source.bounds.expandBy(rasterVertex(mesh,*array,i,spatial,projection.get()));
                }
            }
            extent.expandBy(source.bounds);
            sources.push_back(std::move(source));
        }
    };
    visit(visit,root);
    if (!extent.valid()) throw std::runtime_error("No vertices in the orthophoto extent");
    const int width=dimension(extent.xMax()-extent.xMin()),height=dimension(extent.yMax()-extent.yMin());
    for (auto& source:sources) for (auto& mesh:source.cached) toPixels(mesh,extent);
    const auto stripBudget=budget-(budget/2-cacheLeft);
    const int band=static_cast<int>(std::clamp<std::uint64_t>(stripBudget/(sizeof(double)+4)/width,1,1024));
    fs::create_directories((options.output/orthoTiffPath).parent_path());
    writeOrthophotoTiff(options.output/orthoTiffPath,width,height,extent.xMin(),extent.yMax(),resolution,epsg,[&](const auto& writeRow) {
        for (int first=0;first<height;) {
            if (options.checkCancelled) options.checkCancelled();
            int rows=std::min(band,height-first);
            Strip strip{width,first,rows,{},{}};
            std::size_t samples=std::size_t(rows)*width;
            strip.rgba.resize(samples*4); strip.depth.assign(samples,-std::numeric_limits<double>::infinity());
            if (options.event) options.event({{"type","progress"},{"stage","orthophoto"},{"done",first},{"total",height},{"unit","rows"}});
            std::vector<const RasterMesh*> batch;
            for (const auto& source:sources) {
                if (source.bounds.yMax()<extent.yMax()-(first+rows)*resolution || source.bounds.yMin()>extent.yMax()-first*resolution) continue;
                if (options.checkCancelled) options.checkCancelled();
                if (!source.cached.empty()) {
                    for (const auto& mesh:source.cached) batch.push_back(&mesh);
                } else {
                    render(strip,batch,options);
                    batch.clear();
                    for (const auto& input:read(*source.record,source.uri)) {
                        auto mesh=prepare(input,spatial,projection.get(),false);
                        toPixels(mesh,extent);
                        render(strip,{&mesh},options);
                    }
                }
            }
            render(strip,batch,options);
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
