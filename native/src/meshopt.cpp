#include "native_tiles.hpp"
#include <meshoptimizer.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace nt {
namespace {
void validateLayout(std::size_t count,std::size_t stride,bool indices) {
    if(!count || !stride || count>std::numeric_limits<std::size_t>::max()/stride)
        throw std::runtime_error("无效 meshopt 缓冲大小");
    if(indices ? (stride!=2&&stride!=4) : (stride%4!=0||stride>256))
        throw std::runtime_error("无效 EXT_meshopt_compression 步长: "+std::to_string(stride)+" indices="+std::to_string(indices));
}
}
Bytes decodeMeshopt(const Bytes& encoded,std::size_t count,std::size_t stride,bool indices) {
    validateLayout(count,stride,indices);
    if(encoded.empty() || (indices?(encoded[0]!=0xd1 && encoded[0]!=0xe1):encoded[0]!=0xa0))
        throw std::runtime_error("位流版本不符合 EXT_meshopt_compression");
    bool triangles=indices && encoded[0]==0xe1;
    if(triangles && count%3) throw std::runtime_error("TRIANGLES 索引数量必须是 3 的倍数");
    Bytes decoded(count*stride);
    int status=triangles?meshopt_decodeIndexBuffer(decoded.data(),count,stride,encoded.data(),encoded.size()):
        indices?meshopt_decodeIndexSequence(decoded.data(),count,stride,encoded.data(),encoded.size()):
        meshopt_decodeVertexBuffer(decoded.data(),count,stride,encoded.data(),encoded.size());
    if(status) throw std::runtime_error("meshopt 解码校验失败");
    return decoded;
}
Bytes encodeMeshopt(const void* data,std::size_t length,std::size_t stride,bool indices,bool triangles) {
    if(!data || !stride || length%stride) throw std::runtime_error("不完整的 meshopt 原始缓冲");
    std::size_t count=length/stride;validateLayout(count,stride,indices);
    if(triangles && (!indices || count%3)) throw std::runtime_error("TRIANGLES 仅用于完整的三角形索引");
    const auto* source=static_cast<const unsigned char*>(data);Bytes encoded;std::size_t written=0;
    if(indices) {
        // Points/lines require exact sequence order. Triangle compression preserves
        // the oriented face sequence and can rotate a face's starting corner.
        std::vector<unsigned int> values(count);
        for(std::size_t i=0;i<count;++i) {
            if(stride==2) {std::uint16_t value;std::memcpy(&value,source+i*stride,2);values[i]=value;}
            else std::memcpy(&values[i],source+i*stride,4);
        }
        std::size_t vertices=std::size_t(*std::max_element(values.begin(),values.end()))+1;
        encoded.resize(triangles?meshopt_encodeIndexBufferBound(count,vertices):meshopt_encodeIndexSequenceBound(count,vertices));
        written=triangles?meshopt_encodeIndexBuffer(encoded.data(),encoded.size(),values.data(),count):
            meshopt_encodeIndexSequence(encoded.data(),encoded.size(),values.data(),count);
    } else {
        encoded.resize(meshopt_encodeVertexBufferBound(count,stride));
        // 按调用钉住 EXT 的 version 0；不修改进程级全局版本，允许并发编码。
        written=meshopt_encodeVertexBufferLevel(encoded.data(),encoded.size(),data,count,stride,2,0);
    }
    if(!written) throw std::runtime_error("meshopt 编码失败");encoded.resize(written);
    if(encoded[0] != (indices ? (triangles ? 0xe1 : 0xd1) : 0xa0))
        throw std::runtime_error("meshopt 编码位流与声明 mode/version 不符，拒绝发布");
    auto decoded=decodeMeshopt(encoded,count,stride,indices);
    if(triangles) {
        for(std::size_t face=0;face<count;face+=3) {
            bool same=false;
            for(unsigned rotation=0;rotation<3 && !same;++rotation) {
                same=true;
                for(unsigned corner=0;corner<3;++corner)
                    same=same && std::memcmp(source+(face+corner)*stride,decoded.data()+(face+(corner+rotation)%3)*stride,stride)==0;
            }
            if(!same) throw std::runtime_error("meshopt 改变三角形朝向、角点或面次序，拒绝发布");
        }
    } else if(std::memcmp(source,decoded.data(),length)!=0) throw std::runtime_error("meshopt 往返未逐字节一致，拒绝发布");
    return encoded;
}
}
