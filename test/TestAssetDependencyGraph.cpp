// TestAssetDependencyGraph.cpp —— 双向依赖图的契约判据。
//
// 锁定六件事：
//   1. 双向一致：写入正向边必然同步出反向边，删除亦然；绝不允许「A 说依赖 B，
//      但 B 的反向表里没有 A」这种单向残留；
//   2. 整体替换：RegisterDependencies 是替换而不是追加 —— 删掉一行依赖后
//      必须真的从图里消失；
//   3. 去重与去自环：重复项只留一条，自依赖被丢弃（自依赖会让拓扑排序必然失败）；
//   4. 传递闭包：能顺着边一路找到间接依赖/间接引用方，且不重复、不含起点；
//   5. 拓扑序：被依赖者排在依赖者之前，无环时必须排完；
//   6. 环检测：人为造环时拓扑排序返回 false 而不是静默给出错误顺序。
#include <hgl/asset/AssetDependencyGraph.h>
#include <hgl/asset/AssetID.h>

#include <hgl/log/Log.h>
#include <hgl/log/Logger.h>

using namespace hgl;
using namespace hgl::asset;

namespace
{
    int g_fail = 0;
    int g_pass = 0;

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

    bool Has(const ValueArray<AssetID> &list, const AssetID &id)
    {
        for (int i = 0; i < list.GetCount(); ++i)
            if (list[i] == id)
                return true;

        return false;
    }

    int PosOf(const ValueArray<AssetID> &list, const AssetID &id)
    {
        for (int i = 0; i < list.GetCount(); ++i)
            if (list[i] == id)
                return i;

        return -1;
    }

    // ── 基本读写 ───────────────────────────────────────────────────────
    void TestBasics()
    {
        AssetDependencyGraph graph;

        Check(graph.GetRegisteredCount() == 0, "新建图为空");
        Check(graph.GetNodeCount() == 0, "新建图无节点");

        const AssetID scene    = AssetID::Create();
        const AssetID material = AssetID::Create();
        const AssetID texture  = AssetID::Create();

        ValueArray<AssetID> mat_deps;

        mat_deps.Add(texture);

        Check(graph.RegisterDependencies(material, mat_deps), "注册 material -> texture");

        ValueArray<AssetID> scene_deps;

        scene_deps.Add(material);

        Check(graph.RegisterDependencies(scene, scene_deps), "注册 scene -> material");

        Check(graph.GetRegisteredCount() == 2, "注册数 = 2");
        Check(graph.GetNodeCount() == 3, "节点数 = 3（texture 只被引用，无正向边）");

        // 正向
        ValueArray<AssetID> deps;

        Check(graph.GetDependencies(scene, deps), "取 scene 的直接依赖");
        Check(deps.GetCount() == 1 && deps[0] == material, "scene 直接依赖 material");
        Check(graph.GetDependencyCount(scene) == 1, "scene 依赖计数为 1");

        // 反向
        ValueArray<AssetID> referrers;

        Check(graph.GetDependents(material, referrers), "取 material 的引用方");
        Check(referrers.GetCount() == 1 && referrers[0] == scene, "material 被 scene 引用");
        Check(graph.GetDependentCount(material) == 1, "material 引用计数为 1");
        Check(graph.HasDependents(material), "material 有引用方");

        Check(graph.HasDependents(texture), "texture 有引用方（经由 material）");
        Check(!graph.HasDependents(scene), "scene 没有被引用");

        // 叶子的正向边不存在
        Check(!graph.GetDependencies(texture, deps), "texture 没有正向依赖");
        Check(deps.GetCount() == 0, "失败时清空输出");

        // 不存在的节点
        const AssetID ghost = AssetID::Create();

        Check(!graph.GetDependents(ghost, referrers), "未知节点无引用方");
        Check(graph.GetDependentCount(ghost) == 0, "未知节点引用计数为 0");

        // 全零身份被拒绝
        Check(!graph.RegisterDependencies(AssetID(), mat_deps), "全零身份拒绝注册");
    }

