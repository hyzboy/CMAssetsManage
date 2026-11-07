#include<hgl/asset/AssetCPUTaskPool.h>

#include<hgl/log/Log.h>
#include<hgl/time/Time.h>
#include<hgl/platform/CpuInfo.h>

DEFINE_LOGGER_MODULE(AssetCPU)
USE_MODULE_LOGGER(AssetCPU)

namespace hgl::asset
{
    namespace
    {
        constexpr uint AutoWorkerLimit = 8;
    }//namespace

    AssetCPUTaskPool::AssetCPUTaskPool()
        :pool(this)
    {
    }

    AssetCPUTaskPool::~AssetCPUTaskPool()
    {
        Stop(thread::ShutdownMode::Drain);

        // 池已停止后仍可能留下「已交付但无人认领」的成品。此处统一销毁，
        // 否则业务层从未调用 Collect 的那部分任务会泄漏。
        while(main_mailbox.TrySwap())
        {
            ValueArray<AssetCPUTask *> &batch=main_mailbox.GetReceive();

            for(int i=0;i<batch.GetCount();i++)
                delete batch[i];

            main_mailbox.ReleaseReceive();
        }

        main_mailbox.Close();
    }

    uint AssetCPUTaskPool::GetRecommendedWorkerCount()
    {
        CpuInfo info={};

        if(!GetCpuInfo(&info)||info.logical_core_count<=1)
            return(1);

        // 留出 2 个逻辑核心给主线程与 IO 线程：解析再快，抢不到核的主线程一样掉帧。
        uint count=info.logical_core_count>2?(info.logical_core_count-2):1;

        if(count>AutoWorkerLimit)
            count=AutoWorkerLimit;

        return(count);
    }

    bool AssetCPUTaskPool::Start(uint worker_count)
    {
        if(started.load()||stopped.load())
            return(false);

        if(worker_count==0)
            worker_count=GetRecommendedWorkerCount();

        if(worker_count==0)
            return(false);

        if(!pool.Start(worker_count))
            return(false);

        started.store(true);

        FLogInfo(u8"CPU 任务池启动：%u 个工作线程",worker_count);

        return(true);
    }

    bool AssetCPUTaskPool::Stop(thread::ShutdownMode mode)
    {
        if(stopped.load())
            return(true);

        if(!started.load())
        {
            stopped.store(true);
            return(true);
        }

        const bool result=pool.Stop(mode);

        started.store(false);
        stopped.store(true);

        // 刻意不关闭 main_mailbox：Stop 之后业务层仍需要 Collect() 才能拿回
        // 取消/已完成的成品并归还句柄。邮箱只在析构时关闭。

        FLogInfo(u8"CPU 任务池停止：已执行 %u，失败 %u，取消 %u",
                 executed.load(),failed.load(),cancelled.load());

        return(result);
    }

    void AssetCPUTaskPool::AccountPending(const int delta)
    {
        const uint cur = delta>=0
                       ? pending.fetch_add(static_cast<uint>(delta)) + static_cast<uint>(delta)
                       : pending.fetch_sub(static_cast<uint>(-delta)) - static_cast<uint>(-delta);

        uint peak=peak_pending.load();

        while(cur>peak&&!peak_pending.compare_exchange_weak(peak,cur))
        {
        }
    }

    bool AssetCPUTaskPool::PostToSink(AssetCPUTask *task,const uint32 sink_id)
    {
        AssetCPUTaskSink *sink=nullptr;

        {
            MutexLock guard(&sink_lock);

            AssetCPUTaskSink **found=sinks.GetValuePointer(sink_id);

            if(found)
                sink=*found;
        }

        if(!sink)
            return(false);

        // 回调期间不持锁：sink 实现可能立刻把任务再投回本池。
        sink->OnAssetCPUTaskReady(task);

        return(true);
    }

    void AssetCPUTaskPool::Deliver(AssetCPUTask *task)
    {
        if(!task)
            return;

        // 点对点中继：任务自带 receiver 时直接通知，跳过 sink 锁表与主线程邮箱。
        // 所有权随 event.task 移交接收方（约定：接收方负责最终 delete）。
        if(task->receiver)
        {
            thread::TaskEvent ev{};

            ev.task=task;
            ev.user_data=task->user_data;
            ev.task_kind=static_cast<uint32>(task->kind);
            ev.completed=static_cast<uint64>(task->elapsed_microseconds);

            switch(task->status)
            {
            case AssetCPUTaskStatus::Done:      ev.state=thread::TaskState::Done;      break;
            case AssetCPUTaskStatus::Failed:    ev.state=thread::TaskState::Failed;    break;
            case AssetCPUTaskStatus::Cancelled: ev.state=thread::TaskState::Cancelled; break;
            default:                            ev.state=thread::TaskState::Pending;   break;
            }

            task->receiver->OnTaskEvent(ev);

            AccountPending(-1);
            delivered_receiver.fetch_add(1);
            return;
        }

        if(task->result_affinity==ExecutionAffinity::Dedicated)
        {
            if(PostToSink(task,task->result_target_id))
            {
                AccountPending(-1);
                delivered_dedicated.fetch_add(1);
                return;
            }

            // 没有登记消费端：宁可回主线程，也不要把成品丢掉
            delivered_fallback.fetch_add(1);
        }

        if(main_mailbox.Post(task))
        {
            AccountPending(-1);
            delivered_main.fetch_add(1);
            return;
        }

        // 邮箱已关闭（只可能是析构期）：就地销毁，避免泄漏
        delete task;

        AccountPending(-1);
    }

