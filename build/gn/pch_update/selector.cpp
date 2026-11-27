#include "pch_model.h"

#include "common.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_map>

namespace NPchUpdate {

    namespace {

        constexpr double NEG_INF = -std::numeric_limits<double>::infinity();
        constexpr int MAX_FAILED_DELTA_SEEDS = 48;

        template <class T>
        void SortUnique(std::vector<T>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        uint64_t HashIntVec(const std::vector<int>& values) {
            uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(values.size()) * 0xBF58476D1CE4E5B9ull);
            for (int v : values) {
                uint64_t x = uint64_t(uint32_t(v)) + 0x9E3779B97F4A7C15ull;
                x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
                x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
                h ^= x ^ (x >> 31);
                h = (h << 13) | (h >> 51);
                h = h * 5 + 0x52DCE729ull;
            }
            return h;
        }

        struct TConsumerBreakdown {
            double ParseSaved = 0.0;
            double LoadCost = 0.0;
            double ExtraCompiles = 0.0;
            double Net() const { return ParseSaved - LoadCost - ExtraCompiles; }
            void Add(const TConsumerBreakdown& o, double sign = 1.0) {
                ParseSaved += sign * o.ParseSaved;
                LoadCost += sign * o.LoadCost;
                ExtraCompiles += sign * o.ExtraCompiles;
            }
        };

        struct TPreparedSpec {
            std::vector<int> Units;
            uint64_t UnitsHash = 0;
            double TotalParseSec = 0.0;
            std::vector<uint64_t> CommitBits;
            size_t CommitCount = 0;
        };

        struct TGroupCost {
            double Bytes = 1.0;
            double SingleBuildSec = 0.001;
            double ExpectedBuildCpuSec = 0.001;
        };

        class TModelContext {
        public:
            const TAnalysisModel& M;
            const TModelParams& P;
            const TSelectorFeedback& Fb;
            const int Scenarios;
            const size_t BitWords;
            const double Inc;
            const double LambdaPerByte;

            std::vector<std::vector<uint64_t>> UnitBits;
            std::vector<std::vector<uint64_t>> FileBits;
            std::vector<double> FileBaseFreq;
            std::vector<double> FileFloorSec;
            std::vector<std::vector<int>> Closures;
            std::vector<std::vector<uint64_t>> ClosureBits;
            std::vector<char> RootEligible;
            std::vector<char> BaseUnitAllowed;
            std::vector<char> BaseRootEligible;

            TModelContext(const TAnalysisModel& model, const TModelParams& params, const TSelectorFeedback& feedback)
                : M(model)
                , P(params)
                , Fb(feedback)
                , Scenarios(std::max(1, model.Scenarios))
                , BitWords(size_t((std::max(0, model.Scenarios) + 63) / 64))
                , Inc((1.0 - params.Q) / double(std::max(1, model.Scenarios)))
                , LambdaPerByte(std::max(0.0, params.MinRatio) / 1e6)
            {
                const size_t nUnits = M.Units.size();
                const size_t nFiles = M.Files.size();

                UnitBits.assign(nUnits, std::vector<uint64_t>(BitWords, 0));
                for (size_t u = 0; u < nUnits; ++u) {
                    for (int c : M.Units[u].Changes) {
                        if (c >= 0 && c < M.Scenarios) {
                            UnitBits[u][size_t(c) >> 6] |= (uint64_t(1) << (c & 63));
                        }
                    }
                }

                FileBits.assign(nFiles, std::vector<uint64_t>(BitWords, 0));
                FileBaseFreq.resize(nFiles, P.Q);
                FileFloorSec.resize(nFiles, 0.0);
                for (size_t i = 0; i < nFiles; ++i) {
                    const TTuFile& f = M.Files[i];
                    for (int c : f.Changes) {
                        if (c >= 0 && c < M.Scenarios) {
                            FileBits[i][size_t(c) >> 6] |= (uint64_t(1) << (c & 63));
                        }
                    }
                    FileBaseFreq[i] = P.Q + Inc * double(f.Changes.size());
                    double floorSec = 0.02 * f.CompileSec;
                    if (f.Traced && f.FrontendSec > 0.0 && f.CompileSec > f.FrontendSec) {
                        floorSec = std::max(f.CompileSec - f.FrontendSec, 0.01 * f.CompileSec);
                    }
                    FileFloorSec[i] = std::min(std::max(0.0, floorSec), f.CompileSec);
                }

                Closures.resize(nUnits);
                ClosureBits.assign(nUnits, std::vector<uint64_t>(BitWords, 0));
                RootEligible.assign(nUnits, 0);
                BaseUnitAllowed.assign(nUnits, 0);
                BaseRootEligible.assign(nUnits, 0);

                for (size_t u = 0; u < nUnits; ++u) {
                    const THeaderUnit& unit = M.Units[u];
                    BaseUnitAllowed[u] = unit.SafeLink && !unit.Generated &&
                        (unit.Changes.empty() || P.Q >= 1.0 ||
                         (M.Scenarios >= 20 && unit.Changes.size() * 50 <= size_t(M.Scenarios)));

                    const std::string& inc = unit.Include;
                    if (!inc.empty()) {
                        const std::string inner = inc.size() > 2 ? inc.substr(1, inc.size() - 2) : std::string();
                        const bool absQuoted = inc.size() > 2 && inc.front() == '"' && IsAbs(inner);
                        const bool fragment = EndsWith(inner, ".inc") || EndsWith(inner, ".def");
                        RootEligible[u] = !absQuoted && !fragment;
                    }
                    Closures[u] = ComputeUnitClosure(M, {int(u)});
                    for (int v : Closures[u]) {
                        OrBits(ClosureBits[u], UnitBits[size_t(v)]);
                    }
                }
                for (size_t u = 0; u < nUnits; ++u) {
                    if (!RootEligible[u]) { continue; }
                    bool ok = true;
                    for (int v : Closures[u]) {
                        if (!BaseUnitAllowed[size_t(v)]) { ok = false; break; }
                    }
                    BaseRootEligible[u] = ok;
                }
            }

            static void OrBits(std::vector<uint64_t>& dst, const std::vector<uint64_t>& src) {
                for (size_t w = 0; w < dst.size(); ++w) {
                    dst[w] |= src[w];
                }
            }

            static size_t PopCount(const std::vector<uint64_t>& bits) {
                size_t total = 0;
                for (uint64_t w : bits) {
                    total += size_t(std::popcount(w));
                }
                return total;
            }

            static size_t PopCountDiff(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
                size_t total = 0;
                for (size_t w = 0; w < a.size(); ++w) {
                    total += size_t(std::popcount(a[w] & ~b[w]));
                }
                return total;
            }

            bool IsRootBanned(int cls, int root) const {
                if (cls < 0 || size_t(cls) >= Fb.BannedUnits.size()) { return false; }
                const auto& banned = Fb.BannedUnits[size_t(cls)];
                for (int v : Closures[size_t(root)]) {
                    if (size_t(v) < banned.size() && banned[size_t(v)]) {
                        return true;
                    }
                }
                return false;
            }

            double UnitBytes(int cls, int u) const {
                double scale = P.PchBytesPerSourceByte;
                if (cls >= 0 && size_t(cls) < Fb.UnitBytesScale.size() &&
                    size_t(u) < Fb.UnitBytesScale[size_t(cls)].size() &&
                    Fb.UnitBytesScale[size_t(cls)][size_t(u)] > 0.0) {
                    scale = Fb.UnitBytesScale[size_t(cls)][size_t(u)];
                }
                return M.Units[size_t(u)].SourceBytes * scale;
            }

            double UnitBuildSec(int cls, int u, bool isBase) const {
                double scale = isBase ? P.BaseBuildScale : P.DeltaBuildScale;
                if (cls >= 0 && size_t(cls) < Fb.UnitBuildScale.size() &&
                    size_t(u) < Fb.UnitBuildScale[size_t(cls)].size() &&
                    Fb.UnitBuildScale[size_t(cls)][size_t(u)] > 0.0) {
                    scale = Fb.UnitBuildScale[size_t(cls)][size_t(u)];
                }
                return M.Units[size_t(u)].ParseSec * scale;
            }

