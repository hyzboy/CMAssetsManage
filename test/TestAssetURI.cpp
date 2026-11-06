// TestAssetURI.cpp —— 统一资产 URI 的解析/拼写判据。
//
// 这里锁定的是「文本 -> 段」的契约，不涉及任何资产实现，因此可以在 CI 里
// 挡住 "URI 语法被悄悄改坏" 这类静默失效。
#include <hgl/asset/AssetURI.h>
#include <hgl/utf.h>
#include <hgl/log/Log.h>
#include <hgl/log/Logger.h>

using namespace hgl;
using namespace hgl::asset;

namespace
{
    int g_fail = 0;
    int g_pass = 0;

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

    // ── 类型/来源拼写表 ────────────────────────────────────────────────
    void TestSpellingTables()
    {
        Check(AnsiString(GetAssetTypeName(AssetType::Geometry)) == "geo", "GetAssetTypeName(geo)");
        Check(AnsiString(GetAssetTypeName(AssetType::Texture)) == "tex", "GetAssetTypeName(tex)");
        Check(AnsiString(GetAssetTypeName(AssetType::MaterialRecipe)) == "rcp", "GetAssetTypeName(rcp)");
        Check(AnsiString(GetAssetTypeName(AssetType::Primitive)) == "prim", "GetAssetTypeName(prim)");
        Check(AnsiString(GetAssetTypeName(AssetType::Scene)) == "scene", "GetAssetTypeName(scene)");

        Check(ParseAssetType("geo") == AssetType::Geometry, "ParseAssetType(geo)");
        Check(ParseAssetType("GEO") == AssetType::Geometry, "ParseAssetType(GEO) 应大小写不敏感");
        Check(ParseAssetType(" nope ") == AssetType::Unknown, "ParseAssetType 不做 Trim");
        Check(ParseAssetType("nope") == AssetType::Unknown, "ParseAssetType(nope) 应为 Unknown");
        Check(ParseAssetType("") == AssetType::Unknown, "ParseAssetType(\"\") 应为 Unknown");
        Check(ParseAssetType(AnsiString()) == AssetType::Unknown, "ParseAssetType(空 AnsiString)");

        Check(AnsiString(GetAssetSourceKindName(AssetSourceKind::Builtin)) == "builtin", "source(builtin)");
        Check(AnsiString(GetAssetSourceKindName(AssetSourceKind::File)) == "file", "source(file)");
        Check(AnsiString(GetAssetSourceKindName(AssetSourceKind::RenderTarget)) == "rt", "source(rt)");

        Check(ParseAssetSourceKind("RT") == AssetSourceKind::RenderTarget, "ParseAssetSourceKind(RT)");
        Check(ParseAssetSourceKind("nope") == AssetSourceKind::Unknown, "ParseAssetSourceKind(nope)");
    }

    // ── 完整三段形式 ───────────────────────────────────────────────────
    void TestFullForm()
    {
        const AssetURI uri = AssetURI::Parse(OS_TEXT("geo:builtin:cube"));

        Check(uri.type == AssetType::Geometry, "geo:builtin:cube -> type");
        Check(uri.source == AssetSourceKind::Builtin, "geo:builtin:cube -> source");
        CheckText(uri.target, OS_TEXT("cube"), "geo:builtin:cube -> target");
        Check(uri.type_name == "geo", "geo:builtin:cube -> type_name");
        Check(uri.source_name == "builtin", "geo:builtin:cube -> source_name");
        Check(uri.IsValid(), "geo:builtin:cube -> IsValid");
        Check(uri.IsBuiltin(), "geo:builtin:cube -> IsBuiltin");
        CheckText(uri.ToString(), OS_TEXT("geo:builtin:cube"), "geo:builtin:cube -> ToString 往返");

        const AssetURI scene = AssetURI::Parse(OS_TEXT("scene:file:res/scene/demo.scene.toml"));

        Check(scene.type == AssetType::Scene, "scene:file:... -> type");
        Check(scene.IsFile(), "scene:file:... -> IsFile");
        CheckText(scene.target, OS_TEXT("res/scene/demo.scene.toml"), "scene:file:... -> target");
    }

