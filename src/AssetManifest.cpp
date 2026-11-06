#include<hgl/asset/AssetManifest.h>
#include<hgl/serialize/TomlCodec.h>
#include<hgl/serialize/Value.h>
#include<hgl/filesystem/FileSystem.h>
#include<hgl/time/Time.h>
#include<hgl/utf.h>

namespace hgl::asset
{
    namespace
    {
        // ── 文本写出 ────────────────────────────────────────────────────────

        /// `hgl::String` 刻意删除了 `String(T)` 单字符构造，因此逐字符追加
        /// 必须显式包一层，不能写 `out += 'x'`。
        inline void AppendChar(AnsiString &out,const char ch)
        {
            out += AnsiString(&ch,1);
        }

        /// 只转义 TOML 基本字符串真正需要的两个字符。
        /// 资产路径用 '/'，反斜杠与引号在实际工程里极少出现，但一旦出现
        /// 不转义就会让整份清单解析失败，代价远高于这两行。
        void AppendEscaped(AnsiString &out,const char *str)
        {
            AppendChar(out,'"');

            if(str)
            {
                for(const char *p=str;*p;p++)
                {
                    if(*p=='\\'||*p=='"')
                        AppendChar(out,'\\');

                    AppendChar(out,*p);
                }
            }

            AppendChar(out,'"');
        }

        void AppendEscapedU8(AnsiString &out,const U8String &str)
        {
            constexpr int kBufSize = 4096;

            const int len = str.Length();

            if(len<=0)
            {
                AppendEscaped(out,nullptr);
                return;
            }

            const u8char *src = str.c_str();

            char buf[kBufSize];

            // 超长路径走分块拷贝：清单里出现 4KB 以上的路径本身就不正常，
            // 但截断比失败更糟 —— 截断会静默改写资产名。
            if(len>=kBufSize)
            {
                AppendChar(out,'"');

                for(int i=0;i<len;i++)
                {
                    const char ch = (src[i]<=0x7F)?char(src[i]):'?';

                    if(ch=='\\'||ch=='"')
                        AppendChar(out,'\\');

                    AppendChar(out,ch);
                }

                AppendChar(out,'"');
                return;
            }

            for(int i=0;i<len;i++)
                buf[i] = (src[i]<=0x7F)?char(src[i]):'?';

            buf[len]=0;

            AppendEscaped(out,buf);
        }

        void AppendKeyInt(AnsiString &out,const char *key,const uint64 value)
        {
            out += key;
            out += " = ";
            out += AnsiString::numberOf(static_cast<int64>(value));
            AppendChar(out,'\n');
        }

        void AppendKeyText(AnsiString &out,const char *key,const char *value)
        {
            out += key;
            out += " = ";
            AppendEscaped(out,value);
            AppendChar(out,'\n');
        }

        void AppendKeyU8(AnsiString &out,const char *key,const U8String &value)
        {
            out += key;
            out += " = ";
            AppendEscapedU8(out,value);
            AppendChar(out,'\n');
        }

        // ── 稳定排序 ────────────────────────────────────────────────────────

        /**
         * 条目排序：先类型段、后逻辑路径。
         *
         * HGL 里没有通用排序工具，而这里只是把指针数组按字符串排一遍，
         * 为此引入 `<algorithm>` 或 `std::vector` 都不值得。归并排序是最小
         * 可信度最高的选择：稳定、最坏 O(n log n)、实现短到不用解释。
         */
        int CompareEntry(const AssetIndexEntry *a,const AssetIndexEntry *b)
        {
            const char *ta = GetAssetTypeName(a->type);
            const char *tb = GetAssetTypeName(b->type);

            const int type_cmp = hgl::strcmp(ta,tb);

            if(type_cmp!=0)
                return type_cmp;

            return hgl::strcmp(a->logical_path.c_str(),b->logical_path.c_str());
        }

