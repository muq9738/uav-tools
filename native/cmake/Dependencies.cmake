include(FetchContent)
# 官方 PROJ 源码固定版本；关闭网络网格、TIFF 与附属程序，仅构建坐标转换库。
set(saved_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
set(BUILD_APPS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL OFF CACHE BOOL "" FORCE)
set(ENABLE_TIFF OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# 分发包不读取开发机 proj.db；同一数据库与库一起固定。
set(EMBED_RESOURCE_FILES ON CACHE BOOL "" FORCE)
set(USE_ONLY_EMBEDDED_RESOURCE_FILES ON CACHE BOOL "" FORCE)
FetchContent_Declare(proj
  URL https://codeload.github.com/OSGeo/PROJ/tar.gz/refs/tags/9.6.2
  URL_HASH SHA256=58dc7a024b583ca439cc87ad392b15a5949df63c6224e18ca8e0ba61b585a944
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(proj)
set(BUILD_TESTING "${saved_testing}")