            TPreparedSpec PrepareSpec(std::vector<int> units) const {
                SortUnique(units);
                TPreparedSpec spec;
                spec.UnitsHash = HashIntVec(units);
                spec.CommitBits.assign(BitWords, 0);
                for (int u : units) {
                    spec.TotalParseSec += M.Units[size_t(u)].ParseSec;
                    OrBits(spec.CommitBits, UnitBits[size_t(u)]);
                }
                spec.CommitCount = PopCount(spec.CommitBits);
                spec.Units = std::move(units);
                return spec;
            }

            TGroupCost GroupCost(const TPchGroup& g, const TPchGroup* parent, const TPreparedSpec& spec) const {
                const double freq = P.Q + Inc * double(spec.CommitCount);
                if (g.ActualBytes > 0) {
                    const double buildSec = std::max(0.001, g.ActualBuildSec);
                    return {double(g.ActualBytes), buildSec, freq * buildSec};
                }
                if (!Fb.KnownArtifacts.empty()) {
                    std::vector<int> pRoots = parent ? parent->Roots : std::vector<int>{};
                    std::vector<int> ownRoots = g.Roots;
                    SortUnique(pRoots);
                    SortUnique(ownRoots);
                    const auto it = Fb.KnownArtifacts.find(TArtifactKey{g.Class, pRoots, ownRoots});
                    if (it != Fb.KnownArtifacts.end() && it->second.Bytes > 0) {
                        const double buildSec = std::max(0.001, it->second.BuildSec);
                        return {double(it->second.Bytes), buildSec, freq * buildSec};
                    }
                }
                const bool isBase = (parent == nullptr);
                const std::vector<int>* pUnits = parent ? &parent->Units : nullptr;
                double rawBytes = 0.0;
                double rawBuild = 0.0;
                double parentParse = 0.0;
                if (pUnits) {
                    for (int pu : *pUnits) { parentParse += M.Units[size_t(pu)].ParseSec; }
                }
                size_t j = 0;
                for (int u : spec.Units) {
                    while (pUnits && j < pUnits->size() && (*pUnits)[j] < u) { ++j; }
                    if (pUnits && j < pUnits->size() && (*pUnits)[j] == u) { continue; }
                    rawBytes += UnitBytes(g.Class, u);
                    rawBuild += UnitBuildSec(g.Class, u, isBase);
                }
                const double bytes = std::max(1.0, rawBytes);
                const double singleBuild = std::max(0.001, rawBuild + (isBase ? 0.0 : P.LoadShare * parentParse));
                return {bytes, singleBuild, freq * singleBuild};
            }

            double EvalTarget(int targetId, const TPreparedSpec& spec, TConsumerBreakdown* breakdown = nullptr) const {
                if (!TargetCanUseUnits(M, targetId, spec.Units)) {
                    return NEG_INF;
                }
                const double loadSec = P.LoadShare * spec.TotalParseSec;
                double total = 0.0;
                for (int fIdx : M.Targets[size_t(targetId)].Files) {
                    const TTuFile& f = M.Files[size_t(fIdx)];
                    double saved = 0.0;
                    size_t j = 0;
                    for (const auto& [u, own] : f.Own) {
                        while (j < spec.Units.size() && spec.Units[j] < u) { ++j; }
                        if (j == spec.Units.size()) { break; }
                        if (spec.Units[j] == u) { saved += own; }
                    }
                    const double floorSec = FileFloorSec[size_t(fIdx)];
                    const double withLoad = std::max(floorSec, f.CompileSec - saved + loadSec);
                    const double baseFreq = FileBaseFreq[size_t(fIdx)];
                    const double extraFreq = Inc * double(PopCountDiff(spec.CommitBits, FileBits[size_t(fIdx)]));
                    total += baseFreq * (f.CompileSec - withLoad) - extraFreq * withLoad;
                    if (breakdown) {
                        const double withoutLoad = std::max(floorSec, f.CompileSec - saved);
                        breakdown->ParseSaved += baseFreq * (f.CompileSec - withoutLoad);
                        breakdown->LoadCost += baseFreq * (withLoad - withoutLoad);
                        breakdown->ExtraCompiles += extraFreq * withLoad;
                    }
                }
                return total;
            }
        };

        // Per-class RAM memoization cache: avoids recomputing (target, PCH) scores across steps.
        class TClassMemo {
        public:
            explicit TClassMemo(const TModelContext& ctx) : Ctx_(ctx) {}

            double Benefit(int targetId, const TPreparedSpec& spec) {
                const Key key{targetId, spec.UnitsHash, int(spec.Units.size())};
                const auto it = Cache_.find(key);
                if (it != Cache_.end()) {
                    return it->second;
                }
                const double val = Ctx_.EvalTarget(targetId, spec, nullptr);
                Cache_.emplace(key, val);
                return val;
            }

        private:
            struct Key {
                int TargetId = 0;
                uint64_t Hash = 0;
                int Size = 0;
                bool operator==(const Key& o) const = default;
            };
            struct KeyHash {
                size_t operator()(const Key& k) const noexcept {
                    return size_t(k.Hash ^ (uint64_t(uint32_t(k.TargetId)) * 0x9E3779B97F4A7C15ull) ^ uint64_t(k.Size));
                }
            };
            const TModelContext& Ctx_;
            std::unordered_map<Key, double, KeyHash> Cache_;
        };

