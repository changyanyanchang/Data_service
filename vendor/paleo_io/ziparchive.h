// 层：数据
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <cstdint>

// 方向41：最小内存 ZIP 读面（.sfpkg 与 .xlsx 都是 ZIP 容器）。
//
// 为什么不用 QgsZipUtils：它只按磁盘文件名工作（内层 surface.npz 必须落临时文件），
// 且 isZipFile 走 QMimeDatabase —— 非 .zip 扩展名在缺 mime 数据的机器上会被判成
// 非 ZIP。这里按 ZIP 记录结构直接读，全程在内存里做（不落临时文件、不依赖 mime 库），
// 内层 surface.npz 是 ZIP-of-NPY，同样直接用同一读面解。
//
// 支持：stored（method 0）与 deflate（method 8，zlib）。加密条目明确报因，不读
// 截断数据。单包上限 512 MiB（超限报因，不静默截断）。
// 方向67 写面 + ZIP64：zipBuildArchive/zipWriteArchive 组包（stored/deflate），
// 条目 >4 GiB / >65535 条 / 偏移溢出时自动 ZIP64（哨兵 0xFFFFFFFF + extra
// field 0x0001 + EOCD64/locator），forceZip64 可强制小包走 ZIP64 结构；读面
// 同步扩到 ZIP64（条目级预算闸不变——单包仍 ≤ 512 MiB，>4 GiB 条目解不出）。
// 层：数据
namespace paleo::data::zip
{

struct ZipEntry
{
  QString name;
  std::uint16_t method = 0;
  std::uint32_t crc32 = 0; // 中央目录 CRC32——解压后必须比对（#477）
  std::uint64_t compressedSize = 0;
  std::uint64_t uncompressedSize = 0;
  std::uint64_t localHeaderOffset = 0;
  bool encrypted = false;
};

struct ZipListResult
{
  bool ok = false;
  QString error;
  QVector<ZipEntry> entries;

  const ZipEntry *entry( const QString &name ) const;
  bool contains( const QString &name ) const { return entry( name ) != nullptr; }
};

// 整包读入内存（普通文件；> 512 MiB 报因）。失败返回空 QByteArray 并写 error。
QByteArray zipReadArchive( const QString &path, QString *error );

// 列条目（中央目录；ZIP64 哨兵字段经 extra field 0x0001 还原 64 位真值）。
ZipListResult zipListBytes( const QByteArray &archive );

// 内存解出单个条目。失败时 error 给原因（缺条目/不支持的方法/解压失败）。
bool zipExtractBytes( const QByteArray &archive, const QString &entryName, QByteArray *out,
                      QString *error );

// PW-074：解出 zipListBytes 已列出的条目（不重解中央目录）。多条目读取先列一次
// 目录再逐条调用本函数；entry 须来自同一 archive 的列表。
bool zipExtractEntry( const QByteArray &archive, const ZipEntry &entry, QByteArray *out,
                      QString *error );

// ---- 写面（方向67）----

struct ZipWriteEntry
{
  QString name;   // UTF-8 写入（非 ASCII 自动置通用位标志 bit 11）
  QByteArray data;
};

enum class ZipCompression
{
  Stored,
  Deflate
};

struct ZipBuildOptions
{
  ZipCompression compression = ZipCompression::Stored;
  // 结构性 ZIP64：小包也写条目哨兵 + extra + EOCD64（round-trip 测试与前向
  // 兼容用；常规组包按需自动升级，不需要置位）。
  bool forceZip64 = false;
};

// 内存组包。条目按输入顺序写入（sfpkg 契约顺序由调用方保证）；重名条目报因
//（读面拒重名，写面不产重名）。zip64Used 非空时回写是否用了 ZIP64 结构。
QByteArray zipBuildArchive( const QVector<ZipWriteEntry> &entries, const ZipBuildOptions &options,
                            QString *error, bool *zip64Used = nullptr );

// 组包 + 落盘（截断写）。目录不存在/写一半失败都如实报因。
bool zipWriteArchive( const QString &path, const QVector<ZipWriteEntry> &entries,
                      const ZipBuildOptions &options, QString *error, bool *zip64Used = nullptr );

} // namespace paleo::data::zip
