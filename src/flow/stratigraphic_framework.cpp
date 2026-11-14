#include "paleo_data/stratigraphic_framework.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <QString>

namespace paleo::data::framework {
namespace {
bool finite(double v) { return std::isfinite(v); }
std::string fnv(const std::string& text)
{
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
    std::ostringstream s; s << std::hex << std::setw(16) << std::setfill('0') << h; return s.str();
}
std::string num(double v)
{
    if (!finite(v)) return "nan";
    std::ostringstream s; s << std::setprecision(17) << v; return s.str();
}
void issue(std::vector<FrameworkIssue>& out, const char* sev, const char* code, const std::string& id, const std::string& msg)
{
    out.push_back({sev, code, id, msg});
}
const SubIntervalDivision* divisionOf(const StratigraphicScheme& s, const IntervalId& id)
{
    for (const auto& d : s.divisions) if (d.interval == id) return &d;
    return nullptr;
}
const WellIntervalOccurrence* occurrenceOf(const StratigraphicScheme& s, long long well, const IntervalId& id)
{
    const WellIntervalOccurrence* found=nullptr;
    for (const auto& o : s.wellOccurrences) if (o.wellId == well && o.interval == id) {
        if(found)return nullptr;found=&o;
    }
    return found;
}
double depthScale(const WellIntervalOccurrence &o) {
    const auto unit=QString::fromStdString(o.depthUnit).trimmed().toUpper();
    if(unit=="M"||unit=="METRE"||unit=="METRES"||unit=="METER"||unit=="METERS")return 1.;
    if(unit=="FT"||unit=="FEET"||unit=="FOOT"||unit=="F")return .3048;
    return Missing;
}
bool comparable(const WellIntervalOccurrence &a,const WellIntervalOccurrence &b) {
    return (a.depthType=="MD"||a.depthType=="TVD"||a.depthType=="TVDSS")&&
        a.depthType==b.depthType&&finite(depthScale(a))&&finite(depthScale(b));
}
bool strictlyIncreasing(const std::array<double, 3>& f, double lo, double hi)
{
    double prev = lo;
    for (double v : f) { if (!finite(v) || !(v > prev)) return false; prev = v; }
    return hi > prev;
}
}

void deriveUnits(StratigraphicScheme& scheme, const std::string& idSeed)
{
    const std::string seed = idSeed.empty() ? scheme.id : idSeed;
    for (auto& b : scheme.boundaries)
        if (b.id.empty()) b.id.value = "bnd-" + fnv(seed + "|boundary|" + b.name + "|" + std::to_string(b.order));
    std::vector<const StratigraphicBoundary*> ordered;
    for (const auto& b : scheme.boundaries) ordered.push_back(&b);
    std::stable_sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->order < b->order; });
    std::map<std::pair<std::string, std::string>, StratigraphicInterval> existing;
    for (const auto& i : scheme.intervals) existing[{i.top.value, i.bottom.value}] = i;
    std::vector<StratigraphicInterval> intervals;
    for (std::size_t k = 0; k + 1 < ordered.size(); ++k) {
        if (ordered[k]->order == ordered[k + 1]->order) continue; // 顺序冲突交由 validate 报告
        const auto key = std::make_pair(ordered[k]->id.value, ordered[k + 1]->id.value);
        StratigraphicInterval iv;
        if (auto it = existing.find(key); it != existing.end()) iv = it->second;
        else {
            iv.id.value = "int-" + fnv(seed + "|interval|" + key.first + "|" + key.second);
            iv.top = ordered[k]->id; iv.bottom = ordered[k + 1]->id;
        }
        iv.order = int(intervals.size()) + 1;
        intervals.push_back(iv);
    }
    scheme.intervals = intervals;
    std::vector<StratigraphicSubInterval> subs;
    std::vector<SubIntervalDivision> divisions;
    for (const auto& iv : scheme.intervals) {
        for (int n = 1; n <= SubIntervalCount; ++n) {
            StratigraphicSubInterval sub;
            bool found = false;
            for (const auto& old : scheme.subIntervals)
                if (old.interval == iv.id && old.index == n) { sub = old; found = true; break; }
            if (!found) {
                sub.id.value = "sub-" + fnv(seed + "|sub|" + iv.id.value + "|" + std::to_string(n));
                sub.interval = iv.id; sub.index = n;
            }
            subs.push_back(sub);
        }
        if (const auto* d = divisionOf(scheme, iv.id)) divisions.push_back(*d);
        else { SubIntervalDivision fresh; fresh.interval = iv.id; divisions.push_back(fresh); }
    }
    scheme.subIntervals = subs;
    scheme.divisions = divisions;
    // 删除引用已不存在父单元的井出现（父单元由界面相邻关系唯一决定）
    std::set<std::string> live;
    for (const auto& iv : scheme.intervals) live.insert(iv.id.value);
    scheme.wellOccurrences.erase(std::remove_if(scheme.wellOccurrences.begin(), scheme.wellOccurrences.end(),
        [&](const auto& o) { return !live.count(o.interval.value); }), scheme.wellOccurrences.end());
}

