#include<hgl/asset/AssetByteSource.h>
#include<hgl/asset/AssetStream.h>
#include<hgl/asset/AssetURI.h>
#include<hgl/io/InputStream.h>

namespace hgl
{
    namespace asset
    {
        AssetURIByteSource::AssetURIByteSource(const OSString &route,const AssetURI &asset_uri):
            route_path(route),
            uri(asset_uri)
        {
        }

        AssetURIByteSource::~AssetURIByteSource()
        {
            if(stream)
            {
                delete stream;
                stream=nullptr;
            }
        }

        int64 AssetURIByteSource::GetSize()
        {
            if(!stream)
            {
                stream=OpenAssetStream(uri);
                if(!stream)
                    return(-1);
            }

            return stream->GetSize();
        }

        int64 AssetURIByteSource::Read(uint8 *buffer,int64 size)
        {
            if(!stream)
                return(0);

            return stream->Read(buffer,size);
        }
    }//namespace asset
}//namespace hgl
