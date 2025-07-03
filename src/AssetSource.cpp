#include<hgl/asset/AssetSource.h>
#include<hgl/type/Map.h>

namespace hgl::asset
{
    namespace
    {
        Map<U8String,AssetSource *> assets_source_map;
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

    io::InputStream *GetAssets(const U8String &uri)
    {
        int pos=uri.FindChar(':');

        if(pos<=0)return(nullptr);

        if(uri.Comp(pos,u8"://",3))
            return(nullptr);

        const U8String sn=uri.SubString(0,pos);

        AssetSource *source=GetSource(uri);
            
        if(!source)
            return(nullptr);

        const U8String surl=uri.SubString(pos+3);

        return source->Open(surl);
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
