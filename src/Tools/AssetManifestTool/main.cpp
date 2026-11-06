// AssetManifestTool —— 目录包裹清单的离线校验与内容哈希工具。
//
// ── 它为什么必须是一个独立程序 ────────────────────────────────────────────
//
// 运行期挂载只做「浅校验」：扫一遍目录、比对 size 与 mtime、该复用的身份复用、
// 该作废的哈希作废。它**绝不**读文件内容 —— 一个十万级资产的工程如果每次启动
// 都把每个资产读一遍算 BLAKE3，冷启动会从毫秒级掉到分钟级。
//
// 但内容指纹总得有人算，而且必须在「能容忍慢」的时机算：
//   · 进版本库之前，确认「这份清单描述的就是当前这批文件」；
//   · 出包（Cook）之前，把内容哈希写进包头，让运行期能走「不读内容也能
//     判断变没变」的快路径；
//   · CI 里作为一致性闸门（`--check-only` 差异即非零退出码）。
//
// 这三件事的共同点是「不该由引擎运行时承担」，因此独立成工具。
//
// ── 它做的事 ──────────────────────────────────────────────────────────────
//
//   <dir>/.asset_manifest.toml   读入（不存在则视为空清单）
//   <dir>/**                     递归找资产
//
//   ── 复用运行期同一套扫描与对拍（挂载一个 save_manifest=false 的临时包裹）
//   ── 只为「当前这批文件」保留身份：新增的分配新 UUID，消失的条目自然淘汰
//   ── 按**包级**算法（默认 BLAKE3）逐文件计算内容哈希
//   ── 输出差异报告；非 --check-only 时原子写回清单
//
// 刻意复用 `CreateSourceByFilesystem` 而不是自己再写一遍扫描：
// 工具与运行期一旦对「什么算资产」有分歧，清单就会在两者之间反复抖动，
// 而那种抖动极难定位（每次都「看起来只是重排了一下」）。
#include <hgl/asset/AssetIndex.h>
#include <hgl/asset/AssetManifest.h>
#include <hgl/asset/AssetSource.h>

#include <hgl/filesystem/FileSystem.h>
#include <hgl/log/Log.h>
#include <hgl/log/Logger.h>
#include <hgl/time/Time.h>
#include <hgl/utf.h>
#include <hgl/util/hash/Hash.h>

#include <cstring>

namespace
{
    using namespace hgl;
    using namespace hgl::asset;

    /// 临时挂载名。带双下划线前缀，避免与工程里真实的包裹名冲突。
    constexpr const char *kTempMountName = "__manifest_tool__";

    /// 报告里逐项列出的上限。一份几千行的清单刷满终端反而看不见真正重要的那几行。
    constexpr int kListLimit = 40;

    /// `hash::Hash` 的 size 参数是 32 位，超过这个大小的文件无法一次读入。
    constexpr uint64 kMaxHashFileSize = 0xFFFFFFFFull;

    struct Options
    {
        OSString   root;
        bool       check_only    = false;
        bool       compute_hash  = true;
        bool       algo_override = false;
        AnsiString algo_name;
    };

    AnsiString Narrow(const OSString &str)
    {
        const U8String u8 = ToU8String(str);

        return AnsiString(reinterpret_cast<const char *>(u8.c_str()), u8.Length());
    }

    AnsiString Narrow(const U8String &str)
    {
        return AnsiString(reinterpret_cast<const char *>(str.c_str()), str.Length());
    }

    // ── 算法名 ──────────────────────────────────────────────────────────────

    /**
     * 名字 → 算法。
     *
     * 刻意不用 `hash::FromName`：它对不认识的输入**默认返回 MD5**。对清单来说
     * 这是最坏的失败模式 —— `hash_algorithm = "BLAKE4"`（拼错）会被静默当成
     * MD5，于是整包按错误算法重算一遍，而没有任何一处报错。
     */
    bool ParseAlgorithm(const AnsiString &name, util::hash::Algorithm &out)
    {
        static const struct
        {
            const char             *name;
            util::hash::Algorithm   algo;
        } table[] =
        {
            { "BLAKE3",    util::hash::Algorithm::BLAKE3    },
            { "MD5",       util::hash::Algorithm::MD5       },
            { "SHA1",      util::hash::Algorithm::SHA1      },
            { "SHA256",    util::hash::Algorithm::SHA256    },
            { "SHA512",    util::hash::Algorithm::SHA512    },
            { "XXH3-128",  util::hash::Algorithm::XXH3_128  },
            { "XXH3-64",   util::hash::Algorithm::XXH3_64   },
            { "XXH64",     util::hash::Algorithm::XXH64     },
            { "CRC32",     util::hash::Algorithm::CRC32     },
        };

        for (const auto &item : table)
            if (name == item.name)
            {
                out = item.algo;
                return true;
            }

        return false;
    }

