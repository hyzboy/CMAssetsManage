#pragma once

#include<hgl/asset/AssetIODispatcher.h>
#include<hgl/asset/LoadedFileHandler.h>
#include<hgl/concurrency/SwapMailbox.h>
#include<hgl/sync/Mutex.h>
#include<hgl/thread/ShutdownMode.h>
#include<hgl/thread/TaskNotification.h>
#include<hgl/type/Queue.h>
#include<hgl/type/UnorderedMap.h>
#include<hgl/type/ValueArray.h>

namespace hgl
{
    /**
     * @brief CN: 加载管理器的运行统计快照。
     *        EN: Snapshot of the load manager's runtime statistics.
     * @details CN: 除 io 外的字段只由调用 Pump()/Collect() 的线程更新，读取时无需加锁。
     *          EN: All fields except io are updated only by the thread calling
     *              Pump()/Collect(), so reading them needs no locking.
     */
    /// @brief CN: TaskEvent.task_kind 中本管理器 IO 任务的编号。 EN: task_kind value used for this manager's IO tasks.
    static const uint32 AssetIOTaskKind=1;

    struct LoadedFileManagerStatistics
    {
        uint64 submitted=0;             ///< @brief CN: Submit 接受的请求数。 EN: Requests accepted by Submit.
        uint64 cache_hits=0;            ///< @brief CN: 直接命中已加载表(含待释放区复活)的请求数。 EN: Requests served from the loaded table, including resurrection from the pending-release area.
        uint64 merged_in_flight=0;      ///< @brief CN: 合并到在途读盘、未额外发起读盘的请求数。 EN: Requests merged into an in-flight read instead of starting a new one.
        uint64 dispatched_loaded=0;     ///< @brief CN: 已回调 OnFileLoaded 的次数。 EN: Number of OnFileLoaded callbacks delivered.
        uint64 dispatched_failed=0;     ///< @brief CN: 已回调 OnFileFailed 的次数。 EN: Number of OnFileFailed callbacks delivered.
        uint64 dispatched_cancelled=0;  ///< @brief CN: 已回调 OnFileCancelled 的次数。 EN: Number of OnFileCancelled callbacks delivered.
        uint64 alive_files=0;           ///< @brief CN: 已加载文件表中的文件数(含待释放)。 EN: Files in the loaded table, including pending-release ones.
        uint64 pending_release=0;       ///< @brief CN: 当前待释放队列长度。 EN: Current pending-release queue length.
        uint64 released_files=0;        ///< @brief CN: Collect 累计真正释放的文件数。 EN: Files actually released by Collect so far.
        io::AsyncFileLoaderStatistics io;  ///< @brief CN: 底层读盘统计。 EN: Underlying read statistics.
    };//struct LoadedFileManagerStatistics

