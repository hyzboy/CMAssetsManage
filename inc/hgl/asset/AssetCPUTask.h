#pragma once

/**
 * CPU 阶段资产任务的公共契约（三级流水线的 Tier 2 基类）。
 *
 * ## 为什么需要这一层
 *
 * Tier 1（`AssetIODispatcher`）只负责把**原始字节**从盘上搬到 RAM。字节到手之后，
 * 「这堆字节是什么格式、该由谁解析、解析完交给哪个线程」与 IO 毫无关系，却被历史
 * 实现统统塞在主线程里同步完成 —— 这正是大模型装载时掉帧的根因。
 *
 * 本头把「解析工作」抽象成可投递的任务对象，由 `AssetCPUTaskPool` 在 CPU 工作线程
 * 上执行，并按任务声明的**亲和性**把成品送回对应线程。
 *
 * ## 两个亲和性不是一回事
 *
 * ```
 *   affinity         = Execute() 应该在哪个线程跑   （输入侧）
 *   result_affinity  = 任务完成后应该把对象送到哪   （输出侧）
 * ```
 *
 * 二者常常不同，且这种不同正是分流 B 的价值所在：工作线程只做「元信息快切」
 * （纯 CPU、无上下文依赖），成品立刻被送回主线程做 GPU 上传 —— 主线程因此只承担
 * 微秒级的提交，而不承担毫秒级的解析。
 *
 * ## 所有权
 *
 * 任务对象一律由 `new` 出来（派生类允许持有任意私有成员），经 `AssetCPUTaskPool::Submit()`
 * **移交所有权**给池；池在任务完成并交付（`Collect()` 取回，或投递到 `AssetCPUTaskSink`）
 * 之后**不再触碰**该对象，所有权随之转给接收方。接收方处理完必须 `delete`。
 *
 * 这条链上没有隐藏的回收池：谁最后拿到指针，谁负责销毁。
 */

#include <hgl/type/String.h>
#include <hgl/type/EnumUtil.h>
#include <hgl/serialize/EnumMeta.h>
#include <hgl/platform/Platform.h>
#include <hgl/thread/TaskNotification.h>

#include <cstdint>

namespace hgl::asset
{
    /**
     * 任务的**执行亲和性**。
     *
     * 只描述「在哪跑」，不描述「是什么资产」—— 后者是 `AssetCPUTaskKind` 与派生类的事。
     */
    enum class ExecutionAffinity : uint8_t
    {
        WorkerAny = 0,  ///< 任意 CPU 工作线程（纯 CPU、无外部上下文依赖，默认）
        MainThread,     ///< 主渲染线程（GPU 上传、场景图挂载等上下文依赖）
        Dedicated,      ///< 由 `AssetCPUTask::target_id` 指定的专用线程/子系统

        ENUM_CLASS_RANGE(WorkerAny, Dedicated)
    };

    /// 任务种类。仅用于统计与诊断，不参与调度决策。
    enum class AssetCPUTaskKind : uint8_t
    {
        GenericDeserialize = 0, ///< 分流 A：`HGL_ASSET_STRUCT` 通用反序列化
        MetaProbe,              ///< 分流 B：仅元信息快切（`MetaObject::Init()`）
        Custom,                 ///< 业务自定义

        ENUM_CLASS_RANGE(GenericDeserialize, Custom)
    };

    /// 任务执行结果。`Pending` 之外的取值一旦写入即不再变化。
    enum class AssetCPUTaskStatus : uint8_t
    {
        Pending = 0,    ///< 尚未执行（含被取消但未执行）
        Done,           ///< 执行成功
        Failed,         ///< 执行失败，原因见 `error`
        Cancelled,      ///< 排队期间被取消，`Execute()` 从未调用

        ENUM_CLASS_RANGE(Pending, Cancelled)
    };

    /**
     * CPU 阶段任务的基类。
     *
     * 派生类只需实现 `Execute()`；调度、计时、状态落地、投递全部由
     * `AssetCPUTaskPool` 统一完成，派生类里**不要**再写线程代码。
     */
    class AssetCPUTask
    {
        NO_COPY_NO_MOVE(AssetCPUTask)

    protected:

        explicit AssetCPUTask(const AssetCPUTaskKind k)
            : kind(k)
        {
        }

    public:

        virtual ~AssetCPUTask() = default;

        /// 任务种类（构造期固定）
        const AssetCPUTaskKind kind;

        /// `Execute()` 的目标线程
        ExecutionAffinity affinity = ExecutionAffinity::WorkerAny;

        /// 完成后送交的目标线程；默认主线程（Tier 3 提交阶段的归属）
        ExecutionAffinity result_affinity = ExecutionAffinity::MainThread;