std::vector<FrameworkIssue> validate(const StratigraphicScheme& s)
{
    std::vector<FrameworkIssue> out;
    std::set<std::string> ids;
    std::map<int, std::string> orders;
    auto unique = [&](const std::string& id, const char* what) {
        if (id.empty()) issue(out, "error", "empty_id", id, std::string(what) + " 缺少稳定 ID");
        else if (!ids.insert(id).second) issue(out, "error", "duplicate_id", id, std::string(what) + " ID 重复: " + id);
    };
    std::map<std::string, int> names;
    for (const auto& b : s.boundaries) {
        unique(b.id.value, "界面");
        if (!orders.emplace(b.order, b.id.value).second)
            issue(out, "error", "order_conflict", b.id.value, "界面顺序冲突: " + b.name);
        if (++names[b.name] == 2) issue(out, "error", "duplicate_name", b.id.value, "同一方案内界面同名: " + b.name);
    }
    std::vector<const StratigraphicBoundary*> ordered;
    for (const auto& b : s.boundaries) ordered.push_back(&b);
    std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->order < b->order; });
    std::set<std::pair<std::string, std::string>> adjacent;
    for (std::size_t k = 0; k + 1 < ordered.size(); ++k) adjacent.insert({ordered[k]->id.value, ordered[k + 1]->id.value});
    if (s.intervals.size() != adjacent.size())
        issue(out, "error", "interval_count", s.id, "父单元数必须等于相邻界面对数（N 个界面 → N-1 个单元）");
    for (const auto& iv : s.intervals) {
        unique(iv.id.value, "父单元");
        if (!adjacent.count({iv.top.value, iv.bottom.value}))
            issue(out, "error", "not_adjacent", iv.id.value, "父单元顶底不是相邻有序界面或顶底倒置");
        std::set<int> idx;
        int count = 0;
        for (const auto& sub : s.subIntervals) if (sub.interval == iv.id) { ++count; idx.insert(sub.index); }
        if (count != SubIntervalCount || idx != std::set<int>{1, 2, 3, 4})
            issue(out, "error", "subinterval_count", iv.id.value, "父单元必须恰有 S1–S4 四个子层段");
        const auto* d = divisionOf(s, iv.id);
        if (!d) issue(out, "error", "division_missing", iv.id.value, "缺少内部分界定义");
        else if (!strictlyIncreasing(d->fractions, 0.0, 1.0))
            issue(out, "error", "division_order", iv.id.value, "内部分界比例必须位于 (0,1) 且严格递增");
    }
    for (const auto& sub : s.subIntervals) {
        unique(sub.id.value, "子层段");
        if (!findInterval(s, sub.interval)) issue(out, "error", "orphan_subinterval", sub.id.value, "子层段引用不存在的父单元");
    }
    std::set<std::string> occurrenceIds;
    std::map<std::pair<long long,std::string>,std::vector<const WellIntervalOccurrence*>> repeated;
    std::map<long long, std::vector<const WellIntervalOccurrence*>> perWell;
    for (const auto& o : s.wellOccurrences) {
        const auto* iv = findInterval(s, o.interval);
        if (!iv) { issue(out, "error", "orphan_occurrence", o.interval.value, "井出现引用不存在的父单元"); continue; }
        if(!o.occurrenceId.empty()&&(!occurrenceIds.insert(o.occurrenceId).second||o.occurrenceId.size()>256||
           o.occurrenceId.find('\0')!=std::string::npos))
            issue(out,"error","duplicate_occurrence_id",o.interval.value,"井出现身份重复或无效");
        auto &previous=repeated[{o.wellId,o.interval.value}];
        previous.push_back(&o);
        if (finite(o.top) && finite(o.bottom) && !(o.bottom > o.top) && o.status != "pinched_out")
            issue(out, "error", "inverted", o.interval.value, "井 " + std::to_string(o.wellId) + " 顶底倒置或零厚度（尖灭请显式标 pinched_out）");
        double prev = o.top;
        for (int k = 0; k < 3; ++k) {
            if (!finite(o.internal[k])) continue;
            if ((finite(prev) && !(o.internal[k] > prev)) || (finite(o.bottom) && !(o.internal[k] < o.bottom)))
                issue(out, "error", "internal_order", o.interval.value, "井 " + std::to_string(o.wellId) + " 内部分界逆序或越出父单元");
            prev = o.internal[k];
        }
        perWell[o.wellId].push_back(&o);
    }
    for(auto &[key,occurrences]:repeated) {
        if(occurrences.size()<2)continue;
        if(std::any_of(occurrences.begin(),occurrences.end(),[&](const auto *value){
            return !comparable(*occurrences.front(),*value)||!finite(value->top)||!finite(value->bottom);})) {
            issue(out,"error","ambiguous_occurrence",key.second,"多次井出现缺少共同轴、已知单位或完整范围");continue;
        }
        std::sort(occurrences.begin(),occurrences.end(),[](const auto *a,const auto *b){
            return a->top*depthScale(*a)<b->top*depthScale(*b);});
        for(std::size_t index=1;index<occurrences.size();++index)
            if((occurrences[index-1]->top*depthScale(*occurrences[index-1])==occurrences[index]->top*depthScale(*occurrences[index])&&
                occurrences[index-1]->bottom*depthScale(*occurrences[index-1])==occurrences[index]->bottom*depthScale(*occurrences[index]))||
               occurrences[index-1]->bottom*depthScale(*occurrences[index-1])>occurrences[index]->top*depthScale(*occurrences[index]))
                issue(out,"error","duplicate_occurrence",key.second,"同一井同一父单元出现重叠；须明确实际区间");
    }
    for (auto& [well, list] : perWell) {
        const auto orderKey=[&](const auto *occurrence) {
            const bool known=finite(depthScale(*occurrence))&&finite(occurrence->top);
            return std::make_tuple(occurrence->depthType,known?0:1,known?occurrence->top*depthScale(*occurrence):0.,
                                  findInterval(s,occurrence->interval)->order);
        };
        std::stable_sort(list.begin(), list.end(), [&](auto* a, auto* b) {
            return orderKey(a)<orderKey(b);
        });
        for (std::size_t k = 0; k + 1 < list.size(); ++k) {
            const auto* a = list[k]; const auto* b = list[k + 1];
            if(!comparable(*a,*b))continue;
            if (findInterval(s, b->interval)->order != findInterval(s, a->interval)->order + 1) continue;
            if (!finite(a->bottom) || !finite(b->top)) continue;
            if (a->bottom*depthScale(*a) > b->top*depthScale(*b) + 1e-6) issue(out, "error", "overlap", b->interval.value, "井 " + std::to_string(well) + " 相邻父单元重叠");
            else if (b->top*depthScale(*b) > a->bottom*depthScale(*a) + 1e-6 && a->status != "faulted" && b->status != "faulted")
                issue(out, "warning", "gap", b->interval.value, "井 " + std::to_string(well) + " 相邻父单元间存在空隙（共享界面深度不一致）");
        }
    }
    return out;
}

