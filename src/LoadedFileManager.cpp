#include<hgl/asset/LoadedFileManager.h>

namespace hgl
{
    LoadedFileManager::LoadedFileManager()
    {
    }

    LoadedFileManager::~LoadedFileManager()
    {
        Stop(thread::ShutdownMode::CancelPending);

        // 1. 丢弃所有未回调的请求与在读分组；它们持有的引用随之放开，归零的文件进入待释放队列
        {
            ValueArray<Job *> all;
            jobs.GetValueArray(all);

            for(int i=0;i<all.GetCount();i++)
                delete all[i];

            jobs.Clear();
            ready.Clear();

            if(done_mailbox.TrySwap())              // 已完成但还没被 Pump 取走的分组
            {
                ValueArray<ReadGroup *> &batch=done_mailbox.GetReceive();

                for(int i=0;i<batch.GetCount();i++)
                {
                    ReadGroup *group=batch[i];

                    reading.DeleteByKey(group->file->path_key);    // 同一个组可能还在在途表里，先摘掉避免重复析构
                    delete group;
                }

                batch.Clear();
                done_mailbox.ReleaseReceive();
            }

            ValueArray<ReadGroup *> groups;
            reading.GetValueArray(groups);           // 读盘被取消、永远不会再有回调的分组

            for(int i=0;i<groups.GetCount();i++)
                delete groups[i];

            reading.Clear();

            ReadGroup *group=nullptr;

            while(ready_groups.Pop(group))
                delete group;

            ready_groups.Clear();
        }

        // 2. 解除所有文件与本管理器的关联；计数为 0 的立即销毁，其余等持有者放手后自行销毁
        ValueArray<LoadedFile *> dead;

        {
            MutexLock guard(&table_lock);

            for(int i=0;i<pending_release.GetCount();i++)
            {
                LoadedFile *file=pending_release[i];

                if(file->GetRefCount()==0)
                    dead.Add(file);
                else
                    file->owner=nullptr;

                file->in_pending=false;
            }

            pending_release.Clear();

            ValueArray<LoadedFile *> alive;
            files.GetValueArray(alive);

            for(int i=0;i<alive.GetCount();i++)
                alive[i]->owner=nullptr;

            files.Clear();
        }

        for(int i=0;i<dead.GetCount();i++)
            ReleaseFile(dead[i]);
    }

    bool LoadedFileManager::Start(uint io_worker_count)
    {
        if(started)
            return(false);

        started=loader.Start(this,io_worker_count);
        return started;
    }

    bool LoadedFileManager::Stop(thread::ShutdownMode mode)
    {
        const bool result=loader.Stop(mode);

        done_mailbox.Close();                       // 已接受的结果仍可被 Pump 取走
        return result;
    }

    // ---------------------------------------------------------------- IO 线程侧

    void LoadedFileManager::PostGroup(ReadGroup *group,Outcome outcome,io::AsyncFileLoadError error)
    {
        group->outcome=outcome;
        group->error=error;
        FireGroupRelay(group,outcome,error);
        done_mailbox.Post(group);
    }

    bool LoadedFileManager::OnFileLoaded(uint,io::AsyncFileLoadRequest *request)
    {
        ReadGroup *group=static_cast<ReadGroup *>(request->user_data);

        if(!group)
            return(false);

        group->outcome=Outcome::Loaded;
        FireGroupRelay(group,Outcome::Loaded,io::AsyncFileLoadError::None);
        return done_mailbox.Post(group);
    }

    void LoadedFileManager::OnFileError(uint,io::AsyncFileLoadRequest *request,io::AsyncFileLoadError error)
    {
        ReadGroup *group=static_cast<ReadGroup *>(request->user_data);

        if(group)
            PostGroup(group,Outcome::Failed,error);
    }

    void LoadedFileManager::OnFileCancelled(uint,io::AsyncFileLoadRequest *request)
    {
        ReadGroup *group=static_cast<ReadGroup *>(request->user_data);

        if(group)
            PostGroup(group,Outcome::Cancelled);
    }