    /// 算法 → 清单里写的规范名（一定是 `ParseAlgorithm` 认得的拼写）。
    AnsiString CanonicalAlgorithmName(util::hash::Algorithm algo)
    {
        const char *name = util::hash::GetName(algo);

        return (name && *name) ? AnsiString(name) : AnsiString(kDefaultContentHashAlgorithm);
    }

    // ── 内容哈希 ────────────────────────────────────────────────────────────

    /**
     * 计算单个文件的内容哈希，输出小写十六进制。
     *
     * `hgl::util::hash::HashFile()` 在 CMUtil 里**只有声明、实现直接 return false**，
     * 因此这里整文件读入后走 `hash::Hash()`。资产文件都是元数据（TOML /
     * FlatBuffers），几 KB 到几百 KB，整读没有实际代价；超过 4GB（`Hash` 的
     * size 是 32 位）直接判失败并报出来，而不是悄悄跳过。
     */
    bool ComputeFileHash(const OSString &filename, util::hash::Algorithm algo, AnsiString &out_hex)
    {
        out_hex.Clear();

        const size_t digest_size = util::hash::GetDigestSize(algo);

        if (digest_size == 0 || digest_size > 64)
            return false;

        int64 size = 0;

        void *data = filesystem::LoadFileToMemory(filename, size, false);

        if (!data)
            return false;

        if (size <= 0 || static_cast<uint64>(size) > kMaxHashFileSize)
        {
            delete[] static_cast<char *>(data);
            return false;
        }

        uint8 digest[64] = {0};

        const bool ok = util::hash::Hash(algo, data, static_cast<size_t>(size), digest);

        delete[] static_cast<char *>(data);

        if (!ok)
            return false;

        static constexpr char kHex[] = "0123456789abcdef";

        char text[129];

        for (size_t i = 0; i < digest_size; ++i)
        {
            text[i * 2]     = kHex[(digest[i] >> 4) & 0x0F];
            text[i * 2 + 1] = kHex[digest[i] & 0x0F];
        }

        text[digest_size * 2] = 0;

        out_hex = AnsiString(text, static_cast<int>(digest_size * 2));
        return true;
    }

    // ── 差异统计 ────────────────────────────────────────────────────────────

    struct DiffReport
    {
        int added        = 0;   ///<清单里没有、磁盘上有的文件
        int removed      = 0;   ///<清单里有、磁盘上没有的文件
        int hash_changed = 0;   ///<内容变了
        int meta_changed = 0;   ///<size 或 mtime 变了
        int hash_failed  = 0;   ///<读不到 / 算不出

        int listed = 0;

        bool HasDifference() const
        {
            return added != 0 || removed != 0 || hash_changed != 0 || meta_changed != 0;
        }

        void Line(const char *kind, const AssetIndexEntry &entry)
        {
            ++listed;

            if (listed > kListLimit)
                return;

            GLogInfo(u8"  [%s] %s",
                     kind,
                     Narrow(MakeIndexPathKey(entry.type, entry.logical_path)).c_str());
        }

        void FlushOverflow() const
        {
            if (listed > kListLimit)
                GLogInfo(u8"  …… 另有 %d 项未列出", listed - kListLimit);
        }
    };//struct DiffReport

    // ── 参数 ────────────────────────────────────────────────────────────────

