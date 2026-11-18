#include "native_tiles.hpp"
#include <CGAL/Simple_cartesian.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_processing/distance.h>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace nt;
using Kernel=CGAL::Simple_cartesian<double>;
using Point=Kernel::Point_3;
using Mesh=CGAL::Surface_mesh<Point>;
namespace {
struct Content {
    std::string uri,source;
};
struct Model {
    fs::path root;
    Json tileset;
    std::map<std::string,std::string> source;
};
Json document(const fs::path& file,Bytes& bytes) {
    bytes=readBytes(file);
    if(bytes.size()<28) throw std::runtime_error("GLB 过短");
    std::uint32_t length=0;std::memcpy(&length,bytes.data()+12,4);
    if(length+28>bytes.size()) throw std::runtime_error("GLB JSON 越界");
    auto doc=Json::parse(bytes.begin()+20,bytes.begin()+20+length);
    bytes=Bytes(bytes.begin()+28+length,bytes.end());
    return doc;
}
Bytes view(const Json& doc,const Bytes& bytes,unsigned index) {
    const auto& info=doc["bufferViews"][index];
    std::size_t offset=info.value("byteOffset",0),length=info.at("byteLength").get<std::size_t>();
    if(info.contains("extensions")) {
        const auto& ext=info["extensions"]["EXT_meshopt_compression"];
        offset=ext.value("byteOffset",0);
        return decodeMeshopt(Bytes(bytes.begin()+offset,bytes.begin()+offset+ext["byteLength"].get<std::size_t>()),
            ext["count"].get<std::size_t>(),ext["byteStride"].get<std::size_t>(),
            info.value("target",0)==34963 || ext["mode"]=="TRIANGLES" || ext["mode"]=="INDICES");
    }
    return Bytes(bytes.begin()+offset,bytes.begin()+offset+length);
}
template<class T> std::vector<T> values(const Json& doc,const Bytes& bytes,unsigned index) {
    const auto& accessor=doc["accessors"][index];const auto& meta=doc["bufferViews"][accessor["bufferView"].get<unsigned>()];
    auto data=view(doc,bytes,accessor["bufferView"].get<unsigned>());
    const unsigned components=accessor["type"]=="VEC2"?2:accessor["type"]=="VEC3"?3:1;
    const std::size_t width=components*sizeof(T),stride=meta.value("byteStride",width),offset=accessor.value("byteOffset",0);
    if(offset+stride*(accessor["count"].get<std::size_t>()-1)+width>data.size()) throw std::runtime_error("属性越界");
    std::vector<T> out(accessor["count"].get<std::size_t>()*components);
    for(std::size_t i=0;i<accessor["count"].get<std::size_t>();++i) std::memcpy(out.data()+i*components,data.data()+offset+i*stride,width);
    return out;
}
void append(Mesh& mesh,const Model& model,const std::string& uri) {
    Bytes bytes;auto doc=document(model.root/uri,bytes);
    std::vector<std::size_t> mapping;
    for(unsigned nodeIndex=0;nodeIndex<doc["nodes"].size();++nodeIndex) {
        const auto& node=doc["nodes"][nodeIndex];if(!node.contains("mesh")) continue;
        auto m=node.value("matrix",std::vector<double>{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1});
        const auto transform=[&](const std::vector<float>& p,unsigned index) {
            double x=p[index*3],y=p[index*3+1],z=p[index*3+2];
            double a=x*m[0]+y*m[4]+z*m[8]+m[12],b=x*m[1]+y*m[5]+z*m[9]+m[13],c=x*m[2]+y*m[6]+z*m[10]+m[14];
            return Point(a,-c,b);
        };
        for(const auto& primitive:doc["meshes"][node["mesh"].get<unsigned>()]["primitives"]) {
            auto positions=values<float>(doc,bytes,primitive["attributes"]["POSITION"].get<unsigned>());
            std::vector<std::uint32_t> indexes;
            const auto& accessor=doc["accessors"][primitive["indices"].get<unsigned>()];
            if(accessor["componentType"]==5123) {
                auto v=values<std::uint16_t>(doc,bytes,primitive["indices"].get<unsigned>());indexes.assign(v.begin(),v.end());
            } else indexes=values<std::uint32_t>(doc,bytes,primitive["indices"].get<unsigned>());
            if(primitive.value("mode",4)!=4) continue;
            for(std::size_t i=0;i<indexes.size();i+=3) {
                std::vector<Mesh::Vertex_index> face;
                for(unsigned j=0;j<3;++j) face.push_back(mesh.add_vertex(transform(positions,indexes[i+j])));
                mesh.add_face(face);
            }
        }
    }
}
}
int main(int argc,char** argv) {
    try {
        if(argc<3) {std::cerr<<"用法: measure_lod_error MODEL_DIR SOURCE_BASENAME...\n";return 2;}
        Model model;model.root=fs::canonical(argv[1]);model.tileset=Json::parse(readBytes(model.root/"tileset.json"));
        auto report=Json::parse(readBytes(model.root/"conversion-report.json"));
        for(const auto& record:report["records"]) if(!record["content"].empty()) model.source[record["content"][0]["uri"]]=record["source"];
        for(int arg=2;arg<argc;++arg) {
            std::string match=argv[arg];const Json* target=nullptr;
            auto locate=[&](auto&& self,const Json& node)->void {
                if(target) return;
                if(node.contains("content") && model.source[node["content"]["uri"]].find(match)!=std::string::npos) {target=&node;return;}
                if(node.contains("children")) for(const auto& child:node["children"]) self(self,child);
            };
            locate(locate,model.tileset["root"]);
            if(!target) throw std::runtime_error("未找到源节点: "+match);
            const auto& node=*target;
            Mesh coarse,fine,immediate;
            auto source=model.source[node["content"]["uri"]];append(coarse,model,node["content"]["uri"]);
            std::function<void(const Json&)> terminals=[&](const Json& child) {
                if(child.contains("children")&&!child["children"].empty()) {
                    for(const auto& next:child["children"]) terminals(next);
                } else if(child.contains("content")) append(fine,model,child["content"]["uri"]);
            };
            terminals(node);
            if(node.contains("children")) for(const auto& child:node["children"]) if(child.contains("content")) append(immediate,model,child["content"]["uri"]);
            if(coarse.is_empty() || fine.is_empty() || immediate.is_empty()) throw std::runtime_error("误差输入为空: "+match);
            double errorBound=1e-5;
            auto all=CGAL::Polygon_mesh_processing::bounded_error_symmetric_Hausdorff_distance<CGAL::Sequential_tag>(coarse,fine,errorBound);
            auto direct=CGAL::Polygon_mesh_processing::bounded_error_symmetric_Hausdorff_distance<CGAL::Sequential_tag>(coarse,immediate,errorBound);
            std::cout<<Json({{"source",source},{"currentError",node["geometricError"]},
                {"coarseTriangles",coarse.number_of_faces()},{"fineTriangles",fine.number_of_faces()},
                {"directTriangles",immediate.number_of_faces()},{"boundedError",errorBound},
                {"directHausdorff",direct},{"terminalHausdorff",all},
                {"directUpperBound",direct+errorBound},{"terminalUpperBound",all+errorBound}}).dump()<<'\n';
        }
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
