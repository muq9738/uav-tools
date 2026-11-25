#include "top.hpp"
#include "source_attributes.hpp"
#include <osg/Texture2D>
#include <osg/TexMat>
#include <osg/TexEnv>
#include <osg/AlphaFunc>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

// Independent native implementation from the behavior contract in
// SPEC.md. Independent implementation, no external conversion runtime.
namespace nt {
namespace {
using V=osg::Vec3d;
using UV=osg::Vec2d;
using Color=osg::Vec4d;
constexpr unsigned imageSize=1024;
void check(const Options& options) {
    if(options.checkCancelled) options.checkCancelled();
    // The conversion layer translates its cancellation callback into pause.

}
struct Vertex {V p;Color color;std::vector<unsigned> faces;unsigned version=0;bool alive=true;UV uv;};
inline void addFace(Vertex& v,unsigned id) {if(std::find(v.faces.begin(),v.faces.end(),id)==v.faces.end()) v.faces.push_back(id);}
inline void removeFace(Vertex& v,unsigned id) {
    auto it=std::find(v.faces.begin(),v.faces.end(),id);
    if(it!=v.faces.end()) {*it=v.faces.back();v.faces.pop_back();}
}
struct Face {std::array<unsigned,3> v;unsigned material=0;bool alive=true;};
struct Material {osg::ref_ptr<osg::StateSet> state;osg::ref_ptr<osg::Image> image;osg::ref_ptr<osg::Texture> texture;};
struct SourceFace {std::array<V,3> p;std::array<UV,3> uv;unsigned material=0;};
struct Mesh {std::vector<Vertex> v;std::vector<Face> f;std::vector<Material> materials;std::vector<SourceFace> source;std::size_t inputTriangles=0;bool hasColors=false;};
V position(osg::Array* array,unsigned i) {
    if(auto a=dynamic_cast<osg::Vec3Array*>(array)) return V(a->at(i));
    if(auto a=dynamic_cast<osg::Vec3dArray*>(array)) return a->at(i);
    throw std::runtime_error("Top: POSITION must be Vec3/Vec3d");
}
unsigned attributeIndex(osg::Array* a,unsigned vertex,unsigned primitive) {
    if(!a) return 0;
    unsigned i=a->getBinding()==osg::Array::BIND_OVERALL?0:
        a->getBinding()==osg::Array::BIND_PER_PRIMITIVE_SET?primitive:vertex;
    if(i>=a->getNumElements()) throw std::runtime_error("Top: attribute count mismatch");
    return i;
}
Color colorAt(osg::Array* a,unsigned v,unsigned p) {
    Color color(1,1,1,1);
    if(!a) return color;
    const auto width=a->getDataSize();
    if(width!=3 && width!=4) throw std::runtime_error("Top: color must be RGB or RGBA");
    std::size_t start=std::size_t(attributeIndex(a,v,p))*width;
    for(int c=0;c<width;++c) color[c]=attributeComponent(*a,start+c);
    return color;
}
UV uvAt(osg::Array* a,unsigned v,unsigned p) {
    if(!a) return {};
    if(a->getDataSize()!=2) throw std::runtime_error("Top: UV must have two components");
    std::size_t start=std::size_t(attributeIndex(a,v,p))*2;
    return {attributeComponent(*a,start),attributeComponent(*a,start+1)};
}
Mesh collect(const std::vector<GeometryInput>& inputs,const SpatialReference* spatial,bool preserveUV=false) {
    Mesh m;
    using Key=std::tuple<unsigned,double,double,double,double,double,double,double,double,double>;
    std::map<Key,unsigned> welded;std::set<std::array<unsigned,3>> unique;
    for(const auto& input:inputs) {
        auto g=input.geometry.get();auto a=g->getVertexArray();m.hasColors|=g->getColorArray()!=nullptr;
        if(!a) throw std::runtime_error("Top: missing vertices");
        unsigned mat=m.materials.size();Material material;material.state=input.state;
        if(input.state) {
            material.texture=dynamic_cast<osg::Texture*>(input.state->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
            if(material.texture && (input.state->getTextureMode(0,GL_TEXTURE_2D)&osg::StateAttribute::ON)) {
                material.image=material.texture->getImage(0);
                if(!material.image || !g->getTexCoordArray(0)) throw std::runtime_error("Top: texture/UV missing");
                // CPU sampling is restricted to the same uncompressed image types
                // already accepted by the source-exact writer.
                validateImageForEncoding(*material.image);
            }
        }
        m.materials.push_back(material);
        for(unsigned pi=0;pi<g->getNumPrimitiveSets();++pi) {
            auto ps=g->getPrimitiveSet(pi);std::vector<std::vector<unsigned>> runs;
            if(auto lengths=dynamic_cast<osg::DrawArrayLengths*>(ps)) {
                unsigned first=lengths->getFirst();for(auto n:*lengths) {
                    if(n<0) throw std::runtime_error("Top: invalid DrawArrayLengths");
                    std::vector<unsigned> r(n);std::iota(r.begin(),r.end(),first);first+=n;runs.push_back(std::move(r));
                }
            } else {std::vector<unsigned> r;for(unsigned j=0;j<ps->getNumIndices();++j) r.push_back(ps->index(j));runs.push_back(std::move(r));}
            for(auto& run:runs) {
                std::vector<unsigned> triangles;auto mode=ps->getMode();
                if(mode==GL_TRIANGLES) triangles=run;
                else if(mode==GL_TRIANGLE_STRIP || mode==GL_TRIANGLE_FAN) {
                    for(unsigned j=2;j<run.size();++j) {
                        unsigned x=mode==GL_TRIANGLE_FAN?run[0]:(j%2?run[j-1]:run[j-2]);
                        unsigned y=mode==GL_TRIANGLE_FAN?run[j-1]:(j%2?run[j-2]:run[j-1]);
                        if(x!=y && x!=run[j] && y!=run[j]) triangles.insert(triangles.end(),{x,y,run[j]});
                    }
                } else throw std::runtime_error("Top: only triangle surfaces can be rebuilt");
                if(triangles.size()%3) throw std::runtime_error("Top: incomplete triangle");
                for(unsigned j=0;j<triangles.size();j+=3) {
                    ++m.inputTriangles;
                    Face f;f.material=mat;SourceFace original;original.material=mat;
                    for(unsigned k=0;k<3;++k) {
                        unsigned index=triangles[j+k];if(index>=a->getNumElements()) throw std::runtime_error("Top: index out of range");
                        V p=position(a,index)*input.matrix;if(spatial) p=spatial->toLocal(p);
                        Color c=colorAt(g->getColorArray(),index,pi);
                        original.p[k]=p;original.uv[k]=uvAt(g->getTexCoordArray(0),index,pi);
                        if(input.state) if(auto tm=dynamic_cast<osg::TexMat*>(input.state->getTextureAttribute(0,osg::StateAttribute::TEXMAT))) {
                            auto uv=V(original.uv[k].x(),original.uv[k].y(),0)*tm->getMatrix();original.uv[k]={uv.x(),uv.y()};
                        }
                        Key key={mat,p.x(),p.y(),p.z(),c.r(),c.g(),c.b(),c.a(),preserveUV?original.uv[k].x():0.,preserveUV?original.uv[k].y():0.};
                        auto [it,inserted]=welded.emplace(key,m.v.size());if(inserted) {m.v.push_back({p,c,{}});m.v.back().uv=original.uv[k];}
                        f.v[k]=it->second;
                    }
                    if(f.v[0]==f.v[1] || f.v[1]==f.v[2] || f.v[2]==f.v[0]) continue;
                    auto sorted=f.v;std::sort(sorted.begin(),sorted.end());if(!unique.insert(sorted).second) continue;
                    unsigned id=m.f.size();m.f.push_back(f);m.source.push_back(original);
                    for(auto v:f.v) addFace(m.v[v],id);
                }
            }
        }
    }
    if(m.f.empty()) throw std::runtime_error("Top: empty surface");
    return m;
}
V normal(const Mesh& m,const Face& f) {return (m.v[f.v[1]].p-m.v[f.v[0]].p)^(m.v[f.v[2]].p-m.v[f.v[0]].p);}
struct Quadric {
    std::array<double,10> a{};
    Quadric& operator+=(const Quadric& b) {for(unsigned i=0;i<10;++i) a[i]+=b.a[i];return *this;}
    double value(const V& p) const {double x=p.x(),y=p.y(),z=p.z();return std::max(0.,a[0]*x*x+2*a[1]*x*y+2*a[2]*x*z+2*a[3]*x+a[4]*y*y+2*a[5]*y*z+2*a[6]*y+a[7]*z*z+2*a[8]*z+a[9]);}
};
using Edge=std::pair<unsigned,unsigned>;
Edge edge(unsigned a,unsigned b) {return std::minmax(a,b);}
unsigned shared(const Mesh& m,unsigned a,unsigned b) {unsigned n=0;for(auto f:m.v[a].faces) if(m.f[f].alive && std::find(m.f[f].v.begin(),m.f[f].v.end(),b)!=m.f[f].v.end()) ++n;return n;}
void collectNeighbors(const Mesh& m,unsigned a,std::vector<unsigned>& out) {
    out.clear();
    for(auto f:m.v[a].faces) if(m.f[f].alive) for(auto v:m.f[f].v) if(v!=a) out.push_back(v);
    std::sort(out.begin(),out.end());
    out.erase(std::unique(out.begin(),out.end()),out.end());
}
void collectBoundaryNeighbors(const Mesh& m,unsigned v,std::vector<unsigned>& scratch,std::vector<unsigned>& out) {
    scratch.clear();out.clear();
    for(auto f:m.v[v].faces) if(m.f[f].alive) for(auto u:m.f[f].v) if(u!=v) scratch.push_back(u);
    std::sort(scratch.begin(),scratch.end());
    for(std::size_t i=0;i<scratch.size();) {
        std::size_t j=i+1;while(j<scratch.size() && scratch[j]==scratch[i]) ++j;
        if(j-i==1) out.push_back(scratch[i]);
        i=j;
    }
}
bool boundary(const Mesh& m,unsigned v) {
    std::vector<unsigned> scratch;
    scratch.reserve(m.v[v].faces.size()*2);
    for(auto f:m.v[v].faces) if(m.f[f].alive) for(auto u:m.f[f].v) if(u!=v) scratch.push_back(u);
    std::sort(scratch.begin(),scratch.end());
    for(std::size_t i=0;i<scratch.size();) {
        std::size_t j=i+1;while(j<scratch.size() && scratch[j]==scratch[i]) ++j;
        if(j-i==1) return true;
        i=j;
    }
    return false;
}
struct Candidate {double cost;unsigned a,b,va,vb;V p;bool operator<(const Candidate& x) const {return std::tie(cost,a,b)>std::tie(x.cost,x.a,x.b);}};
double interpolation(const Vertex& a,const Vertex& b,const V& p) {
    double d=(b.p-a.p).length();return d>1e-12?std::clamp((p-a.p).length()/d,0.,1.):0.;
}
Color interpolated(const Vertex& a,const Vertex& b,const V& p) {
    double t=interpolation(a,b,p);return a.color*(1-t)+b.color*t;
}
UV interpolatedUv(const Vertex& a,const Vertex& b,const V& p) {
    double t=interpolation(a,b,p);return a.uv*(1-t)+b.uv*t;
}
Candidate candidate(const Mesh& m,const std::vector<Quadric>& quadrics,unsigned a,unsigned b) {
    Quadric q=quadrics[a];q+=quadrics[b];auto& x=m.v[a];auto& y=m.v[b];
    bool ba=boundary(m,a),bb=boundary(m,b);std::vector<V> choices;choices.reserve(4);
    if(ba!=bb) choices.push_back(ba?x.p:y.p);
    else {
        choices={x.p,y.p,(x.p+y.p)*.5};
        if(!ba) {
            auto& c=q.a;double d=c[0]*(c[4]*c[7]-c[5]*c[5])-c[1]*(c[1]*c[7]-c[2]*c[5])+c[2]*(c[1]*c[5]-c[2]*c[4]);
            if(std::abs(d)>1e-10) choices.insert(choices.begin(),V(
                -((c[4]*c[7]-c[5]*c[5])*c[3]+(c[2]*c[5]-c[1]*c[7])*c[6]+(c[1]*c[5]-c[2]*c[4])*c[8])/d,
                -((c[2]*c[5]-c[1]*c[7])*c[3]+(c[0]*c[7]-c[2]*c[2])*c[6]+(c[1]*c[2]-c[0]*c[5])*c[8])/d,
                -((c[1]*c[5]-c[2]*c[4])*c[3]+(c[1]*c[2]-c[0]*c[5])*c[6]+(c[0]*c[4]-c[1]*c[1])*c[8])/d));
        }
    }
    std::vector<unsigned> bna,bnb,scratch;
    if(ba && bb) {collectBoundaryNeighbors(m,a,scratch,bna);collectBoundaryNeighbors(m,b,scratch,bnb);}
    Candidate best{std::numeric_limits<double>::infinity(),a,b,x.version,y.version,{}};
    for(const auto& p:choices) {
        double value=q.value(p);
        if(ba && bb) {
            // Penalize movement of the open boundary instead of sealing it.
            for(auto n:bna) {
                V before=m.v[n].p-x.p,after=m.v[n].p-p;double length=before.normalize();after.normalize();
                value+=(1-std::clamp(before*after,-1.,1.))*length*100.;
            }
            for(auto n:bnb) {
                V before=m.v[n].p-y.p,after=m.v[n].p-p;double length=before.normalize();after.normalize();
                value+=(1-std::clamp(before*after,-1.,1.))*length*100.;
            }
            value*=10.;
            if(shared(m,a,b)==1 && x.faces.size()==1 && y.faces.size()==1) value=normal(m,m.f[x.faces.front()]).length()*.5*.01;
        }
        auto c=interpolated(x,y,p);for(unsigned k=0;k<4;++k) value+=std::abs(x.color[k]-y.color[k])*101.+std::abs(c[k]-x.color[k])+std::abs(c[k]-y.color[k]);
        // UV 接缝在 collect(preserveUV) 中已拆成拓扑边界；此处再惩罚跨 UV 的坍缩，
        // 避免清纹理预览为了少量三角形把角点坐标拉到另一张图或另一重复周期。
        auto uv=interpolatedUv(x,y,p);value+=(x.uv-y.uv).length()*100.+(uv-x.uv).length()+(uv-y.uv).length();
        if(std::isfinite(value) && value<best.cost) {best.cost=value;best.p=p;}
    }
    return best;
}
bool safeCollapse(const Mesh& m,const Candidate& c,bool relaxTopology=false) {
    unsigned count=shared(m,c.a,c.b);if(count<1 || count>2) {if(!relaxTopology) return false;}
    if(m.v[c.a].faces.size()==1 && m.v[c.b].faces.size()==1) return false; // retain isolated components
    if(!relaxTopology) {
        std::vector<unsigned> na,nb;
        collectNeighbors(m,c.a,na);collectNeighbors(m,c.b,nb);
        unsigned common=0;std::size_t ia=0,ib=0;
        while(ia<na.size() && ib<nb.size()) {
            if(na[ia]==nb[ib]) {++common;++ia;++ib;}
            else if(na[ia]<nb[ib]) ++ia;
            else ++ib;
        }
        if(common!=count) return false; // manifold link condition
        if(boundary(m,c.a) && boundary(m,c.b) && count!=1) return false;
    }
    auto checkFaces=[&](const std::vector<unsigned>& list) {
        for(auto i:list) {
            const auto& f=m.f[i];bool a=false,b=false;std::array<V,3> p;
            for(unsigned k=0;k<3;++k) {a|=f.v[k]==c.a;b|=f.v[k]==c.b;p[k]=f.v[k]==c.a||f.v[k]==c.b?c.p:m.v[f.v[k]].p;}
            if(a && b) continue;
            V before=normal(m,f),after=(p[1]-p[0])^(p[2]-p[0]);
            if(after.length2()<1e-24 || before*after<=0) return false;
        }
        return true;
    };
    return checkFaces(m.v[c.a].faces) && checkFaces(m.v[c.b].faces);
}
// ---- boundary sealing -----------------------------------------------------
// Adjacent surfaces (blocks of one source, or separate inputs) usually leave an
// open gap between their boundary chains. The reference seals chains whose gap
// is within 2 m before running QEM, so its collapse budget is spent on the
// fused surface. Without it our ancestors keep the seams. Chains are sealed
// across materials as well (that is what makes multi-input ancestors fuse), and
// every merge is reported instead of being silent.
struct Chain {std::vector<unsigned> points;unsigned material=0;bool closed=false;};
V maskedMul(const V& v,const V& mask) {return V(v.x()*mask.x(),v.y()*mask.y(),v.z()*mask.z());}
std::vector<Chain> boundaryChains(const Mesh& mesh) {
    std::map<Edge,std::vector<unsigned>> incid;
    for(unsigned i=0;i<mesh.f.size();++i) if(mesh.f[i].alive)
        for(unsigned k=0;k<3;++k) incid[edge(mesh.f[i].v[k],mesh.f[i].v[(k+1)%3])].push_back(i);
    std::map<unsigned,std::vector<unsigned>> adjacent;std::map<Edge,unsigned> material;
    for(const auto& entry:incid) if(entry.second.size()==1) {
        adjacent[entry.first.first].push_back(entry.first.second);
        adjacent[entry.first.second].push_back(entry.first.first);
        material[entry.first]=mesh.f[entry.second.front()].material;
    }
    std::vector<Chain> chains;std::set<Edge> used;
    // Open chains first (endpoints are not degree-2), then whatever remains.
    for(unsigned pass=0;pass<2;++pass) for(const auto& entry:adjacent) {
        if(pass==0 && entry.second.size()==2) continue;
        for(unsigned first:entry.second) {
            Edge seed=edge(entry.first,first);if(used.count(seed)) continue;
            Chain chain;used.insert(seed);chain.points={entry.first,first};
            unsigned previous=entry.first,current=first;
            while(true) {
                auto it=adjacent.find(current);if(it==adjacent.end()) break;
                unsigned next=std::numeric_limits<unsigned>::max();
                for(unsigned candidate:it->second) if(candidate!=previous && !used.count(edge(current,candidate))) {next=candidate;break;}
                if(next==std::numeric_limits<unsigned>::max()) break;
                used.insert(edge(current,next));
                if(next==entry.first) {chain.closed=true;break;}
                chain.points.push_back(next);previous=current;current=next;
            }
            auto found=material.find(seed);chain.material=found==material.end()?0:found->second;
            chains.push_back(std::move(chain));
        }
    }
    return chains;
}
std::map<unsigned,V> boundaryNormals(const Mesh& mesh) {
    std::map<Edge,std::vector<unsigned>> incid;
    for(unsigned i=0;i<mesh.f.size();++i) if(mesh.f[i].alive)
        for(unsigned k=0;k<3;++k) incid[edge(mesh.f[i].v[k],mesh.f[i].v[(k+1)%3])].push_back(i);
    std::map<unsigned,V> normals;std::map<unsigned,double> weight;
    for(const auto& entry:incid) if(entry.second.size()==1) {
        V n=normal(mesh,mesh.f[entry.second.front()]);double length=n.normalize();
        for(unsigned v:{entry.first.first,entry.first.second}) {normals[v]+=n*length;weight[v]+=length;}
    }
    for(auto& entry:normals) {double w=weight[entry.first];if(w>0) entry.second/=w;entry.second.normalize();}
    return normals;
}
bool maskedBoxesWithin(const std::vector<unsigned>& a,const std::vector<unsigned>& b,const Mesh& mesh,const V& mask,double limit) {
    double amin[3],amax[3],bmin[3],bmax[3];
    for(unsigned k=0;k<3;++k) {amin[k]=amax[k]=bmin[k]=bmax[k]=0;}
    auto accumulate=[&](const std::vector<unsigned>& points,double* lo,double* hi) {
        bool first=true;
        for(unsigned v:points) {V p=maskedMul(mesh.v[v].p,mask);
            if(first) {lo[0]=hi[0]=p.x();lo[1]=hi[1]=p.y();lo[2]=hi[2]=p.z();first=false;continue;}
            lo[0]=std::min(lo[0],p.x());hi[0]=std::max(hi[0],p.x());
            lo[1]=std::min(lo[1],p.y());hi[1]=std::max(hi[1],p.y());
            lo[2]=std::min(lo[2],p.z());hi[2]=std::max(hi[2],p.z());}
    };
    accumulate(a,amin,amax);accumulate(b,bmin,bmax);
    double squared=0;
    for(unsigned k=0;k<3;++k) {
        double gap=std::max(0.,std::max(bmin[k]-amax[k],amin[k]-bmax[k]));
        squared+=gap*gap;
    }
    return std::sqrt(squared)<=limit;
}
Json sealBoundaries(Mesh& mesh,double sealDistance) {
    Json audit={{"policy","boundary chains within 2 m (perpendicular <=6 m), welded at midpoint"}};
    if(mesh.f.empty()) return audit;
    V weighted;double totalArea=0;
    for(const auto& f:mesh.f) if(f.alive) {
        double area=normal(mesh,f).length()*.5;if(area<=0) continue;
        V n=normal(mesh,f);n.normalize();weighted+=n*area;totalArea+=area;
    }
    audit["totalArea"]=totalArea;
    if(totalArea<=0) return audit;
    V mask(1,1,1);weighted.normalize();
    if(std::abs(weighted.x())>=std::abs(weighted.y()) && std::abs(weighted.x())>=std::abs(weighted.z())) mask.x()=0;
    else if(std::abs(weighted.y())>=std::abs(weighted.z())) mask.y()=0;
    else mask.z()=0;
    // Large tolerances can bridge genuine gaps between separate facades, not
    // just numerical cracks. Real blocks can contain gaps inside a 2 m radius,
    // so pass --top-seal-distance 0 (or a smaller value) to trade sealing for
    // fidelity there. A zero tolerance disables sealing entirely.
    const double perpendicularLimit=3*sealDistance;
    constexpr std::size_t minChain=5;
    auto chains=boundaryChains(mesh);auto normals=boundaryNormals(mesh);
    // Welding is only safe across a crack in a continuous surface: a 2 m gap
    // between two different facades (or across a void) would bridge geometry
    // that does not exist in the source. Opposite boundary edges must therefore
    // agree on orientation within this tolerance.
    constexpr double normalAgreement=.9;
    audit["chains"]=chains.size();
    std::vector<std::pair<unsigned,unsigned>> welds;std::vector<std::tuple<double,unsigned,unsigned>> pairs;
    double maxMaskedDistance=0,maxPerpendicularDistance=0,maxRankingDistance=0;unsigned considered=0,rejectedShort=0,rejectedMatch=0,mergedPairs=0,crossMaterial=0;
    for(unsigned i=0;i<chains.size();++i) for(unsigned j=i+1;j<chains.size();++j) {
        auto& a=chains[i];auto& b=chains[j];
        if(a.points.size()<minChain || b.points.size()<minChain) {++rejectedShort;continue;}
        if(!maskedBoxesWithin(a.points,b.points,mesh,mask,sealDistance)) continue;
        ++considered;
        std::map<unsigned,double> forward,backward;
        std::vector<std::tuple<double,unsigned,unsigned>> candidate;
        for(unsigned ai=0;ai<a.points.size();++ai) {
            V p=maskedMul(mesh.v[a.points[ai]].p,mask);
            for(unsigned bi=0;bi+1<b.points.size();++bi) {
                V q0=maskedMul(mesh.v[b.points[bi]].p,mask),q1=maskedMul(mesh.v[b.points[bi+1]].p,mask);
                V segment=q1-q0;double length2=segment.length2();
                double t=length2>0?std::clamp(((p-q0)*segment)/length2,0.,1.):0.;
                V nearest=q0+segment*t;
                double masked=(p-nearest).length();if(masked>sealDistance) continue;
                V world0=mesh.v[b.points[bi]].p,world1=mesh.v[b.points[bi+1]].p;
                V onSurface=world0*(1-t)+world1*t;
                double perpendicular=(mesh.v[a.points[ai]].p-onSurface).length();
                if(perpendicular>=perpendicularLimit) continue;
                V na=normals[a.points[ai]],nb=normals[b.points[bi]];
                if(na*na>0 && nb*nb>0 && na*nb<normalAgreement) continue;
                double ranking=(mesh.v[a.points[ai]].p-onSurface).length();
                unsigned keyA=a.points[ai],keyB=bi;
                if(!forward.count(keyA) || forward[keyA]>ranking) forward[keyA]=ranking;
                if(!backward.count(keyB) || backward[keyB]>ranking) backward[keyB]=ranking;
                candidate.emplace_back(ranking,keyA,keyB);
                maxMaskedDistance=std::max(maxMaskedDistance,masked);
                maxPerpendicularDistance=std::max(maxPerpendicularDistance,perpendicular);
            }
        }
        if(forward.size()<2 || backward.size()<2) {++rejectedMatch;continue;}
        std::sort(candidate.begin(),candidate.end());
        if(!candidate.empty() && std::get<0>(candidate.front())==0.) {++rejectedMatch;continue;}
        std::set<unsigned> usedA,usedB;unsigned merged=0;
        for(const auto& entry:candidate) {
            double distance=std::get<0>(entry);unsigned keyA=std::get<1>(entry),segment=std::get<2>(entry);
            unsigned keyB=b.points[segment];
            if(usedA.count(keyA) || usedB.count(keyB)) continue;
            usedA.insert(keyA);usedB.insert(keyB);welds.emplace_back(keyA,keyB);
            maxRankingDistance=std::max(maxRankingDistance,distance);++merged;
        }
        if(merged) {++mergedPairs;if(a.material!=b.material) ++crossMaterial;}
    }
    audit["welds"]=welds.size();audit["mergePairs"]=mergedPairs;audit["pairsInRange"]=considered;
    audit["crossMaterialPairs"]=crossMaterial;
    audit["rejectedShortChain"]=rejectedShort;audit["rejectedInsufficientMatch"]=rejectedMatch;
    audit["sealDistanceMeters"]=sealDistance;
    audit["maxMergeGapMeters"]=maxMaskedDistance;
    audit["maxMergePerpendicularMeters"]=maxPerpendicularDistance;
    audit["maxMergeRankingDistanceMeters"]=maxRankingDistance;
    if(welds.empty()) return audit;
    std::map<unsigned,unsigned> parent;
    std::function<unsigned(unsigned)> rootOf=[&](unsigned v) {
        auto it=parent.find(v);if(it==parent.end()) return v;
        unsigned r=rootOf(it->second);parent[v]=r;return r;
    };
    for(const auto& weld:welds) {
        unsigned ra=rootOf(weld.first),rb=rootOf(weld.second);
        if(ra!=rb) parent[rb]=ra;
    }
    struct Accumulated {V position;Color color;unsigned count=0;};
    std::map<unsigned,Accumulated> accumulated;
    for(unsigned v=0;v<mesh.v.size();++v) if(mesh.v[v].alive) {
        unsigned r=rootOf(v);auto& slot=accumulated[r];
        slot.position+=mesh.v[v].p;slot.color+=mesh.v[v].color;++slot.count;
    }
    Mesh sealed;sealed.materials=mesh.materials;sealed.hasColors=mesh.hasColors;sealed.inputTriangles=mesh.inputTriangles;
    std::map<unsigned,unsigned> assign;std::size_t removed=0;
    for(unsigned i=0;i<mesh.f.size();++i) {
        if(!mesh.f[i].alive) {++removed;continue;}
        std::array<unsigned,3> mapped{};
        for(unsigned k=0;k<3;++k) {
            unsigned r=rootOf(mesh.f[i].v[k]);
            auto it=assign.find(r);
            if(it==assign.end()) {
                auto& slot=accumulated[r];if(!slot.count) throw std::runtime_error("Top: boundary weld lost a vertex");
                unsigned index=sealed.v.size();sealed.v.push_back({slot.position/slot.count,slot.color/slot.count,{},0,true});
                it=assign.emplace(r,index).first;
            }
            mapped[k]=it->second;
        }
        if(mapped[0]==mapped[1] || mapped[1]==mapped[2] || mapped[2]==mapped[0]) {++removed;continue;}
        Face face;face.v=mapped;face.material=mesh.f[i].material;
        sealed.f.push_back(face);sealed.source.push_back(mesh.source[i]);
    }
    if(sealed.f.empty()) throw std::runtime_error("Top: boundary weld removed the whole surface");
    for(unsigned i=0;i<sealed.f.size();++i) for(unsigned v:sealed.f[i].v) addFace(sealed.v[v],i);
    audit["weldedVertices"]=parent.size();audit["removedFaces"]=removed;
    audit["facesBefore"]=mesh.f.size();audit["facesAfter"]=sealed.f.size();
    mesh=std::move(sealed);
    return audit;
}
Json simplify(Mesh& m,std::size_t target,const Options& options,bool relaxTopology=false) {
    std::vector<Quadric> q(m.v.size());
    for(const auto& f:m.f) if(f.alive) {
        V n=normal(m,f);if(n.normalize()==0) continue;double d=-(n*m.v[f.v[0]].p);
        Quadric plane{{n.x()*n.x(),n.x()*n.y(),n.x()*n.z(),n.x()*d,n.y()*n.y(),n.y()*n.z(),n.y()*d,n.z()*n.z(),n.z()*d,d*d}};
        for(auto v:f.v) q[v]+=plane;
    }
    std::priority_queue<Candidate> queue;
    std::vector<Edge> edges;edges.reserve(m.f.size()*3);
    for(const auto& f:m.f) if(f.alive) for(unsigned k=0;k<3;++k) edges.push_back(edge(f.v[k],f.v[(k+1)%3]));
    std::sort(edges.begin(),edges.end());edges.erase(std::unique(edges.begin(),edges.end()),edges.end());
    for(auto e:edges) queue.push(candidate(m,q,e.first,e.second));
    std::size_t live=0;for(const auto& f:m.f) if(f.alive) ++live;
    std::size_t collapses=0,rejected=0;double displacement=0;
    std::vector<unsigned> affected,around,nb,scratch;std::vector<Edge> update;
    while(live>target && !queue.empty()) {
        if(collapses%128==0) check(options);
        auto c=queue.top();queue.pop();auto& a=m.v[c.a];auto& b=m.v[c.b];
        if(!a.alive || !b.alive || a.version!=c.va || b.version!=c.vb || !shared(m,c.a,c.b)) continue;
        if(!safeCollapse(m,c,relaxTopology)) {++rejected;continue;}
        affected=a.faces;affected.insert(affected.end(),b.faces.begin(),b.faces.end());
        std::sort(affected.begin(),affected.end());affected.erase(std::unique(affected.begin(),affected.end()),affected.end());
        collectNeighbors(m,c.a,around);collectNeighbors(m,c.b,nb);
        around.insert(around.end(),nb.begin(),nb.end());around.push_back(c.a);
        std::sort(around.begin(),around.end());around.erase(std::unique(around.begin(),around.end()),around.end());
        around.erase(std::remove(around.begin(),around.end(),c.b),around.end());
        displacement=std::max({displacement,(c.p-a.p).length(),(c.p-b.p).length()});
        a.color=interpolated(a,b,c.p);a.uv=interpolatedUv(a,b,c.p);a.p=c.p;q[c.a]+=q[c.b];b.alive=false;
        for(auto i:affected) {
            auto& f=m.f[i];for(auto v:f.v) removeFace(m.v[v],i);
            for(auto& v:f.v) if(v==c.b) v=c.a;
            if(f.v[0]==f.v[1] || f.v[1]==f.v[2] || f.v[2]==f.v[0]) {f.alive=false;--live;}
            else for(auto v:f.v) addFace(m.v[v],i);
        }
        for(auto v:around) ++m.v[v].version;
        update.clear();
        for(auto v:around) {collectNeighbors(m,v,scratch);for(auto n:scratch) update.push_back(edge(v,n));}
        std::sort(update.begin(),update.end());update.erase(std::unique(update.begin(),update.end()),update.end());
        for(auto e:update) queue.push(candidate(m,q,e.first,e.second));++collapses;
    }
    return {{"inputTriangles",m.source.size()},{"targetTriangles",target},{"outputTriangles",live},
        {"collapses",collapses},{"rejectedEdges",rejected},{"maxCollapseDisplacementMeters",displacement},
        {"budgetReached",live<=target},{"relaxedTopology",relaxTopology},
        {"boundaryPolicy",relaxTopology?"material/UV partitions; relaxed link condition; no-flip":"material partitions; boundary QEM weights 10/100; manifold/no-flip"}};
}
// Small (<=20-face) least-squares conformal charts; two pinned UV vertices.
// Householder-free modified Gram-Schmidt with reorthogonalization avoids a new
// system-wide linear algebra dependency. Rank failure splits into single faces.
bool leastSquares(std::vector<std::vector<double>> a,const std::vector<double>& b,std::vector<double>& x) {
    unsigned rows=a.size(),cols=a[0].size();if(cols>rows) return false;
    std::vector<std::vector<double>> q(cols,std::vector<double>(rows)),r(cols,std::vector<double>(cols));
    for(unsigned j=0;j<cols;++j) {
        for(unsigned i=0;i<rows;++i) q[j][i]=a[i][j];
        for(unsigned pass=0;pass<2;++pass) for(unsigned k=0;k<j;++k) {
            double d=std::inner_product(q[k].begin(),q[k].end(),q[j].begin(),0.);r[k][j]+=d;
            for(unsigned i=0;i<rows;++i) q[j][i]-=d*q[k][i];
        }
        double d=std::sqrt(std::inner_product(q[j].begin(),q[j].end(),q[j].begin(),0.));if(d<1e-12) return false;
        r[j][j]=d;for(auto& v:q[j]) v/=d;
    }
    x.resize(cols);for(unsigned j=0;j<cols;++j) x[j]=std::inner_product(q[j].begin(),q[j].end(),b.begin(),0.);
    for(int j=static_cast<int>(cols)-1;j>=0;--j) {for(unsigned k=j+1;k<cols;++k) x[j]-=r[j][k]*x[k];x[j]/=r[j][j];}
    return true;
}
struct Chart {std::vector<unsigned> faces;std::map<unsigned,UV> uv;double w=0,h=0;unsigned x=0,y=0,pw=0,ph=0;bool rotated=false;};
bool unfold(const Mesh& m,Chart& chart) {
    std::vector<unsigned> vertices;std::map<unsigned,unsigned> id;
    for(auto f:chart.faces) for(auto v:m.f[f].v) if(id.emplace(v,id.size()).second) vertices.push_back(v);
    unsigned pinA=0,pinB=1;double nearest=std::numeric_limits<double>::infinity();
    for(unsigned a=0;a<vertices.size();++a) for(unsigned b=a+1;b<vertices.size();++b) {
        double d=(m.v[vertices[a]].p-m.v[vertices[b]].p).length();if(d>0 && d<nearest) {nearest=d;pinA=a;pinB=b;}
    }
    if(!std::isfinite(nearest)) return false;
    unsigned n=vertices.size()*2;std::vector<int> free(n,-1);unsigned cols=0;
    for(unsigned j=0;j<n;++j) if(j/2!=pinA && j/2!=pinB) free[j]=cols++;
    std::vector<double> pinned(n,0);pinned[pinB*2]=nearest;
    std::vector<std::vector<double>> matrix;std::vector<double> rhs;
    for(auto f:chart.faces) {
        auto tri=m.f[f].v;V p=m.v[tri[0]].p,q=m.v[tri[1]].p,r=m.v[tri[2]].p;
        V u=q-p;double length=u.normalize();if(length<1e-12) return false;
        V d=r-p;double tx=d*u,ty=(d-u*tx).length();if(ty<1e-12) return false;
        // Cauchy-Riemann equations for the triangle's intrinsic coordinates.
        std::array<double,3> real={tx/length-1,-tx/length,1},imag={ty/length,-ty/length,0};
        std::vector<double> row0(cols,0),row1(cols,0);double b0=0,b1=0;
        for(unsigned k=0;k<3;++k) {
            unsigned index=id.at(tri[k]);for(unsigned axis=0;axis<2;++axis) {
                unsigned variable=index*2+axis;double c0=axis?-imag[k]:real[k],c1=axis?real[k]:imag[k];
                if(free[variable]<0) {b0-=c0*pinned[variable];b1-=c1*pinned[variable];}
                else {row0[free[variable]]+=c0;row1[free[variable]]+=c1;}
            }
        }
        matrix.push_back(std::move(row0));matrix.push_back(std::move(row1));rhs.push_back(b0);rhs.push_back(b1);
    }
    std::vector<double> solution;if(cols && !leastSquares(matrix,rhs,solution)) return false;
    for(unsigned j=0;j<vertices.size();++j) {UV uv;for(unsigned k=0;k<2;++k) uv[k]=free[j*2+k]<0?pinned[j*2+k]:solution[free[j*2+k]];chart.uv[vertices[j]]=uv;}
    double area3=0,area2=0;int orientation=0;
    for(auto f:chart.faces) {
        auto v=m.f[f].v;auto a=chart.uv[v[0]],b=chart.uv[v[1]],c=chart.uv[v[2]];
        double signedArea=(b.x()-a.x())*(c.y()-a.y())-(b.y()-a.y())*(c.x()-a.x());
        if(std::abs(signedArea)<1e-14) return false;
        int sign=signedArea>0?1:-1;if(orientation && sign!=orientation) return false;orientation=sign;
        area3+=normal(m,m.f[f]).length()*.5;area2+=std::abs(signedArea)*.5;
    }
    double scale=std::sqrt(area3/area2);double minX=1e300,minY=1e300,maxX=-1e300,maxY=-1e300;
    for(auto& [v,uv]:chart.uv) {uv*=scale;minX=std::min(minX,uv.x());minY=std::min(minY,uv.y());maxX=std::max(maxX,uv.x());maxY=std::max(maxY,uv.y());}
    chart.w=maxX-minX;chart.h=maxY-minY;if(chart.w<=0 || chart.h<=0) return false;
    for(auto& [v,uv]:chart.uv) uv-=UV(minX,minY);
    return true;
}
std::vector<Chart> charts(const Mesh& m,const Options& options) {
    std::map<Edge,std::vector<unsigned>> adjacency;std::set<unsigned> unassigned;
    for(unsigned f=0;f<m.f.size();++f) if(m.f[f].alive) {unassigned.insert(f);for(unsigned k=0;k<3;++k) adjacency[edge(m.f[f].v[k],m.f[f].v[(k+1)%3])].push_back(f);}
    std::vector<Chart> pending,result;std::vector<V> centers(m.f.size());osg::BoundingBoxd centerBox;
    for(auto f:unassigned) {centers[f]=(m.v[m.f[f].v[0]].p+m.v[m.f[f].v[1]].p+m.v[m.f[f].v[2]].p)/3.;centerBox.expandBy(centers[f]);}
    std::vector<unsigned> seeds;std::map<unsigned,double> distances;
    unsigned first=*unassigned.begin();double farthest=-1;
    for(auto f:unassigned) {double d=(centers[f]-centerBox.center()).length2();if(d>farthest) {farthest=d;first=f;}distances[f]=std::numeric_limits<double>::infinity();}
    seeds.push_back(first);
    while(seeds.size()<std::min<std::size_t>(12,unassigned.size())) {
        unsigned best=first;double distance=-1;
        for(auto f:unassigned) {distances[f]=std::min(distances[f],(centers[f]-centers[seeds.back()]).length2());
            if(std::find(seeds.begin(),seeds.end(),f)==seeds.end() && distances[f]>distance) {best=f;distance=distances[f];}}
        seeds.push_back(best);
    }
    // Ensure every disconnected/material component has a seed.
    std::set<unsigned> visited;
    for(auto firstFace:unassigned) if(!visited.count(firstFace)) {
        std::deque<unsigned> queue{firstFace};visited.insert(firstFace);bool seeded=false;
        while(!queue.empty()) {auto f=queue.front();queue.pop_front();seeded|=std::find(seeds.begin(),seeds.end(),f)!=seeds.end();
            for(unsigned k=0;k<3;++k) for(auto other:adjacency[edge(m.f[f].v[k],m.f[f].v[(k+1)%3])]) if(!visited.count(other) && m.f[other].material==m.f[f].material) {visited.insert(other);queue.push_back(other);}}
        if(!seeded) seeds.push_back(firstFace);
    }
    struct Entry {double cost;unsigned face,chart,order;bool operator<(const Entry& e) const {return std::tie(cost,order)>std::tie(e.cost,e.order);}};
    std::priority_queue<Entry> queue;std::vector<V> axes;unsigned serial=0;
    auto newChart=[&](unsigned f) {unsigned index=pending.size();pending.emplace_back();auto n=normal(m,m.f[f]);n.normalize();axes.push_back(n);queue.push({0,f,index,serial++});};
    for(auto seed:seeds) newChart(seed);
    while(!queue.empty()) {
        auto entry=queue.top();queue.pop();if(!unassigned.count(entry.face)) continue;
        if(entry.cost>.4 || pending[entry.chart].faces.size()>=20) {newChart(entry.face);continue;}
        auto f=entry.face;unassigned.erase(f);pending[entry.chart].faces.push_back(f);
        for(unsigned k=0;k<3;++k) for(auto other:adjacency[edge(m.f[f].v[k],m.f[f].v[(k+1)%3])]) if(unassigned.count(other) && m.f[other].material==m.f[f].material) {
            auto n=normal(m,m.f[other]);n.normalize();queue.push({std::pow(axes[entry.chart]*n-1.,2),other,entry.chart,serial++});}
    }
    for(auto& chart:pending) if(!chart.faces.empty()) {
        check(options);
        if(unfold(m,chart)) result.push_back(std::move(chart));
        else for(auto f:chart.faces) {Chart single;single.faces={f};if(!unfold(m,single)) throw std::runtime_error("Top: UV unfolding degenerate surface");result.push_back(std::move(single));}
    }
    return result;
}
std::vector<UV> convexHull(std::vector<UV> points) {
    std::sort(points.begin(),points.end(),[](const UV& a,const UV& b) {return a.x()<b.x() || (a.x()==b.x() && a.y()<b.y());});
    points.erase(std::unique(points.begin(),points.end(),[](const UV& a,const UV& b){return a.x()==b.x() && a.y()==b.y();}),points.end());
    if(points.size()<3) return points;
    auto cross=[](const UV& o,const UV& a,const UV& b) {return (a.x()-o.x())*(b.y()-o.y())-(a.y()-o.y())*(b.x()-o.x());};
    std::vector<UV> hull(points.size()*2);std::size_t k=0;
    for(std::size_t i=0;i<points.size();++i) {while(k>=2 && cross(hull[k-2],hull[k-1],points[i])<=0) --k;hull[k++]=points[i];}
    for(std::size_t i=points.size()-1,lower=k+1;i>0;--i) {while(k>=lower && cross(hull[k-2],hull[k-1],points[i-1])<=0) --k;hull[k++]=points[i-1];}
    hull.resize(k?k-1:0);return hull;
}
// Smallest-area enclosing rectangle orientation (rotating calipers over hull edges).
double minimumAreaRectAngle(const std::vector<UV>& hull) {
    if(hull.size()<3) return 0;
    double best=0,bestArea=std::numeric_limits<double>::infinity();
    for(std::size_t i=0;i<hull.size();++i) {
        const UV& a=hull[i];const UV& b=hull[(i+1)%hull.size()];
        double angle=std::atan2(b.y()-a.y(),b.x()-a.x());
        double c=std::cos(-angle),s=std::sin(-angle);
        double minX=1e300,maxX=-1e300,minY=1e300,maxY=-1e300;
        for(const auto& p:hull) {
            double x=p.x()*c-p.y()*s,y=p.x()*s+p.y()*c;
            minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);
        }
        double area=(maxX-minX)*(maxY-minY);
        if(area<bestArea) {bestArea=area;best=angle;}
    }
    return best;
}
// Islands are pre-rotated to their minimal enclosing rectangle before packing,
// which is what the reference packer does. Texel density is already equalised
// by unfold() (each island's UV area equals its 3D area), so no extra global
// scale is applied here; the atlas scale search below bounds the total.
double pack(std::vector<Chart>& charts,double& appliedScale) {
    for(auto& chart:charts) {
        std::vector<UV> points;UV center{0,0};
        for(const auto& entry:chart.uv) {points.push_back(entry.second);center+=entry.second;}
        if(points.empty()) continue;
        center/=static_cast<double>(points.size());
        auto hull=convexHull(points);
        double angle=minimumAreaRectAngle(hull);
        if(std::abs(angle)>1e-9) {
            double c=std::cos(-angle),s=std::sin(-angle);
            for(auto& entry:chart.uv) {
                UV relative=entry.second-center;
                entry.second=UV(relative.x()*c-relative.y()*s,relative.x()*s+relative.y()*c);
            }
        }
        double minX=1e300,minY=1e300,maxX=-1e300,maxY=-1e300;
        for(auto& entry:chart.uv) {minX=std::min(minX,entry.second.x());minY=std::min(minY,entry.second.y());
            maxX=std::max(maxX,entry.second.x());maxY=std::max(maxY,entry.second.y());}
        for(auto& entry:chart.uv) entry.second-=UV(minX,minY);
        chart.w=std::max(1e-9,maxX-minX);chart.h=std::max(1e-9,maxY-minY);
    }
    // Shelf packing packs best with decreasing height; the reference ranks by
    // long + 0.05 * short for its box packer, which our row layout does not
    // benefit from (measured: 12% fewer baked texels with that order).
    std::sort(charts.begin(),charts.end(),[](const Chart& a,const Chart& b) {return std::tie(a.h,a.w)>std::tie(b.h,b.w);});
    double area=0,extent=0;for(const auto& c:charts) {area+=c.w*c.h;extent=std::max({extent,c.w,c.h});}
    // MaxRects with best-short-side-fit and free 90-degree rotation. The
    // reference reaches ~92% atlas coverage with a true box packer; shelf
    // packing strands the gaps between large islands (measured ~36%).
    struct FreeRect {int x,y,w,h;};
    auto attempt=[&](double scale) {
        for(auto& c:charts) {
            c.pw=std::max(1U,static_cast<unsigned>(std::ceil(c.w*scale)))+4;
            c.ph=std::max(1U,static_cast<unsigned>(std::ceil(c.h*scale)))+4;
            if(c.pw>imageSize || c.ph>imageSize) return false;
        }
        std::vector<FreeRect> free{{0,0,(int)imageSize,(int)imageSize}};
        auto place=[&](unsigned w,unsigned h,unsigned& outX,unsigned& outY,unsigned& outW,unsigned& outH)->bool {
            int best=-1,bestShort=0x7fffffff,bestLong=0x7fffffff;bool bestRot=false;
            for(unsigned i=0;i<free.size();++i) {
                const auto& f=free[i];
                for(unsigned rot=0;rot<2;++rot) {
                    unsigned w0=rot?h:w,h0=rot?w:h;
                    if((int)w0>f.w || (int)h0>f.h) continue;
                    int leftoverW=f.w-(int)w0,leftoverH=f.h-(int)h0;
                    int shortSide=std::min(leftoverW,leftoverH),longSide=std::max(leftoverW,leftoverH);
                    if(shortSide<bestShort || (shortSide==bestShort && longSide<bestLong)) {
                        best=i;bestShort=shortSide;bestLong=longSide;bestRot=rot;
                        outX=f.x;outY=f.y;outW=w0;outH=h0;
                    }
                }
            }
            if(best<0) return false;
            FreeRect used{(int)outX,(int)outY,(int)outW,(int)outH};
            std::vector<FreeRect> next;
            for(const auto& f:free) {
                if(used.x>=f.x+f.w || used.x+used.w<=f.x || used.y>=f.y+f.h || used.y+used.h<=f.y) {next.push_back(f);continue;}
                if(used.x>f.x) next.push_back({f.x,f.y,used.x-f.x,f.h});
                if(used.x+used.w<f.x+f.w) next.push_back({used.x+used.w,f.y,f.x+f.w-(used.x+used.w),f.h});
                if(used.y>f.y) next.push_back({f.x,f.y,f.w,used.y-f.y});
                if(used.y+used.h<f.y+f.h) next.push_back({f.x,used.y+used.h,f.w,f.y+f.h-(used.y+used.h)});
            }
            free.clear();
            for(unsigned i=0;i<next.size();++i) {
                bool contained=false;
                for(unsigned j=0;j<next.size();++j) {
                    if(i==j) continue;
                    const auto& a=next[i];const auto& b=next[j];
                    if(a.x>=b.x && a.y>=b.y && a.x+a.w<=b.x+b.w && a.y+a.h<=b.y+b.h) {contained=true;break;}
                }
                if(!contained && next[i].w>0 && next[i].h>0) free.push_back(next[i]);
            }
            return true;
        };
        for(auto& c:charts) {
            unsigned w=c.pw,h=c.ph,ox=0,oy=0,ow=0,oh=0;
            if(!place(w,h,ox,oy,ow,oh)) return false;
            c.x=ox;c.y=oy;c.rotated=(ow==h && oh==w && w!=h);
            c.pw=ow;c.ph=oh;
        }
        return true;
    };
    double low=0,high=std::min(imageSize/std::sqrt(area),imageSize/extent);
    if(!attempt(0)) throw std::runtime_error("Top: too many UV islands for 1024 atlas");
    for(unsigned i=0;i<40;++i) {double mid=(low+high)*.5;if(attempt(mid)) low=mid;else high=mid;}
    attempt(low);
    appliedScale=low;double used=0;
    for(auto& c:charts) {
        used+=static_cast<double>(c.pw)*c.ph;
        for(auto& entry:c.uv) {
            double u=entry.second.x()*low,v=entry.second.y()*low;
            // When the island was rotated 90 degrees during packing, swap the
            // local axes so the UVs land on the rotated footprint.
            double lu=c.rotated?(c.h*low-v):u, lv=c.rotated?u:v;
            entry.second=UV((c.x+2+lu)/imageSize,(c.y+2+lv)/imageSize);
        }
    }
    return used/(static_cast<double>(imageSize)*imageSize);
}

struct Hit {unsigned face=0;V bary;double distance=std::numeric_limits<double>::infinity();bool valid=false;};
struct Bvh {
    const std::vector<SourceFace>& faces;std::vector<unsigned> order;
    struct Node {osg::BoundingBoxd box;unsigned begin=0,end=0,left=0,right=0;};std::vector<Node> nodes;
    explicit Bvh(const std::vector<SourceFace>& f):faces(f) {order.resize(f.size());std::iota(order.begin(),order.end(),0);build(0,order.size());}
    unsigned build(unsigned begin,unsigned end) {
        unsigned index=nodes.size();nodes.push_back({});Node n;n.begin=begin;n.end=end;
        for(unsigned i=begin;i<end;++i) for(auto p:faces[order[i]].p) n.box.expandBy(p);
        if(end-begin>10) {
            unsigned axis=0;for(unsigned k=1;k<3;++k) if(n.box._max[k]-n.box._min[k]>n.box._max[axis]-n.box._min[axis]) axis=k;
            unsigned mid=(begin+end)/2;std::nth_element(order.begin()+begin,order.begin()+mid,order.begin()+end,[&](unsigned a,unsigned b){return faces[a].p[0][axis]+faces[a].p[1][axis]+faces[a].p[2][axis]<faces[b].p[0][axis]+faces[b].p[1][axis]+faces[b].p[2][axis];});
            n.left=build(begin,mid);n.right=build(mid,end);
        }
        nodes[index]=n;return index;
    }
    bool intersects(const osg::BoundingBoxd& box,const V& p,const V& direction,double best) const {
        double lo=-best,hi=best;
        for(unsigned k=0;k<3;++k) {if(std::abs(direction[k])<1e-15) {if(p[k]<box._min[k]-1e-9 || p[k]>box._max[k]+1e-9) return false;}
            else {double a=(box._min[k]-p[k])/direction[k],b=(box._max[k]-p[k])/direction[k];if(a>b) std::swap(a,b);lo=std::max(lo,a);hi=std::min(hi,b);if(lo>hi) return false;}}
        return true;
    }
    static V closestBarycentric(const SourceFace& f,const V& p) {
        const V a=f.p[0],b=f.p[1],c=f.p[2],ab=b-a,ac=c-a,ap=p-a;
        double d1=ab*ap,d2=ac*ap;if(d1<=0 && d2<=0) return {1,0,0};
        V bp=p-b;double d3=ab*bp,d4=ac*bp;if(d3>=0 && d4<=d3) return {0,1,0};
        double vc=d1*d4-d3*d2;if(vc<=0 && d1>=0 && d3<=0) {double v=d1/(d1-d3);return {1-v,v,0};}
        V cp=p-c;double d5=ab*cp,d6=ac*cp;if(d6>=0 && d5<=d6) return {0,0,1};
        double vb=d5*d2-d1*d6;if(vb<=0 && d2>=0 && d6<=0) {double w=d2/(d2-d6);return {1-w,0,w};}
        double va=d3*d6-d5*d4;if(va<=0 && d4-d3>=0 && d5-d6>=0) {double w=(d4-d3)/((d4-d3)+(d5-d6));return {0,1-w,w};}
        double denominator=va+vb+vc;
        if(std::abs(denominator)<1e-30) {
            V best(1,0,0);double distance=(p-a).length2();
            for(unsigned i=0;i<3;++i) {unsigned j=(i+1)%3;V edge=f.p[j]-f.p[i];double t=edge.length2()>0?std::clamp(((p-f.p[i])*edge)/edge.length2(),0.,1.):0.;
                double d=(p-f.p[i]-edge*t).length2();if(d<distance) {distance=d;best=V();best[i]=1-t;best[j]=t;}}
            return best;
        }
        double v=vb/denominator,w=vc/denominator;return {1-v-w,v,w};
    }
    void closest(unsigned node,const V& p,unsigned material,Hit& hit) const {
        const auto& n=nodes[node];double lower=0;for(unsigned k=0;k<3;++k) {double d=p[k]<n.box._min[k]?n.box._min[k]-p[k]:p[k]>n.box._max[k]?p[k]-n.box._max[k]:0;lower+=d*d;}
        if(lower>hit.distance*hit.distance) return;
        if(n.left) {closest(n.left,p,material,hit);closest(n.right,p,material,hit);return;}
        for(unsigned i=n.begin;i<n.end;++i) {auto id=order[i];const auto& f=faces[id];if(f.material!=material) continue;auto bary=closestBarycentric(f,p);
            V q=f.p[0]*bary[0]+f.p[1]*bary[1]+f.p[2]*bary[2];double distance=(q-p).length();if(distance<hit.distance) hit={id,bary,distance,true};}
    }
    void trace(unsigned node,const V& p,const V& direction,unsigned material,Hit& hit) const {
        auto& n=nodes[node];if(!intersects(n.box,p,direction,hit.distance)) return;
        if(n.left) {trace(n.left,p,direction,material,hit);trace(n.right,p,direction,material,hit);return;}
        for(unsigned i=n.begin;i<n.end;++i) {
            unsigned id=order[i];auto& f=faces[id];if(f.material!=material) continue;
            V e1=f.p[1]-f.p[0],e2=f.p[2]-f.p[0],h=direction^e2;double det=e1*h;if(std::abs(det)<1e-12) continue;
            V s=p-f.p[0];double u=(s*h)/det;if(u< -1e-8 || u>1+1e-8) continue;
            V q=s^e1;double v=(direction*q)/det;if(v< -1e-8 || u+v>1+1e-8) continue;
            double distance=std::abs((e2*q)/det);if(distance<hit.distance) hit={id,V(1-u-v,u,v),distance,true};
        }
    }
};
struct Baked {osg::ref_ptr<osg::Image> image;Json audit;};
Baked bake(const Mesh& m,const std::vector<Chart>& charts,const Options& options) {
    Bvh bvh(m.source);auto image=new osg::Image;image->allocateImage(imageSize,imageSize,1,GL_RGBA,GL_UNSIGNED_BYTE,1);image->setOrigin(osg::Image::BOTTOM_LEFT);
    std::fill_n(image->data(),imageSize*imageSize*4,0);std::vector<int> owner(imageSize*imageSize,-1);std::deque<unsigned> filled;
    std::uint64_t hitCount=0,missCount=0,fallbacks=0;double maxDistance=0,maxSurfaceDistance=0,maxUnboundedRayDistance=0;bool textured=false;
    for(unsigned ci=0;ci<charts.size();++ci) {
        check(options);auto& chart=charts[ci];
        for(auto fi:chart.faces) {
            auto& face=m.f[fi];auto& material=m.materials[face.material];if(!material.image) continue;textured=true;
            std::array<UV,3> uv;for(unsigned k=0;k<3;++k) uv[k]=chart.uv.at(face.v[k]);
            double det=(uv[1].x()-uv[0].x())*(uv[2].y()-uv[0].y())-(uv[1].y()-uv[0].y())*(uv[2].x()-uv[0].x());
            V n=normal(m,face);n.normalize();
            int minx=std::max(0,static_cast<int>(std::floor(std::min({uv[0].x(),uv[1].x(),uv[2].x()})*imageSize)));
            int maxx=std::min(1023,static_cast<int>(std::ceil(std::max({uv[0].x(),uv[1].x(),uv[2].x()})*imageSize)));
            int miny=std::max(0,static_cast<int>(std::floor(std::min({uv[0].y(),uv[1].y(),uv[2].y()})*imageSize)));
            int maxy=std::min(1023,static_cast<int>(std::ceil(std::max({uv[0].y(),uv[1].y(),uv[2].y()})*imageSize)));
            for(int y=miny;y<=maxy;++y) for(int x=minx;x<=maxx;++x) {
                UV d=UV((x+.5)/imageSize,(y+.5)/imageSize)-uv[0];
                double b=(d.x()*(uv[2].y()-uv[0].y())-d.y()*(uv[2].x()-uv[0].x()))/det;
                double c=((uv[1].x()-uv[0].x())*d.y()-(uv[1].y()-uv[0].y())*d.x())/det;
                if(b< -1e-9 || c< -1e-9 || b+c>1+1e-9) continue;
                V p=m.v[face.v[0]].p*(1-b-c)+m.v[face.v[1]].p*b+m.v[face.v[2]].p*c;
                Hit hit;bvh.trace(0,p,n,face.material,hit);Hit closest;bvh.closest(0,p,face.material,closest);
                if(hit.valid) maxUnboundedRayDistance=std::max(maxUnboundedRayDistance,hit.distance);
                if(closest.valid) {
                    maxSurfaceDistance=std::max(maxSurfaceDistance,closest.distance);
                    if(!hit.valid || hit.distance>closest.distance*4+1e-5) {hit=closest;++fallbacks;}
                }
                if(!hit.valid) {++missCount;continue;}
                ++hitCount;maxDistance=std::max(maxDistance,hit.distance);const auto& source=m.source[hit.face];UV sourceUv;
                for(unsigned k=0;k<3;++k) sourceUv+=source.uv[k]*hit.bary[k];Color color=sampleSourceTexture(*material.texture,sourceUv.x(),sourceUv.y());
                auto pixel=image->data(x,y);for(unsigned k=0;k<4;++k) pixel[k]=static_cast<unsigned char>(std::lround(std::clamp(color[k],0.,1.)*255));
                unsigned index=y*imageSize+x;if(owner[index]<0) filled.push_back(index);owner[index]=ci;
            }
        }
    }
    if(textured && !hitCount) throw std::runtime_error("Top: texture bake produced no source hits");
    // Multi-source flood fill pads every empty texel; bounded O(width*height),
    // preserving alpha and avoiding the target's unbounded empty-fill loop.
    while(!filled.empty()) {
        unsigned p=filled.front();filled.pop_front();unsigned x=p%imageSize,y=p/imageSize;
        for(auto delta:std::array<std::pair<int,int>,4>{{{-1,0},{1,0},{0,-1},{0,1}}}) {
            int nx=static_cast<int>(x)+delta.first,ny=static_cast<int>(y)+delta.second;if(nx<0||ny<0||nx>=1024||ny>=1024) continue;
            unsigned next=ny*imageSize+nx;if(owner[next]>=0) continue;owner[next]=owner[p];
            std::copy_n(image->data(x,y),4,image->data(nx,ny));filled.push_back(next);
        }
    }
    return {image,{{"textureSize",imageSize},{"bakedPixels",hitCount},{"unmatchedPixels",missCount},
        {"maxNormalProjectionDistanceMeters",maxDistance},{"maxSampledSurfaceDistanceMeters",maxSurfaceDistance},
        {"maxUnboundedNormalRayDistanceMeters",maxUnboundedRayDistance},{"closestSurfaceFallbackPixels",fallbacks},
        {"textureEncoding","PNG RGBA"},{"uvCharts",charts.size()}}};
}
// Plain primitives keep their material. Atlas primitives can share a draw only
// if replacing their StateSets with the first member's preserves its factors.
using MaterialKey=std::tuple<unsigned,bool,bool,bool,float,osg::TexEnv::Mode,osg::Vec4>;
MaterialKey materialKey(const Material& material,unsigned index) {
    const auto* state=material.state.get();
    auto enabled=[&](GLenum mode) {return state && (state->getMode(mode)&osg::StateAttribute::ON);};
    auto alpha=state?dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC)):nullptr;
    auto env=state?dynamic_cast<const osg::TexEnv*>(state->getTextureAttribute(0,osg::StateAttribute::TEXENV)):nullptr;
    auto factor=diffuseColor(state);
    return {material.image?0:index+1,enabled(GL_CULL_FACE),enabled(GL_BLEND),enabled(GL_ALPHA_TEST),
            alpha?alpha->getReferenceValue():0.f,env?env->getMode():osg::TexEnv::MODULATE,
            factor?*factor:osg::Vec4(1,1,1,1)};
}
std::vector<GeometryInput> outputGeometry(const Mesh& m,const std::vector<Chart>& charts,osg::Image* atlas) {
    std::vector<GeometryInput> result;
    osg::ref_ptr<osg::Texture2D> atlasTexture=new osg::Texture2D(atlas);
    atlasTexture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);atlasTexture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
    atlasTexture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);atlasTexture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
    // Group materials by their render-state signature. All groups that share
    // the atlas and one signature collapse into a single primitive.
    std::map<MaterialKey,std::vector<unsigned>> groups;
    for(unsigned mi=0;mi<m.materials.size();++mi) groups[materialKey(m.materials[mi],mi)].push_back(mi);
    for(const auto& [key,materials]:groups) {
        auto g=new osg::Geometry;auto positions=new osg::Vec3dArray;
        auto uv=new osg::Vec2Array;osg::ref_ptr<osg::Vec4Array> colors=new osg::Vec4Array;auto indices=new osg::DrawElementsUInt(GL_TRIANGLES);
        std::map<std::pair<unsigned,unsigned>,unsigned> remap;
        std::set<unsigned> inGroup(materials.begin(),materials.end());
        for(unsigned ci=0;ci<charts.size();++ci) {
            const auto& c=charts[ci];
            for(auto fi:c.faces) if(inGroup.count(m.f[fi].material)) for(auto vertex:m.f[fi].v) {
                auto [it,inserted]=remap.emplace(std::make_pair(ci,vertex),positions->size());
                if(inserted) {positions->push_back(m.v[vertex].p);uv->push_back(osg::Vec2(c.uv.at(vertex)));colors->push_back(osg::Vec4(m.v[vertex].color));}
                indices->push_back(it->second);
            }
        }
        if(positions->empty()) {osg::ref_ptr<osg::Geometry> cleanup=g;continue;}
        const auto& representative=m.materials[materials.front()];
        g->setVertexArray(positions);if(m.hasColors) g->setColorArray(colors,osg::Array::BIND_PER_VERTEX);g->addPrimitiveSet(indices);
        osg::ref_ptr<osg::StateSet> state=representative.state?new osg::StateSet(*representative.state,osg::CopyOp::SHALLOW_COPY):new osg::StateSet;
        if(representative.image) {
            state->setTextureAttributeAndModes(0,atlasTexture,osg::StateAttribute::ON);state->removeTextureAttribute(0,osg::StateAttribute::TEXMAT);g->setTexCoordArray(0,uv);
        } else {osg::ref_ptr<osg::Vec2Array> cleanup=uv;}
        result.push_back({g,state,osg::Matrixd::identity()});
    }
    return result;
}
osg::BoundingBoxd tileBounds(const Json& tile) {
    auto box=tile.at("boundingVolume").at("box").get<std::vector<double>>();if(box.size()!=12) throw std::runtime_error("Top: invalid box");
    osg::BoundingBoxd b;for(int x:{-1,1}) for(int y:{-1,1}) for(int z:{-1,1}) b.expandBy(V(box[0]+x*box[3]+y*box[6]+z*box[9],box[1]+x*box[4]+y*box[7]+z*box[10],box[2]+x*box[5]+y*box[8]+z*box[11]));return b;
}
struct Item {Json tile;osg::BoundingBoxd bounds;std::vector<GeometryInput> meshes;double error=0;std::size_t triangles=0;};
std::vector<std::vector<unsigned>> partition(const std::vector<Item>& items) {
    unsigned n=items.size();if(n<=4) return {};
    std::vector<std::array<unsigned,2>> labels(n);std::array<unsigned,2> counts{};
    for(unsigned axis=0;axis<2;++axis) {
        std::vector<unsigned> sorted(n);std::iota(sorted.begin(),sorted.end(),0);
        auto center=[&](unsigned i) {return items[i].bounds._min[axis]+items[i].bounds._max[axis];};
        std::stable_sort(sorted.begin(),sorted.end(),[&](unsigned a,unsigned b){return center(a)<center(b);});
        double mean=0,variance=0;for(unsigned i=0;i+1<n;++i) mean+=center(sorted[i])-center(sorted[i+1]);mean/=(n-1);
        for(unsigned i=0;i+1<n;++i) variance+=std::pow(center(sorted[i])-center(sorted[i+1])-mean,2);variance/=(n-1);
        unsigned label=0;bool previousSplit=false;labels[sorted[0]][axis]=0;
        for(unsigned i=1;i<n;++i) {
            bool split=false;double gap=center(sorted[i-1])-center(sorted[i]);
            if(std::pow(gap-mean,2)>=variance*1.5) {
                double next=i+1<n?center(sorted[i+1]):center(sorted[i]);
                if(std::abs(gap)>std::abs(next-center(sorted[i]))) {if(previousSplit) previousSplit=false;else {split=true;++label;}}
            }
            previousSplit=split;labels[sorted[i]][axis]=label;
        }
        counts[axis]=label+1;
    }
    std::vector<std::vector<unsigned>> groups(4);for(unsigned i=0;i<n;++i) groups[(labels[i][0]*2/counts[0])+(labels[i][1]*2/counts[1])*2].push_back(i);
    for(const auto& g:groups) if(g.size()==n) return {};return groups;
}
class Builder {
    const Options& options_;const SpatialReference* spatial_;unsigned serial_=0;
public:
    Json audit=Json::array();Builder(const Options& o,const SpatialReference* s):options_(o),spatial_(s) {}
    Item build(std::vector<Item> inputs) {
        check(options_);if(inputs.size()==1) return std::move(inputs.front());
        auto groups=partition(inputs);
        if(!groups.empty()) {
            std::vector<Item> grouped;for(auto& group:groups) if(!group.empty()) {std::vector<Item> sub;for(auto i:group) sub.push_back(std::move(inputs[i]));grouped.push_back(build(std::move(sub)));}inputs=std::move(grouped);
        }
        std::vector<GeometryInput> meshes;Json children=Json::array();osg::BoundingBoxd bounds;double childError=0;std::size_t sum=0;
        for(auto& input:inputs) {
            children.push_back(input.tile);bounds.expandBy(input.bounds);childError=std::max(childError,input.error);
            meshes.insert(meshes.end(),input.meshes.begin(),input.meshes.end());sum+=input.triangles;
        }
        std::size_t target=(sum+inputs.size()-1)/inputs.size();auto mesh=collect(meshes,nullptr);
        auto seal=sealBoundaries(mesh,options_.topSealDistance);
        // Inputs have already been projected once when creating source Items.
        auto metrics=simplify(mesh,target,options_);metrics["boundarySeal"]=seal;metrics["uniqueInputTriangles"]=metrics["inputTriangles"];metrics["inputTriangles"]=sum;
        auto islands=charts(mesh,options_);double atlasScale=0;double atlasFill=pack(islands,atlasScale);auto baked=bake(mesh,islands,options_);
        bool preserveTopEncoded=false;
        if(options_.embedTextures && options_.textureFormat=="original") {
            if(auto jpeg=encodeOpaqueJpegImage(*baked.image,85)) {
                baked.image->setUserData(jpeg.get());
                baked.audit["textureEncoding"]="JPEG RGB";
                preserveTopEncoded=true;
            }
        }
        auto output=outputGeometry(mesh,islands,baked.image);std::string name="top-"+std::to_string(serial_++)+".glb";
        if(options_.observeTopGeometry) options_.observeTopGeometry(name,output);
        auto content=writeGlb(output,options_.output/"content"/name,options_.output/"textures",preserveTopEncoded,nullptr,options_.compressGeometry,false,&options_);
        bounds.expandBy(content.bounds);double extent=std::max({bounds.xMax()-bounds.xMin(),bounds.yMax()-bounds.yMin(),bounds.zMax()-bounds.zMin()});
        double error=childError>0?childError*2:extent/50.;
        Json tile={{"boundingVolume",boundingBoxJson(bounds)},{"geometricError",error},{"refine","REPLACE"},{"content",{{"uri","content/"+name}}},{"children",children}};
        metrics["atlasFillRatio"]=atlasFill;metrics["atlasScale"]=atlasScale;metrics["content"]="content/"+name;metrics["inputCount"]=inputs.size();metrics["bake"]=baked.audit;metrics["geometricErrorHeuristic"]=error;metrics["lossy"]=true;
        metrics["images"]=content.imageAudit;metrics["draco"]=content.dracoAudit;
        metrics["vertices"]=content.vertices;metrics["bytes"]=content.bytes;
        metrics["geometryRawBytes"]=content.geometryRawBytes;metrics["geometryStoredBytes"]=content.geometryStoredBytes;
        metrics["originalFloatBytes"]=content.originalFloatBytes;metrics["compressedViews"]=content.compressedViews;metrics["coordinateRoundingErrorMeters"]=content.coordinateRoundingError;
        audit.push_back(metrics);
        if(options_.event) options_.event({{"type","progress"},{"stage","rebuild-top"},{"done",audit.size()},{"total",0},{"unit","ancestors"}});
        return {tile,bounds,std::move(output),error,static_cast<std::size_t>(content.triangles)};
    }
    Item source(TopSource input) {
        auto mesh=collect(input.meshes,spatial_);
        // Keep source attributes until simplification. Project copied positions,
        // retaining the source's per-corner UV for the first bake.
        std::vector<GeometryInput> local;
        for(const auto& original:input.meshes) {
            osg::ref_ptr<osg::Geometry> g=new osg::Geometry(*original.geometry,osg::CopyOp::DEEP_COPY_ARRAYS);
            auto positions=new osg::Vec3dArray;
            for(unsigned i=0;i<g->getVertexArray()->getNumElements();++i) {auto p=position(g->getVertexArray(),i)*original.matrix;if(spatial_) p=spatial_->toLocal(p);positions->push_back(p);}
            g->setVertexArray(positions);local.push_back({g,original.state,osg::Matrixd::identity()});
        }
        return {input.tile,tileBounds(input.tile),std::move(local),input.tile.at("geometricError").get<double>(),mesh.inputTriangles};
    }
};
}
TopResult rebuildTop(std::vector<TopSource> sources,const Options& options,const SpatialReference* spatial) {
    if(sources.empty()) throw std::runtime_error("Top: no resident source roots");
    Builder builder(options,spatial);std::vector<Item> items;for(auto& source:sources) items.push_back(builder.source(std::move(source)));
    auto root=builder.build(std::move(items));TopResult result;result.children.push_back(root.tile);result.error=root.error;result.audit=std::move(builder.audit);return result;
}
// 最后兜底：流形与聚类都无法达到预算时（例如无拓扑关联的三角汤），
// 按面积保留较大的三角面。会产生空洞，报告单独警告，不用于常规场景。
Json decimateToBudget(Mesh& mesh,std::size_t target) {
    std::vector<unsigned> order;
    for(unsigned i=0;i<mesh.f.size();++i) if(mesh.f[i].alive) order.push_back(i);
    if(order.size()<=target) return {{"method","none"},{"budgetReached",true}};
    std::vector<double> area(order.size());
    for(std::size_t i=0;i<order.size();++i) area[i]=normal(mesh,mesh.f[order[i]]).length()*.5;
    std::vector<std::size_t> ranking(order.size());std::iota(ranking.begin(),ranking.end(),0);
    std::nth_element(ranking.begin(),ranking.begin()+static_cast<std::ptrdiff_t>(target),ranking.end(),
        [&](std::size_t a,std::size_t b) {return area[a]>area[b];});
    ranking.resize(target);
    std::set<unsigned> keep;double minimum=std::numeric_limits<double>::infinity();
    for(auto index:ranking) {keep.insert(order[index]);minimum=std::min(minimum,area[index]);}
    Mesh reduced;reduced.materials=mesh.materials;reduced.hasColors=mesh.hasColors;reduced.inputTriangles=mesh.inputTriangles;
    for(unsigned i=0;i<mesh.f.size();++i) {
        if(!mesh.f[i].alive) continue;
        if(!keep.count(i)) {++reduced.inputTriangles;continue;}
        reduced.f.push_back(mesh.f[i]);reduced.source.push_back(i<mesh.source.size()?mesh.source[i]:SourceFace());
    }
    // 重建实际被引用的顶点，避免悬空索引。
    std::map<unsigned,unsigned> remap;std::vector<Vertex> vertices;
    for(auto& face:reduced.f) for(auto& index:face.v) {
        auto [it,inserted]=remap.emplace(index,vertices.size());
        if(inserted) vertices.push_back(mesh.v[index]);
        index=it->second;
    }
    reduced.v=std::move(vertices);
    for(unsigned i=0;i<reduced.f.size();++i) for(auto v:reduced.f[i].v) addFace(reduced.v[v],i);
    mesh=std::move(reduced);
    return {{"method","triangle-decimate"},{"outputTriangles",mesh.f.size()},{"budgetReached",mesh.f.size()<=target},
            {"minKeptFaceAreaSquareMeters",minimum},{"warning","非流形几何无法聚簇，按面积裁剪三角面，可能出现空洞"}};
}

