#include "preview.hpp"
#include "product_metadata.hpp"
#include <osg/Geode>
#include <osg/LOD>
#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osg/Transform>
#include <osgDB/ReadFile>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace nt {
namespace {
constexpr double kPreviewSimplifyRatio = 0.28;
constexpr std::size_t kPreviewMinTriangles = 64;

osg::ref_ptr<osg::StateSet> inherited(osg::StateSet* parent,osg::StateSet* local) {
    auto result=new osg::StateSet;if(parent) result->merge(*parent);if(local) result->merge(*local);return result;
}
bool compactExternalChildren(unsigned children,unsigned files) {return children<files;}
bool hasDrawableGeometry(const osg::Geometry* g) {
    if(!g) return false;
    auto vertices=g->getVertexArray();
    if(!vertices || !vertices->getNumElements()) return false;
    for(unsigned p=0;p<g->getNumPrimitiveSets();++p) {
        auto ps=g->getPrimitiveSet(p);
        if(!ps) continue;
        if(auto lengths=dynamic_cast<const osg::DrawArrayLengths*>(ps)) {
            for(auto length:*lengths) if(length>0) return true;
        } else if(ps->getNumIndices()>0) return true;
    }
    return false;
}
void collectResident(osg::Node* node,const osg::Matrixd& parent,osg::StateSet* previous,
                     std::vector<GeometryInput>& meshes) {
    if(node->getNodeMask()==0) return;
    osg::Matrixd matrix=parent;
    if(auto transform=dynamic_cast<osg::Transform*>(node)) {
        if(transform->getReferenceFrame()!=osg::Transform::RELATIVE_RF) throw std::runtime_error("预览: 不支持绝对参考系节点");
        if(!transform->computeLocalToWorldMatrix(matrix,nullptr)) throw std::runtime_error("预览: 无法求节点矩阵");
    }
    auto state=inherited(previous,node->getStateSet());
    if(auto geode=dynamic_cast<osg::Geode*>(node)) {
        for(unsigned i=0;i<geode->getNumDrawables();++i) {
            auto geometry=geode->getDrawable(i)->asGeometry();if(!geometry) throw std::runtime_error("预览: 非 Geometry Drawable");
            if(hasDrawableGeometry(geometry)) meshes.push_back({geometry,inherited(state.get(),geometry->getStateSet()),matrix});
        }
        return;
    }
    if(auto geometry=dynamic_cast<osg::Geometry*>(node)) {if(hasDrawableGeometry(geometry)) meshes.push_back({geometry,state,matrix});return;}
    if(auto paged=dynamic_cast<osg::PagedLOD*>(node)) {
        unsigned resident=0;bool compact=compactExternalChildren(paged->getNumChildren(),paged->getNumFileNames());
        for(unsigned i=0;i<paged->getNumChildren();++i) {
            if(!compact && i<paged->getNumFileNames() && !paged->getFileName(i).empty()) continue;
            if(++resident>1) throw std::runtime_error("预览: 多个无文件引用的常驻 LOD 分支");
            collectResident(paged->getChild(i),matrix,state.get(),meshes);
        }
        return;
    }
    if(auto proxy=dynamic_cast<osg::ProxyNode*>(node)) {
        bool compact=compactExternalChildren(proxy->getNumChildren(),proxy->getNumFileNames());
        for(unsigned i=0;i<proxy->getNumChildren();++i)
            if(compact || i>=proxy->getNumFileNames() || proxy->getFileName(i).empty()) collectResident(proxy->getChild(i),matrix,state.get(),meshes);
        return;
    }
    if(auto lod=dynamic_cast<osg::LOD*>(node)) if(lod->getNumChildren()>1) throw std::runtime_error("预览: 多个常驻 LOD 分支");
    if(auto group=node->asGroup()) for(unsigned i=0;i<group->getNumChildren();++i) collectResident(group->getChild(i),matrix,state.get(),meshes);
}

// ---------- Per-tile border-locked, UV-preserving QEM simplification ----------
struct Quadric {
    std::array<double,10> a{};
    Quadric& operator+=(const Quadric& o) {for(unsigned i=0;i<10;++i) a[i]+=o.a[i];return *this;}
    double value(const osg::Vec3d& p) const {
        double x=p.x(),y=p.y(),z=p.z();
        return std::max(0.0, a[0]*x*x + 2*a[1]*x*y + 2*a[2]*x*z + 2*a[3]*x +
                             a[4]*y*y + 2*a[5]*y*z + 2*a[6]*y + a[7]*z*z + 2*a[8]*z + a[9]);
    }
};

struct QemVertex {
    osg::Vec3d p;
    osg::Vec3d rawPos;
    osg::Vec2d uv;
    std::array<double,4> color{1,1,1,1};
    std::vector<unsigned> faces;
    unsigned version=0;
    bool alive=true;
    bool locked=false;
    bool modified=false;
};

struct QemFace {
    std::array<unsigned,3> v{};
    bool alive=true;
};

using Edge = std::pair<unsigned,unsigned>;
Edge makeEdge(unsigned a,unsigned b) {return a<b?Edge{a,b}:Edge{b,a};}

double uvCross(const osg::Vec2d& a,const osg::Vec2d& b,const osg::Vec2d& c) {
    return (b.x()-a.x())*(c.y()-a.y()) - (b.y()-a.y())*(c.x()-a.x());
}

osg::Vec3d faceCross(const std::vector<QemVertex>& verts,const QemFace& f) {
    return (verts[f.v[1]].p - verts[f.v[0]].p) ^ (verts[f.v[2]].p - verts[f.v[0]].p);
}

struct CollapseCandidate {
    double cost=0;
    unsigned a=0,b=0,va=0,vb=0;
    osg::Vec3d p;
    double t=0;
    bool operator<(const CollapseCandidate& o) const {
        return std::tie(cost,a,b) > std::tie(o.cost,o.a,o.b);
    }
};

bool readFloatArray(const osg::Array* array,unsigned width,unsigned count,std::vector<double>& out) {
    if(!array) return false;
    if(array->getDataSize()!=static_cast<int>(width)) return false;
    auto binding=array->getBinding();
    bool overall=binding==osg::Array::BIND_OVERALL;
    if(!overall && binding!=osg::Array::BIND_PER_VERTEX) return false;
    if((!overall && array->getNumElements()!=count) || (overall && array->getNumElements()<1)) return false;
    out.resize(static_cast<std::size_t>(count)*width);
    for(unsigned i=0;i<count;++i) {
        std::size_t base=static_cast<std::size_t>(overall?0:i)*width;
        for(unsigned c=0;c<width;++c) {
            double v=0;
            switch(array->getDataType()) {
                case GL_FLOAT: v=static_cast<const float*>(array->getDataPointer())[base+c]; break;
                case GL_DOUBLE: v=static_cast<const double*>(array->getDataPointer())[base+c]; break;
                case GL_UNSIGNED_BYTE: v=static_cast<const unsigned char*>(array->getDataPointer())[base+c]/255.0; break;
                case GL_UNSIGNED_SHORT: v=static_cast<const unsigned short*>(array->getDataPointer())[base+c]/65535.0; break;
                default: return false;
            }
            if(!std::isfinite(v)) return false;
            out[static_cast<std::size_t>(i)*width+c]=v;
        }
    }
    return true;
}

std::uint64_t doubleBits(double v) {
    if(v==0.0) v=0.0; // normalize -0.0
    std::uint64_t bits=0;
    std::memcpy(&bits,&v,sizeof(bits));
    return bits;
}

GeometryInput simplifyGeometryForPreview(const GeometryInput& input,const Options& options,
                                         std::uint64_t& sourceTriCount) {
    auto g=input.geometry.get();
    if(!g) return input;
    auto posArray=g->getVertexArray();
    if(!posArray || posArray->getNumElements()==0 || posArray->getDataSize()!=3 ||
       (posArray->getDataType()!=GL_FLOAT && posArray->getDataType()!=GL_DOUBLE)) return input;
    for(unsigned unit=1;unit<g->getNumTexCoordArrays();++unit)
        if(g->getTexCoordArray(unit)) return input;
    unsigned vcount=posArray->getNumElements();
    std::vector<double> rawPos;
    if(!readFloatArray(posArray,3,vcount,rawPos)) return input;

    auto uvArray=g->getTexCoordArray(0);
    std::vector<double> rawUv;
    bool hasUv=false;
    if(uvArray) {
        if(!readFloatArray(uvArray,2,vcount,rawUv)) return input;
        hasUv=true;
    }

    auto colorArray=g->getColorArray();
    std::vector<double> rawColor;
    unsigned colorWidth=0;
    if(colorArray) {
        int ds=colorArray->getDataSize();
        if(ds!=3 && ds!=4) return input;
        colorWidth=static_cast<unsigned>(ds);
        if(!readFloatArray(colorArray,colorWidth,vcount,rawColor)) return input;
    }

    // Extract all triangles; if any primitive set is non-triangle (points/lines), pass through unchanged.
    std::vector<std::uint32_t> triIndices;
    for(unsigned p=0;p<g->getNumPrimitiveSets();++p) {
        auto ps=g->getPrimitiveSet(p);
        unsigned mode=ps->getMode();
        if(mode!=GL_TRIANGLES && mode!=GL_TRIANGLE_STRIP && mode!=GL_TRIANGLE_FAN) return input;
        std::vector<std::vector<std::uint32_t>> runs;
        if(auto lengths=dynamic_cast<const osg::DrawArrayLengths*>(ps)) {
            if(lengths->getFirst()<0) return input;
            std::uint64_t first=lengths->getFirst();
            for(auto length:*lengths) {
                if(length<0 || first+static_cast<std::uint64_t>(length)>vcount) return input;
                std::vector<std::uint32_t> run;run.reserve(length);
                for(int i=0;i<length;++i) run.push_back(static_cast<std::uint32_t>(first+i));
                first+=length;runs.push_back(std::move(run));
            }
        } else {
            std::vector<std::uint32_t> run;run.reserve(ps->getNumIndices());
            for(unsigned i=0;i<ps->getNumIndices();++i) {
                unsigned idx=ps->index(i);
                if(idx>=vcount) return input;
                run.push_back(idx);
            }
            runs.push_back(std::move(run));
        }
        for(const auto& run:runs) {
            if(mode==GL_TRIANGLES) {
                if(run.size()%3!=0) return input;
                triIndices.insert(triIndices.end(),run.begin(),run.end());
            } else {
                for(std::size_t i=2;i<run.size();++i) {
                    auto a=mode==GL_TRIANGLE_FAN?run[0]:(i%2?run[i-1]:run[i-2]);
                    auto b=mode==GL_TRIANGLE_FAN?run[i-1]:(i%2?run[i-2]:run[i-1]);
                    auto c=run[i];
                    if(a!=b && b!=c && a!=c) {triIndices.push_back(a);triIndices.push_back(b);triIndices.push_back(c);}
                }
            }
        }
    }

    const std::size_t inputTris=triIndices.size()/3;
    sourceTriCount+=inputTris;
    if(inputTris<=kPreviewMinTriangles) return input;
    const std::size_t targetTris=std::max(kPreviewMinTriangles,static_cast<std::size_t>(std::llround(inputTris*kPreviewSimplifyRatio)));
    if(inputTris<=targetTris) return input;

    // Weld vertices with identical (pos, uv, color) so interior connectivity is clean while UV/color seams stay split.
    using FullKey = std::array<std::uint64_t,9>;
    using PosKey = std::array<std::uint64_t,3>;
    std::map<FullKey,unsigned> fullToId;
    std::map<PosKey,std::vector<unsigned>> posToIds;
    std::vector<QemVertex> verts;
    std::vector<unsigned> oldToNew(vcount,0);
    std::vector<bool> usedOld(vcount,false);
    for(auto idx:triIndices) usedOld[idx]=true;

    osg::Vec3d origin(0,0,0);
    std::size_t usedCount=0;
    for(unsigned i=0;i<vcount;++i) {
        if(!usedOld[i]) continue;
        origin+=osg::Vec3d(rawPos[i*3],rawPos[i*3+1],rawPos[i*3+2]);
        ++usedCount;
    }
    if(usedCount>0) origin/=static_cast<double>(usedCount);

    verts.reserve(usedCount);
    for(unsigned i=0;i<vcount;++i) {
        if(!usedOld[i]) continue;
        FullKey fk{};
        fk[0]=doubleBits(rawPos[i*3]);
        fk[1]=doubleBits(rawPos[i*3+1]);
        fk[2]=doubleBits(rawPos[i*3+2]);
        fk[3]=hasUv?doubleBits(rawUv[i*2]):0;
        fk[4]=hasUv?doubleBits(rawUv[i*2+1]):0;
        for(unsigned c=0;c<4;++c) fk[5+c]=(c<colorWidth)?doubleBits(rawColor[i*colorWidth+c]):0;
        auto it=fullToId.find(fk);
        if(it!=fullToId.end()) {
            oldToNew[i]=it->second;
            continue;
        }
        unsigned id=static_cast<unsigned>(verts.size());
        fullToId[fk]=id;
        oldToNew[i]=id;
        QemVertex v;
        v.rawPos=osg::Vec3d(rawPos[i*3],rawPos[i*3+1],rawPos[i*3+2]);
        v.p=v.rawPos-origin;
        if(hasUv) v.uv=osg::Vec2d(rawUv[i*2],rawUv[i*2+1]);
        for(unsigned c=0;c<colorWidth;++c) v.color[c]=rawColor[i*colorWidth+c];
        verts.push_back(std::move(v));
        PosKey pk{fk[0],fk[1],fk[2]};
        posToIds[pk].push_back(id);
    }

    // Lock vertices that share a 3D position with another welded vertex (UV/color seams).
    for(const auto& kv:posToIds) {
        if(kv.second.size()>1) {
            for(unsigned id:kv.second) verts[id].locked=true;
        }
    }

    std::vector<QemFace> faces;
    faces.reserve(inputTris);
    std::vector<Edge> allEdges;
    allEdges.reserve(inputTris*3);
    for(std::size_t i=0;i<inputTris;++i) {
        unsigned a=oldToNew[triIndices[i*3]];
        unsigned b=oldToNew[triIndices[i*3+1]];
        unsigned c=oldToNew[triIndices[i*3+2]];
        if(a==b || b==c || c==a) continue;
        unsigned fid=static_cast<unsigned>(faces.size());
        faces.push_back({{a,b,c},true});
        verts[a].faces.push_back(fid);
        verts[b].faces.push_back(fid);
        verts[c].faces.push_back(fid);
        allEdges.push_back(makeEdge(a,b));
        allEdges.push_back(makeEdge(b,c));
        allEdges.push_back(makeEdge(c,a));
    }

    if(faces.size()<=targetTris) return input;

    // Count edge incidences; lock both endpoints of any open boundary (count==1) or non-manifold (count>2) edge.
    std::sort(allEdges.begin(),allEdges.end());
    std::vector<Edge> uniqueEdges;
    uniqueEdges.reserve(allEdges.size()/2+1);
    for(std::size_t i=0;i<allEdges.size();) {
        std::size_t j=i+1;
        while(j<allEdges.size() && allEdges[j]==allEdges[i]) ++j;
        if(j-i!=2) {
            verts[allEdges[i].first].locked=true;
            verts[allEdges[i].second].locked=true;
        }
        uniqueEdges.push_back(allEdges[i]);
        i=j;
    }

    // Compute plane quadrics per vertex.
    std::vector<Quadric> quadrics(verts.size());
    for(const auto& f:faces) {
        osg::Vec3d n=faceCross(verts,f);
        if(n.normalize()==0) continue;
        double d=-(n*verts[f.v[0]].p);
        Quadric plane{{n.x()*n.x(),n.x()*n.y(),n.x()*n.z(),n.x()*d,
                       n.y()*n.y(),n.y()*n.z(),n.y()*d,
                       n.z()*n.z(),n.z()*d,d*d}};
        for(unsigned v:f.v) quadrics[v]+=plane;
    }

    auto makeCandidate=[&](unsigned a,unsigned b,CollapseCandidate& out)->bool {
        const auto& va=verts[a];
        const auto& vb=verts[b];
        if(va.locked && vb.locked) return false;
        Quadric q=quadrics[a];
        q+=quadrics[b];
        out.a=a;out.b=b;out.va=va.version;out.vb=vb.version;
        out.cost=std::numeric_limits<double>::infinity();
        osg::Vec3d ab=vb.p-va.p;
        double abLen2=ab.length2();
        auto eval=[&](const osg::Vec3d& p,double t) {
            double cost=q.value(p);
            if(colorWidth>0) {
                double cdiff=0;
                for(unsigned k=0;k<colorWidth;++k) cdiff+=std::abs(va.color[k]-vb.color[k]);
                cost+=cdiff*1e-2;
            }
            if(std::isfinite(cost) && cost<out.cost) {out.cost=cost;out.p=p;out.t=t;}
        };
        if(va.locked) {
            eval(va.p,0.0);
        } else if(vb.locked) {
            eval(vb.p,1.0);
        } else {
            eval(va.p,0.0);
            eval(vb.p,1.0);
            eval((va.p+vb.p)*0.5,0.5);
            const auto& c=q.a;
            double det=c[0]*(c[4]*c[7]-c[5]*c[5])-c[1]*(c[1]*c[7]-c[2]*c[5])+c[2]*(c[1]*c[5]-c[2]*c[4]);
            if(std::abs(det)>1e-10) {
                osg::Vec3d opt(
                    -((c[4]*c[7]-c[5]*c[5])*c[3]+(c[2]*c[5]-c[1]*c[7])*c[6]+(c[1]*c[5]-c[2]*c[4])*c[8])/det,
                    -((c[2]*c[5]-c[1]*c[7])*c[3]+(c[0]*c[7]-c[2]*c[2])*c[6]+(c[1]*c[2]-c[0]*c[5])*c[8])/det,
                    -((c[1]*c[5]-c[2]*c[4])*c[3]+(c[1]*c[2]-c[0]*c[5])*c[6]+(c[0]*c[4]-c[1]*c[1])*c[8])/det);
                osg::Vec3d mid=(va.p+vb.p)*0.5;
                if((opt-mid).length2() <= std::max(abLen2*4.0, 1e-12)) {
                    double t=abLen2>1e-24?std::clamp(((opt-va.p)*ab)/abLen2,0.0,1.0):0.5;
                    eval(opt,t);
                }
            }
        }
        return std::isfinite(out.cost);
    };

    auto sharedFaceCount=[&](unsigned a,unsigned b)->unsigned {
        unsigned count=0;
        for(unsigned fi:verts[a].faces) {
            const auto& f=faces[fi];
            if(f.alive && (f.v[0]==b || f.v[1]==b || f.v[2]==b)) ++count;
        }
        return count;
    };

    auto collectNeighbors=[&](unsigned v,std::vector<unsigned>& out) {
        out.clear();
        for(unsigned fi:verts[v].faces) {
            const auto& f=faces[fi];
            if(!f.alive) continue;
            for(unsigned k:f.v) if(k!=v) out.push_back(k);
        }
        std::sort(out.begin(),out.end());
        out.erase(std::unique(out.begin(),out.end()),out.end());
    };

    std::vector<unsigned> na,nb,around,affected,scratch;
    auto safeCollapse=[&](const CollapseCandidate& c)->bool {
        unsigned shared=sharedFaceCount(c.a,c.b);
        if(shared!=2) return false;
        collectNeighbors(c.a,na);
        collectNeighbors(c.b,nb);
        unsigned common=0;
        std::size_t ia=0,ib=0;
        while(ia<na.size() && ib<nb.size()) {
            if(na[ia]==nb[ib]) {++common;++ia;++ib;}
            else if(na[ia]<nb[ib]) ++ia;
            else ++ib;
        }
        if(common!=2) return false; // strict manifold link condition

        osg::Vec2d newUv=verts[c.a].uv*(1.0-c.t)+verts[c.b].uv*c.t;
        auto checkList=[&](const std::vector<unsigned>& flist)->bool {
            for(unsigned fi:flist) {
                const auto& f=faces[fi];
                if(!f.alive) continue;
                bool hasA=false,hasB=false;
                std::array<osg::Vec3d,3> np;
                std::array<osg::Vec2d,3> nuv;
                for(unsigned k=0;k<3;++k) {
                    if(f.v[k]==c.a) {hasA=true;np[k]=c.p;nuv[k]=newUv;}
                    else if(f.v[k]==c.b) {hasB=true;np[k]=c.p;nuv[k]=newUv;}
                    else {np[k]=verts[f.v[k]].p;nuv[k]=verts[f.v[k]].uv;}
                }
                if(hasA && hasB) continue;
                osg::Vec3d before=faceCross(verts,f);
                osg::Vec3d after=(np[1]-np[0])^(np[2]-np[0]);
                if(after.length2()<1e-24 || before*after<=0.0) return false;
                if(hasUv) {
                    double uvBefore=uvCross(verts[f.v[0]].uv,verts[f.v[1]].uv,verts[f.v[2]].uv);
                    if(std::abs(uvBefore)>1e-12) {
                        double uvAfter=uvCross(nuv[0],nuv[1],nuv[2]);
                        if(uvBefore*uvAfter<=0.0) return false;
                    }
                }
            }
            return true;
        };
        return checkList(verts[c.a].faces) && checkList(verts[c.b].faces);
    };

    std::priority_queue<CollapseCandidate> queue;
    for(const auto& e:uniqueEdges) {
        CollapseCandidate cand;
        if(makeCandidate(e.first,e.second,cand)) queue.push(cand);
    }

    std::size_t live=faces.size();
    std::size_t collapses=0;
    std::vector<Edge> update;
    while(live>targetTris && !queue.empty()) {
        if((collapses & 255)==0 && options.checkCancelled) options.checkCancelled();
        auto c=queue.top();queue.pop();
        auto& va=verts[c.a];
        auto& vb=verts[c.b];
        if(!va.alive || !vb.alive || va.version!=c.va || vb.version!=c.vb) continue;
        if(!safeCollapse(c)) continue;

        // Always keep the locked vertex if one is locked, so its exact raw position/UV/color is untouched.
        unsigned keep=vb.locked?c.b:c.a;
        unsigned drop=vb.locked?c.a:c.b;
        auto& vk=verts[keep];
        auto& vd=verts[drop];

        affected=vk.faces;
        affected.insert(affected.end(),vd.faces.begin(),vd.faces.end());
        std::sort(affected.begin(),affected.end());
        affected.erase(std::unique(affected.begin(),affected.end()),affected.end());

        collectNeighbors(keep,around);
        collectNeighbors(drop,nb);
        around.insert(around.end(),nb.begin(),nb.end());
        around.push_back(keep);
        std::sort(around.begin(),around.end());
        around.erase(std::unique(around.begin(),around.end()),around.end());
        around.erase(std::remove(around.begin(),around.end(),drop),around.end());

        if(!vk.locked) {
            double tKeep=(keep==c.a)?c.t:(1.0-c.t);
            vk.uv=vk.uv*(1.0-tKeep)+vd.uv*tKeep;
            for(unsigned k=0;k<colorWidth;++k) vk.color[k]=vk.color[k]*(1.0-tKeep)+vd.color[k]*tKeep;
            vk.p=c.p;
            vk.modified=true;
        }
        quadrics[keep]+=quadrics[drop];
        vd.alive=false;

        auto removeFace=[&](QemVertex& v,unsigned fi) {
            auto it=std::find(v.faces.begin(),v.faces.end(),fi);
            if(it!=v.faces.end()) {*it=v.faces.back();v.faces.pop_back();}
        };
        for(unsigned fi:affected) {
            auto& f=faces[fi];
            if(!f.alive) continue;
            for(unsigned vi:f.v) removeFace(verts[vi],fi);
            for(unsigned& vi:f.v) if(vi==drop) vi=keep;
            if(f.v[0]==f.v[1] || f.v[1]==f.v[2] || f.v[2]==f.v[0]) {
                f.alive=false;--live;
            } else {
                for(unsigned vi:f.v) verts[vi].faces.push_back(fi);
            }
        }
        for(unsigned v:around) ++verts[v].version;
        update.clear();
        for(unsigned v:around) {
            collectNeighbors(v,scratch);
            for(unsigned n:scratch) update.push_back(makeEdge(v,n));
        }
        std::sort(update.begin(),update.end());
        update.erase(std::unique(update.begin(),update.end()),update.end());
        for(const auto& e:update) {
            CollapseCandidate cand;
            if(makeCandidate(e.first,e.second,cand)) queue.push(cand);
        }
        ++collapses;
    }

    if(collapses==0) return input;

    // Compact surviving vertices and rebuild osg::Geometry (omitting normals since preview is unlit).
    std::vector<unsigned> compact(verts.size(),std::numeric_limits<unsigned>::max());
    std::vector<unsigned> keptVerts;
    keptVerts.reserve(verts.size());
    std::vector<std::uint32_t> outIndices;
    outIndices.reserve(live*3);
    for(const auto& f:faces) {
        if(!f.alive) continue;
        for(unsigned vi:f.v) {
            if(compact[vi]==std::numeric_limits<unsigned>::max()) {
                compact[vi]=static_cast<unsigned>(keptVerts.size());
                keptVerts.push_back(vi);
            }
            outIndices.push_back(compact[vi]);
        }
    }
    if(outIndices.empty()) return input;

    auto outGeom=new osg::Geometry;
    if(posArray->getDataType()==GL_DOUBLE) {
        auto outPos=new osg::Vec3dArray;outPos->reserve(keptVerts.size());
        for(unsigned vi:keptVerts) {
            const auto& v=verts[vi];
            outPos->push_back(v.modified?(origin+v.p):v.rawPos);
        }
        outGeom->setVertexArray(outPos);
    } else {
        auto outPos=new osg::Vec3Array;outPos->reserve(keptVerts.size());
        for(unsigned vi:keptVerts) {
            const auto& v=verts[vi];
            osg::Vec3d p=v.modified?(origin+v.p):v.rawPos;
            outPos->push_back(osg::Vec3(static_cast<float>(p.x()),static_cast<float>(p.y()),static_cast<float>(p.z())));
        }
        outGeom->setVertexArray(outPos);
    }
    if(hasUv) {
        auto outUv=new osg::Vec2Array;outUv->reserve(keptVerts.size());
        for(unsigned vi:keptVerts) {
            const auto& uv=verts[vi].uv;
            outUv->push_back(osg::Vec2(static_cast<float>(uv.x()),static_cast<float>(uv.y())));
        }
        outGeom->setTexCoordArray(0,outUv,osg::Array::BIND_PER_VERTEX);
    }
    if(colorWidth==3) {
        auto outCol=new osg::Vec3Array;outCol->reserve(keptVerts.size());
        for(unsigned vi:keptVerts) {
            const auto& col=verts[vi].color;
            outCol->push_back(osg::Vec3(static_cast<float>(col[0]),static_cast<float>(col[1]),static_cast<float>(col[2])));
        }
        outGeom->setColorArray(outCol,osg::Array::BIND_PER_VERTEX);
    } else if(colorWidth==4) {
        auto outCol=new osg::Vec4Array;outCol->reserve(keptVerts.size());
        for(unsigned vi:keptVerts) {
            const auto& col=verts[vi].color;
            outCol->push_back(osg::Vec4(static_cast<float>(col[0]),static_cast<float>(col[1]),static_cast<float>(col[2]),static_cast<float>(col[3])));
        }
        outGeom->setColorArray(outCol,osg::Array::BIND_PER_VERTEX);
    }
    if(keptVerts.size()<65535) {
        auto prim=new osg::DrawElementsUShort(GL_TRIANGLES);
        prim->reserve(outIndices.size());
        for(auto idx:outIndices) prim->push_back(static_cast<std::uint16_t>(idx));
        outGeom->addPrimitiveSet(prim);
    } else {
        auto prim=new osg::DrawElementsUInt(GL_TRIANGLES);
        prim->reserve(outIndices.size());
        for(auto idx:outIndices) prim->push_back(idx);
        outGeom->addPrimitiveSet(prim);
    }
    return {outGeom,input.state,input.matrix};
}

int parseSplitBranch(const std::string& uri) {
    auto pos=uri.rfind("-split-");
    if(pos==std::string::npos) return -1;
    auto end=uri.rfind(".glb");
    if(end==std::string::npos || end<=pos+7) return -1;
    try {return std::stoi(uri.substr(pos+7,end-(pos+7)));} catch(...) {return -1;}
}

struct TileTask {
    std::string mainUri;
    std::string previewUri;
    fs::path file;
    osg::Matrixd matrix=osg::Matrixd::identity();
    int splitBranch=-1;
    bool isOverview=false;
    bool isLeaf=false;
    unsigned contentDepth=0;
};

struct TileResult {
    ContentResult content;
    std::uint64_t sourceTriangles=0;
    std::uint64_t embeddedImageBytes=0;
    std::uint64_t embeddedImages=0;
    bool isOverview=false;
    bool isLeaf=false;
};

TileResult convertTile(const TileTask& task,const Options& tileOptions,const SpatialReference* spatial,
                       const std::function<void(const fs::path&)>& checkSource) {
    if(tileOptions.checkCancelled) tileOptions.checkCancelled();
    if(checkSource) checkSource(task.file);
    osg::ref_ptr<osgDB::Options> readOptions=new osgDB::Options;
    readOptions->setObjectCacheHint(osgDB::Options::CACHE_NONE);
    auto node=osgDB::readRefNodeFile(task.file.u8string(),readOptions.get());
    if(!node) throw std::runtime_error("预览: 源读取失败: "+task.file.u8string());
    std::vector<GeometryInput> meshes;
    if(task.splitBranch>=0) {
        auto group=node->asGroup();
        auto state=group?inherited(nullptr,group->getStateSet()):osg::ref_ptr<osg::StateSet>();
        if(!group || static_cast<unsigned>(task.splitBranch)>=group->getNumChildren())
            throw std::runtime_error("预览: 分支索引越界: "+task.file.u8string());
        collectResident(group->getChild(static_cast<unsigned>(task.splitBranch)),task.matrix,state.get(),meshes);
    } else {
        collectResident(node.get(),task.matrix,nullptr,meshes);
    }
    if(meshes.empty()) throw std::runtime_error("预览: 源常驻几何为空: "+task.file.u8string());
    TileResult out;
    out.isOverview=task.isOverview;
    out.isLeaf=task.isLeaf;
    std::vector<GeometryInput> simplified;
    simplified.reserve(meshes.size());
    for(const auto& mesh:meshes) {
        simplified.push_back(simplifyGeometryForPreview(mesh,tileOptions,out.sourceTriangles));
    }
    Options localOptions=tileOptions;
    // 仅对非叶层（远/中景）瓦片按层级压小内嵌纹理以控制弱网首屏流量（2~4 MB）；
    // 叶子层（isLeaf）恒为 0，100% 原始无损直通，确保拉近与保真版完全一致清晰。
    if(!task.isLeaf) {
        if(task.contentDepth<=2) {localOptions.previewMaxTextureSize=256;localOptions.previewJpegQuality=78;}
        else {localOptions.previewMaxTextureSize=512;localOptions.previewJpegQuality=82;}
    } else {
        localOptions.previewMaxTextureSize=0;
    }
    out.content=writeGlb(simplified,localOptions.output/fs::u8path(task.previewUri),localOptions.output/"textures",
                         true,spatial,true,true,&localOptions);
    if(out.sourceTriangles<out.content.triangles) out.sourceTriangles=out.content.triangles;
    for(const auto& image:out.content.imageAudit) {
        out.embeddedImages++;
        out.embeddedImageBytes+=image.value("bytes",0ULL);
    }
    return out;
}
}