    /**
     * @brief CN: 通用异步文件加载管理器(两段式)：IO 线程读盘 + 调用线程提交。
     *        EN: Generic asynchronous file-load manager (two stages): IO-thread reads plus caller-thread commit.
     * @details
     * CN: 第一段(读取)由 CMCore 的 io::AsyncFileLoader 在 IO 线程池中把整个文件读进
     *     LoadedFile 自带的请求缓冲；第二段(提交)由调用线程周期性调用 Pump()，
     *     在其中回调 LoadedFileHandler，便于把结果投递到需要线程亲和性的目标(GPU 队列、
     *     音频线程等)。
     *
     *     已加载文件表与待释放队列共用同一把锁 table_lock：
     *      - 引用归零只入待释放队列，不立即销毁；
     *      - Collect() 时若计数仍为 0 才真正 Release()+delete；
     *      - 排队期间被 Find()/Submit() 命中则 RefInc 复活，Collect() 放过它。
     *     由此得到的语义是"弱表缓存"：只要还有人持有 LoadedFilePtr，同名文件就不会重复读盘。
     *
     *     表键由 MakeLoadedFileKey() 生成：分隔符规范化 + Windows 下大小写折叠。因此
     *     "Res\\Image/A.Tex2D" 与 "res/image/a.tex2d" 视为同一个文件；回调里报告的文件名
     *     仍是该文件首次 Submit 时的原始拼写。
     *
     *     读盘默认只有一个 IO 线程(Start 的默认参数为 1)：同一块物理设备上的并发读会互相
     *     争道，串行反而更快更稳。未来按设备分流时再提高线程数。同一路径在途期间的重复
     *     Submit 会合并到同一个 ReadGroup，只发起一次读盘，所有请求各自收到回调。
     *
     *     线程约束：
     *      - Submit/Cancel/Pump/Collect/Stop 只允许在同一(主)线程调用；
     *      - Find 以及 LoadedFile 的 RefInc/RefDec 可在任意线程调用。
     *
     *     管理器必须比所有 LoadedFile 活得更久；析构时会解除关联(见析构函数说明)。
     * EN: Stage one (read) happens on AsyncFileLoader worker threads, which read a whole
     *     file into the request buffer owned by LoadedFile. Stage two (commit) happens when
     *     the caller invokes Pump() periodically; handlers run there so results can be
     *     routed to thread-affine destinations (GPU queues, audio threads, ...).
     *
     *     The loaded-file table and the pending-release queue share one table_lock: a
     *     reference count reaching zero only enqueues the file, Collect() performs the real
     *     Release()+delete when the count is still zero, and a hit through Find()/Submit()
     *     while queued resurrects it with RefInc. The result is a weak table: as long as
     *     someone holds a LoadedFilePtr the same path is never read twice.
     *
     *     Table keys come from MakeLoadedFileKey(): separator normalization plus case
     *     folding on Windows, so "Res\\Image/A.Tex2D" and "res/image/a.tex2d" are one
     *     entry. The file name reported to handlers keeps the spelling used by the first
     *     Submit of that file.
     *
     *     Reading defaults to a single IO worker (Start's default argument is 1): concurrent
     *     reads on one physical device contend for the same head, so serializing is faster
     *     and steadier. Raise the worker count only after reads are split per device.
     *     Repeated Submit of one path while it is being read merges into a single ReadGroup:
     *     the file is read once and every request still gets its own callback.
     *
     *     Threading: Submit/Cancel/Pump/Collect/Stop must be called from one thread
     *     (normally the main thread); Find and reference counting are thread-safe.
     *     The manager must outlive every LoadedFile; destruction detaches them.
     */
    class LoadedFileManager:public io::AsyncFileLoadHandler
    {
        friend class LoadedFile;

        /**
         * @brief CN: 一次提交请求的私有状态。 EN: Private state of one submitted request.
         */
        enum class Outcome:uint8
        {
            Loading=0,      ///< @brief CN: 仍在读盘。 EN: Still reading.
            Loaded,         ///< @brief CN: 读取成功(可能来自缓存)。 EN: Read succeeded, possibly from cache.
            Failed,         ///< @brief CN: 读取或负载校验失败。 EN: Read or payload validation failed.
            Cancelled       ///< @brief CN: 被取消。 EN: Cancelled.
        };//enum class Outcome

        struct ReadGroup;

