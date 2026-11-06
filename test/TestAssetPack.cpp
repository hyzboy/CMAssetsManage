// TestAssetPack.cpp —— 包裹（pack）机制的契约判据。
//
// 锁定三件事：
//   1. `pack(<name>)` 来源段的拼写与 URI 往返；
//   2. 包裹内路径规范化（拒绝越界、统一分隔符）；
//   3. 目录包裹作为「位置透明后端」的真实读写能力
//      —— 将来换成真实包裹文件时，这一组用例就是回归基线。
#include <hgl/asset/AssetURI.h>
#include <hgl/asset/AssetSource.h>
#include <hgl/asset/AssetIndex.h>
#include <hgl/asset/AssetManifest.h>
#include <hgl/asset/AssetStream.h>
#include <hgl/filesystem/FileSystem.h>
#include <hgl/filesystem/EnumFile.h>
#include <hgl/io/InputStream.h>
#include <hgl/utf.h>
#include <hgl/log/Log.h>
#include <hgl/log/Logger.h>

using namespace hgl;
using namespace hgl::asset;

namespace
{
    int g_fail = 0;
    int g_pass = 0;

    U8String U8(const char *str)
    {
        return U8String(reinterpret_cast<const u8char *>(str));
    }

    AnsiString Narrow(const OSString &str)
    {
        const U8String u8_str = ToU8String(str);
        return AnsiString(reinterpret_cast<const char *>(u8_str.c_str()), u8_str.Length());
    }

    void Check(bool cond, const char *what, const char *detail = "")
    {
        if (cond)
        {
            ++g_pass;
            return;
        }

        ++g_fail;
        GLogError(u8"失败：%s %s", what, detail);
    }

    void CheckText(const OSString &actual, const OSString &expect, const char *what)
    {
        if (actual == expect)
        {
            ++g_pass;
            return;
        }

        ++g_fail;
        GLogError(u8"失败：%s 实际=[%s] 期望=[%s]",
                  what, Narrow(actual).c_str(), Narrow(expect).c_str());
    }

    void CheckU8(const U8String &actual, const char *expect, const char *what)
    {
        if (actual == U8(expect))
        {
            ++g_pass;
            return;
        }

        ++g_fail;
        GLogError(u8"失败：%s", what);
    }

    bool SameBytes(const char *left, const char *right, int count)
    {
        for (int i = 0; i < count; ++i)
            if (left[i] != right[i])
                return false;

        return true;
    }

    /// `AnsiString` 里是否含某段文本（`FindString` 是成员函数，这里包一层）
    bool Contains(const AnsiString &text, const char *needle)
    {
        return text.FindString(AnsiString(needle), 0) >= 0;
    }

    /// 递归删除一个目录树。
    ///
    /// 不能用 `filesystem::DeleteTree`：它在 Windows 上只有声明、没有实现
    /// （见 CMCore 已知缺口）；`DeletePath` 又是单层删除，遇到非空目录会失败，
    /// 于是测试会在源码目录里留下垃圾。这里用 `EnumFile` 自底向上删。
    void RemoveTree(const OSString &path);

    class TreeRemover : public filesystem::EnumFile
    {
    protected:

        filesystem::EnumFileConfig *CreateSubConfig(filesystem::EnumFileConfig *up_efc,
                                                    const filesystem::FileInfo &fi) override
        {
            // 先递归清空子目录，再告诉基类「不要继续往下走」。
            RemoveTree(filesystem::JoinPathWithFilename(up_efc->folder_name, fi.name));

            return nullptr;
        }

        void ProcFile(filesystem::EnumFileConfig *cfg, filesystem::FileInfo &fi) override
        {
            filesystem::FileDelete(filesystem::JoinPathWithFilename(cfg->folder_name, fi.name));
        }
    };

    void RemoveTree(const OSString &path)
    {
        if (path.IsEmpty())
            return;

        if (!filesystem::IsDirectory(path.c_str()))
        {
            filesystem::FileDelete(path);
            return;
        }

        TreeRemover remover;

        filesystem::EnumFileConfig config(path);

        config.proc_folder = true;
        config.proc_file   = true;
        config.sub_folder  = true;

        remover.Enum(&config);

        filesystem::DeletePath(path);
    }