    // ── 整体替换语义 ───────────────────────────────────────────────────
    void TestReplace()
    {
        AssetDependencyGraph graph;

        const AssetID a = AssetID::Create();
        const AssetID b = AssetID::Create();
        const AssetID c = AssetID::Create();

        ValueArray<AssetID> first;

        first.Add(b);
        first.Add(c);

        Check(graph.RegisterDependencies(a, first), "注册 a -> {b, c}");
        Check(graph.GetDependencyCount(a) == 2, "a 有 2 个依赖");
        Check(graph.HasDependents(b), "b 被引用");
        Check(graph.HasDependents(c), "c 被引用");

        // 换成只依赖 b：c 的反向表必须清空
        ValueArray<AssetID> second;

        second.Add(b);

        Check(graph.RegisterDependencies(a, second), "替换为 a -> {b}");
        Check(graph.GetDependencyCount(a) == 1, "a 只剩 1 个依赖");

        ValueArray<AssetID> out;

        Check(!graph.GetDependents(c, out), "c 的反向边必须被拆掉");
        Check(graph.HasDependents(b), "b 仍被引用");

        // 换成空依赖：两个反向边都要拆掉，且 a 不再算「已注册」
        Check(graph.RegisterDependencies(a, ValueArray<AssetID>()), "替换为空依赖");
        Check(graph.GetRegisteredCount() == 0, "空依赖后 a 不再是已注册节点");
        Check(!graph.HasDependents(b), "b 不再被引用");

        // 去重
        ValueArray<AssetID> dup;

        dup.Add(b);
        dup.Add(b);
        dup.Add(c);
        dup.Add(b);

        Check(graph.RegisterDependencies(a, dup), "注册含重复项的依赖");
        Check(graph.GetDependencyCount(a) == 2, "重复项被去重");

        // 自依赖被丢弃
        ValueArray<AssetID> self;

        self.Add(a);
        self.Add(b);

        Check(graph.RegisterDependencies(a, self), "注册含自依赖的列表");
        Check(graph.GetDependencyCount(a) == 1, "自依赖被丢弃");

        graph.GetDependencies(a, out);
        Check(!Has(out, a), "依赖列表不含自身");
    }

    // ── 注销 ───────────────────────────────────────────────────────────
    void TestUnregister()
    {
        AssetDependencyGraph graph;

        const AssetID scene    = AssetID::Create();
        const AssetID material = AssetID::Create();
        const AssetID texture  = AssetID::Create();

        ValueArray<AssetID> mat_deps;

        mat_deps.Add(texture);
        graph.RegisterDependencies(material, mat_deps);

        ValueArray<AssetID> scene_deps;

        scene_deps.Add(material);
        graph.RegisterDependencies(scene, scene_deps);

        // 删掉中间的 material：scene 的正向边与 texture 的反向边都要被清理
        Check(graph.UnregisterAsset(material), "注销 material");
        Check(graph.GetDependencyCount(scene) == 0, "scene 的正向边被清空");
        Check(!graph.HasDependents(texture), "texture 的反向边被清空");
        Check(!graph.HasDependents(material), "material 自身不再有引用方");
        Check(graph.GetRegisteredCount() == 0, "注销后没有已注册节点");

        Check(!graph.UnregisterAsset(material), "重复注销返回 false");
        Check(!graph.UnregisterAsset(AssetID()), "全零身份注销返回 false");

        graph.Clear();
        Check(graph.GetNodeCount() == 0, "Clear 后无节点");
    }

