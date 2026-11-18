#include "codecs.hpp"
#include <encoder/basisu_comp.h>
#include <mutex>
#include <cmath>
#include <stdexcept>
namespace nt {
namespace {
void initialize() {static std::once_flag once;std::call_once(once,[]{basisu::basisu_encoder_init(false);basist::basisu_transcoder_init();});}
unsigned aligned(unsigned value) {return std::clamp(static_cast<unsigned>(std::floor(value/4.+.5))*4,4U,4096U);}
}
Json inspectBasis(const Bytes& bytes,bool decode) {
    initialize();basist::ktx2_transcoder t;
    if(bytes.size()>std::numeric_limits<std::uint32_t>::max() || !t.init(bytes.data(),bytes.size())) throw std::runtime_error("KTX2: invalid Basis container");
    if(t.get_faces()!=1 || t.get_layers()!=0 || t.get_dfd_transfer_func()!=2) throw std::runtime_error("KTX2: expected single sRGB 2D texture");
    unsigned expected=1;for(unsigned n=std::max(t.get_width(),t.get_height());n>1;n>>=1) ++expected;
    if(t.get_levels()!=expected) throw std::runtime_error("KTX2: incomplete mip chain");
    Json levels=Json::array();if(decode && !t.start_transcoding()) throw std::runtime_error("KTX2: cannot start transcoder");
    unsigned alphaMin=255,alphaMax=0;
    for(unsigned level=0;level<t.get_levels();++level) {
        unsigned w=std::max(1U,t.get_width()>>level),h=std::max(1U,t.get_height()>>level);
        if(decode) {
            Bytes rgba(static_cast<std::size_t>(w)*h*4);
            if(!t.transcode_image_level(level,0,0,rgba.data(),w*h,basist::transcoder_texture_format::cTFRGBA32)) throw std::runtime_error("KTX2: mip transcode failed");
            if(level==0) for(unsigned i=3;i<rgba.size();i+=4) {alphaMin=std::min(alphaMin,unsigned(rgba[i]));alphaMax=std::max(alphaMax,unsigned(rgba[i]));}
        }
        levels.push_back({{"width",w},{"height",h}});
    }
    // GPU residency estimate, stated per tier instead of a single "80%" claim.
    // ETC1S stores RGB at 4bpp and a second slice for alpha (8bpp with alpha);
    // UASTC is always 8bpp. The uncompressed RGBA8 baseline is 32bpp. These are
    // format arithmetic: the real upload format is chosen by the consumer/GPU.
    const bool uastc=t.is_uastc();const bool alpha=t.get_has_alpha();
    unsigned bpp=uastc?8:(alpha?8:4);
    double base=double(t.get_width())*t.get_height()*bpp/8.;
    double rgba8=double(t.get_width())*t.get_height()*4.;
    Json gpu={{"bitsPerPixel",bpp},{"hasAlpha",alpha},
        {"baseLevelBytes",base},{"withMipChainBytes",base*4./3.},
        {"rgba8BaseLevelBytes",rgba8},{"rgba8WithMipChainBytes",rgba8*4./3.},
        {"residencyVsRgba8",base/rgba8},{"savedVsRgba8",1.-base/rgba8},
        {"tier",uastc?"uastc":(alpha?"etc1s-alpha":"etc1s-opaque")},
        {"basis","format arithmetic; consumer/GPU picks the real upload format"}};
    return {{"format",uastc?"uastc":"etc1s"},{"width",t.get_width()},{"height",t.get_height()},
        {"mipLevels",t.get_levels()},{"hasAlpha",alpha},{"sRGB",true},{"dfdColorModel",t.get_dfd_color_model()},
        {"gpuEstimate",gpu},
        {"decodedAlphaMin",alphaMin},{"decodedAlphaMax",alphaMax},{"levels",levels}};
}
BasisImage encodeBasis(const osg::Image& image,const std::string& format,bool previewQuality) {
    if(format!="uastc" && format!="etc1s") throw std::runtime_error("Unknown Basis texture format");
    initialize();encodeImage(image); // same explicit input type/mip rejection as source writer
    basisu::image source(image.s(),image.t());
    for(int y=0;y<image.t();++y) for(int x=0;x<image.s();++x) {
        auto color=image.getColor(x,y,0);auto& p=source(x,y);
        for(unsigned k=0;k<4;++k) p[k]=static_cast<unsigned char>(std::lround(std::clamp(color[k],0.f,1.f)*255));
    }
    unsigned w=aligned(image.s()),h=aligned(image.t());basisu::image resized(w,h);
    if(w==static_cast<unsigned>(image.s()) && h==static_cast<unsigned>(image.t())) resized=source;
    else if(!basisu::image_resample(source,resized,false,"lanczos3")) throw std::runtime_error("Basis resize failed");
    // One encoder worker per source conversion worker; no hidden CPU fan-out.
    basisu::job_pool pool(1);basisu::basis_compressor_params p;p.m_pJob_pool=&pool;
    p.m_source_images.push_back(resized);p.m_read_source_images=false;p.m_write_output_basis_files=false;
    p.m_status_output=false;p.m_compute_stats=false;p.m_print_stats=false;p.m_multithreading=false;
    p.m_create_ktx2_file=true;p.m_ktx2_srgb_transfer_func=true;p.m_perceptual=true;p.m_mip_srgb=true;p.m_mip_gen=true;
    p.m_uastc=format=="uastc";p.m_pack_uastc_flags=previewQuality?basisu::cPackUASTCLevelVerySlow:1;p.m_rdo_uastc=false;
    p.m_ktx2_uastc_supercompression=basist::KTX2_SS_ZSTANDARD;p.m_compression_level=previewQuality?6:2;p.m_quality_level=previewQuality?255:128;
    if(previewQuality) p.m_mip_filter="kaiser";
    basisu::basis_compressor encoder;if(!encoder.init(p) || encoder.process()!=basisu::basis_compressor::cECSuccess) throw std::runtime_error("Basis encoder failed");
    auto encoded=new EncodedImage;auto& result=encoder.get_output_ktx2_file();encoded->bytes.assign(result.begin(),result.end());encoded->mime="image/ktx2";encoded->flipV=false;
    auto audit=inspectBasis(encoded->bytes);audit["sourceWidth"]=image.s();audit["sourceHeight"]=image.t();audit["lossy"]=true;
    // Compare decoded first level against the actual resized encoder input.
    basist::ktx2_transcoder decoder;decoder.init(encoded->bytes.data(),encoded->bytes.size());decoder.start_transcoding();Bytes pixels(w*h*4);
    if(!decoder.transcode_image_level(0,0,0,pixels.data(),w*h,basist::transcoder_texture_format::cTFRGBA32)) throw std::runtime_error("Basis verification decode failed");
    unsigned maximum=0;double squared=0;for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) for(unsigned k=0;k<4;++k) {
        int delta=int(pixels[(y*w+x)*4+k])-int(resized(x,y)[k]);maximum=std::max(maximum,unsigned(std::abs(delta)));squared+=double(delta)*delta;
    }
    audit["maxChannelErrorAgainstResizedInput"]=maximum;audit["rmseAgainstResizedInput"]=std::sqrt(squared/(w*h*4));
    audit["resizeFilter"]="lanczos3";audit["uastcRdo"]=false;audit["speedLevel"]=format=="etc1s"?2:(previewQuality?4:1);
    audit["qualityProfile"]=previewQuality?"preview-uastc-very-slow":"conversion-fast";
    return {encoded,audit};
}
BasisImage encodeBasisImage(const osg::Image& image,const std::string& format) {return encodeBasis(image,format,false);}
BasisImage encodePreviewBasisImage(const osg::Image& image) {return encodeBasis(image,"uastc",true);}
}
