#include "native_tiles.hpp"
#include "codecs.hpp"
#include "product_metadata.hpp"
#include <osg/AlphaFunc>
#include <osg/Material>
#include <osg/Texture2D>
#include <osg/TexMat>
#include <osg/TexEnv>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>

namespace nt {
namespace {
void appendU32(Bytes& out,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) out.push_back(static_cast<unsigned char>(value>>(i*8)));
}
std::vector<float> arrayValues(const osg::Array* a,unsigned width,unsigned count,unsigned primitive) {
    if(!a) return {};
    if(a->getDataSize()!=static_cast<int>(width)) throw std::runtime_error("数组维数不符合 glTF 属性: "+std::string(a->className()));
    auto available=a->getNumElements();auto binding=a->getBinding();
    bool overall=binding==osg::Array::BIND_OVERALL;
    bool perSet=binding==osg::Array::BIND_PER_PRIMITIVE_SET;
    if((!overall&&!perSet&&available!=count) || (overall&&available<1) || (perSet&&available<=primitive))
        throw std::runtime_error("属性数量与顶点或 binding 不匹配: "+std::string(a->className()));
    std::vector<float> result(static_cast<std::size_t>(width)*count);
    for(unsigned i=0;i<count;++i) for(unsigned c=0;c<width;++c) {
        auto offset=static_cast<std::size_t>(overall?0:(perSet?primitive:i))*width+c;
        double value=0;
        switch(a->getDataType()) {
            case GL_FLOAT:value=static_cast<const float*>(a->getDataPointer())[offset];break;
            case GL_DOUBLE:value=static_cast<const double*>(a->getDataPointer())[offset];break;
            case GL_UNSIGNED_BYTE:value=static_cast<const unsigned char*>(a->getDataPointer())[offset]/255.0;break;
            case GL_UNSIGNED_SHORT:value=static_cast<const unsigned short*>(a->getDataPointer())[offset]/65535.0;break;
            default:throw std::runtime_error("不支持的属性元素类型");
        }
        float converted=static_cast<float>(value);
        if(!std::isfinite(value)) throw std::runtime_error("几何包含非有限数值");
        if(a->getDataType()==GL_DOUBLE && static_cast<double>(converted)!=value)
            throw std::runtime_error("双精度属性不能无损表示为 glTF FLOAT；拒绝静默量化");
        result[static_cast<std::size_t>(i)*width+c]=converted;
    }
    return result;
}
std::vector<double> positionValues(const osg::Array& array) {
    if(array.getDataSize()!=3 || (array.getDataType()!=GL_FLOAT && array.getDataType()!=GL_DOUBLE))
        throw std::runtime_error("位置必须是 Vec3/Vec3d 数组");
    std::vector<double> values(static_cast<std::size_t>(array.getNumElements())*3);
    for(std::size_t i=0;i<values.size();++i) {
        values[i]=array.getDataType()==GL_DOUBLE?static_cast<const double*>(array.getDataPointer())[i]:static_cast<const float*>(array.getDataPointer())[i];
        if(!std::isfinite(values[i])) throw std::runtime_error("位置包含非有限数值");
    }
    return values;
}
int wrapMode(osg::Texture::WrapMode value) {
    if(value==osg::Texture::CLAMP) return GL_CLAMP_TO_EDGE;
    if(value==osg::Texture::REPEAT || value==osg::Texture::MIRROR || value==osg::Texture::CLAMP_TO_EDGE) return value;
    throw std::runtime_error("glTF 不支持该纹理边界采样，拒绝更改 wrap");
}
struct Writer {
    Json doc={{"asset",{{"version","2.0"},{"generator",kAssetGenerator}}},
              {"scene",0},{"scenes",Json::array({{{"nodes",Json::array()}}})},
              {"nodes",Json::array()},{"meshes",Json::array()},{"materials",Json::array()},
              {"accessors",Json::array()},{"bufferViews",Json::array()},
              {"images",Json::array()},{"textures",Json::array()},{"samplers",Json::array()},
              {"extensionsUsed",Json::array({"KHR_materials_unlit"})}};
    Bytes binary;
    ContentResult result;
    fs::path textureDir;
    bool preserve;
    bool compressGeometry=false;
    bool quantizeGeometry=false;
    bool embedTextures=false;
    bool preview=false;
    unsigned previewThreads=1;
    unsigned previewMaxTextureSize=0;
    unsigned previewJpegQuality=80;
    std::string textureFormat="original";
    std::size_t virtualBufferLength=0;
    const SpatialReference* spatial=nullptr;
    std::map<std::vector<double>,unsigned> meshesByMatrix;
    std::map<const osg::Texture*,std::pair<int,bool>> textures;
    std::map<std::string,int> embeddedImageIds;
    void requireExtension(const char* name) {
        for(auto key:{"extensionsUsed","extensionsRequired"}) {
            if(!doc.contains(key)) doc[key]=Json::array();
            if(std::find(doc[key].begin(),doc[key].end(),name)==doc[key].end()) doc[key].push_back(name);
        }
    }
    int bufferView(const void* data,std::size_t length,int target,std::size_t stride,std::size_t originalLength=0,bool triangles=false) {
        while(binary.size()%4) binary.push_back(0);
        int index=doc["bufferViews"].size();bool isIndex=target==34963;
        Bytes encoded;
        if(compressGeometry && (length>=512 || preview)) encoded=encodeMeshopt(data,length,stride,isIndex,triangles);
        // 主产物小视图保留原始表示；预览契约固定 Meshopt，即使小夹具也实际编码。
        bool compressed=!encoded.empty() && (preview || encoded.size()+(result.compressedViews?192:512)<length);
        // 解码后的虚拟范围也必须互不重叠；Cesium 索引缓存使用该偏移和 accessor 形状作为键。
        auto virtualOffset=(virtualBufferLength+3)&~std::size_t(3);
        Json view={{"buffer",compressed?1:0},{"byteOffset",compressed?virtualOffset:binary.size()},{"byteLength",length}};
        if(target) view["target"]=target;
        if(compressed) {
            view["extensions"]["EXT_meshopt_compression"]={{"buffer",0},{"byteOffset",binary.size()},
                {"byteLength",encoded.size()},{"byteStride",stride},{"count",length/stride},
                {"mode",isIndex?(triangles?"TRIANGLES":"INDICES"):"ATTRIBUTES"},{"filter","NONE"}};
            virtualBufferLength=virtualOffset+length;requireExtension("EXT_meshopt_compression");
            binary.insert(binary.end(),encoded.begin(),encoded.end());result.compressedViews++;
        } else {
            auto b=static_cast<const unsigned char*>(data);binary.insert(binary.end(),b,b+length);
        }
        result.geometryRawBytes+=length;result.geometryStoredBytes+=compressed?encoded.size():length;
        result.originalFloatBytes+=originalLength?originalLength:length;
        doc["bufferViews"].push_back(view);return index;
    }
    int floats(const std::vector<float>& values,unsigned width,bool bounds=false) {
        if(values.empty() || values.size()%width) throw std::runtime_error("空或不完整的 glTF 属性");
        int view=bufferView(values.data(),values.size()*sizeof(float),34962,width*sizeof(float));
        Json accessor={{"bufferView",view},{"componentType",5126},{"count",values.size()/width},
                       {"type",width==1?"SCALAR":"VEC"+std::to_string(width)}};
        if(bounds) {
            std::vector<double> min(width,std::numeric_limits<double>::infinity()),max(width,-std::numeric_limits<double>::infinity());
            for(std::size_t i=0;i<values.size();++i) {min[i%width]=std::min(min[i%width],double(values[i]));max[i%width]=std::max(max[i%width],double(values[i]));}
            accessor["min"]=min;accessor["max"]=max;
        }
        int index=doc["accessors"].size();doc["accessors"].push_back(accessor);return index;
    }
    int normalizedShorts(const std::vector<std::uint16_t>& values,unsigned width,bool bounds=false) {
        if(values.empty() || values.size()%width) throw std::runtime_error("空或不完整的归一化属性");
        auto packed=values;
        std::size_t stride=width*sizeof(std::uint16_t);
        if(width==3) {
            // Padding must not retain unrelated source components: it is encoded too.
            packed.assign(values.size()/3*4,0);
            for(std::size_t i=0;i<values.size()/3;++i) std::copy_n(values.data()+i*3,3,packed.data()+i*4);
            stride=8;
        }
        int view=bufferView(packed.data(),packed.size()*sizeof(std::uint16_t),34962,stride,values.size()*sizeof(float));
        if(stride!=width*sizeof(std::uint16_t)) doc["bufferViews"][view]["byteStride"]=stride;
        Json accessor={{"bufferView",view},{"componentType",5123},{"count",values.size()/width},
                       {"type",width==1?"SCALAR":"VEC"+std::to_string(width)},{"normalized",true}};
        if(bounds) {
            std::vector<std::uint16_t> min(width,65535),max(width,0);
            for(std::size_t i=0;i<values.size();++i) {min[i%width]=std::min(min[i%width],values[i]);max[i%width]=std::max(max[i%width],values[i]);}
            accessor["min"]=min;accessor["max"]=max;
        }
        requireExtension("KHR_mesh_quantization");
        int index=doc["accessors"].size();doc["accessors"].push_back(accessor);return index;
    }
    int indices(const std::vector<std::uint32_t>& values,unsigned vertices,bool triangles) {
        if(values.empty()) throw std::runtime_error("空索引");
        auto max=*std::max_element(values.begin(),values.end());
        if(max>=vertices) throw std::runtime_error("索引越界");
        int type=5125,view=0;
        // glTF 保留每种索引类型的最大值用于 primitive restart；65535 必须升为 UINT。
        if(max<65535) {
            std::vector<std::uint16_t> shortValues(values.begin(),values.end());type=5123;
            view=bufferView(shortValues.data(),shortValues.size()*2,34963,2,0,triangles);
        } else view=bufferView(values.data(),values.size()*4,34963,4,0,triangles);
        int index=doc["accessors"].size();doc["accessors"].push_back({{"bufferView",view},{"componentType",type},{"count",values.size()},{"type","SCALAR"}});return index;
    }
    std::pair<int,bool> texture(const osg::Texture* t) {
        auto found=textures.find(t);if(found!=textures.end()) return found->second;
        auto image=t->getImage(0);if(!image) throw std::runtime_error("材质引用了缺失纹理");
        if(image->isMipmap())
            throw std::runtime_error("源纹理包含内嵌 mip 链，当前 JPEG/PNG 路径不能等价保留，拒绝静默丢弃: "+image->getFileName());
        auto captured=dynamic_cast<EncodedImage*>(const_cast<osg::Image*>(image)->getUserData());
        if(captured && captured->mime!="image/png" && captured->mime!="image/jpeg")
            throw std::runtime_error("已编码纹理容器不是 PNG/JPEG，尚未建立等价输出，拒绝静默转码: "+image->getFileName());
        osg::ref_ptr<EncodedImage> encoded=preserve?captured:nullptr;
        bool original=encoded.valid();Json basisAudit;
        if(textureFormat!="original") {auto basis=preview?encodePreviewBasisImage(*image,previewThreads):encodeBasisImage(*image,textureFormat);encoded=basis.encoded;basisAudit=basis.audit;original=false;}
        if(preview && textureFormat=="original" && previewMaxTextureSize>0) {
            if(auto resized=encodePreviewResizedImage(*image,previewMaxTextureSize,previewJpegQuality)) {
                if(!encoded || resized->bytes.size()<encoded->bytes.size()) {
                    encoded=resized;
                    original=false;
                }
            }
        }
        if(!encoded) encoded=encodeImage(*image);
        std::string key=contentKey(encoded->bytes);
        int imageId=-1;
        std::string auditUri;
        bool embed=preview || embedTextures;
        bool embeddedNew=false;
        if(embed) {
            auto existingImage=embeddedImageIds.find(key);
            if(existingImage!=embeddedImageIds.end()) {
                imageId=existingImage->second;
            } else {
                while(binary.size()%4) binary.push_back(0);
                int viewIndex=doc["bufferViews"].size();
                doc["bufferViews"].push_back({{"buffer",0},{"byteOffset",binary.size()},{"byteLength",encoded->bytes.size()}});
                binary.insert(binary.end(),encoded->bytes.begin(),encoded->bytes.end());
                imageId=doc["images"].size();
                doc["images"].push_back({{"bufferView",viewIndex},{"mimeType",encoded->mime}});
                embeddedImageIds[key]=imageId;
                embeddedNew=true;
            }
        } else {
            std::string name=key+(encoded->mime=="image/ktx2"?".ktx2":encoded->mime=="image/jpeg"?".jpg":".png");
            {
                static std::mutex textureMutex;std::lock_guard<std::mutex> lock(textureMutex);
                fs::path path=textureDir/name;
                if(fs::exists(path)) {if(readBytes(path)!=encoded->bytes) throw std::runtime_error("图片内容键冲突");}
                else atomicWrite(path,encoded->bytes);
            }
            imageId=doc["images"].size();doc["images"].push_back({{"uri","../textures/"+name},{"mimeType",encoded->mime}});
            auditUri="textures/"+name;
        }
        int sampler=doc["samplers"].size();
        doc["samplers"].push_back({{"magFilter",preview?GL_LINEAR:t->getFilter(osg::Texture::MAG_FILTER)},
            {"minFilter",preview?GL_LINEAR_MIPMAP_LINEAR:t->getFilter(osg::Texture::MIN_FILTER)},
            {"wrapS",wrapMode(t->getWrap(osg::Texture::WRAP_S))},{"wrapT",wrapMode(t->getWrap(osg::Texture::WRAP_T))}});
        int id=doc["textures"].size();
        if(encoded->mime=="image/ktx2") {
            requireExtension("KHR_texture_basisu");doc["textures"].push_back({{"sampler",sampler},{"extensions",{{"KHR_texture_basisu",{{"source",imageId}}}}}});
        } else doc["textures"].push_back({{"source",imageId},{"sampler",sampler}});
        if(!embed || embeddedNew) {
            result.images++;
            Json audit={{"sourceName",image->getFileName()},
                {"embedded",embed},{"originalEncoded",original},{"flipV",encoded->flipV},
                {"width",image->s()},{"height",image->t()},{"bytes",encoded->bytes.size()}};
            if(!auditUri.empty()) audit["uri"]=auditUri;
            else audit["contentKey"]=key;
            if(!basisAudit.is_null()) audit["basis"]=basisAudit;
            result.imageAudit.push_back(std::move(audit));
        }
        return textures[t]={id,encoded->flipV};
    }
    int material(const osg::StateSet* state,bool hasUv) {
        Json pbr={{"baseColorFactor",Json::array({1,1,1,1})},{"metallicFactor",0},{"roughnessFactor",1}};
        if(state) {
            auto m=dynamic_cast<const osg::Material*>(state->getAttribute(osg::StateAttribute::MATERIAL));
            if(m) {auto color=m->getDiffuse(osg::Material::FRONT);pbr["baseColorFactor"]={color.r(),color.g(),color.b(),color.a()};}
            for(unsigned unit=1;unit<state->getTextureAttributeList().size();++unit)
                if((state->getTextureMode(unit,GL_TEXTURE_2D)&osg::StateAttribute::ON) && state->getTextureAttribute(unit,osg::StateAttribute::TEXTURE))
                    throw std::runtime_error("多个启用的纹理单元尚未等价映射，拒绝丢弃材质");
            auto tex=dynamic_cast<const osg::Texture*>(state->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
            if(tex && (state->getTextureMode(0,GL_TEXTURE_2D)&osg::StateAttribute::ON)) {
                if(!hasUv) throw std::runtime_error("纹理材质缺少 UV");
                auto env=dynamic_cast<const osg::TexEnv*>(state->getTextureAttribute(0,osg::StateAttribute::TEXENV));
                if(env && env->getMode()!=osg::TexEnv::MODULATE)
                    throw std::runtime_error("非 MODULATE 纹理混合尚未等价映射，拒绝静默改色");
                auto matrix=dynamic_cast<const osg::TexMat*>(state->getTextureAttribute(0,osg::StateAttribute::TEXMAT));
                if(matrix && !matrix->getMatrix().isIdentity()) throw std::runtime_error("非单位纹理矩阵尚未支持");
                auto [id,flip]=texture(tex);Json info={{"index",id}};
                if(flip) {
                    info["extensions"]["KHR_texture_transform"]={{"offset",{0,1}},{"scale",{1,-1}}};
                    requireExtension("KHR_texture_transform");
                }
                pbr["baseColorTexture"]=info;
            }
        }
        Json material={{"pbrMetallicRoughness",pbr},{"doubleSided",!state || !(state->getMode(GL_CULL_FACE)&osg::StateAttribute::ON)},
                       {"extensions",{{"KHR_materials_unlit",Json::object()}}}};
        if(state && (state->getMode(GL_BLEND)&osg::StateAttribute::ON)) material["alphaMode"]="BLEND";
        if(state && (state->getMode(GL_ALPHA_TEST)&osg::StateAttribute::ON)) {
            auto alpha=dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC));
            if(!alpha || (alpha->getFunction()!=osg::AlphaFunc::GEQUAL && alpha->getFunction()!=osg::AlphaFunc::GREATER))
                throw std::runtime_error("不支持的 alpha test 规则");
            material["alphaMode"]="MASK";material["alphaCutoff"]=alpha->getReferenceValue();
        }
        int id=doc["materials"].size();doc["materials"].push_back(material);return id;
    }
    void geometry(const GeometryInput& input) {
        auto g=input.geometry.get();auto source=g->getVertexArray();
        if(!source || !source->getNumElements()) throw std::runtime_error("几何没有顶点");
        unsigned count=source->getNumElements();
        auto sourcePositions=positionValues(*source);std::vector<float> positions(sourcePositions.size());
        osg::Matrixd nodeMatrix=input.matrix;
        std::vector<osg::Vec3d> local;osg::BoundingBoxd centeredBounds;
        if(spatial || source->getDataType()==GL_DOUBLE) {
            local.reserve(count);
            for(unsigned i=0;i<count;++i) {
                osg::Vec3d point(sourcePositions[i*3],sourcePositions[i*3+1],sourcePositions[i*3+2]);
                if(spatial) point=spatial->toLocal(point*input.matrix);
                local.push_back(point);centeredBounds.expandBy(point);
            }
            auto center=centeredBounds.center();nodeMatrix=osg::Matrixd::translate(center);
            if(!spatial) nodeMatrix=nodeMatrix*input.matrix;
            for(unsigned i=0;i<count;++i) {
                osg::Vec3d reconstructed;
                for(unsigned c=0;c<3;++c) {positions[i*3+c]=static_cast<float>(local[i][c]-center[c]);reconstructed[c]=double(positions[i*3+c])+center[c];}
                auto exact=local[i];if(!spatial) {exact=exact*input.matrix;reconstructed=reconstructed*input.matrix;}
                result.coordinateRoundingError=std::max(result.coordinateRoundingError,(reconstructed-exact).length());
                result.bounds.expandBy(exact);result.bounds.expandBy(reconstructed);
            }
        } else {
            std::transform(sourcePositions.begin(),sourcePositions.end(),positions.begin(),[](double v){return static_cast<float>(v);});
            for(unsigned i=0;i<count;++i) result.bounds.expandBy(osg::Vec3d(sourcePositions[i*3],sourcePositions[i*3+1],sourcePositions[i*3+2])*input.matrix);
        }
        osg::Vec3d quantizedMin,quantizedRange(1,1,1);
        std::vector<std::uint16_t> quantizedPositions;
        if(quantizeGeometry) {
            quantizedMin={positions[0],positions[1],positions[2]};
            osg::Vec3d maximum=quantizedMin;
            for(unsigned i=0;i<count;++i) for(unsigned c=0;c<3;++c) {
                quantizedMin[c]=std::min(quantizedMin[c],double(positions[i*3+c]));maximum[c]=std::max(maximum[c],double(positions[i*3+c]));
            }
            quantizedRange=maximum-quantizedMin;
            for(unsigned c=0;c<3;++c) if(quantizedRange[c]==0) quantizedRange[c]=1;
            auto center=centeredBounds.valid()?centeredBounds.center():osg::Vec3d();
            nodeMatrix=osg::Matrixd::scale(quantizedRange)*osg::Matrixd::translate(center+quantizedMin)*(spatial?osg::Matrixd::identity():input.matrix);
            quantizedPositions.reserve(sourcePositions.size());
            for(unsigned i=0;i<count;++i) {
                osg::Vec3d reconstructed;
                for(unsigned c=0;c<3;++c) {
                    double ratio=(positions[i*3+c]-quantizedMin[c])/quantizedRange[c];
                    if(ratio<-1e-7 || ratio>1+1e-7) throw std::runtime_error("量化范围计算越界");
                    auto q=static_cast<std::uint16_t>(std::llround(std::clamp(ratio,0.0,1.0)*65535.0));
                    quantizedPositions.push_back(q);
                    reconstructed[c]=quantizedMin[c]+quantizedRange[c]*(q/65535.0);
                }
                osg::Vec3d encoded;
                if(spatial) {
                    auto center=centeredBounds.center();
                    encoded=reconstructed+center;
                    result.quantizationErrorMeters=std::max(result.quantizationErrorMeters,(encoded-local[i]).length());
                } else {
                    auto raw=osg::Vec3d(sourcePositions[i*3],sourcePositions[i*3+1],sourcePositions[i*3+2])*input.matrix;
                    encoded=(reconstructed+center)*input.matrix;
                    result.quantizationErrorMeters=std::max(result.quantizationErrorMeters,(encoded-raw).length());
                }
                result.bounds.expandBy(encoded);
            }
        }
        // 仅置换存储槽位：按首次访问排列所有属性，未引用顶点保留在末尾；不改绘制或角点顺序。
        std::vector<unsigned> remap(count,std::numeric_limits<unsigned>::max()),order;order.reserve(count);
        auto visit=[&](unsigned index) {
            if(index>=count) throw std::runtime_error("索引越界");
            if(remap[index]==std::numeric_limits<unsigned>::max()) {remap[index]=order.size();order.push_back(index);}
        };
        for(unsigned p=0;p<g->getNumPrimitiveSets();++p) {
            auto ps=g->getPrimitiveSet(p);
            if(auto lengths=dynamic_cast<const osg::DrawArrayLengths*>(ps)) {
                if(lengths->getFirst()<0) throw std::runtime_error("DrawArrayLengths 起点为负");
                std::uint64_t first=lengths->getFirst();
                for(auto length:*lengths) {
                    if(length<0 || first+static_cast<std::uint64_t>(length)>count) throw std::runtime_error("DrawArrayLengths 分段越界");
                    for(int i=0;i<length;++i) visit(static_cast<unsigned>(first+i));first+=length;
                }
            } else for(unsigned i=0;i<ps->getNumIndices();++i) visit(ps->index(i));
        }
        for(unsigned i=0;i<count;++i) visit(i);
        auto reorder=[&](const std::vector<float>& values,unsigned width) {
            std::vector<float> output(values.size());
            for(unsigned i=0;i<count;++i) std::copy_n(values.data()+order[i]*width,width,output.data()+i*width);
            return output;
        };
        auto reorderQuantized=[&](const std::vector<std::uint16_t>& values) {
            std::vector<std::uint16_t> output(values.size());
            for(unsigned i=0;i<count;++i) std::copy_n(values.data()+order[i]*3,3,output.data()+i*3);
            return output;
        };
        int position=quantizeGeometry?normalizedShorts(reorderQuantized(quantizedPositions),3,true):floats(reorder(positions,3),3,true);
        auto writeUv=[&](const std::vector<float>& values) {
            if(!quantizeGeometry) return floats(values,2);
            bool inRange=true;
            for(auto value:values) if(value<0 || value>1) {inRange=false;break;}
            if(!inRange) {
                if(preview) return floats(values,2);
                throw std::runtime_error("UV 超出归一化范围，拒绝量化实验");
            }
            std::vector<std::uint16_t> normalized;normalized.reserve(values.size());
            for(auto value:values) normalized.push_back(static_cast<std::uint16_t>(std::llround(value*65535.0)));
            return normalizedShorts(normalized,2);
        };
        auto uvSource=g->getTexCoordArray(0);
        // per-primitive-set 的 UV 必须逐组取值；其余绑定（含 overall 广播）共用同一访问器。
        bool perSetUv=uvSource && uvSource->getBinding()==osg::Array::BIND_PER_PRIMITIVE_SET;
        int uv=uvSource && !perSetUv?writeUv(reorder(arrayValues(uvSource,2,count,0),2)):-1;
        for(unsigned unit=1;unit<g->getNumTexCoordArrays();++unit) if(g->getTexCoordArray(unit)) throw std::runtime_error("存在未支持的额外 UV 通道");
        int mat=material(input.state.get(),uvSource!=nullptr);Json primitives=Json::array();
        for(unsigned p=0;p<g->getNumPrimitiveSets();++p) {
            auto ps=g->getPrimitiveSet(p);unsigned mode=ps->getMode();std::vector<std::vector<std::uint32_t>> runs;
            if(mode>GL_TRIANGLE_FAN) throw std::runtime_error("不支持的图元模式: "+std::to_string(mode));
            if(auto lengths=dynamic_cast<const osg::DrawArrayLengths*>(ps)) {
                if(lengths->getFirst()<0) throw std::runtime_error("DrawArrayLengths 起点为负");
                std::uint64_t first=lengths->getFirst();
                for(auto length:*lengths) {
                    if(length<0 || first+static_cast<std::uint64_t>(length)>count) throw std::runtime_error("DrawArrayLengths 分段越界");
                    std::vector<std::uint32_t> run;run.reserve(length);
                    for(int i=0;i<length;++i) run.push_back(static_cast<std::uint32_t>(first+i));
                    first+=length;runs.push_back(std::move(run));
                }
            } else {
                std::vector<std::uint32_t> run;run.reserve(ps->getNumIndices());
                for(unsigned i=0;i<ps->getNumIndices();++i) run.push_back(ps->index(i));
                runs.push_back(std::move(run));
            }
            if(mode==GL_TRIANGLE_STRIP || mode==GL_TRIANGLE_FAN) {
                for(auto& indexes:runs) {
                    std::vector<std::uint32_t> expanded;
                    for(std::size_t i=2;i<indexes.size();++i) {
                        auto a=mode==GL_TRIANGLE_FAN?indexes[0]:(i%2?indexes[i-1]:indexes[i-2]);
                        auto b=mode==GL_TRIANGLE_FAN?indexes[i-1]:(i%2?indexes[i-2]:indexes[i-1]);auto c=indexes[i];
                        if(a!=b && b!=c && a!=c) expanded.insert(expanded.end(),{a,b,c});
                    }
                    indexes=std::move(expanded);
                }
                mode=GL_TRIANGLES;
            }
            for(const auto& indexes:runs) if(mode==GL_TRIANGLES && indexes.size()%3) throw std::runtime_error("三角形索引数量不完整");
            Json attributes={{"POSITION",position}};
            int uvAccessor=perSetUv?writeUv(reorder(arrayValues(uvSource,2,count,p),2)):uv;
            if(uvAccessor>=0) attributes["TEXCOORD_0"]=uvAccessor;
            if(!preview && g->getNormalArray()) {
                auto normals=arrayValues(g->getNormalArray(),3,count,p);
                if(spatial) {
                    osg::Matrixd inverse;if(!inverse.invert(input.matrix)) throw std::runtime_error("几何矩阵不可逆");
                    osg::Matrixd inverseTranspose;inverseTranspose.transpose(inverse);
                    for(unsigned i=0;i<count;++i) {
                        auto point=osg::Vec3d(sourcePositions[i*3],sourcePositions[i*3+1],sourcePositions[i*3+2])*input.matrix;
                        auto n=osg::Matrixd::transform3x3(osg::Vec3d(normals[i*3],normals[i*3+1],normals[i*3+2]),inverseTranspose);
                        n=spatial->normalToLocal(point,n);
                        for(unsigned c=0;c<3;++c) normals[i*3+c]=static_cast<float>(n[c]);
                    }
                }
                if(quantizeGeometry) {
                    // The new node scale also affects the inverse-transpose normal matrix.
                    // Precompensate that scale so the final world normal stays unchanged.
                    for(unsigned i=0;i<count;++i) {
                        osg::Vec3d n(normals[i*3]*quantizedRange.x(),normals[i*3+1]*quantizedRange.y(),normals[i*3+2]*quantizedRange.z());
                        if(n.normalize()==0) throw std::runtime_error("量化几何包含零法线");
                        for(unsigned c=0;c<3;++c) normals[i*3+c]=static_cast<float>(n[c]);
                    }
                }
                attributes["NORMAL"]=floats(reorder(normals,3),3);
            }
            if(auto colors=g->getColorArray()) {
                unsigned width=colors->getDataSize();if(width!=3&&width!=4) throw std::runtime_error("颜色不是 RGB/RGBA");
                auto values=arrayValues(colors,width,count,p);
                for(auto v:values) if(v<0 || v>1) throw std::runtime_error("顶点颜色超出标准范围，拒绝猜测归一化");
                attributes["COLOR_0"]=floats(reorder(values,width),width);
            }
            for(const auto& indexes:runs) {
                if(indexes.empty()) continue;
                std::vector<std::uint32_t> mapped;mapped.reserve(indexes.size());
                for(auto index:indexes) mapped.push_back(remap.at(index));
                primitives.push_back({{"attributes",attributes},{"indices",indices(mapped,count,mode==GL_TRIANGLES)},{"material",mat},{"mode",mode}});
                if(mode==GL_TRIANGLES) result.triangles+=indexes.size()/3;
            }
        }
        if(primitives.empty()) throw std::runtime_error("几何没有可绘制图元");
        // 精确 Z-up → Y-up；相同坐标框架下的材质几何共享节点，保留各自 primitive 与索引顺序。
        osg::Matrixd axis(1,0,0,0, 0,0,-1,0, 0,1,0,0, 0,0,0,1);
        osg::Matrixd transform=nodeMatrix*axis;
        std::vector<double> matrix(transform.ptr(),transform.ptr()+16);
        for(auto value:matrix) if(!std::isfinite(value)) throw std::runtime_error("glTF 节点矩阵包含非有限值");
        auto existing=meshesByMatrix.find(matrix);
        if(existing!=meshesByMatrix.end()) {
            for(auto& primitive:primitives) doc["meshes"][existing->second]["primitives"].push_back(std::move(primitive));
        } else {
            unsigned meshId=doc["meshes"].size();doc["meshes"].push_back({{"primitives",primitives}});meshesByMatrix[matrix]=meshId;
            unsigned node=doc["nodes"].size();doc["nodes"].push_back({{"mesh",meshId},{"matrix",matrix}});doc["scenes"][0]["nodes"].push_back(node);
        }
        result.vertices+=count;
    }
};
}
ContentResult writeGlb(const std::vector<GeometryInput>& meshes,const fs::path& output,const fs::path& textures,bool preserve,const SpatialReference* spatial,bool compressGeometry,bool quantizeGeometry,const Options* codecOptions) {
    Writer writer;writer.textureDir=textures;writer.preserve=preserve;writer.spatial=spatial;writer.compressGeometry=compressGeometry && !(codecOptions && codecOptions->dracoGeometry);writer.quantizeGeometry=quantizeGeometry;
    if(codecOptions) {writer.textureFormat=codecOptions->textureFormat;writer.embedTextures=codecOptions->embedTextures;writer.preview=codecOptions->previewEncoding;writer.previewThreads=std::max(1U,codecOptions->threads);writer.previewMaxTextureSize=codecOptions->previewMaxTextureSize;writer.previewJpegQuality=codecOptions->previewJpegQuality;}
    if(writer.preview) writer.requireExtension("KHR_materials_unlit");
    for(const auto& mesh:meshes) writer.geometry(mesh);
    if(writer.doc["meshes"].empty()) throw std::runtime_error("拒绝输出空 GLB");
    if(codecOptions && codecOptions->dracoGeometry) {
        if(quantizeGeometry) throw std::runtime_error("Draco cannot be combined with experimental U16 quantization");
        writer.result.dracoAudit=compressDraco(writer.doc,writer.binary,codecOptions->dracoSpeed);
        writer.result.quantizationErrorMeters=writer.result.dracoAudit.at("maxWorldPositionErrorMeters").get<double>();
        writer.result.geometryStoredBytes=writer.result.dracoAudit.value("geometryStoredBytes",static_cast<std::uint64_t>(writer.binary.size()));
        writer.result.compressedViews=writer.result.dracoAudit.value("dracoViews",static_cast<std::uint64_t>(writer.doc["bufferViews"].size()));
        if(writer.result.dracoAudit.contains("bounds")) {auto b=writer.result.dracoAudit["bounds"].get<std::vector<double>>();writer.result.bounds.expandBy(osg::BoundingBoxd(b[0],b[1],b[2],b[3],b[4],b[5]));}
    }
    writer.doc["buffers"]=Json::array({{{"byteLength",writer.binary.size()}}});
    if(writer.virtualBufferLength) writer.doc["buffers"].push_back({{"byteLength",writer.virtualBufferLength},
        {"extensions",{{"EXT_meshopt_compression",{{"fallback",true}}}}}});
    for(auto key:{"images","textures","samplers"}) if(writer.doc[key].empty()) writer.doc.erase(key);
    std::string json=writer.doc.dump();while(json.size()%4) json+=' ';
    while(writer.binary.size()%4) writer.binary.push_back(0);
    const std::uint64_t size=12+8+json.size()+8+writer.binary.size();
    if(size>std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("GLB 超过 4 GiB，需更细源分块");
    Bytes result;result.reserve(size);appendU32(result,0x46546c67);appendU32(result,2);appendU32(result,size);
    appendU32(result,json.size());appendU32(result,0x4e4f534a);result.insert(result.end(),json.begin(),json.end());
    appendU32(result,writer.binary.size());appendU32(result,0x004e4942);result.insert(result.end(),writer.binary.begin(),writer.binary.end());
    atomicWrite(output,result);writer.result.bytes=result.size();return writer.result;
}
Json boundingBoxJson(const osg::BoundingBoxd& b) {
    if(!b.valid()) throw std::runtime_error("无效包围体");
    auto c=b.center();return {{"box",{c.x(),c.y(),c.z(),(b.xMax()-b.xMin())/2,0,0,0,(b.yMax()-b.yMin())/2,0,0,0,(b.zMax()-b.zMin())/2}}};
}
}
