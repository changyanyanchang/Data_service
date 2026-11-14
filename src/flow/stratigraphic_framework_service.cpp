// 统一地层层段体系的持久化：独立新表，CREATE IF NOT EXISTS，首次写入时建表；
// 不改动任何既有表/原始资料（非破坏、可回滚：删除 strat_* 表即恢复旧状态）。
#include "paleo_data/service.h"
#include "store/interpretation_store.h"
#include <QJsonDocument>
#include <algorithm>
#include <cmath>
#include <set>
#include <map>
#include <tuple>

namespace paleo::data {
using namespace detail;
namespace ix = detail::interpretation;
namespace fw = framework;
namespace {
constexpr int FrameworkSchema = 1;
// A scheme can be committed alone or inside the builder's publication
// transaction. Releasing an inner savepoint must not commit the outer write.
class SchemeSavepoint {
    ProjectStore& store;
    QString name;
public:
    bool active;
    SchemeSavepoint(ProjectStore& value,std::string& error)
        :store(value),name(QStringLiteral("\"paleo_strat_%1\"").arg(ix::newId())),
         active(store.exec("SAVEPOINT "+name,{},error)) {}
    ~SchemeSavepoint(){if(active){std::string ignored;store.exec("ROLLBACK TO "+name,{},ignored);store.exec("RELEASE "+name,{},ignored);}}
    bool commit(std::string& error){if(!active || !store.exec("RELEASE "+name,{},error))return false;active=false;return true;}
    SchemeSavepoint(const SchemeSavepoint&)=delete;
    SchemeSavepoint& operator=(const SchemeSavepoint&)=delete;
};
QJsonValue num(double v) { return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null); }
double real(const QJsonValue& v) { return v.isDouble() ? v.toDouble() : Missing; }

bool tableExists(const ProjectStore& st, std::string& error, bool& exists)
{
    QSqlQuery r;
    if (!st.exec("SELECT 1 FROM sqlite_master WHERE type='table' AND name='strat_scheme_revisions'", {}, error, &r)) return false;
    exists = r.next();
    return true;
}
bool ensureSchema(ProjectStore& st, std::string& error)
{
    const QStringList sql = {
        "CREATE TABLE IF NOT EXISTS strat_schemes(id TEXT PRIMARY KEY,name TEXT NOT NULL,head_revision TEXT NOT NULL,created TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS strat_scheme_revisions(revision TEXT PRIMARY KEY,scheme_id TEXT NOT NULL REFERENCES strat_schemes(id),parent_revision TEXT NOT NULL,created TEXT NOT NULL,author TEXT NOT NULL,note TEXT NOT NULL,payload BLOB NOT NULL,digest TEXT NOT NULL)",
        "CREATE INDEX IF NOT EXISTS strat_scheme_revision_owner ON strat_scheme_revisions(scheme_id,created)",
        // 子层段成果：原始解释成果资产保持不变，这里只登记归属与状态
        "CREATE TABLE IF NOT EXISTS strat_result_bindings(id TEXT PRIMARY KEY,kind TEXT NOT NULL,scheme_id TEXT NOT NULL REFERENCES strat_schemes(id),scheme_revision TEXT NOT NULL REFERENCES strat_scheme_revisions(revision),sub_interval_id TEXT NOT NULL,interval_id TEXT NOT NULL,aggregate INTEGER NOT NULL,well_id INTEGER NOT NULL,asset_ref TEXT NOT NULL,input_fingerprint TEXT NOT NULL,legacy_label TEXT NOT NULL,status TEXT NOT NULL,created TEXT NOT NULL,detail TEXT NOT NULL)",
        "CREATE INDEX IF NOT EXISTS strat_result_sub ON strat_result_bindings(scheme_id,sub_interval_id)",
        "CREATE INDEX IF NOT EXISTS strat_result_state ON strat_result_bindings(scheme_id,status)",
        "INSERT OR REPLACE INTO ds_meta(key,value) VALUES('stratigraphic_framework_schema','1')"};
    for (const auto& s : sql) if (!st.exec(s, {}, error)) return false;
    return true;
}
template <class Id> QString idq(const Id& id) { return q(id.value); }
QJsonArray arr3(const std::array<double, 3>& a) { return {num(a[0]), num(a[1]), num(a[2])}; }
QJsonArray arr3(const std::array<std::string, 3>& a) { return {q(a[0]), q(a[1]), q(a[2])}; }
QJsonArray arr3(const std::array<bool, 3>& a) { return {a[0], a[1], a[2]}; }

QJsonObject encode(const StratigraphicScheme& v)
{
    QJsonObject o{{"schema", FrameworkSchema}, {"id", q(v.id)}, {"name", q(v.name)}, {"revision", idq(v.revision)},
                  {"parentRevision", idq(v.parentRevision)}, {"projectId", q(v.projectId)}, {"created", q(v.created)},
                  {"author", q(v.author)}, {"note", q(v.note)}};
    QJsonArray b, i, s, d, w;
    for (const auto& x : v.boundaries)
        b.append(QJsonObject{{"id", idq(x.id)}, {"name", q(x.name)}, {"order", x.order}, {"kind", q(x.kind)}, {"source", q(x.source)},
                             {"seismicHorizonResourceId", QString::number(x.seismicHorizonResourceId)}, {"evidence", q(x.evidenceJson)}});
    for (const auto& x : v.intervals)
        i.append(QJsonObject{{"id", idq(x.id)}, {"top", idq(x.top)}, {"bottom", idq(x.bottom)}, {"order", x.order},
                             {"status", q(x.status)}, {"legacySegment", q(x.legacySegment)}, {"evidence", q(x.evidenceJson)}});
    for (const auto& x : v.subIntervals)
        s.append(QJsonObject{{"id", idq(x.id)}, {"interval", idq(x.interval)}, {"index", x.index}, {"status", q(x.status)}});
    for (const auto& x : v.divisions)
        d.append(QJsonObject{{"interval", idq(x.interval)}, {"mode", q(x.mode)}, {"fractions", arr3(x.fractions)},
                             {"edgeStatus", arr3(x.edgeStatus)}, {"basis", q(x.basis)}});
    for (const auto& x : v.wellOccurrences)
        w.append(QJsonObject{{"wellId", QString::number(x.wellId)}, {"interval", idq(x.interval)}, {"depthType", q(x.depthType)},
                             {"depthUnit", q(x.depthUnit)}, {"top", num(x.top)}, {"bottom", num(x.bottom)}, {"internal", arr3(x.internal)},
                             {"internalStatus", arr3(x.internalStatus)}, {"locked", arr3(x.locked)}, {"status", q(x.status)},
                             {"evidence", q(x.evidenceJson)}, {"occurrenceId", q(x.occurrenceId)}});
    o.insert("boundaries", b); o.insert("intervals", i); o.insert("subIntervals", s); o.insert("divisions", d); o.insert("wellOccurrences", w);
    return o;
}
std::string str(const QJsonObject& o, const char* k) { return s(o.value(k).toString()); }
bool decode(const QJsonObject& o, StratigraphicScheme& v, std::string& error)
{
    if (o.value("schema").toInt() != FrameworkSchema) { error = "层段方案格式版本不受支持"; return false; }
    v = {};
    v.id = str(o, "id"); v.name = str(o, "name"); v.revision.value = str(o, "revision"); v.parentRevision.value = str(o, "parentRevision");
    v.projectId = str(o, "projectId"); v.created = str(o, "created"); v.author = str(o, "author"); v.note = str(o, "note");
    for (const auto& e : o.value("boundaries").toArray()) {
        const auto x = e.toObject(); StratigraphicBoundary b;
        b.id.value = str(x, "id"); b.name = str(x, "name"); b.order = x.value("order").toInt(); b.kind = str(x, "kind"); b.source = str(x, "source");
        b.seismicHorizonResourceId = x.value("seismicHorizonResourceId").toString().toLongLong(); b.evidenceJson = str(x, "evidence");
        v.boundaries.push_back(b);
    }
    for (const auto& e : o.value("intervals").toArray()) {
        const auto x = e.toObject(); StratigraphicInterval i;
        i.id.value = str(x, "id"); i.top.value = str(x, "top"); i.bottom.value = str(x, "bottom"); i.order = x.value("order").toInt();
        i.status = str(x, "status"); i.legacySegment = str(x, "legacySegment"); i.evidenceJson = str(x, "evidence");
        v.intervals.push_back(i);
    }
    for (const auto& e : o.value("subIntervals").toArray()) {
        const auto x = e.toObject(); StratigraphicSubInterval sub;
        sub.id.value = str(x, "id"); sub.interval.value = str(x, "interval"); sub.index = x.value("index").toInt(); sub.status = str(x, "status");
        v.subIntervals.push_back(sub);
    }
    for (const auto& e : o.value("divisions").toArray()) {
        const auto x = e.toObject(); SubIntervalDivision d;
        d.interval.value = str(x, "interval"); d.mode = str(x, "mode"); d.basis = str(x, "basis");
        const auto f = x.value("fractions").toArray(), st = x.value("edgeStatus").toArray();
        if (f.size() != 3 || st.size() != 3) { error = "内部分界记录损坏"; return false; }
        for (int k = 0; k < 3; ++k) { d.fractions[k] = real(f[k]); d.edgeStatus[k] = s(st[k].toString()); }
        v.divisions.push_back(d);
    }
    for (const auto& e : o.value("wellOccurrences").toArray()) {
        const auto x = e.toObject(); WellIntervalOccurrence w;
        w.wellId = x.value("wellId").toString().toLongLong(); w.interval.value = str(x, "interval");
        w.depthType = str(x, "depthType"); w.depthUnit = str(x, "depthUnit"); w.top = real(x.value("top")); w.bottom = real(x.value("bottom"));
        const auto in = x.value("internal").toArray(), is = x.value("internalStatus").toArray(), lk = x.value("locked").toArray();
        if (in.size() != 3 || is.size() != 3 || lk.size() != 3) { error = "井层段记录损坏"; return false; }
        for (int k = 0; k < 3; ++k) { w.internal[k] = real(in[k]); w.internalStatus[k] = s(is[k].toString()); w.locked[k] = lk[k].toBool(); }
        w.status = str(x, "status"); w.evidenceJson = str(x, "evidence");
        if(x.contains("occurrenceId")&&!x.value("occurrenceId").isString()) {error="井出现身份字段损坏";return false;}
        w.occurrenceId=str(x,"occurrenceId");
        v.wellOccurrences.push_back(w);
    }
    return true;
}
bool readRevision(const ProjectStore& st, const std::string& schemeId, std::string revision, StratigraphicScheme& out, std::string& error)
{
    bool exists = false;
    if (!tableExists(st, error, exists)) return false;
    if (!exists) { error = "工区没有层段方案（旧工程或尚未建立）"; return false; }
    if (revision.empty()) {
        QSqlQuery h;
        if (!st.exec("SELECT head_revision FROM strat_schemes WHERE id=?", {q(schemeId)}, error, &h)) return false;
        if (!h.next()) { error = "层段方案不存在"; return false; }
        revision = s(h.value(0).toString());
    }
    QSqlQuery r;
    if (!st.exec("SELECT payload,digest FROM strat_scheme_revisions WHERE revision=? AND scheme_id=?", {q(revision), q(schemeId)}, error, &r)) return false;
    if (!r.next()) { error = "层段方案修订不存在"; return false; }
    const QByteArray payload = r.value(0).toByteArray();
    if (ix::digest(payload) != r.value(1).toString()) { error = "层段方案校验失败"; return false; }
    const auto doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject()) { error = "层段方案数据损坏"; return false; }
    return decode(doc.object(), out, error);
}
SubIntervalResultBinding bindingFrom(const QSqlQuery& r)
{
    SubIntervalResultBinding b;
    b.id = s(r.value(0).toString()); b.kind = s(r.value(1).toString()); b.schemeId = s(r.value(2).toString());
    b.schemeRevision.value = s(r.value(3).toString()); b.subInterval.value = s(r.value(4).toString()); b.interval.value = s(r.value(5).toString());
    b.aggregate = r.value(6).toInt() != 0; b.wellId = r.value(7).toLongLong(); b.assetRef = s(r.value(8).toString());
    b.inputFingerprint = s(r.value(9).toString()); b.legacyLabel = s(r.value(10).toString()); b.status = s(r.value(11).toString());
    b.created = s(r.value(12).toString()); b.detailJson = s(r.value(13).toString());
    return b;
}
const char* BindingColumns = "id,kind,scheme_id,scheme_revision,sub_interval_id,interval_id,aggregate,well_id,asset_ref,input_fingerprint,legacy_label,status,created,detail";
bool listBindings(const ProjectStore& st, const std::string& schemeId, const SubIntervalId& sub, std::vector<SubIntervalResultBinding>& out, std::string& error)
{
    out.clear();
    bool exists = false;
    if (!tableExists(st, error, exists)) return false;
    if (!exists) return true;
    QSqlQuery r;
    const QString sql = QString("SELECT %1 FROM strat_result_bindings WHERE scheme_id=?%2 ORDER BY created,id").arg(BindingColumns, sub.empty() ? "" : " AND sub_interval_id=?");
    QVariantList args{q(schemeId)};
    if (!sub.empty()) args << q(sub.value);
    if (!st.exec(sql, args, error, &r)) return false;
    while (r.next()) out.push_back(bindingFrom(r));
    return true;
}
bool affected(const SubIntervalResultBinding& b, const StratigraphicScheme& before, const StratigraphicScheme& after)
{
    if (b.status == "legacy_unresolved") return false;
    return fw::resultGeometryFingerprint(before,b)!=fw::resultGeometryFingerprint(after,b);
}
bool invalidateAffected(ProjectStore& st,const StratigraphicScheme& before,const StratigraphicScheme& after,std::string& error)
{
    QSqlQuery rows;
    if(!st.exec("SELECT id,kind,sub_interval_id,interval_id,aggregate,well_id FROM strat_result_bindings "
                "WHERE scheme_id=? AND status IN ('current','candidate','review_required')",
                {q(after.id)},error,&rows))return false;
    using Key=std::tuple<std::string,std::string,std::string,bool,long long>;
    std::map<Key,bool> changes;std::vector<std::string> ids;
    while(rows.next()) {
        SubIntervalResultBinding binding;binding.id=s(rows.value(0).toString());binding.kind=s(rows.value(1).toString());
        binding.subInterval.value=s(rows.value(2).toString());binding.interval.value=s(rows.value(3).toString());
        binding.aggregate=rows.value(4).toBool();binding.wellId=rows.value(5).toLongLong();
        const Key key{binding.kind,binding.subInterval.value,binding.interval.value,binding.aggregate,binding.wellId};
        auto found=changes.find(key);
        if(found==changes.end())found=changes.emplace(key,affected(binding,before,after)).first;
        if(found->second)ids.push_back(binding.id);
    }
    rows.finish();
    for(const auto& id:ids)if(!st.exec("UPDATE strat_result_bindings SET status='stale' WHERE id=?",{q(id)},error))return false;
    return true;
}
bool commitInternal(ProjectStore& st, StratigraphicScheme scheme, const std::string& expected, StratigraphicScheme& committed,
                    std::vector<FrameworkIssue>& issues, std::string& error)
{
    if (st.readOnly) { error = "只读工程不能写入层段方案"; return false; }
    if (scheme.id.empty()) scheme.id = s(ix::newId());
    fw::deriveUnits(scheme);
    issues = fw::validate(scheme);
    if (fw::hasErrors(issues)) { error = "层段方案校验失败：" + issues.front().message; for (auto& i : issues) if (i.severity == "error") { error = "层段方案校验失败：" + i.message; break; } return false; }
    SchemeSavepoint tx(st, error);
    if (!tx.active || !ensureSchema(st, error)) return false;
    std::string project;
    if (!ix::projectId(st, project, error)) return false;
    QSqlQuery h;
    if (!st.exec("SELECT head_revision FROM strat_schemes WHERE id=?", {q(scheme.id)}, error, &h)) return false;
    const bool existing = h.next();
    const std::string head = existing ? s(h.value(0).toString()) : std::string();
    h.finish();
    if (head != expected) { error = "层段方案版本冲突：当前头修订已变化，请重新读取后再提交"; return false; }
    StratigraphicScheme before;
    if (existing && !readRevision(st, scheme.id, head, before, error)) return false;
    // New occurrences receive durable identities only in the committing transaction.
    // Older immutable revisions remain unchanged; subsequent edits carry these IDs.
    for(auto &occurrence:scheme.wellOccurrences) {
        if(existing&&!occurrence.occurrenceId.empty()) {
            const auto old=std::find_if(before.wellOccurrences.begin(),before.wellOccurrences.end(),[&](const auto &value){
                return value.occurrenceId==occurrence.occurrenceId;});
            if(old==before.wellOccurrences.end()||old->wellId!=occurrence.wellId||old->interval!=occurrence.interval) {
                error="井出现身份失效或被移到其他井/父单元";return false;
            }
        }
        if(!existing||occurrence.occurrenceId.empty())occurrence.occurrenceId="occ-"+s(ix::newId());
    }
    scheme.projectId = project;
    scheme.parentRevision.value = head;
    scheme.revision.value = s(ix::newId());
    scheme.created = s(ix::now());
    const QByteArray payload = ix::bytes(encode(scheme));
    if (existing) {
        if (!st.exec("UPDATE strat_schemes SET head_revision=?,name=? WHERE id=? AND head_revision=?", {idq(scheme.revision), q(scheme.name), q(scheme.id), q(head)}, error)) return false;
    }
    // 先插方案行再插修订（外键）；新方案的头指针在修订插入后更新
    else if (!st.exec("INSERT INTO strat_schemes(id,name,head_revision,created) VALUES(?,?,?,?)", {q(scheme.id), q(scheme.name), idq(scheme.revision), q(scheme.created)}, error)) return false;
    if (!st.exec("INSERT INTO strat_scheme_revisions(revision,scheme_id,parent_revision,created,author,note,payload,digest) VALUES(?,?,?,?,?,?,?,?)",
                 {idq(scheme.revision), q(scheme.id), q(head), q(scheme.created), q(scheme.author), q(scheme.note), payload, ix::digest(payload)}, error)) return false;
    if (existing && !invalidateAffected(st,before,scheme,error))return false;
    if (!tx.commit(error)) return false;
    committed = scheme;
    return true;
}
}

