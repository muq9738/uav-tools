#include "preview.hpp"
#include "top.hpp"
#include <osg/AlphaFunc>
#include <osg/Geode>
#include <osg/LOD>
#include <osg/Material>
#include <osg/PagedLOD>
#include <osg/ProxyNode>
#include <osg/TexEnv>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osgDB/ReadFile>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace nt {
namespace {
using V=osg::Vec3d;
using UV=osg::Vec2d;
using Color=osg::Vec4d;
constexpr unsigned atlasLimit=4096;
constexpr unsigned gutter=4;
constexpr std::size_t defaultTriangleBudget=100000;
constexpr std::size_t maximumSpatialBlocks=32;

osg::ref_ptr<osg::StateSet> inherited(osg::StateSet* parent,osg::StateSet* local) {
    auto result=new osg::StateSet;if(parent) result->merge(*parent);if(local) result->merge(*local);return result;
}
bool compactExternalChildren(unsigned children,unsigned files) {return children<files;}
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
            meshes.push_back({geometry,inherited(state.get(),geometry->getStateSet()),matrix});
        }
        return;
    }
    if(auto geometry=dynamic_cast<osg::Geometry*>(node)) {meshes.push_back({geometry,state,matrix});return;}
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

std::vector<std::vector<std::size_t>> spatialGroups(const std::vector<PreviewSource>& sources,std::size_t wanted) {
    std::vector<std::vector<std::size_t>> groups(1);groups[0].resize(sources.size());std::iota(groups[0].begin(),groups[0].end(),0);
    while(groups.size()<wanted) {
        std::size_t selected=groups.size();std::uint64_t most=0;
        for(std::size_t i=0;i<groups.size();++i) if(groups[i].size()>1) {
            std::uint64_t triangles=0;for(auto index:groups[i]) triangles+=sources[index].inputTriangles;
            if(triangles>most) {most=triangles;selected=i;}
        }
        if(selected==groups.size()) break;
        auto group=std::move(groups[selected]);groups.erase(groups.begin()+selected);
        unsigned axis=0;V low=group.empty()?V():sources[group[0]].bounds._min,high=group.empty()?V():sources[group[0]].bounds._max;
        for(auto index:group) {low.x()=std::min(low.x(),sources[index].bounds.xMin());low.y()=std::min(low.y(),sources[index].bounds.yMin());low.z()=std::min(low.z(),sources[index].bounds.zMin());
            high.x()=std::max(high.x(),sources[index].bounds.xMax());high.y()=std::max(high.y(),sources[index].bounds.yMax());high.z()=std::max(high.z(),sources[index].bounds.zMax());}
        if(high.y()-low.y()>high[axis]-low[axis]) axis=1;if(high.z()-low.z()>high[axis]-low[axis]) axis=2;
        std::stable_sort(group.begin(),group.end(),[&](std::size_t a,std::size_t b) {
            return sources[a].bounds.center()[axis]<sources[b].bounds.center()[axis];});
        std::size_t middle=group.size()/2;groups.push_back(std::vector<std::size_t>(group.begin(),group.begin()+middle));
        groups.push_back(std::vector<std::size_t>(group.begin()+middle,group.end()));
    }
    return groups;
}

