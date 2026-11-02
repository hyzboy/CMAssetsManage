#include<hgl/asset/AssetSource.h>
#include<hgl/type/UnorderedMap.h>

namespace hgl::asset
{
    namespace
    {
        UnorderedMap<U8String,AssetSource *> assets_source_map;
    }//namespace

    bool RegisterAssetsSource(const U8String &uri_short_name,AssetSource *as)
    {
        if(!as)
            return(false);

        if(uri_short_name.IsEmpty())
            return(false);

        if(assets_source_map.ContainsKey(uri_short_name))
            return(false);

        assets_source_map.Add(uri_short_name,as);
        return(true);
    }

    void UnregisterAssetsSource(const U8String &uri_short_name)
    {
        if(uri_short_name.IsEmpty())
            return;

        assets_source_map.DeleteByKey(uri_short_name);
    }

    AssetSource *GetSource(const U8String &uri_short_name)
    {
        if(uri_short_name.IsEmpty())
            return(nullptr);

        AssetSource *as;

        if(assets_source_map.Get(uri_short_name,as))
            return as;

        return(nullptr);
    }

    AssetSource::AssetSource(const U8String &sn)
    {
        uri_short_name=sn;
        RegisterAssetsSource(sn,this);
    }

    AssetSource::~AssetSource()
    {
        UnregisterAssetsSource(uri_short_name);
    }
}//namespace hgl::asset
