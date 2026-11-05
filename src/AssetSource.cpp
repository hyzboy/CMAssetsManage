#include<hgl/asset/AssetSource.h>
#include<hgl/sync/RWLock.h>
#include<hgl/type/UnorderedMap.h>

namespace hgl::asset
{
    namespace
    {
        RWLock                                  sources_lock;
        UnorderedMap<AnsiString,AssetSource *>  assets_source_map;
    }//namespace

    // ── 包裹内路径规范化 ────────────────────────────────────────────────────

    namespace
    {
        inline bool IsPathSeparator(const u8char ch)
        {
            return ch==u8char('/')||ch==u8char('\\');
        }
    }//namespace

    bool NormalizePackPath(const U8String &in,U8String &out)
    {
        out.Clear();

        const int len=in.Length();

        if(len<=0)
            return false;

        const u8char *src=in.c_str();

        if(!src)
            return false;

        // 绝对路径与盘符路径一律拒绝：包裹内路径必须落在挂载根之下。
        if(IsPathSeparator(src[0]))
            return false;

        if(len>=2&&src[1]==u8char(':'))
            return false;

        static const u8char kSlash[]={u8char('/'),0};

        U8String result;

        int seg_start=0;

        for(int i=0;i<=len;i++)
        {
            if(i<len&&!IsPathSeparator(src[i]))
                continue;

            const int seg_len=i-seg_start;

            if(seg_len>0)
            {
                const u8char *seg=src+seg_start;

                if(seg_len==1&&seg[0]==u8char('.'))
                {
                    // 忽略 "." 段
                }
                else if(seg_len==2&&seg[0]==u8char('.')&&seg[1]==u8char('.'))
                {
                    // 不允许越出挂载根
                    return false;
                }
                else
                {
                    if(!result.IsEmpty())
                        result+=U8String(kSlash,1);

                    result+=in.SubString(seg_start,seg_len);
                }
            }

            seg_start=i+1;
        }

        if(result.IsEmpty())
            return false;

        out=result;
        return true;
    }

    // ── AssetSource ─────────────────────────────────────────────────────────

    AssetSource::AssetSource(const AnsiString &sn)
    {
        uri_short_name=sn;
    }

    AssetSource::~AssetSource()=default;

    bool AssetSource::Exists(const U8String &path)const
    {
        SourceEntry entry;

        return Stat(path,entry);
    }

    bool AssetSource::Stat(const U8String &,SourceEntry &)const
    {
        return false;
    }

    io::InputStream *AssetSource::Open(const U8String &)
    {
        return nullptr;
    }

    int AssetSource::ListDirectory(const U8String &,ManagedArray<SourceEntry> &)const
    {
        return -1;
    }

    // ── 挂载表 ──────────────────────────────────────────────────────────────

    bool RegisterAssetsSource(const AnsiString &name,AssetSource *src)
    {
        if(!src||name.IsEmpty())
            return false;

        if(!(src->GetName()==name))
            return false;

        WriteLockGuard guard(sources_lock);

        if(assets_source_map.ContainsKey(name))
            return false;

        return assets_source_map.Add(name,src);
    }

    bool UnregisterAssetsSource(const AnsiString &name)
    {
        if(name.IsEmpty())
            return false;

        AssetSource *src=nullptr;

        {
            WriteLockGuard guard(sources_lock);

            if(!assets_source_map.Get(name,src))
                return false;

            assets_source_map.DeleteByKey(name);
        }

        delete src;
        return true;
    }

    AssetSource *GetSource(const AnsiString &uri_short_name)
    {
        if(uri_short_name.IsEmpty())
            return nullptr;

        ReadLockGuard guard(sources_lock);

        AssetSource *src=nullptr;

        assets_source_map.Get(uri_short_name,src);

        // 注意：返回的裸指针在另一线程注销同名来源后会失效。挂载/注销按约定
        // 只在启动期或明确的停帧窗口发生，运行期只读。
        return src;
    }

    int GetSourceCount()
    {
        ReadLockGuard guard(sources_lock);

        return assets_source_map.GetCount();
    }

    void ClearAssetsSource()
    {
        WriteLockGuard guard(sources_lock);

        for(auto &item:assets_source_map)
            delete item.second;

        assets_source_map.Clear();
    }
}//namespace hgl::asset