std::string imageKey(const osg::Image& image) {return contentKey(encodeImage(image)->bytes);}
std::string stateSignature(const osg::StateSet* state) {
    std::string key;
    auto flag=[&](unsigned mode) {key+=state && (state->getMode(mode)&osg::StateAttribute::ON)?'1':'0';};
    flag(GL_CULL_FACE);flag(GL_BLEND);flag(GL_ALPHA_TEST);
    if(state) {
        if(auto alpha=dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC)))
            key+=":alpha:"+std::to_string(alpha->getFunction())+":"+std::to_string(alpha->getReferenceValue());
        if(auto material=dynamic_cast<const osg::Material*>(state->getAttribute(osg::StateAttribute::MATERIAL))) {
            auto color=material->getDiffuse(osg::Material::FRONT);key+=":diffuse:"+std::to_string(color.r())+":"+std::to_string(color.g())+":"+std::to_string(color.b())+":"+std::to_string(color.a());
        }
        auto env=dynamic_cast<const osg::TexEnv*>(state->getTextureAttribute(0,osg::StateAttribute::TEXENV));
        key+=env&&env->getMode()!=osg::TexEnv::MODULATE?":nonmodulate":":modulate";
    } else key+=":default";
    return key;
}
std::string samplerSignature(const osg::Texture* texture) {
    if(!texture) return {};
    return std::to_string(texture->getWrap(osg::Texture::WRAP_S))+":"+std::to_string(texture->getWrap(osg::Texture::WRAP_T))+":"+
        std::to_string(texture->getFilter(osg::Texture::MIN_FILTER))+":"+std::to_string(texture->getFilter(osg::Texture::MAG_FILTER));
}
bool clamped(const osg::Texture& texture) {
    auto wrap=[&](osg::Texture::WrapMode mode) {return mode==osg::Texture::CLAMP || mode==osg::Texture::CLAMP_TO_EDGE;};
    return wrap(texture.getWrap(osg::Texture::WRAP_S)) && wrap(texture.getWrap(osg::Texture::WRAP_T));
}
osg::ref_ptr<osg::Image> pageImage(osg::Image* source,Json& warnings) {
    // 图集拷贝统一为 RGBA8，避免 RGB/灰度源按四字节读取；行列顺序保持 OSG 原样。
    osg::ref_ptr<osg::Image> copy=new osg::Image;copy->allocateImage(source->s(),source->t(),1,GL_RGBA,GL_UNSIGNED_BYTE,1);copy->setOrigin(source->getOrigin());copy->setFileName(source->getFileName());
    for(int y=0;y<source->t();++y) for(int x=0;x<source->s();++x) {
        auto color=source->getColor(x,y,0);auto pixel=copy->data(x,y);for(unsigned k=0;k<4;++k) pixel[k]=static_cast<unsigned char>(std::lround(std::clamp(color[k],0.f,1.f)*255));
    }
    if(copy->s()>int(atlasLimit) || copy->t()>int(atlasLimit)) {
        double scale=std::min(double(atlasLimit)/copy->s(),double(atlasLimit)/copy->t());
        unsigned w=std::max(4U,static_cast<unsigned>(std::floor(copy->s()*scale/4.))*4);
        unsigned h=std::max(4U,static_cast<unsigned>(std::floor(copy->t()*scale/4.))*4);
        copy->scaleImage(w,h,1);
        warnings.push_back("源纹理 "+source->getFileName()+" 超过 4096，仅为该预览纹理缩小到 "+std::to_string(w)+"x"+std::to_string(h));
    }
    return copy;
}
struct Placement {std::string key;osg::ref_ptr<osg::Image> image;unsigned x=0,y=0,w=0,h=0,page=0;};
struct Page {unsigned x=0,y=0,shelf=0,usedW=0,usedH=0;};
std::vector<Placement> packTextures(std::map<std::string,osg::ref_ptr<osg::Image>>& images,std::vector<Page>& pages) {
    std::vector<Placement> result;for(auto& [key,image]:images) result.push_back({key,image});
    std::sort(result.begin(),result.end(),[](const Placement& a,const Placement& b) {return std::make_tuple(a.image->t(),a.image->s(),a.key)>std::make_tuple(b.image->t(),b.image->s(),b.key);});
    for(auto& item:result) {
        item.w=item.image->s();item.h=item.image->t();bool placed=false;
        for(unsigned pi=0;pi<pages.size() && !placed;++pi) {
            auto& page=pages[pi];
            if(item.w+gutter*2>atlasLimit || item.h+gutter*2>atlasLimit) continue;
            if(page.x+item.w+gutter*2>atlasLimit) {page.y+=page.shelf;page.x=0;page.shelf=0;}
            if(page.y+item.h+gutter*2>atlasLimit) continue;
            item.x=page.x+gutter;item.y=page.y+gutter;item.page=pi;
            page.x+=item.w+gutter*2;page.shelf=std::max(page.shelf,item.h+gutter*2);
            page.usedW=std::max(page.usedW,page.x);page.usedH=std::max(page.usedH,page.y+page.shelf);placed=true;
        }
        if(!placed) {
            Page page;pages.push_back(page);item.x=gutter;item.y=gutter;item.page=pages.size()-1;
            auto& current=pages.back();current.x=item.w+gutter*2;current.shelf=item.h+gutter*2;current.usedW=current.x;current.usedH=current.shelf;
        }
    }
    return result;
}
osg::ref_ptr<osg::Image> makeAtlas(const Page& page,const std::vector<Placement>& placements,unsigned index) {
    unsigned width=std::max(4U,(page.usedW+3U)&~3U),height=std::max(4U,(page.usedH+3U)&~3U);
    auto atlas=new osg::Image;atlas->allocateImage(width,height,1,GL_RGBA,GL_UNSIGNED_BYTE,1);atlas->setOrigin(osg::Image::BOTTOM_LEFT);
    std::fill_n(atlas->data(),static_cast<std::size_t>(width)*height*4,0);
    for(const auto& placement:placements) if(placement.page==index) {
        const auto& source=*placement.image;
        for(int y=-int(gutter);y<int(placement.h+gutter);++y) for(int x=-int(gutter);x<int(placement.w+gutter);++x) {
            int sx=std::clamp(x,0,int(placement.w)-1),sy=std::clamp(y,0,int(placement.h)-1);
            std::copy_n(source.data(sx,sy),4,atlas->data(int(placement.x)+x,int(placement.y)+y));
        }
    }
    return atlas;
}
struct MaterialPlan {std::string group;int placement=-1;std::string textureKey;};
struct Group {std::vector<const PreviewFace*> faces;unsigned material=0;};

