#pragma once
#include "native_tiles.hpp"
#include <set>
namespace nt {
struct BasisImage {osg::ref_ptr<EncodedImage> encoded;Json audit;};
BasisImage encodeBasisImage(const osg::Image& image,const std::string& format);
BasisImage encodePreviewBasisImage(const osg::Image& image);
Json inspectBasis(const Bytes& bytes,bool decodeAllLevels=true);
Json compressDraco(Json& document,Bytes& binary,unsigned speed);
std::set<unsigned> validateDraco(const Json& document,const Bytes& binary);
}