    void PrintUsage()
    {
        GLogInfo(u8"用法：AssetManifestTool <目录> [选项]");
        GLogInfo(u8"");
        GLogInfo(u8"  --check-only     只校验不写回；发现差异时以退出码 1 结束（CI 用）");
        GLogInfo(u8"  --update         校验并写回清单（默认）");
        GLogInfo(u8"  --no-hash        跳过内容哈希，只做结构校验（快，但不产生指纹）");
        GLogInfo(u8"  --algo=<NAME>    强制换用该算法全量重算（BLAKE3/MD5/SHA256/…）");
        GLogInfo(u8"  -h, --help       显示本帮助");
        GLogInfo(u8"");
        GLogInfo(u8"内容哈希算法是**包级**属性，写在清单头部的 hash_algorithm；");
        GLogInfo(u8"未指定时沿用清单里的值，清单不存在时用 BLAKE3。");
    }

    bool ParseArgs(int argc, char **argv, Options &opt)
    {
        for (int i = 1; i < argc; ++i)
        {
            const char *arg = argv[i];

            if (!arg || !*arg)
                continue;

            if (arg[0] != '-')
            {
                if (!opt.root.IsEmpty())
                {
                    GLogError(u8"只能指定一个目录");
                    return false;
                }

                opt.root = ToOSString(arg);
                continue;
            }

            if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0)
                return false;

            if (std::strcmp(arg, "--check-only") == 0)
            {
                opt.check_only = true;
                continue;
            }

            if (std::strcmp(arg, "--update") == 0)
            {
                opt.check_only = false;
                continue;
            }

            if (std::strcmp(arg, "--no-hash") == 0)
            {
                opt.compute_hash = false;
                continue;
            }

            if (std::strncmp(arg, "--algo=", 7) == 0)
            {
                opt.algo_override = true;
                opt.algo_name     = AnsiString(arg + 7);
                continue;
            }

            GLogError(u8"未知选项：%s", arg);
            return false;
        }

        if (opt.root.IsEmpty())
        {
            GLogError(u8"必须指定一个目录");
            return false;
        }

