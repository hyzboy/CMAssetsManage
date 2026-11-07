#include <hgl/asset/AssetCPUTasks.h>

#include <hgl/type/MemoryUtil.h>
#include <hgl/utf.h>

namespace hgl::asset
{
    // ── GenericAssetDeserializeTask ─────────────────────────────────────────

    void GenericAssetDeserializeTask::Init(AssetObject *obj, const uint8 *src, int size)
    {
        if (!obj)
        {
            setup_error = "目标资产为空";
            return;
        }

        if (!src || size <= 0)
        {
            setup_error = "资产字节缓冲无效";
            return;
        }

        target = obj;

        // 一次线性拷贝：Tier 1 的缓冲区随后即可归还，解码不再需要那条 IO 通道
        data.Add(src, size);
    }

    GenericAssetDeserializeTask::GenericAssetDeserializeTask(AssetObject *obj,
                                                             const uint8 *src,
                                                             int size,
                                                             serialize::ArchiveFormat fmt)
        : AssetCPUTask(AssetCPUTaskKind::GenericDeserialize)
    {
        Init(obj, src, size);

        if (fmt == serialize::ArchiveFormat::Auto)
        {
            if (setup_error.IsEmpty())
                setup_error = "未指定物理格式（不能为 Auto）";
        }
        else
            format = fmt;
    }

    GenericAssetDeserializeTask::GenericAssetDeserializeTask(AssetObject *obj,
                                                             const OSString &filename,
                                                             const uint8 *src,
                                                             int size)
        : AssetCPUTask(AssetCPUTaskKind::GenericDeserialize)
    {
        Init(obj, src, size);

        serialize::ArchiveFormat resolved = serialize::ArchiveFormat::Auto;

        if (!serialize::ResolveArchiveFormatByExtension(filename, resolved))
        {
            if (setup_error.IsEmpty())
            {
                const U8String u8_name = ToU8String(filename);

                setup_error = AnsiString("无法从文件名判定格式: ")
                            + AnsiString(reinterpret_cast<const char *>(u8_name.c_str()));
            }

            return;
        }

        format = resolved;
    }

    bool GenericAssetDeserializeTask::Execute()
    {
        if (!setup_error.IsEmpty())
        {
            error = setup_error;
            return false;
        }

        AnsiString detail;

        if (!serialize::LoadAssetObjectFromMemory(*target,
                                                  data.GetData(),
                                                  static_cast<size_t>(data.GetCount()),
                                                  format,
                                                  &detail))
        {
            error = detail;
            return false;
        }

        return true;
    }

    // ── MetaProbeTask ───────────────────────────────────────────────────────

    MetaProbeTask::MetaProbeTask(hgl::MetaObject *obj,
                                 ExecutionAffinity payload_affinity,
                                 uint32 payload_target_id)
        : AssetCPUTask(AssetCPUTaskKind::MetaProbe)
        , target(obj)
    {
        // 第一段永远是纯 CPU，跑在任意工作线程上
        affinity = ExecutionAffinity::WorkerAny;

        // 第二段（GPU 上传）的归属由提交方声明
        result_affinity = payload_affinity;
        result_target_id = payload_target_id;
    }

    bool MetaProbeTask::Execute()
    {
        if (!target)
        {
            error = "目标对象为空";
            return false;
        }

        if (target->IsReleased())
        {
            error = "目标对象已释放";
            return false;
        }

        if (!target->Init())
        {
            error = "元信息初始化失败";
            return false;
        }

        return true;
    }
}//namespace hgl::asset
