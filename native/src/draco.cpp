#include "codecs.hpp"
#include <draco/compression/expert_encode.h>
#include <draco/compression/decode.h>
#include <draco/compression/attributes/normal_compression_utils.h>
#include <draco/mesh/mesh.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
namespace nt {
namespace {
unsigned width(const Json& a) {auto type=a.at("type").get<std::string>();return type=="SCALAR"?1:type=="VEC2"?2:type=="VEC3"?3:type=="VEC4"?4:0;}
std::vector<float> values(const Json& d,const Bytes& b,unsigned id) {
    const auto& a=d.at("accessors").at(id);const auto& view=d.at("bufferViews").at(a.at("bufferView").get<unsigned>());
    if(view.contains("extensions") || view.at("buffer")!=0) throw std::runtime_error("Draco input must be uncompressed");
    unsigned w=width(a),type=a.at("componentType").get<unsigned>(),size=type==5126||type==5125?4:type==5123?2:1;
    auto count=a.at("count").get<std::size_t>();auto offset=view.value("byteOffset",0ULL)+a.value("byteOffset",0ULL);auto stride=view.value("byteStride",w*size);
    std::vector<float> result;for(std::size_t i=0;i<count;++i) for(unsigned k=0;k<w;++k) {
        std::size_t start=offset+i*stride+k*size;if(start+size>b.size()) throw std::runtime_error("Draco source accessor out of range");
        if(type==5126) {float v;std::memcpy(&v,b.data()+start,4);result.push_back(v);}else {unsigned v=0;std::memcpy(&v,b.data()+start,size);result.push_back(static_cast<float>(v));}
    }
    return result;
}
std::vector<unsigned> indexValues(const Json& d,const Bytes& b,unsigned id) {
    const auto& a=d.at("accessors").at(id);const auto& v=d.at("bufferViews").at(a.at("bufferView").get<unsigned>());
    auto type=a.at("componentType").get<unsigned>();unsigned size=type==5125?4:type==5123?2:type==5121?1:0;
    if(!size || a.at("type")!="SCALAR" || v.contains("extensions") || v.at("buffer")!=0) throw std::runtime_error("Draco source index type invalid");
    auto offset=v.value("byteOffset",0ULL)+a.value("byteOffset",0ULL);auto stride=v.value("byteStride",size);std::vector<unsigned> result;
    for(std::size_t i=0;i<a.at("count").get<std::size_t>();++i) {
        auto start=offset+i*stride;if(start>b.size() || size>b.size()-start) throw std::runtime_error("Draco source index range invalid");
        unsigned value=0;std::memcpy(&value,b.data()+start,size);result.push_back(value);
    }
    return result;
}
struct Attribute {std::string name;unsigned accessor=0,w=0,bits=0;int id=0;std::vector<float> data,min;float range=0;};
draco::GeometryAttribute::Type type(const std::string& name) {
    return name=="POSITION"?draco::GeometryAttribute::POSITION:name=="NORMAL"?draco::GeometryAttribute::NORMAL:name=="TEXCOORD_0"?draco::GeometryAttribute::TEX_COORD:name=="COLOR_0"?draco::GeometryAttribute::COLOR:draco::GeometryAttribute::GENERIC;
}
std::vector<Attribute> attributes(const Json& doc,const Bytes& bin,const Json& primitive) {
    std::vector<Attribute> out;
    for(auto it=primitive.at("attributes").begin();it!=primitive.at("attributes").end();++it) {
        Attribute a;a.name=it.key();a.accessor=it.value().get<unsigned>();a.w=width(doc.at("accessors").at(a.accessor));
        a.bits=a.name=="POSITION"?14:a.name=="NORMAL"?10:a.name=="COLOR_0"?8:12;
        a.data=values(doc,bin,a.accessor);a.min.assign(a.w,std::numeric_limits<float>::infinity());std::vector<float> maximum(a.w,-std::numeric_limits<float>::infinity());
        for(unsigned i=0;i<a.data.size();++i) {a.min[i%a.w]=std::min(a.min[i%a.w],a.data[i]);maximum[i%a.w]=std::max(maximum[i%a.w],a.data[i]);}
        for(unsigned k=0;k<a.w;++k) a.range=std::max(a.range,maximum[k]-a.min[k]);if(a.range==0) a.range=1;
        out.push_back(std::move(a));
    }
    return out;
}
std::string pointKey(const std::vector<Attribute>& attrs,unsigned point,const draco::Mesh* decoded=nullptr) {
    std::ostringstream key;
    for(const auto& a:attrs) {
        std::vector<float> value(a.w);if(decoded) {
            auto att=decoded->GetAttributeByUniqueId(a.id);
            if(!att || !att->ConvertValue<float>(att->mapped_index(draco::PointIndex(point)),a.w,value.data())) throw std::runtime_error("Draco attribute decode failed");
        } else std::copy_n(a.data.data()+point*a.w,a.w,value.data());
        // The same float32 transform used by Draco, evaluated at decoded grid
        // centers for output. This allows face-order/cyclic-start changes only.
        key<<a.name<<':';
        if(a.name=="NORMAL") {
            draco::OctahedronToolBox oct;oct.SetQuantizationBits(a.bits);std::int32_t s,t;oct.FloatVectorToQuantizedOctahedralCoords(value.data(),&s,&t);
            key<<s<<','<<t<<';';continue;
        }
        float scale=static_cast<float>((1U<<a.bits)-1)/a.range;
        for(unsigned k=0;k<a.w;++k) key<<static_cast<long long>(std::floor((value[k]-a.min[k])*scale+.5f))<<',';
    }
    return key.str();
}
std::string faceKey(const std::array<std::string,3>& p) {
    std::string best;for(unsigned k=0;k<3;++k) {auto s=p[k]+"|"+p[(k+1)%3]+"|"+p[(k+2)%3];if(k==0 || s<best) best=s;}return best;
}
std::unique_ptr<draco::Mesh> decode(const Bytes& data) {
    draco::Decoder decoder;draco::DecoderBuffer buffer;buffer.Init(reinterpret_cast<const char*>(data.data()),data.size());
    auto result=decoder.DecodeMeshFromBuffer(&buffer);if(!result.ok()) throw std::runtime_error("Draco stream decode failed: "+result.status().error_msg_string());return std::move(result).value();
}
bool coverage(const std::vector<Attribute>& attrs,const std::vector<unsigned>& indices,unsigned count,const draco::Mesh& decoded) {
    if(decoded.num_points()!=count || decoded.num_faces()*3!=indices.size()) return false;
    std::multiset<std::string> before,after,pointsBefore,pointsAfter;
    std::vector<std::string> sourceKeys(count),decodedKeys(count);
    for(unsigned i=0;i<count;++i) {sourceKeys[i]=pointKey(attrs,i);decodedKeys[i]=pointKey(attrs,i,&decoded);pointsBefore.insert(sourceKeys[i]);pointsAfter.insert(decodedKeys[i]);}
    if(pointsBefore!=pointsAfter) return false;
    for(unsigned i=0;i<indices.size();i+=3) before.insert(faceKey({sourceKeys[indices[i]],sourceKeys[indices[i+1]],sourceKeys[indices[i+2]]}));
    for(unsigned i=0;i<decoded.num_faces();++i) {auto f=decoded.face(draco::FaceIndex(i));after.insert(faceKey({decodedKeys[f[0].value()],decodedKeys[f[1].value()],decodedKeys[f[2].value()]}));}
    return before==after;
}
}
Json compressDraco(Json& doc,Bytes& bin,unsigned speed) {
    Json sourceDoc=doc;Bytes source=bin,output;Json newViews=Json::array(),newAccessors=Json::array(),audit=Json::array();double maxError=0;osg::BoundingBoxd bounds;
    // Stream reuse cache. Local error metrics are content-only and reused;
    // maxError/bounds depend on each node's transform and are always recomputed.
    struct CachedStream {unsigned view;std::shared_ptr<draco::Mesh> decoded;bool fallback;double localError,uvError,normalError;};
    std::map<std::uint64_t,CachedStream> streamCache;
    if(speed>10) throw std::runtime_error("Draco speed out of range");
    unsigned meshIndex=0;
    for(auto& meshDoc:doc["meshes"]) {
        std::vector<osg::Matrixd> transforms;for(const auto& node:doc["nodes"]) if(node.contains("mesh") && node["mesh"]==meshIndex) {auto matrix=node.at("matrix").get<std::vector<double>>();transforms.emplace_back(matrix.data());}
        for(auto& primitive:meshDoc["primitives"]) {
            if(primitive.value("mode",4)!=4) throw std::runtime_error("Draco vertex compression currently requires triangle primitives");
            auto attrs=attributes(sourceDoc,source,primitive);unsigned count=sourceDoc["accessors"][primitive["attributes"]["POSITION"].get<unsigned>()]["count"].get<unsigned>();
            // Identical primitives (instanced geometry) share one Draco stream:
            // the encoded bytes and the decoded mesh are reused verbatim, so the
            // point/face coverage is unchanged and only the file gets smaller.
            auto fingerprint=[&] {
                std::uint64_t h=1469598103934665603ull;
                auto mix=[&](const void* data,std::size_t size) {const auto* p=static_cast<const unsigned char*>(data);for(std::size_t i=0;i<size;++i) {h^=p[i];h*=1099511628211ull;}};
                for(const auto& a:attrs) {mix(a.name.data(),a.name.size());mix(&a.bits,sizeof(a.bits));mix(a.data.data(),a.data.size()*sizeof(float));}
                return h;
            };
            draco::Mesh mesh;mesh.set_num_points(count);
            for(auto& a:attrs) {
                if(a.data.size()!=static_cast<std::size_t>(count)*a.w) throw std::runtime_error("Draco attribute count mismatch");
                draco::GeometryAttribute descriptor;descriptor.Init(type(a.name),nullptr,a.w,draco::DT_FLOAT32,false,a.w*sizeof(float),0);
                a.id=mesh.AddAttribute(descriptor,true,count);auto att=mesh.attribute(a.id);
                for(unsigned i=0;i<count;++i) att->SetAttributeValue(draco::AttributeValueIndex(i),a.data.data()+i*a.w);
            }
            auto indices=indexValues(sourceDoc,source,primitive["indices"].get<unsigned>());std::set<unsigned> used;
            auto key=fingerprint();for(auto i:indices) {key=key*1099511628211ull^(i+1);}
            for(auto i:indices) {if(i>=count) throw std::runtime_error("Draco index invalid");used.insert(i);}
            bool special=used.size()!=count;std::set<std::string> points;
            for(unsigned i=0;i<count;++i) if(!points.insert(pointKey(attrs,i)).second) special=true;
            for(unsigned i=0;i<indices.size();i+=3) {
                draco::Mesh::Face f;for(unsigned k=0;k<3;++k) f[k]=draco::PointIndex(indices.at(i+k));mesh.AddFace(f);
                auto& p=*std::find_if(attrs.begin(),attrs.end(),[](const Attribute& a){return a.name=="POSITION";});
                osg::Vec3d a(p.data[indices[i]*3],p.data[indices[i]*3+1],p.data[indices[i]*3+2]),b(p.data[indices[i+1]*3],p.data[indices[i+1]*3+1],p.data[indices[i+1]*3+2]),c(p.data[indices[i+2]*3],p.data[indices[i+2]*3+1],p.data[indices[i+2]*3+2]);
                if(((b-a)^(c-a)).length2()==0) special=true;
            }
            auto encode=[&](bool sequential) {
                draco::ExpertEncoder encoder(mesh);encoder.SetSpeedOptions(speed,speed);
                encoder.SetEncodingMethod(sequential?draco::MESH_SEQUENTIAL_ENCODING:draco::MESH_EDGEBREAKER_ENCODING);
                for(auto& a:attrs) encoder.SetAttributeQuantization(a.id,a.bits);
                draco::EncoderBuffer buffer;auto status=encoder.EncodeToBuffer(&buffer);if(!status.ok()) throw std::runtime_error("Draco encoding failed: "+status.error_msg_string());
                return Bytes(buffer.data(),buffer.data()+buffer.size());
            };
            unsigned view=0;std::shared_ptr<draco::Mesh> decoded;bool fallback=false,reused=false;
            auto cached=streamCache.find(key);
            if(cached!=streamCache.end()) {
                view=cached->second.view;decoded=cached->second.decoded;reused=true;fallback=cached->second.fallback;
            } else {
                auto bytes=encode(special);decoded=std::shared_ptr<draco::Mesh>(decode(bytes));
                if(!coverage(attrs,indices,count,*decoded) && !special) {special=true;fallback=true;bytes=encode(true);decoded=std::shared_ptr<draco::Mesh>(decode(bytes));}
                if(!coverage(attrs,indices,count,*decoded)) throw std::runtime_error("Draco quantized directed face/point coverage mismatch");
                while(output.size()%4) output.push_back(0);view=newViews.size();newViews.push_back({{"buffer",0},{"byteOffset",output.size()},{"byteLength",bytes.size()}});output.insert(output.end(),bytes.begin(),bytes.end());
                streamCache.emplace(key,CachedStream{view,decoded,fallback,0,0,0});
            }
            // Match each decoded point against the original quantized key and
            // report actual position/UV error; normals remain explicitly lossy.
            // Error metrics are recomputed even on stream reuse (cheap compared
            // to encoding) because world error depends on each node's transform.
            std::map<std::string,std::vector<unsigned>> original;for(unsigned i=0;i<count;++i) original[pointKey(attrs,i)].push_back(i);
            double localError=0,uvError=0,normalError=0;
            for(unsigned i=0;i<count;++i) {
                auto key2=pointKey(attrs,i,decoded.get());auto& matches=original.at(key2);unsigned sourceIndex=matches.front();
                for(const auto& a:attrs) {
                    std::vector<float> value(a.w);auto att=decoded->GetAttributeByUniqueId(a.id);att->ConvertValue<float>(att->mapped_index(draco::PointIndex(i)),a.w,value.data());
                    if(a.name=="POSITION") {
                        osg::Vec3d p(a.data[sourceIndex*3],a.data[sourceIndex*3+1],a.data[sourceIndex*3+2]),d(value[0],value[1],value[2]);localError=std::max(localError,(d-p).length());
                        for(auto match:matches) {
                            osg::Vec3d originalPosition(a.data[match*3],a.data[match*3+1],a.data[match*3+2]);
                            localError=std::max(localError,(d-originalPosition).length());
                            for(const auto& transform:transforms) maxError=std::max(maxError,((d*transform)-(originalPosition*transform)).length());
                        }
                        for(auto& transform:transforms) {auto world=d*transform;bounds.expandBy(osg::Vec3d(world.x(),-world.z(),world.y()));
                        }
                    } else if(a.name=="TEXCOORD_0") {
                        for(auto match:matches) for(unsigned k=0;k<a.w;++k)
                            uvError=std::max(uvError,std::abs(double(value[k])-a.data[match*a.w+k]));
                    }
                    else if(a.name=="NORMAL") {
                        double nearest=std::numeric_limits<double>::infinity();
                        for(auto match:matches) {double error=0;for(unsigned k=0;k<3;++k) error+=std::pow(double(value[k])-a.data[match*3+k],2);nearest=std::min(nearest,std::sqrt(error));}
                        normalError=std::max(normalError,nearest);
                    }
                }
            }
            if(normalError>.025) throw std::runtime_error("Draco normal angular component coverage mismatch");
            Json map=Json::object(),quant=Json::object();
            for(const auto& a:attrs) {
                Json accessor=sourceDoc["accessors"][a.accessor];accessor.erase("bufferView");accessor.erase("byteOffset");accessor["count"]=decoded->num_points();
                if(a.name=="POSITION") {
                    std::vector<float> low(3,std::numeric_limits<float>::infinity()),high(3,-std::numeric_limits<float>::infinity());auto att=decoded->GetAttributeByUniqueId(a.id);
                    for(unsigned i=0;i<count;++i) {float p[3];att->ConvertValue<float>(att->mapped_index(draco::PointIndex(i)),3,p);for(unsigned k=0;k<3;++k) {low[k]=std::min(low[k],p[k]);high[k]=std::max(high[k],p[k]);}}
                    accessor["min"]=low;accessor["max"]=high;
                }
                primitive["attributes"][a.name]=newAccessors.size();newAccessors.push_back(accessor);map[a.name]=a.id;quant[a.name]={{"bits",a.bits},{"range",a.range},{"halfStep",a.range/(2.*((1U<<a.bits)-1))}};
            }
            Json index={{"componentType",decoded->num_points()<65535?5123:5125},{"count",indices.size()},{"type","SCALAR"}};
            primitive["indices"]=newAccessors.size();newAccessors.push_back(index);primitive["extensions"]["KHR_draco_mesh_compression"]={{"bufferView",view},{"attributes",map}};
            audit.push_back({{"points",count},{"triangles",indices.size()/3},{"method",special?"sequential":"edgebreaker"},{"streamReused",reused},{"coverageFallback",fallback},
                {"directedFaceAndPointCoverage",true},{"quantization",quant},{"maxLocalPositionError",localError},{"maxUvError",uvError},{"maxNormalVectorError",normalError}});
        }
        ++meshIndex;
    }
    doc["bufferViews"]=newViews;doc["accessors"]=newAccessors;bin=std::move(output);
    for(auto name:{"extensionsUsed","extensionsRequired"}) {if(!doc.contains(name)) doc[name]=Json::array();doc[name].push_back("KHR_draco_mesh_compression");}
    Json result={{"lossy",true},{"speed",speed},{"positionBits",14},{"uvBits",12},{"maxWorldPositionErrorMeters",maxError},{"primitives",audit}};
    if(bounds.valid()) result["bounds"]={bounds.xMin(),bounds.yMin(),bounds.zMin(),bounds.xMax(),bounds.yMax(),bounds.zMax()};return result;
}
std::set<unsigned> validateDraco(const Json& doc,const Bytes& binary) {
    std::set<unsigned> covered;
    for(const auto& mesh:doc.at("meshes")) for(const auto& primitive:mesh.at("primitives")) {
        if(!primitive.contains("extensions") || !primitive["extensions"].contains("KHR_draco_mesh_compression")) continue;
        const auto& extension=primitive["extensions"]["KHR_draco_mesh_compression"];const auto& view=doc.at("bufferViews").at(extension.at("bufferView").get<unsigned>());
        auto offset=view.value("byteOffset",0ULL);auto size=view.at("byteLength").get<std::size_t>();
        if(view.at("buffer")!=0 || offset>binary.size() || size>binary.size()-offset) throw std::runtime_error("Draco buffer view range");
        auto meshDecoded=decode(Bytes(binary.begin()+offset,binary.begin()+offset+size));
        auto index=primitive.at("indices").get<unsigned>();const auto& a=doc.at("accessors").at(index);
        if(meshDecoded->num_faces()*3!=a.at("count").get<unsigned>()) throw std::runtime_error("Draco decoded index count mismatch");covered.insert(index);
        for(auto it=primitive.at("attributes").begin();it!=primitive.at("attributes").end();++it) {
            unsigned id=it.value().get<unsigned>();auto& accessor=doc.at("accessors").at(id);auto attribute=meshDecoded->GetAttributeByUniqueId(extension.at("attributes").at(it.key()).get<unsigned>());
            if(!attribute || attribute->num_components()!=width(accessor) || accessor.at("count").get<unsigned>()!=meshDecoded->num_points()) throw std::runtime_error("Draco decoded attribute mismatch");
            if(accessor.value("byteOffset",0U)!=0 || accessor.contains("bufferView") || accessor.value("normalized",false)) throw std::runtime_error("Draco accessor must have no fallback view/offset/normalization");
            for(unsigned point=0;point<meshDecoded->num_points();++point) {
                float value[4];if(!attribute->ConvertValue<float>(attribute->mapped_index(draco::PointIndex(point)),width(accessor),value)) throw std::runtime_error("Draco attribute unreadable");
                for(unsigned k=0;k<width(accessor);++k) {
                    if(!std::isfinite(value[k])) throw std::runtime_error("Draco attribute is nonfinite");
                    if(it.key()=="POSITION" && (accessor.at("min").size()!=3 || accessor.at("max").size()!=3 || value[k]<accessor.at("min").at(k).get<double>() || value[k]>accessor.at("max").at(k).get<double>())) throw std::runtime_error("Draco POSITION bounds mismatch");
                }
            }
            covered.insert(id);
        }
    }
    return covered;
}
}
