#include "orthophoto.hpp"
#include "source_attributes.hpp"
#include <osg/AlphaFunc>
#include <osg/TriangleIndexFunctor>
#include <png.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <set>

namespace nt {
namespace {
constexpr double resolution = 0.05;
struct Source {
    std::string uri;
    const Json* record;
    double south, north;
};

// Keep libpng's longjmp within one call, outside the lifetime of C++ buffers.
class PngWriter {
    std::ofstream stream;
    png_structp png = nullptr;
    png_infop info = nullptr;
public:
    explicit PngWriter(const fs::path& file) : stream(file,std::ios::binary) {
        if (!stream) throw std::runtime_error("无法创建正射影像");
        png = png_create_write_struct(PNG_LIBPNG_VER_STRING,nullptr,nullptr,nullptr);
        if (!png) throw std::bad_alloc();
    }
    ~PngWriter() { png_destroy_write_struct(&png,&info); }
    void begin(int width,int height) {
        info = png_create_info_struct(png);
        if (!info) throw std::bad_alloc();
        if (setjmp(png_jmpbuf(png))) throw std::runtime_error("无法初始化 PNG");
        png_set_write_fn(png,&stream,[](png_structp p,png_bytep bytes,png_size_t size) {
            auto& out = *static_cast<std::ofstream*>(png_get_io_ptr(p));
            if (!out.write(reinterpret_cast<const char*>(bytes),size)) png_error(p,"write failed");
        },nullptr);
        png_set_user_limits(png,PNG_UINT_31_MAX,PNG_UINT_31_MAX);
        png_set_IHDR(png,info,width,height,8,PNG_COLOR_TYPE_RGBA,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);
        png_write_info(png,info);
    }
    void row(unsigned char* bytes) {
        if (setjmp(png_jmpbuf(png))) throw std::runtime_error("正射影像行写入失败");
        png_write_row(png,bytes);
    }
    void finish() {
        if (setjmp(png_jmpbuf(png))) throw std::runtime_error("正射影像写入失败");
        png_write_end(png,info);
        stream.close();
        if (!stream) throw std::runtime_error("正射影像文件写入失败");
    }
};

int dimension(double span) {
    double pixels = std::ceil(span/resolution);
    if (!std::isfinite(pixels) || pixels > PNG_UINT_31_MAX) throw std::runtime_error("全域影像尺寸超过 PNG 上限");
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

void render(Strip& strip,const GeometryInput& mesh,const SpatialReference& spatial) {
    auto array=mesh.geometry->getVertexArray();
    std::vector<osg::Vec3d> positions(array->getNumElements());
    for (std::size_t i=0;i<positions.size();++i) {
        auto p=osg::Vec3d(attributeComponent(*array,3*i),attributeComponent(*array,3*i+1),attributeComponent(*array,3*i+2))*mesh.matrix;
        if (spatial.projected()) p=spatial.toLocal(p);
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
    const int width=dimension(bounds.xMax()-bounds.xMin()),height=dimension(bounds.yMax()-bounds.yMin());
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
            auto box=tile.at("boundingVolume").at("box").get<std::vector<double>>();
            double radius=std::abs(box[4])+std::abs(box[7])+std::abs(box[10]);
            sources.push_back({uri,ledger.at(uri),box[1]-radius,box[1]+radius});
        }
    };
    visit(visit,root);
    // Stream horizontal bands to libpng; even a 20k x 20k image needs only one band in memory.
    auto budget=options.memoryBudgetBytes ? std::min<std::uint64_t>(64ULL<<20,options.memoryBudgetBytes/4) : 64ULL<<20;
    const int band=static_cast<int>(std::clamp<std::uint64_t>(budget/(sizeof(double)+4)/width,1,1024));
    PngWriter png(options.output/"orthophoto.png");
    png.begin(width,height);
    for (int first=0;first<height;) {
        if (options.checkCancelled) options.checkCancelled();
        int rows=std::min(band,height-first);
        Strip strip{bounds.xMin(),bounds.yMax(),width,first,rows};
        std::size_t samples=std::size_t(rows)*width;
        strip.rgba.resize(samples*4); strip.depth.assign(samples,-std::numeric_limits<double>::infinity());
        if (options.event) options.event({{"type","progress"},{"stage","orthophoto"},{"done",first},{"total",height},{"unit","rows"}});
        for (const auto& source:sources) {
            if (source.north<strip.north-(first+rows)*resolution || source.south>strip.north-first*resolution) continue;
            if (options.checkCancelled) options.checkCancelled();
            for (const auto& mesh:read(*source.record,source.uri)) render(strip,mesh,spatial);
        }
        for (int y=0;y<rows;++y) png.row(strip.rgba.data()+std::size_t(y)*width*4);
        first+=rows;
    }
    png.finish();
    if (options.event) options.event({{"type","progress"},{"stage","orthophoto"},{"done",height},{"total",height},{"unit","rows"}});
    if (options.checkCancelled) options.checkCancelled();
    const auto& placement=spatial.placement();
    return {{"uri","orthophoto.png"},{"width",width},{"height",height},{"metersPerPixel",resolution},
            {"upperLeftEdgeENU",{bounds.xMin(),bounds.yMax()}},{"localToEcef",std::vector<double>(placement.ptr(),placement.ptr()+16)}};
}
}