    /// 清掉一个挂载根：清单 + 目录。
    ///
    /// `DeletePath` 是单层删除，目录里还有文件时会失败 —— 所以这里先递归清空。
    void CleanupRoot(const OSString &root)
    {
        filesystem::FileDelete(GetAssetManifestPath(root));
        filesystem::FileDelete(GetAssetManifestPath(root) + OS_TEXT(".tmp"));
        RemoveTree(root);
    }

    /// 建出一个文件；返回是否成功
    bool WriteFile(const OSString &folder, const OSString &name, const char *payload)
    {
        filesystem::MakePath(folder);

        const OSString full = filesystem::JoinPathWithFilename(folder, name);

        return filesystem::SaveMemoryToFile(full, payload,
                                            static_cast<int64>(strlen(payload))) > 0;
    }

    // ── pack 段拼写 ────────────────────────────────────────────────────
    void TestPackSegment()
    {
        AnsiString name;

        Check(ParsePackSegment("pack(res)", name), "ParsePackSegment(pack(res))");
        Check(name == "res", "ParsePackSegment 名字 = res");

        Check(ParsePackSegment("PACK(Res_1-x)", name), "ParsePackSegment 大小写不敏感");
        Check(name == "Res_1-x", "ParsePackSegment 保留原拼写");

        name = "untouched";
        Check(!ParsePackSegment("pack()", name), "ParsePackSegment(pack()) 应拒绝");
        Check(name.IsEmpty(), "失败时必须清空输出");

        Check(!ParsePackSegment("pack(a b)", name), "名字含空格应拒绝");
        Check(!ParsePackSegment("pack(a/b)", name), "名字含分隔符应拒绝");
        Check(!ParsePackSegment("pack(a:b)", name), "名字含冒号应拒绝");
        Check(!ParsePackSegment("pack(a(b))", name), "嵌套括号应拒绝");
        Check(!ParsePackSegment("pack(a", name), "缺少右括号应拒绝");
        Check(!ParsePackSegment("pack", name), "裸 pack 应拒绝");
        Check(!ParsePackSegment("", name), "空串应拒绝");

        // 33 个字符超长
        Check(!ParsePackSegment("pack(abcdefghijklmnopqrstuvwxyz0123456)", name),
              "名字超过 32 字符应拒绝");

        Check(MakePackSegment("res") == "pack(res)", "MakePackSegment(res)");
        Check(MakePackSegment(AnsiString()).IsEmpty(), "MakePackSegment 空名返回空串");
        Check(MakePackSegment("a b").IsEmpty(), "MakePackSegment 非法名返回空串");
    }

    // ── URI 往返 ───────────────────────────────────────────────────────
    void TestPackURI()
    {
        AssetURI uri = AssetURI::Parse(OS_TEXT("geo:pack(res):geo/unit.geo.toml"));

        Check(uri.type == AssetType::Geometry, "pack URI 类型段");
        Check(uri.source == AssetSourceKind::Pack, "pack URI 来源段 = Pack");
        Check(uri.pack_name == "res", "pack URI 挂载名");
        Check(uri.IsPack(), "IsPack()");
        CheckText(uri.target, OS_TEXT("geo/unit.geo.toml"), "pack URI target");

        AssetURI omit_type = AssetURI::Parse(OS_TEXT("pack(res):geo/x.geo.toml"));

        Check(omit_type.type == AssetType::Unknown, "省略类型时 type 保持 Unknown");
        Check(omit_type.source == AssetSourceKind::Pack, "省略类型时仍识别 pack");
        Check(omit_type.pack_name == "res", "省略类型时挂载名");
        CheckText(omit_type.target, OS_TEXT("geo/x.geo.toml"), "省略类型时 target");

        // `pack` 单独一段不是合法来源段，必须落回「整段当 target」的旧行为。
        AssetURI bare_pack = AssetURI::Parse(OS_TEXT("geo:pack:foo"));

        Check(bare_pack.source == AssetSourceKind::Unknown, "geo:pack:foo 来源段应为 Unknown");
        Check(bare_pack.pack_name.IsEmpty(), "geo:pack:foo 不应有挂载名");
        CheckText(bare_pack.target, OS_TEXT("pack:foo"), "geo:pack:foo 整段当 target");

        CheckText(uri.ToString(), OS_TEXT("geo:pack(res):geo/unit.geo.toml"), "pack URI ToString");
        CheckText(AssetURI::Normalize(OS_TEXT("geo:pack(res):geo/unit.geo.toml")),
                  OS_TEXT("geo:pack(res):geo/unit.geo.toml"), "pack URI Normalize 不变");

        CheckText(AssetURI::MakePackURI(AssetType::Geometry, "res", OS_TEXT("a.geo.toml")),
                  OS_TEXT("geo:pack(res):a.geo.toml"), "MakePackURI 带类型");
        CheckText(AssetURI::MakePackURI(AssetType::Unknown, "res", OS_TEXT("a.geo.toml")),
                  OS_TEXT("pack(res):a.geo.toml"), "MakePackURI 省略类型");
        Check(AssetURI::MakePackURI(AssetType::Geometry, "a b", OS_TEXT("x")).IsEmpty(),
              "MakePackURI 非法名返回空串");
    }

