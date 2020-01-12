#ifndef HGL_ASSETS_SOURCE_INCLUDE
#define HGL_ASSETS_SOURCE_INCLUDE

namespace hgl
{
    /**
     * 资产来源
     */
    enum class AssetsSource
    {
        Memory=0,               ///<内存
        Filesystem,             ///<文件系统
        Archive,                ///<打包/压缩档
        Database,               ///<数据库
        SaveDevice,             ///<存储设备
        StreamDevice,           ///<流式设备(如摄像头、麦克风)
        Network,                ///<网络

        BEGIN_RANGE =Memory,
        END_RANGE   =Network,
        RANGE_SIZE  =(END_RANGE-BEGIN_RANGE)+1
    };//enum class AssetsSource
}//namespace hgl
#endif//HGL_ASSETS_SOURCE_INCLUDE
