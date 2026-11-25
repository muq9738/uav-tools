#pragma once
#include <osg/Array>
#include <osg/Material>
#include <osg/StateSet>
#include <osg/Texture>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace nt {
// Source attributes use the same component conversion in GLB export and atlas rebuilding.
inline double attributeComponent(const osg::Array& array,std::size_t offset) {
    const void* data=array.getDataPointer();
    switch(array.getDataType()) {
        case GL_FLOAT: return static_cast<const float*>(data)[offset];
        case GL_DOUBLE: return static_cast<const double*>(data)[offset];
        case GL_UNSIGNED_BYTE: return static_cast<const unsigned char*>(data)[offset]/255.0;
        case GL_UNSIGNED_SHORT: return static_cast<const unsigned short*>(data)[offset]/65535.0;
        default: throw std::runtime_error("不支持的属性元素类型");
    }
}

inline const osg::Texture* activeBaseColorTexture(const osg::StateSet* state) {
    if(!state || !(state->getTextureMode(0,GL_TEXTURE_2D)&osg::StateAttribute::ON)) return nullptr;
    return dynamic_cast<const osg::Texture*>(state->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
}

inline osg::Vec4d sampleSourceTexture(const osg::Texture& texture,double u,double v) {
    const auto& image=*texture.getImage(0);
    auto axis=[](double uv,int size,osg::Texture::WrapMode mode) {
        if (mode==osg::Texture::REPEAT) uv-=std::floor(uv);
        else if (mode==osg::Texture::MIRROR) { uv-=2*std::floor(uv/2); uv=std::min(uv,2-uv); }
        else uv=std::clamp(uv,0.,1.);
        double x=uv*size-.5;
        int i=static_cast<int>(std::floor(x));
        auto index=[&](int j) { return mode==osg::Texture::REPEAT ? (j+size)%size : std::clamp(j,0,size-1); };
        struct Sample { int a,b; double t; };
        return Sample{index(i),index(i+1),x-i};
    };
    auto x=axis(u,image.s(),texture.getWrap(osg::Texture::WRAP_S));
    auto y=axis(v,image.t(),texture.getWrap(osg::Texture::WRAP_T));
    auto pixel=[&](int i,int j){return osg::Vec4d(image.getColor(i,j,0));};
    return (pixel(x.a,y.a)*(1-x.t)+pixel(x.b,y.a)*x.t)*(1-y.t)+
           (pixel(x.a,y.b)*(1-x.t)+pixel(x.b,y.b)*x.t)*y.t;
}

inline const osg::Vec4* diffuseColor(const osg::StateSet* state) {
    if(!state) return nullptr;
    auto material=dynamic_cast<const osg::Material*>(state->getAttribute(osg::StateAttribute::MATERIAL));
    return material?&material->getDiffuse(osg::Material::FRONT):nullptr;
}
}
