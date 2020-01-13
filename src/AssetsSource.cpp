#include<hgl/assets/AssetsSource.h>
#include<hgl/type/Map.h>

namespace hgl
{
    namespace assets
    {
        namespace
        {
            Map<UTF8String,AssetsSource *> assets_source_map;
        }//namespace

        bool RegistryAssetsSource(const UTF8String &uri_short_name,AssetsSource *as)
        {
            if(!as)
                return(false);

            if(uri_short_name.IsEmpty())
                return(false);

            if(assets_source_map.KeyExist(uri_short_name))
                return(false);

            assets_source_map.Add(uri_short_name,as);
            return(true);
        }

        void UnregistryAssetsSource(const UTF8String &uri_short_name)
        {
            if(uri_short_name.IsEmpty())
                return;

            assets_source_map.DeleteByKey(uri_short_name);
        }

        AssetsSource *GetSource(const UTF8String &uri_short_name)
        {
            if(uri_short_name.IsEmpty())
                return(nullptr);

            AssetsSource *as;

            if(assets_source_map.Get(uri_short_name,as))
                return as;

            return(nullptr);
        }        

        io::InputStream *GetAssets(const UTF8String &uri)
        {
            int pos=uri.FindChar(':');

            if(pos<=0)return(nullptr);

            if(uri.Comp(pos,"://",3))
                return(nullptr);

            const UTF8String sn=uri.SubString(0,pos);

            AssetsSource *source=GetSource(uri);
            
            if(!source)
                return(nullptr);

            const UTF8String surl=uri.SubString(pos+3);

            return source->OpenByName(surl);
        }
        
        AssetsSource::AssetsSource(const UTF8String &sn)
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