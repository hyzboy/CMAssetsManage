#include<hgl/asset/AssetSource.h>
#include<hgl/asset/AssetManifest.h>
#include<hgl/filesystem/EnumFile.h>
#include<hgl/filesystem/FileSystem.h>
#include<hgl/io/FileInputStream.h>
#include<hgl/platform/Platform.h>
#include<hgl/Charset.h>

namespace hgl::asset
{
    namespace
    {
        /**
         * 目录枚举器。
         *
         * 不使用 `filesystem::GetFileInfoList`：它的实现只转发了 `ProcFile`，
         * 目录项会被静默丢掉，而包裹的 `ListDirectory` 必须同时给出子目录。
         *
         * 平台差异：Windows 的枚举器对每个目录项都会调用 `ProcFolderEnd`
         * （即使不递归），UNIX 版本走的是 `ProcFolder` 回调（当前 `EnumFile`
         * 并未声明该虚函数，属于 CMCore 侧待修的问题）。因此这里按平台选择
         * 可用的那个回调。
         */
        class SourceEnumerator:public filesystem::EnumFile
        {
            ManagedArray<SourceEntry> *out_list;
            const U8String            &base_path;

        private:

            void Append(const filesystem::FileInfo &fi,bool is_directory)
            {
                SourceEntry entry;

                entry.name        =ToU8String(OSString(fi.name));
                entry.path        =base_path.IsEmpty()?entry.name:(base_path+U8String(u8"/")+entry.name);
                entry.size        =is_directory?0:fi.size;
                entry.mtime       =fi.mtime;
                entry.is_directory=is_directory;

                out_list->Create(entry);
            }

        protected:

#if HGL_OS == HGL_OS_Windows
            void ProcFolderEnd(struct filesystem::EnumFileConfig *,
                               struct filesystem::EnumFileConfig *,
                               filesystem::FileInfo &fi) override
            {
                Append(fi,true);
            }
#else
            void ProcFolderBegin(struct filesystem::EnumFileConfig *,
                                 struct filesystem::EnumFileConfig *,
                                 filesystem::FileInfo &fi) override
            {
                Append(fi,true);
            }
#endif//HGL_OS == HGL_OS_Windows

            void ProcFile(struct filesystem::EnumFileConfig *,filesystem::FileInfo &fi) override
            {
                Append(fi,false);
            }

        public:

            SourceEnumerator(ManagedArray<SourceEntry> *list,const U8String &base)
                :out_list(list),base_path(base) {}
        };//class SourceEnumerator

        // ── 挂载扫描 ────────────────────────────────────────────────────────

        /**
         * 带相对路径的枚举配置。
         *
         * `EnumFile` 只把 `FileInfo` 交给回调，没有「当前在哪个子目录」的信息；
         * 从 `fi.fullname` 反剥挂载根前缀虽然可行，但一旦根路径里出现符号链接
         * 或大小写差异就会剥错。既然 `CreateSubConfig` 是虚函数，就让配置
         * 自己记住相对路径 —— 每层递归各带一份，永远不会算错。
         */
        class ScanConfig:public filesystem::EnumFileConfig
        {
        public:

            U8String rel_path;      ///<相对挂载根的目录路径（空串 = 根）

            ScanConfig(const OSString &full,const U8String &rel)
                :EnumFileConfig(full),rel_path(rel) {}

            ScanConfig(const EnumFileConfig *up,const OSString &sub_full,const U8String &sub_rel)
                :EnumFileConfig(up,sub_full),rel_path(sub_rel) {}
        };//class ScanConfig

        /// 扫描到的一个候选资产（尚未与旧索引对拍）。
        struct ScannedAsset
        {
            AssetType type;
            U8String  logical_path;
            U8String  physical_path;
            uint64    size;
            uint64    mtime;
        };//struct ScannedAsset

        /// 扫描结果：候选资产 + 触发判定的计数。
        struct ScanResult
        {
            ManagedArray<ScannedAsset> assets;