    // ── 路径规范化 ─────────────────────────────────────────────────────
    void TestNormalizePackPath()
    {
        U8String out;

        Check(NormalizePackPath(U8("geo/a.toml"), out), "Normalize 普通相对路径");
        CheckU8(out, "geo/a.toml", "Normalize 普通相对路径结果");

        Check(NormalizePackPath(U8("geo//a.toml"), out), "Normalize 合并分隔符");
        CheckU8(out, "geo/a.toml", "Normalize 合并分隔符结果");

        Check(NormalizePackPath(U8("geo\\a.toml"), out), "Normalize 反斜杠");
        CheckU8(out, "geo/a.toml", "Normalize 反斜杠结果");

        Check(NormalizePackPath(U8("./geo/./a.toml"), out), "Normalize 忽略点段");
        CheckU8(out, "geo/a.toml", "Normalize 忽略点段结果");

        Check(!NormalizePackPath(U8("geo/../a.toml"), out), "Normalize 拒绝 ..");
        Check(out.IsEmpty(), "Normalize 失败时清空");

        Check(!NormalizePackPath(U8(".."), out), "Normalize 拒绝裸 ..");
        Check(!NormalizePackPath(U8("/geo/a.toml"), out), "Normalize 拒绝绝对路径");
        Check(!NormalizePackPath(U8("\\geo\\a.toml"), out), "Normalize 拒绝反斜杠绝对路径");
        Check(!NormalizePackPath(U8("C:/geo/a.toml"), out), "Normalize 拒绝盘符路径");
        Check(!NormalizePackPath(U8(""), out), "Normalize 拒绝空路径");
        Check(!NormalizePackPath(U8("///"), out), "Normalize 拒绝纯分隔符");
    }