        return true;
    }

    // ── 主流程 ──────────────────────────────────────────────────────────────

    int RunTool(const Options &opt)
    {
        if (!filesystem::IsDirectory(opt.root.c_str()))
        {
            GLogError(u8"目录不存在：%s", Narrow(opt.root).c_str());
            return 2;
        }

        const OSString manifest_path = GetAssetManifestPath(opt.root);

        // ── 1. 读旧清单：它既是对拍的基准，也是差异报告的对照物 ──────────
        AssetIndexTable   old_table;
        AssetManifestInfo manifest;

        AnsiString load_error;

        const bool had_manifest = LoadAssetManifest(manifest_path, old_table, manifest, &load_error);

        if (!had_manifest && !(load_error == "manifest not found"))
        {
            GLogError(u8"清单无法读取：%s", load_error.c_str());
            return 2;
        }

        // ── 2. 决定包级算法 ─────────────────────────────────────────────
        util::hash::Algorithm algo = util::hash::Algorithm::BLAKE3;

        AnsiString algo_name = opt.algo_override
                             ? opt.algo_name
                             : (had_manifest ? manifest.hash_algorithm
                                             : AnsiString(kDefaultContentHashAlgorithm));

        if (algo_name.IsEmpty())
            algo_name = kDefaultContentHashAlgorithm;

        if (!ParseAlgorithm(algo_name, algo))
        {
            GLogError(u8"不认识的哈希算法：%s", algo_name.c_str());
            GLogError(u8"支持：BLAKE3 / MD5 / SHA1 / SHA256 / SHA512 / XXH3-128 / XXH3-64 / XXH64 / CRC32");
            return 2;
        }

        const AnsiString canonical_algo = CanonicalAlgorithmName(algo);

        // ── 3. 复用运行期的扫描与对拍 ───────────────────────────────────
        //
        // save_manifest 关掉：写盘由本工具在算完哈希之后统一做一次，否则会先
        // 落一份「没有哈希」的中间清单、再落一份完整的，白写一次。
        AssetsSourceMountOptions mount_options;

        mount_options.build_index   = true;
        mount_options.load_manifest = true;
        mount_options.save_manifest = false;

        AssetSource *source = CreateSourceByFilesystem(AnsiString(kTempMountName),
                                                       opt.root, true, mount_options);

        AssetIndexTable work_table;

        if (!source || !source->GetIndexTable())
        {
            GLogError(u8"无法为目录建立索引：%s", Narrow(opt.root).c_str());

            if (source)
                UnregisterAssetsSource(AnsiString(kTempMountName));

            return 2;
        }

        work_table.CopyFrom(*source->GetIndexTable());
        work_table.SetHashAlgorithm(canonical_algo);

        UnregisterAssetsSource(AnsiString(kTempMountName));

        // ── 4. 逐文件算内容哈希 ─────────────────────────────────────────
        ManagedArray<AssetIndexEntry> entries;
        DiffReport                    report;

        work_table.GetAllEntries(entries);

        GLogInfo(u8"扫描完成：%d 个资产（清单原有 %d 项）",
                 entries.GetCount(), old_table.GetCount());

        for (int i = 0; i < entries.GetCount(); ++i)
        {
            AssetIndexEntry &entry = *entries[i];

            AssetIndexEntry old_entry;

            const bool existed = old_table.FindByID(entry.id, old_entry);

            if (!existed)
            {
                ++report.added;
                report.Line("新增", entry);
            }
            else if (old_entry.size != entry.size || old_entry.mtime != entry.mtime)
            {
                ++report.meta_changed;
                report.Line("已改", entry);
            }

            if (!opt.compute_hash)
                continue;

            const OSString full = filesystem::JoinPathWithFilename(opt.root,
                                                                  ToOSString(entry.physical_path));

            AnsiString hex;

            if (!ComputeFileHash(full, algo, hex))
            {
                ++report.hash_failed;
                GLogWarning(u8"  无法计算哈希：%s", Narrow(entry.physical_path).c_str());

                entry.content_hash.Clear();
            }
            else
            {
                if (existed && !(old_entry.content_hash == hex))
                    ++report.hash_changed;

                entry.content_hash = hex;
            }

            work_table.AddOrUpdate(entry);
        }

        // ── 5. 清单里有、磁盘上没有的条目 ───────────────────────────────
        {
            ManagedArray<AssetIndexEntry> old_entries;

            old_table.GetAllEntries(old_entries);

            for (int i = 0; i < old_entries.GetCount(); ++i)
            {
                AssetIndexEntry probe;

                if (work_table.FindByID(old_entries[i]->id, probe))
                    continue;

                ++report.removed;
                report.Line("已删", *old_entries[i]);
            }
        }

        // ── 6. 报告 ─────────────────────────────────────────────────────
        report.FlushOverflow();

        GLogInfo(u8"新增 %d / 已删 %d / 内容变化 %d / 元信息变化 %d / 哈希失败 %d",
                 report.added, report.removed, report.hash_changed,
                 report.meta_changed, report.hash_failed);

        GLogInfo(u8"包级哈希算法：%s", canonical_algo.c_str());

        if (!report.HasDifference())
            GLogInfo(u8"清单与目录一致");

        // ── 7. 写回 / 退出码 ────────────────────────────────────────────
        if (opt.check_only)
        {
            // CI 的闸门只看两件事：有没有差异、有没有算不出来。任何一项都意味着
            // 这份清单不能代表当前这批文件。
            if (report.HasDifference() || report.hash_failed != 0)
            {
                GLogError(u8"--check-only：清单与目录不一致");
                return 1;
            }

            return 0;
        }

        AssetManifestInfo save_info = manifest;

        save_info.version        = kAssetManifestVersion;
        save_info.hash_algorithm = canonical_algo;
        save_info.updated_at     = hgl::GetUnixTimestampSec();

        if (save_info.pack_name.IsEmpty())
        {
            // 包名取目录名：它是给人看的标识；缺失时留空会让清单看起来像残缺。
            save_info.pack_name = Narrow(opt.root);
        }

        AnsiString save_error;

        if (!SaveAssetManifest(manifest_path, work_table, save_info, &save_error))
        {
            GLogError(u8"写回清单失败：%s", save_error.c_str());
            return 2;
        }

        GLogInfo(u8"已写回：%s", Narrow(manifest_path).c_str());

        return report.hash_failed != 0 ? 1 : 0;
    }//int RunTool(const Options &opt)
}//namespace

int main(int argc, char **argv)
{
    hgl::logger::InitLogger(OS_TEXT("AssetManifestTool"));

    Options opt;

    if (!ParseArgs(argc, argv, opt))
    {
        PrintUsage();
        return 2;
    }

    return RunTool(opt);
}