        // Greedy root picker for a single PCH (base or delta) under CPU-time + MinRatio.
        TPchGroup PickRoots(const TModelContext& ctx, int cls, const std::vector<int>& targetIds,
                            const TPchGroup* parent, bool requireBaseSafe) {
            TPchGroup out;
            out.Class = cls;
            if (targetIds.empty()) { return out; }
            out.RepresentativeFile = ctx.M.Targets[size_t(targetIds.front())].Files.front();

            const size_t nUnits = ctx.M.Units.size();
            std::vector<char> selected(nUnits, 0);
            std::vector<uint64_t> commitBits(ctx.BitWords, 0);
            double parentParseSec = 0.0;
            if (parent) {
                out.Units = parent->Units;
                for (int u : parent->Units) {
                    selected[size_t(u)] = 1;
                    parentParseSec += ctx.M.Units[size_t(u)].ParseSec;
                    TModelContext::OrBits(commitBits, ctx.UnitBits[size_t(u)]);
                }
            }

            std::vector<int> files;
            for (int t : targetIds) {
                const auto& tf = ctx.M.Targets[size_t(t)].Files;
                files.insert(files.end(), tf.begin(), tf.end());
            }
            if (files.empty()) { return out; }

            const size_t nFiles = files.size();
            std::vector<std::vector<std::pair<int, double>>> unitPostings(nUnits);
            std::vector<char> parsedByBundle(nUnits, 0);
            std::vector<double> savedSoFar(nFiles, 0.0);

            for (size_t loc = 0; loc < nFiles; ++loc) {
                const TTuFile& f = ctx.M.Files[size_t(files[loc])];
                for (const auto& [u, own] : f.Own) {
                    parsedByBundle[size_t(u)] = 1;
                    if (own > 0.0) {
                        unitPostings[size_t(u)].emplace_back(int(loc), own);
                        if (selected[size_t(u)]) {
                            savedSoFar[loc] += own;
                        }
                    }
                }
            }

            std::vector<int> sortedTargets = targetIds;
            SortUnique(sortedTargets);
            auto allowedForTargets = [&](int root) {
                const auto& forbidden = ctx.M.Units[size_t(root)].ForbiddenTargets;
                if (forbidden.empty()) { return true; }
                size_t i = 0, j = 0;
                while (i < sortedTargets.size() && j < forbidden.size()) {
                    if (sortedTargets[i] < forbidden[j]) { ++i; }
                    else if (forbidden[j] < sortedTargets[i]) { ++j; }
                    else { return false; }
                }
                return true;
            };

            std::vector<int> candidates;
            for (size_t u = 0; u < nUnits; ++u) {
                if (selected[u] || !parsedByBundle[u]) { continue; }
                if (!(requireBaseSafe ? ctx.BaseRootEligible[u] : ctx.RootEligible[u])) { continue; }
                if (ctx.IsRootBanned(cls, int(u)) || !allowedForTargets(int(u))) { continue; }
                candidates.push_back(int(u));
            }
            if (candidates.empty()) { return out; }

            const bool isBase = (parent == nullptr);
            auto buildCpuCost = [&](double ownBuildSec, size_t totalCommits) {
                const double freq = ctx.P.Q + ctx.Inc * double(totalCommits);
                const double single = std::max(0.001, ownBuildSec + (isBase ? 0.0 : ctx.P.LoadShare * parentParseSec));
                return freq * single;
            };

            double loadParseSoFar = parentParseSec;
            double ownBuildSoFar = 0.0;
            size_t commitsSoFar = TModelContext::PopCount(commitBits);
            double buildCostSoFar = isBase && out.Units.empty() ? 0.0 : buildCpuCost(ownBuildSoFar, commitsSoFar);

            auto fileCostAt = [&](size_t loc, double saved, double loadParse, const std::vector<uint64_t>& pBits) {
                const int gFile = files[loc];
                const TTuFile& f = ctx.M.Files[size_t(gFile)];
                const double withLoad = std::max(ctx.FileFloorSec[size_t(gFile)],
                                                 f.CompileSec - saved + ctx.P.LoadShare * loadParse);
                const double freq = ctx.FileBaseFreq[size_t(gFile)] +
                    ctx.Inc * double(TModelContext::PopCountDiff(pBits, ctx.FileBits[size_t(gFile)]));
                return freq * withLoad;
            };

            std::vector<double> curFileCost(nFiles, 0.0);
            double totalFileCostSoFar = 0.0;
            for (size_t loc = 0; loc < nFiles; ++loc) {
                curFileCost[loc] = fileCostAt(loc, savedSoFar[loc], loadParseSoFar, commitBits);
                totalFileCostSoFar += curFileCost[loc];
            }

            std::vector<double> deltaSaved(nFiles, 0.0);
            std::vector<char> touched(nFiles, 0);
            std::vector<int> touchedList;
            std::vector<uint64_t> trialBits(ctx.BitWords, 0);

            struct TEval {
                double Gain = NEG_INF;
                double Bytes = 1.0;
                double AddParse = 0.0;
                double AddBuild = 0.0;
                double NewBuildCost = 0.0;
            };

            auto evaluateRoot = [&](int root) -> TEval {
                TEval ev;
                double rawBytes = 0.0;
                for (size_t w = 0; w < ctx.BitWords; ++w) { trialBits[w] = commitBits[w]; }
                bool anyNewUnit = false;
                for (int u : ctx.Closures[size_t(root)]) {
                    if (selected[size_t(u)]) { continue; }
                    anyNewUnit = true;
                    ev.AddParse += ctx.M.Units[size_t(u)].ParseSec;
                    ev.AddBuild += ctx.UnitBuildSec(cls, u, isBase);
                    rawBytes += ctx.UnitBytes(cls, u);
                    TModelContext::OrBits(trialBits, ctx.UnitBits[size_t(u)]);
                    for (const auto& [loc, own] : unitPostings[size_t(u)]) {
                        if (!touched[size_t(loc)]) {
                            touched[size_t(loc)] = 1;
                            touchedList.push_back(loc);
                        }
                        deltaSaved[size_t(loc)] += own;
                    }
                }
                if (!anyNewUnit) {
                    for (int loc : touchedList) { touched[size_t(loc)] = 0; deltaSaved[size_t(loc)] = 0.0; }
                    touchedList.clear();
                    return ev;
                }
                ev.Bytes = std::max(1.0, rawBytes);
                const size_t newCommits = TModelContext::PopCount(trialBits);
                ev.NewBuildCost = buildCpuCost(ownBuildSoFar + ev.AddBuild, newCommits);
                const double newLoadParse = loadParseSoFar + ev.AddParse;

                double newTotalFileCost = 0.0;
                if (newCommits == commitsSoFar) {
                    // No fresh commits: only Load and touched files' savings change.
                    for (size_t loc = 0; loc < nFiles; ++loc) {
                        const double s = savedSoFar[loc] + (touched[loc] ? deltaSaved[loc] : 0.0);
                        newTotalFileCost += fileCostAt(loc, s, newLoadParse, commitBits);
                    }
                } else {
                    for (size_t loc = 0; loc < nFiles; ++loc) {
                        const double s = savedSoFar[loc] + (touched[loc] ? deltaSaved[loc] : 0.0);
                        newTotalFileCost += fileCostAt(loc, s, newLoadParse, trialBits);
                    }
                }
                for (int loc : touchedList) {
                    touched[size_t(loc)] = 0;
                    deltaSaved[size_t(loc)] = 0.0;
                }
                touchedList.clear();

                ev.Gain = (totalFileCostSoFar - newTotalFileCost) - (ev.NewBuildCost - buildCostSoFar);
                return ev;
            };

            struct THeapItem {
                double Density = 0.0;
                int Root = 0;
                bool operator<(const THeapItem& o) const {
                    return Density != o.Density ? Density < o.Density : Root > o.Root;
                }
            };

            std::priority_queue<THeapItem> heap;
            std::vector<TEval> cachedEval(nUnits);
            std::vector<int> evalEpoch(nUnits, -1);
            std::vector<char> inDeferred(nUnits, 0);
            std::vector<int> deferred;
            int epoch = 0;
            int lastSweepEpoch = 0;

            auto passesThreshold = [&](const TEval& e) {
                return e.Gain > 1e-9 && e.Gain >= ctx.LambdaPerByte * e.Bytes;
            };

            for (int r : candidates) {
                cachedEval[size_t(r)] = evaluateRoot(r);
                evalEpoch[size_t(r)] = epoch;
                if (passesThreshold(cachedEval[size_t(r)])) {
                    heap.push({cachedEval[size_t(r)].Gain / cachedEval[size_t(r)].Bytes, r});
                } else {
                    inDeferred[size_t(r)] = 1;
                    deferred.push_back(r);
                }
            }

            for (;;) {
                while (!heap.empty()) {
                    const THeapItem top = heap.top();
                    heap.pop();
                    const int r = top.Root;
                    if (selected[size_t(r)]) { continue; }
                    if (evalEpoch[size_t(r)] != epoch) {
                        cachedEval[size_t(r)] = evaluateRoot(r);
                        evalEpoch[size_t(r)] = epoch;
                        if (!passesThreshold(cachedEval[size_t(r)])) {
                            if (!inDeferred[size_t(r)]) {
                                inDeferred[size_t(r)] = 1;
                                deferred.push_back(r);
                            }
                            continue;
                        }
                        const double density = cachedEval[size_t(r)].Gain / cachedEval[size_t(r)].Bytes;
                        if (!heap.empty() && THeapItem{density, r} < heap.top()) {
                            heap.push({density, r});
                            continue;
                        }
                    }
                    const TEval& ev = cachedEval[size_t(r)];
                    if (!passesThreshold(ev)) {
                        if (!inDeferred[size_t(r)]) {
                            inDeferred[size_t(r)] = 1;
                            deferred.push_back(r);
                        }
                        continue;
                    }

                    // Accept root `r` and update PCH state.
                    out.Roots.push_back(r);
                    std::vector<uint64_t> freshCommitMask(ctx.BitWords, 0);
                    for (int u : ctx.Closures[size_t(r)]) {
                        if (selected[size_t(u)]) { continue; }
                        selected[size_t(u)] = 1;
                        out.Units.push_back(u);
                        for (const auto& [loc, own] : unitPostings[size_t(u)]) {
                            savedSoFar[size_t(loc)] += own;
                        }
                        for (size_t w = 0; w < ctx.BitWords; ++w) {
                            freshCommitMask[w] |= (ctx.UnitBits[size_t(u)][w] & ~commitBits[w]);
                            commitBits[w] |= ctx.UnitBits[size_t(u)][w];
                        }
                    }
                    loadParseSoFar += ev.AddParse;
                    ownBuildSoFar += ev.AddBuild;
                    commitsSoFar = TModelContext::PopCount(commitBits);
                    buildCostSoFar = ev.NewBuildCost;
                    totalFileCostSoFar = 0.0;
                    for (size_t loc = 0; loc < nFiles; ++loc) {
                        curFileCost[loc] = fileCostAt(loc, savedSoFar[loc], loadParseSoFar, commitBits);
                        totalFileCostSoFar += curFileCost[loc];
                    }
                    ++epoch;

                    // Supermodularity wakeup: re-queue deferred roots that share a newly covered
                    // unit or a newly added commit with `r`.
                    std::vector<int> stillDeferred;
                    stillDeferred.reserve(deferred.size());
                    for (int dRoot : deferred) {
                        if (selected[size_t(dRoot)]) {
                            inDeferred[size_t(dRoot)] = 0;
                            continue;
                        }
                        bool shares = false;
                        for (size_t w = 0; w < ctx.BitWords; ++w) {
                            if (ctx.ClosureBits[size_t(dRoot)][w] & freshCommitMask[w]) {
                                shares = true;
                                break;
                            }
                        }
                        if (!shares) {
                            for (int u : ctx.Closures[size_t(dRoot)]) {
                                if (selected[size_t(u)]) { shares = true; break; }
                            }
                        }
                        if (shares) {
                            inDeferred[size_t(dRoot)] = 0;
                            cachedEval[size_t(dRoot)] = evaluateRoot(dRoot);
                            evalEpoch[size_t(dRoot)] = epoch;
                            if (passesThreshold(cachedEval[size_t(dRoot)])) {
                                heap.push({cachedEval[size_t(dRoot)].Gain / cachedEval[size_t(dRoot)].Bytes, dRoot});
                            } else {
                                inDeferred[size_t(dRoot)] = 1;
                                stillDeferred.push_back(dRoot);
                            }
                        } else {
                            stillDeferred.push_back(dRoot);
                        }
                    }
                    deferred = std::move(stillDeferred);
                }

                // Final exact sweep over remaining deferred roots if state advanced since last sweep.
                if (epoch == lastSweepEpoch || deferred.empty()) {
                    break;
                }
                lastSweepEpoch = epoch;
                std::vector<int> nextDeferred;
                for (int dRoot : deferred) {
                    if (selected[size_t(dRoot)]) {
                        inDeferred[size_t(dRoot)] = 0;
                        continue;
                    }
                    cachedEval[size_t(dRoot)] = evaluateRoot(dRoot);
                    evalEpoch[size_t(dRoot)] = epoch;
                    if (passesThreshold(cachedEval[size_t(dRoot)])) {
                        inDeferred[size_t(dRoot)] = 0;
                        heap.push({cachedEval[size_t(dRoot)].Gain / cachedEval[size_t(dRoot)].Bytes, dRoot});
                    } else {
                        nextDeferred.push_back(dRoot);
                    }
                }
                deferred = std::move(nextDeferred);
                if (heap.empty()) {
                    break;
                }
            }

            SortUnique(out.Units);
            return out;
        }