std::size_t liveFaces(const Mesh& mesh) {
    return static_cast<std::size_t>(std::count_if(mesh.f.begin(),mesh.f.end(),[](const Face& face){return face.alive;}));
}
// QEM 之后可能残留与已删除面重复的三角面（顶点合并后三元组相同），
// 它们在渲染时会被重复绘制，必须按顶点三元组去重后重新统计有效面数。
void deduplicateFaces(Mesh& mesh) {
    std::set<std::array<unsigned,3>> seen;
    for(auto& face:mesh.f) if(face.alive) {
        auto sorted=face.v;std::sort(sorted.begin(),sorted.end());
        if(!seen.insert(sorted).second) face.alive=false;
    }
}

// QEM 依赖流形邻接；扫描数据里的三角汤或大量孤岛会让所有边被拒绝。
// 此时按世界坐标量化做顶点聚类兜底：同格且材质/UV/颜色接近的顶点合并，退化面剔除，
// 报告实际格子边长作为几何误差上界，避免预览在不可简化网格上失效。
Json clusterToBudget(Mesh& mesh,std::size_t target) {
    if(liveFaces(mesh)<=target) return {{"method","none"},{"budgetReached",true}};
    V low=mesh.v[mesh.f.front().v[0]].p,high=low;
    for(const auto& f:mesh.f) for(auto index:f.v) for(unsigned k=0;k<3;++k) {
        low[k]=std::min(low[k],mesh.v[index].p[k]);high[k]=std::max(high[k],mesh.v[index].p[k]);
    }
    double diagonal=std::max(1e-6,(high-low).length());
    auto build=[&](double cell) {
        struct Accumulated {V position;UV uv;Color color;unsigned count=0;};
        struct ClusterKey {
            long long x,y,z,u,v;unsigned mat,col;
            bool operator==(const ClusterKey& o) const {
                return x==o.x && y==o.y && z==o.z && u==o.u && v==o.v && mat==o.mat && col==o.col;
            }
        };
        struct ClusterHash {
            std::size_t operator()(const ClusterKey& k) const noexcept {
                std::size_t h=static_cast<std::size_t>(k.x)*11400714819323198485ULL;
                auto mix=[&](std::size_t v) {h^=v+0x9e3779b97f4a7c15ULL+(h<<6)+(h>>2);};
                mix(static_cast<std::size_t>(k.y));mix(static_cast<std::size_t>(k.z));
                mix(static_cast<std::size_t>(k.u));mix(static_cast<std::size_t>(k.v));
                mix(k.mat);mix(k.col);return h;
            }
        };
        std::unordered_map<ClusterKey,unsigned,ClusterHash> remap;remap.reserve(mesh.v.size());
        std::vector<Accumulated> accumulated;accumulated.reserve(mesh.v.size());
        std::vector<Face> faces;faces.reserve(mesh.f.size());
        double inverse=1./cell;
        auto quantize=[] (double value,double inverse) {return static_cast<long long>(std::floor(value*inverse));};
        for(const auto& face:mesh.f) if(face.alive) {
            std::array<unsigned,3> mapped{};
            for(unsigned k=0;k<3;++k) {
                const auto& vertex=mesh.v[face.v[k]];
                ClusterKey key{quantize(vertex.p.x(),inverse),quantize(vertex.p.y(),inverse),quantize(vertex.p.z(),inverse),
                               quantize(vertex.uv.x(),32.0),quantize(vertex.uv.y(),32.0),face.material,
                               static_cast<unsigned>(std::llround(std::clamp(vertex.color.x(),0.,1.)*255.))};
                auto [it,inserted]=remap.emplace(key,accumulated.size());
                if(inserted) accumulated.push_back({});
                auto& slot=accumulated[it->second];
                slot.position+=vertex.p;slot.uv+=vertex.uv;slot.color+=vertex.color;++slot.count;
                mapped[k]=it->second;
            }
            if(mapped[0]==mapped[1] || mapped[1]==mapped[2] || mapped[2]==mapped[0]) continue;
            Face output;output.v=mapped;output.material=face.material;faces.push_back(output);
        }
        std::vector<Vertex> vertices;vertices.reserve(accumulated.size());
        for(auto& slot:accumulated) {
            Vertex merged;double count=std::max(1U,slot.count);
            merged.p=slot.position/count;merged.uv=slot.uv/count;merged.color=slot.color/count;
            vertices.push_back(std::move(merged));
        }
        return std::make_pair(std::move(vertices),std::move(faces));
    };
    // 单调性：格子越小面数越多。二分找“面数不超过预算的最小格子”，
    // 这样既保证预算，又尽量保留细节；过粗导致整块消失时不直接放弃。
    double lowCell=diagonal/8192.,highCell=diagonal,chosen=0;
    std::pair<std::vector<Vertex>,std::vector<Face>> best;
    for(unsigned iteration=0;iteration<18;++iteration) {
        double middle=(lowCell+highCell)*.5;auto candidate=build(middle);
        if(candidate.second.empty()) {highCell=middle;continue;}
        if(candidate.second.size()>target) {lowCell=middle;continue;}
        best=std::move(candidate);chosen=middle;highCell=middle;
        if(best.second.size()==target) break;
    }
    if(best.second.empty()) return {{"method","none"},{"budgetReached",false}};
    for(unsigned i=0;i<best.second.size();++i) for(auto v:best.second[i].v) addFace(best.first[v],i);
    mesh.v=std::move(best.first);mesh.f=std::move(best.second);
    return {{"method","vertex-cluster"},{"cellSizeMeters",chosen},{"outputTriangles",liveFaces(mesh)},
            {"budgetReached",liveFaces(mesh)<=target},{"diagonalMeters",diagonal},{"targetTriangles",target}};
}

