// Optional macOS hardware counterexample probe, deliberately outside CTest.
// Build/run commands and interpretation: docs/specs/compressed-image-candidate.md.
// Reference values come from compressed GPU texelFetch, never from a CPU decoder.
#define GL_SILENCE_DEPRECATION
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include "compressed_image.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using Json=nlohmann::json;
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
void checkGl() {
    const auto error=glGetError();
    if(error) throw std::runtime_error("GL error: "+std::to_string(error));
}
GLuint shader(GLenum kind,const char* source) {
    const auto id=glCreateShader(kind);glShaderSource(id,1,&source,nullptr);glCompileShader(id);
    GLint ok=0;glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
    if(!ok) {
        char log[2048]{};glGetShaderInfoLog(id,sizeof(log),nullptr,log);
        throw std::runtime_error(std::string("Shader compile: ")+log);
    }
    return id;
}
std::vector<float> sample(GLuint texture,GLsizei width,GLsizei height) {
    GLuint target=0,fbo=0;glGenTextures(1,&target);glBindTexture(GL_TEXTURE_2D,target);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,width,height,0,GL_RGBA,GL_FLOAT,nullptr);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    require(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"RGBA32F framebuffer incomplete");
    glViewport(0,0,width,height);glBindTexture(GL_TEXTURE_2D,texture);glDrawArrays(GL_TRIANGLES,0,3);checkGl();
    std::vector<float> values(std::size_t(width)*height*4);
    glReadPixels(0,0,width,height,GL_RGBA,GL_FLOAT,values.data());checkGl();
    glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&target);
    return values;
}
Json probe(const Json& corpus) {
    using namespace nt::experimental;
    Json result={{"renderer",reinterpret_cast<const char*>(glGetString(GL_RENDERER))},
        {"vendor",reinterpret_cast<const char*>(glGetString(GL_VENDOR))},
        {"version",reinterpret_cast<const char*>(glGetString(GL_VERSION))},
        {"framebuffer","RGBA32F; framebuffer sRGB, blend, dither disabled"},
        {"fetch","texelFetch integer coordinates, level 0; source row order"},
        {"comparison","exact float equality, no tolerance; differingSamples is per channel"},
        {"policy",bcCandidatePolicy},{"cases",Json::array()}};
    const auto vertex=shader(GL_VERTEX_SHADER,
        "#version 150\nvoid main(){vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));gl_Position=vec4(p[gl_VertexID],0,1);}");
    const auto fragment=shader(GL_FRAGMENT_SHADER,
        "#version 150\nuniform sampler2D t;out vec4 c;void main(){c=texelFetch(t,ivec2(gl_FragCoord.xy),0);}");
    const auto program=glCreateProgram();glAttachShader(program,vertex);glAttachShader(program,fragment);glLinkProgram(program);
    GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);require(linked,"Shader link failed");
    glDeleteShader(vertex);glDeleteShader(fragment);glUseProgram(program);glUniform1i(glGetUniformLocation(program,"t"),0);
    GLuint vao=0;glGenVertexArrays(1,&vao);glBindVertexArray(vao);
    glDisable(GL_BLEND);glDisable(GL_DITHER);glDisable(GL_FRAMEBUFFER_SRGB);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);
    glPixelStorei(GL_PACK_ALIGNMENT,1);glPixelStorei(GL_UNPACK_ALIGNMENT,1);glActiveTexture(GL_TEXTURE0);checkGl();
    for(const auto& test:corpus.at("cases")) {
        // This probe consumes the project's trusted --vectors output, not arbitrary files.
        const auto data=test.at("data").get<std::vector<std::uint8_t>>();
        const auto format=test.at("glFormat").get<GLuint>();
        const auto width=test.at("width").get<GLsizei>(),height=test.at("height").get<GLsizei>();
        require(width>0 && height>0 && !test.contains("mipOffsets"),"Probe supports positive base-level vectors only");
        const auto candidate=decodeBcCandidate(format,width,height,data.data(),data.size());
        const bool srgb=candidate.format.colorEncoding==ColorEncoding::Srgb;
        GLuint textures[2]{};glGenTextures(2,textures);std::vector<float> values[2];
        for(unsigned i=0;i<2;++i) {
            glBindTexture(GL_TEXTURE_2D,textures[i]);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            if(i==0) glCompressedTexImage2D(GL_TEXTURE_2D,0,format,width,height,0,data.size(),data.data());
            else glTexImage2D(GL_TEXTURE_2D,0,srgb?GL_SRGB8_ALPHA8:GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,candidate.levels[0].rgba.data());
            checkGl();
            GLint storedFormat=0,compressed=0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&storedFormat);
            glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_COMPRESSED,&compressed);
            require(storedFormat==static_cast<GLint>(i==0?format:(srgb?GL_SRGB8_ALPHA8:GL_RGBA8)) && (compressed!=0)==(i==0),"Unexpected GPU texture format");
            checkGl();values[i]=sample(textures[i],width,height);
        }
        Json item={{"name",test.at("name")},{"glFormat",format},{"colorEncoding",srgb?"srgb":"linear"},
            {"differingSamples",0},{"maxAbsoluteDifference",0.0},{"firstDifference",nullptr},{"largestDifference",nullptr}};
        std::size_t differences=0;double maximum=0;
        for(std::size_t i=0;i<values[0].size();++i) {
            require(std::isfinite(values[0][i]) && std::isfinite(values[1][i]),"Nonfinite GPU sample");
            const double delta=std::abs(double(values[0][i])-values[1][i]);
            if(delta==0) continue;
            ++differences;
            Json detail={{"x",(i/4)%width},{"y",(i/4)/width},{"channel",i%4},
                {"compressedSample",values[0][i]},{"candidateSample",values[1][i]},
                {"candidateEncodedByte",candidate.levels[0].rgba[i]}};
            // For sRGB RGB channels the float value is linearized, so it is not
            // comparable with the stored byte. Do not label it as an encoded byte.
            if(!srgb || i%4==3) detail["compressedUnorm8Rounded"]=std::lround(values[0][i]*255);
            if(item["firstDifference"].is_null()) item["firstDifference"]=detail;
            if(delta>maximum) {maximum=delta;item["largestDifference"]=detail;}
        }
        item["differingSamples"]=differences;item["maxAbsoluteDifference"]=maximum;
        result["cases"].push_back(item);glDeleteTextures(2,textures);
    }
    glDeleteVertexArrays(1,&vao);glDeleteProgram(program);checkGl();return result;
}
}
int main(int argc,char** argv) {
    CGLContextObj context=nullptr;
    try {
        require(argc==2,"usage: bc-candidate-gpu-probe oracle-vectors.json");
        const CGLPixelFormatAttribute attributes[]={kCGLPFAOpenGLProfile,static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_3_2_Core),kCGLPFAAccelerated,static_cast<CGLPixelFormatAttribute>(0)};
        CGLPixelFormatObj pixel=nullptr;GLint count=0;
        require(CGLChoosePixelFormat(attributes,&pixel,&count)==kCGLNoError && pixel,"No accelerated CGL pixel format");
        const auto status=CGLCreateContext(pixel,nullptr,&context);CGLDestroyPixelFormat(pixel);
        require(status==kCGLNoError,"CGL context creation failed");require(CGLSetCurrentContext(context)==kCGLNoError,"CGL context activation failed");
        std::ifstream input(argv[1]);Json corpus;input>>corpus;std::cout<<probe(corpus).dump(2)<<'\n';
        CGLSetCurrentContext(nullptr);CGLDestroyContext(context);return 0;
    } catch(const std::exception& error) {
        if(context) {CGLSetCurrentContext(nullptr);CGLDestroyContext(context);}
        std::cerr<<error.what()<<'\n';return 1;
    }
}
