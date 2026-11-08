// TestAssetIndex.cpp —— 资产索引表的契约判据。
//
// 锁定四件事：
//   1. 逻辑路径推导：先剥格式后缀、再剥类型后缀；非资产文件必须判失败；
//      「名叫 geo 的目录」这类歧义要有确定的、可解释的行为；
//   2. 双主键一致性：任何增删都必须同时维护 ID 与 (类型, 逻辑路径) 两侧，
//      不允许出现查得到 ID 却查不到路径（反之亦然）的行；
//   3. 覆盖语义：同 ID 换路径、同路径换 ID 都必须拆掉陈旧映射，不留孤儿；
//   4. 键的拼写：`<类型段>:<逻辑路径>`，类型段来自 AssetTypeTable 的唯一拼写。
#include <hgl/asset/AssetID.h>
#include <hgl/asset/AssetIndex.h>

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

    AssetIndexEntry MakeEntry(const char *physical, AssetType declared, const char *content_hash = "")
    {
        AssetIndexEntry entry;

        Check(DeriveLogical(U8(physical), declared, entry.type, entry.logical_path),
              "DeriveLogical 必须成功", physical);

        entry.physical_path = U8(physical);
        entry.id            = AssetID::Create();
        entry.content_hash  = content_hash;

        return entry;
    }

    // ── 逻辑路径推导 ───────────────────────────────────────────────────
    void TestDerive()
    {
        AssetType type = AssetType::Unknown;
        U8String  logical;
        U8String  out;

        // 作者侧文本：类型后缀 + 格式后缀
        Check(DeriveLogical(U8("models/hero.geo.toml"), AssetType::Unknown, type, logical),
              "hero.geo.toml 推导成功");
        Check(type == AssetType::Geometry, "hero.geo.toml 类型 = Geometry");
        CheckU8(logical, "models/hero", "hero.geo.toml 逻辑路径");

        // 发行侧二进制：后缀换成 fb 结果必须一致
        Check(DeriveLogical(U8("models/hero.geo.fb"), AssetType::Unknown, type, logical),
              "hero.geo.fb 推导成功");
        Check(type == AssetType::Geometry, "hero.geo.fb 类型 = Geometry");
        CheckU8(logical, "models/hero", "hero.geo.fb 逻辑路径");

        // 只有类型后缀（纹理常见）
        Check(DeriveLogical(U8("tex/brick.tex"), AssetType::Unknown, type, logical),
              "brick.tex 推导成功");
        Check(type == AssetType::Texture, "brick.tex 类型 = Texture");
        CheckU8(logical, "tex/brick", "brick.tex 逻辑路径");

        // 已经是逻辑路径：靠 declared_type 补类型
        Check(DeriveLogical(U8("models/hero"), AssetType::Geometry, type, logical),
              "裸逻辑路径 + declared 推导成功");
        Check(type == AssetType::Geometry, "裸逻辑路径类型来自 declared");
        CheckU8(logical, "models/hero", "裸逻辑路径不变");

        // 反斜杠必须归一化成 '/'
        Check(DeriveLogical(U8("models\\hero.geo.toml"), AssetType::Unknown, type, logical),
              "反斜杠路径推导成功");
        CheckU8(logical, "models/hero", "反斜杠被归一化");

        // 名字里本身带点：只剥两层，剩下的点属于名字
        Check(DeriveLogical(U8("models/hero.v2.geo.toml"), AssetType::Unknown, type, logical),
              "hero.v2.geo.toml 推导成功");
        CheckU8(logical, "models/hero.v2", "名字里的点被保留");

        // 非资产：格式后缀剥掉但没有类型可认
        Check(!DeriveLogical(U8("data/blob.bin"), AssetType::Unknown, type, logical),
              "blob.bin 不是资产");
        Check(type == AssetType::Unknown, "失败时类型为 Unknown");
        Check(logical.IsEmpty(), "失败时逻辑路径清空");

        Check(!DeriveLogical(U8("readme"), AssetType::Unknown, type, logical),
              "无后缀无类型声明不是资产");
        Check(!DeriveLogical(U8(""), AssetType::Unknown, type, logical),
              "空路径不是资产");

        // 目录名里的点不是后缀分隔符
        Check(!DeriveLogical(U8("a.b/hero"), AssetType::Unknown, type, logical),
              "目录名里的点不当作后缀");

        // 隐藏文件名：开头的点不是后缀分隔符
        Check(!DeriveLogical(U8(".asset_manifest.toml"), AssetType::Unknown, type, logical),
              "隐藏文件名不当作资产");

        // declared_type 覆盖后缀推断
        Check(DeriveLogical(U8("models/hero.tex"), AssetType::Geometry, type, logical),
              "declared 优先于后缀");
        Check(type == AssetType::Geometry, "declared 类型胜出");

        // 磁盘历史拼写：扫盘路径认长别名（引擎既有产物命名）
        Check(DeriveLogical(U8("ABeautifulGame.StaticMesh/Board.recipe.toml"), AssetType::Unknown, type, logical),
              "recipe.toml 经历史别名进索引");
        Check(type == AssetType::MaterialRecipe, "recipe.toml 类型 = MaterialRecipe");
        CheckU8(logical, "ABeautifulGame.StaticMesh/Board", "别名拼写被剥掉");

        // 配方命名约定：源码 .recipe.toml 与烘焙产物 .recipe.fb 必须推导同一身份。
        // URI 只写逻辑路径（不带任何物理扩展名），寻址天然兼容两种产物。
        {
            AssetType toml_type = AssetType::Unknown;
            U8String  toml_logical;
            Check(DeriveLogical(U8("asset/recipe/Examples/Interaction/PlaneGrid3D.VertexLuminance.recipe.toml"),
                                AssetType::Unknown, toml_type, toml_logical),
                  "PlaneGrid3D 配方 .recipe.toml 推导成功");
            Check(toml_type == AssetType::MaterialRecipe, "配方 .recipe.toml 类型 = MaterialRecipe");

            AssetType fb_type = AssetType::Unknown;
            U8String  fb_logical;
            Check(DeriveLogical(U8("asset/recipe/Examples/Interaction/PlaneGrid3D.VertexLuminance.recipe.fb"),
                                AssetType::Unknown, fb_type, fb_logical),
                  "PlaneGrid3D 配方 .recipe.fb 推导成功");
            Check(fb_type == AssetType::MaterialRecipe, "配方 .recipe.fb 类型 = MaterialRecipe");
            Check(toml_logical == fb_logical, "同一配方的 toml/fb 推导结果必须一致");
            CheckU8(fb_logical, "asset/recipe/Examples/Interaction/PlaneGrid3D.VertexLuminance",
                    "配方逻辑路径不含物理扩展名");
        }

        Check(DeriveLogical(U8("models/hero.material_data.toml"), AssetType::Unknown, type, logical),
              "material_data.toml 经历史别名进索引");
        Check(type == AssetType::MaterialData, "material_data.toml 类型 = MaterialData");

        Check(DeriveLogical(U8("props/crate.primitive.toml"), AssetType::Unknown, type, logical),
              "primitive.toml 经历史别名进索引");
        Check(type == AssetType::Primitive, "primitive.toml 类型 = Primitive");

        Check(ParseAssetType(AnsiString("recipe")) == AssetType::Unknown,
              "URI 类型段不认别名拼写");

        // 格式后缀表
        Check(IsAssetFormatSuffix(U8("toml")), "toml 是格式后缀");
        Check(IsAssetFormatSuffix(U8("TOML")), "格式后缀大小写不敏感");
        Check(IsAssetFormatSuffix(U8("fb")), "fb 是格式后缀");
        Check(IsAssetFormatSuffix(U8("bin")), "bin 是格式后缀");
        Check(!IsAssetFormatSuffix(U8("geo")), "geo 不是格式后缀");
        Check(!IsAssetFormatSuffix(U8("")), "空串不是格式后缀");

        // 键的拼写
        CheckU8(MakeIndexPathKey(AssetType::Geometry, U8("models/hero")), "geo:models/hero",
                "MakeIndexPathKey 拼写");
        Check(MakeIndexPathKey(AssetType::Unknown, U8("x")).IsEmpty(), "Unknown 类型键为空");
        Check(MakeIndexPathKey(AssetType::Geometry, U8("")).IsEmpty(), "空路径键为空");
    }

    // ── 双主键一致性 ───────────────────────────────────────────────────
    void TestAddAndFind()
    {
        AssetIndexTable table;

        Check(table.IsEmpty(), "新建表为空");
        Check(table.GetCount() == 0, "新建表计数为 0");

        AssetIndexEntry hero = MakeEntry("models/hero.geo.toml", AssetType::Unknown);

        hero.size  = 4096;
        hero.mtime = 123456;

        Check(table.AddOrUpdate(hero), "插入 hero");
        Check(table.GetCount() == 1, "计数为 1");

        AssetIndexEntry out;

        Check(table.FindByID(hero.id, out), "按 ID 命中");
        Check(out.logical_path == hero.logical_path, "按 ID 取回的逻辑路径一致");
        Check(out.size == 4096 && out.mtime == 123456, "按 ID 取回 size/mtime 一致");

        Check(table.FindByPath(AssetType::Geometry, U8("models/hero"), out), "按路径命中");
        Check(out.id == hero.id, "按路径取回的 ID 一致");

        Check(table.ContainsID(hero.id), "ContainsID 命中");
        Check(table.ContainsPath(AssetType::Geometry, U8("models/hero")), "ContainsPath 命中");
        Check(!table.ContainsPath(AssetType::Texture, U8("models/hero")), "类型不同必须未命中");

        // 同一条逻辑路径的不同类型是两个独立资产
        AssetIndexEntry hero_tex = MakeEntry("models/hero.tex", AssetType::Unknown);

        Check(table.AddOrUpdate(hero_tex), "插入同名纹理");
        Check(table.GetCount() == 2, "同名不同类型是两项");

        Check(table.FindByPath(AssetType::Geometry, U8("models/hero"), out), "几何仍在");
        Check(out.id == hero.id, "几何 ID 未变");
        Check(table.FindByPath(AssetType::Texture, U8("models/hero"), out), "纹理命中");
        Check(out.id == hero_tex.id, "纹理 ID 正确");

        // 只按逻辑路径查（忽略类型）应同时拿到两个
        ManagedArray<AssetIndexEntry> found;

        Check(table.FindByLogicalPath(U8("models/hero"), found) == 2, "FindByLogicalPath 拿到两项");
        Check(found.GetCount() == 2, "快照计数为 2");

        // 全表快照
        ManagedArray<AssetIndexEntry> all;

        Check(table.GetAllEntries(all) == 2, "GetAllEntries 计数一致");
        Check(all.GetCount() == 2, "快照项数一致");

        table.Clear();
        Check(table.IsEmpty(), "Clear 后为空");
    }

    // ── 覆盖语义：不留孤儿映射 ─────────────────────────────────────────
    void TestOverwrite()
    {
        AssetIndexTable table;

        AssetIndexEntry entry = MakeEntry("models/hero.geo.toml", AssetType::Unknown);

        Check(table.AddOrUpdate(entry), "初次插入");

        // 情况一：同一 ID 换了路径（改名/挪目录）
        AssetIndexEntry moved = entry;

        Check(DeriveLogical(U8("chars/hero2.geo.toml"), AssetType::Unknown,
                            moved.type, moved.logical_path), "改名后推导");
        moved.physical_path = U8("chars/hero2.geo.toml");

        Check(table.AddOrUpdate(moved), "同 ID 换路径");
        Check(table.GetCount() == 1, "同 ID 换路径后仍只有 1 项");

        AssetIndexEntry out;

        Check(table.FindByID(entry.id, out), "按 ID 仍命中");
        CheckU8(out.logical_path, "chars/hero2", "按 ID 取到新路径");
        Check(!table.ContainsPath(AssetType::Geometry, U8("models/hero")), "旧路径必须失效");

        // 情况二：同一路径换了 ID（删除后重建）
        AssetIndexEntry rebuilt = moved;

        rebuilt.id = AssetID::Create();

        Check(table.AddOrUpdate(rebuilt), "同路径换 ID");
        Check(table.GetCount() == 1, "同路径换 ID 后仍只有 1 项");
        Check(!table.ContainsID(entry.id), "旧 ID 必须失效");
        Check(table.ContainsID(rebuilt.id), "新 ID 有效");
        Check(table.FindByPath(AssetType::Geometry, U8("chars/hero2"), out), "路径仍命中");
        Check(out.id == rebuilt.id, "路径指向新 ID");

        // 情况三：同一路径换了类型
        AssetIndexEntry retyped = rebuilt;

        Check(DeriveLogical(U8("chars/hero2.rcp.toml"), AssetType::Unknown,
                            retyped.type, retyped.logical_path), "换类型推导");
        retyped.physical_path = U8("chars/hero2.rcp.toml");

        Check(table.AddOrUpdate(retyped), "同路径换类型");
        Check(table.GetCount() == 1, "同路径换类型后仍只有 1 项");
        Check(!table.ContainsPath(AssetType::Geometry, U8("chars/hero2")), "旧类型路径失效");
        Check(table.ContainsPath(AssetType::MaterialRecipe, U8("chars/hero2")), "新类型路径有效");

        // 删除
        Check(table.RemoveByID(retyped.id), "按 ID 删除");
        Check(table.IsEmpty(), "删除后为空");

        Check(table.AddOrUpdate(retyped), "重新插入");
        Check(table.RemoveByPath(AssetType::MaterialRecipe, U8("chars/hero2")), "按路径删除");
        Check(table.IsEmpty(), "按路径删除后为空");
        Check(!table.ContainsID(retyped.id), "按路径删除后 ID 也失效");

        // 非法输入
        AssetIndexEntry bad;

        Check(!table.AddOrUpdate(bad), "全空项必须拒绝");
        bad.id = AssetID::Create();
        Check(!table.AddOrUpdate(bad), "空路径项必须拒绝");

        AssetIndexEntry no_id = MakeEntry("a/b.geo.toml", AssetType::Unknown);

        no_id.id.Clear();
        Check(!table.AddOrUpdate(no_id), "全零 ID 必须拒绝");

        // 删除不存在的项
        AssetID ghost = AssetID::Create();

        Check(!table.RemoveByID(ghost), "删除不存在的 ID 返回 false");
        Check(!table.RemoveByPath(AssetType::Environment, U8("nope")), "删除不存在的路径返回 false");
    }

    // ── 内容哈希与包级算法 ─────────────────────────────────────────────
    void TestContentHash()
    {
        AssetIndexTable table;

        Check(table.GetHashAlgorithm().IsEmpty(), "默认未声明算法");

        table.SetHashAlgorithm("BLAKE3");
        Check(table.GetHashAlgorithm() == "BLAKE3", "算法名可读写");

        AssetIndexEntry entry = MakeEntry("models/hero.geo.toml", AssetType::Unknown);

        Check(!entry.HasContentHash(), "缺省无内容哈希");

        entry.content_hash = "0123456789abcdef";
        Check(entry.HasContentHash(), "写入后 HasContentHash 为真");

        Check(table.AddOrUpdate(entry), "插入带哈希的项");

        AssetIndexEntry out;

        Check(table.FindByID(entry.id, out), "按 ID 命中");
        Check(out.content_hash == "0123456789abcdef", "内容哈希往返一致");

        table.Clear();
        Check(table.GetHashAlgorithm() == "BLAKE3", "Clear 不动包级算法声明");
    }

    // ── 批量规模 ───────────────────────────────────────────────────────
    void TestBulk()
    {
        AssetIndexTable table;

        constexpr int kCount = 2000;

        int add_failed = 0;

        for (int i = 0; i < kCount; ++i)
        {
            // 2000 个不同路径的几何资产
            char physical[64];

            physical[0] = 'm';
            physical[1] = '/';

            int n = i;
            int p = 2;

            if (n == 0)
                physical[p++] = '0';
            else
            {
                char tmp[16];
                int t = 0;

                while (n > 0)
                {
                    tmp[t++] = char('0' + n % 10);
                    n /= 10;
                }

                while (t > 0)
                    physical[p++] = tmp[--t];
            }

            static const char kTail[] = ".geo.toml";

            for (int k = 0; kTail[k]; ++k)
                physical[p++] = kTail[k];

            physical[p] = 0;

            AssetIndexEntry entry;

            if (!DeriveLogical(U8(physical), AssetType::Unknown, entry.type, entry.logical_path))
            {
                ++add_failed;
                continue;
            }

            entry.physical_path = U8(physical);
            entry.id            = AssetID::Create();

            if (!table.AddOrUpdate(entry))
                ++add_failed;
        }

        Check(add_failed == 0, "2000 项全部插入成功");
        Check(table.GetCount() == kCount, "2000 项计数一致");

        // 全表快照后逐项回查，双向都必须命中
        ManagedArray<AssetIndexEntry> all;

        Check(table.GetAllEntries(all) == kCount, "快照计数一致");

        int miss_id = 0;
        int miss_path = 0;

        for (int i = 0; i < all.GetCount(); ++i)
        {
            AssetIndexEntry out;

            if (!table.FindByID(all[i]->id, out))
                ++miss_id;

            if (!table.FindByPath(all[i]->type, all[i]->logical_path, out))
                ++miss_path;
        }

        Check(miss_id == 0, "批量按 ID 全命中");
        Check(miss_path == 0, "批量按路径全命中");
    }

    // ── 大小写折叠与跨平台一致性 ─────────────────────────────────────
    void TestCaseFolding()
    {
        AssetIndexTable table;

        AssetIndexEntry entry = MakeEntry("Models/Hero.geo.toml", AssetType::Unknown);
        Check(table.AddOrUpdate(entry), "插入大小写混合路径");

        AssetIndexEntry out;
        Check(table.FindByPath(AssetType::Geometry, U8("models/hero"), out), "全小写查找命中");
        Check(table.FindByPath(AssetType::Geometry, U8("MODELS/HERO"), out), "全大写查找命中");
        Check(table.FindByPath(AssetType::Geometry, U8("Models/Hero"), out), "原始大小写查找命中");
    }

    // ── 严格冲突检测与合法伴生 ───────────────────────────────────────
    void TestCompanionAndConflict()
    {
        AssetIndexTable table;

        AssetIndexEntry src = MakeEntry("models/hero.geo.toml", AssetType::Geometry);
        AnsiString conflict;
        Check(table.TryAddStrict(src, &conflict), "初次插入源文件成功");
        Check(conflict.IsEmpty(), "无冲突报错");

        // 合法伴生：同逻辑路径的 .fb
        AssetIndexEntry baked = MakeEntry("models/hero.geo.fb", AssetType::Geometry);
        baked.id = src.id; // 伴生通常共享或映射同一 ID
        Check(table.TryAddStrict(baked, &conflict), "插入伴生 .fb 允许放行");

        // 验证伴生状态已经记录
        AssetIndexEntry out_entry;
        Check(table.FindByID(baked.id, out_entry), "查找伴生条目");
        Check(out_entry.IsBaked(), "伴生烘焙标记正确");
        Check(out_entry.source_file == U8("models/hero.geo.toml"), "伴生源文件记录正确");

        // 冲突情况 1：同名另一个源文件（另一个物理路径）
        AssetIndexEntry conflict_src = MakeEntry("models/hero.geo.toml", AssetType::Geometry);
        conflict_src.physical_path = U8("models/alt/hero.geo.toml");
        Check(!table.TryAddStrict(conflict_src, &conflict), "同逻辑路径另一源文件必须拦截");
        Check(!conflict.IsEmpty(), "必须给出冲突原因");

        // 冲突情况 2：新表测试两个大小写冲突的源文件（例如在 Linux 区分大小写系统或不同分支导入）
        AssetIndexTable case_table;
        AssetIndexEntry case_src1 = MakeEntry("models/hero.geo.toml", AssetType::Geometry);
        Check(case_table.TryAddStrict(case_src1, &conflict), "插入首个源文件");

        AssetIndexEntry case_src2 = MakeEntry("models/hero.geo.toml", AssetType::Geometry);
        case_src2.physical_path = U8("Models/Hero.geo.toml"); // 拼写不同的源文件
        Check(!case_table.TryAddStrict(case_src2, &conflict), "大小写折叠后撞车的源文件必须拦截");
        Check(!conflict.IsEmpty(), "必须给出大小写冲突原因");
    }

    // ── 内存探索查询：按类型 / 目录 / 前缀 ───────────────────────────
    void TestExploreQueries()
    {
        AssetIndexTable table;

        // 固定语料：
        //   "hero"                      Geometry      （包根直接层级）
        //   "models/hero"               Geometry      \
        //   "models/hero"               MaterialRecipe } 同族伴生（大小写混拼验证折叠）
        //   "models/HERO"               MaterialData  /
        //   "models/weapons/sword"      Geometry      （二级子目录）
        //   "tex/brick"                 Texture      （兄弟目录）
        Check(table.AddOrUpdate(MakeEntry("hero.geo.toml",             AssetType::Geometry)),        "根层 hero");
        Check(table.AddOrUpdate(MakeEntry("models/hero.geo.toml",      AssetType::Geometry)),         "models/hero geo");
        Check(table.AddOrUpdate(MakeEntry("models/hero.rcp.toml",      AssetType::MaterialRecipe)),   "models/hero rcp");
        Check(table.AddOrUpdate(MakeEntry("models/HERO.mtldata.toml",  AssetType::MaterialData)),     "models/HERO mtldata");
        Check(table.AddOrUpdate(MakeEntry("models/weapons/sword.geo.toml", AssetType::Geometry)),     "models/weapons/sword");
        Check(table.AddOrUpdate(MakeEntry("tex/brick.tex",             AssetType::Unknown)),          "tex/brick");

        ManagedArray<AssetIndexEntry> out;

        // ── GetEntriesByType ──
        Check(table.GetEntriesByType(AssetType::Unknown, out) == 0, "Unknown 类型必须返回 0");
        Check(out.GetCount() == 0, "Unknown 不得写入输出");

        const int added = table.GetEntriesByType(AssetType::Geometry, out);
        Check(added == 3, "Geometry 共 3 条");
        Check(out.GetCount() == 3, "输出计数与返回值一致");

        // 追加模式：不清空既有内容
        const int added2 = table.GetEntriesByType(AssetType::MaterialRecipe, out);
        Check(added2 == 1 && out.GetCount() == 4, "追加模式不清空 out");

        out.Clear();
        Check(table.GetEntriesByType(AssetType::Environment, out) == 0, "缺失类型返回 0");

        // ── FindInDirectory ──
        Check(table.FindInDirectory(U8("models"), AssetType::Unknown, out) == 3,
              "models 非递归命中 3（含大小写折叠的 HERO 伴生）");
        out.Clear();

        Check(table.FindInDirectory(U8("models"), AssetType::Unknown, out, true) == 4,
              "models 递归命中 4（含 weapons/sword）");
        out.Clear();

        Check(table.FindInDirectory(U8("models"), AssetType::Geometry, out, true) == 2,
              "models 递归 + Geometry 过滤 = 2");
        out.Clear();

        // 根目录：空串与 "/" 等价；非递归只认无 '/' 的条目
        Check(table.FindInDirectory(U8(""), AssetType::Unknown, out) == 1,
              "空串 = 根，非递归只命中根层 hero");
        out.Clear();
        Check(table.FindInDirectory(U8("/"), AssetType::Unknown, out) == 1,
              "\"/\" 与空串同义");
        out.Clear();
        Check(table.FindInDirectory(U8(""), AssetType::Unknown, out, true) == table.GetCount(),
              "根 + 递归 = 全表");
        out.Clear();

        // 规范化：反斜杠、前后多余分隔符、大小写
        Check(table.FindInDirectory(U8("\\models\\"), AssetType::Unknown, out) == 3,
              "反斜杠与尾部分隔符被规范化");
        out.Clear();
        Check(table.FindInDirectory(U8("MODELS"), AssetType::Unknown, out) == 3,
              "目录名大小写不敏感");
        out.Clear();
        Check(table.FindInDirectory(U8("models//weapons"), AssetType::Unknown, out) == 1,
              "连续分隔符折叠");
        out.Clear();

        Check(table.FindInDirectory(U8("nope"), AssetType::Unknown, out) == 0,
              "不存在目录返回 0");

        // 条目归属校验：非递归不得混入子孙
        table.FindInDirectory(U8("models"), AssetType::Unknown, out, true);
        bool has_sword = false;
        for(int i = 0; i < out.GetCount(); ++i)
            if(out[i]->logical_path == U8("models/weapons/sword"))
                has_sword = true;
        Check(has_sword, "递归结果应含 models/weapons/sword");
        out.Clear();

        // ── FindByPathPrefix ──
        // 同族伴生：logical 等于前缀（大小写折叠）即命中
        Check(table.FindByPathPrefix(U8("models/hero"), AssetType::Unknown, out) == 3,
              "族前缀命中 geo+rcp+mtldata 三条");
        out.Clear();

        Check(table.FindByPathPrefix(U8("models/hero"), AssetType::MaterialData, out) == 1,
              "族前缀 + 类型过滤 = 1");
        out.Clear();

        // 前缀即目录：任意深度子孙命中，但不含前缀本身之外的兄弟
        Check(table.FindByPathPrefix(U8("models"), AssetType::Unknown, out) == 4,
              "目录前缀命中 models 子树 4 条");
        out.Clear();

        // 半截名字必须不命中
        Check(table.FindByPathPrefix(U8("models/her"), AssetType::Unknown, out) == 0,
              "models/her 不得命中 models/hero");

        // 空前缀 = 全表误用，拒绝
        Check(table.FindByPathPrefix(U8(""), AssetType::Unknown, out) == 0,
              "空前缀返回 0");
        Check(table.FindByPathPrefix(U8("/"), AssetType::Unknown, out) == 0,
              "纯分隔符前缀返回 0");

        // 大小写不敏感 + 反斜杠规范化
        Check(table.FindByPathPrefix(U8("MODELS\\Hero"), AssetType::Unknown, out) == 3,
              "前缀折叠大小写并规范分隔符");
        out.Clear();
    }
}

int main(int, char **)
{
    hgl::logger::InitLogger(OS_TEXT("TestAssetIndex"));

    TestDerive();
    TestAddAndFind();
    TestOverwrite();
    TestContentHash();
    TestBulk();
    TestCaseFolding();
    TestCompanionAndConflict();
    TestExploreQueries();

    if (g_fail != 0)
    {
        GLogError(u8"=== TestAssetIndex: %d 通过 / %d 失败 ===", g_pass, g_fail);
        return 1;
    }

    GLogInfo(u8"=== TestAssetIndex: %d 项全部通过 ===", g_pass);
    return 0;
}