PreviewMesh simplifyPreview(const std::vector<GeometryInput>& inputs,std::size_t target,
                            const Options& options,const SpatialReference* spatial) {
    auto mesh=collect(inputs,spatial,true);
    PreviewMesh result;result.inputTriangles=mesh.inputTriangles;result.hasColors=mesh.hasColors;
    if(liveFaces(mesh)<=target) result.simplification={
        {"inputTriangles",liveFaces(mesh)},{"targetTriangles",target},{"outputTriangles",liveFaces(mesh)},
        {"collapses",0},{"rejectedEdges",0},{"maxCollapseDisplacementMeters",0.},{"budgetReached",true},
        {"relaxedTopology",false},{"method","none"},
        {"boundaryPolicy","material/UV partitions; source already within budget"}};
    else {
        // 先做严格流形 QEM；扫描网格常见的非流形三角汤会拒绝全部边，
        // 此时放宽连接条件再跑一次（仍禁止翻面与零面积），保证预览预算可达。
        result.simplification=simplify(mesh,target,options);
        result.simplification["method"]="qem";
        deduplicateFaces(mesh);
        if(liveFaces(mesh)>target) {
            auto relaxed=simplify(mesh,target,options,true);
            relaxed["collapses"]=relaxed.value("collapses",0ULL)+result.simplification.value("collapses",0ULL);
            relaxed["rejectedEdges"]=relaxed.value("rejectedEdges",0ULL)+result.simplification.value("rejectedEdges",0ULL);
            relaxed["maxCollapseDisplacementMeters"]=std::max(relaxed.value("maxCollapseDisplacementMeters",0.0),result.simplification.value("maxCollapseDisplacementMeters",0.0));
            relaxed["method"]="qem-relaxed";
            result.simplification=std::move(relaxed);
        }
        if(liveFaces(mesh)>target) {
            auto clustered=clusterToBudget(mesh,target);
            deduplicateFaces(mesh);
            clustered["qemCollapses"]=result.simplification.value("collapses",0ULL);
            clustered["qemRejectedEdges"]=result.simplification.value("rejectedEdges",0ULL);
            clustered["inputTriangles"]=result.simplification.value("inputTriangles",0ULL);
            clustered["maxCollapseDisplacementMeters"]=std::max(result.simplification.value("maxCollapseDisplacementMeters",0.0),
                clustered.value("cellSizeMeters",0.0)*1.75);
            result.simplification=std::move(clustered);
        }
        if(liveFaces(mesh)>target) {
            auto decimated=decimateToBudget(mesh,target);
            decimated["previousMethod"]=result.simplification.value("method","none");
            result.simplification=std::move(decimated);
        }
    }
    for(const auto& material:mesh.materials) result.materials.push_back({material.state,material.image,material.texture});
    result.faces.reserve(mesh.f.size());
    for(const auto& face:mesh.f) if(face.alive) {
        PreviewFace output;output.material=face.material;
        for(unsigned k=0;k<3;++k) {
            const auto& vertex=mesh.v[face.v[k]];
            output.positions[k]=vertex.p;output.uv[k]=vertex.uv;output.colors[k]=vertex.color;
        }
        result.faces.push_back(output);
    }
    result.simplification["outputTriangles"]=result.faces.size();
    result.simplification["uniqueInputTriangles"]=liveFaces(mesh);
    return result;
}
}
