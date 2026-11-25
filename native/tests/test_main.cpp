#include "native_tiles.hpp"
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/PagedLOD>
#include <osg/Texture2D>
#include <osgDB/WriteFile>
#include <proj.h>
#include <png.h>
#include <tiffio.h>
#include <osg/TexEnv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace nt;
void require(bool condition,const std::string& message) {if(!condition) throw std::runtime_error(message);}
Json gltf(const fs::path& file) {
    auto b=readBytes(file);std::uint32_t length=0;std::memcpy(&length,b.data()+12,4);
    require(b.size()>=20+length,"GLB 长度");return Json::parse(b.begin()+20,b.begin()+20+length);
}
Bytes viewBytes(const Bytes& bytes,const Json& view) {
    std::uint32_t jsonLength=0;std::memcpy(&jsonLength,bytes.data()+12,4);std::size_t bin=28+jsonLength;
    if(view.contains("extensions") && view["extensions"].contains("EXT_meshopt_compression")) {
        const auto& ext=view["extensions"]["EXT_meshopt_compression"];
        require(view["buffer"]==1 && ext["buffer"]==0,"扩展必须位于访问器引用的同一虚拟视图");
        auto start=bin+ext.value("byteOffset",0U);auto length=ext["byteLength"].get<std::size_t>();
        require(start+length<=bytes.size(),"压缩视图范围");
        Bytes encoded(bytes.begin()+start,bytes.begin()+start+length);
        return decodeMeshopt(encoded,ext["count"],ext["byteStride"],ext["mode"]!="ATTRIBUTES");
    }
    require(view["buffer"]==0,"原始视图不能指向不存在的 fallback");
    auto start=bin+view.value("byteOffset",0U);auto length=view["byteLength"].get<std::size_t>();
    require(start+length<=bytes.size(),"原始视图范围");return Bytes(bytes.begin()+start,bytes.begin()+start+length);
}
// 按 glTF node → 标准 Y-up→Z-up → tileset 的完整链路解出世界坐标。
std::vector<osg::Vec3d> worldPositions(const fs::path& file,const osg::Matrixd& placement) {
    auto bytes=readBytes(file);auto doc=gltf(file);std::vector<osg::Vec3d> points;
    osg::Matrixd yToZ(1,0,0,0, 0,0,1,0, 0,-1,0,0, 0,0,0,1);
    for(const auto& node:doc["nodes"]) {
        auto values=node["matrix"].get<std::vector<double>>();osg::Matrixd matrix(values.data());
        std::set<unsigned> positions;
        for(const auto& primitive:doc["meshes"][node["mesh"].get<unsigned>()]["primitives"]) {
            auto position=primitive["attributes"]["POSITION"].get<unsigned>();if(!positions.insert(position).second) continue;
            const auto& accessor=doc["accessors"][position];
            require(accessor["componentType"]==5126 && accessor["type"]=="VEC3","测试位置类型");
            const auto& view=doc["bufferViews"][accessor["bufferView"].get<unsigned>()];
            auto valuesBytes=viewBytes(bytes,view);auto start=accessor.value("byteOffset",0U);
            for(unsigned i=0;i<accessor["count"].get<unsigned>();++i) {
                float xyz[3];std::memcpy(xyz,valuesBytes.data()+start+i*view.value("byteStride",12U),12);
                points.push_back(osg::Vec3d(xyz[0],xyz[1],xyz[2])*matrix*yToZ*placement);
            }
        }
    }
    return points;
}
Bytes tiffPixels(const fs::path& file) {
    std::unique_ptr<TIFF,decltype(&TIFFClose)> tif(TIFFOpen(file.string().c_str(),"r"),TIFFClose);
    require(bool(tif),"TIFF open");
    uint32_t height=0;
    require(TIFFGetField(tif.get(),TIFFTAG_IMAGELENGTH,&height),"TIFF height");
    const auto stride=TIFFScanlineSize(tif.get());
    Bytes pixels(stride*height);
    for (uint32_t y=0;y<height;++y) require(TIFFReadScanline(tif.get(),pixels.data()+stride*y,y)>=0,"TIFF scanline");
    return pixels;
}
Bytes pngPixels(const fs::path& file) {
    png_image image{};image.version=PNG_IMAGE_VERSION;
    require(png_image_begin_read_from_file(&image,file.string().c_str()),"独立 PNG 解码初始化");image.format=PNG_FORMAT_RGBA;
    Bytes pixels(PNG_IMAGE_SIZE(image));
    bool ok=png_image_finish_read(&image,nullptr,pixels.data(),0,nullptr);png_image_free(&image);
    require(ok,"独立 PNG 顶行在前解码");return pixels;
}
osg::ref_ptr<osg::Geode> triangle(bool strip=false,bool color=true) {
    auto geometry=new osg::Geometry;auto positions=new osg::Vec3Array;
    positions->push_back({0,0,0});positions->push_back({1,0,0});positions->push_back({0,1,0});
    if(strip) positions->push_back({1,1,0});geometry->setVertexArray(positions);
    geometry->addPrimitiveSet(new osg::DrawArrays(strip?GL_TRIANGLE_STRIP:GL_TRIANGLES,0,positions->size()));
    if(color) {
        auto colors=new osg::Vec4ubArray;
        for(unsigned i=0;i<positions->size();++i) colors->push_back({255,64,32,255});
        colors->setBinding(osg::Array::BIND_PER_VERTEX);geometry->setColorArray(colors);
    }
    auto geode=new osg::Geode;geode->addDrawable(geometry);return geode;
}
void writeNode(osg::Node* node,const fs::path& path,bool compressed=false) {
    fs::create_directories(path.parent_path());osg::ref_ptr<osgDB::Options> opts=new osgDB::Options(
        std::string("WriteImageHint=IncludeFile")+(compressed?" Compressor=zlib":""));
    require(osgDB::writeNodeFile(*node,path.string(),opts.get()),"fixture 写入失败");
}
Options options(const fs::path& input,const fs::path& output) {
    Options o;o.input=input;o.output=output;o.compressGeometry=false;o.srs="ENU:32.427247976,120.6706181";o.origin="1,2,30.665";return o;
}
int main(int argc,char** argv) {
    try {
        if (argc==3 && std::string(argv[1])=="--write-triangle") {
            writeNode(triangle(),fs::absolute(fs::u8path(argv[2])));
            return 0;
        }
        fs::path temp=fs::current_path()/("test-data-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp);unsigned checks=0;
        auto plain=triangle();writeNode(plain.get(),temp/"plain.osgb");
        auto strip=triangle(true);writeNode(strip.get(),temp/"strip.osgb",true);
        auto report=convert(options(temp/"plain.osgb",temp/"out-plain"));
        require(report["trianglesAllLods"]==1 && report["verticesAllLods"]==3,"无索引三角形保留");checks++;
        auto plainManifest=Json::parse(readBytes(temp/"out-plain/tileset.json"));
        require(plainManifest["root"]["geometricError"].get<double>()>0,"单层输入的主入口根误差必须为正，否则 Cesium 不遍历");checks++;
        auto local=options(temp/"plain.osgb",temp/"out-local");local.srs="LOCAL";
        const auto ortho=convert(local).at("orthophoto");
        require(ortho.at("width")==20 && ortho.at("height")==20 && ortho.at("metersPerPixel")==0.05 &&
                ortho.at("upperLeftEdge")==Json::array({0,1}) && ortho.at("crs")=="LOCAL","Local orthophoto grid");
        auto pixels=tiffPixels(local.output/ortho.at("uri").get<std::string>());
        auto at=[&](int x,int y,int channel){return pixels.at((y*20+x)*4+channel);};
        require(pixels.size()==20*20*4 && at(16,2,3)==0 && at(16,19,3)==255 && at(16,19,1)==64,"北朝上、透明背景和顶点颜色");checks++;
        auto scene=new osg::Group;
        auto rooftop=new osg::MatrixTransform(osg::Matrixd::translate(0,0,10));
        rooftop->addChild(triangle(true,false)); scene->addChild(rooftop); scene->addChild(triangle(true));
        auto far=new osg::MatrixTransform(osg::Matrixd::translate(220,0.5,0));far->addChild(triangle(true));scene->addChild(far);
        writeNode(scene,temp/"wide.osgb");local.input=temp/"wide.osgb";local.output=temp/"out-wide";
        auto wide=convert(local);
        auto widePixels=tiffPixels(local.output/wide.at("orthophoto").at("uri").get<std::string>());
        auto wideAt=[&](int x,int y,int c){return widePixels.at((y*4420+x)*4+c);};
        require(wide["orthophoto"]["width"]==4420 && widePixels.size()==4420*30*4 &&
                wideAt(10,9,3)==0 && wideAt(4410,9,1)==64 && wideAt(10,10,1)==255 && wideAt(30,10,3)==0 &&
                wideAt(4410,19,3)==255 && wideAt(4410,20,3)==0 && wideAt(10,29,1)==255,
                "Repeated TIFF rows preserve transparency, depth and changes between runs");checks++;
        auto uri=report["records"][0]["content"][0]["uri"].get<std::string>();auto doc=gltf(temp/"out-plain"/uri);
        require(doc["meshes"][0]["primitives"][0]["attributes"].contains("COLOR_0"),"顶点颜色丢失");checks++;
        auto zipped=convert(options(temp/"strip.osgb",temp/"out-strip"));
        require(zipped["trianglesAllLods"]==2 && zipped["verticesAllLods"]==4,"zlib 和 strip 展开");checks++;
        bool failed=false;try {convert(options(temp/"plain.osgb",temp/"out-plain"));} catch(...) {failed=true;}
        require(failed,"非空输出应拒绝覆盖");checks++;
        auto paged=new osg::PagedLOD;paged->setRangeMode(osg::LOD::PIXEL_SIZE_ON_SCREEN);
        paged->addChild(triangle().get(),0,128);paged->setFileName(1,"child.osgb");paged->setRange(1,128,1e9);
        writeNode(triangle(true).get(),temp/"tree/child.osgb");writeNode(paged,temp/"tree/root.osgb");
        auto tree=convert(options(temp/"tree/root.osgb",temp/"out-tree"));
        require(tree["files"]==2 && tree["trianglesAllLods"]==3,"分页文件/原 LOD 保留");checks++;
        for(const auto& record:tree["records"]) if(fs::path(record["source"].get<std::string>()).filename()=="root.osgb") {
            auto bytes=readBytes(temp/"out-tree"/record["tileset"].get<std::string>());auto manifest=Json::parse(bytes);
            require(std::abs(manifest["root"]["geometricError"].get<double>()-0.02)<1e-12,"空分组不能放大 LOD 误差");checks++;
        }
        auto missing=new osg::PagedLOD;missing->addChild(triangle().get(),0,128);missing->setFileName(1,"absent.osgb");missing->setRange(1,128,1e9);
        writeNode(missing,temp/"missing.osgb");failed=false;
        try {convert(options(temp/"missing.osgb",temp/"out-missing"));} catch(...) {failed=true;}
        require(failed && !fs::exists(temp/"out-missing/tileset.json"),"缺引用不能发布成功根清单");checks++;
        auto cyclic=new osg::PagedLOD;cyclic->addChild(triangle().get(),0,128);cyclic->setFileName(1,"cycle.osgb");cyclic->setRange(1,128,1e9);
        writeNode(cyclic,temp/"cycle.osgb");failed=false;
        try {convert(options(temp/"cycle.osgb",temp/"out-cycle"));} catch(...) {failed=true;}
        require(failed && !fs::exists(temp/"out-cycle/tileset.json"),"循环引用不能发布");checks++;
        auto transform=new osg::MatrixTransform;transform->setMatrix(osg::Matrixd::translate(1000000000.125,20.25,-7.5));transform->addChild(triangle().get());
        writeNode(transform,temp/"transform.osgb");auto moved=convert(options(temp/"transform.osgb",temp/"out-transform"));
        auto transformedDoc=gltf(temp/"out-transform"/moved["records"][0]["content"][0]["uri"].get<std::string>());
        require(transformedDoc["nodes"][0]["matrix"][12]==1000000000.125,"双精度局部变换丢失");checks++;
        auto enu=enuTransform(0,0,0,{0,0,30});auto placed=osg::Vec3d(0,0,0)*enu;
        require(std::abs(placed.x()-6378167)<1e-8 && std::abs(placed.y())<1e-8,"ENU 非零高度和经纬顺序");checks++;
        SpatialReference utm("EPSG:32651",{500000,0,10});
        auto zero=utm.toLocal({0,0,0});auto east=utm.toLocal({1,0,0});
        const double groundScale=(1+10.0/6378137.0)/0.9996;
        require(zero.length()<1e-7 && std::abs(east.x()-groundScale)<1e-7 && std::abs(east.y())<1e-7,"UTM 原点/轴顺序/带高度比例");checks++;
        auto projectedOptions=options(temp/"plain.osgb",temp/"out-projected");projectedOptions.srs="EPSG:32651";projectedOptions.origin="500000,0,10";
        auto projected=convert(projectedOptions);
        require(projected["coordinateRoundingErrorMeters"].get<double>()<1e-6 && projected["trianglesAllLods"]==1,"投影后局部精度和三角形");checks++;
        auto projectedFile=temp/"out-projected"/projected["records"][0]["content"][0]["uri"].get<std::string>();
        auto world=worldPositions(projectedFile,utm.placement());
        const osg::Vec3d sourcePoints[]={{0,0,0},{1,0,0},{0,1,0}};
        for(unsigned i=0;i<3;++i) require((world[i]-utm.toLocal(sourcePoints[i])*utm.placement()).length()<1e-6,"GLB 节点/客户端 up 轴只能转换一次");checks++;
        SpatialReference site("EPSG:32651",{224107.81419846753,3366639.462932359,-66.472000000981907});
        const auto& frame=site.placement();
        for(unsigned i=0;i<3;++i) for(unsigned j=0;j<3;++j) {
            double dot=0;for(unsigned k=0;k<3;++k) dot+=frame(i,k)*frame(j,k);
            require(std::abs(dot-(i==j?1.0:0.0))<1e-12,"ENU 基向量不正交");
        }
        auto context=proj_context_create();const char* dataPaths[]={TEST_PROJ_DATA};proj_context_set_search_paths(context,1,dataPaths);
        auto reverse=proj_create_crs_to_crs(context,"EPSG:4978","EPSG:32651",nullptr);require(reverse,"建立反向 PROJ");
        for(unsigned i=0;i<1000;++i) {
            osg::Vec3d p((i*37)%601-300.0,(i*61)%601-300.0,(i*17)%101-50.0);
            auto ecef=site.toLocal(p)*frame;auto back=proj_trans(reverse,PJ_FWD,proj_coord(ecef.x(),ecef.y(),ecef.z(),0));
            osg::Vec3d actual(back.xyz.x-224107.81419846753,back.xyz.y-3366639.462932359,back.xyz.z+66.472000000981907);
            require((actual-p).length()<1e-6,"负高度、UTM、ENU 世界坐标闭环");
        }
        proj_destroy(reverse);proj_context_destroy(context);checks++;
        auto image=new osg::Image;image->allocateImage(2,2,1,GL_RGBA,GL_UNSIGNED_BYTE);
        const unsigned char rgba[]={255,0,0,255,0,255,0,255,0,0,255,255,255,255,0,96};
        std::memcpy(image->data(),rgba,sizeof(rgba));image->setFileName((temp/"original.png").string());
        require(osgDB::writeImageFile(*image,image->getFileName()),"PNG fixture 输出");
        auto textured=triangle(false,false);auto geometry=textured->getDrawable(0)->asGeometry();auto uv=new osg::Vec2Array;
        uv->push_back({0,0});uv->push_back({1,0});uv->push_back({0,1});geometry->setTexCoordArray(0,uv);
        auto texture=new osg::Texture2D(image);texture->setWrap(osg::Texture::WRAP_S,osg::Texture::REPEAT);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
        textured->getOrCreateStateSet()->setTextureAttributeAndModes(0,texture,osg::StateAttribute::ON);
        textured->getOrCreateStateSet()->setMode(GL_BLEND,osg::StateAttribute::ON);
        writeNode(textured.get(),temp/"texture+alpha.osgb");
        auto tex=convert(options(temp/"texture+alpha.osgb",temp/"out-texture"));
        require(tex["originalEncodedImages"]==1,"原始编码图片没有透传");checks++;
        auto content=tex["records"][0]["content"][0];auto imagePath=temp/"out-texture"/content["images"][0]["uri"].get<std::string>();
        require(readBytes(imagePath)==readBytes(temp/"original.png"),"PNG 原字节发生变化");checks++;
        auto textureDoc=gltf(temp/"out-texture"/content["uri"].get<std::string>());
        require(textureDoc["materials"][0]["alphaMode"]=="BLEND" && textureDoc["samplers"][0]["wrapS"]==GL_REPEAT,"alpha 或采样丢失");checks++;
        auto streaming=options(temp/"texture+alpha.osgb",temp/"out-texture-streamed");
        streaming.memoryBudgetBytes=256; // Force the on-demand mesh path and one-row bands.
        auto streamed=convert(streaming).at("orthophoto");
        const auto rasterPath=streamed.at("uri").get<std::string>();
        require(streamed==tex.at("orthophoto") && readBytes(streaming.output/rasterPath)==readBytes(temp/"out-texture"/rasterPath),
                "Texture sampling must match with and without the raster cache");checks++;
        auto sheet=new osg::MatrixTransform(osg::Matrixd::scale(110,12,1));
        sheet->addChild(textured);writeNode(sheet,temp/"ortho-sheet.osgb");
        auto serial=options(temp/"ortho-sheet.osgb",temp/"out-sheet-serial");serial.srs="LOCAL";serial.threads=1;
        auto threaded=serial;threaded.output=temp/"out-sheet-threaded";threaded.threads=4;
        auto sheetReport=convert(serial).at("orthophoto");
        require(sheetReport.at("width")==2200 && sheetReport.at("height")==240 && convert(threaded).at("orthophoto")==sheetReport &&
                readBytes(threaded.output/rasterPath)==readBytes(serial.output/rasterPath),"Orthophoto row workers must produce identical TIFFs");checks++;
        auto rawOptions=options(temp/"texture+alpha.osgb",temp/"out-texture-pixels");rawOptions.preserveEncoded=false;
        auto raw=convert(rawOptions);require(raw["originalEncodedImages"]==0 && raw["imageReferences"]==1,"原始像素 PNG 路径失败");checks++;
        auto encodedPixels=pngPixels(imagePath);
        auto rawImagePath=temp/"out-texture-pixels"/raw["records"][0]["content"][0]["images"][0]["uri"].get<std::string>();
        auto memoryPixels=pngPixels(rawImagePath);require(content["images"][0]["flipV"]==true,"原图必须匹配 OSG 内存采样方向");
        for(unsigned y=0;y<2;++y) for(unsigned x=0;x<2;++x) {
            auto a=(y*2+x)*4,b=((1-y)*2+x)*4;
            if(encodedPixels[a+3]==255) for(unsigned c=0;c<4;++c) require(encodedPixels[a+c]==memoryPixels[b+c],"原编码顶行与 OSG 内存底行方向回归");
        }
        checks++;
        textured->getOrCreateStateSet()->setTextureMode(0,GL_TEXTURE_2D,osg::StateAttribute::OFF);
        writeNode(textured.get(),temp/"texture-off.osgb");auto disabled=convert(options(temp/"texture-off.osgb",temp/"out-texture-off"));
        auto disabledDoc=gltf(temp/"out-texture-off"/disabled["records"][0]["content"][0]["uri"].get<std::string>());
        require(!disabledDoc["materials"][0]["pbrMetallicRoughness"].contains("baseColorTexture") && disabled["imageReferences"]==0,"纹理属性存在但 OFF 时不能采样");checks++;
        textured->getOrCreateStateSet()->setTextureMode(0,GL_TEXTURE_2D,osg::StateAttribute::ON);
        textured->getOrCreateStateSet()->setTextureAttribute(0,new osg::TexEnv(osg::TexEnv::DECAL));
        writeNode(textured.get(),temp/"texture-decal.osgb");failed=false;
        try {convert(options(temp/"texture-decal.osgb",temp/"out-decal"));} catch(const std::exception& e) {failed=std::string(e.what()).find("MODULATE")!=std::string::npos;}
        require(failed,"不支持的 TexEnv 不能静默当作 MODULATE");checks++;
        writeNode(triangle().get(),temp/"parallel/a.osgb");writeNode(triangle().get(),temp/"parallel/b.osgb");
        auto parallelOptions=options(temp/"parallel",temp/"out-parallel");parallelOptions.threads=2;
        auto parallel=convert(parallelOptions);require(parallel["files"]==2 && parallel["trianglesAllLods"]==2,"并行根块遗漏");checks++;
        auto branches=new osg::Group;
        for(unsigned i=0;i<2;++i) {
            auto branch=new osg::PagedLOD;branch->addChild(triangle().get(),0,128);
            auto name=std::string("child-")+std::to_string(i)+".osgb";branch->setFileName(1,name);branch->setRange(1,128,1e9);
            branches->addChild(branch);writeNode(triangle(true).get(),temp/"branches"/name);
        }
        writeNode(branches,temp/"branches/root.osgb");
        auto branched=convert(options(temp/"branches/root.osgb",temp/"out-branches"));
        for(const auto& row:branched["records"]) if(fs::path(row["source"].get<std::string>()).filename()=="root.osgb") {
            require(row["content"].size()==1,"同一源文件不能拆为多个替换单元");
            auto model=gltf(temp/"out-branches"/row["content"][0]["uri"].get<std::string>());
            unsigned primitiveCount=0;for(const auto& mesh:model["meshes"]) primitiveCount+=mesh["primitives"].size();
            require(primitiveCount==2,"文件合并不能丢常驻分支");
        }
        checks++;
        auto external=new osg::PagedLOD;external->setFileName(0,"leaf.osgb");external->setRange(0,0,1e9);
        auto externalTransform=new osg::MatrixTransform;externalTransform->setMatrix(osg::Matrixd::translate(17.5,4,-2));externalTransform->addChild(external);
        writeNode(triangle().get(),temp/"projected-ref/leaf.osgb");writeNode(externalTransform,temp/"projected-ref/root.osgb");
        auto refOptions=options(temp/"projected-ref/root.osgb",temp/"out-projected-ref");refOptions.srs="EPSG:32651";refOptions.origin="500000,0,10";
        auto ref=convert(refOptions);bool leafFound=false;
        for(const auto& row:ref["records"]) if(!row["content"].empty()) {
            auto points=worldPositions(temp/"out-projected-ref"/row["content"][0]["uri"].get<std::string>(),utm.placement());
            require((points[0]-utm.toLocal({17.5,4,-2})*utm.placement()).length()<1e-5,"跨文件矩阵必须先于投影");leafFound=true;
        }
        require(leafFound,"纯索引文件必须保留有效子内容");checks++;
        fs::create_directories(temp/"layout/Data/Block");writeNode(triangle().get(),temp/"layout/Data/Block/Block.osgb");
        const std::string xml="<?xml version=\"1.0\"?><ModelMetadata><!--<SRS>错误</SRS>--><SRS><![CDATA[EPSG:32651]]></SRS><SRSOrigin>500000,0,-10</SRSOrigin></ModelMetadata>";
        atomicWrite(temp/"layout/metadata.xml",Bytes(xml.begin(),xml.end()));
        Options layout;layout.input=temp/"layout/Data";layout.output=temp/"out-layout";
        auto layoutResult=convert(layout);require(layoutResult["srs"]=="EPSG:32651" && layoutResult["files"]==1,"Data 目录、XML 注释和 CDATA 解析");checks++;
        auto directoryTree=convert(options(temp/"tree",temp/"out-directory-tree"));
        auto rootBytes=readBytes(temp/"out-directory-tree/tileset.json");auto directoryManifest=Json::parse(rootBytes);
        require(directoryTree["records"].size()==2 && directoryManifest["root"]["children"].size()==1,"目录入口不能重复加载被引用子瓦片");checks++;
        writeNode(triangle().get(),temp/"upper/Tile.OSGB");
        auto upper=convert(options(temp/"upper",temp/"out-upper"));require(upper["files"]==1,"大写 OSGB 扩展名");checks++;
        auto makeIndex=[&](const fs::path& file,const std::string& target) {
            auto p=new osg::PagedLOD;p->setFileName(0,target);p->setRange(0,0,1e9);writeNode(p,file);
        };
        writeNode(triangle().get(),temp/"transitive/B.osgb");
        makeIndex(temp/"transitive/sub/middle.osgb","../B.osgb");makeIndex(temp/"transitive/A.osgb","sub/middle.osgb");
        auto transitive=convert(options(temp/"transitive",temp/"out-transitive"));
        auto transitiveRoot=Json::parse(readBytes(temp/"out-transitive/tileset.json"));
        require(transitive["files"]==3 && transitiveRoot["root"]["children"].size()==1,"根发现必须覆盖间接引用");checks++;
        makeIndex(temp/"root-cycle/A.osgb","B.osgb");makeIndex(temp/"root-cycle/B.osgb","A.osgb");writeNode(triangle().get(),temp/"root-cycle/C.osgb");
        failed=false;try {convert(options(temp/"root-cycle",temp/"out-root-cycle"));} catch(...) {failed=true;}
        require(failed && !fs::exists(temp/"out-root-cycle/tileset.json"),"不能把循环组件静默删掉只发布其他根");checks++;
        Options noSrs;noSrs.input=temp/"plain.osgb";noSrs.output=temp/"out-no-srs";
        failed=false;try {convert(noSrs);} catch(...) {failed=true;}
        require(failed,"缺失 SRS 必须显式失败");checks++;
        auto enuOffset=enuTransform(32.427248,120.670618,0,{0,0,30.665});
        auto enuHeight=enuTransform(32.427248,120.670618,30.665,{0,0,0});
        for(unsigned i=0;i<16;++i) require(std::abs(enuOffset.ptr()[i]-enuHeight.ptr()[i])<1e-8,"ENU 原点 up 偏移应等价于椭球高度");checks++;
        auto segmentedGeometry=new osg::Geometry;auto segmentPositions=new osg::Vec3Array;
        for(float x:{0.0f,10.0f}) {segmentPositions->push_back({x,0,0});segmentPositions->push_back({x+1,0,0});segmentPositions->push_back({x,1,0});segmentPositions->push_back({x+1,1,0});}
        segmentedGeometry->setVertexArray(segmentPositions);auto lengths=new osg::DrawArrayLengths(GL_TRIANGLE_STRIP,0);lengths->push_back(4);lengths->push_back(4);
        segmentedGeometry->addPrimitiveSet(lengths);segmentedGeometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
        auto segmentColors=new osg::Vec4ubArray;segmentColors->push_back({255,0,0,255});segmentColors->push_back({0,255,0,255});segmentColors->setBinding(osg::Array::BIND_PER_PRIMITIVE_SET);segmentedGeometry->setColorArray(segmentColors);
        auto segments=new osg::Geode;segments->addDrawable(segmentedGeometry);writeNode(segments,temp/"segments.osgb");
        auto segmented=convert(options(temp/"segments.osgb",temp/"out-segments"));
        auto segmentedDoc=gltf(temp/"out-segments"/segmented["records"][0]["content"][0]["uri"].get<std::string>());
        require(segmented["trianglesAllLods"]==5 && segmentedDoc["meshes"][0]["primitives"].size()==3,"分段 strip 不能生成跨段桥接面");checks++;
        auto degenerate=new osg::Geometry;degenerate->setVertexArray(segmentPositions);auto stripIndices=new osg::DrawElementsUInt(GL_TRIANGLE_STRIP);
        for(unsigned index:{0U,1U,2U,2U,4U,4U,5U,6U}) stripIndices->push_back(index);degenerate->addPrimitiveSet(stripIndices);
        auto degenerateNode=new osg::Geode;degenerateNode->addDrawable(degenerate);writeNode(degenerateNode,temp/"degenerate.osgb");
        auto degenerateResult=convert(options(temp/"degenerate.osgb",temp/"out-degenerate"));require(degenerateResult["trianglesAllLods"]==2,"连接条带的退化索引不能作为可见面计数");checks++;
        auto doubleGeometry=new osg::Geometry;auto doublePositions=new osg::Vec3dArray;
        doublePositions->push_back({224107.81419846753,3366639.462932359,-66.472});doublePositions->push_back({224108.81419846753,3366639.462932359,-66.472});doublePositions->push_back({224107.81419846753,3366640.462932359,-66.472});
        doubleGeometry->setVertexArray(doublePositions);doubleGeometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));auto doubleNode=new osg::Geode;doubleNode->addDrawable(doubleGeometry);writeNode(doubleNode,temp/"double.osgb");
        auto doubleOptions=options(temp/"double.osgb",temp/"out-double");doubleOptions.srs="EPSG:32651";doubleOptions.origin="0,0,0";
        auto doubleResult=convert(doubleOptions);require(doubleResult["trianglesAllLods"]==1 && doubleResult["coordinateRoundingErrorMeters"].get<double>()<1e-6,"Vec3d 必须先投影/居中再写 float");checks++;
        writeNode(triangle().get(),temp/"retry-input/a.osgb");writeNode(textured.get(),temp/"retry-input/b.osgb");
        auto retryOptions=options(temp/"retry-input",temp/"out-retry");retryOptions.threads=2;
        failed=false;try {convert(retryOptions);} catch(...) {failed=true;}
        require(failed && !fs::exists(temp/"out-retry"),"失败不能留下不可重跑的输出目录");
        for(const auto& entry:fs::directory_iterator(temp)) require(entry.path().filename().string().rfind(".out-retry.building-",0)!=0,"失败暂存目录必须清理");
        writeNode(triangle().get(),temp/"retry-input/b.osgb");auto retried=convert(retryOptions);
        require(retried["files"]==2 && fs::exists(temp/"out-retry/tileset.json"),"修复输入后应可对同一路径直接重跑");checks++;
        auto budgetOptions=options(temp/"retry-input",temp/"out-budget");budgetOptions.memoryBudgetBytes=1ull<<20;budgetOptions.threads=2;
        auto budgeted=convert(budgetOptions);
        require(budgeted["files"]==2 && fs::exists(temp/"out-budget/tileset.json"),"内存预算不能减少处理文件或阻断发布");
        require(budgeted["memoryThrottled"].get<bool>() && budgeted["peakResidentBytes"].get<std::uint64_t>()>0,"内存预算门控与驻留采样应生效");checks++;
        const float codecVertices[]={0.0f,-0.0f,1.2345678f,-1.0f,1e-20f,1e20f};
        auto encodedVertices=encodeMeshopt(codecVertices,sizeof(codecVertices),12,false);
        auto decodedVertices=decodeMeshopt(encodedVertices,2,12,false);
        require(encodedVertices[0]==0xa0 && std::memcmp(codecVertices,decodedVertices.data(),sizeof(codecVertices))==0,"EXT v0 顶点位流必须逐字节保真");checks++;
        const std::uint16_t sequence16[]={0,65535,1,2,1,0};const std::uint32_t sequence32[]={0,65536,2,999999,65536,0};
        for(unsigned size:{2U,4U}) {
            const void* data=size==2?static_cast<const void*>(sequence16):static_cast<const void*>(sequence32);
            auto encoded=encodeMeshopt(data,6*size,size,true);auto decoded=decodeMeshopt(encoded,6,size,true);
            require(encoded[0]==0xd1 && std::memcmp(data,decoded.data(),6*size)==0,"索引序列不能轮转、重排或截断");
        }
        checks++;
        failed=false;try {decodeMeshopt(Bytes{0xa1},2,12,false);} catch(...) {failed=true;}require(failed,"EXT 必须拒绝不兼容的 v1 顶点位流");checks++;
        // Canonical oriented triples are an independent oracle: a cyclic start
        // change is valid; reversing winding or moving a face is not.
        const std::uint16_t faces16[]={1,2,0,2,3,0,7,7,7,4,5,3};
        const std::uint32_t faces32[]={70001,70002,70000,70002,70003,70000,70007,70007,70007,70004,70005,70003};
        auto canonical=[](std::array<unsigned,3> a) {
            return std::min({a,std::array<unsigned,3>{a[1],a[2],a[0]},std::array<unsigned,3>{a[2],a[0],a[1]}});
        };
        require(canonical({0,1,2})!=canonical({0,2,1}),"朝向 oracle 必须发现反面");
        require(canonical({1,2,0})==canonical({0,1,2}) && canonical({0,1,2})!=canonical({3,4,5}),"oracle 只允许面内循环，不允许换面序");
        for(unsigned size:{2U,4U}) {
            const void* data=size==2?static_cast<const void*>(faces16):static_cast<const void*>(faces32);
            auto encoded=encodeMeshopt(data,12*size,size,true,true),decoded=decodeMeshopt(encoded,12,size,true);
            require(encoded[0]==0xe1,"TRIANGLES 必须使用目标解码器支持的 v1 索引位流");
            for(unsigned face=0;face<12;face+=3) {
                std::array<unsigned,3> old{},next{};
                for(unsigned c=0;c<3;++c) {std::memcpy(&old[c],static_cast<const unsigned char*>(data)+(face+c)*size,size);std::memcpy(&next[c],decoded.data()+(face+c)*size,size);}
                require(canonical(old)==canonical(next),"TRIANGLES 改变面次序、朝向或角点");
            }
            failed=false;try {decodeMeshopt(encoded,11,size,true);} catch(...) {failed=true;}require(failed,"TRIANGLES 解码必须拒绝不完整的面");
        }
        failed=false;try {encodeMeshopt(faces16,sizeof(faces16)-2,2,true,true);} catch(...) {failed=true;}require(failed,"TRIANGLES 编码必须拒绝不完整的面");
        failed=false;try {encodeMeshopt(codecVertices,sizeof(codecVertices),12,false,true);} catch(...) {failed=true;}require(failed,"TRIANGLES 不能用于属性");checks++;
        auto compressNode=new osg::Geode;auto compressGeometry=new osg::Geometry;auto compressPositions=new osg::Vec3Array;
        for(unsigned i=0;i<600;++i) compressPositions->push_back({float(i%20)*0.25f,float(i/20)*0.25f,0});
        auto compressUv=new osg::Vec2Array;auto compressNormals=new osg::Vec3Array;auto compressColors=new osg::Vec4Array;
        for(unsigned i=0;i<600;++i) {
            compressUv->push_back({float(i%17)/16, i%2?-0.0f:1e-40f});
            compressNormals->push_back({0,0,1});compressColors->push_back({float(i%3)*0.5f,0.25f,1,0.5f});
        }
        compressGeometry->setTexCoordArray(0,compressUv);compressGeometry->setNormalArray(compressNormals,osg::Array::BIND_PER_VERTEX);
        compressGeometry->setColorArray(compressColors,osg::Array::BIND_PER_VERTEX);
        compressGeometry->setVertexArray(compressPositions);auto compressFaces=new osg::DrawElementsUInt(GL_TRIANGLES);
        for(unsigned i=0;i<596;i+=4) for(unsigned id:{i,i+1,i+2,i+2,i+3,i}) compressFaces->push_back(id);
        compressGeometry->addPrimitiveSet(compressFaces);compressNode->addDrawable(compressGeometry);writeNode(compressNode,temp/"compress.osgb");
        auto baseline=convert(options(temp/"compress.osgb",temp/"out-compress-raw"));
        auto packedOptions=options(temp/"compress.osgb",temp/"out-compress-meshopt");packedOptions.compressGeometry=true;auto packed=convert(packedOptions);
        auto rawFile=temp/"out-compress-raw"/baseline["records"][0]["content"][0]["uri"].get<std::string>();
        auto packedFile=temp/"out-compress-meshopt"/packed["records"][0]["content"][0]["uri"].get<std::string>();
        auto rawDoc=gltf(rawFile),packedDoc=gltf(packedFile);auto rawBytes=readBytes(rawFile),packedBytes=readBytes(packedFile);
        require(packed["compressedViews"].get<unsigned>()>0 && rawDoc["accessors"]==packedDoc["accessors"] && rawDoc["nodes"]==packedDoc["nodes"],"meshopt 不能改变访问器或变换");
        require(rawDoc["bufferViews"].size()==packedDoc["bufferViews"].size(),"meshopt 不能创建脱离访问器的伪配对视图");
        bool sawTriangles=false,sawRotation=false;
        for(unsigned i=0;i<rawDoc["bufferViews"].size();++i) {
            auto old=viewBytes(rawBytes,rawDoc["bufferViews"][i]),next=viewBytes(packedBytes,packedDoc["bufferViews"][i]);
            const auto& v=packedDoc["bufferViews"][i];
            if(v.contains("extensions") && v["extensions"]["EXT_meshopt_compression"]["mode"]=="TRIANGLES") {
                sawTriangles=true;require(old.size()==next.size(),"三角形索引数量改变");
                auto stride=v["extensions"]["EXT_meshopt_compression"]["byteStride"].get<unsigned>();
                for(unsigned face=0;face<old.size()/stride;face+=3) {
                    std::array<unsigned,3> a{},b{};
                    for(unsigned c=0;c<3;++c) {std::memcpy(&a[c],old.data()+(face+c)*stride,stride);std::memcpy(&b[c],next.data()+(face+c)*stride,stride);}
                    require(canonical(a)==canonical(b),"GLB 改变有向面序列");sawRotation|=a!=b;
                }
            } else require(old==next,"GLB 非三角形视图解码必须逐字节匹配 raw");
        }
        require(sawTriangles && sawRotation,"集成夹具必须实际压缩且发生合法循环轮转");
        require(std::find(packedDoc["extensionsRequired"].begin(),packedDoc["extensionsRequired"].end(),"EXT_meshopt_compression")!=packedDoc["extensionsRequired"].end(),"无回退数据时必须声明 required");
        // 全部属性视图（包括未引用顶点）已逐字节核对，节点矩阵也保持相同。
        checks++;
        for(unsigned mode:{GL_POINTS,GL_LINES,GL_LINE_STRIP,GL_LINE_LOOP}) {
            auto lineNode=new osg::Geode;auto lineGeometry=new osg::Geometry;
            lineGeometry->setVertexArray(compressPositions);lineGeometry->addPrimitiveSet(new osg::DrawArrays(mode,0,600));lineNode->addDrawable(lineGeometry);
            auto input=temp/("ordered-"+std::to_string(mode)+".osgb"),output=temp/("ordered-out-"+std::to_string(mode));writeNode(lineNode,input);
            auto lineOptions=options(input,output);lineOptions.compressGeometry=true;auto result=convert(lineOptions);
            auto file=output/result["records"][0]["content"][0]["uri"].get<std::string>();auto doc=gltf(file);auto bytes=readBytes(file);
            auto id=doc["meshes"][0]["primitives"][0]["indices"].get<unsigned>();const auto& view=doc["bufferViews"][doc["accessors"][id]["bufferView"].get<unsigned>()];
            require(view["extensions"]["EXT_meshopt_compression"]["mode"]=="INDICES","点/线不能使用三角形循环编码");
            auto sequence=viewBytes(bytes,view);for(unsigned i=0;i<600;++i) {std::uint16_t value;std::memcpy(&value,sequence.data()+i*2,2);require(value==i,"点/线索引顺序被改变");}
        }
        checks++;
        auto sharedFrame=new osg::Group;
        const osg::Vec3d offsets[]={{-150.125,-100.25,12},{150.125,100.25,23}};
        for(const auto& offset:offsets) {
            auto part=triangle();auto positions=dynamic_cast<osg::Vec3Array*>(part->getDrawable(0)->asGeometry()->getVertexArray());
            for(auto& vertex:*positions) vertex+=osg::Vec3(offset.x(),offset.y(),offset.z());sharedFrame->addChild(part.get());
        }
        writeNode(sharedFrame,temp/"shared-frame.osgb");auto sharedOptions=options(temp/"shared-frame.osgb",temp/"out-shared-frame");
        auto sharedResult=convert(sharedOptions);auto sharedFile=temp/"out-shared-frame"/sharedResult["records"][0]["content"][0]["uri"].get<std::string>();
        auto sharedPlacement=enuTransform(32.427247976,120.6706181,0,{1,2,30.665});
        auto sharedDoc=gltf(sharedFile);auto sharedWorld=worldPositions(sharedFile,sharedPlacement);
        require(sharedDoc["nodes"].size()==1 && sharedDoc["meshes"][0]["primitives"].size()==2 && sharedWorld.size()==6,"相同矩阵应复用节点并保留几何");
        for(unsigned i=0;i<2;++i) for(unsigned v=0;v<3;++v) require((sharedWorld[i*3+v]-(offsets[i]+sourcePoints[v])*sharedPlacement).length()<1e-7,"相同矩阵合并不能改写坐标");checks++;
        auto scrambled=new osg::Geometry;auto scramblePositions=new osg::Vec3Array;auto scrambleUv=new osg::Vec2Array;
        for(unsigned i=0;i<8;++i) {scramblePositions->push_back({float(i),float(i)*0.5f,-float(i)});scrambleUv->push_back({float(i)*0.125f,1-float(i)*0.0625f});}
        scrambled->setVertexArray(scramblePositions);scrambled->setTexCoordArray(0,scrambleUv);
        auto scrambleNormals=new osg::Vec3Array;scrambleNormals->push_back({0,0,1});scrambleNormals->push_back({0,1,0});scrambleNormals->setBinding(osg::Array::BIND_PER_PRIMITIVE_SET);scrambled->setNormalArray(scrambleNormals);
        auto scrambleColors=new osg::Vec4Array;scrambleColors->push_back({1,0,0,1});scrambleColors->push_back({0,1,0,1});scrambleColors->setBinding(osg::Array::BIND_PER_PRIMITIVE_SET);scrambled->setColorArray(scrambleColors);
        const unsigned sourceIndices[2][3]={{5,2,7},{7,2,4}};
        for(const auto& ids:sourceIndices) {auto ps=new osg::DrawElementsUInt(GL_TRIANGLES);for(auto id:ids)ps->push_back(id);scrambled->addPrimitiveSet(ps);}
        auto scrambleNode=new osg::Geode;scrambleNode->addDrawable(scrambled);writeNode(scrambleNode,temp/"scrambled.osgb");
        auto scrambleReport=convert(options(temp/"scrambled.osgb",temp/"out-scrambled"));auto scrambleFile=temp/"out-scrambled"/scrambleReport["records"][0]["content"][0]["uri"].get<std::string>();
        auto scrambleDoc=gltf(scrambleFile);auto scrambleBytes=readBytes(scrambleFile);
        auto floatElement=[&](unsigned accessor,unsigned index,unsigned width) {
            const auto& a=scrambleDoc["accessors"][accessor];require(a["componentType"]==5126,"属性应保持 FLOAT");
            const auto& view=scrambleDoc["bufferViews"][a["bufferView"].get<unsigned>()];auto bytes=viewBytes(scrambleBytes,view);
            std::vector<float> value(width);std::memcpy(value.data(),bytes.data()+a.value("byteOffset",0U)+index*view.value("byteStride",width*4),width*4);return value;
        };
        std::set<unsigned> retained;
        for(unsigned p=0;p<2;++p) {
            const auto& primitive=scrambleDoc["meshes"][0]["primitives"][p];const auto& a=scrambleDoc["accessors"][primitive["indices"].get<unsigned>()];
            auto indexBytes=viewBytes(scrambleBytes,scrambleDoc["bufferViews"][a["bufferView"].get<unsigned>()]);unsigned step=a["componentType"]==5123?2:4;
            for(unsigned corner=0;corner<3;++corner) {
                unsigned index=0;std::memcpy(&index,indexBytes.data()+corner*step,step);unsigned old=sourceIndices[p][corner];
                require(floatElement(primitive["attributes"]["POSITION"],index,3)==std::vector<float>({float(old),float(old)*0.5f,-float(old)}),"存储置换改变了三角形角点");
                require(floatElement(primitive["attributes"]["TEXCOORD_0"],index,2)==std::vector<float>({float(old)*0.125f,1-float(old)*0.0625f}),"存储置换改变了 UV 对应");
                require(floatElement(primitive["attributes"]["NORMAL"],index,3)==(p?std::vector<float>{0,1,0}:std::vector<float>{0,0,1}),"存储置换改变了分组法线");
                require(floatElement(primitive["attributes"]["COLOR_0"],index,4)==(p?std::vector<float>{0,1,0,1}:std::vector<float>{1,0,0,1}),"存储置换改变了分组颜色");
            }
            auto pos=primitive["attributes"]["POSITION"].get<unsigned>();require(scrambleDoc["accessors"][pos]["count"]==8,"未引用顶点不能丢弃");
            for(unsigned i=0;i<8;++i) {
                auto value=floatElement(pos,i,3);auto old=static_cast<unsigned>(value[0]);retained.insert(old);
                require(value==std::vector<float>({float(old),float(old)*0.5f,-float(old)}),"未引用位置被改写");
                require(floatElement(primitive["attributes"]["TEXCOORD_0"],i,2)==std::vector<float>({float(old)*0.125f,1-float(old)*0.0625f}),"未引用 UV 被改写");
            }
        }
        require(retained==std::set<unsigned>({0,1,2,3,4,5,6,7}),"未引用顶点内容丢失");checks++;
        auto cacheNode=new osg::Geode;
        for(unsigned part=0;part<2;++part) {
            auto geometry=new osg::Geometry;auto values=new osg::Vec3Array;
            values->push_back({float(part)*2,0,0});values->push_back({float(part)*2+1,0,0});values->push_back({float(part)*2,1,0});values->push_back({float(part)*2+1,1,0});geometry->setVertexArray(values);
            auto ids=new osg::DrawElementsUInt(GL_TRIANGLES);const unsigned patterns[2][6]={{0,1,2,0,2,3},{0,1,2,1,3,2}};
            for(unsigned repeat=0;repeat<100;++repeat)for(auto id:patterns[part])ids->push_back(id);
            geometry->addPrimitiveSet(ids);cacheNode->addDrawable(geometry);
        }
        writeNode(cacheNode,temp/"index-cache.osgb");auto cacheOptions=options(temp/"index-cache.osgb",temp/"out-index-cache");cacheOptions.compressGeometry=true;
        auto cacheReport=convert(cacheOptions);auto cacheFile=temp/"out-index-cache"/cacheReport["records"][0]["content"][0]["uri"].get<std::string>();auto cacheDoc=gltf(cacheFile);auto cacheBytes=readBytes(cacheFile);
        std::set<std::size_t> indexOffsets;std::vector<Bytes> indexData;std::size_t end=0;
        for(const auto& view:cacheDoc["bufferViews"])if(view["buffer"]==1) {
            auto offset=view["byteOffset"].get<std::size_t>();require(offset%4==0 && offset>=end,"压缩视图的虚拟范围必须唯一、对齐且不重叠");end=offset+view["byteLength"].get<std::size_t>();
            if(view["target"]==34963){indexOffsets.insert(offset);indexData.push_back(viewBytes(cacheBytes,view));}
        }
        require(indexOffsets.size()==2 && indexData[0]!=indexData[1] && end<=cacheDoc["buffers"][1]["byteLength"].get<std::size_t>(),"等长不同索引不能在 Cesium 缓存中别名");checks++;
        auto restartNode=new osg::Geode;auto restartGeometry=new osg::Geometry;auto restartPositions=new osg::Vec3Array;
        for(unsigned i=0;i<65536;++i) restartPositions->push_back({float(i)/1024,0,0});
        restartGeometry->setVertexArray(restartPositions);restartGeometry->addPrimitiveSet(new osg::DrawArrays(GL_POINTS,0,65536));restartNode->addDrawable(restartGeometry);
        writeNode(restartNode,temp/"restart-index.osgb");auto restartReport=convert(options(temp/"restart-index.osgb",temp/"out-restart-index"));
        auto restartDoc=gltf(temp/"out-restart-index"/restartReport["records"][0]["content"][0]["uri"].get<std::string>());
        auto restartAccessor=restartDoc["meshes"][0]["primitives"][0]["indices"].get<unsigned>();
        require(restartDoc["accessors"][restartAccessor]["componentType"]==5125 && restartDoc["accessors"][restartAccessor]["count"]==65536,"65535 不能作为 UNSIGNED_SHORT 普通索引");checks++;
        // 边界：DrawArrays 子区间、坏属性和父内容失败均不能静默改变结果。
        auto rangeGeometry=new osg::Geometry;auto rangePositions=new osg::Vec3Array;
        for(unsigned i=0;i<6;++i) rangePositions->push_back({float(i),float(i%2),0});
        rangeGeometry->setVertexArray(rangePositions);rangeGeometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,3,3));
        auto rangeNode=new osg::Geode;rangeNode->addDrawable(rangeGeometry);writeNode(rangeNode,temp/"draw-range.osgb");
        auto rangeOptions=options(temp/"draw-range.osgb",temp/"out-draw-range");auto rangeReport=convert(rangeOptions);
        auto rangeFile=temp/"out-draw-range"/rangeReport["records"][0]["content"][0]["uri"].get<std::string>();
        auto rangeDoc=gltf(rangeFile);auto rangeBytes=readBytes(rangeFile);const auto& rangePrimitive=rangeDoc["meshes"][0]["primitives"][0];
        auto rangeIndex=rangeDoc["accessors"][rangePrimitive["indices"].get<unsigned>()];
        auto rangeIndices=viewBytes(rangeBytes,rangeDoc["bufferViews"][rangeIndex["bufferView"].get<unsigned>()]);
        std::uint16_t firstIndices[3];std::memcpy(firstIndices,rangeIndices.data(),sizeof(firstIndices));
        auto rangePlacement=enuTransform(32.427247976,120.6706181,0,{1,2,30.665});auto rangeWorld=worldPositions(rangeFile,rangePlacement);
        for(unsigned i=0;i<3;++i) require((rangeWorld[i]-osg::Vec3d(i+3,(i+3)%2,0)*rangePlacement).length()<1e-6,"DrawArrays 必须引用 first 后的角点");
        require(rangeReport["trianglesAllLods"]==1 && rangeIndex["count"]==3 && firstIndices[0]==0 && firstIndices[1]==1 && firstIndices[2]==2 && rangeWorld.size()==6,"DrawArrays 必须只绘制 first/count，仍保留未引用顶点");checks++;
        auto badColors=new osg::Vec4Array;badColors->push_back({1,1,1,1});badColors->push_back({1,1,1,1});badColors->setBinding(osg::Array::BIND_PER_VERTEX);
        rangeGeometry->setColorArray(badColors);writeNode(rangeNode,temp/"bad-colors.osgb");failed=false;
        try {convert(options(temp/"bad-colors.osgb",temp/"out-bad-colors"));} catch(...) {failed=true;}
        require(failed && !fs::exists(temp/"out-bad-colors"),"属性数量错误不得删除属性后成功发布");checks++;
        auto badParent=new osg::PagedLOD;badParent->addChild(rangeNode,0,128);badParent->setFileName(1,"plain.osgb");badParent->setRange(1,128,1e9);
        writeNode(badParent,temp/"bad-parent.osgb");failed=false;
        try {convert(options(temp/"bad-parent.osgb",temp/"out-bad-parent"));} catch(...) {failed=true;}
        require(failed && !fs::exists(temp/"out-bad-parent"),"有效孩子不能掩盖父内容转换失败");checks++;
        auto eventOptions=options(temp/"plain.osgb",temp/"out-events");std::vector<Json> events;
        eventOptions.event=[&](const Json& event) {events.push_back(event);};convert(eventOptions);
        require(events.front()["stage"]=="scan" && events.back()["stage"]=="done" && events.back()["done"]==1,"结构化进度必须覆盖扫描到发布");checks++;
        auto canceledOptions=options(temp/"plain.osgb",temp/"out-canceled");bool cancelRequested=false;
        canceledOptions.event=[&](const Json& event) {if(event["stage"]=="convert") cancelRequested=true;};
        canceledOptions.checkCancelled=[&] {if(cancelRequested) throw std::runtime_error("取消");};
        failed=false;try {convert(canceledOptions);} catch(...) {failed=true;}
        require(failed && !fs::exists(temp/"out-canceled"),"取消必须清理事务且不发布结果");checks++;
        require(capabilities()["defaults"]["geometry"]=="meshopt" && capabilities()["protocolVersion"]==1,"CLI 与 App 能力契约");checks++;
        // 空几何与空叶子 OSGB（如大疆智图边缘分块）应安全跳过，全空输入仍报错。
        {
            auto mixedGeode=new osg::Geode;
            auto emptyNoVert=new osg::Geometry;
            auto emptyZeroVert=new osg::Geometry;emptyZeroVert->setVertexArray(new osg::Vec3Array);
            auto emptyNoPrim=new osg::Geometry;
            auto dummyVerts=new osg::Vec3Array;dummyVerts->push_back({0,0,0});emptyNoPrim->setVertexArray(dummyVerts);
            mixedGeode->addDrawable(emptyNoVert);mixedGeode->addDrawable(emptyZeroVert);mixedGeode->addDrawable(emptyNoPrim);
            mixedGeode->addDrawable(triangle()->getDrawable(0));
            writeNode(mixedGeode,temp/"mixed-empty-geom.osgb");
            auto mixedReport=convert(options(temp/"mixed-empty-geom.osgb",temp/"out-mixed-empty-geom"));
            require(mixedReport["trianglesAllLods"]==1,"混有空几何的文件应跳过空几何并保留有效三角形");checks++;

            auto emptyLeaf=new osg::Geode;emptyLeaf->addDrawable(new osg::Geometry);
            writeNode(emptyLeaf,temp/"empty-leaf.osgb");
            auto parentWithEmptyLeaf=new osg::PagedLOD;
            parentWithEmptyLeaf->addChild(triangle(),0,128);
            parentWithEmptyLeaf->setFileName(1,"empty-leaf.osgb");parentWithEmptyLeaf->setRange(1,128,1e9);
            parentWithEmptyLeaf->setFileName(2,"plain.osgb");parentWithEmptyLeaf->setRange(2,128,1e9);
            writeNode(parentWithEmptyLeaf,temp/"parent-with-empty-leaf.osgb");
            auto leafOpts=options(temp/"parent-with-empty-leaf.osgb",temp/"out-parent-empty-leaf");
            leafOpts.preview=true;
            auto leafReport=convert(leafOpts);
            require(leafReport["files"]==2 && fs::exists(temp/"out-parent-empty-leaf/tileset.json") && fs::exists(temp/"out-parent-empty-leaf/preview/tileset.json"),
                "含空叶子 OSGB 的 PagedLOD 应跳过空叶子并正常发布主成果与预览");checks++;

            failed=false;
            try {convert(options(temp/"empty-leaf.osgb",temp/"out-only-empty-leaf"));} catch(...) {failed=true;}
            require(failed && !fs::exists(temp/"out-only-empty-leaf"),"全空输入必须报错且不发布输出");checks++;
        }
        std::cout<<"通过 "<<checks<<" 项原生回归；测试数据: "<<temp<<'\n';return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
