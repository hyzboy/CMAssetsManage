#pragma once

/**
 * 两级分流的具体任务实现。
 *
 * ```
 *   分流 A  GenericAssetDeserializeTask  ── 通用反序列化，全 CPU，WorkerAny
 *   分流 B  MetaProbeTask                ── 元信息快切，载荷归属由构造参数声明
 * ```
 *
 * ## 分流 A：为什么它能「通用」
 *
 * 凡是字段遍历由 `HGL_ASSET_STRUCT` 注解 + AssetGen 生成 `Serialize()` 的类型，
 * 解码路径就与具体资产类型无关：值树怎么读、字段怎么填，全由反射元数据驱动。
 * 于是「加一种资产」不需要加一行流水线代码 —— 这是本分流唯一的准入条件。
 *
 * ## 分流 B：为什么它必须存在
 *
 * 纹理要建 VkImage、几何要建 VBO、材质要写 SSBO。这些操作**不能**在 CPU 工作
 * 线程上做（缺上下文、且会把驱动锁带进工作线程）。分流 B 的做法是把工作拆成两段：
 *
 * ```
 *   Worker:  Init()  →  PayloadState::MetaOnly   （只读头部、算尺寸，毫秒级剥走）
 *      │  成品送去 payload_affinity 声明的线程
 *      ▼
 *   主线程:  LoadPayload()  →  Resident          （GPU 上传，微秒级提交）
 * ```
 *
 * 注意这里的「成品」是**任务对象本身**：`MetaProbeTask` 完成后由
 * `AssetCPUTaskPool` 按 `result_affinity` 送到目标线程，业务层在那里
 * `Collect()` 出来执行第二阶段。任务对象因此天然成为两级之间的信封。
 */

#include <hgl/asset/AssetCPUTask.h>
#include <hgl/asset/AssetObject.h>
#include <hgl/object/MetaObject.h>
#include <hgl/serialize/ArchiveFile.h>
#include <hgl/type/ValueArray.h>

namespace hgl::asset
{
    /**
     * 分流 A：把**已在 RAM 中**的资产字节通用反序列化进 `AssetObject`。
     *
     * ## 所有权
     *
     * - 字节缓冲：构造时**拷贝**进任务（一次线性 memcpy，Tier 1 的缓冲区随后可释放）。
     * - `target`：**借用**。调用方必须在 `Collect()` 拿到本任务之前保证对象存活，
     *   通常做法是「提交方持有 `SharedPtr`，在 Collect 回调处理完再释放」。
     *
     * ## 线程安全
     *
     * `Execute()` 只触碰 `target` 与自己的缓冲。同一个 `target` 不得被两个任务
     * 并发解码 —— 那是业务层的责任（一个资产一次只提交一次装载）。
     */
    class GenericAssetDeserializeTask : public AssetCPUTask
    {
        ValueArray<uint8> data;

        AssetObject *target = nullptr;
        serialize::ArchiveFormat format = serialize::ArchiveFormat::Auto;

        /// 构造期就能判定的失败（例如后缀无法解析），`Execute()` 直接转报
        AnsiString setup_error;

        void Init(AssetObject *obj, const uint8 *src, int size);

    public:

        /**
         * @param obj    目标资产（借用，非空）
         * @param src    字节缓冲（非空）
         * @param size   字节数（>0）
         * @param format 物理格式；`Auto` 视为构造失败
         */
        GenericAssetDeserializeTask(AssetObject *obj,
                                    const uint8 *src,
                                    int size,
                                    serialize::ArchiveFormat format);

        /**
         * @param obj      目标资产（借用，非空）
         * @param filename 资产文件名，用于按后缀解析物理格式并生成诊断文案
         * @param src      字节缓冲（非空）
         * @param size     字节数（>0）
         */
        GenericAssetDeserializeTask(AssetObject *obj,
                                    const OSString &filename,
                                    const uint8 *src,
                                    int size);

        bool Execute() override;

        AssetObject *GetTarget() const { return target; }
        int GetDataSize() const { return data.GetCount(); }
        serialize::ArchiveFormat GetFormat() const { return format; }
    };//class GenericAssetDeserializeTask

    /**
     * 分流 B 第一段：元信息快切。
     *
     * 工作线程只调用 `MetaObject::Init()`（走类内的 `OnInitMeta()`），把
     * 「需要什么尺寸/什么格式」这类纯 CPU 判断从主线程剥走；载入 GPU 资源的
     * `OnLoadPayload()` **不在**本任务里发生。
     *
     * ## 对派生类的要求
     *
     * `OnInitMeta()` 必须是纯 CPU、无全局可变状态、不触碰 GPU / 窗口 / 渲染设备。
     * 一旦某个资产的 meta 阶段做不到这点，它就不该走分流 B，而应直接声明
     * `ExecutionAffinity::MainThread`。
     *
     * ## 所有权
     *
     * `target` 是借用的 `RefObject`；提交方应持有引用直到 `Collect()` 处理完毕。
     */
    class MetaProbeTask : public AssetCPUTask
    {
        hgl::MetaObject *target = nullptr;

    public:

        /**
         * @param obj              目标对象（借用，非空）
         * @param payload_affinity 第二阶段（`LoadPayload`）的归属线程
         * @param payload_target_id `payload_affinity == Dedicated` 时的目标标识
         */
        MetaProbeTask(hgl::MetaObject *obj,
                      ExecutionAffinity payload_affinity = ExecutionAffinity::MainThread,
                      uint32 payload_target_id = 0);

        bool Execute() override;

        hgl::MetaObject *GetTarget() const { return target; }
    };//class MetaProbeTask
}//namespace hgl::asset
