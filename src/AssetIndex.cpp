#include<hgl/asset/AssetIndex.h>
#include<hgl/utf.h>

namespace hgl::asset
{
    namespace
    {
        /// 单个后缀的剥离上限。合法资产名最多「类型 + 格式」两层，
        /// 给到 4 是留给 `hero.geo.toml.bak` 这类夹生文件一个确定的收手点。
        constexpr int kMaxStripRounds = 4;

        inline bool IsPathSeparator(const u8char ch)
        {
            return ch==u8char('/')||ch==u8char('\\');
        }

        /// 把 `u8char` 段窄化成 `AnsiString`（后缀名恒为 ASCII；非 ASCII 直接视为非法）
        bool SuffixToAnsi(const u8char *str,int length,AnsiString &out)
        {
            out.Clear();

            if(!str||length<=0||length>16)
                return false;

            char buf[17];

            for(int i=0;i<length;i++)
            {
                const u8char ch=str[i];

                if(ch==0||ch>0x7F)
                    return false;

                buf[i]=char(ch);
            }

            buf[length]=0;

            out=AnsiString(buf,length);
            return true;
        }

        /**
         * 找出最后一节（文件名）里最后一个 '.' 的位置，返回相对 basename 起的偏移。
         *
         * 只认文件名里的点：`a.b/hero` 的最后一个点属于目录名，不是后缀。
         * 结果 -1 表示没有后缀。
         */
        int FindLastSuffixDot(const u8char *str,const int length)
        {
            if(!str||length<=0)
                return -1;

            int basename_start=0;

            for(int i=0;i<length;i++)
                if(IsPathSeparator(str[i]))
                    basename_start=i+1;

            int dot=-1;

            for(int i=basename_start;i<length;i++)
                if(str[i]==u8char('.'))
                    dot=i;

            // 开头的点（`.asset_manifest`）不是后缀分隔符，是隐藏文件名的一部分。
            if(dot<=basename_start)
                return -1;

            // 末尾的点（`hero.`）同样不构成后缀。
            if(dot>=length-1)
                return -1;

            return dot;
        }
    }//namespace

    bool IsAssetFormatSuffix(const U8String &suffix)
    {
        if(suffix.IsEmpty())
            return false;

        static const U8String kFormats[] =
        {
            U8String(u8"toml"),
            U8String(u8"fb"),
            U8String(u8"flat"),
            U8String(u8"bin"),
        };

        for(const U8String &fmt : kFormats)
            if(hgl::stricmp(suffix.c_str(),fmt.c_str())==0)
                return true;

        return false;
    }

    bool DeriveLogical(const U8String &physical_path, AssetType declared_type,
                       AssetType &out_type, U8String &out_logical)
    {
        out_type   = AssetType::Unknown;
        out_logical.Clear();

        if(physical_path.IsEmpty())
            return false;

        U8String work = physical_path;

        // 统一分隔符：后端给的是物理路径，'\\' 与 '/' 都可能出现。
        for(int i=0;i<work.Length();i++)
            if(work[i]==u8char('\\'))
                work[i]=u8char('/');

        AssetType type = declared_type;

        for(int round=0;round<kMaxStripRounds;round++)
        {
            const int length = work.Length();
            const int dot    = FindLastSuffixDot(work.c_str(),length);

            if(dot<0)
                break;

            const u8char *suffix_str = work.c_str()+dot+1;
            const int     suffix_len = length-dot-1;

            AnsiString suffix;

            if(!SuffixToAnsi(suffix_str,suffix_len,suffix))
                break;

            if(IsAssetFormatSuffix(U8String(suffix_str,suffix_len)))
            {
                // 第一优先：格式后缀纯粹是物理编码细节，无条件剥掉。
                work = work.SubString(0,dot);
                continue;
            }

            const AssetType suffix_type = ParseAssetType(suffix);

            if(suffix_type!=AssetType::Unknown)
            {
                // 第二优先：类型后缀。只在类型尚未确定时采信，但无论采信与否都剥掉 ——
                // 它确实是文件名的装饰，不是名字的一部分。
                if(type==AssetType::Unknown)
                    type=suffix_type;

                work = work.SubString(0,dot);
                continue;
            }

            // 既不是格式也不是类型：这是名字本身的点（`hero.v2`），到此为止。
            break;
        }

        if(work.IsEmpty())
            return false;

        if(type==AssetType::Unknown)
            return false;

        out_type   = type;
        out_logical= work;
        return true;
    }

    U8String MakeIndexPathKey(AssetType type, const U8String &logical_path)
    {
        if(logical_path.IsEmpty())
            return U8String();

        // `Unknown` 不是一种资产类型，而 `GetAssetTypeName` 对它返回 "unknown"
        // 这个兜底串。若放行，`unknown:x` 会成为一张能被查询命中的真实键，
        // 于是「类型没推导出来的条目」就悄悄混进了索引。这里直接堵死。
        if(type==AssetType::Unknown)
            return U8String();

        const char *type_name = GetAssetTypeName(type);

        if(!type_name||!*type_name)
            return U8String();

        U8String key(reinterpret_cast<const u8char *>(type_name));

        key += U8String(u8":");

        key += logical_path;

        return key;
    }

    // ── AssetIndexTable ────────────────────────────────────────────────────