            int new_files   = 0;    ///<清单里没有的物理文件
            int meta_change = 0;    ///<size 或 mtime 变了
            int hash_dropped= 0;    ///<内容变了，旧哈希作废
        };//struct ScanResult

        /**
         * 递归扫描器。
         *
         * 只收文件：目录仅用于递归。以 `.` 开头的目录与文件一律跳过 ——
         * `.asset_manifest.toml`、`.git`、编辑器临时文件都属于「不是资产」，
         * 把它们收进索引会让每次保存都触发一次清单重写。
         */
        class SourceScanner:public filesystem::EnumFile
        {
            ScanResult *result;

        private:

            static bool AcceptName(const U8String &name)
            {
                if(name.IsEmpty())
                    return false;

                return name.c_str()[0]!=u8char('.');
            }

        protected:

            filesystem::EnumFileConfig *CreateSubConfig(struct filesystem::EnumFileConfig *up_efc,
                                                        const filesystem::FileInfo &fi) override
            {
                const U8String child_name=ToU8String(OSString(fi.name));

                // 返回 nullptr 即「不要递归进这个目录」（`EnumFile` 会跳过 sub_efc）。
                if(!AcceptName(child_name))
                    return nullptr;

                const ScanConfig *up_cfg=static_cast<const ScanConfig *>(up_efc);

                const U8String child_rel=up_cfg->rel_path.IsEmpty()
                                        ? child_name
                                        : (up_cfg->rel_path+U8String(u8"/")+child_name);

                const OSString child_full=filesystem::JoinPathWithFilename(up_efc->folder_name,fi.name);

                return new ScanConfig(up_efc,child_full,child_rel);
            }

            void ProcFile(struct filesystem::EnumFileConfig *cfg,filesystem::FileInfo &fi) override
            {
                if(!result||!cfg)
                    return;

                const ScanConfig *scan_cfg=static_cast<const ScanConfig *>(cfg);

                const U8String name=ToU8String(OSString(fi.name));

                if(!AcceptName(name))
                    return;

                const U8String physical=scan_cfg->rel_path.IsEmpty()
                                       ? name
                                       : (scan_cfg->rel_path+U8String(u8"/")+name);

                ScannedAsset asset;

                if(!DeriveLogical(physical,AssetType::Unknown,asset.type,asset.logical_path))
                    return;     // 不是资产（原始素材、说明文件……）：不进索引

                asset.physical_path=physical;
                asset.size         =fi.size;
                asset.mtime        =fi.mtime;

                result->assets.Create(asset);
            }

        public:

            SourceScanner(ScanResult *res):result(res) {}
        };//class SourceScanner
    }//namespace

    /**
     * 目录包裹：把挂载名映射到一个真实目录。
     *
     * 这是「位置透明」的当前实现；将来换成真实包裹文件（zip / pak）时，
     * 只需另写一个实现同一组虚函数的类，`pack(<name>)` 之下的资产路径不用改。
     * 因此这里的 Exists/Stat/ListDirectory 不是可选装饰 —— 它们是后端契约。
     */
    class AssetsSourceFilesytem:public AssetSource
    {
        OSString root_path;
        bool     only_read;

        AssetsSourceMountOptions mount_options;

        AssetIndexTable index_table;

    private:
        /// 规范化后的包裹内路径 → 真实路径。`norm` 为空表示挂载根本身。
        OSString MakeFullPath(const U8String &norm)const
        {
            if(norm.IsEmpty())
                return root_path;

            return filesystem::JoinPathWithFilename(root_path,ToOSString(norm));
        }

    public:

        /**
         * 扫描目录并与旧索引对拍，把结果整体换入 `index_table`。
         *
         * 对拍规则（三条，缺一都会让身份或指纹失去意义）：
         *   · 物理文件已在索引里 ⇒ **复用它的 ID**。这是跨资产引用不断链的
         *     唯一保证：改名、改内容都不该换身份；
         *   · `size` 与 `mtime` 都没变 ⇒ 连同 `content_hash` 一起复用。
         *     任一变化都说明内容可能变了，旧哈希**必须作废**（置空），
         *     否则清单会长期携带一个「看起来有、实际是上一版」的指纹 ——
         *     比没有更危险；
         *   · 索引里存在、但本次没扫到的条目 ⇒ 自然消失（新表是重建的）。
         *
         * 只有 `build_index` 开启时才做；关闭时索引为空，后端退回纯路径访问。
         */
        bool RebuildIndex(AnsiString *out_error) override
        {
            if(out_error)
                out_error->Clear();

            if(!mount_options.build_index)
            {
                index_table.Clear();
                return true;
            }

            // ── 1. 读旧清单 ────────────────────────────────────────────────
            AssetIndexTable old_table;
            AssetManifestInfo manifest;

            bool manifest_exists = false;

            if(mount_options.load_manifest)
            {
                AnsiString load_error;

                manifest_exists=LoadAssetManifest(GetAssetManifestPath(root_path),
                                                  old_table,manifest,&load_error);

                // 清单损坏必须报出来，但不阻断挂载：资产照样能按路径访问，
                // 只是这一轮的身份会按「新文件」处理。静默吞掉才是不可接受的。
                if(!manifest_exists&&out_error&&!load_error.IsEmpty())
                    *out_error=load_error;
            }

            if(!manifest_exists)
                manifest.pack_name=GetName();

            // ── 2. 扫描 ────────────────────────────────────────────────────
            ScanResult result;

            {
                // 根配置必须就是 ScanConfig：`CreateSubConfig` 会把它 static_cast
                // 回 ScanConfig 再取 rel_path，传一个裸 EnumFileConfig 会当场越界。
                ScanConfig root_cfg(root_path,U8String());

                root_cfg.proc_folder=true;  // EnumFile 只有走目录分支才会递归
                root_cfg.proc_file  =true;
                root_cfg.sub_folder =true;

                SourceScanner scanner(&result);

                scanner.Enum(&root_cfg);
            }

            // ── 3. 对拍 ────────────────────────────────────────────────────
            AssetIndexTable new_table;

            new_table.SetHashAlgorithm(manifest.hash_algorithm.IsEmpty()
                                      ?AnsiString(kDefaultContentHashAlgorithm)
                                      :manifest.hash_algorithm);

            int reused = 0;

            for(int i=0;i<result.assets.GetCount();i++)
            {
                const ScannedAsset &scanned=*result.assets[i];

                AssetIndexEntry entry;

                entry.type          =scanned.type;
                entry.logical_path  =scanned.logical_path;
                entry.physical_path =scanned.physical_path;
                entry.size          =scanned.size;
                entry.mtime         =scanned.mtime;

                AssetIndexEntry old_entry;

                const bool found=old_table.FindByPath(scanned.type,scanned.logical_path,old_entry);

                if(found)
                {
                    entry.id=old_entry.id;
                    ++reused;

                    if(old_entry.size==scanned.size&&old_entry.mtime==scanned.mtime)
                    {
                        entry.content_hash=old_entry.content_hash;
                    }
                    else
                    {
                        if(old_entry.HasContentHash())
                            ++result.hash_dropped;
                    }
                }
                else
                {
                    entry.id=AssetID::Create();
                    ++result.new_files;
                }

                new_table.AddOrUpdate(entry);
            }

            // ── 4. 判定是否需要回写 ────────────────────────────────────────
            const int old_count=old_table.GetCount();
            const int new_count=new_table.GetCount();

            const bool changed = !manifest_exists
                              || old_count!=new_count
                              || reused!=old_count
                              || result.new_files!=0
                              || result.hash_dropped!=0
                              || !(old_table.GetHashAlgorithm()==new_table.GetHashAlgorithm());

            // 整体换入：一次写锁内完成，避免并发读者看到半张索引。
            index_table.CopyFrom(new_table);

            if(changed&&mount_options.save_manifest)
            {
                AnsiString save_error;

                if(!SaveAssetManifest(GetAssetManifestPath(root_path),index_table,manifest,&save_error))
                {
                    if(out_error)
                        *out_error=AnsiString("manifest save failed: ")+save_error;
                }
            }

            return true;
        }