    // ── 传递闭包 ───────────────────────────────────────────────────────
    void TestClosure()
    {
        AssetDependencyGraph graph;

        // scene -> material -> {texture, shader} ; shader -> include
        const AssetID scene    = AssetID::Create();
        const AssetID material = AssetID::Create();
        const AssetID texture  = AssetID::Create();
        const AssetID shader   = AssetID::Create();
        const AssetID include  = AssetID::Create();

        ValueArray<AssetID> d;

        d.Add(texture);
        d.Add(shader);
        graph.RegisterDependencies(material, d);

        d.Clear();
        d.Add(material);
        graph.RegisterDependencies(scene, d);

        d.Clear();
        d.Add(include);
        graph.RegisterDependencies(shader, d);

        ValueArray<AssetID> deps;

        // scene → material → {texture, shader} → shader → include：闭包共 4 项
        Check(graph.CollectDependencies(scene, deps, true) == 4, "scene 的传递依赖 4 项");
        Check(Has(deps, material) && Has(deps, texture) && Has(deps, shader) && Has(deps, include),
              "含直接与间接依赖");
        Check(!Has(deps, scene), "闭包不含起点");

        // 直接依赖（非递归）
        ValueArray<AssetID> direct;

        Check(graph.CollectDependencies(scene, direct, false) == 1, "非递归只取 1 项");
        Check(direct.GetCount() == 1 && direct[0] == material, "非递归取到 material");

        // 深度限制
        ValueArray<AssetID> depth1;

        Check(graph.CollectDependencies(scene, depth1, true, 1) == 1, "深度 1 只取 material");
        Check(!Has(depth1, include), "深度 1 不该出现 include");

        ValueArray<AssetID> depth2;

        Check(graph.CollectDependencies(scene, depth2, true, 2) == 3, "深度 2 取到 3 项");
        Check(!Has(depth2, include), "深度 2 仍不该出现 include");

        // 反向闭包：谁用到了 include
        ValueArray<AssetID> users;

        Check(graph.CollectDependents(include, users, true) == 3, "include 的传递引用方 3 项");
        Check(Has(users, shader) && Has(users, material) && Has(users, scene), "反向含全链路");
        Check(!Has(users, include), "反向闭包不含起点");

        // 汇入两条路径时不得重复计数
        const AssetID shared = AssetID::Create();

        d.Clear();
        d.Add(shared);
        graph.RegisterDependencies(texture, d);
        graph.RegisterDependencies(shader, d);

        ValueArray<AssetID> both;

        Check(graph.CollectDependencies(scene, both, true) >= 4, "菱形依赖可收集");
        Check(Has(both, shared), "共享节点被收集到");

        int hits = 0;

        for (int i = 0; i < both.GetCount(); ++i)
            if (both[i] == shared)
                ++hits;

        Check(hits == 1, "共享节点只出现一次");
    }

    // ── 拓扑序 ─────────────────────────────────────────────────────────
    void TestTopological()
    {
        AssetDependencyGraph graph;

        const AssetID scene    = AssetID::Create();
        const AssetID material = AssetID::Create();
        const AssetID texture  = AssetID::Create();

        ValueArray<AssetID> d;

        d.Add(texture);
        graph.RegisterDependencies(material, d);

        d.Clear();
        d.Add(material);
        graph.RegisterDependencies(scene, d);

        ValueArray<AssetID> order;

        Check(graph.TopologicalOrder(order), "无环图可拓扑排序");
        Check(order.GetCount() == 3, "拓扑序含 3 项");

        const int ti = PosOf(order, texture);
        const int mi = PosOf(order, material);
        const int si = PosOf(order, scene);

        Check(ti >= 0 && mi >= 0 && si >= 0, "三项都在序里");
        Check(ti < mi, "被依赖者 texture 排在 material 之前");
        Check(mi < si, "被依赖者 material 排在 scene 之前");

        Check(graph.IsAcyclic(), "无环图 IsAcyclic 为真");

        // 空图也算无环
        AssetDependencyGraph empty;

        Check(empty.TopologicalOrder(order), "空图拓扑排序成功");
        Check(order.GetCount() == 0, "空图无项");
        Check(empty.IsAcyclic(), "空图无环");

        // ── 造环 ──────────────────────────────────────────────────────
        // a -> b -> c -> a
        const AssetID a = AssetID::Create();
        const AssetID b = AssetID::Create();
        const AssetID c = AssetID::Create();

        d.Clear();
        d.Add(b);
        graph.RegisterDependencies(a, d);

        d.Clear();
        d.Add(c);
        graph.RegisterDependencies(b, d);

        d.Clear();
        d.Add(a);
        graph.RegisterDependencies(c, d);

        ValueArray<AssetID> cyclic;

        Check(!graph.TopologicalOrder(cyclic), "有环时必须返回 false");
        Check(cyclic.GetCount() < graph.GetNodeCount(), "有环时排不完所有节点");
        Check(!graph.IsAcyclic(), "有环图 IsAcyclic 为假");

        // 环不会让闭包死循环
        ValueArray<AssetID> loop;

        Check(graph.CollectDependencies(a, loop, true) <= graph.GetNodeCount(), "闭包在环下仍终止");
    }
}

int main(int, char **)
{
    hgl::logger::InitLogger(OS_TEXT("TestAssetDependencyGraph"));

    TestBasics();
    TestReplace();
    TestUnregister();
    TestClosure();
    TestTopological();

    if (g_fail != 0)
    {
        GLogError(u8"=== TestAssetDependencyGraph: %d 通过 / %d 失败 ===", g_pass, g_fail);
        return 1;
    }

    GLogInfo(u8"=== TestAssetDependencyGraph: %d 项全部通过 ===", g_pass);
    return 0;
}
