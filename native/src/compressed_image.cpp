#include "compressed_image.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>

namespace nt::experimental {
namespace {
using Pixel = std::array<std::uint8_t,4>;
std::size_t multiply(std::size_t a,std::size_t b) {
    if(b && a>std::numeric_limits<std::size_t>::max()/b)
        throw std::runtime_error("BC candidate: size overflow");
    return a*b;
}
std::size_t add(std::size_t a,std::size_t b) {
    if(a>std::numeric_limits<std::size_t>::max()-b)
        throw std::runtime_error("BC candidate: size overflow");
    return a+b;
}
std::size_t blocks(std::uint32_t dimension) {return dimension/4+(dimension%4!=0);}
std::uint64_t littleEndian(const std::uint8_t* bytes,unsigned count) {
    std::uint64_t result=0;
    for(unsigned i=0;i<count;++i) result|=std::uint64_t(bytes[i])<<(8*i);
    return result;
}
Pixel endpoint(std::uint16_t value) {
    unsigned r=value>>11,g=(value>>5)&63,b=value&31;
    return {std::uint8_t((r<<3)|(r>>2)),std::uint8_t((g<<2)|(g>>4)),std::uint8_t((b<<3)|(b>>2)),255};
}
// Implemented from the public S3TC block equations; the 8-bit rounding policy is
// ours and is deliberately named in the API. See docs/specs/compressed-image-candidate.md.
std::array<Pixel,16> decodeBlock(const std::uint8_t* block,const BcFormatInfo& format) {
    const bool bc1=format.blockBytes==8;
    const auto* color=block+(bc1?0:8);
    auto c0=static_cast<std::uint16_t>(littleEndian(color,2));
    auto c1=static_cast<std::uint16_t>(littleEndian(color+2,2));
    std::array<Pixel,4> palette{endpoint(c0),endpoint(c1),Pixel{0,0,0,255},Pixel{0,0,0,255}};
    if(!bc1 || c0>c1) {
        for(unsigned c=0;c<3;++c) {
            palette[2][c]=(2*unsigned(palette[0][c])+palette[1][c])/3;
            palette[3][c]=(unsigned(palette[0][c])+2*palette[1][c])/3;
        }
    } else {
        for(unsigned c=0;c<3;++c) palette[2][c]=(unsigned(palette[0][c])+palette[1][c])/2;
        if(format.alpha==BcAlpha::PunchThrough) palette[3][3]=0;
    }
    std::array<std::uint8_t,8> alphas{};
    if(format.alpha==BcAlpha::Interpolated8) {
        alphas[0]=block[0];alphas[1]=block[1];
        const unsigned denominator=alphas[0]>alphas[1]?7:5;
        for(unsigned i=1;i<denominator;++i)
            alphas[i+1]=((denominator-i)*unsigned(alphas[0])+i*alphas[1])/denominator;
        if(denominator==5) {alphas[6]=0;alphas[7]=255;}
    }
    const auto codes=littleEndian(color+4,4);
    const auto alphaCodes=format.alpha==BcAlpha::Explicit4?littleEndian(block,8):
        (format.alpha==BcAlpha::Interpolated8?littleEndian(block+2,6):0);
    std::array<Pixel,16> pixels{};
    for(unsigned i=0;i<16;++i) {
        pixels[i]=palette[(codes>>(2*i))&3];
        if(format.alpha==BcAlpha::Explicit4) pixels[i][3]=((alphaCodes>>(4*i))&15)*17;
        if(format.alpha==BcAlpha::Interpolated8) pixels[i][3]=alphas[(alphaCodes>>(3*i))&7];
    }
    return pixels;
}
}
BcFormatInfo bcFormatInfo(std::uint32_t glFormat) {
    using F=BcFormat;using C=ColorEncoding;using A=BcAlpha;
    switch(glFormat) {
        case 0x83F0:return {F::RgbDxt1,"GL_COMPRESSED_RGB_S3TC_DXT1_EXT",8,C::Linear,A::Opaque};
        case 0x83F1:return {F::RgbaDxt1,"GL_COMPRESSED_RGBA_S3TC_DXT1_EXT",8,C::Linear,A::PunchThrough};
        case 0x83F2:return {F::RgbaDxt3,"GL_COMPRESSED_RGBA_S3TC_DXT3_EXT",16,C::Linear,A::Explicit4};
        case 0x83F3:return {F::RgbaDxt5,"GL_COMPRESSED_RGBA_S3TC_DXT5_EXT",16,C::Linear,A::Interpolated8};
        case 0x8C4C:return {F::SrgbDxt1,"GL_COMPRESSED_SRGB_S3TC_DXT1_EXT",8,C::Srgb,A::Opaque};
        case 0x8C4D:return {F::SrgbAlphaDxt1,"GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT",8,C::Srgb,A::PunchThrough};
        case 0x8C4E:return {F::SrgbAlphaDxt3,"GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT",16,C::Srgb,A::Explicit4};
        case 0x8C4F:return {F::SrgbAlphaDxt5,"GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT",16,C::Srgb,A::Interpolated8};
        default:throw std::runtime_error("BC candidate: unsupported GL format "+std::to_string(glFormat));
    }
}
std::size_t bcLevelByteSize(std::uint32_t glFormat,std::uint32_t width,std::uint32_t height) {
    const auto format=bcFormatInfo(glFormat);
    if(!width || !height) throw std::runtime_error("BC candidate: dimensions must be nonzero");
    return multiply(multiply(blocks(width),blocks(height)),format.blockBytes);
}
BcImage decodeBcCandidate(std::uint32_t glFormat,std::uint32_t width,std::uint32_t height,
                          const std::uint8_t* data,std::size_t dataSize,
                          const std::vector<std::size_t>& mipOffsets,ImageOrigin origin) {
    BcImage result{bcFormatInfo(glFormat),origin,{}};
    if(!data) throw std::runtime_error("BC candidate: null source data");
    if(origin!=ImageOrigin::BottomLeft && origin!=ImageOrigin::TopLeft)
        throw std::runtime_error("BC candidate: unknown image origin");
    // Validate every level before touching source memory or allocating RGBA buffers.
    std::size_t end=0,totalRgba=0;
    for(std::size_t level=0;;++level) {
        const auto size=bcLevelByteSize(glFormat,width,height);
        const auto rgbaSize=multiply(multiply(width,height),4);
        totalRgba=add(totalRgba,rgbaSize);
        if(totalRgba>std::vector<std::uint8_t>().max_size())
            throw std::runtime_error("BC candidate: decoded data exceeds vector capacity");
        const auto offset=level?mipOffsets[level-1]:0;
        if(offset!=end) throw std::runtime_error("BC candidate: mip offsets must be contiguous");
        end=add(offset,size);
        if(end>dataSize) throw std::runtime_error("BC candidate: truncated source level");
        result.levels.push_back({width,height,offset,size,{}});
        if(level==mipOffsets.size()) break;
        if(width==1 && height==1) throw std::runtime_error("BC candidate: excess mip after 1x1");
        width=std::max(std::uint32_t(1),width/2);height=std::max(std::uint32_t(1),height/2);
    }
    if(end!=dataSize) throw std::runtime_error("BC candidate: trailing source bytes or missing mip offsets");
    for(auto& level:result.levels) {
        level.rgba.resize(multiply(multiply(level.width,level.height),4));
        const auto columns=blocks(level.width),rows=blocks(level.height);
        for(std::size_t by=0;by<rows;++by) for(std::size_t bx=0;bx<columns;++bx) {
            const auto offset=level.sourceOffset+(by*columns+bx)*result.format.blockBytes;
            const auto pixels=decodeBlock(data+offset,result.format);
            for(unsigned y=0;y<4;++y) for(unsigned x=0;x<4;++x) {
                const auto px=bx*4+x,py=by*4+y;
                if(px>=level.width || py>=level.height) continue;
                const auto target=(py*level.width+px)*4;
                std::copy(pixels[y*4+x].begin(),pixels[y*4+x].end(),level.rgba.begin()+target);
            }
        }
    }
    return result;
}
}
