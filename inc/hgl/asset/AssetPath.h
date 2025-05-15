#pragma once

#include<hgl/type/String.h>

namespace hgl::asset
{
    /**
    * 资产位置枚举
    */
    enum class Location
    {
        Asset=0,
        Engine,
        PlugIn,
        ExtPack,
        OS,
    };//

    /**
     * 资产路径
     * @see doc/AssetPath.md
     */
    class AssetPath
    {
        hgl::asset::Location location;///< 资产位置
        
        hgl::U8String name;           ///< 资产名称
        hgl::U8String path;           ///< 资产路径

        hgl::U8String full_path;      ///< 资产完整路径名

    public:

        AssetPath()
        {
            location=hgl::asset::Location::Asset;
            name.Clear();
            path.Clear();
            full_path.Clear();
        }
        AssetPath(const AssetPath &ap)
        {
            location=ap.location;
            name=ap.name;
            path=ap.path;
            full_path=ap.full_path;
        }
        ~AssetPath()=default;

        void SetLocation(hgl::asset::Location loc) { location=loc; }
        hgl::asset::Location GetLocation()const { return location; }

        const hgl::U8String &GetName()const { return name; }
        const hgl::U8String &GetPath()const { return path; }
        const hgl::U8String &GetFullPath()const { return full_path; }

        const bool FromString(const U8String &)
        {

        }

    public:

    };//class AssetPath
}//namespace hgl::asset
