# 第三方源码与授权说明

本仓库保留当前数据服务实际使用的第三方源码及原有版权声明。各组件适用其自身的授权条款；没有为来源未明确授权的代码额外指定开源许可证。

| 组件 | 用途 | 来源与声明 |
|---|---|---|
| equinor/segyio | SEG-Y 文件读取 | [上游](https://github.com/equinor/segyio)、[许可证](vendor/segyio/License.md)、[本地补丁](vendor/segyio/PATCHES.md) |
| FreeXL 2.0.0 | 旧版 XLS/BIFF 解析 | [来源及兼容补丁说明](vendor/freexl/NOTICE.txt)、[原始声明](vendor/freexl/freexl-2.0.0/COPYING)、[LGPL 2.1](vendor/freexl/LGPL-2.1.txt) |
| GLM | 地震引擎数学类型 | [版权与许可证](vendor/glm/copying.txt) |
| SBM 地震引擎 | 无界面地震访问基础代码 | [来源](vendor/sbm/README.md)、[补丁记录](vendor/sbm/PATCHES.md) |
| paleo ZIP 读取器 | Office ZIP/XML 内容读取 | [来源声明](vendor/paleo_io/NOTICE.txt) |

Qt、zlib 和 FreeXL 所需的 libiconv 由开发或运行环境提供，未将其安装目录、二进制库或工具链提交到 Git。

为满足“仓库只存代码”的要求，FreeXL 上游随附的电子表格、图片和原始压缩包未提交。解析库源码、构建文件、补丁及授权声明均予以保留。项目自己的回归测试在临时目录中由代码生成小型合成输入，不依赖真实井资料或地震体。
# Embedding patch

The copied `vendor/paleo_io/ziparchive.{h,cpp}` implementation retains its source
and license notices. Its namespace is changed to `paleo::data::zip` to avoid
colliding with the host workstation's separately maintained ZIP implementation.
When embedded, a host can supply its compatible seismic engine target and headers
instead of linking two copies. See `docs/workstation-embedding.md`.
