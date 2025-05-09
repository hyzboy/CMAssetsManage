#pragma once

#include<hgl/type/String.h>

namespace hgl
{
    namespace io
    {
        class InputStream;
    }//namespace io

    namespace asset
    {
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
        class AssetSource
        {
            U8String uri_short_name;

        public:

            const U8String &GetURI()const{return uri_short_name;}

        public:

            AssetSource(const U8String &);
            virtual ~AssetSource();

            virtual bool hasAnonymousAccess ()const{return false;}
            virtual bool hasNameAccess      ()const{return false;}
            virtual bool hasIDAccess        ()const{return false;}
            virtual bool hasPositionAccess  ()const{return false;}

        public:

            virtual io::InputStream *   Open            (const U8String &){return nullptr;}
            virtual AssetSource *       CreateSubSource (const U8String &){return nullptr;}
        };//class AssetSource

        AssetSource *CreateSourceByFilesystem(const U8String &sn,const OSString &pathname,const bool only_read);

        AssetSource *GetSource(const U8String &uri_short_name);

        io::InputStream *GetAssets(const U8String &uri);
    }//namespace asset
}//namespace hgl
