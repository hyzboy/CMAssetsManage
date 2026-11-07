// TestPhysicalVolumeRouter.cpp —— 物理卷路由器的契约判据。
//
// 锁定五件事：
//   1. 归一化：分隔符统一、重复分隔符折叠、尾部必补隔离符；
//   2. 编号分配：0 永远保留给默认通道，自动分配从 1 开始；
//   3. 重复登记同一个挂载根必须原地更新、编号不变（热插拔后编号不能漂移，
//      否则在途请求的路由结果会变）；
//   4. 最长前缀匹配 + 尾部隔离符带来的路径分量边界（"D:\Games" 不得匹配 "D:\Games2\x"）；
//   5. 注销/清空后的编号语义。
//
// 本用例不枚举真实卷（Enumerate 依赖机器），只用手工 AddVolume 构造拓扑，
// 因此无显卡机器与 CI 也能稳定跑。
#include <hgl/asset/PhysicalVolumeRouter.h>

#include <hgl/log/Log.h>
#include <hgl/log/Logger.h>

using namespace hgl;
using namespace hgl::asset;

namespace
{
    int g_fail = 0;
    int g_pass = 0;

    void Check(bool cond, const char *what)
    {
        if (cond)
        {
            ++g_pass;
            return;
        }

        ++g_fail;
        GLogError(u8"失败：%s", what);
    }

    // 用平台分隔符拼一个绝对路径，避免在 Windows 上写死反斜杠导致非 Windows 端失真。
    OSString P(const char *ascii)
    {
        OSString out;

        const os_char sep = HGL_DIRECTORY_SEPARATOR;

        for (const char *p = ascii; *p; ++p)
        {
            if (*p == '/')
                out += OSString(&sep, 1);
            else
            {
                const os_char ch = static_cast<os_char>(*p);
                out += OSString(&ch, 1);
            }
        }

        return out;
    }

    // ── 归一化 ─────────────────────────────────────────────────────────
    void TestNormalize()
    {
        const OSString bad = P("//a///b//");

        const OSString root = PhysicalVolumeRouter::NormalizeRoot(bad);

        Check(!root.IsEmpty(), "NormalizeRoot 非空");
        Check(root.c_str()[root.Length() - 1] == HGL_DIRECTORY_SEPARATOR,
              "NormalizeRoot 补齐尾部隔离符");

        // 反斜杠在 Windows 上必须被改写成标准分隔符
#if HGL_OS == HGL_OS_Windows
        const OSString mixed = OS_TEXT("D:/Games\\Data");
        const OSString fixed = PhysicalVolumeRouter::NormalizeRoot(mixed);

        Check(fixed.c_str()[fixed.Length() - 1] == HGL_DIRECTORY_SEPARATOR,
              "混合分隔符补齐尾部隔离符");
        Check(fixed.FindString(OS_TEXT("/")) < 0 ||
                  OS_TEXT('/') == HGL_DIRECTORY_SEPARATOR,
              "混合分隔符被统一");
#endif

        Check(PhysicalVolumeRouter::NormalizeRoot(OSString()).IsEmpty(),
              "空路径归一化仍为空");
    }

