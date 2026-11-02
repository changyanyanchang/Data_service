// Synthetic fixtures only. Reuse the bounded SEG-Y generator, not its test main.
#define main seismicAdapterContractMain
#include "seismic_service_tests.cpp"
#undef main
#include <QCryptographicHash>

int main(int argc,char** argv){
  QCoreApplication app(argc,argv);
  try{
    QTemporaryDir temp;check(temp.isValid(),"temporary overlay fixture");
    DataService ds;std::string error;const auto root=utf8(temp.path()+"/overlay");
    check(ds.createProject(root,"Overlay",error),error);
    const auto file=temp.filePath("volume.sgy");fixture(file);const auto volumeId=importVolume(ds,file,error);
    const auto source=temp.filePath("horizons.txt");QFile raw(source);check(raw.open(QIODevice::WriteOnly),"horizon source");raw.write("synthetic horizons");raw.close();
    ParsedFile parsed;parsed.kind="test";parsed.status="parsed";
    for(int h=0;h<4;++h){
      SeismicHorizon surface;surface.horizonName=h==0?"T70":h==1?"T72":h==2?"NEG":"SPARSE";
      surface.timeUnit=h==0?"s":"ms";surface.timeType=h==0?"OWT":"TWT";
      for(int i:{100,h==3?140:104})for(int x:{200,h==3?240:204}){
        const double top=100+(i-100)*2.5+(x-200)*5.;
        const double time=h==0?top/2000.:h==1?top+40:h==2?-9.:top;
        surface.points.push_back({i,x,500000.+x,4000000.+i,time});
      }
      if(h==0)surface.points.push_back({104,202,500202,4000104,Missing});
      parsed.horizons.push_back(surface);
    }
    long long sourceId=0;
    const auto hash=QCryptographicHash::hash("synthetic horizons",QCryptographicHash::Sha256).toHex().toStdString();
    check(ds.importPreparedFile(utf8(source),utf8(source),utf8(source),hash,parsed,sourceId,error),error);
    std::vector<DataEntry> horizons;check(ds.listData("horizons",0,horizons,error)&&horizons.size()==4,error);
    StratigraphicScheme scheme;scheme.id="default";scheme.name="Synthetic scheme";
    for(int i=0;i<2;++i){StratigraphicBoundary b;b.name=i?"T72":"T70";b.order=i;b.seismicHorizonResourceId=horizons[i].id;scheme.boundaries.push_back(b);}
    framework::deriveUnits(scheme);StratigraphicScheme committed;std::vector<FrameworkIssue> issues;
    check(ds.commitStratigraphicScheme(scheme,{},committed,issues,error),error);
    SectionHorizonQuery query;query.volumeId=volumeId;query.undeclaredTimeUnit="ms";query.undeclaredTimeType="TWT";
    query.columns={{100,200,true},{100,202,true},{104,202,true},{100,200,false},{110,210,true},{100,200,true}};
    SectionHorizonResult result;check(ds.readSectionHorizons(query,result,error),error);
    const auto named=[&](const std::string& name)->const SectionHorizonCurve&{
      const auto it=std::find_if(result.curves.begin(),result.curves.end(),[&](const auto& c){return c.name==name;});
      check(it!=result.curves.end(),"expected curve "+name);return *it;
    };
    check(result.curves.size()==7,"four major horizons plus three internal edges");
    check(result.bands.size()==5,"explicit major interval plus four subinterval bands");
    check(result.bands[1].topCurveId==named("T70").id&&result.bands[4].bottomCurveId==named("T72").id,
          "bands use scheme surface links, never negative horizon or display sorting");
    check(result.bands[2].topCurveId==result.bands[1].bottomCurveId&&result.bands[2].subdivision==2,
          "S2 shares the actual S1/S2 edge");
    check(named("T70").timesMs[0]==100&&std::abs(named("T70").timesMs[1]-110)<1e-9,"exact OWT seconds and bounded interpolation become TWT ms");
    check(std::isnan(named("T70").timesMs[2])&&std::isnan(named("T70").timesMs[3])&&std::isnan(named("T70").timesMs[4]),"explicit null, missing trace and outside remain gaps");
    check(named("T70").timesMs[5]==100&&named("NEG").timesMs[0]==-9,"duplicate column order and signed source time preserved");
    const auto edge=std::find_if(result.curves.begin(),result.curves.end(),[](const auto& c){return c.internalEdge==1;});
    check(edge!=result.curves.end()&&edge->provisional&&edge->timesMs[0]==110&&std::isnan(edge->timesMs[2]),"provisional S1/S2 follows both actual surfaces, gaps retained");
    const auto oldRevision=committed.revision.value;
    committed.divisions[0].fractions={.1,.5,.9};committed.divisions[0].mode="interpreted";committed.divisions[0].edgeStatus={"confirmed","confirmed","confirmed"};
    check(ds.commitStratigraphicScheme(committed,oldRevision,scheme,issues,error),error);
    check(ds.readSectionHorizons(query,result,error),error);
    const auto updated=std::find_if(result.curves.begin(),result.curves.end(),[](const auto& c){return c.internalEdge==1;});
    check(updated!=result.curves.end()&&!updated->provisional&&updated->timesMs[0]==104,"scheme revision changes actual edge, not fixed quartiles");
    query.revision=oldRevision;check(ds.readSectionHorizons(query,result,error),error);
    check(result.revision==oldRevision&&result.bands.size()==5,"explicit historical revision and band topology retained");
    check(!ds.readSectionHorizons(query,result,error,[]{return true;})&&result.curves.empty(),"cancel has no stale output");
    check(ds.readSectionHorizons(query,result,error),"read snapshot released after cancellation");
    ds.closeProject();check(QFile::remove(file),"external volume removed to prove metadata-only query");
    check(ds.openProjectReadOnly(root,error),error);check(ds.readSectionHorizons(query,result,error)&&result.curves.size()==7,"readonly reopen works without SEG-Y amplitudes");
    query.columns={{120,220,true}};query.revision.clear();check(ds.readSectionHorizons(query,result,error),error);
    check(named("SPARSE").timesMs[0]==250&&result.bands.empty(),"40-line sparse grid interpolates; unrelated surfaces do not invent intervals");
    query.interpolationRadius=30;check(ds.readSectionHorizons(query,result,error)&&result.curves.empty(),"oversize cells do not create isolated interpolated islands at their centre");
    query.interpolationRadius=2;check(ds.readSectionHorizons(query,result,error)&&result.curves.empty(),"explicit small neighbourhood remains bounded");
    query.columns.clear();check(!ds.readSectionHorizons(query,result,error)&&result.curves.empty(),"invalid query clears old data");
    std::cout<<"section horizon overlays: order, gaps, signed time, units, scheme revisions, cancellation and readonly metadata passed\n";
    return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