        void SortEntries(ValueArray<const AssetIndexEntry *> &items)
        {
            const int count = items.GetCount();

            if(count<2)
                return;

            ValueArray<const AssetIndexEntry *> src = items;
            ValueArray<const AssetIndexEntry *> dst;

            dst.Resize(count);

            // 自底向上的归并：每次把 src 里宽度为 w 的有序段两两归并进 dst，
            // 然后交换两个缓冲区。写回 items 在最后一次交换后完成。
            for(int width=1;width<count;width*=2)
            {
                for(int start=0;start<count;start+=width*2)
                {
                    const int mid  = (start+width<count)?(start+width):count;
                    const int end  = (start+width*2<count)?(start+width*2):count;

                    int l = start;
                    int r = mid;
                    int w = start;

                    while(l<mid&&r<end)
                    {
                        if(CompareEntry(src[l],src[r])<=0)
                            dst[w++]=src[l++];
                        else
                            dst[w++]=src[r++];
                    }

                    while(l<mid)
                        dst[w++]=src[l++];

                    while(r<end)
                        dst[w++]=src[r++];
                }

                src = dst;
            }

            items = src;
        }

        // ── 读取辅助 ────────────────────────────────────────────────────────

        bool FieldUInt(const serialize::Value *table,const char *name,uint64 &out)
        {
            if(!table)
                return false;

            const serialize::Value *field = table->FindFieldExact(name);

            if(!field)
                return false;

            return field->GetUInt(out);
        }

        bool FieldText(const serialize::Value *table,const char *name,AnsiString &out)
        {
            if(!table)
                return false;

            const serialize::Value *field = table->FindFieldExact(name);

            if(!field||!field->IsString())
                return false;

            const U8String &str = field->GetString();

            out = AnsiString(reinterpret_cast<const char *>(str.c_str()),str.Length());
            return true;
        }
    }//namespace

    AnsiString SanitizePackName(const AnsiString &pack_name)
    {
        if(pack_name.IsEmpty())
            return AnsiString();

        const char *p = pack_name.c_str();
        int len = pack_name.Length();

        while(len>0 && (p[len-1]=='/' || p[len-1]=='\\'))
            --len;

        if(len<=0)
            return AnsiString();

        int last_sep = -1;
        for(int i=0;i<len;i++)
        {
            if(p[i]=='/' || p[i]=='\\' || p[i]==':')
                last_sep = i;
        }

        if(last_sep>=0)
        {
            const char *sub = p + last_sep + 1;
            const int sub_len = len - last_sep - 1;
            if(sub_len>0)
                return AnsiString(sub,sub_len);
            return AnsiString();
        }

        return AnsiString(p,len);
    }

    OSString GetAssetManifestPath(const OSString &root_path)
    {
        return filesystem::JoinPathWithFilename(root_path,OS_TEXT(".asset_manifest.toml"));
    }

    // ── 读出 ────────────────────────────────────────────────────────────────

