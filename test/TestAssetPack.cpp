// TestAssetPack.cpp —— 包裹（pack）机制的契约判据。
//
// 锁定三件事：
//   1. `pack(<name>)` 来源段的拼写与 URI 往返；
//   2. 包裹内路径规范化（拒绝越界、统一分隔符）；
//   3. 目录包裹作为「位置透明后端」的真实读写能力
//      —— 将来换成真实包裹文件时，这一组用例就是回归基线。
#include <hgl/asset/AssetURI.h>
#include <hgl/asset/AssetSource.h>
#include <hgl/asset/AssetStream.h>
#include <hgl/filesystem/FileSystem.h>
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

        filesystem::FileDelete(file);
        filesystem::DeletePath(geo_dir);
        filesystem::DeletePath(tmp_root);

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

        filesystem::FileDelete(file);
        filesystem::DeletePath(geo_dir);
        filesystem::DeletePath(tmp_root);
    }
}

int main(int, char **)
{
    hgl::logger::InitLogger(OS_TEXT("TestAssetPack"));

    TestPackSegment();
    TestPackURI();
    TestNormalizePackPath();
    TestDirectoryPack();

    ClearAssetsSource();

    if (g_fail != 0)
    {
        GLogError(u8"=== TestAssetPack: %d 通过 / %d 失败 ===", g_pass, g_fail);
        return 1;
    }

    GLogInfo(u8"=== TestAssetPack: %d 项全部通过 ===", g_pass);
    return 0;
}