        /**
         * @brief CN: 一次请求的完整上下文，贯穿读取段与提交段。
         *        EN: Full context of one request spanning both stages.
         * @details CN: Job 由 Submit 创建、由回调段销毁。命中缓存时 file 直接可用；
         *              需要读盘时只挂到所属 ReadGroup 的等待者表上，结果在 DispatchGroup 里填写。
         *          EN: Created by Submit and destroyed by the dispatch stage. A cache hit fills
         *              file directly; a read joins its ReadGroup's waiter list and receives its
         *              result in DispatchGroup.
         */
        struct Job
        {
            FileLoadId                  id=0;                       ///< @brief CN: 请求编号。 EN: Request identifier.
            LoadedFilePtr               file;                       ///< @brief CN: 命中缓存时的文件对象。 EN: File object, for cache hits only.
            ReadGroup *                 group=nullptr;              ///< @brief CN: 所属在途读盘分组，命中缓存时为空。 EN: Owning in-flight read group; null for cache hits.
            LoadedFileHandler *         handler=nullptr;            ///< @brief CN: 结果接收者(不拥有)。 EN: Result receiver, not owned.
            thread::TaskReceiver *      receiver=nullptr;           ///< @brief CN: 统一任务通知接收方(不拥有)，可空。 EN: Unified task receiver, not owned; may be null.
            void *                      task_user_data=nullptr;     ///< @brief CN: 随 TaskEvent.user_data 原样回投的发起方上下文。 EN: Submitter context echoed in TaskEvent.user_data.
            bool                        relay_fired=false;          ///< @brief CN: receiver 已收到终态事件。 EN: Terminal event already delivered to receiver.
            bool                        cancel_requested=false;     ///< @brief CN: 调用方已请求取消。 EN: Caller requested cancellation.
        };//struct Job

        /**
         * @brief CN: 一次真实读盘以及所有等待它的请求。
         *        EN: One real read together with every request waiting on it.
         * @details CN: 同一路径在途期间的重复 Submit 只发起一次读盘，全部请求挂到同一分组里，
         *              读完后逐个回调。只有当分组内所有请求都被取消时才真正取消底层读盘。
         *          EN: Repeated in-flight Submit for one path starts a single read; every request
         *              joins the same group and is dispatched after it finishes. The underlying
         *              read is cancelled only when every request in the group asked for it.
         */
        struct ReadGroup
        {
            LoadedFilePtr               file;                       ///< @brief CN: 本次读盘的文件对象(持引用)。 EN: File object of this read, holding a reference.
            ValueArray<Job *>           waiters;                    ///< @brief CN: 等待同一读盘的请求。 EN: Requests waiting on this read.
            Mutex                       waiters_lock;               ///< @brief CN: 保护 waiters 追加/扫描与 handler_present。 EN: Guards waiter append/scan and handler_present.
            io::AsyncFileByteSource *   source=nullptr;             ///< @brief CN: 本分组拥有的字节源(裸文件读时为空)。 EN: Byte source owned by this group; null for plain file reads.
            bool                        direct=false;               ///< @brief CN: 直读装配分组：文件不进弱表缓存，也不挂入在途合并按键。 EN: Direct-to-buffer group: the file never enters the weak table or the merge key.
            bool                        handler_present=false;      ///< @brief CN: 组内有 handler 型请求；全无时 Pump 段跳过解析与发布。 EN: Any waiter uses a handler; when none, dispatch skips parse/publish.
            Outcome                     outcome=Outcome::Loading;   ///< @brief CN: 读盘结果。 EN: Read outcome.
            io::AsyncFileLoadError      error=io::AsyncFileLoadError::None; ///< @brief CN: 失败原因。 EN: Failure reason.

            ~ReadGroup(){ delete source; }
        };//struct ReadGroup

    private:

        asset::AssetIODispatcher        loader;                     ///< @brief CN: 按物理设备分流的读盘调度器。 EN: Read dispatcher split by physical device.
        SwapMailbox<ReadGroup *>        done_mailbox;               ///< @brief CN: 多写单读：IO 线程 → 调用线程。 EN: Multiple writers, one reader: IO threads to caller thread.

