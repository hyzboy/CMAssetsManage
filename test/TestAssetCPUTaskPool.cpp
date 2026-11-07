// TestAssetCPUTaskPool.cpp —— Tier 2（CPU 任务池）的契约判据。
//
// 锁定六件事：
//   1. 生命周期：Start 只能成功一次；Stop 幂等；停止后 Submit 一律失败且不吞所有权；
//   2. WorkerAny：任务真的在工作线程上跑了，成品经 Collect() 交回主线程；
//   3. 失败传播：Execute() 返回 false ⇒ status=Failed 且 error 文案保留；
//   4. 输入侧亲和性：affinity=MainThread 的任务**不**在工作线程执行，原样进邮箱；
//   5. 输出侧亲和性：result_affinity=Dedicated 命中 sink 走 sink，未登记则回退主线程；
//   6. 统计自洽：pending 归零、peak_pending>0、exec_samples 与 executed 同步增长；
//   7. TaskReceiver 点对点中继：Done/Pending/Cancelled 直达接收器，不进邮箱，
//      接收方可用 ThreadMessagePool 批量排空并接管 delete。
//
// 用例不读文件、不写文件，只依赖本库自身。
#include <hgl/asset/AssetCPUTaskPool.h>
#include <hgl/asset/AssetCPUTasks.h>