    bool AssetCPUTaskPool::Submit(AssetCPUTask *task)
    {
        if(!task||!IsRunning())
            return(false);

        submitted.fetch_add(1);
        AccountPending(1);

        if(task->affinity==ExecutionAffinity::WorkerAny)
        {
            if(pool.Submit(task))
                return(true);

            // 队列拒绝：所有权原地退回调用方，统计也要跟着回退
            AccountPending(-1);
            submitted.fetch_sub(1);

            FLogError(u8"CPU 任务提交被拒绝");

            return(false);
        }

        // 不归本池的 CPU 工作线程执行：直接交到声明的执行端
        if(task->affinity==ExecutionAffinity::Dedicated
         &&PostToSink(task,task->target_id))
        {
            AccountPending(-1);
            delivered_dedicated.fetch_add(1);
            return(true);
        }

        // MainThread，或 Dedicated 但无执行端：有 receiver 就点对点中继，
        // 否则进主线程邮箱，由主线程自行执行
        if(task->receiver)
        {
            Deliver(task);
            return(true);
        }

        if(main_mailbox.Post(task))
        {
            AccountPending(-1);
            delivered_main.fetch_add(1);
            return(true);
        }

        delete task;

        AccountPending(-1);
        submitted.fetch_sub(1);

        return(false);
    }

    int AssetCPUTaskPool::Submit(AssetCPUTask **tasks,const int count)
    {
        if(!tasks||count<=0)
            return(0);

        int accepted=0;

        while(accepted<count&&Submit(tasks[accepted]))
            ++accepted;

        return(accepted);
    }

    int AssetCPUTaskPool::Collect(ValueArray<AssetCPUTask *> &out)
    {
        if(!main_mailbox.TrySwap())
            return(0);

        ValueArray<AssetCPUTask *> &batch=main_mailbox.GetReceive();
        const int count=batch.GetCount();

        if(count>0)
            out.Add(batch.GetData(),count);

        main_mailbox.ReleaseReceive();

        return(count);
    }

    bool AssetCPUTaskPool::RegisterSink(const uint32 target_id,AssetCPUTaskSink *sink)
    {
        if(!sink)
            return(false);

        MutexLock guard(&sink_lock);

        return(sinks.Add(target_id,sink));
    }

    bool AssetCPUTaskPool::UnregisterSink(const uint32 target_id)
    {
        MutexLock guard(&sink_lock);

        return(sinks.DeleteByKey(target_id));
    }

    int AssetCPUTaskPool::GetSinkCount() const
    {
        MutexLock guard(&sink_lock);

        return(sinks.GetCount());
    }

    AssetCPUTaskPoolStatistics AssetCPUTaskPool::GetStatistics() const
    {
        AssetCPUTaskPoolStatistics result;

        result.submitted=submitted.load();
        result.executed=executed.load();
        result.failed=failed.load();
        result.cancelled=cancelled.load();
        result.delivered_main=delivered_main.load();
        result.delivered_dedicated=delivered_dedicated.load();
        result.delivered_fallback=delivered_fallback.load();
        result.delivered_receiver=delivered_receiver.load();
        result.pending=static_cast<int32>(pending.load());
        result.peak_pending=static_cast<int32>(peak_pending.load());
        result.exec_microseconds=exec_microseconds.load();
        result.exec_samples=exec_samples.load();

        return(result);
    }

    void AssetCPUTaskPool::OnWork(const uint worker_index,AssetCPUTask *work)
    {
        if(!work)
            return;

        const uint64 start=GetUptimeUs();
        const bool ok=work->Execute();
        const uint64 cost=GetUptimeUs()-start;

        work->elapsed_microseconds=static_cast<int64>(cost);
        work->worker_index=worker_index;
        work->status=ok?AssetCPUTaskStatus::Done:AssetCPUTaskStatus::Failed;

        executed.fetch_add(1);
        exec_microseconds.fetch_add(cost);
        exec_samples.fetch_add(1);

        if(!ok)
            failed.fetch_add(1);

        Deliver(work);
    }

    void AssetCPUTaskPool::OnCancel(AssetCPUTask *work)
    {
        if(!work)
            return;

        work->status=AssetCPUTaskStatus::Cancelled;

        cancelled.fetch_add(1);

        // 取消的任务同样要回到业务层，否则调用方永远等不到这个句柄的结论
        Deliver(work);
    }
}//namespace hgl::asset
