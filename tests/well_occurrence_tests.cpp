#include "paleo_data/service.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QUuid>
#include <algorithm>

using namespace paleo::data;
namespace fw=paleo::data::framework;
namespace {
StratigraphicScheme scheme() {
    StratigraphicScheme value;value.id="repeated";value.name="Synthetic repeats";
    for(int i=0;i<2;++i){StratigraphicBoundary b;b.name=i?"Base":"Top";b.order=i;value.boundaries.push_back(b);}
    fw::deriveUnits(value);
    for(int i=0;i<2;++i){WellIntervalOccurrence o;o.wellId=1;o.interval=value.intervals.front().id;
        o.top=100+i*100;o.bottom=110+i*100;value.wellOccurrences.push_back(o);}
    return value;
}
const StratigraphicSubInterval &sub(const StratigraphicScheme &value,int index=1) {
    return *std::find_if(value.subIntervals.begin(),value.subIntervals.end(),[index](const auto &v){return v.index==index;});
}
}
class WellOccurrenceTests:public QObject {
    Q_OBJECT
private slots:
    void legacyRevisionRemainsUnmodifiedUntilExplicitNewCommit() {
        QTemporaryDir temp;DataService service;std::string error;QVERIFY(service.createProject(temp.path().toStdString(),"legacy",error));
        auto input=scheme();input.wellOccurrences.resize(1);StratigraphicScheme saved;std::vector<FrameworkIssue>issues;
        QVERIFY(service.commitStratigraphicScheme(input,{},saved,issues,error));service.closeProject();
        const auto connection=QUuid::createUuid().toString();
        {
            auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(temp.filePath("data.sqlite"));QVERIFY(db.open());
            QSqlQuery query(db);QVERIFY(query.exec("SELECT payload FROM strat_scheme_revisions"));QVERIFY(query.next());
            auto json=QJsonDocument::fromJson(query.value(0).toByteArray()).object();auto occurrences=json.value("wellOccurrences").toArray();
            auto occurrence=occurrences.first().toObject();occurrence.remove("occurrenceId");occurrences[0]=occurrence;json.insert("wellOccurrences",occurrences);
            const auto payload=QJsonDocument(json).toJson(QJsonDocument::Compact);query.finish();
            QVERIFY(query.prepare("UPDATE strat_scheme_revisions SET payload=?,digest=?"));query.addBindValue(payload);
            query.addBindValue(QString::fromLatin1(QCryptographicHash::hash(payload,QCryptographicHash::Sha256).toHex()));QVERIFY(query.exec());
        }
        QSqlDatabase::removeDatabase(connection);
        QVERIFY(service.openProjectReadOnly(temp.path().toStdString(),error));StratigraphicScheme legacy;
        QVERIFY(service.readStratigraphicScheme(saved.id,saved.revision.value,legacy,error));QVERIFY(legacy.wellOccurrences.front().occurrenceId.empty());
        QVERIFY(fw::wellRange(legacy,1,sub(legacy).id).resolved);service.closeProject();
        QVERIFY(service.openProject(temp.path().toStdString(),error));StratigraphicScheme current;
        QVERIFY(service.commitStratigraphicScheme(legacy,saved.revision.value,current,issues,error));QVERIFY(!current.wellOccurrences.front().occurrenceId.empty());
        QVERIFY(service.readStratigraphicScheme(saved.id,saved.revision.value,legacy,error));QVERIFY(legacy.wellOccurrences.front().occurrenceId.empty());
        QCOMPARE(legacy.wellOccurrences.front().top,100.);
    }
    void disjointRepeatsPersistWithStableIdentity() {
        QTemporaryDir temp;DataService service;std::string error;QVERIFY2(service.createProject(temp.path().toStdString(),"test",error),error.c_str());
        StratigraphicScheme saved;std::vector<FrameworkIssue>issues;
        QVERIFY2(service.commitStratigraphicScheme(scheme(),{},saved,issues,error),error.c_str());QCOMPARE(saved.wellOccurrences.size(),size_t(2));
        const auto first=saved.wellOccurrences[0].occurrenceId,second=saved.wellOccurrences[1].occurrenceId;
        QVERIFY(!first.empty());QVERIFY(!second.empty());QVERIFY(first!=second);
        const auto revision=saved.revision.value;service.closeProject();QVERIFY(service.openProjectReadOnly(temp.path().toStdString(),error));
        StratigraphicScheme reopened;QVERIFY(service.readStratigraphicScheme(saved.id,revision,reopened,error));
        QCOMPARE(reopened.wellOccurrences[0].occurrenceId,first);QCOMPARE(reopened.wellOccurrences[1].occurrenceId,second);
        QVERIFY(!fw::wellRange(reopened,1,sub(reopened).id).resolved);
        const auto a=fw::wellRangeForOccurrence(reopened,1,sub(reopened).id,first),b=fw::wellRangeForOccurrence(reopened,1,sub(reopened).id,second);
        QVERIFY(a.resolved);QVERIFY(b.resolved);QCOMPARE(a.top,100.);QCOMPARE(a.bottom,102.5);QCOMPARE(b.top,200.);QCOMPARE(b.bottom,202.5);
        QVERIFY(!fw::wellRangeForOccurrence(reopened,2,sub(reopened).id,first).resolved);
        QVERIFY(!fw::wellRangeForOccurrence(reopened,1,sub(reopened).id,"missing").resolved);
        service.closeProject();QVERIFY(service.openProject(temp.path().toStdString(),error));
        auto edit=reopened;edit.wellOccurrences[1].top=220;edit.wellOccurrences[1].bottom=230;StratigraphicScheme next;
        QVERIFY2(service.commitStratigraphicScheme(edit,revision,next,issues,error),error.c_str());
        QCOMPARE(next.wellOccurrences[0].occurrenceId,first);QCOMPARE(next.wellOccurrences[1].occurrenceId,second);
        QVERIFY(service.readStratigraphicScheme(saved.id,revision,reopened,error));QCOMPARE(reopened.wellOccurrences[1].top,200.);
        auto foreign=next;foreign.wellOccurrences[1].wellId=2;StratigraphicScheme rejected;
        QVERIFY(!service.commitStratigraphicScheme(foreign,next.revision.value,rejected,issues,error));
        QVERIFY(service.readStratigraphicScheme(saved.id,{},reopened,error));QCOMPARE(reopened.revision,next.revision);
    }
    void defaultEditorDoesNotModifyFirstOccurrence() {
        auto value=scheme();const auto before=value.wellOccurrences;std::string error;
        QVERIFY(!fw::setWellInternalBoundaries(value,1,value.intervals.front().id,{101,102,103},"confirmed",error));QVERIFY(!error.empty());
        for(size_t i=0;i<before.size();++i)for(int k=0;k<3;++k)QVERIFY(std::isnan(value.wellOccurrences[i].internal[k]));
        QVERIFY(!fw::wellRange(value,1,sub(value).id).resolved);
    }
    void axesUnitsAndOverlapsRemainExplicit() {
        auto value=scheme();QVERIFY(!fw::hasErrors(fw::validate(value)));
        value.wellOccurrences[1].depthUnit="ft";value.wellOccurrences[1].top=700;value.wellOccurrences[1].bottom=740;
        QVERIFY(!fw::hasErrors(fw::validate(value))); // Explicit ft and m are compared in the same declared axis.
        value.wellOccurrences[1].top=330;value.wellOccurrences[1].bottom=350;QVERIFY(fw::hasErrors(fw::validate(value)));
        value=scheme();value.wellOccurrences[1].depthUnit.clear();QVERIFY(fw::hasErrors(fw::validate(value)));
        value=scheme();value.wellOccurrences[1].depthType="TVD";QVERIFY(fw::hasErrors(fw::validate(value)));
        value=scheme();value.wellOccurrences[1].top=105;QVERIFY(fw::hasErrors(fw::validate(value)));
        value=scheme();for(auto &occ:value.wellOccurrences){occ.top=100;occ.bottom=100;occ.status="pinched_out";}
        QVERIFY(fw::hasErrors(fw::validate(value)));
    }
    void secondOccurrenceParticipatesInValueFingerprint() {
        auto value=scheme();value.wellOccurrences[0].occurrenceId="first";value.wellOccurrences[1].occurrenceId="second";
        const auto original=fw::wellSubIntervalFingerprint(value,sub(value).id,1);
        const auto global=fw::subIntervalFingerprint(value,sub(value).id);
        value.wellOccurrences[1].top=220;value.wellOccurrences[1].bottom=230;
        QVERIFY(fw::wellSubIntervalFingerprint(value,sub(value).id,1)!=original);QVERIFY(fw::subIntervalFingerprint(value,sub(value).id)!=global);
        const auto modified=fw::wellSubIntervalFingerprint(value,sub(value).id,1);
        std::swap(value.wellOccurrences[0],value.wellOccurrences[1]);QCOMPARE(fw::wellSubIntervalFingerprint(value,sub(value).id,1),modified);
    }
};
QTEST_GUILESS_MAIN(WellOccurrenceTests)
#include "well_occurrence_tests.moc"