    // ── 目录包裹后端 ───────────────────────────────────────────────────
    void TestDirectoryPack()
    {
        const OSString tmp_root = OS_TEXT("_pack_test_tmp");
        const OSString geo_dir  = filesystem::JoinPathWithFilename(tmp_root, OS_TEXT("geo"));
        const OSString file     = filesystem::JoinPathWithFilename(geo_dir, OS_TEXT("a.geo.toml"));

        CleanupRoot(tmp_root);

        filesystem::MakePath(tmp_root);
        filesystem::MakePath(geo_dir);

        static const char kPayload[] = "hello pack";

        Check(filesystem::SaveMemoryToFile(file, kPayload, static_cast<int64>(sizeof(kPayload) - 1)) > 0,
              "准备测试文件");

        AssetSource *src = CreateSourceByFilesystem(AnsiString("res"), tmp_root, true);

        Check(src != nullptr, "挂载目录包裹 res");
        Check(GetSourceCount() == 1, "挂载表计数为 1");
        Check(GetSource(AnsiString("res")) == src, "按名查回同一个来源");
        Check(CreateSourceByFilesystem(AnsiString("res"), tmp_root, true) == nullptr, "同名重复挂载必须失败");

        Check(src != nullptr && src->Exists(U8("geo/a.geo.toml")), "Exists 命中");
        Check(src != nullptr && !src->Exists(U8("geo/none.toml")), "Exists 未命中");

        SourceEntry entry;

        Check(src != nullptr && src->Stat(U8("geo/a.geo.toml"), entry), "Stat 命中");
        Check(entry.size == sizeof(kPayload) - 1, "Stat 字节数");
        Check(!entry.is_directory, "Stat 文件不是目录");

        Check(src != nullptr && src->Stat(U8("geo"), entry), "Stat 目录命中");
        Check(entry.is_directory, "Stat 目录标记");

        // 越界路径必须被后端拒绝
        Check(src != nullptr && !src->Exists(U8("../a.txt")), "Exists 拒绝越界路径");
        Check(src != nullptr && src->Open(U8("../a.txt")) == nullptr, "Open 拒绝越界路径");

        if (src != nullptr)
        {
            ManagedArray<SourceEntry> list;

            const int count = src->ListDirectory(U8(""), list);

            Check(count > 0, "ListDirectory 根目录有子项");

            bool found_geo = false;

            for (int i = 0; i < list.GetCount(); ++i)
                if (list[i]->name == U8("geo") && list[i]->is_directory)
                    found_geo = true;

            Check(found_geo, "ListDirectory 发现 geo 子目录");
        }

        // 统一取流入口
        AnsiString error;
        io::InputStream *is = OpenAssetStream(OS_TEXT("geo:pack(res):geo/a.geo.toml"), AssetType::Unknown, &error);

        Check(is != nullptr, "OpenAssetStream 经包裹取流", error.c_str());

        if (is != nullptr)
        {
            char buf[64] = {0,};

            const int64 n = is->Read(buf, sizeof(buf) - 1);

            Check(n == static_cast<int64>(sizeof(kPayload) - 1), "包裹取流长度");
            Check(SameBytes(buf, kPayload, sizeof(kPayload) - 1), "包裹取流内容");

            delete is;
        }

        // 未挂载的包裹名
        error.Clear();
        Check(OpenAssetStream(OS_TEXT("geo:pack(nope):a.geo.toml"), AssetType::Unknown, &error) == nullptr,
              "未挂载包裹取流失败");

        // 挂载名合法但路径越界
        error.Clear();
        Check(OpenAssetStream(OS_TEXT("geo:pack(res):../a.geo.toml"), AssetType::Unknown, &error) == nullptr,
              "包裹内越界取流失败");

        Check(UnregisterAssetsSource(AnsiString("res")), "注销包裹");
        Check(GetSourceCount() == 0, "注销后挂载表为空");
        Check(GetSource(AnsiString("res")) == nullptr, "注销后查不到");

        CleanupRoot(tmp_root);
    }

