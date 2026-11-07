// TestAssetIODispatcher.cpp —— 按物理设备分流调度器的契约判据。
//
// 锁定四件事：
//   1. 生命周期：Start 只能成功一次；Stop 幂等且之后 Submit 一律失败；
//   2. 通道拓扑：至少永远有一条默认通道，未匹配路径全部退化到默认通道而不是丢请求；
//   3. 分发正确性：成功读到内容会走 OnFileLoaded，打开失败会走 OnFileError，
//      两条路径都要带上原请求的 target_id；
//   4. 统计合计：各通道的计数被正确相加，峰值取最大值而不是求和。
//
// 用例只读本目录已有的文件（CMakeLists.txt）和必然不存在的路径，
// 不写任何文件，也不依赖真实卷枚举的结果。
#include <hgl/asset/AssetIODispatcher.h>

#include <hgl/io/AsyncFileLoader.h>
#include <hgl/log/Log.h>
#include <hgl/log/Logger.h>
#include <hgl/sync/Atomic.h>
#include <hgl/time/Time.h>

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

    // 记录回调结果。回调运行在 IO worker 线程上，因此全部状态都用原子量。
    class RecordingHandler : public io::AsyncFileLoadHandler
    {
    public:

        atom_uint loaded   {0};
        atom_uint failed   {0};
        atom_uint cancelled{0};
        atom_uint last_error{0};
        atom_uint last_target{0xFFFFFFFFu};
        atom_uint64 loaded_bytes{0};

    public:

        bool OnFileLoaded(uint, io::AsyncFileLoadRequest *request) override
        {
            last_target.store(request->target_id);
            loaded_bytes.fetch_add(static_cast<uint64>(request->GetDataSize()));
            loaded.fetch_add(1);
            return true;
        }

        void OnFileError(uint, io::AsyncFileLoadRequest *request, const io::AsyncFileLoadError error) override
        {
            last_target.store(request->target_id);
            last_error.store(static_cast<uint>(error));
            failed.fetch_add(1);
        }

        void OnFileCancelled(uint, io::AsyncFileLoadRequest *request) override
        {
            last_target.store(request->target_id);
            cancelled.fetch_add(1);
        }
    };

    // 回调在别的线程上，这里只做有超时的轮询，避免用例挂死。
    bool WaitFor(const atom_uint &counter, uint expect, double timeout = 10.0)
    {
        const PreciseTime begin = GetTimeSec();

        while (counter.load() < expect)
        {
            if (GetTimeSec() - begin > timeout)
                return false;

            SleepSecond(0.002);
        }

        return true;
    }

    OSString Sep()
    {
        const os_char ch = HGL_DIRECTORY_SEPARATOR;

        return OSString(&ch, 1);
    }

    OSString SelfDirFile(const OSString &name)
    {
        return OSString(OS_TEXT(".")) + Sep() + name;
    }

    // ── 生命周期 ───────────────────────────────────────────────────────
    void TestLifecycle()
    {
        AssetIODispatcher dispatcher;
        RecordingHandler handler;

        Check(!dispatcher.IsRunning(), "新建时未运行");
        Check(!dispatcher.Submit(nullptr), "未启动时提交失败");

        // 没有下游 handler 不允许启动
        Check(!dispatcher.Start(nullptr), "无下游 handler 启动失败");
        Check(!dispatcher.Start(&handler, 0), "worker 数为 0 启动失败");

        Check(dispatcher.Start(&handler, 1), "启动成功");
        Check(dispatcher.IsRunning(), "启动后处于运行态");
        Check(!dispatcher.Start(&handler, 1), "重复启动失败");

        Check(dispatcher.GetChannelCount() >= 1, "至少存在默认通道");
        Check(dispatcher.GetChannelWorkerCount(AssetDeviceDefault) == 1,
              "默认通道 worker 数为 1");

        // 越界索引返回默认编号，不崩溃
        Check(dispatcher.GetChannelDeviceAt(-1) == AssetDeviceDefault, "负索引返回默认编号");
        Check(dispatcher.GetChannelDeviceAt(9999) == AssetDeviceDefault, "越界索引返回默认编号");
        Check(dispatcher.GetChannelWorkerCount(9999) == 0, "不存在的设备 worker 数为 0");

        Check(dispatcher.Stop(thread::ShutdownMode::Drain), "停止成功");
        Check(!dispatcher.IsRunning(), "停止后不在运行态");
        Check(dispatcher.Stop(thread::ShutdownMode::CancelPending), "重复停止仍返回成功");
        Check(!dispatcher.Submit(nullptr), "停止后提交失败");
        Check(dispatcher.RefreshDevices() < 0, "停止后刷新设备失败");
    }

    // ── 分发与统计 ─────────────────────────────────────────────────────
    void TestDispatch()
    {
        AssetIODispatcher dispatcher;
        RecordingHandler handler;

        if (!dispatcher.Start(&handler, 1))
        {
            Check(false, "启动失败，后续用例无法进行");
            return;
        }

        // 1) 成功读取：本目录下确定存在的文件
        io::AsyncFileLoadRequest good(SelfDirFile(OS_TEXT("CMakeLists.txt")), 11);

        Check(dispatcher.Submit(&good, io::AsyncFilePriority::High), "提交成功读取请求");

        // 2) 打开失败：确定不存在的路径
        io::AsyncFileLoadRequest bad(SelfDirFile(OS_TEXT("__no_such_file_for_test__.bin")), 22);

        Check(dispatcher.Submit(&bad, io::AsyncFilePriority::Normal), "提交失败读取请求");

        Check(WaitFor(handler.loaded, 1), "成功回调到达");
        Check(WaitFor(handler.failed, 1), "失败回调到达");

        Check(handler.loaded.load() == 1, "成功回调恰好一次");
        Check(handler.failed.load() == 1, "失败回调恰好一次");
        Check(handler.cancelled.load() == 0, "没有取消回调");
        Check(handler.loaded_bytes.load() > 0, "成功读到非空内容");

        // target_id 必须原样回传（外部靠它把结果对回请求）
        Check(handler.last_target.load() == 22 || handler.last_target.load() == 11,
              "回调携带原请求的 target_id");

        const io::AsyncFileLoaderStatistics stats = dispatcher.GetStatistics();

        Check(stats.submitted == 2, "提交计数合计为 2");
        Check(stats.loaded == 1, "加载成功计数为 1");
        Check(stats.failed == 1, "加载失败计数为 1");
        Check(stats.cancelled == 0, "取消计数为 0");
        Check(stats.loaded_bytes > 0, "加载字节数合计非 0");
        Check(stats.pending == 0, "全部请求已处理完");

        // 失败请求的错误码必须是真实的打开失败
        Check(handler.last_error.load() == static_cast<uint>(io::AsyncFileLoadError::OpenFailed),
              "失败原因为 OpenFailed");

        Check(dispatcher.Stop(thread::ShutdownMode::Drain), "排空停止成功");
    }

    // ── 手工挂载设备 ───────────────────────────────────────────────────
    void TestManualDevice()
    {
        AssetIODispatcher dispatcher;
        RecordingHandler handler;

        if (!dispatcher.Start(&handler, 1))
        {
            Check(false, "启动失败，后续用例无法进行");
            return;
        }

        const int before = dispatcher.GetChannelCount();

        const OSString mount_root(OS_TEXT("__pack__"));

        Check(dispatcher.GetRouter().AddVolume(mount_root, AssetDeviceType::Mounted, 77),
              "手工挂载虚拟设备 77");

        const OSString probe = mount_root + Sep() + OSString(OS_TEXT("a.bin"));

        Check(dispatcher.ResolveDevice(probe) == 77, "挂载点内的路径路由到设备 77");
        Check(dispatcher.GetChannelWorkerCount(77) == 0, "未 Refresh 前没有该设备的通道");

        const int created = dispatcher.RefreshDevices();

        Check(created >= 1, "刷新后至少新建一条通道");
        Check(dispatcher.GetChannelCount() >= before + 1, "通道数增加");
        Check(dispatcher.GetChannelWorkerCount(77) == 1, "设备 77 的通道已建立");

        // 该设备下的请求会入队，最终以失败结束（文件不存在），但不得丢请求
        io::AsyncFileLoadRequest request(probe, 33);

        Check(dispatcher.Submit(&request), "设备 77 下的请求入队成功");
        Check(WaitFor(handler.failed, 1), "设备 77 的失败回调到达");
        Check(dispatcher.GetStatistics().submitted == 1, "统计只记一次提交");

        dispatcher.GetRouter().RemoveVolume(77);

        Check(dispatcher.GetChannelWorkerCount(77) == 1,
              "注销挂载点不会立刻拆掉通道（只增不减）");

        Check(dispatcher.Stop(thread::ShutdownMode::Drain), "停止成功");
    }

    // ── 在途字节预算 ───────────────────────────────────────────────────
    void TestInFlightBudget()
    {
        AssetIODispatcher dispatcher;
        RecordingHandler handler;

        if (!dispatcher.Start(&handler, 1))
        {
            Check(false, "启动失败，后续用例无法进行");
            return;
        }

        dispatcher.SetMaxInFlightBytes(10);

        // 单个请求就超预算：直接拒绝，不进队列
        io::AsyncFileLoadRequest huge(SelfDirFile(OS_TEXT("CMakeLists.txt")), 41);
        huge.expected_bytes = 100;

        Check(!dispatcher.Submit(&huge, io::AsyncFilePriority::Normal), "超预算请求被拒绝");
        Check(dispatcher.GetInFlightBytes() == 0, "拒绝不占用预算");
        Check(dispatcher.GetStatistics().submitted == 0, "拒绝不计入提交统计");

        // 预算内：两个 5 字节请求恰好占满
        io::AsyncFileLoadRequest a(SelfDirFile(OS_TEXT("CMakeLists.txt")), 42);
        io::AsyncFileLoadRequest b(SelfDirFile(OS_TEXT("CMakeLists.txt")), 43);
        a.expected_bytes = 5;
        b.expected_bytes = 5;

        Check(dispatcher.Submit(&a, io::AsyncFilePriority::Normal), "首个预算内请求被接受");
        Check(dispatcher.GetInFlightBytes() == 5, "预算占用 5");
        Check(dispatcher.Submit(&b, io::AsyncFilePriority::Normal), "恰好占满预算仍被接受");
        Check(dispatcher.GetInFlightBytes() == 10, "预算占满 10");

        io::AsyncFileLoadRequest c(SelfDirFile(OS_TEXT("CMakeLists.txt")), 44);
        c.expected_bytes = 1;
        Check(!dispatcher.Submit(&c, io::AsyncFilePriority::Normal), "占满后新请求被拒绝");

        // 等待两个请求回调完成，预算应全部归还
        Check(WaitFor(handler.loaded, 2), "两个成功回调到达");

        int guard = 0;
        while (dispatcher.GetInFlightBytes() != 0 && guard++ < 5000)
            SleepSecond(0.002);

        Check(dispatcher.GetInFlightBytes() == 0, "回调后预算全部归还");

        // expected_bytes=0 不受限流影响
        io::AsyncFileLoadRequest free(SelfDirFile(OS_TEXT("CMakeLists.txt")), 45);
        Check(dispatcher.Submit(&free, io::AsyncFilePriority::Normal), "0 预算请求不受限流");
        Check(WaitFor(handler.loaded, 3), "第三个回调到达");

        Check(dispatcher.Stop(thread::ShutdownMode::Drain), "停止成功");
    }
}

int main(int, char **)
{
    hgl::logger::InitLogger(OS_TEXT("TestAssetIODispatcher"));

    TestLifecycle();
    TestDispatch();
    TestManualDevice();
    TestInFlightBudget();

    if (g_fail != 0)
    {
        GLogError(u8"=== TestAssetIODispatcher: %d 通过 / %d 失败 ===", g_pass, g_fail);
        return 1;
    }

    GLogInfo(u8"=== TestAssetIODispatcher: %d 项全部通过 ===", g_pass);
    return 0;
}
