// Repository fixtures are generated synthetic examples; no private source data is bundled.
#include "paleo_data/parser.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QDirIterator>
#include <QTextStream>
#include <QStringConverter>
#include <iostream>
#include <stdexcept>
#include <cmath>

using namespace paleo::data;
#ifdef PALEO_TEXT_TEST_STUB
namespace paleo::data {
ParsedFile parseAuxiliaryFile(const std::string& path,const ParseOptions&,const CancelCheck&) { ParsedFile p;p.path=path;p.status="registered";return p; }
bool mapWellCoordinateTables(ParsedFile&,const ParseOptions&,const CancelCheck&) {return false;}
}
#endif
void check(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
std::string utf8(const QString& text) {return text.toUtf8().toStdString();}
std::string write(const QString& directory,const QString& name,const QByteArray& contents) {
    const QString path=directory+'/'+name;QFile file(path);check(file.open(QIODevice::WriteOnly),"write fixture");check(file.write(contents)==contents.size(),"write complete");return utf8(path);
}
bool has(const ParsedFile& p,const char* code) {for(const auto& i:p.issues)if(i.code==code)return true;return false;}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir temp;check(temp.isValid(),"temporary directory");
        const auto logs=write(temp.path(),"HZ1-1-1.csv",QByteArray("井号,深度,TVD,TVDSS,GR,SP,NPHI\nHZ1-1-1,100,99,,2,-30,-0.1\nHZ1-1-1,100.1,99.1,,-9999,-31,0.2\nHZ1-1-1,100.2,99.2,,bad,-32,0.3\n"));
        auto p=parseFile(logs);check(p.wells.size()==1,"one log well");const auto& set=p.wells[0].wellbore.logSets[0];
        check(set.samplingAxis.depthValues.size()==3&&set.logCurves.size()==3,"log shape");
        check(set.logCurves[0].sampleValues[1]==-9999,"raw null preserved");
        check(set.logCurves[1].sampleValues[0]==-30&&set.logCurves[2].sampleValues[0]==-0.1,"signed curves preserved");
        check(std::isnan(set.logCurves[0].sampleValues[2]),"bad token becomes missing");check(has(p,"NON_NUMERIC_CURVE"),"bad token issue");
        check(set.depthInfo.depthUnit.empty()&&set.depthInfo.depthType.empty(),"no guessed depth convention");
        const auto conflict=write(temp.path(),"HZ2-2-2.csv",QByteArray("井号,深度,GR\nHZ1-1-1,100,3\n"));
        p=parseFile(conflict);check(p.wells.empty()&&p.status=="review"&&has(p,"WELL_NAME_CONFLICT"),"identity conflict withheld");check(!p.sourceTables.empty(),"conflict preview");
        ParseOptions mapping;mapping.wellNameOverride="HZ2-2-2";p=parseFile(conflict,mapping);check(p.wells.size()==1&&p.wells[0].wellInfo.wellName=="HZ2-2-2","identity correction");
        const auto formation=write(temp.path(),"HZ3-3-3.csv",QByteArray("井号,道名,层号,顶深,底深,相类型,顶部不整合,底部不整合,底部未钻穿\nHZ3-3-3,段,恩平三段,100.5,200,扇根,False,False,True\n"));
        p=parseFile(formation);const auto& interval=p.wells[0].wellbore.intervalTracks[0].intervals[0];
        check(interval.bottomDepth==200&&interval.bottomNotPenetrated==1&&interval.topUnconformity==0,"undrilled stop preserved");check(interval.faciesType=="扇根"&&interval.sourceRow==2,"facies and source row");
        const QString coordPath=temp.path()+"/wells2.txt";{QFile file(coordPath);check(file.open(QIODevice::WriteOnly),"coordinate fixture");QTextStream stream(&file);stream.setEncoding(QStringConverter::Utf16);stream.setGenerateByteOrderMark(true);stream<<"HZ1-1-1,2000000,300000\n";}
        p=parseFile(utf8(coordPath));check(p.wells.size()==1&&std::isnan(p.wells[0].wellInfo.wellheadX),"unknown coordinate withheld");check(p.sourceTables[0].cells.size()==3,"raw coordinate kept");
        mapping={};mapping.coordinateOrder="yx";mapping.coordinateSystem="TEST_CRS";mapping.coordinateUnit="m";
        p=parseFile(utf8(coordPath),mapping);check(p.wells[0].wellInfo.wellheadX==300000&&p.wells[0].wellInfo.wellheadY==2000000,"explicit axes honored");
#ifndef PALEO_TEXT_TEST_STUB
        const auto labelled=write(temp.path(),"coordinate-table.csv",QByteArray("井号,X（纵坐标）,Y（横坐标）\nSYNTH-1,2360000,300000\nSYNTH-2,2361000,0\nTOTAL,,\n"));
        p=parseFile(labelled);check(p.kind=="coordinates"&&p.wells.size()==2,"labelled coordinate table produces only actual wells");
        check(p.wells[0].wellInfo.wellheadX==300000&&p.wells[0].wellInfo.wellheadY==2360000,"semantic horizontal/vertical labels override X/Y prefixes");
        check(p.wells[1].wellInfo.wellheadX==0&&p.wells[0].wellInfo.coordinateSystem.empty()&&p.wells[0].wellInfo.coordinateUnit.empty(),"zero is valid and conventions remain unknown");
        p=parseFile(labelled,mapping);check(p.wells[0].wellInfo.coordinateSystem=="TEST_CRS"&&p.wells[0].wellInfo.coordinateUnit=="m","explicit coordinate reference preserved");
        const auto duplicate=write(temp.path(),"duplicate-coordinates.csv","WELL,X,Y\nSYNTH-1,100,200\nSYNTH-1,101,201\n");
        p=parseFile(duplicate);check(p.wells.size()==1&&std::isnan(p.wells[0].wellInfo.wellheadX)&&has(p,"coordinate_row_invalid"),"duplicate coordinates do not choose a location");
#endif
        const auto horizon=write(temp.path(),"t70.dat",QByteArray("INLINE : 10 XLINE : 20 1000.25 2000.50 100.25\nINVALID\nINLINE: 10 XLINE: 21 1001.25 2001.50 101.50\n"));
        p=parseFile(horizon);check(p.horizons.size()==1&&p.horizons[0].points.size()==2,"horizon shape");check(p.horizons[0].points[0].zTime==100.25&&p.horizons[0].timeUnit.empty(),"horizon time preserved");check(has(p,"HORIZON_BAD_ROW"),"horizon reject visible");
        const auto empty=write(temp.path(),"empty.sgy",{});p=parseFile(empty);check(p.status=="failed"&&p.volumes.empty(),"empty SEG-Y rejected");
        p=parseFile(logs,{},[]{return true;});check(p.status=="failed"&&p.wells.empty(),"cancellation no partial result");
        const auto las=write(temp.path(),"HZ4-4-4.las",QByteArray("~Version\nVERS. 2.0 : version\nWRAP. NO : wrap\n~Well\nWELL. HZ4-4-4 : well\nNULL. -12345 : declared null\n~Curve\nDEPT.M : depth\nGR.API : gamma ray\nSP.MV : signed potential\n~ASCII\n100 20 -30\n101 -12345 -31\n"));
        p=parseFile(las);check(p.wells.size()==1&&p.declaredNullValues.size()==1&&p.declaredNullValues[0]==-12345,"LAS declared null");check(p.wells[0].wellbore.logSets[0].logCurves[0].sampleValues[1]==-12345,"LAS raw null");
        std::cout<<"Synthetic parser checks passed\n";
        // Optional local acceptance checks are shape-based: no private dataset
        // identities, coordinates, or expected measurements are embedded here.
        if(argc>1) {
            QDirIterator files(app.arguments()[1],QDir::Files,QDirIterator::Subdirectories);
            int checked=0;
            while(files.hasNext()) {
                const QString path=files.next();const auto ext=files.fileInfo().suffix().toLower();
                if(ext!="csv"&&ext!="las"&&ext!="dat"&&ext!="txt")continue;
                auto parsed=parseFile(utf8(path));check(!parsed.status.empty(),"external source has a status");
                for(const auto& well:parsed.wells)for(const auto& log:well.wellbore.logSets)
                    for(const auto& curve:log.logCurves)check(curve.sampleValues.size()==log.samplingAxis.depthValues.size(),"external curve matches its depth axis");
                ++checked;
            }
            std::cout<<"external_files_checked="<<checked<<'\n';
        }
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