bool hasErrors(const std::vector<FrameworkIssue>& issues)
{
    return std::any_of(issues.begin(), issues.end(), [](const auto& i) { return i.severity == "error"; });
}

std::vector<const StratigraphicSubInterval*> subIntervalsOf(const StratigraphicScheme& s, const IntervalId& id)
{
    std::vector<const StratigraphicSubInterval*> out;
    for (const auto& sub : s.subIntervals) if (sub.interval == id) out.push_back(&sub);
    std::sort(out.begin(), out.end(), [](auto* a, auto* b) { return a->index < b->index; });
    return out;
}
const StratigraphicSubInterval* findSubInterval(const StratigraphicScheme& s, const SubIntervalId& id)
{
    for (const auto& v : s.subIntervals) if (v.id == id) return &v;
    return nullptr;
}
const StratigraphicInterval* findInterval(const StratigraphicScheme& s, const IntervalId& id)
{
    for (const auto& v : s.intervals) if (v.id == id) return &v;
    return nullptr;
}
const StratigraphicBoundary* findBoundary(const StratigraphicScheme& s, const BoundaryId& id)
{
    for (const auto& v : s.boundaries) if (v.id == id) return &v;
    return nullptr;
}
const StratigraphicInterval* resolveInterval(const StratigraphicScheme& s, const std::string& idOrName)
{
    if (idOrName.empty()) return nullptr;
    if (const auto* iv = findInterval(s, IntervalId{idOrName})) return iv;
    for (const auto& iv : s.intervals) {
        if (displayName(s, iv.id) == idOrName) return &iv;
        const auto* t = findBoundary(s, iv.top);
        const auto* b = findBoundary(s, iv.bottom);
        if (t && b && (t->name + "-" + b->name) == idOrName) return &iv;
    }
    const auto leg = resolveLegacyLabel(s, idOrName);
    if (!leg.interval.empty()) return findInterval(s, leg.interval);
    return nullptr;
}
const StratigraphicSubInterval* resolveSubInterval(const StratigraphicScheme& s, const std::string& idOrName)
{
    if (idOrName.empty()) return nullptr;
    if (const auto* sub = findSubInterval(s, SubIntervalId{idOrName})) return sub;
    for (const auto& sub : s.subIntervals) {
        if (displayName(s, sub.id) == idOrName) return &sub;
        const auto* iv = findInterval(s, sub.interval);
        if (!iv) continue;
        const auto* t = findBoundary(s, iv->top);
        const auto* b = findBoundary(s, iv->bottom);
        if (t && b) {
            const std::string base = t->name + "-" + b->name;
            const std::string sfx = std::to_string(sub.index);
            if (idOrName == base + "-S" + sfx || idOrName == base + "-s" + sfx ||
                idOrName == base + "/S" + sfx || idOrName == base + "/s" + sfx)
                return &sub;
        }
    }
    const auto leg = resolveLegacyLabel(s, idOrName);
    if (!leg.subInterval.empty()) return findSubInterval(s, leg.subInterval);
    return nullptr;
}
std::string displayName(const StratigraphicScheme& s, const IntervalId& id)
{
    const auto* iv = findInterval(s, id);
    if (!iv) return {};
    const auto* t = findBoundary(s, iv->top); const auto* b = findBoundary(s, iv->bottom);
    return (t ? t->name : "?") + "\xE2\x80\x93" + (b ? b->name : "?");
}
std::string displayName(const StratigraphicScheme& s, const SubIntervalId& id)
{
    const auto* sub = findSubInterval(s, id);
    return sub ? displayName(s, sub->interval) + "/S" + std::to_string(sub->index) : std::string();
}