    ManifestLoadResult LoadAssetManifest(const OSString &filename,
                                         AssetIndexTable &out_table,
                                         AssetManifestInfo &out_info,
                                         AnsiString *out_error)
    {
        if(out_error)
            out_error->Clear();

        if(filename.IsEmpty())
        {
            if(out_error)
                *out_error="manifest path is empty";

            return ManifestLoadResult::IOError;
        }

        if(!filesystem::FileExist(filename))
        {
            // 与「解析失败」严格区分：这是正常的首次挂载。
            if(out_error)
                *out_error="manifest not found";

            return ManifestLoadResult::FileNotFound;
        }

        int64 size = 0;

        void *data = filesystem::LoadFileToMemory(filename,size,true);

        if(!data||size<=0)
        {
            if(out_error)
                *out_error="manifest read failed";

            if(data)
                delete[] static_cast<char *>(data);

            return ManifestLoadResult::IOError;
        }

        serialize::Value root;

        serialize::TomlParseError parse_error;

        const bool parsed = serialize::FromTomlText(static_cast<const char *>(data),
                                                    static_cast<size_t>(size),
                                                    root,
                                                    &parse_error);

        delete[] static_cast<char *>(data);

        if(!parsed)
        {
            if(out_error)
                *out_error=AnsiString("manifest parse failed: ")+parse_error.Format();

            return ManifestLoadResult::ParseError;
        }

        if(!root.IsTable())
        {
            if(out_error)
                *out_error="manifest root is not a table";

            return ManifestLoadResult::ParseError;
        }

        uint64 version = 0;

        AssetManifestInfo info;

        if(FieldUInt(&root,"manifest_version",version))
            info.version = static_cast<int>(version);

        if(info.version>kAssetManifestVersion)
        {
            // 不猜未来格式：猜错会把整包身份重排，那是不可逆的事故。
            if(out_error)
                *out_error=AnsiString("manifest version too new: ")
                          +AnsiString::numberOf(static_cast<int64>(info.version));

            return ManifestLoadResult::VersionTooNew;
        }

        AnsiString raw_pack;
        if(FieldText(&root,"pack_name",raw_pack))
            info.pack_name=SanitizePackName(raw_pack);

        if(!FieldText(&root,"hash_algorithm",info.hash_algorithm)
         ||info.hash_algorithm.IsEmpty())
            info.hash_algorithm=kDefaultContentHashAlgorithm;

        FieldUInt(&root,"updated_at",info.updated_at);

        out_info=info;

        out_table.SetHashAlgorithm(info.hash_algorithm);

        const serialize::Value *assets = root.FindFieldExact("assets");

        if(!assets)
        {
            // 空包是合法的（刚创建的目录）。
            return ManifestLoadResult::Success;
        }

        if(!assets->IsArray())
        {
            if(out_error)
                *out_error="manifest 'assets' is not an array";

            return ManifestLoadResult::ParseError;
        }

        const int count = assets->GetElementCount();

        int bad_id   = 0;
        int bad_type = 0;
        int bad_path = 0;

        for(int i=0;i<count;i++)
        {
            const serialize::Value *item = assets->GetElement(i);

            if(!item||!item->IsTable())
            {
                ++bad_path;
                continue;
            }

            AssetIndexEntry entry;

            AnsiString id_ansi;

            if(!FieldText(item,"id",id_ansi)||!AssetID::Parse(id_ansi,entry.id))
            {
                ++bad_id;
                continue;
            }

            AnsiString type_ansi;

            if(!FieldText(item,"type",type_ansi))
            {
                ++bad_type;
                continue;
            }

            entry.type = ParseAssetType(type_ansi);

            if(entry.type==AssetType::Unknown)
            {
                ++bad_type;
                continue;
            }

            // path / file 用 U8String：它们是资产名，可以包含非 ASCII。
            const serialize::Value *path_field = item->FindFieldExact("path");
            const serialize::Value *file_field = item->FindFieldExact("file");

            if(!path_field||!path_field->IsString()
             ||!file_field||!file_field->IsString())
            {
                ++bad_path;
                continue;
            }

            entry.logical_path  = path_field->GetString();
            entry.physical_path = file_field->GetString();

            if(entry.logical_path.IsEmpty()||entry.physical_path.IsEmpty())
            {
                ++bad_path;
                continue;
            }

            uint64 number = 0;

            entry.size  = FieldUInt(item,"size", number)?number:0;
            entry.mtime = FieldUInt(item,"mtime",number)?number:0;

            AnsiString hash_text;

            if(FieldText(item,"hash",hash_text))
                entry.content_hash=hash_text;
            else
                entry.content_hash.Clear();

            const serialize::Value *source_field = item->FindFieldExact("source_file");
            if(source_field&&source_field->IsString())
                entry.source_file = source_field->GetString();

            AnsiString src_hash_text;
            if(FieldText(item,"source_hash",src_hash_text))
                entry.source_hash=src_hash_text;

            uint64 b_time = 0;
            if(FieldUInt(item,"bake_time",b_time))
                entry.bake_time = b_time;

            if(!out_table.AddOrUpdate(entry))
            {
                ++bad_path;
                continue;
            }
        }

        if(bad_id!=0||bad_type!=0||bad_path!=0)
        {
            if(out_error)
                *out_error=AnsiString("manifest has invalid entries: id=")
                          +AnsiString::numberOf(bad_id)
                          +AnsiString(" type=")+AnsiString::numberOf(bad_type)
                          +AnsiString(" path=")+AnsiString::numberOf(bad_path);
        }

        return ManifestLoadResult::Success;
    }

    // ── 写出 ────────────────────────────────────────────────────────────────