    // ---------------------------------------------------------------- TaskReceiver 中继

    void LoadedFileManager::FireJobRelay(Job *job,LoadedFile *file,Outcome outcome,const uint64 bytes,io::AsyncFileLoadError error)
    {
        if(!job||!job->receiver||job->relay_fired)
            return;

        job->relay_fired=true;

        thread::TaskEvent event;
        event.task_id=(uint64)job->id;
        event.task_kind=AssetIOTaskKind;
        event.user_data=job->task_user_data;
        event.completed=bytes;
        event.total=bytes;
        event.task=nullptr;

        switch(outcome)
        {
            case Outcome::Loaded:
                event.state=thread::TaskState::Done;
                if(file)
                {
                    LoadedFilePtr ref(file);          // 多持一次引用
                    event.task=ref.Detach();          // 交给接收方直接接管，用完 RefDec 一次
                }
                break;

            case Outcome::Failed:
                event.state=thread::TaskState::Failed;
                event.error_code=(int32)error;
                break;

            default:
                event.state=thread::TaskState::Cancelled;
                break;
        }

        job->receiver->OnTaskEvent(event);
    }

    void LoadedFileManager::FireGroupRelay(ReadGroup *group,Outcome outcome,io::AsyncFileLoadError error)
    {
        const uint64 bytes=(outcome==Outcome::Loaded)?(uint64)group->file->request.GetDataSize():0;

        MutexLock guard(&group->waiters_lock);

        if(outcome==Outcome::Loaded)
            group->file->GetMeta().source_size=bytes;   // 先把元信息写好，接收方拿到引用即可读

        for(int i=0;i<group->waiters.GetCount();i++)
        {
            Job *job=group->waiters[i];

            if(job&&job->receiver&&!job->relay_fired)
                FireJobRelay(job,group->file.Get(),outcome,bytes,error);
        }
    }

    // ---------------------------------------------------------------- 引用归零 / 释放

    void LoadedFileManager::OnFileRefZero(LoadedFile *file)
    {
        MutexLock guard(&table_lock);

        if(file->in_pending)                        // 已在队列里：不重复入队
            return;

        file->in_pending=true;
        pending_release.Add(file);
    }

    void LoadedFileManager::ReleaseFile(LoadedFile *file)
    {
        file->Release();                            // Object 析构前必须 Release，否则会报违规日志
        delete file;
    }

    int LoadedFileManager::Collect()
    {
        ValueArray<LoadedFile *> dead;

        {
            MutexLock guard(&table_lock);

            for(int i=0;i<pending_release.GetCount();i++)
            {
                LoadedFile *file=pending_release[i];

                file->in_pending=false;

                if(file->GetRefCount()!=0)          // 排队期间被复活，放过
                    continue;

                LoadedFile *registered=nullptr;

                if(files.Get(file->path_key,registered)&&registered==file)
                    files.DeleteByKey(file->path_key);      // 摘表后 Find 再也找不到它，锁外释放是安全的

                dead.Add(file);
            }

            pending_release.Clear();
            released_files+=(uint64)dead.GetCount();
        }

        for(int i=0;i<dead.GetCount();i++)
            ReleaseFile(dead[i]);

        return dead.GetCount();
    }

    LoadedFilePtr LoadedFileManager::Find(const OSString &filename)
    {
        MutexLock guard(&table_lock);

        LoadedFile *file=nullptr;

        if(!files.Get(MakeLoadedFileKey(filename),file)||!file)
            return LoadedFilePtr();

        return LoadedFilePtr(file);                 // RefInc：计数为 0 的待释放文件在此复活
    }

    // ---------------------------------------------------------------- 提交 / 回调

