#include<hgl/asset/AssetStream.h>
#include<hgl/asset/AssetSource.h>
#include<hgl/asset/AssetIndex.h>
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

                const U8String raw_target=ToU8String(uri.target);

                // ── 1. 按身份寻址（`id:<32 hex>`）──────────────────────────
                // 这条路**不**做任何回落：ID 查不到就是不存在，否则「引用失效」
                // 会被悄悄替换成「读到了另一个资产」，那是最难查的一类错误。
                AssetID id;

                if(ParseAssetIDTarget(raw_target,id))
                {
                    io::InputStream *is=src->Open(id);

                    if(!is)
                        return Fail(out_error,"asset id not found in pack");

                    return is;
                }

                // ── 2. 按逻辑位置寻址 ─────────────────────────────────────
                U8String norm;

                if(!NormalizePackPath(raw_target,norm))
                    return Fail(out_error,"invalid path inside pack");

                // 带扩展名的历史写法在这里被推导成同一个逻辑路径：
                // `models/hero.geo.toml` 与 `models/hero` 指向同一个资产。
                AssetType  logical_type=uri.type;
                U8String   logical_path;

                const bool derived=DeriveLogical(norm,
                                                 uri.type,
                                                 logical_type,
                                                 logical_path);

                if(derived)
                {
                    io::InputStream *is=src->Open(logical_type,logical_path);

                    if(is)
                        return is;

                    // 索引命中失败后**只**在一种情况下继续：后端根本没有索引。
                    // 有索引却查不到，说明这个路径确实不是一个资产 —— 此时回落成
                    // 「按路径摸文件」会让开发期与发行期的行为分叉（发行包里
                    // 没有目录树可摸），所以宁可当场失败。
                    if(src->HasIndexTable())
                        return Fail(out_error,"asset not found in pack index");
                }
                else if(src->HasIndexTable())
                {
                    // 推导不出类型（`data/blob.bin` 这类非资产资源）：
                    // 索引里不会有它，但它在包裹里是真实存在的原始文件。
                    // 这是一条**明确的**「按路径直通」通道，不是试探 ——
                    // 路径就是调用方给的那一个，没有候选后缀轮询。
                }

                io::InputStream *is=src->Open(norm);

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