#include <hgl/log/Log.h>
#include <hgl/log/Logger.h>
#include <hgl/sync/Atomic.h>
#include <hgl/sync/Mutex.h>
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

    // ── 测试用任务 ─────────────────────────────────────────────────────────

    /// 计数任务：证明它确实被执行过
    class CountingTask : public AssetCPUTask
    {
    public:

        explicit CountingTask(atom_int *counter)
            : AssetCPUTask(AssetCPUTaskKind::Custom)
            , hits(counter)
        {
        }

        bool Execute() override
        {
            if (hits)
                hits->fetch_add(1);

            return true;
        }

        atom_int *hits = nullptr;
    };

    /// 失败任务：自带诊断文案
    class FailingTask : public AssetCPUTask
    {
    public:

        FailingTask() : AssetCPUTask(AssetCPUTaskKind::Custom) {}

        bool Execute() override
        {
            error = "故意失败";
            return false;
        }
    };

    /// 记录 sink：用于验证 Dedicated 交付
    class RecordingSink : public AssetCPUTaskSink
    {
    public:

        atom_int *counter = nullptr;
        AssetCPUTask *last = nullptr;
        atom_int received{0};

        explicit RecordingSink(atom_int *c) : counter(c) {}

        void OnAssetCPUTaskReady(AssetCPUTask *task) override
        {
            last = task;
            received.fetch_add(1);

            if (counter)
                counter->fetch_add(1);

            // 所有权已移交本 sink：用完就销毁
            delete task;
        }
    };

    /// 等主线程邮箱攒够 count 个成品（超时返回实际数量）
    int CollectUntil(AssetCPUTaskPool &pool,
                     ValueArray<AssetCPUTask *> &out,
                     const int count,
                     const double timeout_seconds = 5.0)
    {
        const uint64 deadline = GetUptimeUs() + static_cast<uint64>(timeout_seconds * 1000000.0);

        while (out.GetCount() < count && GetUptimeUs() < deadline)
        {
            pool.Collect(out);

            if (out.GetCount() >= count)
                break;

            SleepSecond(0.001);
        }

        return out.GetCount();
    }

    void DestroyAll(ValueArray<AssetCPUTask *> &list)
    {
        for (int i = 0; i < list.GetCount(); ++i)
            delete list[i];

        list.Clear();
    }

    // ── 1. 生命周期 ────────────────────────────────────────────────────────

    void TestLifecycle()
    {
        AssetCPUTaskPool pool;

        Check(!pool.IsRunning(), "未 Start 时不应处于运行状态");

        // 池未运行：Submit 失败，且**不**接管所有权（调用方仍需 delete）
        AssetCPUTask *orphan = new CountingTask(nullptr);
        Check(!pool.Submit(orphan), "未 Start 时 Submit 应失败");
        Check(orphan->status == AssetCPUTaskStatus::Pending, "被拒的任务状态不应被改写");
        delete orphan;

        Check(pool.Start(2), "Start(2) 应成功");
        Check(pool.IsRunning(), "Start 后应处于运行状态");
        Check(pool.GetWorkerCount() == 2, "工作线程数应为 2");
        Check(!pool.Start(4), "重复 Start 应失败");

        Check(pool.GetStatistics().submitted == 0, "刚启动时 submitted 应为 0");

        Check(pool.Stop(), "Stop 应成功");
        Check(!pool.IsRunning(), "Stop 后不应处于运行状态");
        Check(pool.Stop(), "重复 Stop 应幂等成功");
        Check(!pool.Start(2), "Stop 后不可重新启动");

        AssetCPUTask *late = new CountingTask(nullptr);
        Check(!pool.Submit(late), "Stop 后 Submit 应失败");
        delete late;
    }

    // ── 2. WorkerAny 执行 + Collect ────────────────────────────────────────

    void TestWorkerAnyExecution()
    {
        AssetCPUTaskPool pool;

        Check(pool.Start(3), "Start(3) 应成功");

        atom_int hits{0};

        constexpr int TaskCount = 24;

        for (int i = 0; i < TaskCount; ++i)
            Check(pool.Submit(new CountingTask(&hits)), "Submit 应成功");

        ValueArray<AssetCPUTask *> done;
        const int got = CollectUntil(pool, done, TaskCount);

        Check(got == TaskCount, "应取回全部任务");
        Check(hits.load() == TaskCount, "每个任务都应被执行一次");

        int done_count = 0;
        int bad_worker = 0;

        for (int i = 0; i < done.GetCount(); ++i)
        {
            AssetCPUTask *t = done[i];

            if (t->IsDone())
                ++done_count;

            if (t->worker_index >= 3)
                ++bad_worker;

            if (t->elapsed_microseconds < 0)
                ++bad_worker;
        }

        Check(done_count == TaskCount, "全部任务状态应为 Done");
        Check(bad_worker == 0, "worker_index 应落在 [0,3) 且耗时非负");

        const AssetCPUTaskPoolStatistics st = pool.GetStatistics();

        Check(st.submitted == TaskCount, "submitted 应等于提交数");
        Check(st.executed == TaskCount, "executed 应等于提交数");
        Check(st.delivered_main == TaskCount, "成品应全部回到主线程邮箱");
        Check(st.pending == 0, "全部交付后 pending 应为 0");
        Check(st.peak_pending > 0, "peak_pending 应被记录");
        Check(st.exec_samples == st.executed, "exec_samples 应与 executed 同步");

        DestroyAll(done);

        pool.Stop();
    }

    // ── 3. 失败传播 ────────────────────────────────────────────────────────

    void TestFailurePropagation()
    {
        AssetCPUTaskPool pool;

        Check(pool.Start(1), "Start(1) 应成功");
        Check(pool.Submit(new FailingTask()), "失败任务也应被接受");

        ValueArray<AssetCPUTask *> done;
        Check(CollectUntil(pool, done, 1) == 1, "失败任务也应交付");

        if (done.GetCount() == 1)
        {
            Check(done[0]->IsFailed(), "Execute 返回 false 应映射为 Failed");
            Check(!done[0]->error.IsEmpty(), "失败任务应保留诊断文案");
        }

        const AssetCPUTaskPoolStatistics st = pool.GetStatistics();

        Check(st.failed == 1, "failed 计数应为 1");
        Check(st.executed == 1, "失败也计入 executed");

        DestroyAll(done);

        pool.Stop();
    }

    // ── 4. 输入侧亲和性：MainThread ────────────────────────────────────────

    void TestMainThreadAffinity()
    {
        AssetCPUTaskPool pool;

        Check(pool.Start(2), "Start(2) 应成功");

        atom_int hits{0};

        CountingTask *task = new CountingTask(&hits);
        task->affinity = ExecutionAffinity::MainThread;

        Check(pool.Submit(task), "MainThread 任务也应被接受");

        ValueArray<AssetCPUTask *> done;
        Check(CollectUntil(pool, done, 1) == 1, "MainThread 任务应立即进邮箱");

        Check(hits.load() == 0, "MainThread 任务不得在工作线程执行");

        if (done.GetCount() == 1)
        {
            Check(done[0]->status == AssetCPUTaskStatus::Pending,
                  "未被池执行的任务状态应保持 Pending");
            Check(done[0]->worker_index == 0, "未被池执行时 worker_index 应为 0");
        }

        const AssetCPUTaskPoolStatistics st = pool.GetStatistics();

        Check(st.executed == 0, "MainThread 任务不计入 executed");
        Check(st.delivered_main == 1, "MainThread 任务应计入 delivered_main");

        DestroyAll(done);

        pool.Stop();
    }

    // ── 5. 输出侧亲和性：Dedicated sink ────────────────────────────────────

    void TestDedicatedResultSink()
    {
        AssetCPUTaskPool pool;

        Check(pool.Start(2), "Start(2) 应成功");

        constexpr uint32 SinkID = 42;

        atom_int sunk{0};
        atom_int executed{0};

        RecordingSink sink(&sunk);

        Check(pool.RegisterSink(SinkID, &sink), "登记 sink 应成功");
        Check(pool.GetSinkCount() == 1, "sink 数量应为 1");
        Check(!pool.RegisterSink(SinkID, nullptr), "空 sink 应被拒绝");

        // (a) WorkerAny 执行 + Dedicated 结果 ⇒ 工作线程直接交给 sink
        CountingTask *a = new CountingTask(&executed);
        a->result_affinity = ExecutionAffinity::Dedicated;
        a->result_target_id = SinkID;

        Check(pool.Submit(a), "结果亲和 Dedicated 的任务应被接受");

        const uint64 deadline = GetUptimeUs() + 5000000;

        while (sunk.load() == 0 && GetUptimeUs() < deadline)
            SleepSecond(0.001);

        Check(sunk.load() == 1, "成品应交到登记的 sink");
        Check(executed.load() == 1, "任务仍应在工作线程执行");

        // (b) 未登记的 sink id ⇒ 回退主线程邮箱
        CountingTask *b = new CountingTask(&executed);
        b->result_affinity = ExecutionAffinity::Dedicated;
        b->result_target_id = SinkID + 1;

        Check(pool.Submit(b), "未登记 sink 的任务也应被接受");

        ValueArray<AssetCPUTask *> done;
        Check(CollectUntil(pool, done, 1) == 1, "无消费端时应回退到主线程邮箱");

        const AssetCPUTaskPoolStatistics st = pool.GetStatistics();

        Check(st.delivered_dedicated == 1, "delivered_dedicated 应为 1");
        Check(st.delivered_fallback == 1, "delivered_fallback 应为 1");

        DestroyAll(done);

        // (c) 输入侧 Dedicated：不进工作线程，直接交执行端
        atom_int execute_side{0};

        RecordingSink executor(&execute_side);

        Check(pool.RegisterSink(SinkID + 2, &executor), "登记执行端 sink 应成功");

        CountingTask *c = new CountingTask(&executed);
        c->affinity = ExecutionAffinity::Dedicated;
        c->target_id = SinkID + 2;

        Check(pool.Submit(c), "执行侧 Dedicated 的任务应被接受");

        Check(execute_side.load() == 1, "执行侧 Dedicated 应直接交给执行端");
        Check(executed.load() == 2, "执行侧 Dedicated 不应由本池执行");

        Check(pool.UnregisterSink(SinkID + 2), "注销 sink 应成功");
        Check(pool.UnregisterSink(SinkID), "注销 sink 应成功");
        Check(pool.GetSinkCount() == 0, "注销后 sink 数量应为 0");
        Check(!pool.UnregisterSink(SinkID), "重复注销应返回 false");

        pool.Stop();
    }

    // ── 6. 通用反序列化任务（分流 A） ──────────────────────────────────────

    /// 最小可序列化资产：手写两个字段，证明任务走的是通用 Archive 路径
    class MockAsset : public AssetObject
    {
    public:

        AnsiString asset_name = "mock";
        AnsiString title;
        int32 count = 0;

        const ClassMetaInfo *GetClassMetaInfo() const override { return nullptr; }
        const AnsiString &GetAssetName() const override { return asset_name; }

        bool Serialize(hgl::serialize::Archive &ar) override
        {
            ar.Field(1, "title", title);
            ar.Field(2, "count", count);

            return ar.Ok();
        }
    };

    void TestGenericDeserializeTask()
    {
        const char *toml =
            "title = \"hello\"\n"
            "count = 7\n";

        // 无目标
        {
            GenericAssetDeserializeTask t(nullptr,
                                          reinterpret_cast<const uint8 *>(toml),
                                          static_cast<int>(strlen(toml)),
                                          hgl::serialize::ArchiveFormat::Toml);
            Check(!t.Execute(), "空目标应失败");
            Check(!t.error.IsEmpty(), "空目标应带诊断文案");
        }

        // 空缓冲
        {
            MockAsset asset;
            GenericAssetDeserializeTask t(&asset, nullptr, 0,
                                          hgl::serialize::ArchiveFormat::Toml);
            Check(!t.Execute(), "空缓冲应失败");
        }

        // Auto 格式
        {
            MockAsset asset;
            GenericAssetDeserializeTask t(&asset,
                                          reinterpret_cast<const uint8 *>(toml),
                                          static_cast<int>(strlen(toml)),
                                          hgl::serialize::ArchiveFormat::Auto);
            Check(!t.Execute(), "Auto 格式应失败");
        }

        // 无法判定后缀
        {
            MockAsset asset;
            GenericAssetDeserializeTask t(&asset, OS_TEXT("noextension"),
                                          reinterpret_cast<const uint8 *>(toml),
                                          static_cast<int>(strlen(toml)));
            Check(!t.Execute(), "无后缀文件名应失败");
            Check(!t.error.IsEmpty(), "无后缀应带诊断文案");
        }

        // 正常路径：后缀解析 + TOML 解码
        {
            MockAsset asset;
            GenericAssetDeserializeTask t(&asset, OS_TEXT("mock.asset.toml"),
                                          reinterpret_cast<const uint8 *>(toml),
                                          static_cast<int>(strlen(toml)));

            Check(t.GetFormat() == hgl::serialize::ArchiveFormat::Toml,
                  "后缀 .toml 应解析为 Toml");

            Check(t.Execute(), "合法 TOML 应解码成功");
            Check(asset.title == AnsiString("hello"), "title 应被填入");
            Check(asset.count == 7, "count 应被填入");
        }

        // 缓冲在构造时已拷贝：源缓冲被覆盖后仍应解码成功
        {
            MockAsset asset;

            ValueArray<uint8> scratch;
            scratch.Add(reinterpret_cast<const uint8 *>(toml),
                        static_cast<int>(strlen(toml)));

            GenericAssetDeserializeTask t(&asset,
                                          scratch.GetData(),
                                          scratch.GetCount(),
                                          hgl::serialize::ArchiveFormat::Toml);

            scratch.Clear();
            scratch.Add(reinterpret_cast<const uint8 *>("count = 1\n"), 10);

            Check(t.Execute(), "构造后篡改源缓冲不应影响任务");
            Check(asset.count == 7, "任务应使用构造时的那份拷贝");
        }
    }

    // ── 7. 元信息快切任务（分流 B） ────────────────────────────────────────

    class MockMetaObject : public hgl::MetaObject
    {
    public:

        HGL_OBJECT(MockMetaObject, hgl::MetaObject)

        int meta_count = 0;
        int payload_count = 0;

    protected:

        bool OnInitMeta() override
        {
            ++meta_count;
            return true;
        }

        bool OnLoadPayload() override
        {
            ++payload_count;
            return true;
        }
    };//class MockMetaObject

    void TestMetaProbeTask()
    {
        // 空目标
        {
            MetaProbeTask t(nullptr);
            Check(!t.Execute(), "空目标应失败");
        }

        // 正常路径：只做 meta，不碰负载
        {
            MockMetaObject obj;
            MetaProbeTask t(&obj, ExecutionAffinity::Dedicated, 9);

            Check(t.affinity == ExecutionAffinity::WorkerAny,
                  "meta 快切自身应跑在工作线程");
            Check(t.result_affinity == ExecutionAffinity::Dedicated,
                  "负载阶段归属应透传");
            Check(t.result_target_id == 9, "负载阶段目标标识应透传");

            Check(t.Execute(), "meta 快切应成功");
            Check(obj.meta_count == 1, "OnInitMeta 应被调用一次");
            Check(obj.payload_count == 0, "meta 阶段不得加载负载");
            Check(obj.IsInitialized(), "对象应进入已初始化状态");
            Check(obj.GetPayloadState() == hgl::PayloadState::MetaOnly,
                  "载荷状态应停在 MetaOnly");
        }

        // 幂等：重复执行不重复初始化
        {
            MockMetaObject obj;
            MetaProbeTask t(&obj);

            Check(t.Execute(), "第一次应成功");
            Check(t.Execute(), "第二次应成功（幂等）");
            Check(obj.meta_count == 1, "OnInitMeta 只应被调用一次");
        }
    }

    // ── 8. TaskReceiver 点对点中继 ─────────────────────────────────────────

    /// 记录型接收器：加锁保存事件（回调来自工作线程）
    class RecordingReceiver : public thread::TaskReceiver
    {
    public:

        Mutex lock;
        ValueArray<thread::TaskEvent> events;
        atom_int received{0};

        void OnTaskEvent(const thread::TaskEvent &ev) override
        {
            MutexLock guard(&lock);
            events.Add(ev);
            received.fetch_add(1);
        }

        /// 等到攒够 count 条（超时返回 false）
        bool WaitFor(int count, double timeout_seconds = 5.0)
        {
            const uint64 deadline = GetUptimeUs()
                                  + static_cast<uint64>(timeout_seconds * 1000000.0);

            while (received.load() < count && GetUptimeUs() < deadline)
                SleepSecond(0.001);

            return received.load() >= count;
        }

        /// 排空并销毁本测试接管的任务
        ~RecordingReceiver() override
        {
            for (int i = 0; i < events.GetCount(); ++i)
                delete static_cast<AssetCPUTask *>(events[i].task);
        }
    };

    /// 阻塞任务：占住唯一的工作线程，制造排队取消场景
    class SleepingTask : public AssetCPUTask
    {
    public:

        SleepingTask() : AssetCPUTask(AssetCPUTaskKind::Custom) {}

        bool Execute() override
        {
            SleepSecond(0.5);
            return true;
        }
    };

    void TestReceiverRelay()
    {
        AssetCPUTaskPool pool;
        RecordingReceiver receiver;

        Check(pool.Start(2), "Start(2) 应成功");

        // (a) WorkerAny + receiver：Done 事件点对点到达，不进主线程邮箱
        atom_int hits{0};

        CountingTask *a = new CountingTask(&hits);
        a->receiver = &receiver;
        a->user_data = reinterpret_cast<void *>(0x1234);

        Check(pool.Submit(a), "带 receiver 的任务应被接受");
        Check(receiver.WaitFor(1), "Done 事件应到达接收器");

        {
            MutexLock guard(&receiver.lock);
            const thread::TaskEvent &ev = receiver.events[0];

            Check(ev.task == a, "事件应携带任务指针");
            Check(ev.state == thread::TaskState::Done, "事件状态应为 Done");
            Check(ev.user_data == reinterpret_cast<void *>(0x1234), "user_data 应透传");
            Check(ev.task_kind == static_cast<uint32>(AssetCPUTaskKind::Custom),
                  "task_kind 应映射任务种类");
        }

        Check(hits.load() == 1, "任务仍应在工作线程执行");

        ValueArray<AssetCPUTask *> polled;
        Check(pool.Collect(polled) == 0, "中继任务不应再进主线程邮箱");

        // (b) MainThread 亲和 + receiver：立即以 Pending 中继给接收方
        CountingTask *b = new CountingTask(&hits);
        b->affinity = ExecutionAffinity::MainThread;
        b->receiver = &receiver;

        Check(pool.Submit(b), "MainThread+receiver 也应被接受");
        Check(receiver.WaitFor(2), "Pending 事件应立即到达");

        {
            MutexLock guard(&receiver.lock);
            const thread::TaskEvent &ev = receiver.events[1];

            Check(ev.task == b, "第二个事件应携带任务 b");
            Check(ev.state == thread::TaskState::Pending, "未被执行的任务应报 Pending");
        }

        Check(hits.load() == 1, "MainThread+receiver 不得在工作线程执行");

        pool.Stop();

        const AssetCPUTaskPoolStatistics st = pool.GetStatistics();

        Check(st.delivered_receiver == 2, "delivered_receiver 应为 2");
        Check(st.delivered_main == 0, "中继路径不占用主线程邮箱");
        Check(st.pending == 0, "全部交付后 pending 应为 0");
    }

    void TestReceiverCancelRelay()
    {
        AssetCPUTaskPool pool;
        RecordingReceiver receiver;

        Check(pool.Start(1), "单线程池才能制造排队");

        // 先占满唯一 worker，再排入带 receiver 的受害者，随后 CancelPending
        Check(pool.Submit(new SleepingTask()), "占位任务应被接受（无 receiver，走邮箱）");

        atom_int hits{0};

        CountingTask *victim = new CountingTask(&hits);
        victim->receiver = &receiver;

        Check(pool.Submit(victim), "排队任务应被接受");

        Check(pool.Stop(thread::ShutdownMode::CancelPending), "CancelPending 停止应成功");
        Check(receiver.WaitFor(1), "取消也应中继 Cancelled 事件");
        Check(hits.load() == 0, "被取消的任务不得执行");

        {
            MutexLock guard(&receiver.lock);
            Check(receiver.events[0].state == thread::TaskState::Cancelled,
                  "事件状态应为 Cancelled");
        }

        // 占位任务（无 receiver）应回落到主线程邮箱
        ValueArray<AssetCPUTask *> polled;
        CollectUntil(pool, polled, 1);
        DestroyAll(polled);
    }

    void TestInboxReceiverDrain()
    {
        // 标准姿势：InboxTaskReceiver + ThreadMessagePool 批量排空
        AssetCPUTaskPool pool;

        thread::ThreadMessagePool<thread::TaskEvent> inbox;
        thread::InboxTaskReceiver receiver(&inbox);

        Check(pool.Start(2), "Start(2) 应成功");

        atom_int hits{0};

        constexpr int TaskCount = 16;

        for (int i = 0; i < TaskCount; ++i)
        {
            CountingTask *t = new CountingTask(&hits);
            t->receiver = &receiver;
            Check(pool.Submit(t), "Submit 应成功");
        }

        int drained = 0;
        const uint64 deadline = GetUptimeUs() + 5000000;

        // 排空即销毁：接收方的职责就是逐条 delete 移交过来的任务
        while (drained < TaskCount && GetUptimeUs() < deadline)
        {
            drained += inbox.Drain([&](const thread::TaskEvent &ev)
            {
                delete static_cast<AssetCPUTask *>(ev.task);
            });
        }

        Check(drained == TaskCount, "信息池应排空并销毁全部事件");
        Check(hits.load() == TaskCount, "全部任务应执行完成");

        pool.Stop();
    }
}//namespace

int main(int, char **)
{
    TestLifecycle();
    TestWorkerAnyExecution();
    TestFailurePropagation();
    TestMainThreadAffinity();
    TestDedicatedResultSink();
    TestGenericDeserializeTask();
    TestMetaProbeTask();
    TestReceiverRelay();
    TestReceiverCancelRelay();
    TestInboxReceiverDrain();

    GLogInfo(u8"通过 %d，失败 %d", g_pass, g_fail);

    return (g_fail == 0 && g_pass > 0) ? 0 : 1;
}