        FileLoadId                      next_id=1;                  ///< @brief CN: 下一个请求编号。 EN: Next request identifier.
        UnorderedMap<FileLoadId,Job *>  jobs;                       ///< @brief CN: 尚未回调完的请求(调用线程独占)。 EN: Requests not yet dispatched (caller-thread only).
        UnorderedMap<OSString,ReadGroup *> reading;                 ///< @brief CN: 在途读盘表：规范键 → 分组(调用线程独占)。 EN: In-flight reads, key to group (caller-thread only).
        Queue<Job *>                    ready;                      ///< @brief CN: 待回调的缓存命中请求(调用线程独占)。 EN: Cache-hit requests awaiting dispatch (caller-thread only).
        Queue<ReadGroup *>              ready_groups;               ///< @brief CN: 已从邮箱取出、待回调的读盘分组(调用线程独占)。 EN: Completed groups taken from the mailbox (caller-thread only).

        Mutex                           table_lock;                 ///< @brief CN: 保护下面三项。 EN: Guards the three members below.
        UnorderedMap<OSString,LoadedFile *> files;                  ///< @brief CN: 已加载文件表(弱引用，不增加计数)。 EN: Loaded-file table holding weak, non-owning pointers.
        ValueArray<LoadedFile *>        pending_release;            ///< @brief CN: 引用归零、等待 Collect 的文件。 EN: Files whose count reached zero and await Collect.
        uint64                          released_files=0;           ///< @brief CN: 累计真正释放的文件数。 EN: Total files actually released.

        LoadedFileManagerStatistics     stats;                      ///< @brief CN: 调用线程独占的计数。 EN: Counters owned by the caller thread.
        bool                            started=false;              ///< @brief CN: 是否已启动。 EN: Whether the manager has been started.

    private:

        bool OnFileLoaded   (uint worker_index,io::AsyncFileLoadRequest *request)override;
        void OnFileError    (uint worker_index,io::AsyncFileLoadRequest *request,io::AsyncFileLoadError error)override;
        void OnFileCancelled(uint worker_index,io::AsyncFileLoadRequest *request)override;

        /** @brief CN: IO 线程：记录分组的读盘结果并投递邮箱。 EN: IO thread: record the group's outcome and post it to the mailbox. */
        void PostGroup(ReadGroup *group,Outcome outcome,io::AsyncFileLoadError error=io::AsyncFileLoadError::None);

        /**
         * @brief CN: 向 job 的 receiver 投递终态事件(至多一次)。
         *        EN: Deliver the terminal event to the job's receiver (at most once).
         * @details CN: Loaded+file 时事件携带一个额外的文件引用：接收方直接接管它，
         *              不再使用时调用一次 RefDec()。字节数据此刻只是"已读回"，
         *              负载解析仍按既有流程在 Pump 段(或接收方自己的 CPU 线程)完成。
         *          EN: With Loaded+file the event carries one extra file reference the
         *              receiver adopts directly and must RefDec() once when done. The
         *              bytes are only "read back"
         *              here; payload parsing still happens in the Pump stage (or the
         *              receiver's own CPU thread).
         */
        void FireJobRelay(Job *job,LoadedFile *file,Outcome outcome,const uint64 bytes,io::AsyncFileLoadError error);

        /** @brief CN: IO/停止线程：扫描分组等待者并投递未发的终态事件。 EN: IO/stop thread: scan group waiters and fire pending terminal events. */
        void FireGroupRelay(ReadGroup *group,Outcome outcome,io::AsyncFileLoadError error);

        /** @brief CN: 引用归零回调(任意线程，持 table_lock 入队)。 EN: Reference-zero callback; enqueues while holding table_lock. */
        void OnFileRefZero(LoadedFile *file);

        /** @brief CN: 回调一个请求并销毁 Job。 EN: Dispatch one request and destroy its Job. */
        void DispatchOne(Job *job,const OSString &filename,LoadedFile *publish,Outcome outcome,io::AsyncFileLoadError error);

        /** @brief CN: 校验负载、登记到已加载表，然后回调分组里的所有请求并销毁分组。 EN: Validate the payload, register it in the loaded table, dispatch every request of the group and destroy the group. */
        int DispatchGroup(ReadGroup *group);

        /** @brief CN: Release()+delete，必须在 table_lock 之外调用。 EN: Release()+delete; must run outside table_lock. */
        void ReleaseFile(LoadedFile *file);