    // ── 挂载扫描与索引 ─────────────────────────────────────────────────
    //
    // 这一组是「目录包裹 → 打包文件」切换时最关键的回归基线：它锁定的不是
    // 目录后端的实现细节，而是**索引语义** —— 身份持久、逻辑寻址、
    // 内容变化时作废旧哈希。打包后端只要满足同一组断言即可无缝替换。
    void TestIndexScan()
    {
        const OSString root     = OS_TEXT("_pack_index_tmp");
        const OSString geo_dir  = filesystem::JoinPathWithFilename(root, OS_TEXT("models"));
        const OSString tex_dir  = filesystem::JoinPathWithFilename(root, OS_TEXT("tex"));
        const OSString manifest = GetAssetManifestPath(root);

        CleanupRoot(root);

        filesystem::MakePath(root);

        Check(WriteFile(geo_dir, OS_TEXT("hero.geo.toml"), "hero data"), "写入 hero.geo.toml");
        Check(WriteFile(tex_dir, OS_TEXT("brick.tex"), "brick data"), "写入 brick.tex");

        // 非资产：没有可识别的类型后缀，必须被排除在索引之外
        Check(WriteFile(root, OS_TEXT("readme.txt"), "not an asset"), "写入 readme.txt");
        Check(WriteFile(root, OS_TEXT(".hidden.geo.toml"), "hidden"), "写入隐藏文件");

        AssetSource *src = CreateSourceByFilesystem(AnsiString("idx"), root, true);

        Check(src != nullptr, "挂载并建索引");
        Check(filesystem::FileExist(manifest), "挂载后清单已写出");

        if (src == nullptr)
        {
            CleanupRoot(root);
            return;
        }

        const AssetIndexTable *table = src->GetIndexTable();

        Check(table != nullptr, "GetIndexTable 非空");
        Check(src->HasIndexTable(), "HasIndexTable 为真");
        Check(table != nullptr && table->GetCount() == 2, "索引收 2 条（排除 readme 与隐藏文件）");

        if (table != nullptr)
        {
            AssetIndexEntry hero;
            AssetIndexEntry brick;

            Check(table->FindByPath(AssetType::Geometry, U8("models/hero"), hero), "索引含 models/hero");
            Check(table->FindByPath(AssetType::Texture, U8("tex/brick"), brick), "索引含 tex/brick");

            Check(hero.physical_path == U8("models/hero.geo.toml"), "hero 物理路径正确");
            Check(brick.physical_path == U8("tex/brick.tex"), "brick 物理路径正确");
            Check(hero.size == strlen("hero data"), "hero size 正确");
            Check(hero.mtime != 0, "hero mtime 非零");
            Check(!hero.HasContentHash(), "挂载扫描不计算内容哈希");
            Check(!hero.id.IsZero(), "hero 已分配身份");

            const AssetID hero_id = hero.id;

            // ── 逻辑寻址 ──────────────────────────────────────────────
            Check(src->Exists(AssetType::Geometry, U8("models/hero")), "Exists(逻辑路径) 命中");
            Check(!src->Exists(AssetType::Geometry, U8("models/nope")), "Exists(逻辑路径) 未命中");
            Check(src->Exists(hero_id), "Exists(ID) 命中");

            U8String resolved;

            Check(src->ResolvePath(AssetType::Geometry, U8("models/hero"), resolved), "ResolvePath(逻辑) 命中");
            Check(resolved == U8("models/hero.geo.toml"), "ResolvePath 给出物理路径");
            Check(src->ResolvePath(hero_id, resolved), "ResolvePath(ID) 命中");

            // 索引未命中的逻辑路径**不得**回落成磁盘探测：这里放一个真实存在但
            // 不在索引里的文件（readme.txt），按逻辑路径查必须仍然失败。
            Check(!src->Exists(AssetType::Geometry, U8("readme")), "未索引文件不得被逻辑寻址命中");
            Check(src->Open(AssetType::Geometry, U8("readme")) == nullptr, "未索引文件不得被逻辑寻址打开");

            // 按 ID 取流
            io::InputStream *is = src->Open(hero_id);

            Check(is != nullptr, "按 ID 取流");

            if (is != nullptr)
            {
                char buf[64] = {0,};

                const int64 n = is->Read(buf, sizeof(buf) - 1);

                Check(n == static_cast<int64>(strlen("hero data")), "按 ID 取流长度");
                Check(SameBytes(buf, "hero data", static_cast<int>(strlen("hero data"))), "按 ID 取流内容");

                delete is;
            }

            // ── 身份持久：卸载后重新挂载必须复用同一批 ID ──────────────
            Check(UnregisterAssetsSource(AnsiString("idx")), "注销以验证身份持久");

            AssetSource *again = CreateSourceByFilesystem(AnsiString("idx"), root, true);

            Check(again != nullptr, "重新挂载");

            if (again != nullptr)
            {
                AssetIndexEntry hero2;

                Check(again->GetIndexTable() != nullptr
                   && again->GetIndexTable()->FindByPath(AssetType::Geometry, U8("models/hero"), hero2),
                      "重新挂载后仍索引到 hero");
                Check(hero2.id == hero_id, "重新挂载复用同一身份");

                // ── 内容变化 ⇒ 旧哈希作废 ─────────────────────────────
                AssetIndexTable manual;
                AssetManifestInfo info;

                Check(LoadAssetManifest(manifest, manual, info, nullptr) == ManifestLoadResult::Success, "读回清单以注入哈希");

                AssetIndexEntry with_hash;

                Check(manual.FindByID(hero_id, with_hash), "按 ID 找到 hero 条目");
                with_hash.content_hash = "deadbeefdeadbeef";

                manual.AddOrUpdate(with_hash);
                Check(SaveAssetManifest(manifest, manual, info, nullptr), "写入带哈希的清单");

                Check(UnregisterAssetsSource(AnsiString("idx")), "注销以重扫");

                AssetSource *rescan = CreateSourceByFilesystem(AnsiString("idx"), root, true);

                Check(rescan != nullptr, "重扫挂载");

                if (rescan != nullptr && rescan->GetIndexTable() != nullptr)
                {
                    AssetIndexEntry same;

                    Check(rescan->GetIndexTable()->FindByPath(AssetType::Geometry, U8("models/hero"), same),
                          "重扫后命中 hero");
                    Check(same.content_hash == "deadbeefdeadbeef", "size/mtime 未变时哈希保留");

                    Check(UnregisterAssetsSource(AnsiString("idx")), "注销以改内容");
                }

                // 改成不同长度 ⇒ size 变化 ⇒ 哈希必须作废
                Check(WriteFile(geo_dir, OS_TEXT("hero.geo.toml"), "hero data changed"), "改写 hero 内容");

                AssetSource *changed = CreateSourceByFilesystem(AnsiString("idx"), root, true);

                Check(changed != nullptr, "改内容后挂载");

                if (changed != nullptr && changed->GetIndexTable() != nullptr)
                {
                    AssetIndexEntry after;

                    Check(changed->GetIndexTable()->FindByPath(AssetType::Geometry, U8("models/hero"), after),
                          "改内容后仍命中 hero");
                    Check(after.id == hero_id, "改内容不换身份");
                    Check(!after.HasContentHash(), "内容变化后旧哈希必须作废");
                    Check(after.size == strlen("hero data changed"), "size 已更新");

                    Check(UnregisterAssetsSource(AnsiString("idx")), "注销以验证删除");
                }

                // ── 文件被删除 ⇒ 条目消失 ─────────────────────────────
                Check(filesystem::FileDelete(filesystem::JoinPathWithFilename(tex_dir, OS_TEXT("brick.tex"))),
                      "删除 brick.tex");

                AssetSource *shrunk = CreateSourceByFilesystem(AnsiString("idx"), root, true);

                Check(shrunk != nullptr, "删文件后挂载");

                if (shrunk != nullptr && shrunk->GetIndexTable() != nullptr)
                {
                    Check(shrunk->GetIndexTable()->GetCount() == 1, "条目随文件删除而消失");
                    Check(!shrunk->Exists(AssetType::Texture, U8("tex/brick")), "被删条目不再命中");

                    Check(UnregisterAssetsSource(AnsiString("idx")), "注销");
                }
            }
        }

        // ── 关闭建索引：退回纯路径访问 ─────────────────────────────────
        AssetsSourceMountOptions no_index;

        no_index.build_index   = false;
        no_index.load_manifest = false;
        no_index.save_manifest = false;

        AssetSource *plain = CreateSourceByFilesystem(AnsiString("plain"), root, true, no_index);

        Check(plain != nullptr, "不建索引的挂载");

        if (plain != nullptr)
        {
            Check(!plain->HasIndexTable(), "不建索引时 GetIndexTable 为 nullptr");
            Check(!plain->Exists(AssetType::Geometry, U8("models/hero")), "无索引时逻辑寻址一律未命中");

            // 取到的流必须显式释放：Windows 上留在打开状态的文件会锁住目录，
            // 让后续的测试清理删不掉。
            io::InputStream *raw = plain->Open(U8("models/hero.geo.toml"));

            Check(raw != nullptr, "无索引时仍可按原始路径打开");

            delete raw;

            Check(UnregisterAssetsSource(AnsiString("plain")), "注销 plain");
        }

        CleanupRoot(root);
    }

