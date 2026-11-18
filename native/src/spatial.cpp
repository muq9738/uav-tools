#include "native_tiles.hpp"
#include <proj.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

namespace nt {
namespace {
using ProjObject=std::unique_ptr<PJ,decltype(&proj_destroy)>;
ProjObject object(PJ* value) {return ProjObject(value,proj_destroy);}
bool finite(const osg::Vec3d& point) {return std::isfinite(point.x())&&std::isfinite(point.y())&&std::isfinite(point.z());}
// BoundCRS 的轴和基准由其源 CRS 定义，不能当成目标 WGS84 的轴。
ProjObject sourceCrs(PJ_CONTEXT* context,const PJ* crs) {
    return object(proj_get_type(crs)==PJ_TYPE_BOUND_CRS?proj_get_source_crs(context,crs):proj_clone(context,crs));
}
ProjObject operation(PJ_CONTEXT* context,const PJ* source,const PJ* target) {
    const char* options[]={"ALLOW_BALLPARK=NO","ONLY_BEST=YES",nullptr};
    auto raw=object(proj_create_crs_to_crs_from_pj(context,source,target,nullptr,options));
    if(!raw) throw std::runtime_error("无法建立 PROJ 坐标变换（不允许 ballpark 或缺失格网）");
    auto normalized=object(proj_normalize_for_visualization(context,raw.get()));
    if(!normalized) throw std::runtime_error("无法规范 PROJ 轴顺序");return normalized;
}
osg::Vec3d transform(PJ* operation,const osg::Vec3d& point) {
    if(!finite(point)) throw std::runtime_error("PROJ 输入坐标非有限");
    proj_errno_reset(operation);
    auto value=proj_trans(operation,PJ_FWD,proj_coord(point.x(),point.y(),point.z(),0));
    osg::Vec3d result(value.xyz.x,value.xyz.y,value.xyz.z);
    if(proj_errno(operation)||!finite(result)) throw std::runtime_error("PROJ 坐标转换失败（坐标越界或所需格网不可用）");
    return result;
}
}
struct SpatialReference::Impl {
    PJ_CONTEXT* context=nullptr;
    PJ* operation=nullptr;
    osg::Vec3d origin,ecefOrigin,derivativeStep;
    osg::Matrixd placement,inverse;
    bool isProjected=false,isGeographic=false;
    ~Impl() {if(operation) proj_destroy(operation);if(context) proj_context_destroy(context);}
};
SpatialReference::SpatialReference(const std::string& srs,const osg::Vec3d& origin):impl_(new Impl) {
    if(!finite(origin)) throw std::runtime_error("SRSOrigin 必须为有限坐标");
    impl_->origin=origin;
    if(srs.rfind("ENU:",0)==0) {
        auto csv=srs.substr(4);std::stringstream in(csv);std::string text;std::vector<double> values;
        if(csv.empty()||csv.back()==',') throw std::runtime_error("ENU 必须为纬度,经度[,高度]");
        while(std::getline(in,text,',')) {
            std::size_t consumed=0;double value=std::stod(text,&consumed);
            if(!std::isfinite(value)||text.find_first_not_of(" \t\r\n",consumed)!=std::string::npos)
                throw std::runtime_error("无效 ENU 坐标数值");
            values.push_back(value);
        }
        if(values.size()<2||values.size()>3) throw std::runtime_error("ENU 必须为纬度,经度[,高度]");
        impl_->placement=enuTransform(values[0],values[1],values.size()==3?values[2]:0,origin);
    } else {
        if(srs.empty()) throw std::runtime_error("缺少 SRS，拒绝猜测坐标系");
        impl_->isProjected=true;impl_->context=proj_context_create();
        if(!impl_->context) throw std::runtime_error("无法创建 PROJ 上下文");
        auto context=impl_->context;proj_context_set_enable_network(context,0);
#ifdef NATIVE_PROJ_DATA
        const char* paths[]={NATIVE_PROJ_DATA};proj_context_set_search_paths(context,1,paths);
#endif
        auto source=object(proj_create(context,srs.c_str()));
        if(!source||!proj_is_crs(source.get())) throw std::runtime_error("SRS 不是可识别的 CRS: "+srs);
        auto horizontal=sourceCrs(context,source.get());
        if(!horizontal) throw std::runtime_error("无法读取源 CRS");
        auto type=proj_get_type(horizontal.get());
        if(type!=PJ_TYPE_GEOGRAPHIC_2D_CRS&&type!=PJ_TYPE_GEOGRAPHIC_3D_CRS&&type!=PJ_TYPE_PROJECTED_CRS&&type!=PJ_TYPE_GEOCENTRIC_CRS)
            throw std::runtime_error("不支持垂直、复合、工程或时间 CRS；不猜测高程基准: "+srs);
        auto datum=object(proj_crs_get_datum_forced(context,horizontal.get()));
        if(datum&&proj_get_type(datum.get())==PJ_TYPE_DYNAMIC_GEODETIC_REFERENCE_FRAME)
            throw std::runtime_error("动态 CRS 需要坐标历元，当前不支持: "+srs);
        // PROJ >=9.1 的二维→三维变换未必转换高度。明确将未定义的 z 约定为源椭球高（米）。
        auto cs=object(proj_crs_get_coordinate_system(context,horizontal.get()));
        if(!cs) throw std::runtime_error("无法读取 CRS 坐标轴");
        if(proj_cs_get_axis_count(context,cs.get())==2) {
            auto promoted=object(proj_crs_promote_to_3D(context,nullptr,source.get()));
            if(!promoted) throw std::runtime_error("无法提升 CRS 到三维椭球高");source=std::move(promoted);
        }
        auto normalized=object(proj_normalize_for_visualization(context,source.get()));
        if(!normalized) throw std::runtime_error("无法规范源 CRS 轴顺序");
        auto normalizedSource=sourceCrs(context,normalized.get());
        cs=object(normalizedSource?proj_crs_get_coordinate_system(context,normalizedSource.get()):nullptr);
        if(!cs||proj_cs_get_axis_count(context,cs.get())!=3) throw std::runtime_error("源 CRS 必须有三个空间轴");
        impl_->isGeographic=type==PJ_TYPE_GEOGRAPHIC_2D_CRS||type==PJ_TYPE_GEOGRAPHIC_3D_CRS;
        for(unsigned i=0;i<3;++i) {
            double unit=0;
            if(!proj_cs_get_axis_info(context,cs.get(),i,nullptr,nullptr,nullptr,&unit,nullptr,nullptr,nullptr)||!std::isfinite(unit)||unit<=0)
                throw std::runtime_error("无法确定源 CRS 轴单位");
            // 约一米的源轴步长；经纬轴的 SI 单位为弧度，其余轴为米。
            impl_->derivativeStep[i]=1/(unit*(impl_->isGeographic&&i<2?6378137.0:1));
        }
        auto ecef=object(proj_create(context,"EPSG:4978"));auto geographic=object(proj_create(context,"EPSG:4979"));
        if(!ecef||!geographic) throw std::runtime_error("缺少 WGS84 CRS 定义");
        impl_->operation=operation(context,source.get(),ecef.get()).release();
        impl_->ecefOrigin=transform(impl_->operation,origin);
        // 只用同一 ECEF 操作定位原点，避免两个独立源→目标操作的高度/基准语义不同。
        auto toGeographic=operation(context,ecef.get(),geographic.get());auto location=transform(toGeographic.get(),impl_->ecefOrigin);
        impl_->placement=enuTransform(location.y(),location.x(),location.z(),{0,0,0});
        impl_->placement.setTrans(impl_->ecefOrigin);
    }
    if(!impl_->inverse.invert(impl_->placement)) throw std::runtime_error("无效空间基准矩阵");
}
SpatialReference::~SpatialReference()=default;
bool SpatialReference::projected() const {return impl_->isProjected;}
const osg::Matrixd& SpatialReference::placement() const {return impl_->placement;}
osg::Vec3d SpatialReference::toLocal(const osg::Vec3d& source) const {
    if(!impl_->isProjected) return source;
    // 先在 ECEF 相减再旋转，减少两次大数点乘后相消造成的误差。
    return osg::Matrixd::transform3x3(transform(impl_->operation,source+impl_->origin)-impl_->ecefOrigin,impl_->inverse);
}
osg::Vec3d SpatialReference::normalToLocal(const osg::Vec3d& point,const osg::Vec3d& normal) const {
    if(!impl_->isProjected) return normal;
    osg::Vec3d columns[3],absolute=point+impl_->origin;
    for(unsigned i=0;i<3;++i) {
        double h=impl_->derivativeStep[i];
        if(impl_->isGeographic&&i==1) h=std::min(h,(90-std::abs(absolute.y()))*.5);
        auto plus=absolute,minus=absolute;plus[i]+=h;minus[i]-=h;
        double distance=plus[i]-minus[i];
        if(!std::isfinite(distance)||distance<=0) throw std::runtime_error("投影处的法线差分退化或位于极点");
        auto derivative=(transform(impl_->operation,plus)-transform(impl_->operation,minus))/distance;
        columns[i]=osg::Matrixd::transform3x3(derivative,impl_->inverse);
    }
    osg::Matrixd jacobian(columns[0].x(),columns[0].y(),columns[0].z(),0,
        columns[1].x(),columns[1].y(),columns[1].z(),0,columns[2].x(),columns[2].y(),columns[2].z(),0,0,0,0,1);
    osg::Matrixd inverse;if(!inverse.invert(jacobian)) throw std::runtime_error("投影处的法线变换退化");
    osg::Matrixd inverseTranspose;inverseTranspose.transpose(inverse);
    auto result=osg::Matrixd::transform3x3(normal,inverseTranspose);result.normalize();
    if(!finite(result)) throw std::runtime_error("投影法线包含非有限数值");return result;
}
}
