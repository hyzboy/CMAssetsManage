﻿#ifndef HGL_ASSETS_SOURCE_INCLUDE
#define HGL_ASSETS_SOURCE_INCLUDE

#include<hgl/type/String.h>

namespace hgl
{
    namespace io
    {
        class InputStream;
    }//namespace io

    namespace assets
    {
        /**
        * 资产来源类型
        */
        enum class SourceType
        {
            Memory=0,               ///<内存
            Filesystem,             ///<文件系统
            Archive,                ///<打包/压缩档
            Database,               ///<数据库
            SaveDevice,             ///<存储设备
            StreamDevice,           ///<流式设备(如摄像头、麦克风)
            Network,                ///<网络

            ENUM_CLASS_RANGE(Memory,Network)
        };//enum class SourceType

        /**
         * 资产索引方法
         */
        enum class IndexType
        {
            Anonymous=0,          ///<匿名访问
            Name,                 ///<字符串名称
            ID,                   ///<ID
            Position,             ///<坐标访问(如图数据库)

            ENUM_CLASS_RANGE(Anonymous,Position)
        };//enum class IndexType

        /**
         * 资产来源
         */
        class AssetsSource
        {
            U8String uri_short_name;

        public:

            const U8String &GetURI()const{return uri_short_name;}

        public:

            AssetsSource(const U8String &);
            virtual ~AssetsSource();

            virtual bool hasAnonymousAccess ()const{return false;}
            virtual bool hasNameAccess      ()const{return false;}
            virtual bool hasIDAccess        ()const{return false;}
            virtual bool hasPositionAccess  ()const{return false;}

        public:

            virtual io::InputStream *   Open            (const U8String &){return nullptr;}
            virtual AssetsSource *      CreateSubSource (const U8String &){return nullptr;}
        };//class AssetsSource

        AssetsSource *CreateSourceByFilesystem(const U8String &sn,const OSString &pathname,const bool only_read);

        AssetsSource *GetSource(const U8String &uri_short_name);

        io::InputStream *GetAssets(const U8String &uri);
    }//namespace assets
}//namespace hgl
#endif//HGL_ASSETS_SOURCE_INCLUDE