    // ── 省略来源（含历史写法 geo:@<name>） ───────────────────────────────
    void TestOmitSource()
    {
        const AssetURI at_form = AssetURI::Parse(OS_TEXT("geo:@cube"));

        Check(at_form.type == AssetType::Geometry, "geo:@cube -> type");
        Check(at_form.source == AssetSourceKind::Unknown, "geo:@cube -> source 应为 Unknown");
        CheckText(at_form.target, OS_TEXT("@cube"), "geo:@cube -> target 保留 @");

        const AssetURI bare = AssetURI::Parse(OS_TEXT("geo:cube"));

        Check(bare.type == AssetType::Geometry, "geo:cube -> type");
        Check(bare.source == AssetSourceKind::Unknown, "geo:cube -> source");
        CheckText(bare.target, OS_TEXT("cube"), "geo:cube -> target");

        const AssetURI rcp = AssetURI::Parse(OS_TEXT("rcp:@Lit"));

        Check(rcp.type == AssetType::MaterialRecipe, "rcp:@Lit -> type");
        CheckText(rcp.target, OS_TEXT("@Lit"), "rcp:@Lit -> target");

        const AssetURI empty = AssetURI::Parse(OS_TEXT("geo:"));

        Check(empty.type == AssetType::Geometry, "geo: -> type");
        Check(!empty.IsValid(), "geo: -> 空 target 应 IsValid()==false");
    }

    // ── 省略类型：老纹理 URI 文本必须原样可读 ────────────────────────────
    void TestOmitType()
    {
        const AssetURI file_uri = AssetURI::Parse(OS_TEXT("file:res/image/brick.tex"), AssetType::Texture);

        Check(file_uri.type == AssetType::Texture, "file:... -> default_type 补齐");
        Check(file_uri.source == AssetSourceKind::File, "file:... -> source=File");
        CheckText(file_uri.target, OS_TEXT("res/image/brick.tex"), "file:... -> target");

        const AssetURI rt_uri = AssetURI::Parse(OS_TEXT("rt:Offscreen:color0"), AssetType::Texture);

        Check(rt_uri.type == AssetType::Texture, "rt:... -> default_type 补齐");
        Check(rt_uri.source == AssetSourceKind::RenderTarget, "rt:... -> source=RenderTarget");
        CheckText(rt_uri.target, OS_TEXT("Offscreen:color0"), "rt:... -> target 保留 RT 名里的冒号");

        const AssetURI rt_depth = AssetURI::Parse(OS_TEXT("rt:Offscreen:depth"), AssetType::Texture);

        Check(rt_depth.source == AssetSourceKind::RenderTarget, "rt:...:depth -> source");
        CheckText(rt_depth.target, OS_TEXT("Offscreen:depth"), "rt:...:depth -> target");

        // 不传 default_type 时类型保持 Unknown，来源仍要认出来。
        const AssetURI no_default = AssetURI::Parse(OS_TEXT("file:a.tex"));

        Check(no_default.type == AssetType::Unknown, "file:a.tex 无 default_type -> type Unknown");
        Check(no_default.source == AssetSourceKind::File, "file:a.tex 无 default_type -> source 仍为 File");
        Check(no_default.type_name.IsEmpty(), "file:a.tex -> type_name 应为空");
    }

    // ── 显式类型 + rt/盘符里带冒号：只解析前两个冒号 ─────────────────────
    void TestColonHandling()
    {
        const AssetURI rt_typed = AssetURI::Parse(OS_TEXT("tex:rt:Offscreen:color0"));

        Check(rt_typed.type == AssetType::Texture, "tex:rt:... -> type");
        Check(rt_typed.source == AssetSourceKind::RenderTarget, "tex:rt:... -> source");
        CheckText(rt_typed.target, OS_TEXT("Offscreen:color0"), "tex:rt:... -> target");

        const AssetURI drive = AssetURI::Parse(OS_TEXT("geo:file:C:\\data\\x.geo.toml"));

        Check(drive.type == AssetType::Geometry, "geo:file:C:\\... -> type");
        Check(drive.source == AssetSourceKind::File, "geo:file:C:\\... -> source");
        CheckText(drive.target, OS_TEXT("C:\\data\\x.geo.toml"), "geo:file:C:\\... -> target 保留盘符冒号");

        // 裸盘符路径（没有类型段）：不能被误判成 type="C"。
        const AssetURI bare_drive = AssetURI::Parse(OS_TEXT("C:\\data\\x.geo.toml"), AssetType::Geometry);

        Check(bare_drive.type == AssetType::Geometry, "裸盘符路径 -> type 来自 default_type");
        Check(bare_drive.source == AssetSourceKind::Unknown, "裸盘符路径 -> source Unknown");
        Check(bare_drive.type_name == "geo", "裸盘符路径 -> type_name 不该被盘符污染");
        CheckText(bare_drive.target, OS_TEXT("C:\\data\\x.geo.toml"), "裸盘符路径 -> target 整串保留");
    }

