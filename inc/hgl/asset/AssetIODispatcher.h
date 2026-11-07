#pragma once

#include<hgl/asset/PhysicalVolumeRouter.h>

#include<hgl/io/AsyncFileLoader.h>
#include<hgl/thread/ShutdownMode.h>
#include<hgl/type/ManagedArray.h>

namespace hgl::asset
{
    /**
     * 按物理设备分流的异步文件读取调度器。
     *
     * 为什么需要它：io::AsyncFileLoader 是一个"全局队列 + N 个 worker"的模型，所有
     * 请求不管落在哪块磁盘上都在同一个队列里排队。这在多设备机器上是错的 —— 一块
     * 机械盘上并行读两个文件只会互相抢磁头，而两块不同磁盘之间却完全应该并行。
     *
     * 本类的做法：用 PhysicalVolumeRouter 把请求的路径映射到物理设备编号，每个设备
     * 一条独立通道(一个 AsyncFileLoader + 一个 worker)，同设备内严格串行，不同设备
     * 之间完全并行。请求完成后统一回调到同一个下游 AsyncFileLoadHandler，因此对上层
     * 的可见性完全不变 —— LoadedFileManager 只换了 loader 的类型，其余代码不动。
     *
     * 线程契约：
     *  - Start / Stop / RefreshDevices / Submit 必须由同一个线程调用(通常是主线程)；
     *  - 下游 handler 的三个回调在 IO worker 线程上执行，必须自己转投到目标线程；
     *  - GetStatistics 可以在任意线程调用(各通道返回的是各自一致的快照)。
     *
     * 通道与降级：Start() 会为枚举到的每个卷各建一条通道，另外永远保留一条默认通道
     * (AssetDeviceDefault)，未匹配到任何挂载点的路径全部走默认通道。因此即使卷枚举
     * 失败、路径是相对路径或包内虚拟路径，功能也不会丢，只是退化为单通道串行。
     */
    class AssetIODispatcher:public io::AsyncFileLoadHandler
    {
        struct Channel
        {
            AssetDeviceID       device_id;
            uint                worker_count;
            io::AsyncFileLoader loader;

            Channel(const AssetDeviceID id,io::AsyncFileLoadHandler *handler,const uint workers)
                :device_id(id),worker_count(workers),loader(handler){}
        };

    private:

        PhysicalVolumeRouter        router;
        ManagedArray<Channel>       channels;
        io::AsyncFileLoadHandler *  downstream      =nullptr;
        uint                        worker_per_device=1;
        bool                        started         =false;
        bool                        stopped         =false;

    private:

        Channel *FindChannel(AssetDeviceID id)const;
        Channel *CreateChannel(AssetDeviceID id);
        int RefreshDevicesInternal();

        bool OnFileLoaded   (uint worker_index,io::AsyncFileLoadRequest *request)override;
        void OnFileError    (uint worker_index,io::AsyncFileLoadRequest *request,io::AsyncFileLoadError error)override;
        void OnFileCancelled(uint worker_index,io::AsyncFileLoadRequest *request)override;

    public:

        AssetIODispatcher()=default;
        ~AssetIODispatcher()override=default;

        NO_COPY_NO_MOVE(AssetIODispatcher)

        /**
         * 启动调度器：枚举物理卷，为每个卷建一条通道，再建默认通道。
         *
         * @param handler            下游完成回调，不可为空。
         * @param workers_per_device 每个设备的 IO worker 数。默认 1 —— 同一设备上更多
         *                           worker 通常只会降低吞吐；确实需要(如 NVMe)才调大。
         * @return 至少默认通道启动成功则返回 true。
         * @note 只能成功一次；Stop 之后不可重启。
         */
        bool Start(io::AsyncFileLoadHandler *handler,uint workers_per_device=1);

        /**
         * 停止全部通道。
         *
         * @param mode Drain 表示排空已接受请求，CancelPending 表示取消尚未开始的请求。
         * @return 全部通道都正常停止返回 true。
         */
        bool Stop(thread::ShutdownMode mode=thread::ShutdownMode::Drain);

        /**
         * 重新枚举物理卷，为新增的卷补建通道(热插拔)。
         *
         * @return 本次新增的通道数；负数表示枚举失败。
         * @note 只增不减：已存在的通道(及在途请求)不受影响，因此可在运行期安全调用。
         *       已拔出的卷不会消失，它的后续请求仍然可用(失败由下游按读取错误处理)。
         */
        int RefreshDevices();

        /**
         * 提交一个读取请求，按路径自动分流到对应设备的通道。
         *
         * @param request  调用方持有的请求，生命周期要求与 io::AsyncFileLoader 一致。
         * @param priority 优先级，数值越小越高。
         * @return 成功入队返回 true；未启动/已停止/入队失败返回 false。
         */
        bool Submit(io::AsyncFileLoadRequest *request,io::AsyncFilePriority priority);
        bool Submit(io::AsyncFileLoadRequest *request);

        /** 已建立的通道数(含默认通道)。 */
        int GetChannelCount()const{return channels.GetCount();}

        /** 第 index 条通道的设备编号；越界返回 AssetDeviceDefault。 */
        AssetDeviceID GetChannelDeviceAt(int index)const;

        /** 某设备通道的 worker 数；通道不存在返回 0。 */
        uint GetChannelWorkerCount(AssetDeviceID id)const;

        /** 路径解析出的设备编号(调试/测试用)。 */
        AssetDeviceID ResolveDevice(const OSString &path)const{return router.Resolve(path);}

        /** 只读的路由器视图(调试/测试用)。 */
        const PhysicalVolumeRouter &GetRouter()const{return router;}

        /**
         * 可写路由器：供上层手工挂载设备(包、虚拟目录)。改动后需调用 RefreshDevices()
         * 才会补建对应通道；未 Refresh 前该设备的请求会退化到默认通道。
         */
        PhysicalVolumeRouter &GetRouter(){return router;}

        bool IsRunning()const{return started&&!stopped;}

        /**
         * 全部通道的统计合计。
         *
         * @note 各项计数、字节数与耗时都是各通道之和；peak_pending 取各通道最大值
         *       (峰值不可相加)。
         */
        io::AsyncFileLoaderStatistics GetStatistics();
    };
}//namespace hgl::asset
