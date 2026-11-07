// TestLoadedFileManager.cpp —— 加载管理器 TaskReceiver 中继与字节源的契约判据。
//
// 锁定五件事：
//   1. 纯 receiver 请求：读盘线程在 Post 前中继 Done 事件，事件携带一个可直接接管的
//      文件引用(用完 Release 一次)；管理器不解析负载、不登记已加载表；
//   2. 缓存命中 + 迟到的 receiver 请求：Pump 段补发唯一终态事件，指向已登记的同一文件；
//   3. handler 与 receiver 混合同一路径：handler 走原回调，receiver 只收到一个事件；
//   4. 自定义字节源：不存在的路径也能成功，字节按源内容返回；未消费的源由管理器删除；
//   5. 失败路径：receiver 收到 Failed 且 task 为空，error_code 非零。
//
// 用例只读本目录已有的文件，不写任何文件。
#include <hgl/asset/LoadedFileManager.h>

#include <hgl/log/Log.h>
#include <hgl/log/Logger.h>
#include <hgl/sync/Atomic.h>
#include <hgl/time/Time.h>

#include <cstdio>

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

    OSString Sep()
    {
        const os_char ch = HGL_DIRECTORY_SEPARATOR;

        return OSString(&ch, 1);
    }

    OSString SelfDirFile(const OSString &name)
    {
        return OSString(OS_TEXT(".")) + Sep() + name;
    }

    // 回调在 IO 线程或 Pump 线程触发，这里只做有超时的轮询，避免用例挂死。
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

    // ── handler 记录器 ─────────────────────────────────────────────────
    class RecordingHandler : public LoadedFileHandler
    {
    public:

        atom_uint loaded  {0};
        atom_uint failed  {0};
        atom_uint cancelled{0};
        atom_uint64 loaded_bytes{0};

    public:

        void OnFileLoaded(const FileLoadTicket &, LoadedFile *file) override
        {
            loaded_bytes.fetch_add(file->GetSize());
            loaded.fetch_add(1);
        }

        void OnFileFailed(const FileLoadTicket &, io::AsyncFileLoadError) override
        {
            failed.fetch_add(1);
        }

        void OnFileCancelled(const FileLoadTicket &) override
        {
            cancelled.fetch_add(1);
        }
    };

    // ── TaskReceiver 记录器 ────────────────────────────────────────────
    // 事件可能在 IO 线程投递，全部状态用锁保护。Loaded 事件携带的引用被直接
    // 接管(不再 RefInc)，用例结束时统一 Release。
    class RecordingReceiver : public thread::TaskReceiver
    {
    public:

        atom_uint events{0};

        ~RecordingReceiver() override
        {
            ReleaseAdopted();
        }

        void OnTaskEvent(const thread::TaskEvent &event) override
        {
            MutexLock guard(&lock);

            if (event.task)
                adopted.Add(static_cast<LoadedFile *>(event.task));

            ++event_count;
            last_state = event.state;
            last_error = event.error_code;
            last_completed = event.completed;
            last_user_data = event.user_data;
            last_task_id = event.task_id;

            events.fetch_add(1);
        }

        // 取出最近一次事件的状态(要求已无并发投递)。
        bool TakeState(thread::TaskState &state, int32 &error_code, uint64 &completed, void *&user_data)
        {
            MutexLock guard(&lock);

            if (event_count == 0)
                return false;

            state = last_state;
            error_code = last_error;
            completed = last_completed;
            user_data = last_user_data;
            return true;
        }

        // 放手所有接管的引用：文件计数归零进入待释放队列，由 Collect 回收。
        void ReleaseAdopted()
        {
            MutexLock guard(&lock);

            for (int i = 0; i < adopted.GetCount(); i++)
                adopted[i]->RefDec();

            adopted.Clear();
        }

        int AdoptedCount()
        {
            MutexLock guard(&lock);
            return adopted.GetCount();
        }

        LoadedFile *LastAdopted()
        {
            MutexLock guard(&lock);

            return (adopted.GetCount() > 0) ? adopted[adopted.GetCount() - 1] : nullptr;
        }

        thread::TaskState LastState()
        {
            MutexLock guard(&lock);
            return last_state;
        }

    private:

        Mutex        lock;
        ValueArray<LoadedFile *> adopted;
        int          event_count = 0;
        thread::TaskState last_state = thread::TaskState::Pending;
        int32        last_error = 0;
        uint64       last_completed = 0;
        void        *last_user_data = nullptr;
        uint64       last_task_id = 0;
    };

    // ── 内存字节源：验证不存在的文件名也能走通 ─────────────────────────
    static atom_uint g_source_alive{0};

    class MemoryByteSource : public io::AsyncFileByteSource
    {
    public:

        static constexpr int64 kSize = 4;
        static const uint8 kBytes[kSize];

        OSString route;

    public:

        MemoryByteSource()
        {
            route = OS_TEXT("MemoryByteSource");
            g_source_alive.fetch_add(1);
        }
        ~MemoryByteSource() override { g_source_alive.fetch_sub(1); }

        const OSString &GetRoutePath() const override { return route; }

        int64 GetSize() override { return kSize; }

        int64 Read(uint8 *buffer, int64 length) override
        {
            const int64 n = (length < kSize) ? length : kSize;

            for (int64 i = 0; i < n; i++)
                buffer[i] = kBytes[i];

            return n;
        }
    };

    const uint8 MemoryByteSource::kBytes[kSize] = {0xDE, 0xAD, 0xBE, 0xEF};

    // ── 用例 1：纯 receiver 读盘 ───────────────────────────────────────
    void TestReceiverOnlyRead()
    {
        LoadedFileManager manager;
        RecordingReceiver receiver;

        Check(manager.Start(1), "启动管理器");

        const OSString name = SelfDirFile(OS_TEXT("CMakeLists.txt"));
        int marker = 11;

        FileLoadId id = manager.Submit(name, nullptr, io::AsyncFilePriority::Normal,
                                       &receiver, nullptr, &marker, 0);

        Check(id != 0, "纯 receiver 提交成功");
        Check(WaitFor(receiver.events, 1), "收到终态事件");

        thread::TaskState state = thread::TaskState::Pending;
        int32 error_code = 0;
        uint64 completed = 0;
        void *user_data = nullptr;

        Check(receiver.TakeState(state, error_code, completed, user_data), "事件状态可读");
        Check(state == thread::TaskState::Done, "Done 事件");
        Check(user_data == static_cast<void *>(&marker), "user_data 原样回投");
        Check(receiver.AdoptedCount() == 1, "携带一个文件引用");
        Check(completed > 0, "completed 报告字节数");

        // Pump 消费掉分组：relay_only 分组应跳过解析与发布
        manager.Pump();

        LoadedFileManagerStatistics st = manager.GetStatistics();
        Check(st.cache_hits == 0, "没走缓存");
        Check(st.alive_files == 0, "relay_only 不登记已加载表");
        Check(!manager.Find(name), "同一路径查不到文件");

        manager.Stop(thread::ShutdownMode::CancelPending);
        manager.Collect();
    }

    // ── 用例 2：handler 先加载，receiver 迟到走缓存命中 ────────────────
    void TestCacheHitRelay()
    {
        LoadedFileManager manager;
        RecordingHandler handler;
        RecordingReceiver receiver;

        Check(manager.Start(1), "启动管理器");

        const OSString name = SelfDirFile(OS_TEXT("CMakeLists.txt"));
        int marker = 22;

        FileLoadId a = manager.Submit(name, &handler);
        Check(a != 0, "handler 提交成功");

        // 等读盘回调完成并 Pump 派发；随后同一路径改为纯 receiver 提交
        int guard = 0;
        while (handler.loaded.load() < 1 && guard++ < 5000)
        {
            manager.Pump(8);
            SleepSecond(0.002);
        }

        Check(handler.loaded.load() == 1, "handler 收到 OnFileLoaded");
        manager.Pump();

        LoadedFilePtr cached = manager.Find(name);
        Check(cached.Get() != nullptr, "文件已登记在表");

        FileLoadId b = manager.Submit(name, nullptr, io::AsyncFilePriority::Normal,
                                      &receiver, nullptr, &marker, 0);
        Check(b != 0, "缓存命中提交成功");

        manager.Pump();
        manager.Pump();

        Check(receiver.events.load() == 1, "缓存命中补发唯一事件");
        Check(receiver.LastState() == thread::TaskState::Done, "Done 事件");
        Check(receiver.AdoptedCount() == 1, "事件携带文件引用");
        Check(receiver.LastAdopted() == cached.Get(), "引用指向同一已登记文件");

        LoadedFileManagerStatistics st = manager.GetStatistics();
        Check(st.cache_hits == 1, "统计记为缓存命中");

        manager.Stop(thread::ShutdownMode::CancelPending);
        handler.loaded.fetch_add(0);
        manager.Collect();
    }

    // ── 用例 3：handler + receiver 混合同一路径 ────────────────────────
    void TestMixedRelayAndHandler()
    {
        LoadedFileManager manager;
        RecordingHandler handler;
        RecordingReceiver receiver;

        Check(manager.Start(1), "启动管理器");

        const OSString name = SelfDirFile(OS_TEXT("CMakeLists.txt"));

        FileLoadId a = manager.Submit(name, &handler);
        FileLoadId b = manager.Submit(name, nullptr, io::AsyncFilePriority::Normal, &receiver);

        Check(a != 0 && b != 0, "混合提交成功");

        int guard = 0;
        while ((handler.loaded.load() < 1 || receiver.events.load() < 1) && guard++ < 5000)
        {
            manager.Pump(8);
            SleepSecond(0.002);
        }

        manager.Pump();

        Check(handler.loaded.load() == 1, "handler 收到一次回调");
        Check(receiver.events.load() == 1, "receiver 只收到一个事件");
        Check(receiver.LastState() == thread::TaskState::Done, "Done 事件");
        Check(receiver.LastAdopted() != nullptr, "事件携带文件引用");

        LoadedFilePtr cached = manager.Find(name);
        Check(cached.Get() != nullptr, "混合分组正常解析并发布");

        manager.Stop(thread::ShutdownMode::CancelPending);
        manager.Collect();
    }

    // ── 用例 4：自定义字节源 ───────────────────────────────────────────
    void TestByteSourceRead()
    {
        LoadedFileManager manager;
        RecordingReceiver receiver;

        Check(manager.Start(1), "启动管理器");

        const OSString name = SelfDirFile(OS_TEXT("definitely-missing-from-disk.bin"));
        MemoryByteSource *source = new MemoryByteSource;

        // 文件不存在：只有字节源能让这次读盘成功
        FileLoadId id = manager.Submit(name, nullptr, io::AsyncFilePriority::Normal,
                                       &receiver, source, nullptr,
                                       (uint64)MemoryByteSource::kSize);

        Check(id != 0, "字节源提交成功");
        Check(g_source_alive.load() == 1, "字节源存活(归分组所有)");
        Check(WaitFor(receiver.events, 1), "收到终态事件");

        thread::TaskState state = thread::TaskState::Pending;
        int32 error_code = 0;
        uint64 completed = 0;
        void *user_data = nullptr;

        receiver.TakeState(state, error_code, completed, user_data);
        Check(state == thread::TaskState::Done, "内存源读盘成功");
        Check(completed == (uint64)MemoryByteSource::kSize, "completed 等于源字节数");

        LoadedFile *file = receiver.LastAdopted();
        Check(file != nullptr, "携带文件引用");
        if (file)
        {
            Check(file->GetData() != nullptr, "字节可读");

            const uint8 *bytes = file->GetData();
            bool same = true;

            for (int64 i = 0; i < MemoryByteSource::kSize; i++)
                if (bytes[i] != MemoryByteSource::kBytes[i])
                    same = false;

            Check(same, "内容与内存源一致");
        }

        manager.Pump();

        // 放手接管的引用：文件计数归零，进待释放队列，由 Collect 回收
        receiver.ReleaseAdopted();

        Check(g_source_alive.load() == 0, "分组析构删除字节源");

        manager.Stop(thread::ShutdownMode::CancelPending);
        manager.Collect();
    }

    // ── 用例 5：失败路径中继 Failed ────────────────────────────────────
    void TestFailedRelay()
    {
        LoadedFileManager manager;
        RecordingReceiver receiver;

        Check(manager.Start(1), "启动管理器");

        const OSString name = SelfDirFile(OS_TEXT("definitely-missing-from-disk.bin"));

        FileLoadId id = manager.Submit(name, nullptr, io::AsyncFilePriority::Normal, &receiver);
        Check(id != 0, "失败路径提交也被接受");
        Check(WaitFor(receiver.events, 1), "收到终态事件");

        thread::TaskState state = thread::TaskState::Pending;
        int32 error_code = 0;
        uint64 completed = 0;
        void *user_data = nullptr;

        receiver.TakeState(state, error_code, completed, user_data);
        Check(state == thread::TaskState::Failed, "Failed 事件");
        Check(error_code != 0, "携带错误码");
        Check(receiver.AdoptedCount() == 0, "失败不携带文件");

        manager.Pump();
        manager.Stop(thread::ShutdownMode::CancelPending);
        manager.Collect();
    }
}//namespace

int main()
{
    hgl::logger::InitLogger(OS_TEXT("TestLoadedFileManager"));

    TestReceiverOnlyRead();
    TestCacheHitRelay();
    TestMixedRelayAndHandler();
    TestByteSourceRead();
    TestFailedRelay();

    GLogInfo(u8"LoadedFileManager：通过 %d，失败 %d", g_pass, g_fail);

    return (g_fail == 0) ? 0 : 1;
}
