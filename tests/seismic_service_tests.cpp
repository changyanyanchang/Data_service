// Repository fixtures are generated synthetic examples; no private source data is bundled.
#include "paleo_data/service.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QtEndian>
#include <iostream>
#include <stdexcept>
#include <cstring>
using namespace paleo::data;
namespace {
std::string utf8(const QString& s){return s.toUtf8().toStdString();}
void check(bool ok,const std::string& message){if(!ok)throw std::runtime_error(message);}
void u16(QByteArray& bytes,int offset,quint16 value){qToBigEndian(value,bytes.data()+offset);}
void u32(QByteArray& bytes,int offset,quint32 value){qToBigEndian(value,bytes.data()+offset);}
void fixture(const QString& path,bool coordinates=true){
    QFile file(path);check(file.open(QIODevice::WriteOnly),"fixture open");QByteArray text(3200,' ');const QByteArray label("C01 TEST GRID ONLY");std::memcpy(text.data(),label.constData(),size_t(label.size()));file.write(text);
    QByteArray binary(400,'\0');u16(binary,16,2000);u16(binary,20,8);u16(binary,24,5);u16(binary,300,0x100);u16(binary,302,1);file.write(binary);
    for(int i=0;i<3;++i)for(int j=0;j<4;++j){if(i==1&&j==2)continue;QByteArray head(240,'\0');u32(head,0,i*4+j+1);u16(head,70,1);u16(head,88,1);u16(head,108,100);u16(head,114,8);u16(head,116,2000);u32(head,188,100+i*2);u32(head,192,200+j*2);
        if(coordinates){u32(head,180,500000+j*50);u32(head,184,4000000+i*100);}file.write(head);QByteArray samples(32,'\0');for(int s=0;s<8;++s){const float amplitude=float(i*100+j*10+s);quint32 bits=0;std::memcpy(&bits,&amplitude,4);u32(samples,s*4,bits);}file.write(samples);}
}
long long importVolume(DataService& service,const QString& file,std::string& error){ImportReport report;ImportOptions options;options.copySources=false;check(service.importFile(utf8(file),options,report,error),error);std::vector<DataEntry> volumes;check(service.listData("volumes",0,volumes,error),error);check(!volumes.empty(),"volume registered");return volumes.back().id;}
void confirm(DataService& service,long long id,std::string& error){SeismicVolume volume;check(service.readVolume(id,volume,error),error);check(volume.traces.empty(),"metadata must not load traces");volume.geometry.coordinateSystem="TEST_PROJECTED";volume.geometry.coordinateUnit="m";volume.sampling.firstTime=100;volume.sampling.sampleCount=8;volume.sampling.sampleInterval=2;volume.sampling.timeUnit="ms";volume.sampling.timeType="TWT";volume.sampling.timeReference="fixture datum";check(service.saveVolume(id,volume,error),error);}
}
int main(int argc,char**argv){QCoreApplication app(argc,argv);QCoreApplication::setApplicationName("paleo-data-seismic-tests");try{
    QTemporaryDir temp;check(temp.isValid(),"temporary directory");std::string error;DataService service;check(service.createProject(utf8(temp.path()+"/project"),"Seismic tests",error),error);
    const QString path=temp.path()+"/step2.sgy";fixture(path);const auto id=importVolume(service,path,error);SeismicTimeSliceResult slice;
    check(!service.readTimeSlice(id,104,{},slice,error)&&slice.amplitudes.empty(),"unknown reference rejected");confirm(service,id,error);
    TimeSliceReadOptions timeOptions;timeOptions.maxInlineCount=3;timeOptions.maxCrosslineCount=4;
    check(service.readTimeSlice(id,104.1,timeOptions,slice,error),error);check(slice.inlineNumbers==std::vector<int>({100,102,104})&&slice.crosslineNumbers==std::vector<int>({200,202,204,206}),"real step-2 axes");
    check(slice.actualTime==104&&slice.amplitudes.size()==12,"bounded nearest sample");check(slice.amplitudes[0]==2&&slice.amplitudes[11]==232,"time sample values");check(std::isnan(slice.amplitudes[6]),"missing cell is NaN, not neighbour");
    check(slice.x[0]==500000&&slice.y[11]==4000200,"actual coordinates");
    SeismicCoordinateRange range;check(service.readSeismicCoordinateRange(id,range,error),error);
    check(range.status=="complete"&&range.affineValid,"coordinate range complete");
    check(range.inlineMin==100&&range.inlineMax==104&&range.crosslineMin==200&&range.crosslineMax==206,"coordinate range axes");
    check(range.minX==500000&&range.maxX==500150&&range.minY==4000000&&range.maxY==4000200,"coordinate range bbox");
    check(range.corners.size()==4&&range.corners[0].inlineNumber==100&&range.corners[0].crosslineNumber==200&&range.corners[0].x==500000&&range.corners[0].y==4000000,"corner order starts at minimum axes");
    check(range.corners[1].x==500150&&range.corners[2].x==500150&&range.corners[2].y==4000200&&range.corners[3].x==500000,"corners close the ring");
    check(range.corners[0].tracePresent&&range.fitHeaderSamples>0&&std::isfinite(range.fitMaxResidual),"fit facts reported");
    check(!service.readSeismicCoordinateRange(id,range,error,[]{return true;}),"coordinate range cancellation");
    WellInfo north;north.wellName="A-1";north.wellheadX=499900;north.wellheadY=4000050;north.coordinateSystem="TEST_PROJECTED";north.coordinateUnit="m";long long northId=0;check(service.createWell(north,northId,error),error);
    WellInfo south=north;south.wellName="A-2";south.wellheadX=500300;south.wellheadY=3999900;long long southId=0;check(service.createWell(south,southId,error),error);
    WellHeadRange heads;check(service.readWellHeadRange(heads,error),error);
    check(heads.status=="complete"&&heads.extent.valid,"well head range standalone");
    check(heads.extent.minX==499900&&heads.extent.maxX==500300&&heads.extent.minY==3999900&&heads.extent.maxY==4000050,"well head MBR values");
    check(heads.wellCount==2&&heads.wellsWithCoordinates==2&&heads.wellsWithoutCoordinates==0&&heads.extent.pointCount==2,"well head counts");
    check(!service.readWellHeadRange(heads,error,[]{return true;}),"well head range cancellation");
    ProjectPlanarRange area;check(service.readProjectPlanarRange(area,error),error);
    check(area.status=="complete"&&area.merged.valid,"project area merged from wells and seismic");
    check(area.wells.minX==499900&&area.wells.maxX==500300&&area.wells.minY==3999900&&area.wells.maxY==4000050,"well head MBR");
    check(area.merged.minX==499900&&area.merged.maxX==500300&&area.merged.minY==3999900&&area.merged.maxY==4000200,"merged MBR covers both parts");
    check(area.merged.coordinateUnit=="m"&&area.wells.pointCount==2&&area.seismic.pointCount==4,"conventions carried, point counts counted");
    check(area.wellCount==2&&area.wellsWithCoordinates==2&&area.volumeCount==1&&area.volumesWithRange==1,"area counts with single volume");
    check(!service.readProjectPlanarRange(area,error,[]{return true;}),"project area cancellation");
    const QString missing=temp.path()+"/missing_coordinates.sgy";fixture(missing,false);const auto missingId=importVolume(service,missing,error);confirm(service,missingId,error);check(!service.readTimeSlice(missingId,104,{},slice,error),"missing trace coordinates rejected");
    ProjectPlanarRange twoVolumes;check(service.readProjectPlanarRange(twoVolumes,error),error);
    check(twoVolumes.status=="complete"&&twoVolumes.volumeCount==2&&twoVolumes.volumesWithRange==1&&twoVolumes.volumesWithoutRange.size()==1&&twoVolumes.volumesWithoutRange[0]==missingId,"volume without coordinates excluded from area");
    check(twoVolumes.merged.minX==499900&&twoVolumes.merged.maxY==4000200,"unlocatable volume does not distort merged area");
    check(!twoVolumes.warnings.empty(),"exclusion explained in warnings");
    WellInfo feet=north;feet.wellName="A-3";feet.coordinateUnit="ft";long long feetId=0;check(service.createWell(feet,feetId,error),error);
    ProjectPlanarRange mixed;check(service.readProjectPlanarRange(mixed,error),error);
    check(mixed.status=="conventions-mismatch"&&!mixed.merged.valid&&!mixed.wells.valid&&std::isnan(mixed.wells.minX),"mixed well units never merged");
    WellHeadRange mixedHeads;check(service.readWellHeadRange(mixedHeads,error),error);
    check(mixedHeads.status=="conventions-mismatch"&&!mixedHeads.extent.valid&&std::isnan(mixedHeads.extent.minX),"mixed well units rejected by well head range");
    timeOptions.maxValues=5;check(!service.readTimeSlice(id,104,timeOptions,slice,error)&&slice.amplitudes.empty(),"time budget enforced");
    timeOptions.maxValues=12;check(!service.readTimeSlice(id,104,timeOptions,slice,error,[]{return true;})&&slice.amplitudes.empty(),"cancel gives no partial values");
    check(!service.readTimeSlice(id,99,timeOptions,slice,error),"outside time rejected");
    SeismicSection section;section.sectionName="diagonal";section.parentVolumeId=int(id);section.coordinateSystem="TEST_PROJECTED";section.coordinateUnit="m";section.pathPoints={{500000,4000000},{500150,4000200}};long long sectionId=0;check(service.saveSection(sectionId,section,error),error);
    SectionReadOptions opts;opts.maxColumns=4;opts.firstTime=102;opts.lastTime=106;opts.maxValues=12;SeismicSectionResult result;check(service.extractSection(sectionId,opts,result,error),error);check(result.columns==4&&result.samples==3&&result.amplitudes.size()==12,"bounded section shape");check(result.firstTime==102&&result.amplitudes[0]==1&&result.amplitudes[11]==233,"section column-major values");
    check(!service.extractSection(sectionId,opts,result,error,[]{return true;})&&result.amplitudes.empty(),"section cancellation");
    section.coordinateSystem="OTHER_CRS";check(service.saveSection(sectionId,section,error),error);check(!service.extractSection(sectionId,opts,result,error),"CRS mismatch rejected");
    SeismicCoordinateRange missingRange;check(service.readSeismicCoordinateRange(missingId,missingRange,error),"missing coordinates still answers with domain facts");
    check(missingRange.status=="xy-unavailable"&&missingRange.corners.empty()&&std::isnan(missingRange.minX)&&!missingRange.affineValid,"no fabricated bbox without coordinates");
    check(missingRange.inlineMin==100&&missingRange.crosslineMax==206,"trace axes still reported");
    service.closeProject();check(service.openProject(utf8(temp.path()+"/project"),error),error);SeismicVolume reopened;check(service.readVolume(id,reopened,error)&&reopened.sampling.firstTime==100&&reopened.traces.empty(),"confirmed volume metadata survives reopen");
    DataService bare;check(bare.createProject(utf8(temp.path()+"/bare"),"Well-only area",error),error);
    WellInfo only=north;only.wellName="B-1";long long onlyId=0;check(bare.createWell(only,onlyId,error),error);
    ProjectPlanarRange wellsOnly;check(bare.readProjectPlanarRange(wellsOnly,error),error);
    check(wellsOnly.status=="partial"&&wellsOnly.merged.valid&&wellsOnly.merged.minX==499900&&wellsOnly.merged.maxX==499900&&wellsOnly.volumeCount==0,"wells-only area falls back to well MBR");
    WellHeadRange singleHead;check(bare.readWellHeadRange(singleHead,error),error);
    check(singleHead.status=="complete"&&singleHead.extent.minX==499900&&singleHead.extent.maxX==499900&&singleHead.extent.minY==4000050,"single well range is a point MBR");
    bare.closeProject();
    std::cout<<"Seismic adapter checks passed: bounded section/time slice, real axes, missing traces, metadata, CRS, cancellation, coordinate range, well head range, project area union\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
