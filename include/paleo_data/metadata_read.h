#pragma once
namespace paleo::data {
struct MetadataReadOptions {
    long long maxFieldBytes=8192, maxTextBytes=32768;
};
struct MetadataReadStats {
    long long textBytesRead=0; // Returned UTF-8 bytes; zero on failed point/page reads.
};
using SourceIndexReadOptions=MetadataReadOptions;
using SourceIndexReadStats=MetadataReadStats;
}
