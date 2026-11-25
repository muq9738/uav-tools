#include "orthophoto.hpp"
#include <tiffio.h>
#include <cstdint>
#include <cstring>

namespace nt {
void writeOrthophotoTiff(const fs::path& file,int width,int height,double west,double north,
                         double resolution,int epsg,const OrthoRowProducer& produce) {
    // Reserve room for Deflate expansion and strip directories in classic TIFF.
    const char* mode=std::uint64_t(width)*height >= (1ULL<<29) ? "w8" : "w";
#ifdef _WIN32
    auto handle=TIFFOpenW(file.c_str(),mode);
#else
    auto handle=TIFFOpen(file.c_str(),mode);
#endif
    std::unique_ptr<TIFF,decltype(&TIFFClose)> tif(handle,TIFFClose);
    if (!tif) throw std::runtime_error("Cannot create orthophoto TIFF");
    auto set=[&](uint32_t tag,auto... values) {
        if (!TIFFSetField(tif.get(),tag,values...)) throw std::runtime_error("Cannot write TIFF tag "+std::to_string(tag));
    };
    set(TIFFTAG_IMAGEWIDTH,width);
    set(TIFFTAG_IMAGELENGTH,height);
    set(TIFFTAG_SAMPLESPERPIXEL,4);
    set(TIFFTAG_BITSPERSAMPLE,8);
    set(TIFFTAG_PHOTOMETRIC,PHOTOMETRIC_RGB);
    set(TIFFTAG_PLANARCONFIG,PLANARCONFIG_CONTIG);
    set(TIFFTAG_ORIENTATION,ORIENTATION_TOPLEFT);
    uint16_t alpha=EXTRASAMPLE_UNASSALPHA;
    set(TIFFTAG_EXTRASAMPLES,1,&alpha);
    set(TIFFTAG_COMPRESSION,COMPRESSION_ADOBE_DEFLATE);
    set(TIFFTAG_PREDICTOR,PREDICTOR_HORIZONTAL);
    const auto rowsPerStrip=TIFFDefaultStripSize(tif.get(),0);
    set(TIFFTAG_ROWSPERSTRIP,rowsPerStrip);

    // OGC GeoTIFF 1.1 tags. The tiepoint is the outer corner of pixel (0,0).
    constexpr uint32_t scaleTag=33550,tieTag=33922,keysTag=34735;
    const TIFFFieldInfo fields[]={
        {scaleTag,-1,-1,TIFF_DOUBLE,FIELD_CUSTOM,1,1,const_cast<char*>("ModelPixelScaleTag")},
        {tieTag,-1,-1,TIFF_DOUBLE,FIELD_CUSTOM,1,1,const_cast<char*>("ModelTiepointTag")},
        {keysTag,-1,-1,TIFF_SHORT,FIELD_CUSTOM,1,1,const_cast<char*>("GeoKeyDirectoryTag")}
    };
    if (TIFFMergeFieldInfo(tif.get(),fields,3)<0) throw std::runtime_error("Cannot register GeoTIFF tags");
    const double scale[]={resolution,resolution,0},tie[]={0,0,0,west,north,0};
    set(scaleTag,3,scale);
    set(tieTag,6,tie);
    if (epsg) {
        const uint16_t keys[]={1,1,1,3, 1024,0,1,1, 1025,0,1,1, 3072,0,1,static_cast<uint16_t>(epsg)};
        set(keysTag,16,keys);
    }
    uint32_t row=0;
    // Wide rasters have independent, one-row Deflate streams. Reuse the exact
    // bytes produced by this TIFF codec for consecutive identical scanlines.
    Bytes previous,encoded;
    const auto rowBytes=std::size_t(width)*4;
    bool reuseRows=rowsPerStrip==1 && rowBytes<=(1U<<20);
    produce([&](unsigned char* bytes) {
        if (reuseRows && !previous.empty() && !std::memcmp(previous.data(),bytes,rowBytes)) {
            if (encoded.empty()) {
                if (!TIFFFlushData(tif.get())) throw std::runtime_error("Cannot flush orthophoto strip");
                const auto size=TIFFGetStrileByteCount(tif.get(),row-1);
                if (size && size<=rowBytes+rowBytes/2+256) {
                    encoded.resize(size);
                    if (TIFFReadRawStrip(tif.get(),row-1,encoded.data(),size)!=static_cast<tmsize_t>(size)) encoded.clear();
                }
                reuseRows=!encoded.empty();
            }
            if (reuseRows) {
                if (TIFFWriteRawStrip(tif.get(),row++,encoded.data(),encoded.size())!=static_cast<tmsize_t>(encoded.size()))
                    throw std::runtime_error("Cannot write repeated orthophoto strip");
                return;
            }
        } else if (reuseRows) {
            previous.assign(bytes,bytes+rowBytes);
            encoded.clear();
        }
        if (TIFFWriteScanline(tif.get(),bytes,row++,0)<0) throw std::runtime_error("Cannot write orthophoto scanline");
    });
    if (row!=static_cast<uint32_t>(height) || !TIFFWriteDirectory(tif.get())) throw std::runtime_error("Cannot finish orthophoto TIFF");
}
}