        struct TClassSelection {
            std::vector<TPchGroup> Groups;
            // Pairs of (targetId, localGroupIdx).
            std::vector<std::pair<int, int>> Assignments;
            std::vector<std::pair<int, std::string>> Reasons;
        };

        TClassSelection SelectClass(const TModelContext& ctx, int cls, const std::vector<int>& rawTargets) {
            TClassSelection result;
            if (rawTargets.empty()) { return result; }

            std::vector<int> targets = rawTargets;
            std::stable_sort(targets.begin(), targets.end(), [&](int a, int b) {
                return ctx.M.Targets[size_t(a)].Label < ctx.M.Targets[size_t(b)].Label;
            });

            TClassMemo memo(ctx);
            const size_t nUnits = ctx.M.Units.size();

            // 1. Layer 1: Shared Base PCH from targets that parse safe base headers.
            std::vector<int> basePool;
            for (int t : targets) {
                bool hasBaseHdr = false;
                for (int fIdx : ctx.M.Targets[size_t(t)].Files) {
                    for (const auto& [u, own] : ctx.M.Files[size_t(fIdx)].Own) {
                        if (own > 0.0 && ctx.BaseUnitAllowed[size_t(u)]) {
                            hasBaseHdr = true;
                            break;
                        }
                    }
                    if (hasBaseHdr) { break; }
                }
                if (hasBaseHdr) { basePool.push_back(t); }
            }

            TPchGroup baseGroup = PickRoots(ctx, cls, basePool, nullptr, /*requireBaseSafe=*/true);
            if (!baseGroup.Roots.empty()) {
                TPreparedSpec baseSpec = ctx.PrepareSpec(baseGroup.Units);
                std::vector<int> gaining;
                for (int t : targets) {
                    if (memo.Benefit(t, baseSpec) > 0.0) { gaining.push_back(t); }
                }
                if (!gaining.empty() && gaining != basePool) {
                    baseGroup = PickRoots(ctx, cls, gaining, nullptr, /*requireBaseSafe=*/true);
                }
            }

            bool hasBase = !baseGroup.Roots.empty();
            TPreparedSpec baseSpec = hasBase ? ctx.PrepareSpec(baseGroup.Units) : TPreparedSpec{};
            std::vector<char> inBase(nUnits, 0);
            for (int u : baseGroup.Units) { inBase[size_t(u)] = 1; }

            // 2. Layer 2: Greedy delta construction by target bundles around top residual headers.
            auto targetParsesAnyRoot = [&](int t, const std::vector<int>& sortedRoots) {
                for (int fIdx : ctx.M.Targets[size_t(t)].Files) {
                    const auto& ownList = ctx.M.Files[size_t(fIdx)].Own;
                    size_t i = 0, j = 0;
                    while (i < ownList.size() && j < sortedRoots.size()) {
                        if (ownList[i].first < sortedRoots[j]) { ++i; }
                        else if (sortedRoots[j] < ownList[i].first) { ++j; }
                        else {
                            if (ownList[i].second > 0.0) { return true; }
                            ++i;
                            ++j;
                        }
                    }
                }
                return false;
            };

            std::vector<TPchGroup> deltas;
            std::vector<TPreparedSpec> deltaSpecs;
            std::vector<int> unassigned = targets;
            std::vector<char> triedSeed(nUnits, 0);
            int failedSeeds = 0;

            while (!unassigned.empty() && failedSeeds < MAX_FAILED_DELTA_SEEDS) {
                int bestSeed = -1;
                double bestSeedScore = 0.0;
                std::vector<int> bestSeedBundle;

                for (size_t u = 0; u < nUnits; ++u) {
                    if (triedSeed[u] || inBase[u] || !ctx.RootEligible[u] || ctx.IsRootBanned(cls, int(u))) {
                        continue;
                    }
                    double deltaParse = 0.0;
                    for (int v : ctx.Closures[u]) {
                        if (!inBase[size_t(v)]) { deltaParse += ctx.M.Units[size_t(v)].ParseSec; }
                    }
                    if (deltaParse <= 0.0 && ctx.Closures[u].size() <= 1) {
                        continue;
                    }
                    const double approxLoad = ctx.P.LoadShare * deltaParse;
                    double score = 0.0;
                    std::vector<int> bundle;
                    for (int t : unassigned) {
                        if (!TargetCanUseUnits(ctx.M, t, {int(u)})) { continue; }
                        double tGain = 0.0;
                        bool parsesSeed = false;
                        for (int fIdx : ctx.M.Targets[size_t(t)].Files) {
                            const TTuFile& f = ctx.M.Files[size_t(fIdx)];
                            double s = 0.0;
                            size_t j = 0;
                            const auto& cl = ctx.Closures[u];
                            for (const auto& [id, own] : f.Own) {
                                if (id == int(u) && own > 0.0) { parsesSeed = true; }
                                if (inBase[size_t(id)]) { continue; }
                                while (j < cl.size() && cl[j] < id) { ++j; }
                                if (j == cl.size()) { break; }
                                if (cl[j] == id) { s += own; }
                            }
                            tGain += ctx.FileBaseFreq[size_t(fIdx)] * (s - approxLoad);
                        }
                        if (parsesSeed && tGain > 0.0) {
                            score += tGain;
                            bundle.push_back(t);
                        }
                    }
                    if (!bundle.empty() && score > bestSeedScore) {
                        bestSeedScore = score;
                        bestSeed = int(u);
                        bestSeedBundle = std::move(bundle);
                    }
                }

                if (bestSeed < 0 || bestSeedBundle.empty()) {
                    break;
                }
                triedSeed[size_t(bestSeed)] = 1;

                const TPchGroup* parentPtr = hasBase ? &baseGroup : nullptr;
                TPchGroup delta = PickRoots(ctx, cls, bestSeedBundle, parentPtr, /*requireBaseSafe=*/false);

                // If the bundle does not benefit from a heavy base, also check standalone delta.
                if (hasBase) {
                    TPchGroup standalone = PickRoots(ctx, cls, bestSeedBundle, nullptr, /*requireBaseSafe=*/false);
                    if (!standalone.Roots.empty()) {
                        const TPreparedSpec dSpec = delta.Roots.empty() ? TPreparedSpec{} : ctx.PrepareSpec(delta.Units);
                        const TPreparedSpec sSpec = ctx.PrepareSpec(standalone.Units);
                        auto bundleScore = [&](const TPchGroup& g, const TPchGroup* p, const TPreparedSpec& sp) {
                            if (g.Roots.empty()) { return NEG_INF; }
                            double sum = 0.0;
                            for (int t : bestSeedBundle) {
                                const double bGain = hasBase ? std::max(0.0, memo.Benefit(t, baseSpec)) : 0.0;
                                sum += std::max(0.0, memo.Benefit(t, sp) - bGain);
                            }
                            const TGroupCost gc = ctx.GroupCost(g, p, sp);
                            return sum - gc.ExpectedBuildCpuSec - ctx.LambdaPerByte * gc.Bytes;
                        };
                        if (bundleScore(standalone, nullptr, sSpec) > bundleScore(delta, &baseGroup, dSpec) + 1e-6) {
                            delta = std::move(standalone);
                            parentPtr = nullptr;
                        }
                    }
                }

                if (delta.Roots.empty()) {
                    ++failedSeeds;
                    continue;
                }

                TPreparedSpec dSpec = ctx.PrepareSpec(delta.Units);
                std::vector<int> filtered;
                for (int t : bestSeedBundle) {
                    const double bGain = hasBase ? std::max(0.0, memo.Benefit(t, baseSpec)) : 0.0;
                    if (memo.Benefit(t, dSpec) > bGain + 1e-9) {
                        filtered.push_back(t);
                    }
                }
                if (!filtered.empty() && filtered.size() < bestSeedBundle.size()) {
                    TPchGroup refined = PickRoots(ctx, cls, filtered, parentPtr, /*requireBaseSafe=*/false);
                    if (!refined.Roots.empty()) {
                        delta = std::move(refined);
                        dSpec = ctx.PrepareSpec(delta.Units);
                    }
                }

                std::vector<int> sortedDeltaRoots = delta.Roots;
                SortUnique(sortedDeltaRoots);
                std::vector<int> winners;
                double marginalConsumerGain = 0.0;
                for (int t : unassigned) {
                    if (!targetParsesAnyRoot(t, sortedDeltaRoots)) { continue; }
                    const double bGain = hasBase ? std::max(0.0, memo.Benefit(t, baseSpec)) : 0.0;
                    const double dGain = memo.Benefit(t, dSpec);
                    if (dGain > bGain + 1e-9) {
                        winners.push_back(t);
                        marginalConsumerGain += (dGain - bGain);
                    }
                }

                const TGroupCost dCost = ctx.GroupCost(delta, parentPtr, dSpec);
                const double netMarginal = marginalConsumerGain - dCost.ExpectedBuildCpuSec;
                if (!winners.empty() && netMarginal > 1e-9 && netMarginal >= ctx.LambdaPerByte * dCost.Bytes) {
                    delta.Parent = (parentPtr != nullptr) ? 0 : -1;
                    deltas.push_back(std::move(delta));
                    deltaSpecs.push_back(std::move(dSpec));
                    std::vector<int> remaining;
                    size_t wIdx = 0;
                    for (int t : unassigned) {
                        if (wIdx < winners.size() && winners[wIdx] == t) {
                            ++wIdx;
                            continue;
                        }
                        remaining.push_back(t);
                    }
                    unassigned = std::move(remaining);
                } else {
                    ++failedSeeds;
                }
            }

            // 3. Shared-unit hoisting: if >= 2 deltas pull in the same base-safe unit (even one
            // unprofitable alone), hoist it into Base when doing so saves net CPU + disk cost.
            if (deltas.size() >= 2) {
                std::vector<int> unitDeltaCount(nUnits, 0);
                for (const TPchGroup& d : deltas) {
                    for (int u : d.Units) {
                        if (!inBase[size_t(u)]) { ++unitDeltaCount[size_t(u)]; }
                    }
                }
                std::vector<int> sharedSafeRoots;
                for (size_t u = 0; u < nUnits; ++u) {
                    if (unitDeltaCount[u] >= 2 && ctx.BaseRootEligible[u] && !ctx.IsRootBanned(cls, int(u))) {
                        sharedSafeRoots.push_back(int(u));
                    }
                }
                if (!sharedSafeRoots.empty()) {
                    TPchGroup trialBase = baseGroup;
                    trialBase.Class = cls;
                    if (trialBase.RepresentativeFile < 0) {
                        trialBase.RepresentativeFile = ctx.M.Targets[size_t(targets.front())].Files.front();
                    }
                    trialBase.Roots.insert(trialBase.Roots.end(), sharedSafeRoots.begin(), sharedSafeRoots.end());
                    SortUnique(trialBase.Roots);
                    trialBase.Units = ComputeUnitClosure(ctx.M, trialBase.Roots);
                    const TPreparedSpec trialBaseSpec = ctx.PrepareSpec(trialBase.Units);

                    auto evalCandidateSet = [&](const TPchGroup* bPtr, const TPreparedSpec& bSpec,
                                                const std::vector<TPchGroup>& dList) {
                        std::vector<TPreparedSpec> spList;
                        spList.reserve(dList.size());
                        for (const auto& d : dList) { spList.push_back(ctx.PrepareSpec(d.Units)); }

                        std::vector<char> dUsed(dList.size(), 0);
                        bool bUsed = false;
                        double totalBenefit = 0.0;
                        for (int t : targets) {
                            double best = 0.0;
                            int chosen = -2; // -2: none, -1: base, >=0: delta
                            if (bPtr && !bPtr->Roots.empty()) {
                                const double g = memo.Benefit(t, bSpec);
                                if (g > best) { best = g; chosen = -1; }
                            }
                            for (size_t k = 0; k < dList.size(); ++k) {
                                const double g = memo.Benefit(t, spList[k]);
                                if (g > best) { best = g; chosen = int(k); }
                            }
                            totalBenefit += best;
                            if (chosen == -1) { bUsed = true; }
                            else if (chosen >= 0) {
                                dUsed[size_t(chosen)] = 1;
                                if (dList[size_t(chosen)].Parent >= 0) { bUsed = true; }
                            }
                        }
                        double penalty = 0.0;
                        if (bUsed && bPtr) {
                            const TGroupCost bc = ctx.GroupCost(*bPtr, nullptr, bSpec);
                            penalty += bc.ExpectedBuildCpuSec + ctx.LambdaPerByte * bc.Bytes;
                        }
                        for (size_t k = 0; k < dList.size(); ++k) {
                            if (!dUsed[k]) { continue; }
                            const TPchGroup* p = (dList[k].Parent >= 0 && bPtr) ? bPtr : nullptr;
                            const TGroupCost dc = ctx.GroupCost(dList[k], p, spList[k]);
                            penalty += dc.ExpectedBuildCpuSec + ctx.LambdaPerByte * dc.Bytes;
                        }
                        return totalBenefit - penalty;
                    };

                    std::vector<char> inTrialBase(nUnits, 0);
                    for (int u : trialBase.Units) { inTrialBase[size_t(u)] = 1; }
                    std::vector<TPchGroup> hoistedDeltas;
                    for (const TPchGroup& d : deltas) {
                        TPchGroup hd = d;
                        hd.Roots.erase(std::remove_if(hd.Roots.begin(), hd.Roots.end(), [&](int r) {
                            return inTrialBase[size_t(r)] != 0;
                        }), hd.Roots.end());
                        if (hd.Roots.empty()) { continue; }
                        hd.Parent = 0;
                        hd.Units = ComputeUnitClosure(ctx.M, hd.Roots);
                        hd.Units.insert(hd.Units.end(), trialBase.Units.begin(), trialBase.Units.end());
                        SortUnique(hd.Units);
                        hoistedDeltas.push_back(std::move(hd));
                    }

                    const double beforeScore = evalCandidateSet(hasBase ? &baseGroup : nullptr, baseSpec, deltas);
                    const double afterScore = evalCandidateSet(&trialBase, trialBaseSpec, hoistedDeltas);
                    if (afterScore > beforeScore + 1e-6) {
                        baseGroup = std::move(trialBase);
                        baseSpec = trialBaseSpec;
                        hasBase = true;
                        deltas = std::move(hoistedDeltas);
                        deltaSpecs.clear();
                        for (const auto& d : deltas) { deltaSpecs.push_back(ctx.PrepareSpec(d.Units)); }
                    }
                }
            }

            // 4. Assemble local groups (base at index 0 if present, then deltas) and run 1-pass refinement.
            if (hasBase) {
                result.Groups.push_back(baseGroup);
                for (TPchGroup& d : deltas) {
                    d.Parent = (d.Parent >= 0) ? 0 : -1;
                    result.Groups.push_back(std::move(d));
                }
            } else {
                for (TPchGroup& d : deltas) {
                    d.Parent = -1;
                    result.Groups.push_back(std::move(d));
                }
            }
            if (result.Groups.empty()) {
                for (int t : targets) {
                    result.Assignments.emplace_back(t, -1);
                    result.Reasons.emplace_back(t, "below-min-ratio");
                }
                return result;
            }

            auto assignBest = [&](std::vector<int>& outAssign) {
                std::vector<TPreparedSpec> specs;
                specs.reserve(result.Groups.size());
                for (const auto& g : result.Groups) { specs.push_back(ctx.PrepareSpec(g.Units)); }
                outAssign.assign(targets.size(), -1);
                for (size_t i = 0; i < targets.size(); ++i) {
                    double best = 0.0;
                    int bestG = -1;
                    for (size_t g = 0; g < result.Groups.size(); ++g) {
                        const double val = memo.Benefit(targets[i], specs[g]);
                        if (val > best + 1e-9) {
                            best = val;
                            bestG = int(g);
                        }
                    }
                    outAssign[i] = bestG;
                }
            };

            std::vector<int> curAssign;
            assignBest(curAssign);

            // Refine each delta once strictly on its assigned owners.
            const size_t deltaStart = hasBase ? 1 : 0;
            bool anyRebuilt = false;
            for (size_t g = deltaStart; g < result.Groups.size(); ++g) {
                std::vector<int> owners;
                for (size_t i = 0; i < targets.size(); ++i) {
                    if (curAssign[i] == int(g)) { owners.push_back(targets[i]); }
                }
                if (owners.empty()) { continue; }
                const TPchGroup* parentPtr = result.Groups[g].Parent >= 0
                    ? &result.Groups[size_t(result.Groups[g].Parent)] : nullptr;
                TPchGroup rebuilt = PickRoots(ctx, cls, owners, parentPtr, /*requireBaseSafe=*/false);
                if (!rebuilt.Roots.empty() && rebuilt.Units != result.Groups[g].Units) {
                    rebuilt.Parent = result.Groups[g].Parent;
                    result.Groups[g] = std::move(rebuilt);
                    anyRebuilt = true;
                }
            }
            if (anyRebuilt) {
                assignBest(curAssign);
            }

            for (size_t i = 0; i < targets.size(); ++i) {
                result.Assignments.emplace_back(targets[i], curAssign[i]);
                result.Reasons.emplace_back(targets[i], curAssign[i] >= 0 ? "selected" : "no-estimated-benefit");
            }
            return result;
        }

    } // namespace

