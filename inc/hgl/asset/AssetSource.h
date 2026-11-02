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
        enum class AssetIndexType
        {
            Unknow=0,                   ///<未知类型
            ID,                         ///<ID
            Name,                       ///<字符串名称

            ENUM_CLASS_RANGE(ID,Name)
        };//enum class AssetIndexType

        /**
         * 资产来源
         *
         * ## 与 `AssetURI` 的分工
         *
         * `AssetSource` 是**可插拔的 IO 后端**：给定短名 `sn`（如 `u8"game"`）与一个根，
         * 回答「字节流从哪来、能不能匿名/按名/按 ID/按位置取」。
         * `AssetURI`（`<hgl/asset/AssetURI.h>`）是**定位语法**：把
         * `type://source/target` 拆成 `type` / `source` / `target` 三段并归类。
         *
         * 二者是对偶而非重叠：`AssetURI::source` 只是一段短名字符串，`AssetSource`
         * 是这个短名背后的实现。解析 URI 一律走 `AssetURI::Parse`，**不要在这里重复解析**。
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
    }//namespace asset
}//namespace hgl