    public:

        LoadedFileManager();

        /**
         * @brief CN: 析构：停止读盘线程并丢弃尚未回调的请求。
         *        EN: Destructor: stop read threads and drop requests that were never dispatched.
         * @details CN: 仍被外部持有的 LoadedFile 会被解除与本管理器的关联，之后它们引用归零
         *              时按 AutoDestroy 语义自行销毁；计数已为 0 的在析构中立即销毁。
         *          EN: Files still held elsewhere are detached from this manager and destroy
         *              themselves when their count reaches zero; files already at zero are
         *              destroyed here.
         * @note CN: 调用方必须保证析构期间没有其他线程同时对 LoadedFile 做 RefDec。
         *       EN: The caller must guarantee no other thread RefDecs a LoadedFile during destruction.
         */
        ~LoadedFileManager() override;

        /**
         * @brief CN: 启动 IO 线程池，仅可成功一次。
         *        EN: Start the IO worker pool; succeeds only once.
         * @param io_worker_count CN: 每条物理设备通道的 IO 线程数，必须大于 0；默认 1。
         *                        不同物理设备之间并行，同一设备内串行。
         *                        默认按设备分流已完成，通常不需要再调大。
         *                      EN: IO worker count per physical-device channel; must be greater than 0,
         *                          default 1. Reads on one device stay serial, different devices run
         *                          in parallel through AssetIODispatcher, so the default is usually best.
         *                          Concurrent reads on one physical device contend for the same head
         *                          and channel, so serializing there is both faster and more predictable.
         * @return CN: 成功返回 true。 EN: True on success.
         */
        bool Start(uint io_worker_count=1);

        /**
         * @brief CN: 停止读盘；停止后仍可 Pump 取走已完成的结果。
         *        EN: Stop reading; completed results can still be drained through Pump.
         * @param mode CN: Drain 读完已接受的请求；CancelPending 取消仍在排队中的请求。
         *             EN: Drain processes accepted requests; CancelPending cancels still-queued requests.
         * @return CN: 所有 IO 线程成功退出时返回 true。 EN: True when every IO thread joined successfully.
         */
        bool Stop(thread::ShutdownMode mode=thread::ShutdownMode::Drain);

        /** @brief CN: 底层按物理设备分流的调度器(调试/监控用)。 EN: Underlying per-device dispatcher, for debugging and monitoring. */
        const asset::AssetIODispatcher &GetDispatcher()const{return loader;}

