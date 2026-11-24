#pragma once
#include <osg/Array>
#include <osg/Material>
#include <osg/StateSet>
#include <osg/Texture>
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

inline const osg::Vec4* diffuseColor(const osg::StateSet* state) {
    if(!state) return nullptr;
    auto material=dynamic_cast<const osg::Material*>(state->getAttribute(osg::StateAttribute::MATERIAL));
    return material?&material->getDiffuse(osg::Material::FRONT):nullptr;
}
}
