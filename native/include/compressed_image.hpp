#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace osg { class Image; }

namespace nt::experimental {
// Experimental CPU policy, not a GPU-equivalence claim. Never used by encodeImage.
inline constexpr const char* bcCandidatePolicy = "rgb565-bit-replicate-floor-v1";
enum class BcFormat : std::uint32_t {
    RgbDxt1=0x83F0, RgbaDxt1=0x83F1, RgbaDxt3=0x83F2, RgbaDxt5=0x83F3,
    SrgbDxt1=0x8C4C, SrgbAlphaDxt1=0x8C4D, SrgbAlphaDxt3=0x8C4E, SrgbAlphaDxt5=0x8C4F
};
enum class ColorEncoding { Linear, Srgb };
enum class BcAlpha { Opaque, PunchThrough, Explicit4, Interpolated8 };
enum class ImageOrigin { BottomLeft, TopLeft };
struct BcFormatInfo {
    BcFormat format;
    const char* name;
    std::size_t blockBytes;
    ColorEncoding colorEncoding;
    BcAlpha alpha;
};
struct BcLevel {
    std::uint32_t width, height;
    std::size_t sourceOffset, sourceBytes;
    std::vector<std::uint8_t> rgba;
};
struct BcImage {
    BcFormatInfo format;
    ImageOrigin origin;
    std::vector<BcLevel> levels;
};
// Unknown enums, invalid dimensions/layouts and overflow throw std::runtime_error.
BcFormatInfo bcFormatInfo(std::uint32_t glFormat);
std::size_t bcLevelByteSize(std::uint32_t glFormat, std::uint32_t width, std::uint32_t height);
// dataSize is the actual readable length, not a size inferred from dimensions.
// mipOffsets excludes base offset 0. All supplied levels must be tightly packed;
// partial chains are allowed, but trailing bytes and levels after 1x1 are not.
// Rows and encoded sRGB values stay in source order; alpha is never premultiplied.
BcImage decodeBcCandidate(std::uint32_t glFormat, std::uint32_t width, std::uint32_t height,
                          const std::uint8_t* data, std::size_t dataSize,
                          const std::vector<std::size_t>& mipOffsets={},
                          ImageOrigin origin=ImageOrigin::BottomLeft);
// Explicit OSG adapter implemented in image.cpp. The caller supplies an independently
// known allocation length: osg::Image's computed size is not a capacity check.
BcImage decodeOsgBcCandidate(const osg::Image& image, std::size_t sourceByteLength);
}
