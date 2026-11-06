// TestAssetManifest.cpp —— `.asset_manifest.toml` 的读写契约判据。
//
// 锁定五件事：
//   1. 往返：写入 → 读出后，身份、类型、逻辑路径、物理路径、size、mtime、
//      内容哈希必须逐字段一致；
//   2. 包级算法：`hash_algorithm` 是包级属性，缺省补 BLAKE3，不是条目级；
//   3. 缺省政府：文件不存在 与 解析失败 必须是两种不同的结果 ——
//      混为一谈会在数据损坏时静默重排整包身份；
//   4. 稳定输出：同一张表两次写出的字节必须完全一致，且条目按
//      (类型, 逻辑路径) 有序 —— 清单要进版本库，diff 必须只增长不重排；
//   5. 容错：单条坏行不得拖垮整份清单，但必须报出坏了几条。
#include <hgl/asset/AssetManifest.h>
#include <hgl/asset/AssetIndex.h>

#include <hgl/filesystem/FileSystem.h>
#include <hgl/log/Log.h>
#include <hgl/log/Logger.h>
#include <hgl/time/Time.h>

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

    void CheckText(const AnsiString &actual, const char *expect, const char *what)
    {
        if (actual == expect)
        {
            ++g_pass;
            return;
        }

        ++g_fail;
        GLogError(u8"失败：%s 实际=[%s] 期望=[%s]", what, actual.c_str(), expect);
    }

    /// `String::FindString` 是成员函数，这里包一层便于断言
    int IndexOf(const AnsiString &text, const char *needle)
    {
        return text.FindString(AnsiString(needle), 0);
    }

    bool Contains(const AnsiString &text, const char *needle)
    {
        return IndexOf(text, needle) >= 0;
    }

    int CountOf(const AnsiString &text, const char *needle)
    {
        int count = 0;
        int pos   = 0;

        const AnsiString key(needle);

        while ((pos = text.FindString(key, pos)) >= 0)
        {
            ++count;
            ++pos;
        }

        return count;
    }

    const OSString kTmpRoot = OS_TEXT("_manifest_test_tmp");

    AssetIndexEntry MakeEntry(const char *physical, const char *hash = "")
    {
        AssetIndexEntry entry;

        DeriveLogical(U8(physical), AssetType::Unknown, entry.type, entry.logical_path);

        entry.physical_path = U8(physical);
        entry.id            = AssetID::Create();
        entry.size          = 1024;
        entry.mtime         = 1729900000;
        entry.content_hash  = hash;

        return entry;
    }

    void CleanupTmp()
    {
        const OSString manifest = GetAssetManifestPath(kTmpRoot);

        filesystem::FileDelete(manifest);
        filesystem::FileDelete(manifest + OS_TEXT(".tmp"));
        filesystem::DeletePath(kTmpRoot);
    }

    // ── 文本写出：格式与稳定序 ─────────────────────────────────────────
    void TestWriteText()
    {
        AssetIndexTable table;

        table.SetHashAlgorithm("BLAKE3");

        const AssetIndexEntry hero  = MakeEntry("models/hero.geo.toml", "aabbccdd");
        const AssetIndexEntry brick = MakeEntry("tex/brick.tex");
        const AssetIndexEntry scene = MakeEntry("scenes/main.scene.toml");

        // 故意乱序插入，验证写出的顺序由排序决定而不是插入顺序
        table.AddOrUpdate(scene);
        table.AddOrUpdate(hero);
        table.AddOrUpdate(brick);

        AssetManifestInfo info;

        info.pack_name  = "res";
        info.updated_at = 1730000000;

        const AnsiString text = WriteAssetManifestText(table, info);

        Check(text.Length() > 0, "写出文本非空");

        Check(Contains(text, "manifest_version = 1"), "文本含版本号");
        Check(Contains(text, "pack_name = \"res\""), "文本含包名");
        Check(Contains(text, "hash_algorithm = \"BLAKE3\""), "文本含包级算法");
        Check(Contains(text, "updated_at = 1730000000"), "文本含更新时间");

        Check(CountOf(text, "[[assets]]") == 3, "条目数 = 3");

        // 稳定序：geo 在前、scene 次之、tex 最后（按类型段字典序）
        const int geo_pos   = IndexOf(text, "type = \"geo\"");
        const int scene_pos = IndexOf(text, "type = \"scene\"");
        const int tex_pos   = IndexOf(text, "type = \"tex\"");

        Check(geo_pos >= 0 && scene_pos >= 0 && tex_pos >= 0, "三种类型都写出");
        Check(geo_pos < scene_pos && scene_pos < tex_pos, "条目按类型段有序");

        // 无内容哈希的条目不得写出 hash 键
        Check(Contains(text, "aabbccdd"), "有哈希的条目写出哈希");
        Check(!Contains(text, "hash = \"\""), "无哈希的条目不写空哈希键");

        // 最后一行只应有一个换行
        Check(text.c_str()[text.Length() - 1] == '\n', "以单个换行结束");
        Check(text.Length() < 2 || text.c_str()[text.Length() - 2] != '\n', "末尾不得有多余空行");

        // 两次写出必须逐字节一致
        const AnsiString again = WriteAssetManifestText(table, info);

        Check(text == again, "同表两次写出字节一致");
    }

    // ── 往返 ───────────────────────────────────────────────────────────
    void TestRoundTrip()
    {
        CleanupTmp();

        filesystem::MakePath(kTmpRoot);

        AssetIndexTable table;

        table.SetHashAlgorithm("BLAKE3");

        const AssetIndexEntry hero  = MakeEntry("models/hero.geo.toml", "0123456789abcdef");
        const AssetIndexEntry brick = MakeEntry("tex/brick.tex", "fedcba9876543210");
        const AssetIndexEntry scene = MakeEntry("scenes/main.scene.toml");

        table.AddOrUpdate(hero);
        table.AddOrUpdate(brick);
        table.AddOrUpdate(scene);

        AssetManifestInfo info;

        info.pack_name  = "res";
        info.updated_at = 1730000000;

        const OSString filename = GetAssetManifestPath(kTmpRoot);

        AnsiString error;

        Check(SaveAssetManifest(filename, table, info, &error), "保存清单", error.c_str());
        Check(filesystem::FileExist(filename), "清单文件已落盘");
        Check(!filesystem::FileExist(filename + OS_TEXT(".tmp")), "临时文件已清掉");

        AssetIndexTable loaded;
        AssetManifestInfo loaded_info;

        error.Clear();
        Check(LoadAssetManifest(filename, loaded, loaded_info, &error), "读回清单", error.c_str());

        Check(loaded.GetCount() == 3, "读回 3 项");
        CheckText(loaded.GetHashAlgorithm(), "BLAKE3", "包级算法读回");
        CheckText(loaded_info.pack_name, "res", "包名读回");
        Check(loaded_info.updated_at == 1730000000, "更新时间读回");
        Check(loaded_info.version == kAssetManifestVersion, "版本读回");

        // 逐字段核对几何条目
        AssetIndexEntry out;

        Check(loaded.FindByID(hero.id, out), "按 ID 读回 hero");
        Check(out.type == AssetType::Geometry, "type 一致");
        Check(out.logical_path == U8("models/hero"), "logical_path 一致");
        Check(out.physical_path == U8("models/hero.geo.toml"), "physical_path 一致");
        Check(out.size == 1024, "size 一致");
        Check(out.mtime == 1729900000, "mtime 一致");
        CheckText(out.content_hash, "0123456789abcdef", "content_hash 一致");

        Check(loaded.FindByPath(AssetType::Texture, U8("tex/brick"), out), "按路径读回 brick");
        Check(out.id == brick.id, "brick ID 一致");
        CheckText(out.content_hash, "fedcba9876543210", "brick 哈希一致");

        Check(loaded.FindByPath(AssetType::Scene, U8("scenes/main"), out), "按路径读回 scene");
        Check(!out.HasContentHash(), "无哈希条目读回后仍无哈希");

        CleanupTmp();
    }

    // ── 缺省与失败语义 ─────────────────────────────────────────────────
    void TestMissingAndBroken()
    {
        CleanupTmp();

        filesystem::MakePath(kTmpRoot);

        const OSString filename = GetAssetManifestPath(kTmpRoot);

        AssetIndexTable table;
        AssetManifestInfo info;
        AnsiString error;

        // 1. 文件不存在：必须与「解析失败」区分开
        Check(!LoadAssetManifest(filename, table, info, &error), "不存在的清单返回 false");
        CheckText(error, "manifest not found", "缺省错误必须是 not found");
        Check(table.GetCount() == 0, "不存在的清单不产生条目");

        // 2. 解析失败
        static const char kBroken[] = "manifest_version = = 3\n";

        filesystem::SaveMemoryToFile(filename, kBroken, static_cast<int64>(sizeof(kBroken) - 1));

        error.Clear();
        Check(!LoadAssetManifest(filename, table, info, &error), "损坏的清单返回 false");
        Check(!(error == "manifest not found"), "损坏必须与缺失区分");
        Check(Contains(error, "parse failed"), "错误文案指明解析失败");

        // 3. 版本过新：拒绝而不是硬猜
        static const char kFuture[] =
            "manifest_version = 99\n"
            "hash_algorithm = \"BLAKE3\"\n";

        filesystem::SaveMemoryToFile(filename, kFuture, static_cast<int64>(sizeof(kFuture) - 1));

        error.Clear();
        Check(!LoadAssetManifest(filename, table, info, &error), "版本过新返回 false");
        Check(Contains(error, "too new"), "错误文案指明版本过新");

        // 4. 空包（只有头）：合法
        static const char kEmpty[] =
            "manifest_version = 1\n"
            "pack_name = \"empty\"\n";

        filesystem::SaveMemoryToFile(filename, kEmpty, static_cast<int64>(sizeof(kEmpty) - 1));

        AssetIndexTable empty_table;
        AssetManifestInfo empty_info;

        error.Clear();
        Check(LoadAssetManifest(filename, empty_table, empty_info, &error), "空包可读", error.c_str());
        Check(empty_table.GetCount() == 0, "空包无条目");
        CheckText(empty_info.hash_algorithm, "BLAKE3", "缺省补齐 BLAKE3");
        CheckText(empty_info.pack_name, "empty", "空包包名读回");

        // 5. 坏条目：能读的读进来，但必须报出坏了几条
        static const char kPartial[] =
            "manifest_version = 1\n"
            "hash_algorithm = \"MD5\"\n"
            "\n"
            "[[assets]]\n"
            "id = \"0190f2c10000000000000000000000aa\"\n"
            "type = \"geo\"\n"
            "path = \"good/one\"\n"
            "file = \"good/one.geo.toml\"\n"
            "size = 7\n"
            "mtime = 8\n"
            "\n"
            "[[assets]]\n"
            "id = \"not-a-uuid\"\n"
            "type = \"geo\"\n"
            "path = \"bad/id\"\n"
            "file = \"bad/id.geo.toml\"\n"
            "\n"
            "[[assets]]\n"
            "id = \"0190f2c10000000000000000000000bb\"\n"
            "type = \"nosuchtype\"\n"
            "path = \"bad/type\"\n"
            "file = \"bad/type.nosuchtype.toml\"\n";

        filesystem::SaveMemoryToFile(filename, kPartial, static_cast<int64>(sizeof(kPartial) - 1));

        AssetIndexTable partial;

        error.Clear();
        Check(LoadAssetManifest(filename, partial, empty_info, &error), "坏条目不影响整体可读");
        Check(partial.GetCount() == 1, "只有 1 条有效条目");
        Check(error.Length() > 0, "坏条目必须报出计数");
        Check(Contains(error, "id=1"), "错误里含坏 ID 计数");
        Check(Contains(error, "type=1"), "错误里含坏类型计数");
        CheckText(partial.GetHashAlgorithm(), "MD5", "算法随清单读回");

        CleanupTmp();
    }

    // ── 追加/合并语义 ──────────────────────────────────────────────────
    void TestMergeIntoExisting()
    {
        CleanupTmp();

        filesystem::MakePath(kTmpRoot);

        const OSString filename = GetAssetManifestPath(kTmpRoot);

        AssetIndexTable written;
        AssetManifestInfo info;

        written.AddOrUpdate(MakeEntry("a/one.geo.toml"));
        written.AddOrUpdate(MakeEntry("a/two.geo.toml"));

        AnsiString error;

        Check(SaveAssetManifest(filename, written, info, &error), "保存两项", error.c_str());

        // 目标表里先放一项，读入必须合并而不是清空
        AssetIndexTable merged;

        const AssetIndexEntry pre = MakeEntry("pre/existing.tex");

        merged.AddOrUpdate(pre);

        AssetManifestInfo read_info;

        Check(LoadAssetManifest(filename, merged, read_info, &error), "合并读入", error.c_str());
        Check(merged.GetCount() == 3, "合并后 3 项");
        Check(merged.ContainsID(pre.id), "既有项未被清空");

        // updated_at = 0 时应自动填当前时间
        AssetManifestInfo timed;

        timed.updated_at = 0;

        const uint64 before = hgl::GetUnixTimestampSec();

        Check(SaveAssetManifest(filename, written, timed, &error), "保存并自动填时间", error.c_str());
        Check(timed.updated_at >= before, "updated_at 被填充为当前时间");

        CleanupTmp();
    }

    // ── 转义 ───────────────────────────────────────────────────────────
    void TestEscape()
    {
        CleanupTmp();

        filesystem::MakePath(kTmpRoot);

        AssetIndexTable table;

        AssetIndexEntry quote = MakeEntry("a/b.geo.toml");

        quote.logical_path  = U8("a/b\"quote");
        quote.physical_path = U8("a/b\"quote.geo.toml");

        Check(table.AddOrUpdate(quote), "插入含引号的条目");

        AssetManifestInfo info;
        AnsiString error;

        const OSString filename = GetAssetManifestPath(kTmpRoot);

        Check(SaveAssetManifest(filename, table, info, &error), "保存含引号的清单", error.c_str());

        AssetIndexTable loaded;
        AssetManifestInfo read_info;

        error.Clear();
        Check(LoadAssetManifest(filename, loaded, read_info, &error), "读回含引号的清单", error.c_str());
        Check(loaded.GetCount() == 1, "含引号条目可往返");

        AssetIndexEntry out;

        Check(loaded.FindByID(quote.id, out), "按 ID 命中");
        Check(out.logical_path == quote.logical_path, "引号被正确转义并还原");
        Check(out.physical_path == quote.physical_path, "物理路径引号被还原");

        CleanupTmp();
    }
}

int main(int, char **)
{
    hgl::logger::InitLogger(OS_TEXT("TestAssetManifest"));

    TestWriteText();
    TestRoundTrip();
    TestMissingAndBroken();
    TestMergeIntoExisting();
    TestEscape();

    if (g_fail != 0)
    {
        GLogError(u8"=== TestAssetManifest: %d 通过 / %d 失败 ===", g_pass, g_fail);
        return 1;
    }

    GLogInfo(u8"=== TestAssetManifest: %d 项全部通过 ===", g_pass);
    return 0;
}
