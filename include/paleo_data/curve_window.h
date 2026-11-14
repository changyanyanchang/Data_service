#pragma once
#include <functional>

namespace paleo::data {
enum class CurveDepthOrigin { Unknown, Declared, Inferred };
// Origin of each delivered convention. A compatibility fallback is not a
// declaration by the stored log set; failed reads leave all origins Unknown.
struct CurveDepthOrigins {
    CurveDepthOrigin depthType=CurveDepthOrigin::Unknown;
    CurveDepthOrigin depthUnit=CurveDepthOrigin::Unknown;
    CurveDepthOrigin mdReference=CurveDepthOrigin::Unknown;
};
// Budgets count payload handed to the SQL driver, not physical disk/cache I/O.
// Legacy blobs are materialized only when their entire payload fits both limits.
struct CurveReadOptions {
    long long maxSamples=1000000;
    long long maxReadBytes=64LL*1024*1024;
    long long maxLegacyBytes=1024LL*1024;
    std::function<bool()> cancelled;
};
struct CurveReadStats {
    long long payloadBytesRead=0, legacyBytesMaterialized=0;
    long long axisBlocksRead=0, valueBlocksRead=0;
    bool chunked=false;
};
struct CurveWindowQuery {
    long long curveId=0, firstSample=0, sampleCount=5000;
    bool processed=true, includeAuxiliaryAxes=true;
};
}