Json buildPreview(const Json& mainRoot,const Json& records,const Options& options,
                  const SpatialReference* spatial,const osg::Matrixd& placement,
                  const std::function<void(const fs::path&)>& checkSource) {
    auto started=std::chrono::steady_clock::now();
    std::map<std::string,std::pair<const Json*,const Json*>> sourceByContent;
    for(const auto& row:records)
        for(const auto& content:row.at("content"))
            sourceByContent[content.at("uri").get<std::string>()]={&row,&content};

    std::vector<TileTask> tasks;
    std::map<std::string,std::size_t> taskIndexByMain;
    std::uint64_t sourceTiles=0,prunedTiles=0,sourceLeafTriangles=0,sourceOverviewTriangles=0;
    unsigned maxKeptDepth=0;

    auto registerContent=[&](const std::string& mainUri,bool isOverview,bool isLeaf,unsigned contentDepth)->std::string {
        auto found=taskIndexByMain.find(mainUri);
        if(found!=taskIndexByMain.end()) {
            tasks[found->second].isOverview|=isOverview;
            tasks[found->second].isLeaf|=isLeaf;
            tasks[found->second].contentDepth=std::max(tasks[found->second].contentDepth,contentDepth);
            return tasks[found->second].previewUri;
        }
        auto srcIt=sourceByContent.find(mainUri);
        if(srcIt==sourceByContent.end()) throw std::runtime_error("预览缺少源记录: "+mainUri);
        const auto& row=*srcIt->second.first;
        auto matrixValues=row.at("sourceTransform").get<std::vector<double>>();
        if(matrixValues.size()!=16) throw std::runtime_error("预览源矩阵无效");
        std::size_t idx=tasks.size();
        std::string previewUri="content/tile-"+std::to_string(idx)+".glb";
        tasks.push_back({mainUri,previewUri,fs::u8path(row.at("source").get<std::string>()),
                         osg::Matrixd(matrixValues.data()),parseSplitBranch(mainUri),isOverview,isLeaf,contentDepth});
        taskIndexByMain[mainUri]=idx;
        return previewUri;
    };

    // 沿原 OSGB REPLACE 树抽层（保留顶层概览、偶数层级与全部叶层，跳过冗余奇数中间层）
    // + 逐瓦片锁边保 UV QEM 简化至 ~28% 面数 + 内嵌 JPEG/PNG 纹理减少网络往返。
    auto prune=[&](auto&& self,const Json& tile,unsigned contentDepth,unsigned keptDepth)->Json {
        Json results=Json::array();
        const bool hasContent=tile.contains("content");
        if(hasContent) ++sourceTiles;
        Json prunedChildren=Json::array();
        if(tile.contains("children")) {
            unsigned nextContentDepth=hasContent?contentDepth+1:contentDepth;
            unsigned nextKeptDepth=keptDepth;
            if(hasContent) {
                bool willKeep=(contentDepth==0) || (contentDepth%2==0);
                if(willKeep) nextKeptDepth=keptDepth+1;
            }
            for(const auto& child:tile["children"]) {
                auto sub=self(self,child,nextContentDepth,nextKeptDepth);
                for(auto& item:sub) prunedChildren.push_back(std::move(item));
            }
        }
        if(!hasContent) {
            if(prunedChildren.empty()) return results;
            if(!tile.contains("transform")) return prunedChildren;
            Json copy=tile;copy["children"]=std::move(prunedChildren);results.push_back(std::move(copy));
            return results;
        }
        const bool isLeaf=prunedChildren.empty();
        const bool isOverview=(contentDepth==0);
        const bool keep=isOverview || isLeaf || (contentDepth%2==0);
        if(!keep) {
            ++prunedTiles;
            if(!tile.contains("transform") || prunedChildren.empty()) return prunedChildren;
            Json wrapper={{"boundingVolume",tile.at("boundingVolume")},
                          {"geometricError",tile.value("geometricError",0.0)},
                          {"refine","REPLACE"},
                          {"transform",tile.at("transform")},
                          {"children",std::move(prunedChildren)}};
            results.push_back(std::move(wrapper));
            return results;
        }
        const auto mainUri=tile.at("content").at("uri").get<std::string>();
        const auto tris=sourceByContent.at(mainUri).second->at("triangles").get<std::uint64_t>();
        if(isOverview) sourceOverviewTriangles+=tris;
        if(isLeaf) sourceLeafTriangles+=tris;
        maxKeptDepth=std::max(maxKeptDepth,keptDepth+1);
        Json node={{"boundingVolume",tile.at("boundingVolume")},
                   {"geometricError",tile.value("geometricError",0.0)},
                   {"refine","REPLACE"},
                   {"content",{{"uri",registerContent(mainUri,isOverview,isLeaf,contentDepth)}}}};
        if(tile.contains("transform")) node["transform"]=tile.at("transform");
        if(!prunedChildren.empty()) node["children"]=std::move(prunedChildren);
        results.push_back(std::move(node));
        return results;
    };

    Json previewChildren=Json::array();
    if(mainRoot.contains("children")) {
        for(const auto& child:mainRoot["children"]) {
            auto sub=prune(prune,child,0,0);
            for(auto& item:sub) previewChildren.push_back(std::move(item));
        }
    }
    if(tasks.empty()) throw std::runtime_error("预览: 没有可转换的有效瓦片");

    fs::create_directories(options.output/"content");
    if(options.event) options.event({{"type","progress"},{"stage","preview"},{"done",0},{"total",tasks.size()},{"unit","files"}});

    Options tileOptions=options;
    tileOptions.preserveEncoded=true;
    tileOptions.compressGeometry=true;
    tileOptions.quantizeGeometry=true;
    tileOptions.dracoGeometry=false;
    tileOptions.textureFormat="original";
    tileOptions.preview=true;
    tileOptions.previewEncoding=true;

    const unsigned workers=std::min<unsigned>(std::max(1U,options.threads),static_cast<unsigned>(tasks.size()));
    std::uint64_t processed=0;
    std::mutex progressMutex;
    std::deque<std::future<TileResult>> pending;
    bool stopping=false;std::exception_ptr failure;
    std::uint64_t verticesAllLods=0,trianglesAllLods=0,sourceTrianglesAllLods=0;
    std::uint64_t leafTriangles=0,overviewTriangles=0;
    std::uint64_t glbBytes=0,geometryRawBytes=0,geometryStoredBytes=0,geometryOriginalFloatBytes=0,compressedViews=0;
    std::uint64_t embeddedImages=0,textureBytes=0;
    double quantizationErrorMeters=0,coordinateRoundingError=0;

    for(std::size_t next=0;next<tasks.size() || !pending.empty();) {
        try {
            if(tileOptions.checkCancelled) tileOptions.checkCancelled();
            while(!stopping && next<tasks.size() && pending.size()<workers) {
                if(tileOptions.memoryBudgetBytes && !pending.empty() && residentBytes()>=tileOptions.memoryBudgetBytes) break;
                const std::size_t index=next;
                pending.push_back(std::async(std::launch::async,[&,index] {
                    auto res=convertTile(tasks[index],tileOptions,spatial,checkSource);
                    {
                        std::lock_guard<std::mutex> lock(progressMutex);
                        ++processed;
                        if(tileOptions.event) tileOptions.event({{"type","progress"},{"stage","preview"},{"done",processed},{"total",tasks.size()},{"unit","files"}});
                    }
                    return res;
                }));
                ++next;
            }
        } catch(...) {stopping=true;if(!failure) failure=std::current_exception();}
        if(pending.empty()) break;
        try {
            auto completed=pending.front().get();
            verticesAllLods+=completed.content.vertices;
            trianglesAllLods+=completed.content.triangles;
            sourceTrianglesAllLods+=completed.sourceTriangles;
            if(completed.isOverview) overviewTriangles+=completed.content.triangles;
            if(completed.isLeaf) leafTriangles+=completed.content.triangles;
            glbBytes+=completed.content.bytes;
            geometryRawBytes+=completed.content.geometryRawBytes;
            geometryStoredBytes+=completed.content.geometryStoredBytes;
            geometryOriginalFloatBytes+=completed.content.originalFloatBytes;
            compressedViews+=completed.content.compressedViews;
            quantizationErrorMeters=std::max(quantizationErrorMeters,completed.content.quantizationErrorMeters);
            coordinateRoundingError=std::max(coordinateRoundingError,completed.content.coordinateRoundingError);
            embeddedImages+=completed.embeddedImages;
            textureBytes+=completed.embeddedImageBytes;
        } catch(...) {stopping=true;if(!failure) failure=std::current_exception();}
        pending.pop_front();
    }
    if(failure) std::rethrow_exception(failure);

    // 预览根是无内容容器；与主产物一致保持 >= 2000 误差，确保任意初始视距下首帧立即细化到概览层。
    double rootError=std::max(mainRoot.value("geometricError",0.0),2000.0);
    Json previewRoot={{"boundingVolume",mainRoot.at("boundingVolume")},
                      {"transform",std::vector<double>(placement.ptr(),placement.ptr()+16)},
                      {"geometricError",rootError},
                      {"refine","REPLACE"},
                      {"children",std::move(previewChildren)}};
    atomicJson(options.output/"tileset.json",{{"asset",{{"version","1.1"},{"generator",kAssetGenerator}}},
                                              {"geometricError",rootError},
                                              {"root",previewRoot}});

    double simplificationRatio=sourceTrianglesAllLods>0
        ? static_cast<double>(trianglesAllLods)/static_cast<double>(sourceTrianglesAllLods)
        : 1.0;
    Json report={{"status","complete"},{"entry","preview/tileset.json"},{"algorithm","mesh-preview-v2"},
        {"streamingLod",true},{"lodDepth",maxKeptDepth},
        {"sourceTiles",sourceTiles},{"prunedTiles",prunedTiles},{"glbs",tasks.size()},
        {"overviewTriangles",overviewTriangles},{"sourceOverviewTriangles",sourceOverviewTriangles},
        {"triangles",leafTriangles},{"sourceLeafTriangles",sourceLeafTriangles},
        {"trianglesAllLods",trianglesAllLods},{"sourceTrianglesAllLods",sourceTrianglesAllLods},
        {"simplificationRatio",simplificationRatio},{"borderLocked",true},
        {"verticesAllLods",verticesAllLods},
        {"geometryEncoding","EXT_meshopt_compression+KHR_mesh_quantization"},{"textureFormat","original"},
        {"texturesEmbedded",true},{"embeddedImages",embeddedImages},
        {"geometryOriginalFloatBytes",geometryOriginalFloatBytes},{"geometryRawBytes",geometryRawBytes},
        {"geometryStoredBytes",geometryStoredBytes},{"compressedViews",compressedViews},
        {"quantizationErrorMeters",quantizationErrorMeters},{"coordinateRoundingErrorMeters",coordinateRoundingError},
        {"uniqueTextureFiles",0},{"glbBytes",glbBytes},{"textureBytes",textureBytes},
        {"elapsedSeconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()},
        {"normals","omitted"},{"material","KHR_materials_unlit"},{"warnings",Json::array()}};
    atomicJson(options.output/"preview-report.json",report);
    return report;
}
}
