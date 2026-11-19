set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
# 仅构建 Release，避免在 Windows CI 中为每个依赖重复编译 Debug。
set(VCPKG_BUILD_TYPE release)

if(PORT STREQUAL "osg")
    # 仅启用 OSGB → 3D Tiles 实际使用的插件，避免拉取 GDAL/OpenSSL/PostgreSQL/Boost 等重型依赖。
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        -DBUILD_OSG_PLUGIN_OSG=ON
        -DBUILD_OSG_PLUGIN_PNG=ON
        -DBUILD_OSG_PLUGIN_JPEG=ON
        -DCMAKE_REQUIRE_FIND_PACKAGE_PNG=ON
        -DCMAKE_REQUIRE_FIND_PACKAGE_JPEG=ON
    )
endif()