    AnsiString WriteAssetManifestText(const AssetIndexTable &table,const AssetManifestInfo &info)
    {
        ManagedArray<AssetIndexEntry> entries;

        table.GetAllEntries(entries);

        ValueArray<const AssetIndexEntry *> sorted;
        sorted.Reserve(entries.GetCount());

        for(int i=0;i<entries.GetCount();i++)
            sorted.Add(entries[i]);

        SortEntries(sorted);

        AnsiString out;

        out += "# ULRE 资产清单（.asset_manifest.toml）\n";
        out += "# 由 AssetSourceFilesystem（挂载扫描）与 AssetManifestTool（离线校验/内容哈希）维护。\n";
        out += "# 手改只建议改注释：id 一旦变动，所有跨资产引用都会断链。\n";
        AppendChar(out,'\n');

        AppendKeyInt(out,"manifest_version",static_cast<uint64>(info.version));

        if(!info.pack_name.IsEmpty())
            AppendKeyText(out,"pack_name",info.pack_name.c_str());

        AnsiString effective_algo = info.hash_algorithm;
        if(effective_algo.IsEmpty())
            effective_algo = table.GetHashAlgorithm();
        if(effective_algo.IsEmpty())
            effective_algo = kDefaultContentHashAlgorithm;

        AppendKeyText(out,"hash_algorithm",effective_algo.c_str());

        AppendKeyInt(out,"updated_at",info.updated_at);

        const int count = sorted.GetCount();

        AppendChar(out,'\n');

        for(int i=0;i<count;i++)
        {
            const AssetIndexEntry *entry = sorted[i];

            out += "[[assets]]\n";

            const AnsiString id_text = entry->id.ToString();

            AppendKeyText(out,"id",id_text.c_str());
            AppendKeyText(out,"type",GetAssetTypeName(entry->type));
            AppendKeyU8  (out,"path",entry->logical_path);
            AppendKeyU8  (out,"file",entry->physical_path);
            AppendKeyInt (out,"size",entry->size);
            AppendKeyInt (out,"mtime",entry->mtime);

            if(entry->HasContentHash())
                AppendKeyText(out,"hash",entry->content_hash.c_str());

            if(entry->HasSourceFile())
                AppendKeyU8(out,"source_file",entry->source_file);

            if(entry->HasSourceHash())
                AppendKeyText(out,"source_hash",entry->source_hash.c_str());

            if(entry->bake_time>0)
                AppendKeyInt(out,"bake_time",entry->bake_time);

            AppendChar(out,'\n');
        }

        // 去掉末尾多余的空行，保证「最后一行有且只有一个换行」——
        // 版本库里的文本文件应当时刻满足这一点，否则每次写出都会多一个空行。
        while(out.Length()>0&&out.c_str()[out.Length()-1]=='\n')
            out = out.SubString(0,out.Length()-1);

        AppendChar(out,'\n');

        return out;
    }

    bool SaveAssetManifest(const OSString &filename,
                           const AssetIndexTable &table,
                           AssetManifestInfo &info,
                           AnsiString *out_error)
    {
        if(out_error)
            out_error->Clear();

        if(filename.IsEmpty())
        {
            if(out_error)
                *out_error="manifest path is empty";

            return false;
        }

        if(info.updated_at==0)
            info.updated_at=hgl::GetUnixTimestampSec();

        if(info.version<=0)
            info.version=kAssetManifestVersion;

        info.pack_name = SanitizePackName(info.pack_name);

        if(info.hash_algorithm.IsEmpty())
            info.hash_algorithm=table.GetHashAlgorithm();

        if(info.hash_algorithm.IsEmpty())
            info.hash_algorithm=kDefaultContentHashAlgorithm;

        const AnsiString text = WriteAssetManifestText(table,info);

        // 先写临时文件再改名：中途失败（磁盘满、进程被杀）不会留下半截清单。
        // 半截清单比没有清单更危险 —— 它会让下一次挂载以为「资产被删了一批」。
        const OSString tmp_name = filename+OS_TEXT(".tmp");

        if(filesystem::SaveMemoryToFile(tmp_name,text.c_str(),text.Length())<=0)
        {
            if(out_error)
                *out_error="manifest temp write failed";

            return false;
        }

        filesystem::FileDelete(filename);

        if(!filesystem::FileRename(filename,tmp_name))
        {
            if(out_error)
                *out_error="manifest rename failed";

            filesystem::FileDelete(tmp_name);
            return false;
        }

        return true;
    }
}//namespace hgl::asset