    int AssetIndexTable::GetCount() const
    {
        ReadLockGuard guard(lock);

        return by_id.GetCount();
    }

    bool AssetIndexTable::IsEmpty() const
    {
        ReadLockGuard guard(lock);

        return by_id.IsEmpty();
    }

    void AssetIndexTable::Clear()
    {
        WriteLockGuard guard(lock);

        by_id.Clear();
        by_path.Clear();
    }

    AnsiString AssetIndexTable::GetHashAlgorithm() const
    {
        ReadLockGuard guard(lock);

        return hash_algorithm;
    }

    void AssetIndexTable::SetHashAlgorithm(const AnsiString &algo)
    {
        WriteLockGuard guard(lock);

        hash_algorithm=algo;
    }

    bool AssetIndexTable::AddOrUpdate(const AssetIndexEntry &entry)
    {
        if(entry.logical_path.IsEmpty()||entry.physical_path.IsEmpty())
            return false;

        if(entry.id.IsZero())
            return false;

        const U8String key = MakeIndexPathKey(entry.type,entry.logical_path);

        if(key.IsEmpty())
            return false;

        WriteLockGuard guard(lock);

        // ── 先拆掉所有指向同一个资产、但键已经过期的旧映射 ────────────────
        AssetIndexEntry old_entry;

        if(by_id.Get(entry.id,old_entry))
        {
            const U8String old_key = MakeIndexPathKey(old_entry.type,old_entry.logical_path);

            if(!(old_key==key))
                by_path.DeleteByKey(old_key);
        }

        AssetID old_id;

        if(by_path.Get(key,old_id))
        {
            if(!(old_id==entry.id))
                by_id.DeleteByKey(old_id);
        }

        if(!by_id.Update(entry.id,entry))
            by_id.Add(entry.id,entry);

        if(!by_path.Update(key,entry.id))
            by_path.Add(key,entry.id);

        return true;
    }

    bool AssetIndexTable::RemoveByID(const AssetID &id)    {
        if(id.IsZero())
            return false;

        WriteLockGuard guard(lock);

        AssetIndexEntry entry;

        if(!by_id.Get(id,entry))
            return false;

        by_path.DeleteByKey(MakeIndexPathKey(entry.type,entry.logical_path));

        return by_id.DeleteByKey(id);
    }

    bool AssetIndexTable::RemoveByPath(AssetType type,const U8String &logical_path)
    {
        const U8String key = MakeIndexPathKey(type,logical_path);

        if(key.IsEmpty())
            return false;

        WriteLockGuard guard(lock);

        AssetID id;

        if(!by_path.Get(key,id))
            return false;

        by_id.DeleteByKey(id);

        return by_path.DeleteByKey(key);
    }

    bool AssetIndexTable::CopyFrom(const AssetIndexTable &other)
    {
        if(this==&other)
            return true;

        // 先读对方、再写自己：锁顺序固定（先 other 后 this），本函数内部也不会
        // 反向再加一次，因此不会与另一线程的 CopyFrom 互锁。
        ReadLockGuard  read_guard(other.lock);
        WriteLockGuard write_guard(lock);

        by_id.ClearAndDeallocate();
        by_path.ClearAndDeallocate();

        for(const auto &item : other.by_id)
            by_id.Add(item.first,item.second);

        for(const auto &item : other.by_path)
            by_path.Add(item.first,item.second);

        hash_algorithm=other.hash_algorithm;

        return true;
    }

    bool AssetIndexTable::FindByID(const AssetID &id,AssetIndexEntry &out) const
    {
        if(id.IsZero())
            return false;

        ReadLockGuard guard(lock);

        return by_id.Get(id,out);
    }

    bool AssetIndexTable::FindByPath(AssetType type,const U8String &logical_path,AssetIndexEntry &out) const
    {
        return FindByKey(MakeIndexPathKey(type,logical_path),out);
    }

    bool AssetIndexTable::FindByKey(const U8String &path_key,AssetIndexEntry &out) const
    {
        if(path_key.IsEmpty())
            return false;

        ReadLockGuard guard(lock);

        AssetID id;

        if(!by_path.Get(path_key,id))
            return false;

        return by_id.Get(id,out);
    }

    bool AssetIndexTable::ContainsID(const AssetID &id) const
    {
        if(id.IsZero())
            return false;

        ReadLockGuard guard(lock);

        return by_id.ContainsKey(id);
    }

    bool AssetIndexTable::ContainsPath(AssetType type,const U8String &logical_path) const
    {
        const U8String key = MakeIndexPathKey(type,logical_path);

        if(key.IsEmpty())
            return false;

        ReadLockGuard guard(lock);

        return by_path.ContainsKey(key);
    }

    int AssetIndexTable::FindByLogicalPath(const U8String &logical_path,ManagedArray<AssetIndexEntry> &out) const
    {
        if(logical_path.IsEmpty())
            return 0;

        ReadLockGuard guard(lock);

        int found = 0;

        for(const auto &item : by_id)
        {
            if(!(item.second.logical_path==logical_path))
                continue;

            out.Create(item.second);
            ++found;
        }

        return found;
    }

    int AssetIndexTable::GetAllEntries(ManagedArray<AssetIndexEntry> &out) const
    {
        ReadLockGuard guard(lock);

        for(const auto &item : by_id)
            out.Create(item.second);

        return by_id.GetCount();
    }
}//namespace hgl::asset