    FileLoadId LoadedFileManager::Submit(const OSString &filename,LoadedFileHandler *handler,io::AsyncFilePriority priority,
                                         thread::TaskReceiver *receiver,io::AsyncFileByteSource *source,
                                         void *task_user_data,uint64 expected_bytes)
    {
        if(!started||filename.IsEmpty()||(!handler&&!receiver))
            return(0);

        // 1. 命中已加载表：不读盘，下一次 Pump 直接回调
        LoadedFilePtr cached=Find(filename);

        if(cached)
        {
            delete source;                          // 缓存命中用不上字节源：管理器拥有，当场删除

            Job *job=new Job;

            job->id=next_id++;
            job->handler=handler;
            job->receiver=receiver;
            job->task_user_data=task_user_data;
            job->file=cached;

            ++stats.submitted;
            ++stats.cache_hits;

            jobs.Add(job->id,job);
            ready.Push(job);
            return job->id;
        }

        const OSString key=MakeLoadedFileKey(filename);

        // 2. 同一路径已经在读：挂到在途分组上，不重复读盘
        ReadGroup *group=nullptr;

        if(reading.Get(key,group)&&group)
        {
            delete source;                          // 合并在途读盘：本次字节源不会被消费

            Job *job=new Job;

            job->id=next_id++;
            job->handler=handler;
            job->receiver=receiver;
            job->task_user_data=task_user_data;
            job->group=group;

            {
                MutexLock guard(&group->waiters_lock);

                if(handler)
                    group->handler_present=true;

                group->waiters.Add(job);
            }

            ++stats.submitted;
            ++stats.merged_in_flight;

            jobs.Add(job->id,job);
            return job->id;
        }

        // 3. 新读盘：一个分组承载本次读盘及其所有等待者
        LoadedFile *file=new LoadedFile(filename);

        file->owner=this;

        if(!file->Init())
        {
            delete source;
            ReleaseFile(file);
            return(0);
        }

        group=new ReadGroup;
        group->file=LoadedFilePtr(file);            // 计数 0→1：读盘期间由分组持有
        group->source=source;                       // 分组接管字节源所有权
        group->handler_present=(handler!=nullptr);

        Job *job=new Job;
        job->id=next_id++;
        job->handler=handler;
        job->receiver=receiver;
        job->task_user_data=task_user_data;
        job->group=group;

        group->waiters.Add(job);
        jobs.Add(job->id,job);

        file->request.user_data=group;
        file->request.byte_source=source;
        file->request.expected_bytes=expected_bytes;
        reading.Add(key,group);

        if(!loader.Submit(&file->request,priority))
        {
            reading.DeleteByKey(key);
            jobs.DeleteByKey(job->id);
            delete job;

            delete group;                           // 析构里删字节源；引用归零 → 入待释放队列，等 Collect
            return(0);
        }

        ++stats.submitted;
        return job->id;
    }

    bool LoadedFileManager::Cancel(FileLoadId id)
    {
        Job *job=nullptr;

        if(!jobs.Get(id,job)||!job)
            return(false);

        job->cancel_requested=true;

        ReadGroup *group=job->group;

        if(!group)                                  // 缓存命中：无需打断读盘，Pump 时转成取消
            return(true);

        for(int i=0;i<group->waiters.GetCount();i++)
            if(!group->waiters[i]->cancel_requested)
                return(true);                       // 组内还有别的请求要这份数据，不能打断读盘

        group->file->request.Cancel();              // 协作式：读盘线程在检查点响应
        return(true);
    }