    // ── 未注册类型：能解析出来，但 type 保持 Unknown，供工厂降级 ──────────
    void TestUnregisteredType()
    {
        const AssetURI foo = AssetURI::Parse(OS_TEXT("foo:file:bar.dat"));

        Check(foo.type == AssetType::Unknown, "foo:file:bar.dat -> type 保持 Unknown");
        Check(foo.type_name == "foo", "foo:file:bar.dat -> type_name 留原拼写");
        Check(foo.source == AssetSourceKind::File, "foo:file:bar.dat -> source 仍能识别");
        CheckText(foo.target, OS_TEXT("bar.dat"), "foo:file:bar.dat -> target");
        CheckText(foo.ToString(), OS_TEXT("foo:file:bar.dat"), "foo:file:bar.dat -> ToString 往返");
        CheckText(foo.raw, OS_TEXT("foo:file:bar.dat"), "foo:file:bar.dat -> raw 保留原始输入");

        const AssetURI opaque = AssetURI::Parse(OS_TEXT("widget:alpha:beta"));

        Check(opaque.type == AssetType::Unknown, "widget:alpha:beta -> type Unknown");
        Check(opaque.type_name == "widget", "widget:alpha:beta -> type_name");
        Check(opaque.source == AssetSourceKind::Unknown, "widget:alpha:beta -> source Unknown");
        CheckText(opaque.target, OS_TEXT("alpha:beta"), "widget:alpha:beta -> target");
        CheckText(opaque.ToString(), OS_TEXT("widget:alpha:beta"), "widget:alpha:beta -> ToString 往返");

        // 未注册类型即使调用方给了 default_type，也不该被强行改名成已注册类型。
        const AssetURI forced = AssetURI::Parse(OS_TEXT("foo:file:bar.dat"), AssetType::Geometry);

        Check(forced.type == AssetType::Geometry, "foo:... + default_type -> 沿用调用方语义");
        Check(forced.type_name == "foo", "foo:... + default_type -> type_name 仍留原拼写");
        CheckText(forced.ToString(), OS_TEXT("foo:file:bar.dat"), "foo:... + default_type -> 往返不变");
    }

    // ── 裸定位串 / 空串 ─────────────────────────────────────────────────
    void TestBare()
    {
        const AssetURI bare = AssetURI::Parse(OS_TEXT("cube"), AssetType::Geometry);

        Check(bare.type == AssetType::Geometry, "cube -> type");
        Check(bare.source == AssetSourceKind::Unknown, "cube -> source Unknown");
        CheckText(bare.target, OS_TEXT("cube"), "cube -> target");

        const AssetURI empty = AssetURI::Parse(OS_TEXT(""));

        Check(empty.type == AssetType::Unknown, "空串 -> type Unknown");
        Check(!empty.IsValid(), "空串 -> !IsValid");

        const AssetURI nothing = AssetURI::Parse(OS_TEXT(""), AssetType::Texture);

        Check(nothing.type == AssetType::Texture, "空串 + default_type -> type");
        Check(!nothing.IsValid(), "空串 + default_type -> 仍 !IsValid");
    }

    // ── 拼装与规范化 ────────────────────────────────────────────────────
    void TestMakeAndNormalize()
    {
        CheckText(AssetURI::MakeAssetURI(AssetType::Geometry, AssetSourceKind::Builtin, OS_TEXT("cube")),
                  OS_TEXT("geo:builtin:cube"), "MakeAssetURI 全段");
        CheckText(AssetURI::MakeAssetURI(AssetType::Geometry, AssetSourceKind::Unknown, OS_TEXT("@cube")),
                  OS_TEXT("geo:@cube"), "MakeAssetURI 省略来源");
        CheckText(AssetURI::MakeAssetURI(AssetType::Unknown, AssetSourceKind::File, OS_TEXT("a.tex")),
                  OS_TEXT("file:a.tex"), "MakeAssetURI 省略类型");
        CheckText(AssetURI::MakeAssetURI(AssetType::Unknown, AssetSourceKind::Unknown, OS_TEXT("cube")),
                  OS_TEXT("cube"), "MakeAssetURI 裸定位串");

        CheckText(AssetURI::Normalize(OS_TEXT("geo:builtin:cube")),
                  OS_TEXT("geo:builtin:cube"), "Normalize 完整形式不变");
        CheckText(AssetURI::Normalize(OS_TEXT("geo:@cube")),
                  OS_TEXT("geo:@cube"), "Normalize 历史写法不变");
        CheckText(AssetURI::Normalize(OS_TEXT("file:res/x.tex"), AssetType::Texture),
                  OS_TEXT("tex:file:res/x.tex"), "Normalize 补上类型段");
        CheckText(AssetURI::Normalize(OS_TEXT("rt:Offscreen:color0"), AssetType::Texture),
                  OS_TEXT("tex:rt:Offscreen:color0"), "Normalize RT 补上类型段");
    }

