#pragma once

/**
 * 资产统一取流入口。
 *
 * ── 为什么必须有这一层 ────────────────────────────────────────────────────
 * 引入包裹（`pack(<name>)`）之后，「一个资产字节从哪里读」不再只有一个答案：
 * 可能是挂载目录、也可能是将来的打包文件。如果各 loader 继续自己调
 * `filesystem::FileCanRead` / `FileExist` / `new FileInputStream`，那么从目录
 * 切换到真实包裹文件时，凡是绕过包裹的路径都会静默失效 —— 而且很难定位。
 *
 * 因此**所有需要读取资产字节的代码都必须走这里**，而不是直接摸 filesystem。
 *
 * ── 分派规则 ──────────────────────────────────────────────────────────────
 *   · `source == Pack`      → 按 `pack_name` 查挂载表，再依次尝试：
 *       1. 定位段是 `id:<32 hex>` ⇒ 按身份寻址（**不回落**）；
 *       2. 否则归一化为包裹内路径，推导出 (类型, 逻辑路径) ⇒ 按索引寻址；
 *          后端有索引却查不到 ⇒ **当场失败**，不退回「按路径摸文件」；
 *       3. 推导不出类型（非资产原始文件）⇒ 按调用方给的路径直通。
 *   · `source == File`      → 直接当本地文件打开
 *   · `source == Unknown`   → 也按本地文件尝试（兼容裸路径与历史写法）
 *   · `builtin` / `rt`      → 不是字节流来源，返回 nullptr
 *
 * 路径规范化（拒绝 `..`、统一分隔符）在包裹分支里完成，调用方不必预先处理。
 *
 * ── 为什么第 2 步不回落 ───────────────────────────────────────────────────
 *
 * 「索引查不到就试着按文件路径打开」在开发期（目录包裹）能跑，在发行期
 * （打包文件）必然失败 —— 于是变成「开发期正常、出包后炸」的经典缺陷。
 * 更糟的是它会掩盖真正的错误：引用写错了本该立刻报错，却被回落到
 * 「某个恰好同名同后缀的文件」上，读出一份不该读的数据。
 * 因此有索引就必须走索引；只有后端明确声明「我没有索引」时才退回路径访问。
 */

#include<hgl/asset/AssetURI.h>

namespace hgl
{
    namespace io
    {
        class InputStream;
    }//namespace io

    namespace asset
    {
        /**
         * 按 URI 打开只读字节流。
         *
         * @param uri      已经解析好的资产 URI
         * @param out_error 失败时写入原因（可为 nullptr）
         * @return 调用方负责 delete；失败返回 nullptr
         */
        io::InputStream *OpenAssetStream(const AssetURI &uri,AnsiString *out_error=nullptr);

        /**
         * 便捷重载：文本 URI 就地解析后取流。
         *
         * @param uri_text     原始 URI 文本
         * @param default_type 文本里省略类型段时补上的类型（只影响诊断，不影响取流）
         * @param out_error    失败时写入原因（可为 nullptr）
         */
        io::InputStream *OpenAssetStream(const OSString &uri_text,
                                         AssetType default_type=AssetType::Unknown,
                                         AnsiString *out_error=nullptr);
    }//namespace asset
}//namespace hgl