static WellSubIntervalRange rangeOf(const StratigraphicScheme& s,long long wellId,const SubIntervalId& id,
                                  const WellIntervalOccurrence* o)
{
    WellSubIntervalRange r;
    r.subInterval = id; r.wellId = wellId;
    const auto* sub = findSubInterval(s, id);
    if (!sub) { r.reason = "未知子层段"; return r; }
    if(sub->index<1||sub->index>SubIntervalCount){r.reason="子层段序号无效";return r;}
    r.interval = sub->interval; r.index = sub->index;
    if (!o) { r.reason = "该井没有此父单元的分层记录"; return r; }
    r.depthType = o->depthType; r.depthUnit = o->depthUnit;
    if (o->status == "pinched_out") { r.reason = "父单元在该井尖灭"; return r; }
    if (!finite(o->top)) { r.reason = "顶界面缺失"; return r; }
    const auto* d = divisionOf(s, sub->interval);
    std::array<double, 5> edge{o->top, Missing, Missing, Missing, o->bottom};
    std::array<bool, 5> derived{false, true, true, true, false};
    for (int k = 0; k < 3; ++k) {
        if (finite(o->internal[k])) { edge[k + 1] = o->internal[k]; derived[k + 1] = o->internalStatus[k] == "derived"; }
        else if (finite(o->bottom) && d && o->internalStatus[k] != "pending")
            edge[k + 1] = o->top + d->fractions[k] * (o->bottom - o->top);
    }
    const int n = sub->index;
    r.top = edge[n - 1]; r.bottom = edge[n];
    if (!finite(r.top) || !finite(r.bottom)) {
        r.top = r.bottom = Missing;
        r.reason = !finite(o->bottom) ? "底界面缺失或未钻穿，内部分界无法确定（不补造）" : "内部分界待解释";
        return r;
    }
    if (!(r.bottom > r.top)) { r.top = r.bottom = Missing; r.reason = "子层段零厚度或逆序"; return r; }
    r.provisional = derived[n - 1] || derived[n];
    r.resolved = true;
    return r;
}