    // ── 编号分配与重复登记 ─────────────────────────────────────────────
    void TestAddRemove()
    {
        PhysicalVolumeRouter router;

        Check(router.GetVolumeCount() == 0, "初始无登记");

        Check(router.AddVolume(P("D:/Games"), AssetDeviceType::Fixed), "登记 D:/Games");
        Check(router.AddVolume(P("E:/Data"), AssetDeviceType::Fixed), "登记 E:/Data");

        Check(router.GetVolumeCount() == 2, "登记数为 2");

        const PhysicalVolumeRouter::VolumeRecord *first = router.GetVolumeAt(0);

        Check(first != nullptr, "第一条记录存在");

        if (first)
        {
            Check(first->device_id != AssetDeviceDefault, "自动编号不得等于默认编号 0");
            Check(first->type == AssetDeviceType::Fixed, "类型被记录");
            Check(router.GetVolume(first->device_id) == first, "按编号可取回同一条记录");
        }

        const PhysicalVolumeRouter::VolumeRecord *second = router.GetVolumeAt(1);

        Check(second != nullptr && first != nullptr && second->device_id != first->device_id,
              "两条记录编号不同");

        // 同一挂载根重复登记：原地更新，编号不变
        const AssetDeviceID kept = first ? first->device_id : AssetDeviceDefault;

        Check(router.AddVolume(P("D:/Games/"), AssetDeviceType::Fixed), "重复登记同一根成功");
        Check(router.GetVolumeCount() == 2, "重复登记不新增记录");
        Check(first && first->device_id == kept, "重复登记后编号不变");

        // 显式编号冲突必须失败
        Check(!router.AddVolume(P("F:/Other"), AssetDeviceType::Fixed, kept),
              "显式编号与既有挂载根冲突时失败");

        // 显式编号不冲突时成功，且后续自动分配跳过它
        Check(router.AddVolume(P("G:/Explicit"), AssetDeviceType::Fixed, 100),
              "显式编号登记成功");
        Check(router.GetVolumeCount() == 3, "显式登记后记录数为 3");

        const PhysicalVolumeRouter::VolumeRecord *explicit_record = router.GetVolume(100);

        Check(explicit_record != nullptr, "按显式编号可取回记录");

        Check(router.AddVolume(P("H:/Next"), AssetDeviceType::Fixed), "显式登记后再自动登记");
        Check(router.GetVolumeCount() == 4, "自动登记后记录数为 4");

        const PhysicalVolumeRouter::VolumeRecord *next = router.GetVolumeAt(3);

        Check(next != nullptr && next->device_id > 100, "自动分配跳过已被占用的编号");

        // 注销
        Check(router.RemoveVolume(100), "注销显式编号");
        Check(!router.RemoveVolume(100), "重复注销返回 false");
        Check(router.GetVolumeCount() == 3, "注销后记录数为 3");
        Check(router.GetVolume(100) == nullptr, "注销后按编号查不到");

        // 清空重置编号分配
        router.Clear();

        Check(router.GetVolumeCount() == 0, "Clear 后为空");
        Check(router.AddVolume(P("D:/Games"), AssetDeviceType::Fixed), "Clear 后可重新登记");

        const PhysicalVolumeRouter::VolumeRecord *again = router.GetVolumeAt(0);

        Check(again != nullptr && again->device_id == 1, "Clear 后编号分配被重置为 1");
    }

    // ── 最长前缀匹配 ───────────────────────────────────────────────────
    void TestResolve()
    {
        PhysicalVolumeRouter router;

        Check(router.Resolve(P("D:/Games/x.bin")) == AssetDeviceDefault,
              "无登记时一律走默认通道");

        Check(router.AddVolume(P("D:/Games"), AssetDeviceType::Fixed), "登记 D:/Games");
        Check(router.AddVolume(P("D:/Games/Data"), AssetDeviceType::Fixed), "登记 D:/Games/Data");
        Check(router.AddVolume(P("E:/Other"), AssetDeviceType::Fixed), "登记 E:/Other");

        const PhysicalVolumeRouter::VolumeRecord *outer = router.GetVolumeAt(0);
        const PhysicalVolumeRouter::VolumeRecord *inner = router.GetVolumeAt(1);
        const PhysicalVolumeRouter::VolumeRecord *other = router.GetVolumeAt(2);

        Check(outer && inner && other, "三条记录就位");

        if (!outer || !inner || !other)
            return;

        Check(router.Resolve(P("D:/Games/x.bin")) == outer->device_id, "普通路径命中外层");
        Check(router.Resolve(P("D:/Games/Data/y.bin")) == inner->device_id, "最长前缀胜出");
        Check(router.Resolve(P("D:/Games/Data")) == inner->device_id, "挂载根本身命中");
        Check(router.Resolve(P("E:/Other/z.bin")) == other->device_id, "另一设备命中");
        Check(router.Resolve(P("F:/Nowhere/z.bin")) == AssetDeviceDefault, "未匹配走默认通道");

        // 尾部隔离符带来的分量边界：不得把 D:/Games2 当成 D:/Games 的子路径
        Check(router.Resolve(P("D:/Games2/z.bin")) == AssetDeviceDefault,
              "同前缀但不同路径分量不得命中");

        // 匹配顺序：按 match_root 长度降序
        const PhysicalVolumeRouter::VolumeRecord *order0 = router.GetMatchOrderAt(0);

        Check(order0 == inner, "匹配顺序首位是最长前缀");
        Check(router.GetMatchOrderAt(99) == nullptr, "越界返回空");

        // 大小写折叠（仅 Windows：文件系统不区分大小写）
#if HGL_OS == HGL_OS_Windows
        Check(router.Resolve(P("d:/games/data/Z.BIN")) == inner->device_id,
              "Windows 下大小写不敏感");
#endif
    }
}

int main(int, char **)
{
    hgl::logger::InitLogger(OS_TEXT("TestPhysicalVolumeRouter"));

    TestNormalize();
    TestAddRemove();
    TestResolve();

    if (g_fail != 0)
    {
        GLogError(u8"=== TestPhysicalVolumeRouter: %d 通过 / %d 失败 ===", g_pass, g_fail);
        return 1;
    }

    GLogInfo(u8"=== TestPhysicalVolumeRouter: %d 项全部通过 ===", g_pass);
    return 0;
}