bool DataService::commitStratigraphicScheme(const StratigraphicScheme& scheme, const std::string& expectedHeadRevision,
                                            StratigraphicScheme& committed, std::vector<FrameworkIssue>& issues, std::string& error)
{
    error.clear(); issues.clear();
    return commitInternal(impl_->store, scheme, expectedHeadRevision, committed, issues, error);
}

bool DataService::listStratigraphicSchemes(std::vector<StratigraphicSchemeSummary>& result, std::string& error) const
{
    error.clear(); result.clear();
    auto& st = impl_->store; bool exists = false;
    if (!tableExists(st, error, exists)) return false;
    if (!exists) return true;
    QSqlQuery r;
    if (!st.exec("SELECT id,name,head_revision,created FROM strat_schemes ORDER BY created,id", {}, error, &r)) return false;
    while (r.next()) { StratigraphicSchemeSummary x; x.id = s(r.value(0).toString()); x.name = s(r.value(1).toString()); x.headRevision.value = s(r.value(2).toString()); x.created = s(r.value(3).toString()); result.push_back(x); }
    return true;
}

bool DataService::listStratigraphicSchemeRevisions(const std::string& schemeId, std::vector<StratigraphicSchemeRevisionSummary>& result, std::string& error) const
{
    error.clear(); result.clear();
    auto& st = impl_->store; bool exists = false;
    if (!tableExists(st, error, exists)) return false;
    if (!exists) return true;
    QSqlQuery r;
    if (!st.exec("SELECT revision,parent_revision,created,author,note FROM strat_scheme_revisions WHERE scheme_id=? ORDER BY created,revision", {q(schemeId)}, error, &r)) return false;
    while (r.next()) { StratigraphicSchemeRevisionSummary x; x.revision.value = s(r.value(0).toString()); x.parentRevision.value = s(r.value(1).toString()); x.created = s(r.value(2).toString()); x.author = s(r.value(3).toString()); x.note = s(r.value(4).toString()); result.push_back(x); }
    return true;
}