        /**
         * @brief CN: 提交一个文件加载请求。
         *        EN: Submit one file-load request.
         * @param filename       CN: 文件名；查表前会经 MakeLoadedFileKey() 规范化。 EN: File name; normalized through MakeLoadedFileKey() before the lookup.
         * @param handler        CN: 结果接收者，必须活到该请求回调完成；与 receiver 至少给一个。 EN: Result receiver that must outlive this request's callback; at least one of handler/receiver is required.
         * @param priority       CN: 读盘优先级(数值越小越优先)。 EN: Read priority; lower values are served first.
         * @param receiver       CN: 可选统一任务通知接收方(不拥有)。Loaded 终态事件携带一个
         *                       额外的 LoadedFile 引用：该引用由接收方直接接管，
         *                       不再使用时调用一次 file->RefDec()(不要包进 RefPtr，
         *                       那会再加一次引用)。它可能在 IO 线程或 Pump 线程被调用。
         *                       EN: Optional unified task receiver (not owned). The terminal
         *                       Loaded event carries one extra LoadedFile reference that the
         *                       receiver adopts directly: call file->RefDec() exactly once
         *                       when done (do not wrap it in RefPtr, which would RefInc again).
         *                       OnTaskEvent may run on an IO thread or the Pump thread.
         * @param source         CN: 可选自定义字节源(包内资产等)。管理器接管其所有权：
         *                       未用到(缓存命中/合并在途读盘)时当场删除，其余随读盘分组删除。
         *                       EN: Optional custom byte source; the manager owns it and deletes
         *                       it when unused (cache hit / merged read) or with its read group.
         * @param task_user_data CN: 原样回投到 TaskEvent.user_data 的发起方上下文。 EN: Submitter context echoed into TaskEvent.user_data.
         * @param expected_bytes CN: 预期字节数，参与调度器在途预算限流；0 表示不限。 EN: Expected bytes for the dispatcher in-flight budget; 0 means unconstrained.
         * @return CN: 请求编号，0 表示失败。 EN: Request identifier, or 0 on failure.
         * @warning CN: 每个带 receiver 的请求至多收到一个终态事件；但若管理器先于事件
         *              发出而被销毁(析构丢弃未派发的 Job)，接收方不会收到任何通知。
         *              管理器必须存活到所有请求派发完毕。 EN: A receiver job gets at most one
         *              terminal event; but if the manager is destroyed while jobs are still
         *              undispatched, no event is delivered. The manager must outlive its requests.
         * @note CN: 命中已加载表时不读盘，结果在下一次 Pump() 中回调。
         *           同一路径已在读盘时不会重复读盘：本次请求挂到在途分组上，
         *           与先前的请求共享同一份数据。
         *       EN: A table hit skips reading and is dispatched by the next Pump().
         *           When the same path is already being read the read is not repeated: this
         *           request joins the in-flight group and shares the same payload.
         */
        FileLoadId Submit(const OSString &filename,
                          LoadedFileHandler *handler,
                          io::AsyncFilePriority priority=io::AsyncFilePriority::Normal,
                          thread::TaskReceiver *receiver=nullptr,
                          io::AsyncFileByteSource *source=nullptr,
                          void *task_user_data=nullptr,
                          uint64 expected_bytes=0);

        /**
         * @brief CN: 提交一个直读装配请求：字节由 IO 线程直接写入调用方预分配缓冲。
         *        EN: Submit a direct-to-buffer read: IO workers write bytes straight into a caller buffer.
         * @details CN: 面向 Direct-to-Staging 流送(如已映射 Staging 显存)。与 Submit 不同：
         *              - 不查缓存、不进入在途合并表、结果不登记弱表——每个请求都是一次性装配；
         *              - 文件长度超过 dest_capacity 时以 BufferTooSmall 失败；
         *              - 外部缓冲生命周期由发起方管理：LoadedFile 的引用只授予"字节已在
         *                缓冲中"这一事实的访问权，任何路径都不会 free 该缓冲。
         *              取消、失败或管理器销毁时，发起方负责回收自己分配的缓冲。
         *          EN: For Direct-to-Staging streaming (e.g. mapped staging memory). Unlike Submit:
         *              no cache lookup, no in-flight merge, no weak-table registration; every
         *              request is a one-shot payload. Oversized files fail with BufferTooSmall.
         *              The caller owns the buffer: the LoadedFile reference only grants access to
         *              "bytes already landed in the buffer" and no path ever frees it. On cancel,
         *              failure or manager destruction the caller reclaims its own buffer.
         * @param filename       CN: 文件名(直读键原样保留，仅用于报告)。 EN: File name, reported to consumers.
         * @param dest_buffer    CN: 外部目标缓冲起始地址，不得为空。 EN: External destination buffer; must not be null.
         * @param dest_capacity  CN: 外部缓冲容量(字节)，必须大于 0。 EN: Buffer capacity in bytes; must be > 0.
         * @param handler        CN: 结果接收者，可空(与 receiver 至少给一个)。 EN: Optional result handler; at least one of handler/receiver.
         * @param priority       CN: 读盘优先级。 EN: Read priority.
         * @param receiver       CN: 统一任务通知接收方，可空。 EN: Optional unified task receiver.
         * @param source         CN: 可选自定义字节源；管理器接管所有权，口径同 Submit。 EN: Optional byte source; manager owns it, same rule as Submit.
         * @param task_user_data CN: 原样回投的发起方上下文。 EN: Submitter context echoed unchanged.
         * @param expected_bytes CN: 预期字节数，用于在途预算限流。 EN: Expected bytes for in-flight budgeting.
         * @return CN: 请求编号，0 表示失败。 EN: Request identifier, or 0 on failure.
         */
        FileLoadId SubmitDirect(const OSString &filename,
                                void *dest_buffer,
                                int64 dest_capacity,
                                LoadedFileHandler *handler,
                                io::AsyncFilePriority priority=io::AsyncFilePriority::Normal,
                                thread::TaskReceiver *receiver=nullptr,
                                io::AsyncFileByteSource *source=nullptr,
                                void *task_user_data=nullptr,
                                uint64 expected_bytes=0);