WellSubIntervalRange wellRange(const StratigraphicScheme& s,long long wellId,const SubIntervalId& id) {
    const auto *sub=findSubInterval(s,id);
    const auto *occurrence=sub?occurrenceOf(s,wellId,sub->interval):nullptr;
    auto result=rangeOf(s,wellId,id,occurrence);
    if(sub&&!occurrence&&std::count_if(s.wellOccurrences.begin(),s.wellOccurrences.end(),[&](const auto &o){
        return o.wellId==wellId&&o.interval==sub->interval;})>1)result.reason="该井此父单元有多次出现，须给精确 occurrenceId";
    return result;
}
WellSubIntervalRange wellRangeForOccurrence(const StratigraphicScheme& s,long long wellId,const SubIntervalId& id,
                                           const std::string& occurrenceId) {
    const auto *sub=findSubInterval(s,id);const WellIntervalOccurrence *selected=nullptr;
    for(const auto &occurrence:s.wellOccurrences)if(!occurrenceId.empty()&&occurrence.occurrenceId==occurrenceId) {
        if(selected){auto result=rangeOf(s,wellId,id,nullptr);result.reason="井出现身份重复";return result;}
        selected=&occurrence;
    }
    if(!sub||!selected||selected->wellId!=wellId||selected->interval!=sub->interval) {
        auto result=rangeOf(s,wellId,id,nullptr);result.reason="井出现身份缺失、失效或不属于当前井和父单元";return result;
    }
    return rangeOf(s,wellId,id,selected);
}

bool setWellInternalBoundaries(StratigraphicScheme& s, long long wellId, const IntervalId& interval,
                               const std::array<double, 3>& depths, const std::string& status, std::string& error)
{
    if(std::count_if(s.wellOccurrences.begin(),s.wellOccurrences.end(),[&](const auto &occurrence){
        return occurrence.wellId==wellId&&occurrence.interval==interval;})>1) {
        error="该井此父单元有多次出现，未选择出现时不能修改首条";return false;
    }
    for (auto& o : s.wellOccurrences) {
        if (o.wellId != wellId || !(o.interval == interval)) continue;
        if (!finite(o.top) || !finite(o.bottom)) { error = "父单元顶底不完整，不能设置内部分界"; return false; }
        if (!strictlyIncreasing(depths, o.top, o.bottom)) { error = "内部分界必须严格位于顶底之间且顶→底递增"; return false; }
        for (int k = 0; k < 3; ++k) {
            if (o.locked[k] && o.internal[k] != depths[k]) { error = "分界已锁定保护"; return false; }
        }
        o.internal = depths;
        o.internalStatus = {status, status, status};
        return true;
    }
    error = "该井没有此父单元的分层记录";
    return false;
}

SeismicSubIntervalMask seismicMask(const std::vector<double>& top, const std::vector<double>& bottom,
                                   double t0, double dt, std::size_t ns, const SubIntervalDivision& d, int index)
{
    SeismicSubIntervalMask m;
    m.traceCount = std::min(top.size(), bottom.size());
    m.sampleCount = ns;
    m.values.assign(m.traceCount * ns, 0);
    if (index < 1 || index > 4 || !finite(dt) || dt <= 0 || !finite(t0)) return m;
    const double tEnd = t0 + dt * double(ns > 0 ? ns - 1 : 0);
    std::array<double, 5> f{0.0, d.fractions[0], d.fractions[1], d.fractions[2], 1.0};
    for (std::size_t tr = 0; tr < m.traceCount; ++tr) {
        const double a = top[tr], b = bottom[tr];
        auto* row = m.values.data() + tr * ns;
        if (!finite(a) || !finite(b) || !(b > a) || b < t0 || a > tEnd) {
            // 缺失/倒置/交叉/完全越界：只能待复核，不划入任何层段
            std::fill(row, row + ns, std::uint8_t(2));
            ++m.reviewTraceCount;
            continue;
        }
        const double lo = a + f[index - 1] * (b - a), hi = a + f[index] * (b - a);
        for (std::size_t k = 0; k < ns; ++k) {
            const double t = t0 + dt * double(k);
            // 半开区间 [lo,hi)：相邻子层段不重叠；最底的 S4 包含底界
            if (t >= lo && (t < hi || (index == 4 && t <= hi))) { row[k] = 1; ++m.insideCount; }
        }
        if (a < t0 || b > tEnd) ++m.reviewTraceCount; // 层段部分超出体采样范围
    }
    return m;
}