    void LoadedFileManager::DispatchOne(Job *job,const OSString &filename,LoadedFile *publish,Outcome outcome,io::AsyncFileLoadError error)
    {
        jobs.DeleteByKey(job->id);

        FileLoadTicket ticket;
        ticket.id=job->id;
        ticket.filename=&filename;

        if(outcome==Outcome::Loaded&&job->cancel_requested)
            outcome=Outcome::Cancelled;

        // 中继还没发过(缓存命中、迟到的合并、扫描后才挂进的请求)：这次在调用线程补发
        if(job->receiver&&!job->relay_fired)
        {
            LoadedFile *relay_file=publish?publish:job->file.Get();
            const uint64 bytes=(outcome==Outcome::Loaded&&relay_file)?(uint64)relay_file->request.GetDataSize():0;

            FireJobRelay(job,relay_file,outcome,bytes,error);
        }

        if(job->handler)
        {
            if(outcome==Outcome::Loaded&&publish)
            {
                LoadedFilePtr use(publish);         // 在锁外 RefInc，避免与归零回调互相阻塞

                job->handler->OnFileLoaded(ticket,use.Get());
                ++stats.dispatched_loaded;

                use.Reset();                        // 用完即放：没人再持有的文件归零入队
            }
            else
            {
                switch(outcome)
                {
                    case Outcome::Failed:   job->handler->OnFileFailed(ticket,error);
                                            ++stats.dispatched_failed;
                                            break;

                    default:                job->handler->OnFileCancelled(ticket);
                                            ++stats.dispatched_cancelled;
                                            break;
                }
            }
        }

        delete job;                                 // Job 里的引用随之放开
    }

    int LoadedFileManager::DispatchGroup(ReadGroup *group)
    {
        LoadedFile *file=group->file.Get();

        reading.DeleteByKey(file->path_key);        // 先摘在途表：回调里再 Submit 同一路径会走缓存命中

        Outcome outcome=group->outcome;
        io::AsyncFileLoadError error=group->error;

        LoadedFilePtr publish;

        if(outcome==Outcome::Loaded)
        {
            file->GetMeta().source_size=(uint64)file->request.GetDataSize();

            bool relay_only=false;

            {
                MutexLock guard(&group->waiters_lock);
                relay_only=!group->handler_present;
            }

            // 纯 receiver 请求：读盘线程已中继 Done 事件，解析由接收方在自己的线程做；
            // 跳过主线程解析与发布表登记，文件引用随事件转交。
            if(!relay_only&&!file->LoadPayload())
            {
                outcome=Outcome::Failed;
                error=io::AsyncFileLoadError::ReadFailed;
            }
            else if(!relay_only)
            {
                MutexLock guard(&table_lock);

                LoadedFile *existing=nullptr;

                // 同名文件已被别的路径抢先登记：沿用已登记的那份，本份随分组一起走进待释放队列
                if(files.Get(file->path_key,existing)&&existing&&existing!=file)
                    publish=LoadedFilePtr(existing);
                else
                {
                    files.Add(file->path_key,file);
                    publish=group->file;
                }
            }
        }

        const int count=group->waiters.GetCount();

        for(int i=0;i<count;i++)
            DispatchOne(group->waiters[i],file->GetFilename(),publish.Get(),outcome,error);    // 共享同一份数据

        delete group;
        return count;
    }

    int LoadedFileManager::Pump(int max_dispatch)
    {
        if(done_mailbox.TrySwap())
        {
            ValueArray<ReadGroup *> &batch=done_mailbox.GetReceive();

            for(int i=0;i<batch.GetCount();i++)
                ready_groups.Push(batch[i]);

            done_mailbox.ReleaseReceive();
        }

        int count=0;

        while(count<max_dispatch)
        {
            Job *job=nullptr;

            if(ready.Pop(job)&&job)                 // 缓存命中优先：不读盘，处理成本极低
            {
                DispatchOne(job,job->file->GetFilename(),job->file.Get(),Outcome::Loaded,io::AsyncFileLoadError::None);
                ++count;
                continue;
            }

            ReadGroup *group=nullptr;

            if(!ready_groups.Pop(group)||!group)
                break;

            count+=DispatchGroup(group);            // 同一读盘的多个请求共享一次读盘，可能超过剩余名额
        }

        return count;
    }

    LoadedFileManagerStatistics LoadedFileManager::GetStatistics()
    {
        LoadedFileManagerStatistics result=stats;

        {
            MutexLock guard(&table_lock);

            result.alive_files=(uint64)files.GetCount();
            result.pending_release=(uint64)pending_release.GetCount();
            result.released_files=released_files;
        }

        result.io=loader.GetStatistics();
        return result;
    }
}//namespace hgl