bool DataService::readStratigraphicScheme(const std::string& schemeId, const std::string& revision, StratigraphicScheme& result, std::string& error) const
{
    error.clear(); result = {};
    return readRevision(impl_->store, schemeId, revision, result, error);
}

bool DataService::restoreStratigraphicSchemeRevision(const std::string& schemeId, const std::string& revision, const std::string& expectedHeadRevision,
                                                     StratigraphicScheme& committed, std::string& error)
{
    error.clear();
    StratigraphicScheme old;
    if (!readRevision(impl_->store, schemeId, revision, old, error)) return false;
    old.note = "restore " + revision;
    std::vector<FrameworkIssue> issues;
    return commitInternal(impl_->store, old, expectedHeadRevision, committed, issues, error);
}

bool DataService::readWellSubIntervalRange(const std::string& schemeId, const std::string& revision, long long wellId,
                                           const SubIntervalId& subInterval, WellSubIntervalRange& result, std::string& error) const
{
    error.clear(); result = {};
    StratigraphicScheme scheme;
    if (!readRevision(impl_->store, schemeId, revision, scheme, error)) return false;
    const auto* sub = fw::resolveSubInterval(scheme, subInterval.value);
    if (!sub) { error = "子层段不属于该方案修订"; return false; }
    result = fw::wellRange(scheme, wellId, sub->id);
    return true;
}

