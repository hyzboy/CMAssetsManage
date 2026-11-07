#pragma once

/**
 * CPU 阶段任务池（三级流水线的 Tier 2）。
 *
 * ## 职责边界
 *
 * 本类只做三件事，**不解析任何资产格式**：
 *
 * 1. 用固定数量的 CPU 工作线程执行 `AssetCPUTask::Execute()`；
 * 2. 按 `affinity` 决定任务在哪执行，按 `result_affinity` 决定成品送去哪；
 * 3. 统计（提交/执行/失败/耗时/积压），供卡顿归因。
 *
 * 具体解析逻辑全部在派生任务里（见 `AssetCPUTasks.h`）。
 *
 * ## 为什么主线程侧只有 `Collect()`
 *
 * Tier 3 的整个承诺是「主线程每帧只做微秒级的提交」。因此主线程侧 API 只有一个
 * 非阻塞的 `Collect()`：取回成品、`delete`、写回状态。这里**没有** `Wait`，
 * 因为它一旦被调用就意味着主线程开始等子线程，流水线的意义立刻归零。
 *
 * ## 线程契约
 *
 * | 方法 | 允许调用线程 |
 * |---|---|
 * | `Start` / `Stop` | 主线程（且不得从任务回调内重入） |
 * | `Submit` | 任意线程 |
 * | `Collect` | 主线程（唯一消费者） |
 * | `RegisterSink` / `UnregisterSink` | 主线程 |
 * | `GetStatistics` / `GetWorkerCount` / `IsRunning` | 任意线程 |
 *
 * `Stop(CancelPending)` 会把仍未出队的任务交回主线程邮箱并标记
 * `AssetCPUTaskStatus::Cancelled`；业务层在 `Collect()` 里按需归还占用的句柄。
 */

#include <hgl/asset/AssetCPUTask.h>

#include <hgl/thread/WorkerPool.h>
#include <hgl/thread/ShutdownMode.h>
#include <hgl/concurrency/SwapMailbox.h>
#include <hgl/type/UnorderedMap.h>
#include <hgl/type/ValueArray.h>
#include <hgl/sync/Mutex.h>
#include <hgl/sync/Atomic.h>

namespace hgl::asset
{
    /// `AssetCPUTaskPool` 的运行统计快照（值语义，可自由拷贝）。
    struct AssetCPUTaskPoolStatistics
    {
        uint32 submitted = 0;               ///< 已接受的任务总数
        uint32 executed = 0;                ///< 在工作线程执行过的任务数
        uint32 failed = 0;                  ///< `Execute()` 返回 false 的任务数
        uint32 cancelled = 0;               ///< 排队期间被取消的任务数
        uint32 delivered_main = 0;          ///< 投递到主线程邮箱的任务数
        uint32 delivered_dedicated = 0;     ///< 投递到 `Dedicated` sink 的任务数
        uint32 delivered_fallback = 0;      ///< 无 sink 而回退到主线程邮箱的任务数
        int32 pending = 0;                  ///< 已接受但尚未交付的任务数
        int32 peak_pending = 0;             ///< `pending` 的历史峰值
        uint64 exec_microseconds = 0;       ///< `Execute()` 累计耗时（微秒）
        uint64 exec_samples = 0;            ///< 参与累计的任务数（成功与失败都计）
    };

    /**
     * 固定大小 CPU 工作池 + 亲和性交付。
     *
     * 实现直接架在 `hgl::thread::WorkerPool<AssetCPUTask>` 之上：底层负责线程生命周期
     * 与队列，本类只补「亲和性分流」与「成品回收」两块。
     */
    class AssetCPUTaskPool : public thread::WorkHandler<AssetCPUTask>
    {
        NO_COPY_NO_MOVE(AssetCPUTaskPool)

    private:    // 内部子对象，必须先于 sink 表析构

        thread::WorkerPool<AssetCPUTask> pool;
        SwapMailbox<AssetCPUTask *> main_mailbox;

    private:

        mutable Mutex sink_lock;
        UnorderedMap<uint32, AssetCPUTaskSink *> sinks;

        atom_bool started{false};
        atom_bool stopped{false};

        atom_uint submitted{0};
        atom_uint executed{0};
        atom_uint failed{0};
        atom_uint cancelled{0};
        atom_uint delivered_main{0};
        atom_uint delivered_dedicated{0};
        atom_uint delivered_fallback{0};
        atom_uint pending{0};
        atom_uint peak_pending{0};
        atom_uint64 exec_microseconds{0};
        atom_uint64 exec_samples{0};

    private:

        /// 把成品送到 `task` 声明的目标（工作线程或主线程均可调用）
        void Deliver(AssetCPUTask *task);

        /**
         * 把一个任务交给登记过的 `AssetCPUTaskSink`。
         * @return false 表示该 `sink_id` 未登记（调用方需自行回退）
         */
        bool PostToSink(AssetCPUTask *task, uint32 sink_id);

        void AccountPending(int delta);

    public:

        AssetCPUTaskPool();
        ~AssetCPUTaskPool() override;

        /**
         * @brief 启动工作线程。
         * @param worker_count 工作线程数；`0` 表示自动（逻辑核心数 - 2，至少 1，至多 8）
         * @return false 表示已在运行、已停止或创建工作线程失败
         */
        bool Start(uint worker_count = 0);

        /// 自动工作线程数（与 `Start(0)` 采用同一口径）
        static uint GetRecommendedWorkerCount();

        /**
         * @brief 关闭入口并等待工作线程退出。
         * @param mode `Drain` 执行完已入队任务；`CancelPending` 取消未出队任务
         * @return 至少一个 worker 未能 Join 时返回 false
         */
        bool Stop(thread::ShutdownMode mode = thread::ShutdownMode::Drain);

        /// 是否处于可提交状态
        bool IsRunning() const { return started.load() && !stopped.load(); }

        uint GetWorkerCount() const { return pool.GetWorkerCount(); }

        /**
         * @brief 提交一个任务，所有权移交本池。
         *
         * `affinity == WorkerAny` 的任务进入工作线程队列；其余亲和性**不执行**，
         * 直接按 `result_affinity` 交付 —— 提交方因此可以用同一个 API 表达
         * 「这活儿归主线程/专用线程」，不需要自己判断。
         *
         * @return false 表示池未运行或任务为空（此时所有权仍归调用方）
         */
        bool Submit(AssetCPUTask *task);

        /**
         * @brief 顺序提交一组任务。
         * @return 已接受的前缀长度；首个失败即停止
         */
        int Submit(AssetCPUTask **tasks, int count);

        /**
         * @brief 主线程取回成品（非阻塞，唯一消费者）。
         * @param out 追加写入已取回的任务指针；**所有权移交调用方**，需自行 delete
         * @return 本次取回的数量
         */
        int Collect(ValueArray<AssetCPUTask *> &out);

        /**
         * @brief 为 `Dedicated` 亲和性登记消费端。
         * @note `sink` 由调用方持有，必须在 `UnregisterSink` 之后才可销毁
         */
        bool RegisterSink(uint32 target_id, AssetCPUTaskSink *sink);

        bool UnregisterSink(uint32 target_id);

        /// 当前登记的 sink 数量
        int GetSinkCount() const;

        /// 统计快照（可在任意线程调用）
        AssetCPUTaskPoolStatistics GetStatistics() const;

    public:     // thread::WorkHandler

        void OnWork(uint worker_index, AssetCPUTask *work) override;

        void OnCancel(AssetCPUTask *work) override;
    };//class AssetCPUTaskPool
}//namespace hgl::asset