        AssetsSourceFilesytem(const AnsiString &sn,const OSString &path,const bool _or,
                              const AssetsSourceMountOptions &options)
            :AssetSource(sn)
        {
            root_path    =path;
            only_read    =_or;
            mount_options=options;
        }

        bool IsReadOnly()const{return only_read;}

        bool hasNameAccess()const override{return true;}
        bool hasIDAccess()const override{return mount_options.build_index;}

        const AssetIndexTable *GetIndexTable()const override
        {
            return mount_options.build_index?&index_table:nullptr;
        }

        bool Exists(const U8String &path)const override
        {
            SourceEntry entry;

            return Stat(path,entry);
        }

        bool Stat(const U8String &path,SourceEntry &out)const override
        {
            U8String norm;

            // 空串表示挂载根；其余必须能通过规范化（拒绝越界）。
            if(!path.IsEmpty()&&!NormalizePackPath(path,norm))
                return false;

            const OSString fullname=MakeFullPath(norm);

            filesystem::FileInfo fi;

            if(!filesystem::GetFileInfo(fullname.c_str(),fi))
                return false;

            out.name        =ToU8String(OSString(fi.name));
            out.path        =norm;
            out.size        =fi.is_directory?0:fi.size;
            out.mtime       =fi.mtime;
            out.is_directory=fi.is_directory;

            return true;
        }

        io::InputStream *Open(const U8String &path) override
        {
            U8String norm;

            if(!NormalizePackPath(path,norm))
                return nullptr;

            const OSString fullname=MakeFullPath(norm);

            if(!filesystem::FileCanRead(fullname))
                return nullptr;

            io::FileInputStream *fis=new io::FileInputStream;

            if(fis->Open(fullname))
                return fis;

            delete fis;
            return nullptr;
        }

        int ListDirectory(const U8String &path,ManagedArray<SourceEntry> &out)const override
        {
            U8String norm;

            // 空串表示挂载根；其余必须能通过规范化（拒绝越界）。
            if(!path.IsEmpty()&&!NormalizePackPath(path,norm))
                return -1;

            const OSString folder=MakeFullPath(norm);

            if(!filesystem::IsDirectory(folder.c_str()))
                return -1;

            const int before=out.GetCount();

            filesystem::EnumFileConfig config(folder);

            config.proc_folder=true;
            config.proc_file  =true;
            config.sub_folder =false;

            SourceEnumerator enumerator(&out,norm);

            enumerator.Enum(&config);

            return out.GetCount()-before;
        }
    };//class AssetsSourceFilesytem:public AssetSource

    AssetSource *CreateSourceByFilesystem(const AnsiString &uri,const OSString &path,const bool only_read)
    {
        return CreateSourceByFilesystem(uri,path,only_read,AssetsSourceMountOptions());
    }

    AssetSource *CreateSourceByFilesystem(const AnsiString &uri,const OSString &path,const bool only_read,
                                          const AssetsSourceMountOptions &options)
    {
        if(uri.IsEmpty())
            return nullptr;

        if(path.IsEmpty())
            return nullptr;

        if(!filesystem::IsDirectory(path.c_str()))
            return nullptr;

        if(GetSource(uri))
            return nullptr;

        AssetsSourceFilesytem *src=new AssetsSourceFilesytem(uri,path,only_read,options);

        // 先建索引再登记：登记之后并发的 `GetSource()` 立刻就能查到，
        // 若索引还没建完，那一瞬间的「按逻辑路径取资产」会全部落空。
        AnsiString index_error;

        if(!src->RebuildIndex(&index_error))
        {
            delete src;
            return nullptr;
        }

        if(!RegisterAssetsSource(uri,src))
        {
            delete src;
            return nullptr;
        }

        return src;
    }
}//namespace hgl::asset
