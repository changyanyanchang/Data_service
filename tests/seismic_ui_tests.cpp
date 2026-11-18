// Repository fixtures are generated synthetic examples; no private source data is bundled.
#include "apidemo.h"
#include "plotwidgets.h"
#include "paleo_data/service.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace paleo::data;
namespace {
std::string utf8(const QString& value){return value.toUtf8().toStdString();}
void check(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
void fixture(const QString& path,bool muteEnds=false){
    QFile file(path);check(file.open(QIODevice::WriteOnly),"create SEG-Y fixture");
    QByteArray text(3200,' ');const QByteArray label("C01 SYNTHETIC UI TEST DATA ONLY");std::memcpy(text.data(),label.constData(),size_t(label.size()));file.write(text);
    QByteArray binary(400,'\0');qToBigEndian<quint16>(2000,binary.data()+16);qToBigEndian<quint16>(128,binary.data()+20);qToBigEndian<quint16>(5,binary.data()+24);qToBigEndian<quint16>(0x100,binary.data()+300);qToBigEndian<quint16>(1,binary.data()+302);file.write(binary);
    for(int i=0;i<3;++i)for(int j=0;j<4;++j){
        QByteArray header(240,'\0');qToBigEndian<qint32>(i*4+j+1,header.data());qToBigEndian<qint16>(1,header.data()+70);qToBigEndian<qint16>(1,header.data()+88);qToBigEndian<qint16>(100,header.data()+108);qToBigEndian<quint16>(128,header.data()+114);qToBigEndian<quint16>(2000,header.data()+116);qToBigEndian<qint32>(500000+j*50,header.data()+180);qToBigEndian<qint32>(4000000+i*100,header.data()+184);qToBigEndian<qint32>(100+i*2,header.data()+188);qToBigEndian<qint32>(200+j*2,header.data()+192);file.write(header);
        QByteArray samples(512,'\0');for(int k=0;k<128;++k){const float value=muteEnds&&(k<10||k>=118)?0.0f:float(i*1000+j*100+k-64);quint32 bits=0;std::memcpy(&bits,&value,4);qToBigEndian(bits,samples.data()+k*4);}file.write(samples);
    }
}
QJsonObject json(const ApiDemoWidget& widget){return QJsonDocument::fromJson(widget.lastResultJson().toUtf8()).object();}
template<class T>T* control(ApiDemoWidget& widget,const char* name){auto* value=widget.findChild<T*>(name);check(value!=nullptr,std::string("missing control ")+name);return value;}
bool run(ApiDemoWidget& widget,bool cancel=false){
    QEventLoop loop;QTimer timeout;timeout.setSingleShot(true);bool done=false,success=false,timedOut=false;
    const auto a=QObject::connect(&widget,&ApiDemoWidget::runFinished,&loop,[&](bool ok){done=true;success=ok;loop.quit();});
    const auto b=QObject::connect(&widget,&ApiDemoWidget::busyChanged,&loop,[&](bool busy){if(cancel&&busy)widget.requestCancel();});
    QObject::connect(&timeout,&QTimer::timeout,&loop,[&]{timedOut=true;widget.requestCancel();loop.quit();});timeout.start(10000);widget.runExample();if(!done)loop.exec();QObject::disconnect(a);QObject::disconnect(b);
    check(!timedOut&&done,"native seismic UI request timed out");check(!widget.isRunning(),"worker not finished");return success;
}
void cleared(const ApiDemoWidget& widget){check(widget.lastResultJson().isEmpty()&&!widget.lastRunSucceeded(),"stale results remain after parameters changed");}
}

int main(int argc,char** argv){QApplication app(argc,argv);try{
    QTemporaryDir temporary;check(temporary.isValid(),"temporary dir");const QString path=temporary.path()+QStringLiteral("/中文地震文件.sgy");fixture(path);
    const QString project=temporary.path()+"/project";DataService service;std::string error;check(service.createProject(utf8(project),"UI SEG-Y test",error),error);
    ImportOptions options;options.copySources=false;ImportReport report;check(service.importFile(utf8(path),options,report,error),error);
    std::vector<DataEntry> entries;check(service.listData("volumes",0,entries,error)&&entries.size()==1,error);const auto id=entries[0].id;
    ProjectCoordinates projectCoordinates;check(service.readProjectCoordinates(projectCoordinates,error),error);
    SeismicVolume volume;check(service.readVolume(id,volume,error),error);
    check(volume.geometry.coordinateSystem==projectCoordinates.coordinateSystem&&
          volume.geometry.coordinateUnit==projectCoordinates.coordinateUnit&&
          volume.sampling.timeType.empty()&&volume.sampling.timeReference.empty(),
          "volume must inherit the project CRS without guessing its time conventions");
    ApiDemoWidget widget;widget.setProjectPath(project);auto* first=control<QLineEdit>(widget,"apiFirstValue");auto* last=control<QLineEdit>(widget,"apiLastValue");

    widget.setExample("seismic_info");check(widget.selectResource(id),"select volume for inspect");check(first->isHidden()&&last->isHidden(),"inspection should have no numeric controls");first->setText("invalid hidden parameter");last->setText("1e100");check(run(widget),"inspect UI failed");auto out=json(widget);check(out["returnType"].toString()=="SeismicFileInfo"&&out["traceCount"].toInteger()==12&&out["sampleCount"].toInt()==128,"inspection metadata mismatch");check(out["sampleInterval"].toDouble()==2&&out["firstTime"].toDouble()==100,"inspection time axis mismatch");check(widget.generatedCode().contains("service.inspectSeismic"),"inspection code missing");

    widget.setExample("seismic_trace");cleared(widget);first->setText("0");last->setText("8");check(run(widget),"trace UI failed without CRS");out=json(widget);check(out["method"].toString()=="readSeismicTrace"&&out["traceIndex"].toInteger()==0&&out["amplitudeCount"].toInt()==8,"trace identity or size");check(out["preview"].toArray()[0].toObject()["amplitude"].toDouble()==-64&&out["preview"].toArray()[7].toObject()["amplitude"].toDouble()==-57,"signed raw trace values changed");check(out["statistics"].toObject()["mean"].toDouble()==-60.5,"trace mean mismatch");check(control<QWidget>(widget,"apiSeismicTracePlot")!=nullptr,"time-axis waveform missing");
    first->setText("-1");cleared(widget);check(!run(widget)&&widget.lastResultJson().isEmpty(),"negative index should be rejected before service");first->setText("1e100");check(!run(widget),"out-of-range integer must be rejected");first->clear();last->clear();check(run(widget),"default trace failed");check(json(widget)["amplitudeCount"].toInt()==128,"default trace must return all samples");

    widget.setExample("inline");auto* hd=control<QCheckBox>(widget,"apiFullResolution");check(hd->isChecked()&&!last->isEnabled(),"native seismic must default to HD with preview cap disabled");first->setText("102");last->setText("3");check(run(widget),"native HD INLINE failed");out=json(widget);check(out["columns"].toInt()==4&&out["nativeColumns"].toInt()==4&&!out["decimated"].toBool()&&out["fullResolutionRequested"].toBool(),"HD request must retain every source trace despite old preview cap");check(widget.generatedCode().contains("options.fullResolution = true")&&widget.generatedCode().contains("options.maxValues = 64000000"),"HD generated code differs from request");
    hd->setChecked(false);cleared(widget);check(last->isEnabled(),"preview axis limit should be enabled after HD is unchecked");check(run(widget),"native INLINE preview failed");out=json(widget);check(out["lineNumber"].toInt()==102&&out["columns"].toInt()==3&&out["samples"].toInt()==128&&out["decimated"].toBool(),"inline preview shape or selection");check(out["preview"].toArray()[0].toObject()["amplitude"].toDouble()==936,"inline first amplitude");check(out["columnPreview"].toArray()[0].toObject()["inline"].toInt()==102,"inline trace lineage missing");check(!out.contains("sectionId"),"volume ID must not be mislabeled section ID");hd->setChecked(true);
    widget.setExample("crossline");first->setText("204");check(run(widget),"native XLINE failed");out=json(widget);check(out["direction"].toString()=="crossline"&&out["columns"].toInt()==3,"crossline selection");check(out["preview"].toArray()[0].toObject()["amplitude"].toDouble()==136,"crossline amplitude");

    widget.setExample("seismic_slice");first->setText("228");last->setText("4");check(run(widget),"native slice failed without TWT");out=json(widget);check(out["method"].toString()=="readSeismicTimeSlice"&&out["actualTime"].toDouble()==228&&out["amplitudeCount"].toInt()==12,"native slice shape/time");check(out["preview"].toArray()[0].toObject()["amplitude"].toDouble()==0,"zero sample must remain valid");first->clear();last->clear();check(run(widget),"default midpoint slice failed");check(json(widget)["requestedTime"].toDouble()==228,"midpoint time differs from displayed code");
    check(out["nativeInlineCount"].toInt()==3&&out["nativeCrosslineCount"].toInt()==4&&!out["decimated"].toBool(),"native slice resolution metadata lost");hd->setChecked(false);last->setText("2");check(run(widget),"explicit preview slice failed");check(json(widget)["amplitudeCount"].toInt()==4&&json(widget)["decimated"].toBool(),"preview choice must return a visibly labelled sampled grid");hd->setChecked(true);last->setText("invalid disabled preview cap");check(run(widget)&&json(widget)["amplitudeCount"].toInt()==12,"HD must ignore disabled preview cap");

    // Range inputs select actual numbered lines, and match the generated public API code.
    auto* ilMin=control<QLineEdit>(widget,"apiInlineMin");auto* ilMax=control<QLineEdit>(widget,"apiInlineMax");
    auto* xlMin=control<QLineEdit>(widget,"apiCrosslineMin");auto* xlMax=control<QLineEdit>(widget,"apiCrosslineMax");
    auto* workers=control<QComboBox>(widget,"apiSliceWorkers");auto* io=control<QComboBox>(widget,"apiSliceIoMode");
    auto* cache=control<QCheckBox>(widget,"apiSliceCache");auto* timings=control<QCheckBox>(widget,"apiSliceTimings");auto* preview=dynamic_cast<ImagePreview*>(control<QWidget>(widget,"apiSeismicPreview"));check(preview!=nullptr,"seismic preview has unexpected widget type");
    check(ilMin->text().isEmpty()&&ilMax->text().isEmpty()&&xlMin->text().isEmpty()&&xlMax->text().isEmpty()&&workers->currentData().toInt()==2&&cache->isChecked(),"slice defaults unexpectedly restrict coverage");
    first->setText("228");ilMin->setText("102");ilMax->setText("104");xlMin->setText("202");xlMax->setText("204");workers->setCurrentIndex(workers->findData(4));io->setCurrentIndex(io->findData("buffered"));cache->setChecked(false);timings->setChecked(true);
    check(run(widget),"ROI slice failed");out=json(widget);check(out["amplitudeCount"].toInt()==4&&out["range"].toObject()["inlineMin"].toInt()==102,"ROI shape/range metadata mismatch");
    const auto roi=out["preview"].toArray();check(roi.size()==4&&roi[0].toObject()["amplitude"].toDouble()==1100&&roi[1].toObject()["amplitude"].toDouble()==1200&&roi[2].toObject()["amplitude"].toDouble()==2100&&roi[3].toObject()["amplitude"].toDouble()==2200,"ROI did not retain native amplitudes/order");
    check(out["timingsCollected"].toBool()&&out["readStatistics"].toObject()["totalTiles"].toInt()>0,"read diagnostics missing");check(widget.property("apiTilesDisplayed").toInt()>0&&!preview->isPartial()&&preview->nativeImageSize()==QSize(2,2),"progress callback or final replacement missing");
    check(widget.generatedCode().contains("options.inlineMin = 102")&&widget.generatedCode().contains("options.crosslineMax = 204")&&widget.generatedCode().contains("options.workerCount = 4")&&widget.generatedCode().contains("options.useCache = false")&&widget.generatedCode().contains("options, result, error, {}, onTile"),"ROI generated example differs from execution");
    ilMin->setText("105");check(!run(widget)&&preview->nativeImage().isNull(),"inverted ROI retained stale image");ilMin->setText("102");
    DataService::clearSeismicSliceCache();cache->setChecked(true);check(run(widget),"cache population failed");first->setText("230");check(run(widget),"adjacent cached time failed");out=json(widget);check(out["readStatistics"].toObject()["cacheHits"].toInt()>0&&out["preview"].toArray()[0].toObject()["amplitude"].toDouble()==1101,"adjacent cache hit lost requested time");
    check(!run(widget,true)&&preview->nativeImage().isNull(),"cancelled slice retained partial/full image");
    // A programmatic parameter change during a request invalidates all queued tiles.
    const auto changeWhileRunning=QObject::connect(&widget,&ApiDemoWidget::busyChanged,&widget,[&](bool busy){if(busy)first->setText("232");});
    check(!run(widget)&&preview->nativeImage().isNull()&&widget.lastResultJson().isEmpty(),"stale request published a tile/result");QObject::disconnect(changeWhileRunning);
    for(auto* edit:{ilMin,ilMax,xlMin,xlMax})edit->clear();workers->setCurrentIndex(workers->findData(2));io->setCurrentIndex(io->findData("auto"));timings->setChecked(false);

    // Ordered XY points form an actual bent wall; input geometry is not stored implicitly.
    widget.setExample("wall");check(widget.selectResource(id),"select wall volume");auto* coordinates=control<QPlainTextEdit>(widget,"apiWallCoordinates");coordinates->setPlainText("500000,4000000\n500150,4000000\n500150,4000200");
    check(!run(widget),"XY wall must reject unconfirmed coordinate/time conventions");
    volume.geometry.coordinateSystem="SYNTHETIC_LOCAL";volume.geometry.coordinateUnit="m";volume.sampling.timeType="TWT";volume.sampling.timeReference="synthetic test datum";check(service.saveVolume(id,volume,error),error);
    check(run(widget),"direct coordinate wall failed after confirming conventions");out=json(widget);check(out["method"].toString()=="extractSection"&&out["inputPointCount"].toInt()==3&&out["pathVertices"].toArray().size()==3&&!out["pathSaved"].toBool(),"wall result lost direct request geometry");check(out["volumeId"].toInteger()==id&&!out.contains("sectionId"),"direct wall volume mislabeled as saved section");const auto vertices=out["pathVertices"].toArray();check(vertices[1].toObject()["x"].toDouble()==500150&&vertices[1].toObject()["y"].toDouble()==4000000,"wall bend omitted or changed");check(widget.generatedCode().contains("service.extractSection(path, options")&&widget.generatedCode().contains("{500150, 4000200}")&&widget.generatedCode().contains("result.pathX[column]"),"wall generated code must show actual requested coordinates and returned geometry");
    std::vector<DataEntry> sections;check(service.listData("sections",0,sections,error)&&sections.empty(),"direct wall request must not persist an unrequested path");coordinates->setPlainText("500000,4000000\nbad-point");cleared(widget);check(!run(widget)&&widget.lastResultJson().isEmpty(),"invalid coordinate text must not run or retain previous wall");
    coordinates->setPlainText("500000,4000000\n500150,4000000\n500150,4000200");check(!run(widget,true),"cancelled wall published partial result");check(run(widget),"wall did not recover after cancellation");

    widget.setExample("processed_section");first->setText("102");check(run(widget),"processed section failed");out=json(widget);check(out["processing"].toObject()["agcWindowSamples"].toInt()==101&&!out["processing"].toObject()["writesSource"].toBool()&&!out["processing"].toObject()["removeMean"].toBool(),"processing provenance missing or default de-mean enabled");check(out["rawStatistics"].toObject()["minimum"].toDouble()==936,"raw statistics lost");const auto stats=out["statistics"].toObject();check(stats["minimum"].toDouble()>=-3&&stats["maximum"].toDouble()<=3,"clip bound violated");check(widget.generatedCode().contains("service.processSeismicSection")&&widget.generatedCode().contains("agcWindowSamples = 101")&&widget.generatedCode().contains("processing.removeMean = false"),"generated processing code differs from execution");

    // Muted source samples must stay zero under the default preview settings.
    // Otherwise subtracting the nonzero trace mean can invent stripes after AGC.
    const QString mutedPath=temporary.path()+QStringLiteral("/静音首尾.sgy");fixture(mutedPath,true);check(service.importFile(utf8(mutedPath),options,report,error),error);
    entries.clear();check(service.listData("volumes",0,entries,error)&&entries.size()==2,error);long long mutedId=0;for(const auto& entry:entries)if(entry.id!=id)mutedId=entry.id;check(mutedId>0,"muted fixture volume missing");
    widget.setProjectPath(project);widget.setExample("processed_section");check(widget.selectResource(mutedId),"select muted fixture");first->setText("102");check(run(widget),"muted section preview failed");out=json(widget);for(const auto& point:out["preview"].toArray())check(point.toObject()["amplitude"].toDouble(-999)==0,"GUI default processing changed muted source sample");
    SeismicSectionResult rawMuted,processedMuted;SectionReadOptions mutedOptions;mutedOptions.maxColumns=4;check(service.readSeismicLine(mutedId,"inline",102,mutedOptions,rawMuted,error),error);SeismicProcessingOptions defaults;defaults.removeMean=false;defaults.gain=1;defaults.agcWindowSamples=101;defaults.clipAmplitude=3;check(service.processSeismicSection(rawMuted,defaults,processedMuted,error),error);size_t zeroCount=0;for(size_t i=0;i<rawMuted.amplitudes.size();++i)if(rawMuted.amplitudes[i]==0){++zeroCount;check(processedMuted.amplitudes[i]==0,"default AGC/clip changed an originally zero sample");}check(zeroCount==80,"muted fixture must include both source ends on all four traces");
    check(widget.selectResource(id),"restore original volume");

    widget.setExample("inline");check(run(widget),"automatic middle line failed");check(json(widget)["lineNumber"].toInt()==102,"automatic middle line mismatch");check(!run(widget,true),"cancelled request published success");out=json(widget);check(!out["ok"].toBool()&&!out.contains("preview"),"cancelled request exposed partial success");
    check(run(widget),"recovery after cancellation failed");
    DataService empty;const QString second=temporary.path()+"/empty";check(empty.createProject(utf8(second),"Empty",error),error);widget.setProjectPath(second);cleared(widget);check(control<QComboBox>(widget,"apiResourceSelector")->count()==0&&!widget.selectResource(id),"volumes leaked across projects");
    std::cout<<"PASS seismic UI: real info/trace/inline/crossline/native slice/processing, signed values, no guessed CRS/TWT, code parity, invalidation, cancellation, project isolation\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL seismic UI: "<<error.what()<<'\n';return 1;}}