bool DataService::bindSubIntervalResult(const SubIntervalResultBinding& in, SubIntervalResultBinding& stored, std::string& error)
{
    error.clear();
    auto& st = impl_->store;
    if (st.readOnly) { error = "只读工程不能登记成果"; return false; }
    if (in.kind.empty() || in.schemeId.empty()) { error = "成果登记缺少类型或方案"; return false; }
    SubIntervalResultBinding b = in;
    Transaction tx(st, error);
    if (!tx.active || !ensureSchema(st, error)) return false;
    StratigraphicScheme scheme;
    if (!readRevision(st, b.schemeId, b.schemeRevision.value, scheme, error)) return false;
    b.schemeRevision = scheme.revision;
    if (b.subInterval.empty() && !b.aggregate) {
        if (b.legacyLabel.empty()) { error = "新成果必须选择有效子层段"; return false; }
        b.status = "legacy_unresolved"; // 历史成果：保留原标签，不悄悄归类
    } else if (!b.subInterval.empty()) {
        const auto* sub = fw::findSubInterval(scheme, b.subInterval);
        if (!sub) { error = "子层段不属于该方案修订"; return false; }
        if (!b.interval.empty() && !(b.interval == sub->interval)) { error = "子层段与父单元不一致"; return false; }
        b.interval = sub->interval; b.aggregate = false;
    } else if (!fw::findInterval(scheme, b.interval)) { error = "父单元不属于该方案修订"; return false; }
    if (b.id.empty()) b.id = s(ix::newId());
    if (b.status.empty()) b.status = "current";
    b.created = s(ix::now());
    const QString sql = QString("INSERT INTO strat_result_bindings(%1) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)").arg(BindingColumns);
    if (!st.exec(sql, {q(b.id), q(b.kind), q(b.schemeId), idq(b.schemeRevision), idq(b.subInterval), idq(b.interval), b.aggregate ? 1 : 0,
                       b.wellId, q(b.assetRef), q(b.inputFingerprint), q(b.legacyLabel), q(b.status), q(b.created), q(b.detailJson)}, error)) return false;
    if (!tx.commit(error)) return false;
    stored = b;
    return true;
}