    // =========================================================================
    // Public API
    // =========================================================================

    TModelParams MakeModelParams(const TWorkflowConfig& cfg) {
        TModelParams p;
        p.Q = cfg.Q;
        p.MinRatio = cfg.MinRatio;
        p.DiskBudgetBytes = cfg.DiskBudgetBytes;
        p.AnalysisJobs = std::max(1, cfg.AnalysisJobs);
        return p;
    }

    bool TargetCanUseUnits(const TAnalysisModel& model, int targetId, const std::vector<int>& units) {
        if (targetId < 0 || size_t(targetId) >= model.Targets.size()) {
            return false;
        }
        for (int u : units) {
            const auto& forbidden = model.Units.at(size_t(u)).ForbiddenTargets;
            if (std::binary_search(forbidden.begin(), forbidden.end(), targetId)) {
                return false;
            }
        }
        return true;
    }

    std::vector<int> CompactPchLayout(TPchLayout& layout) {
        const size_t n = layout.Groups.size();
        std::vector<char> keep(n, 0);
        for (int g : layout.Assignment) {
            for (int cur = g; cur >= 0 && !keep[size_t(cur)]; cur = layout.Groups[size_t(cur)].Parent) {
                keep[size_t(cur)] = 1;
            }
        }
        // Emit root PCHs (Parent < 0) first, then child deltas (Parent >= 0), preserving order.
        std::vector<int> remap(n, -1);
        std::vector<TPchGroup> compacted;
        compacted.reserve(n);
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t g = 0; g < n; ++g) {
                if (!keep[g]) { continue; }
                const bool isRoot = layout.Groups[g].Parent < 0;
                if ((pass == 0) != isRoot) { continue; }
                TPchGroup copy = std::move(layout.Groups[g]);
                if (copy.Parent >= 0) {
                    if (remap[size_t(copy.Parent)] < 0) {
                        Fail("CompactPchLayout: child PCH references unkept or non-root parent");
                    }
                    copy.Parent = remap[size_t(copy.Parent)];
                }
                remap[g] = int(compacted.size());
                compacted.push_back(std::move(copy));
            }
        }
        for (int& g : layout.Assignment) {
            if (g >= 0) { g = remap[size_t(g)]; }
        }
        layout.Groups = std::move(compacted);
        return remap;
    }

    bool EnforcePaybackAndBudget(const TAnalysisModel& model,
                                 const TModelParams& params,
                                 TPchLayout& layout,
                                 bool allowSiblings,
                                 std::string* report) {
        if (layout.Assignment.size() != model.Targets.size()) {
            Fail("EnforcePaybackAndBudget: assignment size mismatch");
        }
        if (layout.Reasons.size() != model.Targets.size()) {
            layout.Reasons.assign(model.Targets.size(), "unassigned");
        }

        const TSelectorFeedback emptyFb;
        const TModelContext ctx(model, params, emptyFb);
        TClassMemo memo(ctx);

        const size_t nGroups = layout.Groups.size();
        std::vector<TPreparedSpec> specs(nGroups);
        std::vector<TGroupCost> costs(nGroups);
        for (size_t g = 0; g < nGroups; ++g) {
            specs[g] = ctx.PrepareSpec(layout.Groups[g].Units);
        }
        for (size_t g = 0; g < nGroups; ++g) {
            const int p = layout.Groups[g].Parent;
            costs[g] = ctx.GroupCost(layout.Groups[g], p >= 0 ? &layout.Groups[size_t(p)] : nullptr, specs[g]);
        }

        std::vector<char> alive(nGroups, 0);
        for (int g : layout.Assignment) {
            if (g >= 0) {
                alive[size_t(g)] = 1;
                if (layout.Groups[size_t(g)].Parent >= 0) {
                    alive[size_t(layout.Groups[size_t(g)].Parent)] = 1;
                }
            }
        }
        const std::vector<char> initialAlive = alive;
        const std::vector<int> initialAssignment = layout.Assignment;

        auto bestFallbackFor = [&](int targetId, int excludeGroup, double* outBenefit) -> int {
            double best = 0.0;
            int bestG = -1;
            const int curParent = layout.Groups[size_t(excludeGroup)].Parent;
            if (curParent >= 0 && alive[size_t(curParent)]) {
                const double val = memo.Benefit(targetId, specs[size_t(curParent)]);
                if (val > best + 1e-9) {
                    best = val;
                    bestG = curParent;
                }
            }
            if (allowSiblings) {
                const int cls = model.Targets[size_t(targetId)].Class;
                for (size_t s = 0; s < nGroups; ++s) {
                    if (int(s) == excludeGroup || int(s) == curParent || !alive[s]) { continue; }
                    if (layout.Groups[s].Class != cls) { continue; }
                    const double val = memo.Benefit(targetId, specs[s]);
                    if (val > best + 1e-9) {
                        best = val;
                        bestG = int(s);
                    }
                }
            }
            *outBenefit = best;
            return bestG;
        };

        std::ostringstream rep;
        rep.precision(10);
        if (report) {
            rep << "stage\tkind\tgroup\tparent\tclass\towners\tbytes\tconsumer_saved_s\tbuild_s\tnet_s\tratio_s_per_mb\tdecision\n";
        }

        // Phase 1: Weakest-first marginal delta payback (merges duplicates into siblings when allowSiblings).
        for (;;) {
            int weakestDelta = -1;
            double weakestRatio = std::numeric_limits<double>::infinity();
            double weakestNet = 0.0;
            double weakestCons = 0.0;
            size_t weakestOwners = 0;

            for (size_t g = 0; g < nGroups; ++g) {
                if (!alive[g] || layout.Groups[g].Parent < 0) { continue; }
                std::vector<int> owners;
                for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                    if (layout.Assignment[t] == int(g)) { owners.push_back(int(t)); }
                }
                if (owners.empty()) {
                    alive[g] = 0;
                    continue;
                }
                double marginalCons = 0.0;
                for (int t : owners) {
                    double fbVal = 0.0;
                    (void)bestFallbackFor(t, int(g), &fbVal);
                    marginalCons += memo.Benefit(t, specs[g]) - fbVal;
                }
                const double net = marginalCons - costs[g].ExpectedBuildCpuSec;
                const double ratioPerMb = net / (costs[g].Bytes / 1e6);
                const bool fails = !(net > 1e-9 && net >= ctx.LambdaPerByte * costs[g].Bytes);
                if (fails && ratioPerMb < weakestRatio) {
                    weakestRatio = ratioPerMb;
                    weakestDelta = int(g);
                    weakestNet = net;
                    weakestCons = marginalCons;
                    weakestOwners = owners.size();
                }
            }

            if (weakestDelta < 0) {
                break;
            }
            const size_t g = size_t(weakestDelta);
            alive[g] = 0;
            if (report) {
                rep << "payback\tdelta\t" << g << '\t' << layout.Groups[g].Parent << '\t' << layout.Groups[g].Class
                    << '\t' << weakestOwners << '\t' << uint64_t(std::llround(costs[g].Bytes)) << '\t'
                    << weakestCons << '\t' << costs[g].ExpectedBuildCpuSec << '\t' << weakestNet << '\t'
                    << weakestRatio << "\tdrop\n";
            }
            for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                if (layout.Assignment[t] != weakestDelta) { continue; }
                double fbVal = 0.0;
                const int dst = bestFallbackFor(int(t), weakestDelta, &fbVal);
                layout.Assignment[t] = dst;
                layout.Reasons[t] = dst < 0 ? "delta-payback-none"
                    : (dst == layout.Groups[g].Parent ? "delta-payback-parent" : "delta-payback-sibling");
            }
        }

        // Phase 3: Whole-family (base + surviving children, or standalone root) payback vs NONE.
        for (size_t b = 0; b < nGroups; ++b) {
            if (!alive[b] || layout.Groups[b].Parent >= 0) { continue; }
            std::vector<int> familyMembers;
            std::vector<size_t> children;
            double familyCons = 0.0;
            double familyBuild = costs[b].ExpectedBuildCpuSec;
            double familyBytes = costs[b].Bytes;

            for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                if (layout.Assignment[t] == int(b)) {
                    familyMembers.push_back(int(t));
                    familyCons += memo.Benefit(int(t), specs[b]);
                }
            }
            for (size_t d = 0; d < nGroups; ++d) {
                if (!alive[d] || layout.Groups[d].Parent != int(b)) { continue; }
                bool childHasOwners = false;
                for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                    if (layout.Assignment[t] == int(d)) {
                        childHasOwners = true;
                        familyMembers.push_back(int(t));
                        familyCons += memo.Benefit(int(t), specs[d]);
                    }
                }
                if (childHasOwners) {
                    children.push_back(d);
                    familyBuild += costs[d].ExpectedBuildCpuSec;
                    familyBytes += costs[d].Bytes;
                } else {
                    alive[d] = 0;
                }
            }
            if (familyMembers.empty()) {
                alive[b] = 0;
                continue;
            }
            const double net = familyCons - familyBuild;
            const double ratioPerMb = net / (familyBytes / 1e6);
            const bool keep = net > 1e-9 && net >= ctx.LambdaPerByte * familyBytes;
            if (report) {
                rep << "payback\tfamily\t" << b << "\t-1\t" << layout.Groups[b].Class << '\t'
                    << familyMembers.size() << '\t' << uint64_t(std::llround(familyBytes)) << '\t'
                    << familyCons << '\t' << familyBuild << '\t' << net << '\t' << ratioPerMb
                    << '\t' << (keep ? "keep" : "drop") << '\n';
            }
            if (!keep) {
                alive[b] = 0;
                for (size_t d : children) { alive[d] = 0; }
                for (int t : familyMembers) {
                    layout.Assignment[size_t(t)] = -1;
                    layout.Reasons[size_t(t)] = "family-below-min-ratio";
                }
            }
        }

        // Phase 4: Optional hard disk cap (--disk) by lowest lost-net-CPU-sec per freed byte.
        auto totalActiveBytes = [&]() {
            double sum = 0.0;
            for (size_t g = 0; g < nGroups; ++g) {
                if (alive[g]) { sum += costs[g].Bytes; }
            }
            return sum;
        };

        while (params.DiskBudgetBytes > 0.0 && totalActiveBytes() > params.DiskBudgetBytes + 0.5) {
            int bestVictim = -1;
            bool victimIsFamily = false;
            double bestLossPerByte = std::numeric_limits<double>::infinity();
            double bestLossSec = 0.0;
            double bestFreedBytes = 0.0;

            for (size_t g = 0; g < nGroups; ++g) {
                if (!alive[g]) { continue; }
                if (layout.Groups[g].Parent >= 0) {
                    const int parent = layout.Groups[g].Parent;
                    double consLoss = 0.0;
                    size_t ownCnt = 0;
                    for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                        if (layout.Assignment[t] == int(g)) {
                            ++ownCnt;
                            double fbVal = 0.0;
                            (void)bestFallbackFor(int(t), int(g), &fbVal);
                            consLoss += memo.Benefit(int(t), specs[g]) - fbVal;
                        }
                    }
                    double freed = costs[g].Bytes;
                    double buildSaved = costs[g].ExpectedBuildCpuSec;
                    // Check if dropping this delta also empties its parent base.
                    bool parentHasOtherUsers = false;
                    for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                        if (layout.Assignment[t] == parent) { parentHasOtherUsers = true; break; }
                        if (layout.Assignment[t] == int(g)) {
                            double fbVal = 0.0;
                            if (bestFallbackFor(int(t), int(g), &fbVal) == parent) {
                                parentHasOtherUsers = true;
                                break;
                            }
                        }
                    }
                    if (!parentHasOtherUsers) {
                        for (size_t o = 0; o < nGroups; ++o) {
                            if (o != g && alive[o] && layout.Groups[o].Parent == parent) {
                                parentHasOtherUsers = true;
                                break;
                            }
                        }
                    }
                    if (!parentHasOtherUsers && parent >= 0 && alive[size_t(parent)]) {
                        freed += costs[size_t(parent)].Bytes;
                        buildSaved += costs[size_t(parent)].ExpectedBuildCpuSec;
                    }
                    const double netLoss = consLoss - buildSaved;
                    const double density = netLoss / std::max(1.0, freed);
                    if (ownCnt > 0 && density < bestLossPerByte) {
                        bestLossPerByte = density;
                        bestVictim = int(g);
                        victimIsFamily = false;
                        bestLossSec = netLoss;
                        bestFreedBytes = freed;
                    }
                } else {
                    double famCons = 0.0;
                    double famBuild = costs[g].ExpectedBuildCpuSec;
                    double famBytes = costs[g].Bytes;
                    for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                        if (layout.Assignment[t] == int(g)) {
                            famCons += memo.Benefit(int(t), specs[g]);
                        }
                    }
                    for (size_t d = 0; d < nGroups; ++d) {
                        if (alive[d] && layout.Groups[d].Parent == int(g)) {
                            famBuild += costs[d].ExpectedBuildCpuSec;
                            famBytes += costs[d].Bytes;
                            for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                                if (layout.Assignment[t] == int(d)) {
                                    famCons += memo.Benefit(int(t), specs[d]);
                                }
                            }
                        }
                    }
                    const double netLoss = famCons - famBuild;
                    const double density = netLoss / std::max(1.0, famBytes);
                    if (density < bestLossPerByte) {
                        bestLossPerByte = density;
                        bestVictim = int(g);
                        victimIsFamily = true;
                        bestLossSec = netLoss;
                        bestFreedBytes = famBytes;
                    }
                }
            }

            if (bestVictim < 0) {
                Fail("cannot fit PCH artifacts into --disk budget");
            }
            if (victimIsFamily) {
                alive[size_t(bestVictim)] = 0;
                for (size_t d = 0; d < nGroups; ++d) {
                    if (layout.Groups[d].Parent == bestVictim) { alive[d] = 0; }
                }
                size_t moved = 0;
                for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                    const int cur = layout.Assignment[t];
                    if (cur == bestVictim || (cur >= 0 && layout.Groups[size_t(cur)].Parent == bestVictim)) {
                        layout.Assignment[t] = -1;
                        layout.Reasons[t] = "disk-budget";
                        ++moved;
                    }
                }
                if (report) {
                    rep << "disk-cap\tfamily\t" << bestVictim << "\t-1\t" << layout.Groups[size_t(bestVictim)].Class
                        << '\t' << moved << '\t' << uint64_t(std::llround(bestFreedBytes)) << "\t0\t0\t"
                        << bestLossSec << '\t' << (bestLossSec / (bestFreedBytes / 1e6)) << "\tevict\n";
                }
            } else {
                const int parent = layout.Groups[size_t(bestVictim)].Parent;
                alive[size_t(bestVictim)] = 0;
                size_t moved = 0;
                for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                    if (layout.Assignment[t] != bestVictim) { continue; }
                    ++moved;
                    double fbVal = 0.0;
                    const int dst = bestFallbackFor(int(t), bestVictim, &fbVal);
                    layout.Assignment[t] = dst;
                    layout.Reasons[t] = dst < 0 ? "disk-budget" : "disk-fallback";
                }
                // Release an emptied parent base immediately.
                if (parent >= 0 && alive[size_t(parent)]) {
                    bool anyLeft = false;
                    for (int a : layout.Assignment) {
                        if (a == parent || (a >= 0 && layout.Groups[size_t(a)].Parent == parent)) {
                            anyLeft = true;
                            break;
                        }
                    }
                    if (!anyLeft) { alive[size_t(parent)] = 0; }
                }
                if (report) {
                    rep << "disk-cap\tdelta\t" << bestVictim << '\t' << parent << '\t'
                        << layout.Groups[size_t(bestVictim)].Class << '\t' << moved << '\t'
                        << uint64_t(std::llround(bestFreedBytes)) << "\t0\t0\t"
                        << bestLossSec << '\t' << (bestLossSec / (bestFreedBytes / 1e6)) << "\tevict\n";
                }
            }
        }

        if (report) {
            *report = rep.str();
        }
        return alive == initialAlive && layout.Assignment == initialAssignment;
    }

    TPchLayout SelectGreedyLayout(const TAnalysisModel& model,
                                  const TModelParams& params,
                                  const TSelectorFeedback& feedback,
                                  TSelectionDiagnostics* diagnostics) {
        const TModelContext ctx(model, params, feedback);
        const size_t nClasses = model.ClassFlags.size();
        std::vector<std::vector<int>> targetsByClass(nClasses);

        TPchLayout layout;
        layout.Assignment.assign(model.Targets.size(), -1);
        layout.Reasons.assign(model.Targets.size(), "unassigned");

        for (size_t t = 0; t < model.Targets.size(); ++t) {
            const TTargetOwner& owner = model.Targets[t];
            if (!owner.PchEligible || owner.Class < 0 || size_t(owner.Class) >= nClasses) {
                layout.Reasons[t] = owner.IneligibleReason.empty() ? "ineligible" : owner.IneligibleReason;
                continue;
            }
            targetsByClass[size_t(owner.Class)].push_back(int(t));
        }

        std::vector<TClassSelection> perClass(nClasses);
        ParallelFor(nClasses, AnalysisThreads(params.AnalysisJobs), [&](size_t cls) {
            perClass[cls] = SelectClass(ctx, int(cls), targetsByClass[cls]);
        });

        for (size_t cls = 0; cls < nClasses; ++cls) {
            const int offset = int(layout.Groups.size());
            for (TPchGroup& g : perClass[cls].Groups) {
                if (g.Parent >= 0) { g.Parent += offset; }
                layout.Groups.push_back(std::move(g));
            }
            for (const auto& [t, localG] : perClass[cls].Assignments) {
                layout.Assignment[size_t(t)] = localG >= 0 ? offset + localG : -1;
            }
            for (const auto& [t, reason] : perClass[cls].Reasons) {
                layout.Reasons[size_t(t)] = reason;
            }
        }

        std::string paybackReport;
        EnforcePaybackAndBudget(model, params, layout, /*allowSiblings=*/true,
                                diagnostics ? &paybackReport : nullptr);
        CompactPchLayout(layout);

        if (diagnostics) {
            diagnostics->PaybackTsv = paybackReport;

            std::ostringstream groupsOut;
            groupsOut << "group\tclass\tparent\tclosure_units\troot_headers\n";
            for (size_t g = 0; g < layout.Groups.size(); ++g) {
                const TPchGroup& grp = layout.Groups[g];
                groupsOut << g << '\t' << grp.Class << '\t' << grp.Parent << '\t' << grp.Units.size() << '\t';
                const auto ordered = LayeredRootOrder(model, grp.Roots);
                for (size_t r = 0; r < ordered.size(); ++r) {
                    if (r) { groupsOut << ';'; }
                    groupsOut << model.Units[size_t(ordered[r])].Include;
                }
                groupsOut << '\n';
            }
            diagnostics->GroupsTsv = groupsOut.str();

            std::ostringstream targetsOut;
            targetsOut << "target\tclass\tselected_group\treason\n";
            for (size_t t = 0; t < model.Targets.size(); ++t) {
                targetsOut << model.Targets[t].Label << '\t' << model.Targets[t].Class << '\t'
                           << layout.Assignment[t] << '\t' << layout.Reasons[t] << '\n';
            }
            diagnostics->TargetsTsv = targetsOut.str();
        }
        return layout;
    }

    TLayoutAccounting EvaluateLayoutAccounting(const TAnalysisModel& model,
                                               const TModelParams& params,
                                               const TPchLayout& layout) {
        TLayoutAccounting acc;
        const TSelectorFeedback emptyFb;
        const TModelContext ctx(model, params, emptyFb);

        std::vector<TPreparedSpec> specs(layout.Groups.size());
        for (size_t g = 0; g < layout.Groups.size(); ++g) {
            specs[g] = ctx.PrepareSpec(layout.Groups[g].Units);
        }

        // Compressible CPU-seconds: expected header parse CPU-seconds across all files.
        for (size_t fIdx = 0; fIdx < model.Files.size(); ++fIdx) {
            const TTuFile& f = model.Files[fIdx];
            if (!f.Traced) { continue; }
            const double freq = ctx.FileBaseFreq[fIdx];
            double fileHeaderOwn = 0.0;
            for (const auto& [u, own] : f.Own) {
                if (own > 0.0) { fileHeaderOwn += own; }
            }
            const double maxRemovable = std::max(0.0, f.CompileSec - ctx.FileFloorSec[fIdx]);
            const double compressible = freq * std::min(fileHeaderOwn, maxRemovable);
            acc.TotalCompressibleCpuSec += compressible;

            const int g = (f.Target >= 0 && size_t(f.Target) < layout.Assignment.size())
                ? layout.Assignment[size_t(f.Target)] : -1;
            if (g >= 0) {
                double coveredOwn = 0.0;
                size_t j = 0;
                const auto& units = specs[size_t(g)].Units;
                for (const auto& [u, own] : f.Own) {
                    while (j < units.size() && units[j] < u) { ++j; }
                    if (j == units.size()) { break; }
                    if (units[j] == u && own > 0.0) { coveredOwn += own; }
                }
                acc.CoveredCompressibleCpuSec += freq * std::min(coveredOwn, maxRemovable);
            }
        }

        std::vector<char> active(layout.Groups.size(), 0);
        for (size_t t = 0; t < model.Targets.size(); ++t) {
            const int g = t < layout.Assignment.size() ? layout.Assignment[t] : -1;
            if (g < 0) { continue; }
            acc.GrossConsumerSavedCpuSec += ctx.EvalTarget(int(t), specs[size_t(g)], nullptr);
            for (int cur = g; cur >= 0 && !active[size_t(cur)]; cur = layout.Groups[size_t(cur)].Parent) {
                active[size_t(cur)] = 1;
            }
        }

        for (size_t g = 0; g < layout.Groups.size(); ++g) {
            if (!active[g]) { continue; }
            const int p = layout.Groups[g].Parent;
            const TGroupCost gc = ctx.GroupCost(
                layout.Groups[g], p >= 0 ? &layout.Groups[size_t(p)] : nullptr, specs[g]);
            acc.PchBuildCpuSec += gc.ExpectedBuildCpuSec;
            acc.TotalPchBytes += uint64_t(std::llround(gc.Bytes));
        }
        acc.NetSavedCpuSec = acc.GrossConsumerSavedCpuSec - acc.PchBuildCpuSec;
        return acc;
    }

} // namespace NPchUpdate