    // ── 段转换辅助 ──────────────────────────────────────────────────────
    void TestSegmentToAnsi()
    {
        Check(AssetURISegmentToAnsi(OS_TEXT("builtin")) == "builtin", "AssetURISegmentToAnsi(builtin)");
        Check(AssetURISegmentToAnsi(OS_TEXT("")) .IsEmpty(), "AssetURISegmentToAnsi(\"\") 为空");
    }

    // ── 无扩展名逻辑路径与 ID 寻址 ──────────────────────────────────────
    void TestIDTarget()
    {
        // 逻辑路径：包裹定位段不带扩展名，URI 层不做任何改写
        const AssetURI logical = AssetURI::Parse(OS_TEXT("geo:pack(res):models/hero"));

        Check(logical.IsPack(), "逻辑路径 URI 仍是 pack");
        CheckText(logical.target, OS_TEXT("models/hero"), "逻辑路径原样保留");
        Check(!logical.IsIDTarget(), "普通逻辑路径不是 ID 寻址");

        // 带扩展名的历史写法：URI 层也不改写（推导发生在包裹分支）
        const AssetURI physical = AssetURI::Parse(OS_TEXT("geo:pack(res):models/hero.geo.toml"));

        CheckText(physical.target, OS_TEXT("models/hero.geo.toml"), "物理路径原样保留");
        Check(!physical.IsIDTarget(), "物理路径不是 ID 寻址");

        // ID 寻址
        const AssetID id = AssetID::Create();

        const AnsiString hex = id.ToString();
        U8String id_target(u8"id:");

        id_target += U8String(reinterpret_cast<const u8char *>(hex.c_str()), hex.Length());

        const OSString uri_text = OSString(OS_TEXT("geo:pack(res):")) + ToOSString(id_target);

        const AssetURI by_id = AssetURI::Parse(uri_text);

        Check(by_id.IsPack(), "ID 寻址 URI 仍是 pack");
        Check(by_id.IsIDTarget(), "IsIDTarget 命中");

        AssetID parsed;

        Check(by_id.GetIDTarget(parsed), "GetIDTarget 命中");
        Check(parsed == id, "GetIDTarget 值一致");

        // `id:` 后面不是定长 32 位十六进制 ⇒ 必须落回普通路径语义
        const AssetURI not_id = AssetURI::Parse(OS_TEXT("geo:pack(res):id:short"));

        Check(!not_id.IsIDTarget(), "非法 ID 段不得被判定为 ID 寻址");

        AssetID dummy;

        Check(!not_id.GetIDTarget(dummy), "非法 ID 段 GetIDTarget 失败");
        Check(dummy.IsZero(), "失败时输出置零");

        // 末尾空定位段：IsValid 为假
        const AssetURI empty = AssetURI::Parse(OS_TEXT("geo:pack(res):"));

        Check(!empty.IsValid(), "空定位段 IsValid 为假");
        Check(!empty.IsIDTarget(), "空定位段不是 ID 寻址");
    }
}

int main(int, char **)
{
    hgl::logger::InitLogger(OS_TEXT("TestAssetURI"));

    TestSpellingTables();
    TestFullForm();
    TestOmitSource();
    TestOmitType();
    TestColonHandling();
    TestUnregisteredType();
    TestBare();
    TestMakeAndNormalize();
    TestSegmentToAnsi();
    TestIDTarget();

    if (g_fail != 0)
    {
        GLogError(u8"=== TestAssetURI: %d 通过 / %d 失败 ===", g_pass, g_fail);
        return 1;
    }

    GLogInfo(u8"=== TestAssetURI: %d 项全部通过 ===", g_pass);
    return 0;
}