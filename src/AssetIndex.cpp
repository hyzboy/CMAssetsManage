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

        /**
         * 磁盘命名的历史别名：引擎既有产物用长拼写装饰类型
         * （`.recipe.toml` / `.material_data.toml` / `.primitive.toml`），
         * 纹理容器则用格式化的历史拼写作类型后缀（`.Tex2D` / `.TexCube` /
         * `.TexOct`）。
         *
         * 只作用于「扫盘进索引」这一条路径 —— URI 类型段仍只认唯一正字拼写
         * （`rcp`/`mtldata`/`prim`），`ParseAssetType` 的契约不动。否则
         * 自包含模型的配方/数据块在索引里根本不存在，上层内存探索必然空手。
         *
         * `keep_suffix`：容器拼写是资产名的一部分，**不剥离**。同名不同容器
         * 是真实存在的不同资产（`sky.TexCube` 与 `sky.TexOct` 内容完全不同），
         * 剥掉就会在索引里互相冲突；保留它也让「无扩展名引用」明确不可达，
         * 与引擎按后缀分派 loader 的现状一致。
         */
        AssetType ParseDiskTypeAlias(const AnsiString &suffix, bool &out_keep_suffix)
        {
            struct DiskAlias { const char *name; AssetType type; bool keep_suffix; };

            static const DiskAlias kAliases[] =
            {
                { "recipe",        AssetType::MaterialRecipe, false },
                { "material_data", AssetType::MaterialData,   false },
                { "primitive",     AssetType::Primitive,      false },
                { "geometry",      AssetType::Geometry,       false },
                { "Tex2D",         AssetType::Texture,        true  },
                { "TexCube",       AssetType::Texture,        true  },
                { "TexOct",        AssetType::Texture,        true  },
            };

            for(const DiskAlias &a : kAliases)
                if(hgl::stricmp(suffix.c_str(),a.name)==0)
                {
                    out_keep_suffix = a.keep_suffix;
                    return a.type;
                }

            out_keep_suffix = false;
            return AssetType::Unknown;
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

    bool IsSourceFormatSuffix(const U8String &suffix)
    {
        if(suffix.IsEmpty())
            return false;
        return hgl::stricmp(suffix.c_str(),u8"toml")==0;
    }

    bool IsBakedFormatSuffix(const U8String &suffix)
    {
        if(suffix.IsEmpty())
            return false;

        static const U8String kBakedFormats[] =
        {
            U8String(u8"fb"),
            U8String(u8"flat"),
            U8String(u8"bin"),
        };

        for(const U8String &fmt : kBakedFormats)
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

            AssetType suffix_type = ParseAssetType(suffix);

            if(suffix_type==AssetType::Unknown)
            {
                bool keep_suffix = false;

                suffix_type = ParseDiskTypeAlias(suffix,keep_suffix);   // 仅历史拼写别名

                if(suffix_type!=AssetType::Unknown && keep_suffix)
                {
                    // 容器拼写属于资产名：认类型但**不剥离**，就此收手。
                    if(type==AssetType::Unknown)
                        type=suffix_type;

                    break;
                }
            }

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

        // 统一对 logical_path 进行 ASCII 小写折叠，确保跨平台按 URI 寻址时大小写不敏感一致命中
        U8String folded_path = logical_path;
        for(int i = 0; i < folded_path.Length(); ++i)
        {
            const u8char c = folded_path[i];
            if(c >= u8char('A') && c <= u8char('Z'))
                folded_path[i] = u8char(c + ('a' - 'A'));
        }

        key += folded_path;
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

    static bool EqualFolded(const U8String &a, const U8String &b)
    {
        if(a.Length() != b.Length())
            return false;
        for(int i = 0; i < a.Length(); ++i)
        {
            u8char ca = a[i];
            u8char cb = b[i];
            if(ca >= u8char('A') && ca <= u8char('Z')) ca = u8char(ca + ('a' - 'A'));
            if(cb >= u8char('A') && cb <= u8char('Z')) cb = u8char(cb + ('a' - 'A'));
            if(ca != cb) return false;
        }
        return true;
    }

    static U8String StripAssetFormatSuffix(const U8String &path)
    {
        const int dot = path.FindRightChar(u8char('.'));
        if(dot < 0) return path;
        U8String suffix = path.SubString(dot + 1);
        if(IsAssetFormatSuffix(suffix))
            return path.SubString(0, dot);
        return path;
    }

    bool AssetIndexTable::TryAddStrict(const AssetIndexEntry &entry, AnsiString *out_conflict)
    {
        if(out_conflict)
            out_conflict->Clear();

        if(entry.logical_path.IsEmpty() || entry.physical_path.IsEmpty())
            return false;

        if(entry.id.IsZero())
            return false;

        const U8String key = MakeIndexPathKey(entry.type, entry.logical_path);
        if(key.IsEmpty())
            return false;

        WriteLockGuard guard(lock);

        AssetID existing_id;
        if(by_path.Get(key, existing_id))
        {
            AssetIndexEntry existing_entry;
            if(by_id.Get(existing_id, existing_entry))
            {
                // 如果物理文件相同，视为重新更新，直接放行
                if(existing_entry.physical_path == entry.physical_path)
                {
                    by_id.Update(entry.id, entry);
                    return true;
                }

                // 提取两者的物理格式后缀
                const int dot_exist = existing_entry.physical_path.FindRightChar(u8char('.'));
                const int dot_new   = entry.physical_path.FindRightChar(u8char('.'));

                U8String suffix_exist = (dot_exist >= 0) ? existing_entry.physical_path.SubString(dot_exist + 1) : U8String();
                U8String suffix_new   = (dot_new >= 0)   ? entry.physical_path.SubString(dot_new + 1) : U8String();

                const bool exist_is_src   = IsSourceFormatSuffix(suffix_exist);
                const bool exist_is_baked = IsBakedFormatSuffix(suffix_exist);
                const bool new_is_src     = IsSourceFormatSuffix(suffix_new);
                const bool new_is_baked   = IsBakedFormatSuffix(suffix_new);

                // 伴生前提：基本物理名在大小写折叠后必须一致，且恰好一个源一个产物
                const U8String base_exist = StripAssetFormatSuffix(existing_entry.physical_path);
                const U8String base_new   = StripAssetFormatSuffix(entry.physical_path);
                const bool same_base = EqualFolded(base_exist, base_new);

                const bool is_companion = same_base && ((exist_is_src && new_is_baked) || (exist_is_baked && new_is_src));

                if(!is_companion)
                {
                    if(out_conflict)
                    {
                        *out_conflict = AnsiString("conflict for logical path '")
                                      + AnsiString(reinterpret_cast<const char *>(entry.logical_path.c_str()))
                                      + "': '"
                                      + AnsiString(reinterpret_cast<const char *>(existing_entry.physical_path.c_str()))
                                      + "' vs '"
                                      + AnsiString(reinterpret_cast<const char *>(entry.physical_path.c_str()))
                                      + "'";
                    }
                    return false;
                }

                // 合法伴生处理
                if(exist_is_baked && new_is_src)
                {
                    // 烘焙产物已存在，新来的是源文件。若烘焙产物已有不同的伴生源文件，则视为冲突
                    if(existing_entry.HasSourceFile() && !EqualFolded(existing_entry.source_file, entry.physical_path))
                    {
                        if(out_conflict)
                        {
                            *out_conflict = AnsiString("conflict: baked asset already bound to source '")
                                          + AnsiString(reinterpret_cast<const char *>(existing_entry.source_file.c_str()))
                                          + "' cannot bind to '"
                                          + AnsiString(reinterpret_cast<const char *>(entry.physical_path.c_str()))
                                          + "'";
                        }
                        return false;
                    }

                    existing_entry.source_file = entry.physical_path;
                    if(entry.HasContentHash())
                        existing_entry.source_hash = entry.content_hash;
                    by_id.Update(existing_id, existing_entry);
                    return true;
                }
                else if(exist_is_src && new_is_baked)
                {
                    // 源文件已存在，新来的是烘焙产物：保留烘焙产物为主条目，并记录伴生源文件
                    AssetIndexEntry new_entry = entry;
                    new_entry.is_baked = true;
                    new_entry.source_file = existing_entry.physical_path;
                    if(existing_entry.HasContentHash())
                        new_entry.source_hash = existing_entry.content_hash;

                    if(existing_id != entry.id && !existing_id.IsZero())
                        by_id.DeleteByKey(existing_id);

                    if(!by_id.Update(new_entry.id, new_entry))
                        by_id.Add(new_entry.id, new_entry);

                    if(!by_path.Update(key, new_entry.id))
                        by_path.Add(key, new_entry.id);

                    return true;
                }
            }
        }

        // 无冲突，更新映射
        AssetIndexEntry old_entry;
        if(by_id.Get(entry.id, old_entry))
        {
            const U8String old_key = MakeIndexPathKey(old_entry.type, old_entry.logical_path);
            if(!(old_key == key))
                by_path.DeleteByKey(old_key);
        }

        if(existing_id != entry.id && !existing_id.IsZero())
            by_id.DeleteByKey(existing_id);

        if(!by_id.Update(entry.id, entry))
            by_id.Add(entry.id, entry);

        if(!by_path.Update(key, entry.id))
            by_path.Add(key, entry.id);

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

    // ── 内存探索查询（目录/前缀/类型）─────────────────────────────────────

    namespace
    {
        /// 单字符 ASCII 小写折叠（非 ASCII 字节原样返回，与 `MakeIndexPathKey` 同口径）
        inline u8char FoldAsciiLower(u8char c)
        {
            return (c >= u8char('A') && c <= u8char('Z'))
                 ? u8char(c + (u8char('a') - u8char('A')))
                 : c;
        }

        /**
         * 规范化逻辑目录：`\` 与 `/` 一律视为分隔符并统一输出 `/`，
         * 折叠连续分隔符，去除前后多余的 `/`。结果可能为空串（= 包根）。
         */
        U8String NormalizeLogicalDir(const U8String &in)
        {
            const int length = in.Length();

            U8String out;
            out.Reserve(length);

            bool pending_slash = false;   // 已见分隔符但尚未落盘（用于折叠连续分隔符）
            const u8char slash   = u8char('/');

            for(int i = 0; i < length; ++i)
            {
                const u8char c = in[i];

                if(c == u8char('/') || c == u8char('\\'))
                {
                    if(!out.IsEmpty())    // 前导分隔符直接丢弃
                        pending_slash = true;
                    continue;
                }

                if(pending_slash)
                {
                    out.Insert(out.Length(), &slash, 1);
                    pending_slash = false;
                }

                out.Insert(out.Length(), &c, 1);
            }

            return out;   // 尾部分隔符因 pending 未落盘而自然去除
        }

        /// path 的前 prefix_len 个字符与 prefix 折叠比对是否相等
        bool EqualsFoldedPrefix(const U8String &path, const U8String &prefix, int prefix_len)
        {
            if(path.Length() < prefix_len)
                return false;

            for(int i = 0; i < prefix_len; ++i)
            {
                if(FoldAsciiLower(path[i]) != FoldAsciiLower(prefix[i]))
                    return false;
            }

            return true;
        }

        /**
         * 判断 path 是否位于（已规范化的）norm_dir 之下。
         *
         * 非递归时只认直接层级；norm_dir 为空表示包根。
         */
        bool IsUnderLogicalDirectory(const U8String &path, const U8String &norm_dir, bool recursive)
        {
            if(norm_dir.IsEmpty())  // 根目录
            {
                if(recursive)
                    return true;

                // 非递归：不能再有 '/' 才是直接位于根之下
                return path.FindChar(u8'/') < 0;
            }

            const int dir_len = norm_dir.Length();

            // path 必须以 "norm_dir/" 开头
            if(path.Length() <= dir_len)
                return false;

            if(path[dir_len] != u8char('/'))
                return false;

            if(!EqualsFoldedPrefix(path, norm_dir, dir_len))
                return false;

            if(recursive)
                return true;

            // 非递归：剩余部分不能再包含 '/'
            for(int i = dir_len + 1; i < path.Length(); ++i)
            {
                if(path[i] == u8char('/'))
                    return false;
            }

            return true;
        }

        /// 前缀命中：等于前缀本身（同族伴生），或位于前缀目录之下（任意深度）
        bool MatchesLogicalPrefix(const U8String &path, const U8String &norm_prefix)
        {
            if(norm_prefix.IsEmpty())
                return true;

            if(EqualsFoldedPrefix(path, norm_prefix, norm_prefix.Length()) &&
               path.Length() == norm_prefix.Length())
                return true;

            return IsUnderLogicalDirectory(path, norm_prefix, true);
        }
    }//anonymous namespace

    int AssetIndexTable::GetEntriesByType(AssetType type, ManagedArray<AssetIndexEntry> &out) const
    {
        // 契约：必须指定类型。`Unknown` 不是资产类型，放行会让「类型没推导
        // 出来的条目」混进本应精确的过滤结果 —— 与 `MakeIndexPathKey` 同纪律。
        if(type == AssetType::Unknown)
            return 0;

        ReadLockGuard guard(lock);

        int found = 0;

        for(const auto &item : by_id)
        {
            if(item.second.type != type)
                continue;

            out.Create(item.second);
            ++found;
        }

        return found;
    }

    int AssetIndexTable::FindInDirectory(const U8String &logical_dir,
                                         AssetType type,
                                         ManagedArray<AssetIndexEntry> &out,
                                         bool recursive) const
    {
        const U8String norm_dir = NormalizeLogicalDir(logical_dir);

        ReadLockGuard guard(lock);

        int found = 0;

        for(const auto &item : by_id)
        {
            const AssetIndexEntry &entry = item.second;

            if(type != AssetType::Unknown && entry.type != type)
                continue;

            if(!IsUnderLogicalDirectory(entry.logical_path, norm_dir, recursive))
                continue;

            out.Create(entry);
            ++found;
        }

        return found;
    }

    int AssetIndexTable::FindByPathPrefix(const U8String &logical_prefix,
                                          AssetType type,
                                          ManagedArray<AssetIndexEntry> &out) const
    {
        const U8String norm_prefix = NormalizeLogicalDir(logical_prefix);

        // 空前缀 = 全表，语义上该走 GetAllEntries，这里拒绝以免误用
        if(norm_prefix.IsEmpty())
            return 0;

        ReadLockGuard guard(lock);

        int found = 0;

        for(const auto &item : by_id)
        {
            const AssetIndexEntry &entry = item.second;

            if(type != AssetType::Unknown && entry.type != type)
                continue;

            if(!MatchesLogicalPrefix(entry.logical_path, norm_prefix))
                continue;

            out.Create(entry);
            ++found;
        }

        return found;
    }
}//namespace hgl::asset
