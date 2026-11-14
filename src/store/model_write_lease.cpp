#include "paleo_data/model_changes.h"
#include "model_change_journal.h"
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <algorithm>
#include <QFile>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace paleo::data {
namespace {
struct Gate {
    explicit Gate(const QString& path):file(path){file.setStaleLockTime(0);}
    QLockFile file;std::mutex mutex;std::condition_variable changed;
    std::thread::id owner;unsigned leases=0;
};
std::mutex gatesMutex;
std::map<QString,std::weak_ptr<Gate>> gates;
}
struct ModelWriteLease::Impl {
    std::shared_ptr<Gate> gate;
    ~Impl(){if(gate){std::lock_guard lock(gate->mutex);if(--gate->leases==0){gate->owner={};gate->file.unlock();gate->changed.notify_all();}}}
};
ModelWriteLease::ModelWriteLease()=default;
ModelWriteLease::~ModelWriteLease()=default;
ModelWriteLease::ModelWriteLease(ModelWriteLease&&) noexcept=default;
ModelWriteLease& ModelWriteLease::operator=(ModelWriteLease&&) noexcept=default;
ModelWriteLease::operator bool()const{return bool(impl_);}
ModelWriteLease tryAcquireModelWriteLease(const std::string& directory,int timeoutMs,std::string& error) {
    ModelWriteLease lease;error.clear();if(directory.empty()||timeoutMs<0){error="Invalid model write lease request";return lease;}
    QFileInfo info(QString::fromUtf8(directory.data(),qsizetype(directory.size())));
    auto path=QDir::cleanPath(info.absoluteFilePath());
    const auto canonical=info.canonicalFilePath();if(!canonical.isEmpty())path=canonical;
#ifdef Q_OS_WIN
    path=path.toLower();
#endif
    std::shared_ptr<Gate> gate;
    {std::lock_guard lock(gatesMutex);gate=gates[path].lock();if(!gate){gate=std::make_shared<Gate>(path+".write.lock");gates[path]=gate;}}
    const auto end=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
    std::unique_lock lock(gate->mutex);const auto thread=std::this_thread::get_id();
    if(gate->leases&&gate->owner!=thread&&!gate->changed.wait_until(lock,end,[&]{return gate->leases==0;})){error="ModelWriteBusy: model writer or publisher holds the gate";return lease;}
    if(!gate->leases){
        const auto remaining=std::max<long long>(0,std::chrono::duration_cast<std::chrono::milliseconds>(end-std::chrono::steady_clock::now()).count());
        if(!gate->file.tryLock(int(remaining))){error="ModelWriteBusy: cannot acquire model writer gate";return lease;}
        gate->owner=thread;
    }
    ++gate->leases;lease.impl_=std::make_unique<ModelWriteLease::Impl>();lease.impl_->gate=std::move(gate);return lease;
}
namespace detail {
QString modelDatabaseFileIdentity(const QString& file,std::string& error) {
#ifdef Q_OS_WIN
    const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(file.utf16()),FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE){error="StaleModelGeneration: active database file is unavailable";return {};}
    BY_HANDLE_FILE_INFORMATION info{};const bool ok=GetFileInformationByHandle(handle,&info)!=0;CloseHandle(handle);
    if(!ok){error="StaleModelGeneration: cannot identify active database file";return {};}
    return QString("win:%1:%2:%3").arg(qulonglong(info.dwVolumeSerialNumber),0,16).arg(qulonglong(info.nFileIndexHigh),0,16).arg(qulonglong(info.nFileIndexLow),0,16);
#else
    struct stat info{};const auto native=QFile::encodeName(file);
    if(::stat(native.constData(),&info)!=0){error="StaleModelGeneration: active database file is unavailable";return {};}
    return QString("posix:%1:%2").arg(qulonglong(info.st_dev),0,16).arg(qulonglong(info.st_ino),0,16);
#endif
}
}
}