        /// `affinity == Dedicated` 时的执行端标识（该任务不在本池线程执行），其它情况忽略
        uint32 target_id = 0;

        /// `result_affinity == Dedicated` 时的消费端标识，其它情况忽略
        uint32 result_target_id = 0;

        /// 提交方附加的任意上下文；池不解释、不释放
        void *user_data = nullptr;

        /**
         * 统一任务中继接收器（可选）。
         *
         * 不为空时，任务在池内终结（或按 `result_affinity` 交付）后由池直接调用
         * `receiver->OnTaskEvent(ev)` 点对点通知，跳过 `RegisterSink` 的锁表查表；
         * 事件里的 `event.task` 即本任务指针，**所有权随事件移交接收方**，
         * 接收方处理完必须 `delete`。为空时维持原行为：`Dedicated` 走 sink 表，
         * 其余进主线程邮箱由 `Collect()` 批量取回。
         *
         * 回调在 CPU 工作线程（或调用 `Submit`/`Stop` 的线程）上下文触发，
         * 接收器实现必须线程安全且极轻量（通常只压入自己的 `ThreadMessagePool`）。
         */
        thread::TaskReceiver *receiver = nullptr;

        /// 执行结果（由池在 `Execute()` 返回后写入）
        AssetCPUTaskStatus status = AssetCPUTaskStatus::Pending;

        /// `Execute()` 耗时（微秒；由池测量，供卡顿归因）
        int64 elapsed_microseconds = 0;

        /// 执行本任务的 worker 序号（未在工作线程执行时为 0）
        uint32 worker_index = 0;

        /// 失败诊断文案；成功时为空
        AnsiString error;

    public:

        /**
         * @brief 在工作线程执行本任务（纯 CPU，不得阻塞、不得上传 GPU 资源）。
         * @return true 成功；false 失败（`error` 应写明原因）
         */
        virtual bool Execute() = 0;

        bool IsDone() const { return status == AssetCPUTaskStatus::Done; }
        bool IsFailed() const { return status == AssetCPUTaskStatus::Failed; }
        bool IsCancelled() const { return status == AssetCPUTaskStatus::Cancelled; }
    };//class AssetCPUTask

    /**
     * `Dedicated` 亲和性的消费端（专用线程 / 子系统）。
     *
     * 实现必须**线程安全**：`OnAssetCPUTaskReady()` 由 CPU 工作线程直接调用，
     * 实现内部通常只是把一个指针推入自己的无锁队列。
     *
     * 典型实现者：纹理上传线程、音频解码线程、物理烘焙线程。
     */
    class AssetCPUTaskSink
    {
        NO_COPY_NO_MOVE(AssetCPUTaskSink)

    protected:

        AssetCPUTaskSink() = default;

    public:

        virtual ~AssetCPUTaskSink() = default;

        /**
         * @brief 接收一个已完成的任务（所有权移交）。
         * @note 可能在 CPU 工作线程调用；接收方负责最终 `delete`。
         */
        virtual void OnAssetCPUTaskReady(AssetCPUTask *task) = 0;
    };//class AssetCPUTaskSink
}//namespace hgl::asset

// ── 枚举登记 ───────────────────────────────────────────────────────────────
//
// 登记块必须写在全局作用域（见 `<hgl/serialize/EnumMeta.h>`），此后
// `EnumNameOf` / `EnumParse` 与 Archive 文本后端都能直接处理这三个枚举。

HGL_ENUM_BEGIN(hgl::asset::ExecutionAffinity)
    HGL_ENUM_ITEM(WorkerAny, "任意CPU工作线程")
    HGL_ENUM_ITEM(MainThread, "主渲染线程")
    HGL_ENUM_ITEM(Dedicated, "指定专用线程")
HGL_ENUM_END(hgl::asset::ExecutionAffinity)

HGL_ENUM_BEGIN(hgl::asset::AssetCPUTaskKind)
    HGL_ENUM_ITEM(GenericDeserialize, "通用反序列化")
    HGL_ENUM_ITEM(MetaProbe, "元信息快切")
    HGL_ENUM_ITEM(Custom, "业务自定义")
HGL_ENUM_END(hgl::asset::AssetCPUTaskKind)

HGL_ENUM_BEGIN(hgl::asset::AssetCPUTaskStatus)
    HGL_ENUM_ITEM(Pending, "未执行")
    HGL_ENUM_ITEM(Done, "执行成功")
    HGL_ENUM_ITEM(Failed, "执行失败")
    HGL_ENUM_ITEM(Cancelled, "已取消")
HGL_ENUM_END(hgl::asset::AssetCPUTaskStatus)
