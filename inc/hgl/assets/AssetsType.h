#ifndef HGL_ASSETS_TYPE_INCLUDE
#define HGL_ASSETS_TYPE_INCLUDE

#include<hgl/TypeFunc.h>
namespace hgl
{
    /**
     * 资产类型
     */
    enum class AssetsType
    {
        Binary=0,               ///<二进制数据

        Text,                   ///<文本
        Bitmap,                 ///<位图
        Vector,                 ///<矢量图
        MIDI,                   ///<MIDI音乐
        Wave,                   ///<声音数据
        Video,                  ///<视频数据

        ENUM_CLASS_RANGE(Binary,Video)
    };//enum class AssetsType
}//namespace hgl
#endif//HGL_ASSETS_TYPE_INCLUDE