std::string subIntervalFingerprint(const StratigraphicScheme& s, const SubIntervalId& id)
{
    const auto* sub = findSubInterval(s, id);
    if (!sub) return {};
    const auto* iv = findInterval(s, sub->interval);
    std::ostringstream text;
    text << id.value << '|' << sub->index << '|' << sub->status;
    if (iv) text << '|' << iv->top.value << '|' << iv->bottom.value << '|' << iv->status;
    // 只取界定本子层段的两条内部分界：Sn 的顶 = 分界 n-1，底 = 分界 n（S1 顶、S4 底是父单元界面）
    auto edge = [&](int k) { return k >= 0 && k < 3 && (k == sub->index - 2 || k == sub->index - 1); };
    // 指纹只含几何：方案级比例 + 各井实际生效的顶/底。状态/锁定变化（如把派生面
    // 原位确认）不改变几何，不应使成果失效。
    if (const auto* d = divisionOf(s, sub->interval))
        for (int k = 0; k < 3; ++k) if (edge(k)) text << '|' << num(d->fractions[k]);
    for (const auto& o : s.wellOccurrences) {
        if (!(o.interval == sub->interval)) continue;
        const auto r = rangeOf(s,o.wellId,id,&o);
        text << "|w" << o.wellId << ':' << o.depthType << o.depthUnit << (r.resolved ? 1 : 0) << num(r.top) << ',' << num(r.bottom);
    }
    return fnv(text.str());
}

std::string wellSubIntervalFingerprint(const StratigraphicScheme& s, const SubIntervalId& id, long long wellId)
{
    const auto* sub=findSubInterval(s,id);
    const auto* interval=sub?findInterval(s,sub->interval):nullptr;
    if(!interval || wellId<=0)return {};
    std::vector<const WellIntervalOccurrence*> occurrences;
    for(const auto &occurrence:s.wellOccurrences)if(occurrence.wellId==wellId&&occurrence.interval==sub->interval)
        occurrences.push_back(&occurrence);
    if(occurrences.size()>1) {
        std::sort(occurrences.begin(),occurrences.end(),[](const auto *a,const auto *b){return a->occurrenceId<b->occurrenceId;});
        std::ostringstream values;values<<"well-md-occurrences-v1|"<<id.value<<'|'<<wellId<<'|'<<sub->index<<'|'<<sub->status
            <<'|'<<interval->top.value<<'|'<<interval->bottom.value<<'|'<<interval->status;
        for(const auto *occurrence:occurrences){const auto range=rangeOf(s,wellId,id,occurrence);
            values<<'|'<<occurrence->occurrenceId<<'|'<<range.depthType<<'|'<<range.depthUnit<<'|'<<range.resolved
                  <<'|'<<num(range.top)<<'|'<<num(range.bottom);}
        return fnv(values.str());
    }
    const auto range=wellRange(s,wellId,id);
    std::ostringstream text;
    text<<"well-md-v1|"<<id.value<<'|'<<sub->index<<'|'<<sub->status<<'|'<<interval->top.value<<'|'<<interval->bottom.value
        <<'|'<<interval->status<<'|'<<wellId<<'|'<<range.depthType<<'|'<<range.depthUnit<<'|'<<range.resolved
        <<'|'<<num(range.top)<<'|'<<num(range.bottom);
    return fnv(text.str());
}