Json writeBlock(const PreviewMesh& mesh,const fs::path& output,unsigned serial,const Options& requested,
                Json& warnings,std::set<std::string>& textureFiles) {
    std::map<std::string,osg::ref_ptr<osg::Image>> atlasSources;std::map<std::string,osg::ref_ptr<osg::Image>> prepared;
    std::vector<MaterialPlan> plans(mesh.materials.size());
    auto prepare=[&](const osg::ref_ptr<osg::Image>& image) {
        auto key=imageKey(*image);auto found=prepared.find(key);if(found==prepared.end()) found=prepared.emplace(key,pageImage(image,warnings)).first;return std::make_pair(key,found->second);
    };
    for(unsigned mi=0;mi<mesh.materials.size();++mi) {
        const auto& material=mesh.materials[mi];auto& plan=plans[mi];
        std::string signature=stateSignature(material.state.get());
        if(!material.image || !material.texture) {plan.group="plain|"+signature;continue;}
        auto [key,image]=prepare(material.image);plan.textureKey=key;
        bool uvClamped=clamped(*material.texture);
        for(const auto& face:mesh.faces) if(face.material==mi) for(const auto& uv:face.uv)
            uvClamped=uvClamped && std::isfinite(uv.x()) && std::isfinite(uv.y()) && uv.x()>=0 && uv.x()<=1 && uv.y()>=0 && uv.y()<=1;
        if(uvClamped) {
            atlasSources.emplace(key,image);plan.group="atlas:"+key+"|"+signature;
        } else {
            plan.group="direct:"+key+"|"+samplerSignature(material.texture.get())+"|"+signature;
            warnings.push_back("纹理 "+material.image->getFileName()+" 使用重复/镜像或越界 UV，保留独立采样页以避免图集串色");
        }
    }
    std::vector<Page> pages;auto placements=packTextures(atlasSources,pages);
    std::map<std::string,unsigned> placementByKey;for(unsigned i=0;i<placements.size();++i) placementByKey[placements[i].key]=i;
    for(auto& plan:plans) if(plan.group.rfind("atlas:",0)==0) plan.placement=placementByKey.at(plan.textureKey);
    // 同一图集页、同一渲染状态的材质合并为一个 primitive。
    for(auto& plan:plans) if(plan.placement>=0) plan.group="page:"+std::to_string(placements[plan.placement].page)+plan.group.substr(plan.group.find('|'));
    std::map<std::string,Group> groups;
    for(const auto& face:mesh.faces) {
        auto& plan=plans[face.material];auto [it,inserted]=groups.emplace(plan.group,Group{});
        if(inserted) it->second.material=face.material;it->second.faces.push_back(&face);
    }
    std::vector<osg::ref_ptr<osg::Image>> atlasImages;std::vector<osg::ref_ptr<osg::Texture2D>> atlasTextures;
    for(unsigned pi=0;pi<pages.size();++pi) {
        auto image=makeAtlas(pages[pi],placements,pi);atlasImages.push_back(image);
        auto texture=new osg::Texture2D(image);texture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
        texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);atlasTextures.push_back(texture);
    }
    std::map<std::string,osg::ref_ptr<osg::Image>> directImages;
    for(auto& plan:plans) if(plan.placement<0 && !plan.textureKey.empty()) directImages.emplace(plan.textureKey,prepared.at(plan.textureKey));
    std::map<std::string,osg::ref_ptr<osg::Texture2D>> directTextures;
    for(unsigned mi=0;mi<plans.size();++mi) if(plans[mi].placement<0 && !plans[mi].textureKey.empty()) {
        const auto& source=mesh.materials[mi];auto key=plans[mi].group;if(directTextures.count(key)) continue;
        auto texture=new osg::Texture2D(directImages.at(plans[mi].textureKey));
        texture->setWrap(osg::Texture::WRAP_S,source.texture->getWrap(osg::Texture::WRAP_S));texture->setWrap(osg::Texture::WRAP_T,source.texture->getWrap(osg::Texture::WRAP_T));
        texture->setFilter(osg::Texture::MIN_FILTER,source.texture->getFilter(osg::Texture::MIN_FILTER));texture->setFilter(osg::Texture::MAG_FILTER,source.texture->getFilter(osg::Texture::MAG_FILTER));
        directTextures[key]=texture;
    }
    using Key=std::tuple<double,double,double,double,double,double,double,double,double>;
    std::vector<GeometryInput> geometries;
    for(const auto& [name,group]:groups) {
        auto geometry=new osg::Geometry;auto positions=new osg::Vec3dArray;auto uv=new osg::Vec2Array;auto colors=new osg::Vec4Array;auto indices=new osg::DrawElementsUInt(GL_TRIANGLES);
        std::map<Key,unsigned> remap;
        for(const auto* face:group.faces) for(unsigned corner=0;corner<3;++corner) {
            UV coordinate=face->uv[corner];unsigned materialIndex=face->material;const auto& plan=plans[materialIndex];
            if(plan.placement>=0) {
                const auto& placement=placements[plan.placement];const auto& page=atlasImages[placement.page];
                coordinate=UV((placement.x+coordinate.x()*placement.w)/page->s(),(placement.y+coordinate.y()*placement.h)/page->t());
            }
            const auto& p=face->positions[corner];const auto& c=face->colors[corner];
            Key key={p.x(),p.y(),p.z(),coordinate.x(),coordinate.y(),c.r(),c.g(),c.b(),c.a()};
            auto [it,inserted]=remap.emplace(key,positions->size());
            if(inserted) {positions->push_back(p);uv->push_back(osg::Vec2(coordinate));colors->push_back(osg::Vec4(c));}
            indices->push_back(it->second);
        }
        const auto& representative=mesh.materials[group.material];const auto& plan=plans[group.material];
        osg::ref_ptr<osg::StateSet> state=representative.state?new osg::StateSet(*representative.state,osg::CopyOp::SHALLOW_COPY):new osg::StateSet;
        state->removeTextureAttribute(0,osg::StateAttribute::TEXMAT);
        if(plan.placement>=0) state->setTextureAttributeAndModes(0,atlasTextures[plan.placement>=0?placements[plan.placement].page:0],osg::StateAttribute::ON);
        else if(!plan.textureKey.empty()) state->setTextureAttributeAndModes(0,directTextures.at(plan.group),osg::StateAttribute::ON);
        geometry->setVertexArray(positions);geometry->setTexCoordArray(0,uv);if(mesh.hasColors) geometry->setColorArray(colors,osg::Array::BIND_PER_VERTEX);
        geometry->addPrimitiveSet(indices);geometries.push_back({geometry,state,osg::Matrixd::identity()});
    }
    Options options=requested;options.preserveEncoded=false;options.compressGeometry=true;options.dracoGeometry=false;options.quantizeGeometry=false;options.textureFormat="uastc";options.preview=true;options.previewEncoding=true;
    auto name="preview-"+std::to_string(serial)+".glb";
    auto content=writeGlb(geometries,output/"content"/name,output/"textures",false,nullptr,true,false,&options);
    for(const auto& image:content.imageAudit) textureFiles.insert(image.at("uri").get<std::string>());
    Json atlasReport=Json::array();for(unsigned pi=0;pi<atlasImages.size();++pi) atlasReport.push_back({{"page",pi},{"width",atlasImages[pi]->s()},{"height",atlasImages[pi]->t()}});
    return {{"content","content/"+name},{"inputTriangles",mesh.inputTriangles},{"outputTriangles",content.triangles},{"vertices",content.vertices},
        {"bytes",content.bytes},{"primitives",geometries.size()},{"geometryRawBytes",content.geometryRawBytes},{"geometryStoredBytes",content.geometryStoredBytes},
        {"compressedViews",content.compressedViews},{"coordinateRoundingErrorMeters",content.coordinateRoundingError},
        {"atlasPages",atlasReport},{"standaloneTextures",directImages.size()},{"images",content.imageAudit},
        {"bounds",{content.bounds.xMin(),content.bounds.yMin(),content.bounds.zMin(),content.bounds.xMax(),content.bounds.yMax(),content.bounds.zMax()}}};
}
}

