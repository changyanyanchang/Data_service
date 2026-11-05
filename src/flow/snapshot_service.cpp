#include "store/project_store.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <algorithm>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace paleo::data {
using namespace detail;
namespace {
constexpr int PollMs=25;
const QByteArray Completion("paleo-snapshot-complete-v1");
QString workerPath() {
    const auto app=QCoreApplication::applicationDirPath();
    if(app.isEmpty())return {};
    const auto name=QStringLiteral("paleo_data_snapshot_worker")
#ifdef Q_OS_WIN
        +QStringLiteral(".exe")
#endif
        ;
    for(const auto& candidate:{QDir(app).filePath(name),QDir(app).filePath("../libexec/"+name)}) {
        const QFileInfo file(candidate);
        if(file.isFile()&&file.isExecutable())return file.canonicalFilePath();
    }
    return {};
}
}

bool DataService::snapshotDatabase(const std::string& destination,std::string& error)const {
    return snapshotDatabaseCancellable(destination,error,{},300000);
}

bool DataService::snapshotDatabaseCancellable(const std::string& destination,std::string& error,
                                              const CancelCheck& cancelled,int timeoutMs)const {
    error.clear();
    const auto cancel=[&] {if(cancelled&&cancelled()){error="已取消数据库快照";return true;}return false;};
    if(cancel())return false;
    const auto& store=impl_->store;
    if(!store.db.isOpen()){error="请先打开工区";return false;}
    if(destination.empty()||timeoutMs<1){error="快照目标或超时设置无效";return false;}
    const QFileInfo target(q(destination));
    if(target.exists()||target.isSymLink()){error="快照目标已存在，请选择新文件";return false;}
    const auto parent=target.absoluteDir();
    if(!parent.exists()){error="快照目标目录不存在";return false;}
    const auto worker=workerPath();
    if(worker.isEmpty()){error="缺少随应用安装的 paleo_data_snapshot_worker 快照辅助程序";return false;}
    const auto source=QFileInfo(store.db.databaseName()).canonicalFilePath();
    if(source.isEmpty()){error="快照来源数据库不存在";return false;}
    // The requested destination remains absent until a complete private copy is
    // ready. Cleanup owns only this newly generated directory, never user paths.
    QTemporaryDir stage(parent.filePath(".paleo-snapshot-XXXXXX"));
    if(!stage.isValid()){error="无法创建数据库快照暂存目录";return false;}
    const auto staged=QDir(stage.path()).filePath("data.sqlite");
    QStringList arguments{"--source",source,"--destination",staged};
    for(const auto& path:QCoreApplication::libraryPaths())arguments<<"--plugin-path"<<path;
    QProcess process;
    // A libexec worker must find the parent's deployed Qt DLLs as well as its
    // plugin directories. The executable itself is always an absolute path.
    auto environment=QProcessEnvironment::systemEnvironment();
    environment.insert("PATH",QCoreApplication::applicationDirPath()+QDir::listSeparator()+environment.value("PATH"));
    process.setProcessEnvironment(environment);
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args){args->flags|=CREATE_NO_WINDOW;});
#endif
    process.setProgram(worker);process.setArguments(arguments);
    QElapsedTimer elapsed;elapsed.start();
    process.start(QIODevice::ReadOnly);
    QByteArray output,diagnostic;
    const auto drain=[&] {
        output.append(process.readAllStandardOutput());diagnostic.append(process.readAllStandardError());
        if(output.size()>256)output=output.left(256);
        if(diagnostic.size()>2048)diagnostic=diagnostic.left(2048);
    };
    const auto stop=[&] {
        if(process.state()==QProcess::NotRunning)return;
        process.kill();
        if(!process.waitForFinished(3000)) {
            // An OS-level stuck process must not race removal of files it still
            // owns. No partial copy is published; retain only this private stage.
            stage.setAutoRemove(false);error+="；辅助进程尚未退出，已保留私有暂存目录";
        }
    };
    while(process.state()!=QProcess::NotRunning) {
        if(cancel()){stop();return false;}
        const auto remaining=static_cast<qint64>(timeoutMs)-elapsed.elapsed();
        if(remaining<=0){error="数据库快照超时";stop();return false;}
        process.waitForFinished(static_cast<int>(std::min<qint64>(PollMs,remaining)));drain();
    }
    drain();
    if(cancel())return false;
    if(elapsed.elapsed()>=timeoutMs){error="数据库快照超时";return false;}
    if(process.error()==QProcess::FailedToStart || process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0 || output.trimmed()!=Completion) {
        error="数据库快照辅助程序未完成："+s(QString::fromUtf8(diagnostic.isEmpty()?process.errorString().toUtf8():diagnostic));return false;
    }
    if(!QFileInfo(staged).isFile()||QFileInfo(staged).size()<100){error="数据库快照输出不完整";return false;}
    {
        QFile header(staged);
        if(!header.open(QIODevice::ReadOnly)||header.read(16)!=QByteArray("SQLite format 3\0",16)) {
            error="数据库快照输出不完整或格式无效";return false;
        }
    }
    if(cancel())return false;
    // QFile::rename refuses an existing destination, including a concurrent
    // arrival. Failure leaves that file untouched and removes only our stage.
    if(!QFile::rename(staged,target.absoluteFilePath())){error="数据库快照发布失败；目标可能已存在";return false;}
    return true;
}
}
