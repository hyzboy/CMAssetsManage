#include<hgl/asset/AssetIODispatcher.h>

#include<hgl/log/Log.h>

USE_MODULE_LOGGER(AssetIO)

namespace hgl::asset
{
    bool AssetIODispatcher::Start(io::AsyncFileLoadHandler *handler,const uint workers_per_device)
    {
        if(started||stopped||!handler||workers_per_device==0)
            return false;

        downstream=handler;
        worker_per_device=workers_per_device;

        // 卷枚举失败不算致命：默认通道仍然能工作，只是退化为单通道串行。
        RefreshDevicesInternal();

        if(!FindChannel(AssetDeviceDefault))
        {
            FLogError(u8"AssetIODispatcher failed to create the default IO channel.");
            return false;
        }

        started=true;

        return true;
    }

    int AssetIODispatcher::RefreshDevicesInternal()
    {
        const int volume_count=router.Enumerate();

        int created=0;

        for(int i=0;i<router.GetVolumeCount();i++)
        {
            const PhysicalVolumeRouter::VolumeRecord *record=router.GetVolumeAt(i);

            if(!record)
                continue;

            if(!FindChannel(record->device_id)&&CreateChannel(record->device_id))
                ++created;
        }

        if(!FindChannel(AssetDeviceDefault)&&CreateChannel(AssetDeviceDefault))
            ++created;

        return volume_count<0?-1:created;
    }

    int AssetIODispatcher::RefreshDevices()
    {
        if(!started||stopped)
            return -1;

        return RefreshDevicesInternal();
    }

    AssetIODispatcher::Channel *AssetIODispatcher::FindChannel(const AssetDeviceID id)const
    {
        const int count=channels.GetCount();

        for(int i=0;i<count;i++)
        {
            Channel *channel=channels.At(i);

            if(channel&&channel->device_id==id)
                return channel;
        }

        return nullptr;
    }

    AssetIODispatcher::Channel *AssetIODispatcher::CreateChannel(const AssetDeviceID id)
    {
        if(!downstream||FindChannel(id))
            return nullptr;

        Channel *channel=channels.Create(id,static_cast<io::AsyncFileLoadHandler *>(this),worker_per_device);

        if(!channel)
        {
            FLogError(u8"AssetIODispatcher failed to allocate an IO channel for device %u.",id);
            return nullptr;
        }

        if(!channel->loader.Start(worker_per_device))
        {
            FLogError(u8"AssetIODispatcher failed to start %u IO worker(s) for device %u.",worker_per_device,id);

            // 启动失败就摘掉，避免后续请求投进一个永远不消费的队列。
            channels.Remove(channel);
            return nullptr;
        }

        return channel;
    }

    AssetDeviceID AssetIODispatcher::GetChannelDeviceAt(const int index)const
    {
        const Channel *channel=(index<0)?nullptr:channels.At(index);

        return channel?channel->device_id:AssetDeviceDefault;
    }

    uint AssetIODispatcher::GetChannelWorkerCount(const AssetDeviceID id)const
    {
        const Channel *channel=FindChannel(id);

        return channel?channel->worker_count:0;
    }

    bool AssetIODispatcher::Submit(io::AsyncFileLoadRequest *request,const io::AsyncFilePriority priority)
    {
        if(!request||!started||stopped)
            return false;

        Channel *channel=FindChannel(router.Resolve(request->filename));

        if(!channel)
            channel=FindChannel(AssetDeviceDefault);

        if(!channel)
            return false;

        return channel->loader.Submit(request,priority);
    }

    bool AssetIODispatcher::Submit(io::AsyncFileLoadRequest *request)
    {
        return Submit(request,io::AsyncFilePriority::Normal);
    }

    bool AssetIODispatcher::Stop(const thread::ShutdownMode mode)
    {
        if(!started||stopped)
            return true;

        bool result=true;

        const int count=channels.GetCount();

        for(int i=0;i<count;i++)
        {
            Channel *channel=channels.At(i);

            if(!channel)
                continue;

            if(!channel->loader.Stop(mode))
            {
                FLogError(u8"AssetIODispatcher failed to stop the IO channel of device %u.",channel->device_id);
                result=false;
            }
        }

        stopped=true;

        return result;
    }

    bool AssetIODispatcher::OnFileLoaded(const uint worker_index,io::AsyncFileLoadRequest *request)
    {
        if(!downstream)
            return false;

        return downstream->OnFileLoaded(worker_index,request);
    }

    void AssetIODispatcher::OnFileError(const uint worker_index,io::AsyncFileLoadRequest *request,const io::AsyncFileLoadError error)
    {
        if(downstream)
            downstream->OnFileError(worker_index,request,error);
    }

    void AssetIODispatcher::OnFileCancelled(const uint worker_index,io::AsyncFileLoadRequest *request)
    {
        if(downstream)
            downstream->OnFileCancelled(worker_index,request);
    }

    io::AsyncFileLoaderStatistics AssetIODispatcher::GetStatistics()
    {
        io::AsyncFileLoaderStatistics total;

        const int count=channels.GetCount();

        for(int i=0;i<count;i++)
        {
            Channel *channel=channels.At(i);

            if(!channel)
                continue;

            const io::AsyncFileLoaderStatistics one=channel->loader.GetStatistics();

            total.submitted   +=one.submitted;
            total.popped      +=one.popped;
            total.loaded      +=one.loaded;
            total.failed      +=one.failed;
            total.cancelled   +=one.cancelled;
            total.loaded_bytes+=one.loaded_bytes;
            total.pending     +=one.pending;

            total.queue_wait_microseconds+=one.queue_wait_microseconds;
            total.read_microseconds      +=one.read_microseconds;
            total.read_samples           +=one.read_samples;

            // 峰值不能相加，取各通道的最大值。
            if(one.peak_pending>total.peak_pending)
                total.peak_pending=one.peak_pending;
        }

        return total;
    }
}//namespace hgl::asset
