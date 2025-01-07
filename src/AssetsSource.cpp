#include<hgl/assets/AssetsSource.h>
#include<hgl/type/Map.h>

namespace hgl
{
    namespace assets
    {
        namespace
        {
            Map<U8String,AssetsSource *> assets_source_map;
        }//namespace

        bool RegistryAssetsSource(const U8String &uri_short_name,AssetsSource *as)
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

        void UnregistryAssetsSource(const U8String &uri_short_name)
        {
            if(uri_short_name.IsEmpty())
                return;

            assets_source_map.DeleteByKey(uri_short_name);
        }

        AssetsSource *GetSource(const U8String &uri_short_name)
        {
            if(uri_short_name.IsEmpty())
                return(nullptr);

            AssetsSource *as;

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

            AssetsSource *source=GetSource(uri);
            
            if(!source)
                return(nullptr);

            const U8String surl=uri.SubString(pos+3);

            return source->Open(surl);
        }
        
        AssetsSource::AssetsSource(const U8String &sn)
        {
            uri_short_name=sn;
            RegistryAssetsSource(sn,this);
        }

        AssetsSource::~AssetsSource()
        {
            UnregistryAssetsSource(uri_short_name);
        }
    }//namespace assets
}//namespace hgl