        /**
         * @brief CN: 取消一个尚未回调的请求。
         *        EN: Cancel a request that has not been dispatched yet.
         * @return CN: false 表示编号无效(已回调或从未存在)。 EN: False if the identifier is unknown or already dispatched.
         * @note CN: 取消是协作式的：不能中断正在进行的系统读取，但即使读取已完成，
         *           回调也会被转成 OnFileCancelled。共享同一读盘的多个请求互不牵连：
         *           只有当分组内所有请求都被取消时才会真正取消底层读盘。
         *       EN: Cancellation is cooperative and cannot interrupt a blocking OS read, but a
         *           request that already finished is still reported as OnFileCancelled. Requests
         *           sharing one read are independent: the underlying read is cancelled only when
         *           every request in the group has been cancelled.
         */
        bool Cancel(FileLoadId id);

        /**
         * @brief CN: 在调用线程回调已完成的请求。
         *        EN: Dispatch completed requests on the calling thread.
         * @param max_dispatch CN: 本次最多回调的请求数。 EN: Maximum number of requests to dispatch this call.
         * @return CN: 本次实际回调的请求数。 EN: Number of requests actually dispatched.
         * @note CN: 每帧限量可把完成事件平滑摊到多帧，避免一次性提交过多 GPU 上传。
         *           共享同一读盘的请求各自计数，因此一次可能超过 max_dispatch 中剩余的名额。
         *       EN: Limiting per frame spreads completion work across frames, avoiding an upload burst.
         *           Requests sharing one read are counted individually, so one group may exceed the
         *           remaining budget of max_dispatch.
         */
        int Pump(int max_dispatch=0x7fffffff);

        /**
         * @brief CN: 释放待释放队列中引用计数仍为 0 的文件。
         *        EN: Release queued files whose reference count is still zero.
         * @return CN: 本次释放的文件数。 EN: Number of files released.
         * @note CN: 建议在帧末调用(使用方已放手引用之后)。
         *       EN: Call at frame end, after consumers have dropped their references.
         */
        int Collect();

        /**
         * @brief CN: 在已加载表中查找；命中时 RefInc，可复活待释放的文件。
         *        EN: Look up the loaded table; a hit RefIncs and thus resurrects pending-release files.
         * @return CN: 命中返回持有引用的指针，否则为空。 EN: A reference-holding pointer on hit, empty otherwise.
         * @note CN: 任意线程可调用。 EN: Safe to call from any thread.
         */
        LoadedFilePtr Find(const OSString &filename);

        /** @brief CN: 尚未回调完的请求数(仅调用线程)。 EN: Requests not yet dispatched (caller thread only). */
        int GetPendingJobCount()const{return jobs.GetCount();}

        /**
         * @brief CN: 取运行统计快照。
         *        EN: Take a statistics snapshot.
         * @note CN: 可在任意线程调用；内部会短暂持有 table_lock 取表长度。
         *       EN: Callable from any thread; internally holds table_lock briefly.
         */
        LoadedFileManagerStatistics GetStatistics();
    };//class LoadedFileManager
}//namespace hgl