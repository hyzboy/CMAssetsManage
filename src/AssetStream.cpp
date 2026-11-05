#include<hgl/asset/AssetStream.h>
#include<hgl/asset/AssetSource.h>
#include<hgl/io/FileInputStream.h>
#include<hgl/Charset.h>

namespace hgl::asset
{
    namespace
    {
        inline io::InputStream *Fail(AnsiString *out_error,const char *reason)
        {
            if(out_error)
                *out_error=reason;

            return nullptr;
        }

        io::InputStream *OpenLocalFile(const OSString &filename,AnsiString *out_error)
        {
            io::FileInputStream *fis=new io::FileInputStream;

            if(fis->Open(filename))
                return fis;

            delete fis;

            return Fail(out_error,"cannot open file");
        }
    }//namespace

    io::InputStream *OpenAssetStream(const AssetURI &uri,AnsiString *out_error)
    {
        if(out_error)
            out_error->Clear();

        if(!uri.IsValid())
            return Fail(out_error,"empty target");

        switch(uri.source)
        {
            case AssetSourceKind::Pack:
            {
                AssetSource *src=GetSource(uri.pack_name);

                if(!src)
                    return Fail(out_error,"pack not mounted");

                U8String rel;

                if(!NormalizePackPath(ToU8String(uri.target),rel))
                    return Fail(out_error,"invalid path inside pack");

                io::InputStream *is=src->Open(rel);

                if(!is)
                    return Fail(out_error,"cannot open file in pack");

                return is;
            }

            case AssetSourceKind::File:
            case AssetSourceKind::Unknown:
                return OpenLocalFile(uri.target,out_error);

            default:
                // builtin / rt：不是字节流来源，交给各自的 loader 处理。
                return Fail(out_error,"source is not a byte stream");
        }
    }

    io::InputStream *OpenAssetStream(const OSString &uri_text,
                                     AssetType default_type,
                                     AnsiString *out_error)
    {
        return OpenAssetStream(AssetURI::Parse(uri_text,default_type),out_error);
    }
}//namespace hgl::asset
