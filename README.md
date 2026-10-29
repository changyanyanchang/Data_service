# Data Service · 多源数据服务原型

独立的 C++20 数据服务库和 Qt6 桌面演示程序。每个工区使用一个 SQLite 数据库，统一管理单井资料、地震资料、地质图件和来源信息，可通过公开 C++ 接口接入其他应用。

仓库只提供源码、构建脚本、说明和测试生成器。**不包含任何工区数据库、测井文件、地震体、工作簿、图片、报告或导出结果。** 测试中的输入由代码在临时目录生成；使用自己的资料时，请放在仓库外。

## 已实现的范围

- 测井曲线、地层/岩性分层、样品表、井口坐标、测斜、时深关系、岩心照片路径与附件关联。
- CSV/LAS、SpreadsheetML XML、XLS/XLSX、部分 Petrel 文本格式、地震层位文本和图件元信息解析。
- 每文件事务、重复检查、来源追溯、质量标记、人工修改、明确重导入、重开和数据库备份。
- 基于 segyio 的有界文件检查、单道、INLINE/XLINE、时间切片和 XY 路径剖面；大地震体始终保持外部路径。
- 原始分辨率的剖面/切片、多点坐标剖面墙、1:1 缩放和原尺寸 PNG 导出；详情见 [高清地震指南](docs/hd-seismic.md)。
- 时间切片支持 IL/XL 范围、分块显示、相邻时间缓存和 1/2/4 线程；可比较旧通道、连续缓冲及 Windows 映射读取，见 [切片性能指南](docs/time-slice-performance.md)。
- 对返回振幅执行去均值、固定增益、AGC、限幅和统计，原文件保持不变。
- 目录导入支持 1～4 个准备线程，界面提供 1/2/4 选项，默认 2 个；数据库按原文件顺序发布。
- 界面展示 C++ 调用代码、返回对象摘要与曲线/剖面图；提供十一个可编译入门案例。

## 三层结构

| 层 | 目录 | 职责 |
|---|---|---|
| 文件解析 | `src/parsers` | 文件转换为普通数据对象及问题列表，不依赖 SQL/UI |
| 数据流转 | `src/flow`、`src/store` | 工区、导入、事务、归井、CRUD、持久化、来源核验 |
| 业务服务 | `src/services`、`src/adapters` | 曲线特征表、多井分层查询、图件查找、按需地震读取 |

`include/paleo_data` 是公开接口；`demo` 只调用服务接口；`tests` 和 `tools/create_api_demo_project.cpp` 生成合成输入。无需依赖完整工作站或 QGIS 界面。

## 编译

需要 C++20 编译器、CMake 3.24 或更高版本、Qt6 Core/Gui/Sql/Widgets、Zlib，以及 Qt SQLite 驱动。Windows 使用与 Qt 匹配的 MSVC x64 工具链。以下为单配置 Ninja 示例，路径应替换为自己的安装位置：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/Qt/6.x/msvc2022_64" -DZLIB_ROOT="C:/deps/zlib"
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
cmake --install build --prefix sdk
```

运行时应能找到 Qt DLL、`platforms` 和 `sqldrivers` 插件。无窗口测试可设置 `QT_QPA_PLATFORM=offscreen`。多配置构建器需要在构建、安装和测试命令中选定 `Release`，程序通常位于 `build/Release`。

Windows 也可在 MSVC x64 开发终端运行 `build.ps1`，通过 `-QtRoot`、`-DependencyRoot` 指定依赖路径；可选 `-CompilerSetup` 指向自己的编译环境初始化脚本。所有路径由调用者提供，不依赖其他工作站仓库。

旧式 `.xls` 读取动态使用 FreeXL。维护的源码在 `vendor/freexl`，Windows 构建说明见其 `NOTICE.txt` 和构建脚本；运行时还需对应的 libiconv 库。`.xlsx` 使用本项目的 ZIP/XML 解析器。这里给出标准 CMake 构建方法，不宣称所有系统或工具链均已验证。

源码可在 Windows x64 Release 环境独立构建并运行合成回归测试；安装后的 SDK 可编译全部十一个入门案例。验证过程不依赖随仓库发布的数据文件。

## 不带真实资料的演示

先在仓库外的新目录生成合成工区，再运行界面：

```powershell
.\build\create_api_demo_project.exe "D:/PaleoDemo/synthetic-project"
.\build\paleo_data_demo.exe --project "D:/PaleoDemo/synthetic-project"
```

生成器使用数学公式产生曲线、小型地震体和相关记录，不会覆盖已有 `data.sqlite`。生成结果只用于程序演示。移动生成工区后，应重新定位外部来源，或在新位置重新生成。

也可以启动 `paleo_data_demo`，新建工区后导入自己的目录。默认“引用原文件”可省去普通资料复制；原文件需保留在可访问位置。“打开工区”只查询已有数据库，不会重新导入输入目录。

## C++ 接入

```cmake
find_package(PaleoDataService CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core)
target_link_libraries(your_application PRIVATE Paleo::paleo_data_service Qt6::Core)
```

配置消费工程时，在 `CMAKE_PREFIX_PATH` 中提供安装后的 SDK 与 Qt 路径。在代码中包含 `paleo_data/service.h`，宿主进程先建立 `QCoreApplication`；已有 `QApplication` 的界面程序不需要再创建一个。

```cpp
#include <paleo_data/service.h>

// 假定已经建立 QCoreApplication，并取得同一工区中的有效 curveId。
paleo::data::DataService service;
std::string error;
if (!service.openProject(projectDirectory, error)) {
    // 把 error 交给调用方处理。
    return;
}
paleo::data::CurveSlice result;
if (service.readCurveSlice(curveId, paleo::data::Missing,
                          paleo::data::Missing, true, result, error)) {
    // result.depths / result.values 是实际数组，可直接计算或绘图。
}
```

`listData` 返回可供选择的资料卡片，`readCurveSlice` 等方法返回实际数据对象。JSON 是可选的导出形式，不是 C++ 调用的必经步骤。每个调用线程使用自己的 `DataService` 实例，不跨线程共享同一实例。

## 阅读顺序

- [宿主嵌入、资产版本目录与只读会话](docs/workstation-embedding.md)

- [接口与界面演示](docs/api-demo.md)
- [怎样读懂一次 C++ 调用](docs/cpp-reading-guide.md)
- [十一个完整入门案例](examples/getting_started/README.md)
- [SEG-Y 读取与基础处理](docs/segyio-guide.md)
- [多线程与增量导入](docs/import-performance.md)
- [便携运行与测试](docs/portable-testing.md)
- [开发范围与约束](docs/requirements.md)

## 数据解释边界

原始数值与来源信息保留；不把所有负数当成异常。单位、坐标系、深度基准和时深约定不根据数值猜测。未知或冲突的信息进入核对流程。专有文档、媒体和归档文件可能仅登记路径，登记成功不表示已经解析其中的全部内容。

本原型不包含通用 OCR、自动地质解释模型或完整地震处理系统。第三方组件保留各自来源和许可声明，见 [第三方组件说明](THIRD_PARTY_NOTICES.md) 和 `vendor` 中的说明文件。