Json buildPreview(const std::vector<PreviewSource>& sources,const Options& options,
                  const SpatialReference* spatial,const osg::Matrixd& placement) {
    if(sources.empty()) throw std::runtime_error("预览: 显示前沿为空");
    auto started=std::chrono::steady_clock::now();std::uint64_t inputTriangles=0;
    for(const auto& source:sources) inputTriangles+=source.inputTriangles;
    std::size_t blockCount=inputTriangles<=defaultTriangleBudget?1:std::min<std::size_t>(maximumSpatialBlocks,(inputTriangles+defaultTriangleBudget-1)/defaultTriangleBudget);
    blockCount=std::min(blockCount,sources.size());auto groups=spatialGroups(sources,blockCount);
    fs::create_directories(options.output/"content");fs::create_directories(options.output/"textures");
    Json blocks=Json::array(),children=Json::array(),warnings=Json::array();std::set<std::string> textureFiles;
    osg::BoundingBoxd bounds;std::uint64_t outputTriangles=0,bytes=0,vertices=0,atlasPages=0,standaloneTextures=0,processed=0;
    for(unsigned blockIndex=0;blockIndex<groups.size();++blockIndex) {
        if(options.checkCancelled) options.checkCancelled();
        const auto& group=groups[blockIndex];std::uint64_t groupInput=0;for(auto index:group) groupInput+=sources[index].inputTriangles;
        PreviewMesh combined;std::uint64_t remainingBudget=groupInput,remainingInput=groupInput;
        for(std::size_t position=0;position<group.size();++position) {
            if(options.checkCancelled) options.checkCancelled();
            const auto& source=sources[group[position]];
            std::size_t target=position+1==group.size()?remainingBudget:std::max<std::uint64_t>(1,remainingBudget*source.inputTriangles/std::max<std::uint64_t>(1,remainingInput));
            target=std::max<std::size_t>(1,std::min<std::size_t>(target,source.inputTriangles));
            osg::ref_ptr<osgDB::Options> readOptions=new osgDB::Options;readOptions->setObjectCacheHint(osgDB::Options::CACHE_NONE);
            auto node=osgDB::readRefNodeFile(source.file.u8string(),readOptions.get());if(!node) throw std::runtime_error("预览: 源读取失败: "+source.file.u8string());
            std::vector<GeometryInput> meshes;collectResident(node.get(),source.matrix,nullptr,meshes);
            auto simplified=simplifyPreview(meshes,target,options,spatial);
            unsigned materialOffset=combined.materials.size();combined.hasColors|=simplified.hasColors;combined.inputTriangles+=simplified.inputTriangles;
            for(auto& material:simplified.materials) combined.materials.push_back(std::move(material));
            for(auto& face:simplified.faces) {face.material+=materialOffset;combined.faces.push_back(std::move(face));}
            remainingBudget=remainingBudget>target?remainingBudget-target:0;remainingInput=remainingInput>source.inputTriangles?remainingInput-source.inputTriangles:0;
            ++processed;if(options.event) options.event({{"type","progress"},{"stage","preview"},{"done",processed},{"total",sources.size()},{"unit","files"}});
        }
        auto block=writeBlock(combined,options.output,blockIndex,options,warnings,textureFiles);
        auto blockBounds=block.at("bounds").get<std::vector<double>>();osg::BoundingBoxd box(blockBounds[0],blockBounds[1],blockBounds[2],blockBounds[3],blockBounds[4],blockBounds[5]);
        bounds.expandBy(box);outputTriangles+=block.at("outputTriangles").get<std::uint64_t>();bytes+=block.at("bytes").get<std::uint64_t>();vertices+=block.at("vertices").get<std::uint64_t>();
        atlasPages+=block.at("atlasPages").size();standaloneTextures+=block.at("standaloneTextures").get<std::uint64_t>();
        children.push_back({{"boundingVolume",boundingBoxJson(box)},{"geometricError",0.0},{"refine","REPLACE"},{"content",{{"uri",block.at("content")}}}});
        block.erase("bounds");blocks.push_back(std::move(block));
    }
    double extent=std::max({bounds.xMax()-bounds.xMin(),bounds.yMax()-bounds.yMin(),bounds.zMax()-bounds.zMin()});
    // Cesium 在根节点 geometricError<=0 时认定无需细化并跳过整棵子树，
    // 因此预览根必须使用正误差；浅树叶片再用 0，不复制原深层 LOD。
    double rootError=std::max(children.size()==1?extent:extent*2.,1.0);
    Json root={{"boundingVolume",boundingBoxJson(bounds)},{"transform",std::vector<double>(placement.ptr(),placement.ptr()+16)},{"geometricError",rootError},{"refine","REPLACE"}};
    if(children.size()==1) root["content"]=children[0].at("content");else root["children"]=std::move(children);
    atomicJson(options.output/"tileset.json",{{"asset",{{"version","1.1"},{"generator","osgb-tiles-native mesh-preview-v1"}}},{"geometricError",rootError},{"root",root}});
    std::uint64_t textureBytes=0;for(const auto& name:textureFiles) textureBytes+=fs::file_size(options.output/fs::u8path(name));
    Json report={{"status","complete"},{"entry","preview/tileset.json"},{"algorithm","mesh-preview-v1"},{"inputTriangles",inputTriangles},{"triangles",outputTriangles},
        {"vertices",vertices},{"glbs",blocks.size()},{"geometryEncoding","EXT_meshopt_compression"},{"textureFormat","uastc"},
        {"triangleBudget",defaultTriangleBudget},{"spatialBlockLimit",maximumSpatialBlocks},{"atlasPageLimit",atlasLimit},
        {"atlasPages",atlasPages},{"standaloneTextures",standaloneTextures},{"uniqueTextureFiles",textureFiles.size()},
        {"glbBytes",bytes},{"textureBytes",textureBytes},{"elapsedSeconds",std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()},
        {"positionType","FLOAT"},{"uvType","FLOAT"},{"normals","omitted"},{"material","KHR_materials_unlit"},{"mipChain","complete"},{"alpha","preserved"},
        {"budgetReached",outputTriangles<=inputTriangles},{"blocks",blocks},{"warnings",warnings}};
    atomicJson(options.output/"preview-report.json",report);return report;
}
}