    // ── 无扩展名 / ID 寻址的取流 ────────────────────────────────────────
    void TestAddressing()
    {
        const OSString root    = OS_TEXT("_pack_addr_tmp");
        const OSString geo_dir = filesystem::JoinPathWithFilename(root, OS_TEXT("models"));

        CleanupRoot(root);

        filesystem::MakePath(root);

        Check(WriteFile(geo_dir, OS_TEXT("hero.geo.toml"), "hero payload"), "写入 hero");
        Check(WriteFile(root, OS_TEXT("blob.bin"), "raw blob"), "写入 blob.bin");

        AssetSource *src = CreateSourceByFilesystem(AnsiString("adr"), root, true);

        Check(src != nullptr, "挂载 adr");

        if (src == nullptr || src->GetIndexTable() == nullptr)
        {
            CleanupRoot(root);
            return;
        }

        AssetIndexEntry hero;

        Check(src->GetIndexTable()->FindByPath(AssetType::Geometry, U8("models/hero"), hero),
              "索引里有 models/hero");

        const AnsiString hero_id = hero.id.ToString();

        // U8String 拼接：id:<hex>
        U8String id_target(u8"id:");
        id_target += U8String(reinterpret_cast<const u8char *>(hero_id.c_str()), hero_id.Length());

        // ── 逻辑路径（首选形态）────────────────────────────────────────
        {
            AnsiString error;

            io::InputStream *is = OpenAssetStream(OS_TEXT("geo:pack(adr):models/hero"),
                                                  AssetType::Unknown, &error);

            Check(is != nullptr, "逻辑路径取流", error.c_str());

            if (is != nullptr)
            {
                char buf[64] = {0,};

                const int64 n = is->Read(buf, sizeof(buf) - 1);

                Check(n == static_cast<int64>(strlen("hero payload")), "逻辑路径取流长度");
                Check(SameBytes(buf, "hero payload", static_cast<int>(strlen("hero payload"))),
                      "逻辑路径取流内容");

                delete is;
            }
        }

        // ── 带扩展名的历史写法：必须收敛到同一个资产 ───────────────────
        {
            AnsiString error;

            io::InputStream *is = OpenAssetStream(OS_TEXT("geo:pack(adr):models/hero.geo.toml"),
                                                  AssetType::Unknown, &error);

            Check(is != nullptr, "带扩展名历史写法仍可取流", error.c_str());
            delete is;
        }

        // ── 按身份寻址 ─────────────────────────────────────────────────
        {
            const OSString uri = OSString(OS_TEXT("geo:pack(adr):"))
                               + ToOSString(id_target);

            AnsiString error;

            io::InputStream *is = OpenAssetStream(uri, AssetType::Unknown, &error);

            Check(is != nullptr, "ID 寻址取流", error.c_str());

            if (is != nullptr)
            {
                char buf[64] = {0,};

                const int64 n = is->Read(buf, sizeof(buf) - 1);

                Check(n == static_cast<int64>(strlen("hero payload")), "ID 寻址取流长度");
                Check(SameBytes(buf, "hero payload", static_cast<int>(strlen("hero payload"))),
                      "ID 寻址取流内容");

                delete is;
            }
        }

        // ── 不存在的 ID：必须失败，且不得回落 ──────────────────────────
        {
            const AssetID kGhost = AssetID::Create();

            const AnsiString ghost_text = kGhost.ToString();

            U8String ghost_target(u8"id:");
            ghost_target += U8String(reinterpret_cast<const u8char *>(ghost_text.c_str()),
                                     ghost_text.Length());

            const OSString uri = OSString(OS_TEXT("geo:pack(adr):")) + ToOSString(ghost_target);

            AnsiString error;

            Check(OpenAssetStream(uri, AssetType::Unknown, &error) == nullptr,
                  "不存在的 ID 必须取流失败");
            Check(Contains(error, "asset id not found"), "错误文案指明 ID 未找到");
        }

        // ── 索引里没有的逻辑路径：有索引就不得回落成路径探测 ───────────
        {
            AnsiString error;

            Check(OpenAssetStream(OS_TEXT("geo:pack(adr):models/nope"),
                                  AssetType::Unknown, &error) == nullptr,
                  "索引未命中的逻辑路径必须失败");
            Check(Contains(error, "not found in pack index"), "错误文案指明索引未命中");
        }

        // ── 非资产原始文件：推导不出类型 ⇒ 按调用方给的路径直通 ────────
        {
            AnsiString error;

            io::InputStream *is = OpenAssetStream(OS_TEXT("pack(adr):blob.bin"),
                                                  AssetType::Unknown, &error);

            Check(is != nullptr, "非资产原始文件可按路径直通", error.c_str());

            if (is != nullptr)
            {
                char buf[32] = {0,};

                const int64 n = is->Read(buf, sizeof(buf) - 1);

                Check(n == static_cast<int64>(strlen("raw blob")), "原始文件取流长度");
                Check(SameBytes(buf, "raw blob", static_cast<int>(strlen("raw blob"))),
                      "原始文件取流内容");

                delete is;
            }

            // 但它在索引里不该有位置：不能用逻辑寻址打开
            Check(!src->Exists(AssetType::Geometry, U8("blob")), "原始文件不进索引");
        }

        Check(UnregisterAssetsSource(AnsiString("adr")), "注销 adr");

        CleanupRoot(root);
    }
}

int main(int, char **)
{
    hgl::logger::InitLogger(OS_TEXT("TestAssetPack"));

    TestPackSegment();
    TestPackURI();
    TestNormalizePackPath();
    TestDirectoryPack();
    TestIndexScan();
    TestAddressing();

    ClearAssetsSource();

    if (g_fail != 0)
    {
        GLogError(u8"=== TestAssetPack: %d 通过 / %d 失败 ===", g_pass, g_fail);
        return 1;
    }

    GLogInfo(u8"=== TestAssetPack: %d 项全部通过 ===", g_pass);
    return 0;
}
