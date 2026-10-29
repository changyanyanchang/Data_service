// 层：数据
#include "ziparchive.h"

#include <QFile>
#include <QSaveFile>
#include <QSet>
#include <QFileInfo>

#if defined( PALEO_HAVE_ZLIB )
#include <zlib.h>
#else
#error "ziparchive 需要 zlib：deflate 条目（.sfpkg/.xlsx 默认压缩）无法在无 zlib 的构建里读取"
#endif

#include <algorithm>
#include <cstring>

// 层：数据
namespace paleo::data::zip
{
namespace
{

constexpr std::uint32_t kEndOfCentralDirectory = 0x06054b50u;
constexpr std::uint32_t kZip64EndOfCentralDirectory = 0x06064b50u;
constexpr std::uint32_t kZip64Locator = 0x07064b50u;
constexpr std::uint32_t kCentralHeader = 0x02014b50u;
constexpr std::uint32_t kLocalHeader = 0x04034b50u;
constexpr std::uint32_t kZip64Sentinel = 0xFFFFFFFFu;
constexpr std::uint16_t kZip64ExtraHeaderId = 0x0001;
constexpr qint64 kMaxArchiveBytes = 512ll * 1024 * 1024;
// 整包声明解压总量上限（zip bomb 闸；与单条目闸一起生效）。
constexpr qint64 kMaxTotalUncompressedBytes = 512ll * 1024 * 1024;

std::uint16_t readU16( const QByteArray &buffer, int offset )
{
  return static_cast<std::uint16_t>( static_cast<unsigned char>( buffer.at( offset ) ) ) |
         static_cast<std::uint16_t>( static_cast<unsigned char>( buffer.at( offset + 1 ) ) << 8 );
}

std::uint32_t readU32( const QByteArray &buffer, int offset )
{
  return static_cast<std::uint32_t>( static_cast<unsigned char>( buffer.at( offset ) ) ) |
         ( static_cast<std::uint32_t>( static_cast<unsigned char>( buffer.at( offset + 1 ) ) ) << 8 ) |
         ( static_cast<std::uint32_t>( static_cast<unsigned char>( buffer.at( offset + 2 ) ) ) << 16 ) |
         ( static_cast<std::uint32_t>( static_cast<unsigned char>( buffer.at( offset + 3 ) ) ) << 24 );
}

std::uint64_t readU64( const QByteArray &buffer, int offset )
{
  return static_cast<std::uint64_t>( readU32( buffer, offset ) ) |
         ( static_cast<std::uint64_t>( readU32( buffer, offset + 4 ) ) << 32 );
}

void appendU16( QByteArray *out, std::uint16_t value )
{
  out->append( static_cast<char>( value & 0xFF ) );
  out->append( static_cast<char>( ( value >> 8 ) & 0xFF ) );
}

void appendU32( QByteArray *out, std::uint32_t value )
{
  for ( int shift = 0; shift < 32; shift += 8 )
    out->append( static_cast<char>( ( value >> shift ) & 0xFF ) );
}

void appendU64( QByteArray *out, std::uint64_t value )
{
  for ( int shift = 0; shift < 64; shift += 8 )
    out->append( static_cast<char>( ( value >> shift ) & 0xFF ) );
}

// 中央目录结束记录可能被尾部注释推前：按 ZIP 规范只在尾部注释区
//（≤ 65535 字节 + EOCD 22 字节）里从后往前找签名，不整包回扫。
int findEndOfCentralDirectory( const QByteArray &archive )
{
  const int lowest = std::max( 0, static_cast<int>( archive.size() ) - ( 65535 + 22 ) );
  for ( int i = static_cast<int>( archive.size() ) - 22; i >= lowest; --i )
  {
    if ( readU32( archive, i ) == kEndOfCentralDirectory )
      return i;
  }
  return -1;
}

std::uint32_t crc32Of( const QByteArray &data )
{
  return static_cast<std::uint32_t>(
      crc32( 0, reinterpret_cast<const Bytef *>( data.constData() ), static_cast<uInt>( data.size() ) ) );
}

// raw deflate（ZIP 条目不走 zlib 头，windowBits = -MAX_WBITS）。
QByteArray deflateRaw( const QByteArray &input, QString *error )
{
  z_stream stream;
  std::memset( &stream, 0, sizeof( stream ) );
  if ( deflateInit2( &stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                     Z_DEFAULT_STRATEGY ) != Z_OK )
  {
    *error = QStringLiteral( "zlib deflate 初始化失败" );
    return {};
  }
  QByteArray out;
  const uInt bound = deflateBound( &stream, static_cast<uLong>( input.size() ) );
  out.resize( static_cast<int>( bound ) ); // zlib 自己的上界（不可压数据也放得下）
  stream.next_in = reinterpret_cast<Bytef *>( const_cast<char *>( input.constData() ) );
  stream.avail_in = static_cast<uInt>( input.size() );
  stream.next_out = reinterpret_cast<Bytef *>( out.data() );
  stream.avail_out = bound;
  const int status = deflate( &stream, Z_FINISH );
  const bool ok = status == Z_STREAM_END && stream.total_in == static_cast<uLong>( input.size() );
  if ( ok )
    out.resize( static_cast<int>( stream.total_out ) );
  deflateEnd( &stream );
  if ( !ok )
  {
    *error = QStringLiteral( "zlib deflate 失败（状态 %1）" ).arg( status );
    return {};
  }
  return out;
}

} // namespace

QByteArray zipReadArchive( const QString &path, QString *error )
{
  const auto fail = [error]( const QString &message ) {
    if ( error )
      *error = message;
    return QByteArray();
  };
  const QFileInfo info( path );
  if ( !info.exists() || !info.isFile() )
    return fail( QStringLiteral( "文件不存在：%1" ).arg( path ) );
  if ( info.size() > kMaxArchiveBytes )
  {
    return fail( QStringLiteral( "包 %1 大小 %2 字节超过上限 %3 字节，暂不读取" )
                     .arg( info.fileName() )
                     .arg( info.size() )
                     .arg( kMaxArchiveBytes ) );
  }
  QFile file( path );
  if ( !file.open( QIODevice::ReadOnly ) )
    return fail( QStringLiteral( "无法打开文件：%1" ).arg( path ) );
  const QByteArray archive = file.readAll();
  if ( static_cast<qint64>( archive.size() ) != info.size() )
    return fail( QStringLiteral( "包 %1 读取不完整" ).arg( info.fileName() ) );
  return archive;
}

const ZipEntry *ZipListResult::entry( const QString &name ) const
{
  for ( const ZipEntry &item : entries )
  {
    if ( item.name == name )
      return &item;
  }
  return nullptr;
}

ZipListResult zipListBytes( const QByteArray &archive )
{
  ZipListResult result;
  const int eocd = findEndOfCentralDirectory( archive );
  if ( eocd < 0 )
  {
    result.error = QStringLiteral( "不是 ZIP 容器（没有中央目录结束记录）" );
    return result;
  }
  std::uint64_t count = readU16( archive, eocd + 10 );
  std::uint64_t directorySize = readU32( archive, eocd + 12 );
  std::uint64_t directoryOffset = readU32( archive, eocd + 16 );
  // ZIP64：EOCD 字段是哨兵值 → 从紧挨 EOCD 的 locator 找 EOCD64 恢复 64 位真值。
  // 条目数恰好 65535 时 count == 0xFFFF 但不是哨兵——locator 不在就按普通包走
  //（偏移/大小是真哨兵而 locator 缺失才是坏包）。
  const bool countSentinel = count == 0xFFFFu;
  const bool sizeSentinel = directorySize == kZip64Sentinel;
  const bool offsetSentinel = directoryOffset == kZip64Sentinel;
  if ( countSentinel || sizeSentinel || offsetSentinel )
  {
    const int locator = eocd - 20;
    bool resolved = false;
    if ( locator >= 0 && readU32( archive, locator ) == kZip64Locator )
    {
      const std::uint64_t zip64Offset = readU64( archive, locator + 8 );
      // #381：用减法比较——加法在 zip64Offset ≥ 2^64−56 时回绕穿闸，
      // 随后 static_cast<int> 成负值读缓冲区之前的内存。
      if ( zip64Offset <= static_cast<std::uint64_t>( archive.size() ) &&
           static_cast<std::uint64_t>( archive.size() ) - zip64Offset >= 56 &&
           readU32( archive, static_cast<int>( zip64Offset ) ) == kZip64EndOfCentralDirectory )
      {
        count = readU64( archive, static_cast<int>( zip64Offset ) + 32 );
        directorySize = readU64( archive, static_cast<int>( zip64Offset ) + 40 );
        directoryOffset = readU64( archive, static_cast<int>( zip64Offset ) + 48 );
        resolved = true;
      }
    }
    if ( !resolved && ( sizeSentinel || offsetSentinel ) )
    {
      result.error = QStringLiteral( "ZIP64 中央目录定位失败（locator 或 EOCD64 记录缺失/越界）" );
      return result;
    }
  }
  const std::uint64_t archiveSize = static_cast<std::uint64_t>( archive.size() );
  // #382：一律用减法判界——directoryOffset/directorySize 是攻击者可控的 u64
  // （ZIP64 路径由 EOCD64 覆写），加法在 2^64−56 量级回绕成 0 穿过旧闸，
  // 随后 directoryEnd/offset 换 qint64 得负值，readU32 以负下标越读。
  if ( directorySize > archiveSize || directoryOffset > archiveSize - directorySize )
  {
    result.error = QStringLiteral( "ZIP 中央目录偏移越界" );
    return result;
  }
  if ( count > 0x7FFFFFFF )
  {
    result.error = QStringLiteral( "ZIP 条目数声明异常（%1）" ).arg( static_cast<qulonglong>( count ) );
    return result;
  }
  // 条目硬上界 = 中央目录声明的范围（不是整个包）：声言 directorySize=0 的包
  // 不能把包内任意数据当条目读。上面减法闸已把 directoryOffset/directorySize
  // 一并压进 [0, archiveSize]，故此处换 qint64 必为非负；仍显式兜底，防后续
  // 改动松开闸门时静默退回负下标越读。
  if ( directoryOffset > archiveSize )
  {
    result.error = QStringLiteral( "ZIP 中央目录偏移越界" );
    return result;
  }
  const qint64 directoryEnd = static_cast<qint64>( directoryOffset ) + static_cast<qint64>( directorySize );
  qint64 offset = static_cast<qint64>( directoryOffset );
  for ( std::uint64_t i = 0; i < count; ++i )
  {
    if ( offset + 46 > directoryEnd || readU32( archive, static_cast<int>( offset ) ) != kCentralHeader )
    {
      result.error = QStringLiteral( "ZIP 中央目录第 %1 项签名不符" ).arg( i + 1 );
      result.entries.clear();
      return result;
    }
    ZipEntry item;
    const std::uint16_t flags = readU16( archive, static_cast<int>( offset ) + 8 );
    item.method = readU16( archive, static_cast<int>( offset ) + 10 );
    item.crc32 = readU32( archive, static_cast<int>( offset ) + 16 ); // #477：解压后比对
    std::uint64_t compressed = readU32( archive, static_cast<int>( offset ) + 20 );
    std::uint64_t uncompressed = readU32( archive, static_cast<int>( offset ) + 24 );
    const std::uint16_t nameLength = readU16( archive, static_cast<int>( offset ) + 28 );
    const std::uint16_t extraLength = readU16( archive, static_cast<int>( offset ) + 30 );
    const std::uint16_t commentLength = readU16( archive, static_cast<int>( offset ) + 32 );
    std::uint64_t localHeaderOffset = readU32( archive, static_cast<int>( offset ) + 42 );
    item.encrypted = ( flags & 0x1u ) != 0;
    // 条目总长（名字 + extra + 注释）也要落在中央目录内。
    if ( offset + 46 + nameLength + extraLength + commentLength > directoryEnd )
    {
      result.error = QStringLiteral( "ZIP 中央目录第 %1 项越出中央目录范围（名字/extra/注释被截断）" ).arg( i + 1 );
      result.entries.clear();
      return result;
    }
    // 条目级 ZIP64：固定字段是哨兵 → extra field 0x0001 里按（usize, csize,
    // offset）顺序取 64 位真值（本仓写面恒写满 24 字节；他源只写前缀的也兼容）。
    if ( compressed == kZip64Sentinel || uncompressed == kZip64Sentinel ||
         localHeaderOffset == kZip64Sentinel )
    {
      int extra = static_cast<int>( offset ) + 46 + nameLength;
      const int extraEnd = extra + extraLength;
      bool repaired = false;
      while ( extra + 4 <= extraEnd )
      {
        const std::uint16_t headerId = readU16( archive, extra );
        const std::uint16_t payloadSize = readU16( archive, extra + 2 );
        if ( extra + 4 + payloadSize > extraEnd )
          break; // extra 字段自身越界：不吞，交给下面的未修复判错
        if ( headerId == kZip64ExtraHeaderId )
        {
          if ( uncompressed == kZip64Sentinel && payloadSize >= 8 )
            uncompressed = readU64( archive, extra + 4 );
          if ( compressed == kZip64Sentinel && payloadSize >= 16 )
            compressed = readU64( archive, extra + 12 );
          if ( localHeaderOffset == kZip64Sentinel && payloadSize >= 24 )
            localHeaderOffset = readU64( archive, extra + 20 );
          repaired = true;
        }
        extra += 4 + payloadSize;
      }
      if ( !repaired )
      {
        result.error = QStringLiteral( "ZIP64 条目「%1」缺少 extra field 0x0001" )
                           .arg( QString::fromUtf8( archive.mid( static_cast<int>( offset ) + 46, nameLength ) ) );
        result.entries.clear();
        return result;
      }
    }
    item.name = QString::fromUtf8( archive.mid( static_cast<int>( offset ) + 46, nameLength ) );
    item.compressedSize = compressed;
    item.uncompressedSize = uncompressed;
    item.localHeaderOffset = localHeaderOffset;
    // 用减法判界：u64 加法在 localHeaderOffset 接近 2^64 时回绕成小值绕过检查。
    if ( localHeaderOffset > static_cast<std::uint64_t>( archive.size() ) - 30 )
    {
      result.error = QStringLiteral( "ZIP 条目「%1」本地头偏移越界" ).arg( item.name );
      result.entries.clear();
      return result;
    }
    result.entries.append( item );
    offset += 46 + nameLength + extraLength + commentLength;
  }
  // 解压预算：deflate 可把几百字节放大成数百 MiB，逐条 512 MiB 闸挡不住
  // 「几千条各声言 512 MiB」的包。这里按中央目录声明的未压缩总量一次性判。
  // PW-073：u64 累加、逐条先判——ZIP64 声明 >2^63 的尺寸强转 qint64 为负会把总量减小绕过闸。
  quint64 totalUncompressed = 0;
  for ( const ZipEntry &entry : result.entries )
  {
    const quint64 size = static_cast<quint64>( entry.uncompressedSize );
    const quint64 limit = static_cast<quint64>( kMaxTotalUncompressedBytes );
    totalUncompressed = ( size > limit || totalUncompressed > limit - size ) ? limit + 1
                                                                              : totalUncompressed + size;
    if ( totalUncompressed > limit )
    {
      result.error = QStringLiteral( "ZIP 声明解压总量超过上限（%1 字节 > %2 字节），拒绝读取" )
                         .arg( totalUncompressed )
                         .arg( kMaxTotalUncompressedBytes );
      result.entries.clear();
      return result;
    }
  }
  // 重名条目：不同读取器取首条/末条不一致，这里明确拒绝而不是悄悄取一条。
  //（方向67：O(n²) 对比改哈希集——7 万条目的 ZIP64 包逐对比较要数分钟。）
  {
    QSet<QString> names;
    names.reserve( result.entries.size() );
    for ( const ZipEntry &entry : std::as_const( result.entries ) )
    {
      if ( names.contains( entry.name ) )
      {
        result.error = QStringLiteral( "ZIP 内条目重名：%1" ).arg( entry.name );
        result.entries.clear();
        return result;
      }
      names.insert( entry.name );
    }
  }
  result.ok = true;
  return result;
}

bool zipExtractBytes( const QByteArray &archive, const QString &entryName, QByteArray *out,
                      QString *error )
{
  const auto fail = [error]( const QString &message ) {
    if ( error )
      *error = message;
    return false;
  };
  const ZipListResult listing = zipListBytes( archive );
  if ( !listing.ok )
    return fail( listing.error );
  const ZipEntry *item = listing.entry( entryName );
  if ( !item )
    return fail( QStringLiteral( "ZIP 内没有条目：%1" ).arg( entryName ) );
  return zipExtractEntry( archive, *item, out, error );
}

// PW-074：按已列出的条目解压——调用方列一次目录后逐条解，不再每条重解中央目录（O(n²)）。
bool zipExtractEntry( const QByteArray &archive, const ZipEntry &entry, QByteArray *out,
                      QString *error )
{
  const auto fail = [error]( const QString &message ) {
    if ( error )
      *error = message;
    return false;
  };
  const QString &entryName = entry.name;
  const ZipEntry *item = &entry;
  if ( item->encrypted )
    return fail( QStringLiteral( "条目「%1」已加密，不支持" ).arg( entryName ) );
  if ( item->method != 0 && item->method != 8 )
  {
    return fail( QStringLiteral( "条目「%1」压缩方法 %2 不支持（只支持 stored/deflate）" )
                     .arg( entryName )
                     .arg( item->method ) );
  }
  // 减法判界 + 负值先拒：u64 偏移换 qint64 可为负，负下标 at() 是越界读。
  const qint64 headerOffset = static_cast<qint64>( item->localHeaderOffset );
  const qint64 compressedSize = static_cast<qint64>( item->compressedSize );
  if ( headerOffset < 0 || compressedSize < 0 ||
       headerOffset > archive.size() - 30 ||
       readU32( archive, static_cast<int>( headerOffset ) ) != kLocalHeader )
    return fail( QStringLiteral( "条目「%1」本地头签名不符" ).arg( entryName ) );
  const std::uint16_t nameLength = readU16( archive, static_cast<int>( headerOffset ) + 26 );
  const std::uint16_t extraLength = readU16( archive, static_cast<int>( headerOffset ) + 28 );
  const qint64 dataOffset = headerOffset + 30 + nameLength + extraLength;
  if ( dataOffset > archive.size() || compressedSize > archive.size() - dataOffset )
    return fail( QStringLiteral( "条目「%1」数据段被截断" ).arg( entryName ) );
  const QByteArray raw = archive.mid( static_cast<int>( dataOffset ),
                                      static_cast<int>( compressedSize ) );
  if ( !out )
    return true;
  // #477：中央目录的 CRC32 是条目内容的完整性契约——此前只校验长度/zlib
  // 状态，字节损坏但恰好 inflate 成功的条目会静默交出坏数据（xlsx 链
  // outsourceworkbook 亦受益，无需单独 sha256 兜底）。
  const auto verifyCrc = [&]( const QByteArray &data ) {
    if ( crc32Of( data ) != item->crc32 )
    {
      return fail( QStringLiteral( "条目「%1」CRC32 校验失败（中央目录 %2，实得 %3）" )
                       .arg( entryName )
                       .arg( item->crc32, 8, 16, QLatin1Char( '0' ) )
                       .arg( crc32Of( data ), 8, 16, QLatin1Char( '0' ) ) );
    }
    return true;
  };
  if ( item->method == 0 )
  {
    if ( raw.size() != static_cast<int>( item->uncompressedSize ) )
      return fail( QStringLiteral( "条目「%1」stored 长度与声明不符（%2，期望 %3）" )
                       .arg( entryName )
                       .arg( raw.size() )
                       .arg( item->uncompressedSize ) );
    if ( !verifyCrc( raw ) )
      return false;
    *out = raw;
    return true;
  }

  // deflate：raw inflate（-MAX_WBITS），按中央目录给出的未压缩长度预分配。
  if ( item->uncompressedSize > static_cast<std::uint64_t>( kMaxArchiveBytes ) )
    return fail( QStringLiteral( "条目「%1」解压后大小超过上限" ).arg( entryName ) );
  if ( item->uncompressedSize == 0 )
  {
    out->clear(); // 空条目：inflate 零输出缓冲会返回 Z_BUF_ERROR，这里直接给空
    return true;
  }
  z_stream stream;
  std::memset( &stream, 0, sizeof( stream ) );
  if ( inflateInit2( &stream, -MAX_WBITS ) != Z_OK )
    return fail( QStringLiteral( "条目「%1」zlib 初始化失败" ).arg( entryName ) );
  QByteArray inflated;
  inflated.resize( static_cast<int>( item->uncompressedSize ) );
  stream.next_in = reinterpret_cast<Bytef *>( const_cast<char *>( raw.constData() ) );
  stream.avail_in = static_cast<uInt>( raw.size() );
  stream.next_out = reinterpret_cast<Bytef *>( inflated.data() );
  stream.avail_out = static_cast<uInt>( inflated.size() );
  const int status = inflate( &stream, Z_FINISH );
  const uInt produced = stream.total_out;
  inflateEnd( &stream );
  if ( status != Z_STREAM_END || produced != item->uncompressedSize )
  {
    return fail( QStringLiteral( "条目「%1」解压失败（zlib 状态 %2，得到 %3 字节，期望 %4）" )
                     .arg( entryName )
                     .arg( status )
                     .arg( produced )
                     .arg( item->uncompressedSize ) );
  }
  inflated.resize( static_cast<int>( produced ) );
  if ( !verifyCrc( inflated ) )
    return false;
  *out = inflated;
  return true;
}

QByteArray zipBuildArchive( const QVector<ZipWriteEntry> &entries, const ZipBuildOptions &options,
                            QString *error, bool *zip64Used )
{
  const auto fail = [error]( const QString &message ) {
    if ( error )
      *error = message;
    return QByteArray();
  };
  if ( zip64Used )
    *zip64Used = false;
  if ( entries.isEmpty() )
    return fail( QStringLiteral( "ZIP 写面至少需要一个条目" ) );
  QSet<QString> seenNames;
  for ( int i = 0; i < entries.size(); ++i )
  {
    const ZipWriteEntry &entry = entries.at( i );
    if ( entry.name.isEmpty() )
      return fail( QStringLiteral( "第 %1 个条目名字为空" ).arg( i + 1 ) );
    if ( seenNames.contains( entry.name ) )
      return fail( QStringLiteral( "ZIP 条目重名（写面不产重名包）：%1" ).arg( entry.name ) );
    seenNames.insert( entry.name );
  }

  struct StagedEntry
  {
    QByteArray name;
    std::uint16_t method = 0;
    std::uint16_t flags = 0;
    std::uint32_t crc = 0;
    std::uint64_t compressed = 0;
    std::uint64_t uncompressed = 0;
    std::uint64_t localOffset = 0;
    bool zip64 = false;
    QByteArray payload;
  };
  std::vector<StagedEntry> staged;
  staged.reserve( static_cast<std::size_t>( entries.size() ) );
  QByteArray out;
  for ( const ZipWriteEntry &entry : entries )
  {
    StagedEntry item;
    item.name = entry.name.toUtf8();
    // 非 ASCII 名字按规范置通用位 bit 11（UTF-8）。
    for ( const char byte : item.name )
    {
      if ( static_cast<unsigned char>( byte ) > 0x7F )
      {
        item.flags = 0x0800;
        break;
      }
    }
    item.crc = crc32Of( entry.data );
    item.uncompressed = static_cast<std::uint64_t>( entry.data.size() );
    if ( options.compression == ZipCompression::Deflate && !entry.data.isEmpty() )
    {
      QString deflateError;
      item.payload = deflateRaw( entry.data, &deflateError );
      if ( item.payload.isEmpty() )
        return fail( QStringLiteral( "条目「%1」压缩失败：%2" ).arg( entry.name, deflateError ) );
      item.method = 8;
    }
    else
    {
      item.payload = entry.data;
      item.method = 0;
    }
    item.compressed = static_cast<std::uint64_t>( item.payload.size() );
    item.localOffset = static_cast<std::uint64_t>( out.size() );
    item.zip64 = options.forceZip64 || item.uncompressed > 0xFFFFFFFFu ||
                 item.compressed > 0xFFFFFFFFu || item.localOffset > 0xFFFFFFFFu;
    // 本地头（含 ZIP64 extra：usize + csize，偏移按规范不进本地 extra）。
    appendU32( &out, kLocalHeader );
    appendU16( &out, item.zip64 ? 45 : ( item.method == 8 ? 20 : 10 ) );
    appendU16( &out, item.flags );
    appendU16( &out, item.method );
    appendU16( &out, 0 ); // dos time（1980-01-01 00:00）
    appendU16( &out, 0x0021 );
    appendU32( &out, item.crc );
    if ( item.zip64 )
    {
      appendU32( &out, kZip64Sentinel );
      appendU32( &out, kZip64Sentinel );
    }
    else
    {
      appendU32( &out, static_cast<std::uint32_t>( item.compressed ) );
      appendU32( &out, static_cast<std::uint32_t>( item.uncompressed ) );
    }
    appendU16( &out, static_cast<std::uint16_t>( item.name.size() ) );
    appendU16( &out, item.zip64 ? 20 : 0 ); // extra：4 头 + 2×8 字段
    out.append( item.name );
    if ( item.zip64 )
    {
      appendU16( &out, kZip64ExtraHeaderId );
      appendU16( &out, 16 );
      appendU64( &out, item.uncompressed );
      appendU64( &out, item.compressed );
    }
    out.append( item.payload );
    staged.push_back( std::move( item ) );
  }

  // 中央目录。
  const std::uint64_t directoryOffset = static_cast<std::uint64_t>( out.size() );
  bool anyEntryZip64 = false;
  for ( const StagedEntry &item : staged )
  {
    anyEntryZip64 = anyEntryZip64 || item.zip64;
    appendU32( &out, kCentralHeader );
    appendU16( &out, item.zip64 ? 45 : 20 ); // version made by
    appendU16( &out, item.zip64 ? 45 : ( item.method == 8 ? 20 : 10 ) );
    appendU16( &out, item.flags );
    appendU16( &out, item.method );
    appendU16( &out, 0 );
    appendU16( &out, 0x0021 );
    appendU32( &out, item.crc );
    if ( item.zip64 )
    {
      appendU32( &out, kZip64Sentinel );
      appendU32( &out, kZip64Sentinel );
    }
    else
    {
      appendU32( &out, static_cast<std::uint32_t>( item.compressed ) );
      appendU32( &out, static_cast<std::uint32_t>( item.uncompressed ) );
    }
    appendU16( &out, static_cast<std::uint16_t>( item.name.size() ) );
    appendU16( &out, item.zip64 ? 28 : 0 ); // extra：4 头 + 3×8 字段
    appendU16( &out, 0 );                   // comment
    appendU16( &out, 0 );                   // disk start
    appendU16( &out, 0 );                   // internal attrs
    appendU32( &out, 0 );                   // external attrs
    appendU32( &out, item.zip64 ? kZip64Sentinel
                                : static_cast<std::uint32_t>( item.localOffset ) );
    out.append( item.name );
    if ( item.zip64 )
    {
      appendU16( &out, kZip64ExtraHeaderId );
      appendU16( &out, 24 );
      appendU64( &out, item.uncompressed );
      appendU64( &out, item.compressed );
      appendU64( &out, item.localOffset );
    }
  }
  const std::uint64_t directorySize =
      static_cast<std::uint64_t>( out.size() ) - directoryOffset;
  const std::uint64_t entryCount = static_cast<std::uint64_t>( staged.size() );
  const bool archive64 = options.forceZip64 || anyEntryZip64 || entryCount > 0xFFFF ||
                         directoryOffset > 0xFFFFFFFFu || directorySize > 0xFFFFFFFFu;
  if ( archive64 )
  {
    const std::uint64_t eocd64Offset = static_cast<std::uint64_t>( out.size() );
    appendU32( &out, kZip64EndOfCentralDirectory );
    appendU64( &out, 44 ); // 记录余量（无 extensible data）
    appendU16( &out, 45 );
    appendU16( &out, 45 );
    appendU32( &out, 0 );
    appendU32( &out, 0 );
    appendU64( &out, entryCount );
    appendU64( &out, entryCount );
    appendU64( &out, directorySize );
    appendU64( &out, directoryOffset );
    appendU32( &out, kZip64Locator );
    appendU32( &out, 0 );
    appendU64( &out, eocd64Offset );
    appendU32( &out, 1 );
  }
  appendU32( &out, kEndOfCentralDirectory );
  appendU16( &out, 0 );
  appendU16( &out, 0 );
  appendU16( &out, entryCount > 0xFFFF ? 0xFFFF : static_cast<std::uint16_t>( entryCount ) );
  appendU16( &out, entryCount > 0xFFFF ? 0xFFFF : static_cast<std::uint16_t>( entryCount ) );
  appendU32( &out, directorySize > 0xFFFFFFFFu ? kZip64Sentinel
                                               : static_cast<std::uint32_t>( directorySize ) );
  appendU32( &out, directoryOffset > 0xFFFFFFFFu ? kZip64Sentinel
                                                 : static_cast<std::uint32_t>( directoryOffset ) );
  appendU16( &out, 0 ); // comment
  if ( zip64Used )
    *zip64Used = archive64;
  return out;
}

bool zipWriteArchive( const QString &path, const QVector<ZipWriteEntry> &entries,
                      const ZipBuildOptions &options, QString *error, bool *zip64Used )
{
  QString buildError;
  const QByteArray archive = zipBuildArchive( entries, options, &buildError, zip64Used );
  if ( archive.isEmpty() )
  {
    if ( error )
      *error = buildError;
    return false;
  }
  // #414：QSaveFile 原子写——磁盘满/IO 错不留看似完整的截断 .sfpkg
  //（同层其它 writer 的 temp+rename 口径；commit 失败自动清暂存）。
  QSaveFile file( path );
  if ( !file.open( QIODevice::WriteOnly ) )
  {
    if ( error )
      *error = QStringLiteral( "无法写入文件：%1（%2）" ).arg( path, file.errorString() );
    return false;
  }
  if ( file.write( archive ) != archive.size() || !file.commit() )
  {
    if ( error )
      *error = QStringLiteral( "写入不完整：%1（%2）" ).arg( path, file.errorString() );
    return false;
  }
  return true;
}

} // namespace paleo::data::zip