bool DataService::invalidateSubIntervalResult(const std::string& id, std::string& error)
{
    error.clear();
    auto& st = impl_->store;
    if (st.readOnly) { error = "只读工程不能更新成果状态"; return false; }
    if (id.empty()) { error = "成果 ID 为空"; return false; }
    Transaction tx(st, error);
    if (!tx.active || !ensureSchema(st, error)) return false;
    if (!st.exec("UPDATE strat_result_bindings SET status='stale' WHERE id=? AND status IN ('current','candidate','review_required')",
                 {q(id)}, error)) return false;
    return tx.commit(error);
}

bool DataService::listSubIntervalResults(const std::string& schemeId, const SubIntervalId& subInterval,
                                         std::vector<SubIntervalResultBinding>& result, std::string& error) const
{
    error.clear();
    return listBindings(impl_->store, schemeId, subInterval, result, error);
}

bool DataService::previewStratigraphicImpact(const StratigraphicScheme& candidate, std::vector<SubIntervalId>& subIntervals,
                                             std::vector<SubIntervalResultBinding>& affectedResults, std::string& error) const
{
    error.clear(); subIntervals.clear(); affectedResults.clear();
    StratigraphicScheme before, after = candidate;
    if (!readRevision(impl_->store, candidate.id, {}, before, error)) return false;
    fw::deriveUnits(after);
    subIntervals = fw::impactedSubIntervals(before, after);
    for(const auto& sub:before.subIntervals)
        if(fw::seismicSubIntervalFingerprint(before,sub.id)!=fw::seismicSubIntervalFingerprint(after,sub.id) &&
           std::find(subIntervals.begin(),subIntervals.end(),sub.id)==subIntervals.end())subIntervals.push_back(sub.id);
    std::vector<SubIntervalResultBinding> all;
    if (!listBindings(impl_->store, candidate.id, {}, all, error)) return false;
    for (const auto& b : all) if ((b.status=="current"||b.status=="candidate"||b.status=="review_required") && affected(b,before,after)) affectedResults.push_back(b);
    return true;
}
}