std::string seismicSubIntervalFingerprint(const StratigraphicScheme& s, const SubIntervalId& id)
{
    const auto* sub=findSubInterval(s,id);
    const auto* interval=sub?findInterval(s,sub->interval):nullptr;
    if(!interval || sub->index<1 || sub->index>SubIntervalCount)return {};
    const auto* top=findBoundary(s,interval->top);
    const auto* bottom=findBoundary(s,interval->bottom);
    std::array<double,5> edges{0.,.25,.5,.75,1.};
    if(const auto* division=divisionOf(s,interval->id))
        edges={0.,division->fractions[0],division->fractions[1],division->fractions[2],1.};
    std::ostringstream text;
    text<<"seismic-window-v1|"<<id.value<<'|'<<sub->index<<'|'<<sub->status<<'|'<<interval->top.value<<'|'<<interval->bottom.value
        <<'|'<<interval->status<<'|'<<(top?top->seismicHorizonResourceId:0)<<'|'<<(bottom?bottom->seismicHorizonResourceId:0)
        <<'|'<<num(edges[size_t(sub->index-1)])<<'|'<<num(edges[size_t(sub->index)]);
    return fnv(text.str());
}

std::string resultGeometryFingerprint(const StratigraphicScheme& s, const SubIntervalResultBinding& binding)
{
    const auto one=[&](const SubIntervalId& id) {
        if(binding.kind=="well_facies" && binding.wellId>0)return wellSubIntervalFingerprint(s,id,binding.wellId);
        if(binding.kind=="seismic_facies")return seismicSubIntervalFingerprint(s,id);
        return subIntervalFingerprint(s,id);
    };
    if(!binding.subInterval.empty())return one(binding.subInterval);
    if(!binding.aggregate || !findInterval(s,binding.interval))return {};
    std::ostringstream text;
    for(const auto* sub:subIntervalsOf(s,binding.interval))text<<sub->id.value<<':'<<one(sub->id)<<'|';
    return fnv(text.str());
}

std::vector<SubIntervalId> impactedSubIntervals(const StratigraphicScheme& before, const StratigraphicScheme& after)
{
    std::vector<SubIntervalId> out;
    std::set<std::string> ids;
    for (const auto& sub : before.subIntervals) ids.insert(sub.id.value);
    for (const auto& sub : after.subIntervals) ids.insert(sub.id.value);
    for (const auto& id : ids) {
        SubIntervalId sid{id};
        if (subIntervalFingerprint(before, sid) != subIntervalFingerprint(after, sid)) out.push_back(sid);
    }
    return out;
}

LegacyUnitResolution resolveLegacyLabel(const StratigraphicScheme& s, const std::string& raw)
{
    LegacyUnitResolution r{"legacy_unresolved", {}, {}, {}};
    std::string label = raw;
    // 统一几种连接符：en dash、em dash、'-'、'~'
    for (const std::string dash : {"\xE2\x80\x93", "\xE2\x80\x94", "~"}) {
        for (std::size_t p; (p = label.find(dash)) != std::string::npos;) label.replace(p, dash.size(), "-");
    }
    int index = 0;
    if (auto slash = label.find('/'); slash != std::string::npos) {
        const std::string tail = label.substr(slash + 1);
        if (tail.size() == 2 && (tail[0] == 'S' || tail[0] == 's') && tail[1] >= '1' && tail[1] <= '4') index = tail[1] - '0';
        else { r.reason = "无法识别的子层段后缀"; return r; }
        label = label.substr(0, slash);
    } else if (label.size() > 3 && label[label.size() - 3] == '-' &&
               (label[label.size() - 2] == 'S' || label[label.size() - 2] == 's') &&
               label.back() >= '1' && label.back() <= '4') {
        index = label.back() - '0';
        label = label.substr(0, label.size() - 3);
    }
    const auto dash = label.find('-');
    if (dash == std::string::npos) {
        r.reason = "只有单个界面标识，不能确定是哪一个父单元或子层段";
        return r;
    }
    const std::string topName = label.substr(0, dash), bottomName = label.substr(dash + 1);
    for (const auto& iv : s.intervals) {
        const auto* t = findBoundary(s, iv.top); const auto* b = findBoundary(s, iv.bottom);
        if (t && b && t->name == topName && b->name == bottomName) {
            r.interval = iv.id;
            if (index) for (const auto* sub : subIntervalsOf(s, iv.id)) if (sub->index == index) r.subInterval = sub->id;
            r.status = "resolved_candidate";
            r.reason = index ? "显式顶底与子层段序号匹配" : "显式顶底匹配父单元（未指定子层段）";
            return r;
        }
    }
    r.reason = "顶底界面在当前方案中不相邻或不存在";
    return r;
}
}
