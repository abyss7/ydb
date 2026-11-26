#include "two_step.h"
#include "common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>

namespace NPchUpdate::NTwoStep {
    namespace {
        constexpr double INF = std::numeric_limits<double>::infinity();

        // How many sibling deltas a target weighs as alternatives to its own
        // leaf. A class of many clusters has more than three that share a
        // common header; the best of them for the target may be the fourth.
        constexpr int SiblingLimit = 8;

        // An accepted root of a PCH: what it gains per estimated byte, and
        // those bytes. What the disk prices of a candidate are cut from.
        using TDensity = std::pair<double, double>;

        struct TDependencyCycle : TError {
            TDependencyCycle(const std::string& message, std::vector<int> nodes)
                : TError(message), Nodes(std::move(nodes)) {}
            std::vector<int> Nodes; // closed path, in dependency order
        };

        template <class T>
        void Unique(std::vector<T>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        std::vector<int> Topology(const std::vector<TAction>& actions,
                                  std::vector<std::vector<int>>& users) {
            users.resize(actions.size());
            std::vector<int> count(actions.size());
            std::queue<int> ready;
            for (size_t i = 0; i < actions.size(); ++i) {
                count[i] = int(actions[i].Dependencies.size());
                for (int dep : actions[i].Dependencies) {
                    if (dep < 0 || size_t(dep) >= actions.size()) {
                        Fail("scheduler: invalid dependency");
                    }
                    users[dep].push_back(int(i));
                }
                if (!count[i]) {
                    ready.push(int(i));
                }
            }
            std::vector<int> order;
            while (!ready.empty()) {
                int i = ready.front();
                ready.pop();
                order.push_back(i);
                for (int u : users[i]) {
                    if (!--count[u]) {
                        ready.push(u);
                    }
                }
            }
            if (order.size() != actions.size()) {
                // Every unresolved action still depends on an unresolved action.
                // Follow those edges, then discard the prefix leading into the cycle.
                int node = int(std::find_if(count.begin(), count.end(), [](int n) { return n > 0; }) - count.begin());
                std::vector<int> path, position(actions.size(), -1);
                while (position[node] < 0) {
                    position[node] = int(path.size());
                    path.push_back(node);
                    const auto& deps = actions[node].Dependencies;
                    node = *std::find_if(deps.begin(), deps.end(), [&](int dep) { return count[dep] > 0; });
                }
                std::ostringstream message;
                message << "scheduler: dependency cycle (each action depends on the next):";
                path.push_back(node);
                for (size_t i = position[node]; i < path.size(); ++i) {
                    message << "\n  [" << path[i] << "] " << actions[path[i]].Output;
                }
                throw TDependencyCycle(message.str(), {path.begin() + position[node], path.end()});
            }
            return order;
        }

        std::vector<int> Closure(const TData& d, const std::vector<int>& roots) {
            std::vector<char> seen(d.Units.size());
            std::vector<int> stack = roots;
            std::vector<int> result;
            while (!stack.empty()) {
                int u = stack.back();
                stack.pop_back();
                if (!seen[u]) {
                    seen[u] = true;
                    result.push_back(u);
                    stack.insert(stack.end(), d.Units[u].Children.begin(), d.Units[u].Children.end());
                }
            }
            Unique(result);
            return result;
        }

        double Parse(const TData& d, const std::vector<int>& units) {
            double sum = 0;
            for (int u : units) {
                sum += d.Units[u].Parse;
            }
            return sum;
        }

        // A PCH serializes its header source once, whatever the number of files
        // parsing it. Source volume predicts artifact size far better than parse
        // time, which is dominated by template instantiation.
        double Volume(const TData& d, const std::vector<int>& units) {
            double sum = 0;
            for (int u : units) {
                sum += d.Units[u].Bytes;
            }
            return sum;
        }

        double Saved(const TFile& f, const std::vector<int>& units) {
            double sum = 0;
            for (auto [u, own] : f.Own) {
                if (std::binary_search(units.begin(), units.end(), u)) {
                    sum += own;
                }
            }
            return sum;
        }

        TCoefficients Coeff(const TCalibration& c, int cls) {
            return size_t(cls) < c.Classes.size() ? c.Classes[cls] : TCoefficients();
        }

        // What a PCH takes off a compile under one set of coefficients.
        double PchGain(double saved, double load, const TCoefficients& c) {
            double gain = c.Saved * saved - c.Load * load;
            // A combined fit showing a slowdown must not turn extra, unused
            // headers into an apparent speedup outside the measured feature mix.
            if (c.Saved < 0) { gain = std::min(0.0, gain); }
            return gain;
        }

        // Compile time under one set of coefficients. `base` is the compile's
        // baseline seconds in the model's own scale (the trace, or the anchor
        // of a measured target), `floor` what no PCH removes: the backend.
        double Consumer(double base, double floor, double saved, double load, const TCoefficients& c) {
            return std::max(floor, c.Baseline * base - PchGain(saved, load, c) + c.Residual);
        }

        // Same compiler class, own units and roots. Flags are the class, so
        // the representative and the root order do not change the artifact.
        bool SameContent(const TPch& a, const TPch& b) {
            if (a.Class != b.Class || a.Units != b.Units) { return false; }
            auto x = a.Roots, y = b.Roots;
            Unique(x);
            Unique(y);
            return x == y;
        }

        // The consumer model of one calibration state: class coefficients,
        // the per-target and per-unit corrections a measurement taught, and
        // the per-file anchor at the measured point.
        struct TConsumerModel {
            const TData& D;
            const TCalibration& C;

            TConsumerModel(const TData& d, const TCalibration& c) : D(d), C(c) {}

            TCoefficients Coefficients(int cls) const { return Coeff(C, cls); }

            // Irreducible share of a compile, in trace seconds. A known trace
            // gives the LLVM backend (ExecuteCompiler minus Frontend); without
            // one, 2% stays the prior.
            static double Backend(double time, double frontend) {
                if (frontend > 0 && time > frontend) { return std::max(time - frontend, 0.01 * time); }
                return 0.02 * time;
            }
            double Base(const TFile& f) const {
                return size_t(f.Action) < C.ActionTime.size() ? C.ActionTime[f.Action] : D.Actions[f.Action].Time;
            }
            double Floor(const TFile& f) const {
                if (size_t(f.Action) < C.ActionFloor.size()) { return C.ActionFloor[f.Action]; }
                return std::max(0.0, Coefficients(f.Class).Baseline * Backend(D.Actions[f.Action].Time, f.Frontend));
            }
            // Semantics and template instantiation outside the outermost
            // includes, relative to the time spent inside them.
            static double Templates(const TFile& f) {
                if (f.Frontend <= 0 || f.Outer <= 0 || f.Frontend <= f.Outer) { return 0; }
                return std::min((f.Frontend - f.Outer) / f.Outer, 100.0);
            }
            double Theta(const TFile& f) const {
                return std::max(0.0, 1 + Coefficients(f.Class).Instantiate * Templates(f));
            }
            // Saved-time multiplier of the file's target (empirical Bayes).
            double Gain(const TFile& f) const {
                return f.Target >= 0 && size_t(f.Target) < C.TargetGain.size() ? C.TargetGain[f.Target] : 1.0;
            }
            TCoefficients Coefficients(const TFile& f) const {
                auto c = Coefficients(f.Class);
                c.Saved *= Gain(f);
                return c;
            }
            // Negative: no measurement of this unit, the class coefficients
            // stand. Zero is a leaf that helped its owners nothing.
            double UnitGain(int cls, int u) const {
                return cls >= 0 && size_t(cls) < C.UnitGain.size() && size_t(u) < C.UnitGain[cls].size() &&
                       C.UnitGain[cls][u] >= 0 ? C.UnitGain[cls][u] : 1.0;
            }
            // A unit's PCH bytes per source byte are the header's and the
            // compiler's: what its own class measured, else what any class
            // measured, else the scale of the class.
            double SizeWeight(int cls, int u) const {
                if (cls >= 0 && size_t(cls) < C.UnitSize.size() && size_t(u) < C.UnitSize[cls].size() &&
                    C.UnitSize[cls][u] > 0) {
                    return C.UnitSize[cls][u];
                }
                if (size_t(u) < C.GlobalUnitSize.size() && C.GlobalUnitSize[u] > 0) {
                    return C.GlobalUnitSize[u];
                }
                return Coefficients(cls).Size;
            }
            // The seconds a header takes to serialize per parse second are the
            // header's: what its own class measured, else the role's common
            // scale by the header's share of it that any class measured. A
            // role no class timed leaves the class scale to carry that share.
            double BuildWeight(int cls, int u, bool base) const {
                const auto c = Coefficients(cls);
                const double scale = base ? c.BaseBuild : c.DeltaBuild;
                if (cls >= 0 && size_t(cls) < C.UnitBuild.size() && size_t(u) < C.UnitBuild[cls].size() &&
                    C.UnitBuild[cls][u] > 0) {
                    return C.UnitBuild[cls][u];
                }
                if (size_t(u) < C.GlobalUnitBuild.size() && C.GlobalUnitBuild[u] > 0) {
                    const double common = base ? C.GlobalBaseBuild : C.GlobalDeltaBuild;
                    return (common > 0 ? common : scale) * C.GlobalUnitBuild[u];
                }
                return scale;
            }
            // Effective parse saving of a file under a PCH: the units' own
            // times with their measured multipliers, scaled by what the class
            // gains beyond the includes. Own entries and units are both
            // ascending (extraction emits them so): one merge pass.
            double Saved(const TFile& f, const std::vector<int>& units) const {
                double sum = 0;
                size_t j = 0;
                int previous = -1;
                for (auto [u, own] : f.Own) {
                    if (u < previous) { // an unsorted table: search per entry
                        if (std::binary_search(units.begin(), units.end(), u)) { sum += own * UnitGain(f.Class, u); }
                        continue;
                    }
                    previous = u;
                    while (j < units.size() && units[j] < u) { ++j; }
                    if (j == units.size()) { break; }
                    if (units[j] == u) { sum += own * UnitGain(f.Class, u); }
                }
                return Theta(f) * sum;
            }
            // Deserialization feature: the parse seconds the PCH holds, the
            // scale the class fits its load coefficient in. Measured PCH bytes
            // are not folded in here: a header's bytes per parse second already
            // grows with its templates, and multiplying the two charges a small
            // template wrapper dozens of times over. A leaf that is unusually
            // heavy to deserialize shows in its consumers' residuals, which
            // the calibration projects on its own units (UnitGain).
            double Load(const std::vector<int>& units) const {
                return Parse(D, units);
            }
            double Time(const TFile& f, double saved, double load) const {
                return Consumer(Base(f), Floor(f), saved, load, Coefficients(f));
            }
            double Time(const TFile& f, const TCoefficients& c, double saved, double load) const {
                return Consumer(Base(f), Floor(f), saved, load, c);
            }
            // Own units of a PCH: what it serializes beyond its parent.
            template <class F>
            void ForOwn(const TPch& p, const std::vector<int>* parent, F&& fn) const {
                size_t j = 0;
                for (int u : p.Units) {
                    while (parent && j < parent->size() && (*parent)[j] < u) { ++j; }
                    if (parent && j < parent->size() && (*parent)[j] == u) { continue; }
                    fn(u);
                }
            }
            // The S0 artifact of exactly this content, if the measurement built one.
            const TMeasuredPch* Measured(const TLayout& l, size_t g) const {
                const auto& p = l.Pchs[g];
                const TPch* parent = p.Parent >= 0 ? &l.Pchs[p.Parent] : nullptr;
                for (const auto& m : C.Measured) {
                    if (m.Bytes <= 0 || (m.Pch.Parent >= 0) != (parent != nullptr) || !SameContent(m.Pch, p)) { continue; }
                    if (parent && !SameContent(m.Parent, *parent)) { continue; }
                    return &m;
                }
                return nullptr;
            }
            double ArtifactBytes(const TLayout& l, size_t g) const {
                if (const auto* m = Measured(l, g)) { return m->Bytes; }
                const auto& p = l.Pchs[g];
                const std::vector<int>* parent = p.Parent >= 0 ? &l.Pchs[p.Parent].Units : nullptr;
                double bytes = 0;
                ForOwn(p, parent, [&](int u) { bytes += D.Units[u].Bytes * SizeWeight(p.Class, u); });
                return std::max(1.0, bytes);
            }
            // Build work of a PCH: its measured seconds when S0 built the same
            // artifact, otherwise its own units' build weights plus the prior
            // re-read of the parent (in the class's real seconds).
            double BuildTime(const TLayout& l, size_t g) const {
                if (const auto* m = Measured(l, g)) { return std::max(0.001, m->Time); }
                const auto& p = l.Pchs[g];
                const std::vector<int>* parent = p.Parent >= 0 ? &l.Pchs[p.Parent].Units : nullptr;
                double own = 0, reread = 0;
                ForOwn(p, parent, [&](int u) { own += D.Units[u].Parse * BuildWeight(p.Class, u, p.Parent < 0); });
                if (parent) { reread = Parse(D, *parent); }
                return std::max(0.001, own + reread * 0.02 * Coefficients(p.Class).Baseline);
            }
        };

        // Shared base material: safe for every link and not written by the build.
        // A header that changed in the history is base material still when the
        // change costs the objective nothing (cold-only) or next to nothing: a
        // rare commit, and only where the history is long enough to call it rare.
        // Pick prices the rebuilds of what it accepts; what stays out is a header
        // most commits touch, which reprices every rebuild of the whole accepted
        // body. One changed commit in a thousand used to keep a header of util/
        // out of every base together with the whole cone of headers including it.
        // Generated output is never base material, changed or not: the history
        // lists a generated header with every commit touching its source, so
        // to a changed header it would look like any other. A base holding it
        // would order the code generation into every compile of the class.
        bool BaseMaterial(const TData& d, const TUnit& unit) {
            if (!unit.Safe || !unit.Producers.empty()) { return false; }
            for (const auto& header : unit.Headers) {
                if (StartsWith(header, "<out>/")) { return false; }
            }
            if (unit.Stable) { return true; }
            if (unit.Changes.empty()) { return false; }
            if (d.Q >= 1) { return true; }
            return d.Scenarios >= 20 && unit.Changes.size() * 50 <= size_t(d.Scenarios);
        }

        // Fast local surrogate. Full graph simulations are reserved for the
        // layout shortlist below; no worker-count multiplier on PCH readiness.
        // diskPrice (lambda) is the marginal consumer seconds one resident PCH
        // byte must pay for: roots are admitted only at a gain that stays
        // positive net of that price, and Roots leave in acceptance order,
        // which approximates descending benefit per serialized byte, so the
        // tail can be trimmed wholesale under disk pressure (matryoshka).
        TPch Pick(const TData& d, const TCalibration& calibration, int cls,
                  const std::vector<int>& files, const TPch* base, bool stable,
                  std::map<int, std::vector<int>>& closures, double diskPrice = 0,
                  std::vector<TDensity>* densities = nullptr) {
            TConsumerModel m{d, calibration};
            TPch p;
            p.Class = cls;
            if (files.empty()) { return p; }
            p.Representative = files.front();
            if (base) {
                p.Units = base->Units;
            }
            auto c = Coeff(calibration, cls);
            // What one unit costs or brings in this class after a
            // measurement: its real PCH bytes per source byte, its build
            // seconds per parse second and its saved-time multiplier (the
            // class priors where uncalibrated).
            std::vector<double> gainOf(d.Units.size(), 1.0), sizeOf(d.Units.size()), buildOf(d.Units.size());
            for (size_t u = 0; u < d.Units.size(); ++u) {
                gainOf[u] = m.UnitGain(cls, int(u));
                sizeOf[u] = m.SizeWeight(cls, int(u));
                buildOf[u] = m.BuildWeight(cls, int(u), base == nullptr);
            }
            // A root is any self-contained unit the files parse, by its own
            // time or only through what it includes. An umbrella made of
            // nothing but includes has no own time of its own, and still
            // serializes the headers below it that cannot stand alone.
            std::vector<char> parsed(d.Units.size(), 0);
            for (int f : files) {
                for (const auto& own : d.Files[f].Own) { parsed[own.first] = 1; }
            }
            std::vector<int> candidates;
            for (size_t u = 0; u < d.Units.size(); ++u) {
                const auto& include = d.Units[u].Include;
                // A directly included absolute sysroot path loses Clang's
                // system-header classification. In particular, serializing
                // glibc's stdlib.h this way breaks the mm_malloc.h redeclaration
                // workaround in consumers. Keep such units in transitive costs,
                // but require a normal search-path spelling for injected roots.
                bool absolute = include.size() > 2 && include.front() == '"' &&
                    IsAbs(include.substr(1, include.size() - 2));
                // Textual macro fragments can compile on their own while
                // requiring paired cleanup (protobuf port_def/port_undef).
                // Injecting one into a PCH leaks state into later headers.
                auto name = include.size() > 2 ? include.substr(1, include.size() - 2) : std::string();
                bool fragment = name.ends_with(".inc") || name.ends_with(".def");
                if (!include.empty() && !absolute && !fragment && parsed[u]) {
                    candidates.push_back(int(u));
                }
            }
            std::vector<char> selected(d.Units.size());
            for (int u : p.Units) {
                selected[u] = true;
            }
            // Commits already invalidating the PCH under construction: the
            // parent closure plus every accepted root. A commit touching
            // several related headers is paid once, at the first of them,
            // not once per header (as TSelection::Hit in model.cpp did).
            std::set<int> invalidating;
            for (int u : p.Units) {
                invalidating.insert(d.Units[u].Changes.begin(), d.Units[u].Changes.end());
            }
            // Consumer and build state under the PCH built so far, priced as
            // TWorkCosts does: an extra compile runs with this PCH — its full
            // load and every saving, the candidate's own closure included —
            // and a fresh commit forces a rebuild of the whole accepted body,
            // not only of the new root. A measured target's compiles are
            // anchored at what S0 really took for them.
            const double parentParse = base ? Parse(d, base->Units) : 0;
            double loadSoFar = parentParse;
            auto buildCost = [&](double ownBuild, size_t changes) {
                double frequency = d.Q + (1 - d.Q) * double(changes) / std::max(1, d.Scenarios);
                return frequency * std::max(.001, ownBuild + parentParse * .02 * c.Baseline);
            };
            double buildSoFar = 0;
            double buildCostSoFar = buildCost(buildSoFar, invalidating.size());
            // Reverse index over the local file list: an evaluation and an
            // acceptance touch only the files that actually parse the closure
            // units of the root at hand, never the whole Own table. A posting
            // carries the member's effective saving from the unit and, with
            // its target's multiplier and rebuild frequency, its gain.
            struct TPosting {
                int File;
                double Saved, Gain;
            };
            struct TMember {
                double Theta = 0;
                double Frequency = 0;
                TCoefficients Coefficients;
                const TFile* File = nullptr;
            };
            const int fileCount = int(files.size());
            std::vector<std::vector<TPosting>> unitFiles(d.Units.size());
            std::vector<TMember> member(fileCount);
            std::vector<double> savedSoFar(fileCount);
            for (int i = 0; i < fileCount; ++i) {
                const auto& file = d.Files[files[i]];
                auto& x = member[i];
                x.Theta = m.Theta(file);
                x.Frequency = d.Q + (1 - d.Q) * double(d.Actions[file.Action].Changes.size()) / std::max(1, d.Scenarios);
                x.Coefficients = m.Coefficients(file);
                x.File = &file;
                double eta = m.Gain(file);
                for (auto [u, own] : file.Own) {
                    TPosting posting{i, own * gainOf[u], 0};
                    posting.Gain = posting.Saved * x.Theta * eta * x.Frequency;
                    unitFiles[u].push_back(posting);
                    if (selected[u]) {
                        savedSoFar[i] += posting.Saved;
                    }
                }
            }
            std::vector<double> deltaSaved(fileCount);
            std::vector<char> touched(fileCount);
            std::vector<int> touchedFiles;
            auto clearDelta = [&] {
                for (int i : touchedFiles) {
                    touched[i] = 0;
                    deltaSaved[i] = 0;
                }
                touchedFiles.clear();
            };
            const auto consumerTime = [&](const TMember& x, double saved, double load) {
                return Consumer(m.Base(*x.File), m.Floor(*x.File), x.Theta * saved, load, x.Coefficients);
            };
            // Full marginal evaluation of one root against the current state.
            struct TEval {
                bool Valid = true;
                double Work = 0, Gain = 0, Bytes = 0, Build = 0, BuildCost = 0;
                std::set<int> Fresh; // commits that would newly invalidate the PCH
            };
            auto artifactBytes = [&](const TEval& e) { return std::max(1.0, e.Bytes); };
            auto evaluate = [&](int root) {
                clearDelta();
                auto [cached, added] = closures.try_emplace(root);
                if (added) {
                    cached->second = Closure(d, {root});
                }
                const auto& closure = cached->second;
                TEval e;
                for (int u : closure) {
                    // Shared bases stay conservative. Specialized leaves may
                    // require libraries: Preflight checks the actual PCH against
                    // every final link of each assigned owner before accepting it.
                    e.Valid &= !stable || BaseMaterial(d, d.Units[u]);
                    if (!selected[u]) {
                        e.Work += d.Units[u].Parse;
                        e.Build += d.Units[u].Parse * buildOf[u];
                        e.Bytes += d.Units[u].Bytes * sizeOf[u];
                        for (int k : d.Units[u].Changes) {
                            if (!invalidating.count(k)) {
                                e.Fresh.insert(k);
                            }
                        }
                        for (const auto& posting : unitFiles[u]) {
                            e.Gain += c.Saved * posting.Gain;
                            if (!touched[posting.File]) {
                                touched[posting.File] = 1;
                                touchedFiles.push_back(posting.File);
                            }
                            deltaSaved[posting.File] += posting.Saved;
                        }
                    }
                }
                if (!e.Valid) {
                    clearDelta();
                    return e;
                }
                // TWorkCosts::Build of the artifact with this root minus the
                // build cost already carried: fresh commits reprice the whole
                // accepted body, and the parent is re-read on every rebuild.
                e.BuildCost = buildCost(buildSoFar + e.Build, invalidating.size() + e.Fresh.size());
                e.Gain -= e.BuildCost - buildCostSoFar;
                for (int i = 0; i < fileCount; ++i) {
                    const auto& x = member[i];
                    e.Gain -= c.Load * e.Work * x.Frequency;
                    const auto& a = d.Actions[d.Files[files[i]].Action];
                    int extra = 0;
                    for (int k : e.Fresh) {
                        if (!std::binary_search(a.Changes.begin(), a.Changes.end(), k)) {
                            ++extra;
                        }
                    }
                    if (extra) {
                        // Extra compiles parse this PCH, the candidate's
                        // closure included: charge the accelerated consumer
                        // time, not the bare no-PCH action time.
                        e.Gain -= (1 - d.Q) * extra / std::max(1, d.Scenarios) *
                            consumerTime(x, savedSoFar[i] + deltaSaved[i], loadSoFar + e.Work);
                    }
                }
                // The scratch buffers belong to this evaluation only: a queued
                // root can be accepted later without a fresh evaluate (its key
                // still leads the queue), so nothing may outlive this call.
                clearDelta();
                return e;
            };
            // Lazy greedy queue by benefit per resident byte. Umbrella roots
            // whose own mass sits in nested non-self-contained headers compete
            // at their closure value instead of losing to noisy wrappers, and
            // acceptance follows descending density. Keys go stale whenever an
            // acceptance changes the shared state; a stale winner is
            // re-evaluated once and reinserted unless it still leads.
            struct TKey {
                double Density = 0;
                int Root = 0;
            };
            auto lowerValue = [](const TKey& a, const TKey& b) {
                return a.Density != b.Density ? a.Density < b.Density : a.Root > b.Root;
            };
            std::priority_queue<TKey, std::vector<TKey>, decltype(lowerValue)> queue(lowerValue);
            std::vector<TEval> evaluated(d.Units.size());
            std::vector<int> epochAt(d.Units.size(), -1);
            int epoch = 0;
            for (int root : candidates) {
                auto e = evaluate(root);
                if (!e.Valid) {
                    continue;
                }
                evaluated[root] = std::move(e);
                epochAt[root] = epoch;
                queue.push({evaluated[root].Gain / artifactBytes(evaluated[root]), root});
            }
            while (!queue.empty()) {
                TKey key = queue.top();
                queue.pop();
                int root = key.Root;
                if (selected[root]) {
                    continue;
                }
                if (epochAt[root] != epoch) {
                    auto fresh = evaluate(root);
                    evaluated[root] = std::move(fresh);
                    epochAt[root] = epoch;
                    double density = evaluated[root].Gain / artifactBytes(evaluated[root]);
                    if (!queue.empty() && lowerValue({density, root}, queue.top())) {
                        queue.push({density, root});
                        continue;
                    }
                }
                const auto& e = evaluated[root];
                // Admission nets the disk price off the gain: fat low-density
                // tails stay out of the artifact entirely. This is a continue,
                // not a break: accepting another root can remove a shared
                // commit from this root's fresh penalty (supermodularity on
                // co-changed headers) and raise its density from negative to
                // positive, so rejected candidates must stay revisitable.
                if (!(e.Gain > diskPrice * artifactBytes(e))) {
                    continue;
                }
                p.Roots.push_back(root);
                if (densities) {
                    densities->emplace_back(e.Gain / artifactBytes(e), artifactBytes(e));
                }
                const auto& closure = closures.at(root);
                for (int u : closure) {
                    if (!selected[u]) {
                        selected[u] = true;
                        p.Units.push_back(u);
                        for (const auto& posting : unitFiles[u]) {
                            savedSoFar[posting.File] += posting.Saved;
                        }
                    }
                }
                invalidating.insert(e.Fresh.begin(), e.Fresh.end());
                loadSoFar += e.Work;
                buildSoFar += e.Build;
                buildCostSoFar = e.BuildCost;
                ++epoch;
            }
            Unique(p.Units);
            return p;
        }

        // Shared by selection and disk eviction. Costs are expected action-work
        // seconds: every consumer, PCH load and extra invalidation is included.
        struct TConsumerBalance {
            double ParseSaved = 0, Load = 0, ExtraCompiles = 0;
            double Net() const { return ParseSaved - Load - ExtraCompiles; }
            void Add(const TConsumerBalance& other, double sign = 1) {
                ParseSaved += sign * other.ParseSaved;
                Load += sign * other.Load;
                ExtraCompiles += sign * other.ExtraCompiles;
            }
        };

        struct TWorkCosts {
            const TData& D;
            const TLayout& L;
            const TCalibration& C;
            TConsumerModel M{D, C};
            std::map<int, std::pair<double, std::set<int>>> Features;

            const auto& Feature(int g) {
                auto [it, inserted] = Features.try_emplace(g);
                if (inserted && g >= 0) {
                    it->second.first = M.Load(L.Pchs[g].Units);
                    for (int u : L.Pchs[g].Units) {
                        it->second.second.insert(D.Units[u].Changes.begin(), D.Units[u].Changes.end());
                    }
                }
                return it->second;
            }
            double Target(int t, int g, TConsumerBalance* balance = nullptr) {
                const auto& [load, changes] = Feature(g);
                const double increment = (1 - D.Q) / std::max(1, D.Scenarios);
                double total = 0;
                for (int f : D.Targets[t].Files) {
                    const auto& file = D.Files[f];
                    const auto& a = D.Actions[file.Action];
                    const double baselineFrequency = D.Q + increment * a.Changes.size();
                    double frequency = baselineFrequency;
                    for (int k : changes) {
                        if (!std::binary_search(a.Changes.begin(), a.Changes.end(), k)) { frequency += increment; }
                    }
                    // A measured target is anchored at its observed S0 compile:
                    // only the headers another leaf adds or removes are modeled.
                    double saved = g < 0 ? 0 : M.Saved(file, L.Pchs[g].Units);
                    double withLoad = M.Time(file, saved, load);
                    total += frequency * withLoad;
                    if (balance) {
                        // Apply the same calibrated floor/slowdown rules as the selector.
                        double withoutLoad = M.Time(file, saved, 0);
                        balance->ParseSaved += baselineFrequency * (M.Time(file, 0, 0) - withoutLoad);
                        balance->Load += baselineFrequency * (withLoad - withoutLoad);
                        balance->ExtraCompiles += (frequency - baselineFrequency) * withLoad;
                    }
                }
                return total;
            }
            double Build(int g) {
                const auto& [work, changes] = Feature(g);
                return (D.Q + (1 - D.Q) * changes.size() / std::max(1, D.Scenarios)) * M.BuildTime(L, size_t(g));
            }
        };

        // Residual parse profile of one owner over units its class base does
        // not already serialize: the signal for clustering owners into deltas
        // and for finding a target's second-best delta later.
        using TProfile = std::vector<std::pair<int, double>>; // unit ascending

        TProfile ResidualProfile(const TData& d, int t, const std::vector<char>& inBase) {
            std::map<int, double> acc;
            for (int f : d.Targets[t].Files) {
                double frequency = d.Q + (1 - d.Q) *
                    double(d.Actions[d.Files[f].Action].Changes.size()) / std::max(1, d.Scenarios);
                for (auto [u, own] : d.Files[f].Own) {
                    if (!inBase[u] && own > 0) {
                        acc[u] += own * frequency;
                    }
                }
            }
            TProfile profile;
            for (const auto& [u, m] : acc) {
                if (m > 0) {
                    profile.emplace_back(u, m);
                }
            }
            return profile;
        }

        // unit -> deltas of the layout whose own content (everything their
        // closure adds beyond their parent base) contains it. Indexing the
        // full own units, not only the injected roots, matters for umbrella
        // roots: a delta rooted at actors_all.h still serializes actor.h
        // transitively, and a target spending its parse time on actor.h must
        // find that delta.
        std::map<int, std::vector<int>> UnitPostings(const TLayout& l) {
            std::map<int, std::vector<int>> postings;
            for (size_t g = size_t(l.Bases); g < l.Pchs.size(); ++g) {
                const auto& units = l.Pchs[g].Units;
                const auto& parentUnits = l.Pchs[l.Pchs[g].Parent].Units;
                for (size_t i = 0, j = 0; i < units.size(); ++i) {
                    while (j < parentUnits.size() && parentUnits[j] < units[i]) {
                        ++j;
                    }
                    if (j < parentUnits.size() && parentUnits[j] == units[i]) {
                        continue; // already serialized by the parent base
                    }
                    postings[units[i]].push_back(int(g));
                }
            }
            return postings;
        }

        // Deltas of one compiler class overlapping a residual profile, the
        // best net gain first: the parse seconds a target shares with a delta
        // less what loading the delta's own content costs on top of the base.
        // Rank on the raw overlap alone and a bloated delta of another
        // subsystem that pulls in a common header transitively crowds out the
        // compact one that was built for the target. Candidates to absorb a
        // target when its own leaf is evicted or rejected, instead of falling
        // all the way to the base or NONE. Never crosses compiler classes: a
        // PCH of another class cannot serve these files.
        std::vector<int> SiblingDeltas(const TLayout& l, const std::map<int, std::vector<int>>& postings,
                                       const TConsumerModel& m, int cls, int t, const TProfile& profile, int exclude, int limit) {
            std::map<int, double> overlap;
            for (const auto& [u, mass] : profile) {
                auto it = postings.find(u);
                if (it == postings.end()) {
                    continue;
                }
                for (int g : it->second) {
                    if (g != exclude && l.Pchs[g].Class == cls) {
                        overlap[g] += mass;
                    }
                }
            }
            const auto c = m.Coefficients(cls);
            // Saved parse seconds per second of load under the class coefficients.
            const double loadPerSaved = c.Saved > 0 ? std::max(0.0, c.Load) / c.Saved : 0.02;
            // The profile sums the weighted parse seconds of every file of the
            // target, and every one of them pays the load of a delta it takes:
            // the same scale for both, whatever the target's size or churn.
            double frequency = 0;
            for (int f : m.D.Targets[t].Files) {
                const auto& a = m.D.Actions[m.D.Files[f].Action];
                frequency += m.D.Q + (1 - m.D.Q) * a.Changes.size() / std::max(1, m.D.Scenarios);
            }
            std::vector<std::pair<double, int>> ranked;
            for (const auto& [g, mass] : overlap) {
                if (mass <= 0) {
                    continue;
                }
                double own = 0;
                m.ForOwn(l.Pchs[g], &l.Pchs[l.Pchs[g].Parent].Units, [&](int u) { own += m.D.Units[u].Parse; });
                ranked.emplace_back(mass - loadPerSaved * own * frequency, g);
            }
            std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first > b.first : a.second < b.second;
            });
            std::vector<int> result;
            for (const auto& entry : ranked) {
                if (int(result.size()) >= limit) {
                    break;
                }
                result.push_back(entry.second);
            }
            return result;
        }

        // Initial clustering of owner residual profiles for one class:
        // bucketed L2-normalized signatures plus deterministic farthest-point
        // seeding on a subsample. The cluster count is only an upper bound of
        // the width scale — the EM rounds in Select move members across
        // deltas freely, letting dense shared subsystems and rare specialized
        // ones take clusters of very different sizes.
        std::vector<int> ClusterProfiles(const std::vector<TProfile>& profiles, size_t k) {
            constexpr size_t BUCKETS = 64;
            const size_t n = profiles.size();
            std::vector<int> cluster(n, -1);
            if (!n || !k) {
                return cluster;
            }
            std::vector<std::array<double, BUCKETS>> signatures(n);
            std::vector<double> norm(n);
            for (size_t i = 0; i < n; ++i) {
                auto& signature = signatures[i];
                double square = 0;
                for (const auto& [u, m] : profiles[i]) {
                    signature[(uint64_t(u) * 0x9E3779B97F4A7C15ULL) >> 58] += m;
                    square += m * m;
                }
                norm[i] = std::sqrt(square);
                if (norm[i] > 0) {
                    for (auto& value : signature) {
                        value /= norm[i];
                    }
                }
            }
            const size_t wanted = std::max<size_t>(256, 2 * std::min(k, n));
            // Even steps over the non-empty profiles only: a fixed index stride
            // would undersample whenever empty profiles (owners fully covered
            // by their base) interleave.
            std::vector<size_t> nonEmpty;
            for (size_t i = 0; i < n; ++i) {
                if (norm[i] > 0) {
                    nonEmpty.push_back(i);
                }
            }
            const size_t take = std::min(wanted, nonEmpty.size());
            std::vector<size_t> sample;
            for (size_t j = 0; j < take; ++j) {
                sample.push_back(nonEmpty[j * nonEmpty.size() / take]);
            }
            if (sample.empty()) {
                return cluster;
            }
            auto similarity = [&](size_t a, size_t b) {
                const auto& x = signatures[a];
                const auto& y = signatures[b];
                double dot = 0;
                for (size_t h = 0; h < BUCKETS; ++h) {
                    dot += x[h] * y[h];
                }
                return dot;
            };
            std::vector<size_t> seeds;
            size_t first = 0;
            for (size_t j = 1; j < sample.size(); ++j) {
                if (norm[sample[j]] > norm[sample[first]]) {
                    first = j;
                }
            }
            seeds.push_back(first);
            std::vector<double> nearest(sample.size(), -2);
            for (size_t j = 0; j < sample.size(); ++j) {
                nearest[j] = similarity(sample[j], sample[first]);
            }
            std::vector<char> seeded(sample.size());
            seeded[first] = 1;
            while (seeds.size() < std::min(k, n) && seeds.size() < sample.size()) {
                size_t next = sample.size();
                for (size_t j = 0; j < sample.size(); ++j) {
                    if (!seeded[j] && (next == sample.size() || nearest[j] < nearest[next])) {
                        next = j;
                    }
                }
                if (next == sample.size()) {
                    break;
                }
                seeded[next] = 1;
                seeds.push_back(next);
                for (size_t j = 0; j < sample.size(); ++j) {
                    if (!seeded[j]) {
                        nearest[j] = std::max(nearest[j], similarity(sample[j], sample[next]));
                    }
                }
            }
            for (size_t i = 0; i < n; ++i) {
                if (norm[i] <= 0) {
                    continue;
                }
                double best = -2;
                size_t bestSeed = 0;
                for (size_t s = 0; s < seeds.size(); ++s) {
                    double dot = similarity(i, sample[seeds[s]]);
                    if (dot > best) {
                        best = dot;
                        bestSeed = s;
                    }
                }
                cluster[i] = int(bestSeed);
            }
            return cluster;
        }

        // Price a delta against each assigned owner's better fallback. Only
        // surviving children contribute to the subsequent family-vs-NONE test.
        // `onlyDown`: an owner leaves a delta for its parent or for NONE alone.
        // After the compatibility checks no other leaf is known to compile it.
        // `compatible`: whether a fallback compiles an owner; a parent that
        // does not is no fallback, and the delta is priced against NONE alone.
        void CheckPayback(const TData& d, TLayout& l, const TCalibration& c,
                          const std::string& source, int pass, const std::vector<int>& ids,
                          std::vector<std::string>* reasons, std::ostream* report, bool onlyDown = false,
                          const std::function<bool(int, int)>& compatible = {}) {
            TWorkCosts costs{d, l, c};
            struct TGroup {
                std::vector<int> Owners; // members that keep this family alive
                TConsumerBalance VsNone;
                double ChildBuild = 0;
            };
            std::vector<TGroup> groups(l.Pchs.size());
            // Ranked open alternatives per delta owner: the base plus sibling
            // deltas whose roots overlap the owner's residual profile. A
            // near-duplicate delta then competes on marginal value only, and a
            // dropped owner falls to its best surviving sibling instead of all
            // the way to the base or NONE.
            struct TAlternative {
                TConsumerBalance Balance;
                int Group = -1;
            };
            std::vector<std::vector<TAlternative>> alternatives(d.Targets.size());
            std::map<std::pair<int, int>, TConsumerBalance> balanceOf;
            auto benefit = [&](int t, int g) {
                auto [it, added] = balanceOf.try_emplace(std::make_pair(t, g));
                if (added) {
                    costs.Target(t, g, &it->second);
                }
                return it->second;
            };
            const auto postings = UnitPostings(l);
            std::map<int, std::vector<char>> baseMask;
            auto maskOf = [&](int base) -> const std::vector<char>& {
                auto [it, added] = baseMask.emplace(base, d.Units.size());
                if (added) {
                    for (int u : l.Pchs[base].Units) {
                        it->second[u] = 1;
                    }
                }
                return it->second;
            };
            for (size_t t = 0; t < l.Assignment.size(); ++t) {
                int g = l.Assignment[t];
                if (g < 0) {
                    continue;
                }
                groups[g].Owners.push_back(int(t));
                groups[g].VsNone.Add(benefit(int(t), g));
                if (g < l.Bases) {
                    continue;
                }
                const int parent = l.Pchs[g].Parent;
                std::vector<TAlternative> ranked;
                auto consider = [&](int candidate) {
                    TConsumerBalance balance = benefit(int(t), candidate);
                    if (balance.Net() > 0) {
                        ranked.push_back({balance, candidate});
                    }
                };
                consider(parent);
                if (onlyDown) {
                    alternatives[t] = std::move(ranked);
                    continue;
                }
                const auto profile = ResidualProfile(d, int(t), maskOf(parent));
                for (int sibling : SiblingDeltas(l, postings, costs.M, d.Targets[t].Class, t, profile, g, SiblingLimit)) {
                    if (sibling >= l.Bases) {
                        consider(sibling);
                    }
                }
                std::sort(ranked.begin(), ranked.end(), [](const TAlternative& a, const TAlternative& b) {
                    return a.Balance.Net() != b.Balance.Net() ? a.Balance.Net() > b.Balance.Net() : a.Group < b.Group;
                });
                alternatives[t] = std::move(ranked);
            }
            // A delta is a live fallback destination while it still has
            // owners and has not been rejected. Deltas are decided weakest
            // first (ascending preliminary net over their base), so a weak
            // duplicate merges into its stronger sibling rather than the other
            // way around; the sibling is then rechecked on the merged owner
            // set, and falls back itself only if the union still loses.
            std::vector<char> open(l.Pchs.size(), false);
            std::vector<char> kept(l.Pchs.size(), false);
            for (size_t t = 0; t < l.Assignment.size(); ++t) {
                int g = l.Assignment[t];
                if (g >= l.Bases) {
                    open[g] = true;
                }
            }
            auto bestOpen = [&](int t, int exclude) -> const TAlternative* {
                for (const auto& alternative : alternatives[t]) {
                    if (alternative.Group != exclude && (alternative.Group < l.Bases || open[alternative.Group])) {
                        return &alternative;
                    }
                }
                return nullptr;
            };
            auto admit = [&](int g, const TConsumerBalance& net, double build, double mandatory, size_t owners) {
                double value = net.Net() - (build - mandatory);
                if (report) {
                    *report << source << '\t' << pass << '\t' << (g < l.Bases ? "family" : "delta") << '\t'
                            << ids[g] << '\t' << owners << '\t' << net.ParseSaved << '\t'
                            << net.Load << '\t' << net.ExtraCompiles << '\t' << build << '\t' << mandatory
                            << '\t' << value << '\t' << (value > 0 ? "keep" : "drop-assignments") << '\n';
                }
                return value > 0;
            };
            auto assign = [&](int t, int g, const char* reason) {
                l.Assignment[t] = g;
                if (reasons) { reasons->at(t) = reason; }
            };
            // Preliminary marginal over the base, per initially owned delta.
            std::vector<std::pair<double, int>> byStrength;
            for (int g = l.Bases; g < int(l.Pchs.size()); ++g) {
                if (!open[g]) {
                    continue; // no owners: never a destination, decided by nobody
                }
                const int parent = l.Pchs[g].Parent;
                TConsumerBalance marginal;
                const double build = costs.Build(g);
                for (int t : groups[g].Owners) {
                    marginal.Add(benefit(t, g));
                    TConsumerBalance base = benefit(t, parent);
                    if (base.Net() > 0) {
                        marginal.Add(base, -1);
                    }
                }
                byStrength.emplace_back(marginal.Net() - build, g);
            }
            std::sort(byStrength.begin(), byStrength.end(), [](const auto& a, const auto& b) {
                return a.first != b.first ? a.first < b.first : a.second < b.second;
            });
            for (auto [preliminary, g] : byStrength) {
                (void)preliminary;
                std::vector<int> owners;
                for (size_t t = 0; t < l.Assignment.size(); ++t) {
                    if (l.Assignment[t] == g) {
                        owners.push_back(int(t));
                    }
                }
                if (owners.empty()) {
                    continue;
                }
                const int parent = l.Pchs[g].Parent;
                auto& family = groups[parent];
                const double build = costs.Build(g);
                TConsumerBalance vsNone, marginal;
                auto priceOwners = [&] {
                    vsNone = {};
                    TConsumerBalance fallback;
                    for (int t : owners) {
                        vsNone.Add(benefit(t, g));
                        if (const TAlternative* alternative = bestOpen(t, g)) {
                            fallback.Add(alternative->Balance);
                        }
                    }
                    marginal = vsNone;
                    marginal.Add(fallback, -1);
                };
                priceOwners();
                if (compatible && marginal.Net() - build <= 0) {
                    // Probe fallbacks lazily: a delta that already pays over
                    // its parent pays all the more against NONE, and never
                    // needs a compiler check on its parent.
                    bool changed = false;
                    for (int t : owners) {
                        while (const TAlternative* alternative = bestOpen(t, g)) {
                            if (compatible(t, alternative->Group)) { break; }
                            const int rejected = alternative->Group;
                            auto& list = alternatives[t];
                            list.erase(std::remove_if(list.begin(), list.end(),
                                [&](const TAlternative& a) { return a.Group == rejected; }), list.end());
                            changed = true;
                        }
                    }
                    if (changed) { priceOwners(); }
                }
                if (admit(g, marginal, build, 0, owners.size())) {
                    kept[g] = true;
                    family.ChildBuild += build;
                    family.VsNone.Add(vsNone);
                    family.Owners.insert(family.Owners.end(), owners.begin(), owners.end());
                } else {
                    open[g] = false; // no longer a fallback destination
                    for (int t : owners) {
                        const TAlternative* alternative = bestOpen(t, g);
                        const int destination = alternative ? alternative->Group : -1;
                        assign(t, destination, destination < 0 ? "delta-payback-none" :
                            destination < l.Bases ? "delta-payback-parent" : "delta-payback-sibling");
                        if (destination < 0) {
                            continue;
                        }
                        if (destination < l.Bases) {
                            family.VsNone.Add(alternative->Balance);
                            family.Owners.push_back(t);
                        } else if (kept[destination]) {
                            // An already-admitted sibling: fold the member into
                            // its family now. A pending sibling picks the member
                            // up at its own admission decision instead.
                            auto& host = groups[l.Pchs[destination].Parent];
                            host.VsNone.Add(alternative->Balance);
                            host.Owners.push_back(t);
                        }
                    }
                }
            }
            // A family nobody is left on has nothing to decide: its frozen
            // base is not built for the final plan and frees its bytes.
            for (int g = 0; g < l.Bases; ++g) {
                auto& family = groups[g];
                if (family.Owners.empty()) { continue; }
                // A frozen base is frozen in content only. Its build is priced
                // like any other, by what the measurement says it costs: a
                // family that does not cover it against NONE is a loss in the
                // final build, whatever the prior that admitted it in S0.
                if (!admit(g, family.VsNone, costs.Build(g) + family.ChildBuild, 0, family.Owners.size())) {
                    for (int t : family.Owners) { assign(t, -1, "family-no-payback"); }
                }
            }
        }

        double Median(std::vector<double> values) {
            if (values.empty()) { return 0; }
            auto mid = values.begin() + values.size() / 2;
            std::nth_element(values.begin(), mid, values.end());
            if (values.size() % 2 == 0) {
                auto lo = std::max_element(values.begin(), mid);
                return (*mid + *lo) / 2;
            }
            return *mid;
        }

        double Spread(const std::vector<double>& values) {
            if (values.size() < 2) { return 0; }
            double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
            double square = 0;
            for (double v : values) { square += (v - mean) * (v - mean); }
            return std::sqrt(square / (values.size() - 1));
        }

        // Robust class scale: the median of per-observation ratios.
        double Ratio(const std::vector<double>& values, double fallback = 1, double low = 1e-4) {
            if (values.empty()) { return fallback; }
            return std::clamp(Median(values), low, 20.0);
        }

        // Weighted least squares for one to three scaled regressors, pivoted
        // elimination with a rank threshold. No ridge term concealing
        // non-identifiability of parse and load. `matrix` is n x (n+1).
        bool Solve(std::vector<double>& a, int n, double tolerance, std::vector<double>& result) {
            const int w = n + 1;
            std::vector<double> scale(n);
            for (int i = 0; i < n; ++i) {
                scale[i] = std::sqrt(std::max(a[size_t(i) * w + i], 0.0));
                if (scale[i] < 1e-12) { return false; }
            }
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j < n; ++j) { a[size_t(i) * w + j] /= scale[i] * scale[j]; }
                a[size_t(i) * w + n] /= scale[i];
            }
            for (int i = 0; i < n; ++i) {
                int pivot = i;
                for (int j = i + 1; j < n; ++j) {
                    if (std::abs(a[size_t(j) * w + i]) > std::abs(a[size_t(pivot) * w + i])) { pivot = j; }
                }
                if (!std::isfinite(a[size_t(pivot) * w + i]) || std::abs(a[size_t(pivot) * w + i]) < tolerance) { return false; }
                for (int j = 0; j < w; ++j) { std::swap(a[size_t(i) * w + j], a[size_t(pivot) * w + j]); }
                double div = a[size_t(i) * w + i];
                for (int j = i; j < w; ++j) { a[size_t(i) * w + j] /= div; }
                for (int row = 0; row < n; ++row) {
                    if (row == i) { continue; }
                    double mul = a[size_t(row) * w + i];
                    for (int j = i; j < w; ++j) { a[size_t(row) * w + j] -= mul * a[size_t(i) * w + j]; }
                }
            }
            result.resize(n);
            for (int i = 0; i < n; ++i) {
                result[i] = a[size_t(i) * w + n] / scale[i];
                if (!std::isfinite(result[i])) { return false; }
            }
            return true;
        }

        // A group observation over n units has K[g][g] = sum of squared shares
        // >= 1/n: this ridge is far below it for any real closure, so
        // consistent observations are matched exactly and only the
        // underdetermined directions take the minimum-norm (equal-share)
        // solution. Contradictory rows share the difference.
        constexpr double UnitRidge = 1e-9;

        // Unit multipliers (prior 1) fitting group observations
        // sum_u rows[g][u] * x[u] = targets[g], least squares with a weak ridge
        // toward the prior. Rows are shares, so one observation over many
        // units moves all of them by the same share, and units shared by
        // several groups carry the weighted combination of their evidence.
        std::map<int, double> Ridge(const std::vector<std::map<int, double>>& rows, const std::vector<double>& targets,
                                    double gamma) {
            const size_t n = rows.size();
            if (!n) { return {}; }
            std::map<int, std::vector<std::pair<size_t, double>>> postings;
            for (size_t g = 0; g < n; ++g) {
                for (const auto& [u, share] : rows[g]) { postings[u].emplace_back(g, share); }
            }
            std::vector<double> a(n * (n + 1), 0.0);
            for (size_t i = 0; i < n; ++i) {
                double sum = 0;
                for (const auto& [u, share] : rows[i]) { sum += share; }
                a[i * (n + 1) + n] = gamma * (sum - targets[i]);
            }
            for (const auto& [u, list] : postings) {
                for (size_t i = 0; i < list.size(); ++i) {
                    for (size_t j = 0; j < list.size(); ++j) {
                        a[list[i].first * (n + 1) + list[j].first] += list[i].second * list[j].second;
                    }
                }
            }
            for (size_t i = 0; i < n; ++i) { a[i * (n + 1) + i] += gamma; }
            std::vector<double> residual;
            if (!Solve(a, int(n), 1e-12, residual)) { return {}; }
            std::map<int, double> x;
            for (const auto& [u, list] : postings) {
                double value = 1;
                for (const auto& [g, share] : list) { value -= share * residual[g] / gamma; }
                x[u] = value;
            }
            return x;
        }
    } // namespace

    namespace {
        struct TTopology {
            std::vector<std::vector<int>> Users;
            std::vector<int> Order;

            explicit TTopology(const std::vector<TAction>& actions)
                : Order(Topology(actions, Users))
            {}
        };
    }

    // The schedule Ninja runs, and nothing else: the worker count, the
    // dependency graph and the named pools of the build. A real build has no
    // RAM admission and no disk bandwidth budget, so the model has neither.
    // They used to slow the measurement under reservations of their own and
    // to price the PCH bytes a consumer reads a second time, beyond the load
    // the calibration already fits to its time.
    static double MakespanPrepared(const std::vector<TAction>& actions, const TResources& r,
                                   const std::vector<char>& dirty, const TTopology& topology) {
        if (r.Workers < 1 || (!dirty.empty() && dirty.size() != actions.size())) {
            Fail("scheduler: invalid worker count or dirty mask");
        }
        const auto& users = topology.Users;
        const auto& order = topology.Order;
        std::vector<double> time(actions.size()), critical(actions.size());
        std::vector<int> pending(actions.size());
        for (int i : order) {
            const auto& a = actions[i];
            bool active = dirty.empty() || dirty[i];
            time[i] = active && !a.Phony ? a.Time : 0;
            if (!std::isfinite(time[i]) || time[i] < 0) {
                Fail("scheduler: non-finite or negative cost");
            }
            pending[i] = int(a.Dependencies.size());
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            for (int u : users[*it]) {
                critical[*it] = std::max(critical[*it], critical[u]);
            }
            critical[*it] += time[*it];
        }
        auto priority = [&](int a, int b) {
            return critical[a] != critical[b] ? critical[a] > critical[b] : a < b;
        };
        std::set<int, decltype(priority)> ready(priority);
        for (int i : order) {
            if (!pending[i]) {
                ready.insert(i);
            }
        }
        using TEvent = std::pair<double, int>;
        std::priority_queue<TEvent, std::vector<TEvent>, std::greater<TEvent>> running;
        std::map<std::string, int> poolUse;
        double now = 0;
        size_t finished = 0;
        auto complete = [&](int i) {
            ++finished;
            for (int u : users[i]) {
                if (!--pending[u]) {
                    ready.insert(u);
                }
            }
        };
        while (finished < actions.size()) {
            for (auto it = ready.begin(); it != ready.end();) {
                int i = *it;
                if (time[i] == 0) {
                    ready.erase(it);
                    complete(i);
                    it = ready.begin();
                    continue;
                }
                if (running.size() >= size_t(r.Workers)) {
                    break;
                }
                const auto& pool = r.Pools.find(actions[i].Pool);
                if (!actions[i].Pool.empty() && pool == r.Pools.end()) {
                    Fail("scheduler: unknown pool " + actions[i].Pool);
                }
                if (pool != r.Pools.end() && poolUse[actions[i].Pool] >= pool->second) {
                    ++it;
                    continue;
                }
                it = ready.erase(it);
                ++poolUse[actions[i].Pool];
                running.emplace(now + time[i], i);
            }
            if (running.empty()) {
                return finished == actions.size() ? now : INF;
            }
            now = running.top().first;
            do {
                int i = running.top().second;
                running.pop();
                --poolUse[actions[i].Pool];
                complete(i);
            } while (!running.empty() && running.top().first <= now);
        }
        return now;
    }

    static std::vector<char> InvalidatePrepared(const std::vector<TAction>& actions, int scenario,
                                              const TTopology& topology) {
        std::vector<char> dirty(actions.size());
        for (int i : topology.Order) {
            dirty[i] = std::binary_search(actions[i].Changes.begin(), actions[i].Changes.end(), scenario);
            for (int dep : actions[i].DirtyDependencies) {
                if (dep < 0 || size_t(dep) >= actions.size()) {
                    Fail("invalidation: invalid dependency");
                }
                dirty[i] |= dirty[dep];
            }
        }
        return dirty;
    }

    double Makespan(const std::vector<TAction>& actions, const TResources& resources, const std::vector<char>& dirty) {
        return MakespanPrepared(actions, resources, dirty, TTopology(actions));
    }

    std::vector<char> Invalidate(const std::vector<TAction>& actions, int scenario) {
        return InvalidatePrepared(actions, scenario, TTopology(actions));
    }

    void GroupTargets(TData& d, const std::vector<std::string>& owners) {
        if (owners.size() != d.Files.size()) { Fail("target ownership size mismatch"); }
        std::map<std::string, std::vector<int>> members;
        for (size_t f = 0; f < owners.size(); ++f) {
            if (owners[f].empty()) { Fail("C++ compilation has no GN PCH owner"); }
            members[owners[f]].push_back(int(f));
        }
        d.Targets.clear();
        for (auto& [label, files] : members) {
            int cls = d.Files[files.front()].Class;
            for (int f : files) {
                d.Files[f].Target = int(d.Targets.size());
                if (d.Files[f].Class != cls) { cls = -1; }
            }
            d.Targets.push_back({label, cls, std::move(files)});
        }
        ValidateTargets(d);
    }

    void ValidateTargets(const TData& d) {
        std::vector<char> seen(d.Files.size()), actions(d.Actions.size());
        std::set<std::string> labels;
        for (size_t t = 0; t < d.Targets.size(); ++t) {
            const auto& target = d.Targets[t];
            if (target.Label.empty() || !labels.insert(target.Label).second || target.Files.empty()) {
                Fail("invalid or duplicate GN PCH owner");
            }
            int cls = -2;
            for (int f : target.Files) {
                if (f < 0 || size_t(f) >= d.Files.size() || seen[f] || d.Files[f].Target != int(t)) {
                    Fail("target membership must partition C++ compilations");
                }
                seen[f] = true;
                const auto& file = d.Files[f];
                if (file.Action < 0 || size_t(file.Action) >= d.Actions.size() || actions[file.Action] ||
                    file.Class < 0 || file.Class >= d.Classes) {
                    Fail("invalid target compilation action/class");
                }
                actions[file.Action] = true;
                if (cls == -2) { cls = file.Class; }
                else if (cls != file.Class) { cls = -1; }
            }
            if (target.Class != cls) { Fail("target compiler class mismatch"); }
        }
        if (std::find(seen.begin(), seen.end(), false) != seen.end()) {
            Fail("C++ compilation missing from GN target membership; run predict again");
        }
    }

    // Drop every PCH no assignment reaches (a delta, or a base through a kept
    // delta), frozen or not, and renumber the rest. Returns old index -> new
    // index, -1 for the dropped ones.
    std::vector<int> CompactLayout(TLayout& layout) {
        std::vector<char> keep(layout.Pchs.size());
        for (int g : layout.Assignment) {
            for (; g >= 0 && !keep[g]; g = layout.Pchs[g].Parent) { keep[g] = true; }
        }
        std::vector<int> remap(keep.size(), -1);
        std::vector<TPch> pchs;
        int bases = 0, frozen = 0;
        for (size_t g = 0; g < keep.size(); ++g) {
            if (!keep[g]) { continue; }
            auto pch = std::move(layout.Pchs[g]);
            remap[g] = int(pchs.size());
            if (pch.Parent >= 0) { pch.Parent = remap[pch.Parent]; }
            pchs.push_back(std::move(pch));
            bases += int(g) < layout.Bases;
            frozen += int(g) < layout.Frozen;
        }
        for (int& g : layout.Assignment) { if (g >= 0) { g = remap[g]; } }
        layout.Pchs = std::move(pchs);
        layout.Bases = bases;
        layout.Frozen = std::min(frozen, bases);
        return remap;
    }

    namespace {
    constexpr unsigned ProbeBudgetFactor = 2;

    // Same eviction path at the forecast and measured boundaries. A family
    // competes at its total marginal loss/bytes, including its base just once.
    // An evicted owner falls to its best still-open alternative — the base or
    // a surviving sibling delta whose roots overlap its profile — so
    // near-duplicate deltas merge into one another instead of pushing unique
    // specialized groups out of the budget. Losses are recomputed lazily as
    // evictions close alternatives and move owners around. A base lives by
    // its consumers: one without an assigned target or a surviving delta is
    // not part of the final plan and reserves no bytes, so it cannot evict a
    // useful delta of another compiler class. The first `frozen` bases are
    // the initial plan's: never evicted, their consumers' departure frees them.
    void FitDisk(const TData& d, TLayout& l, const TCalibration& c,
                 const std::vector<uint64_t>& bytes, double budget, int frozen,
                 const std::function<bool(int, int)>& compatible, std::ostream* report) {
        if (bytes.size() != l.Pchs.size()) { Fail("missing PCH artifact sizes"); }
        frozen = std::clamp(frozen, 0, l.Bases);
        std::vector<std::vector<int>> owners(bytes.size());
        std::vector<std::vector<int>> children(l.Bases);
        std::vector<char> used(bytes.size());
        std::vector<char> open(bytes.size(), false);
        for (size_t t = 0; t < l.Assignment.size(); ++t) {
            int g = l.Assignment[t];
            if (g < 0) { continue; }
            owners.at(g).push_back(int(t));
            used[g] = true;
            open[g] = true;
            if (g >= l.Bases) { used.at(l.Pchs[g].Parent) = true; }
        }
        for (int b = 0; b < l.Bases; ++b) { open[b] = true; }
        uint64_t total = 0, bases = 0, frozenBytes = 0;
        std::vector<uint64_t> freed = bytes;
        for (size_t g = 0; g < bytes.size(); ++g) {
            if (!used[g]) { continue; }
            if (!bytes[g]) { Fail("no size for retained PCH " + std::to_string(g)); }
            total += bytes[g];
            if (int(g) < l.Bases) {
                bases += bytes[g];
                frozenBytes += int(g) < frozen ? bytes[g] : 0;
            } else {
                children.at(l.Pchs[g].Parent).push_back(int(g));
                freed[l.Pchs[g].Parent] += bytes[g];
            }
        }
        if (frozenBytes > budget) {
            Fail(Format("frozen bases alone require %.0f bytes; disk budget %.0f bytes", double(frozenBytes), budget));
        }
        if (report) {
            *report << "# measured preflight PCH bytes; IDs before compaction; weighted action seconds, not wall time\n"
                    << "# candidate_bytes=" << total << " base_bytes=" << bases << " budget_bytes=" << uint64_t(budget)
                    << "\nkind\tgroup\tbytes\tloss_work_seconds\tto_fallback\tto_none\n";
        }
        TWorkCosts costs{d, l, c};
        // Ranked fallback destinations per delta owner: the base plus sibling
        // deltas sharing its heavy residual headers, by gain versus NONE.
        struct TFallback {
            double Gain = 0;
            int Group = -1;
        };
        std::vector<std::vector<TFallback>> alternatives(l.Assignment.size());
        std::vector<char> ranked(l.Assignment.size(), false);
        const auto postings = UnitPostings(l);
        std::vector<std::vector<char>> baseMask(l.Bases);
        for (int b = 0; b < l.Bases; ++b) {
            baseMask[b].assign(d.Units.size(), 0);
            for (int u : l.Pchs[b].Units) { baseMask[b][u] = 1; }
        }
        auto fallbacksOf = [&](int t) -> std::vector<TFallback>& {
            if (ranked[t]) { return alternatives[t]; }
            ranked[t] = true;
            const int g = l.Assignment[t];
            if (g < l.Bases) { return alternatives[t]; } // family eviction drops straight to NONE
            const int base = l.Pchs[g].Parent;
            std::vector<TFallback> list;
            double none = costs.Target(t, -1);
            auto consider = [&](int candidate) {
                double gain = none - costs.Target(t, candidate);
                if (gain > 0) { list.push_back({gain, candidate}); }
            };
            consider(base);
            const auto profile = ResidualProfile(d, t, baseMask[base]);
            for (int sibling : SiblingDeltas(l, postings, costs.M, l.Pchs[g].Class, t, profile, g, SiblingLimit)) {
                if (sibling >= l.Bases) { consider(sibling); }
            }
            std::sort(list.begin(), list.end(), [](const TFallback& a, const TFallback& b) {
                return a.Gain != b.Gain ? a.Gain > b.Gain : a.Group < b.Group;
            });
            alternatives[t] = std::move(list);
            return alternatives[t];
        };
        int evictions = 0;
        std::vector<double> loss(bytes.size());
        std::vector<int> lossAt(bytes.size(), -1);
        std::vector<char> verified(bytes.size(), !compatible);
        auto bestOpen = [&](int t, int exclude) -> const TFallback* {
            for (const auto& fallback : fallbacksOf(t)) {
                if (fallback.Group != exclude && open[fallback.Group]) {
                    return &fallback;
                }
            }
            return nullptr;
        };
        auto computeLoss = [&](int g) {
            double value = -costs.Build(g);
            if (l.Pchs[g].Parent < 0) {
                // The family drops as one: the base plus every open child,
                // all of them to NONE.
                for (int t : owners[g]) {
                    value += costs.Target(t, -1) - costs.Target(t, g);
                }
                for (int h : children[g]) {
                    if (!used[h]) { continue; }
                    value -= costs.Build(h);
                    for (int t : owners[h]) {
                        value += costs.Target(t, -1) - costs.Target(t, h);
                    }
                }
            } else {
                for (int t : owners[g]) {
                    value += costs.Target(t, -1) - costs.Target(t, g);
                    if (const TFallback* fallback = bestOpen(t, g)) {
                        value -= fallback->Gain;
                    }
                }
            }
            loss[g] = value;
            lossAt[g] = evictions;
        };
        using TEntry = std::pair<double, int>;
        std::set<TEntry> order;
        auto entry = [&](int g) -> TEntry { return {loss[g] / std::max<uint64_t>(freed[g], 1), g}; };
        for (int g = 0; g < int(bytes.size()); ++g) {
            if (!used[g] || g < frozen) { continue; }
            computeLoss(g);
            order.insert(entry(g));
        }
        while (!order.empty()) {
            if (total <= budget && order.begin()->first > 0) { break; }
            auto [value, g] = *order.begin();
            order.erase(order.begin());
            (void)value;
            const int parent = l.Pchs[g].Parent;
            if (lossAt[g] != evictions) {
                computeLoss(g);
                order.insert(entry(g));
                continue;
            }
            if (parent >= 0 && !verified[g]) {
                // Probe pending fallbacks once: a failed compatibility check
                // removes that destination and only raises this loss.
                verified[g] = true;
                for (int t : owners[g]) {
                    auto& list = fallbacksOf(t);
                    for (size_t k = 0; k < list.size();) {
                        if (open[list[k].Group] && !compatible(t, list[k].Group)) {
                            list.erase(list.begin() + k);
                        } else {
                            ++k;
                        }
                    }
                }
                computeLoss(g);
                order.insert(entry(g));
                continue;
            }
            size_t fallbackCount = 0, noneCount = 0;
            ++evictions;
            if (parent < 0) {
                auto drop = [&](int h) {
                    for (int t : owners[h]) { l.Assignment[t] = -1; ++noneCount; }
                    used[h] = false;
                    open[h] = false;
                };
                for (int h : children[g]) {
                    if (used[h]) { order.erase(entry(h)); drop(h); }
                }
                drop(g);
            } else {
                open[g] = false;
                used[g] = false;
                for (int t : owners[g]) {
                    const TFallback* fallback = bestOpen(t, g);
                    const int destination = fallback ? fallback->Group : -1;
                    l.Assignment[t] = destination;
                    if (destination >= 0) {
                        ++fallbackCount;
                        owners[destination].push_back(t);
                    } else {
                        ++noneCount;
                    }
                }
                owners[g].clear();
                freed[parent] -= bytes[g];
                // The last consumer of a frozen base has left: nothing builds
                // it for the final plan, so its bytes return to the budget at
                // once instead of waiting for an eviction that cannot come.
                if (parent < frozen && owners[parent].empty() &&
                    std::none_of(children[parent].begin(), children[parent].end(), [&](int h) { return used[h]; })) {
                    used[parent] = false;
                    total -= bytes[parent];
                    if (report) { *report << "release\t" << parent << '\t' << bytes[parent] << "\t0\t0\t0\n"; }
                }
            }
            total -= freed[g];
            if (report) {
                *report << (parent < 0 ? "family" : "delta") << '\t' << g << '\t' << freed[g] << '\t'
                        << loss[g] << '\t' << fallbackCount << '\t' << noneCount << '\n';
            }
        }
        if (total > budget) { Fail("cannot fit retained PCHs in the disk budget"); }
    }
    } // namespace

    std::string FitMeasuredDisk(const TData& d, TLayout& l, const TCalibration& c,
                                const std::vector<uint64_t>& bytes, int frozenBases,
                                const std::function<bool(int, int)>& compatible) {
        std::ostringstream report;
        FitDisk(d, l, c, bytes, d.Resources.Disk, frozenBases, compatible, &report);
        return report.str();
    }

    std::string FinalPayback(const TData& d, TLayout& l, const TCalibration& c,
                             const std::function<bool(int, int)>& compatible) {
        std::vector<int> ids(l.Pchs.size());
        std::iota(ids.begin(), ids.end(), 0);
        std::ostringstream report;
        report.precision(17);
        CheckPayback(d, l, c, "final", 1, ids, nullptr, &report, true, compatible);
        return report.str();
    }

    TLayout ScoredLayout(const TData& d, const TLayout& layout, const TCalibration& c,
                         size_t analysisJobs, double* objective, const std::vector<uint64_t>* measured) {
        TLayout scored = layout;
        scored.Frozen = scored.Bases;
        std::vector<int> ids(scored.Pchs.size());
        std::iota(ids.begin(), ids.end(), 0);
        // Only down: after the checks no sibling leaf is known to compile an owner.
        CheckPayback(d, scored, c, "scored", 1, ids, nullptr, nullptr, true);
        // The artifacts a plan built were measured: their sizes are known,
        // and only what was never built is estimated.
        const bool known = measured && measured->size() == scored.Pchs.size();
        std::vector<uint64_t> sizes;
        TConsumerModel model{d, c};
        for (size_t g = 0; g < scored.Pchs.size(); ++g) {
            sizes.push_back(known ? (*measured)[g] : uint64_t(std::ceil(model.ArtifactBytes(scored, g))));
        }
        FitDisk(d, scored, c, sizes, d.Resources.Disk, scored.Frozen, {}, nullptr);
        const auto remap = CompactLayout(scored);
        std::vector<uint64_t> kept(known ? scored.Pchs.size() : 0);
        if (known) {
            for (size_t g = 0; g < remap.size(); ++g) {
                if (remap[g] >= 0) { kept[size_t(remap[g])] = sizes[g]; }
            }
        }
        const auto prediction = ScoreCandidate(d, scored, c, analysisJobs, -1, known ? &kept : nullptr);
        if (objective) { *objective = prediction.Objective; }
        return scored;
    }

    TPrediction Predict(const TData& d, const TLayout& l, const TCalibration& calibration, size_t analysisJobs,
                        const std::vector<uint64_t>& measuredBytes) {
        ValidateTargets(d);
        if (!measuredBytes.empty() && measuredBytes.size() != l.Pchs.size()) { Fail("incomplete measured PCH sizes"); }
        if (l.Assignment.size() != d.Targets.size()) {
            Fail("prediction: assignment size mismatch");
        }
        TPrediction p;
        p.Actions = d.Actions;
        for (size_t i = 0; i < d.Actions.size(); ++i) {
            if (i < calibration.ActionTime.size()) {
                p.Actions[i].Time = calibration.ActionTime[i];
                p.Actions[i].Memory = calibration.ActionMemory[i];
            }
        }
        TConsumerModel m{d, calibration};
        double disk = 0;
        int offset = int(d.Actions.size());
        for (size_t g = 0; g < l.Pchs.size(); ++g) {
            const auto& h = l.Pchs[g];
            if (h.Parent >= int(g)) {
                Fail("prediction: PCH parent must precede delta");
            }
            auto c = Coeff(calibration, h.Class);
            TAction a;
            a.Output = "pch/" + std::to_string(g) + ".pch";
            // The build S0 timed for exactly this artifact, else its units'
            // build weights plus the prior re-read of the parent. Consumer
            // load fits do not identify the compiler's PCH-to-PCH load cost.
            const TMeasuredPch* measured = m.Measured(l, g);
            a.Time = m.BuildTime(l, g);
            double bytes = measuredBytes.empty() ? m.ArtifactBytes(l, g) : measuredBytes[g];
            p.PchBytes.push_back(bytes);
            a.Io = bytes;
            if (h.Parent >= 0) {
                a.Dependencies.push_back(offset + h.Parent);
                a.Io += p.PchBytes[h.Parent];
            }
            a.Memory = measured ? measured->Memory : (d.ActionMemory + a.Io) * c.Memory;
            for (int u : h.Units) {
                const auto& unit = d.Units[u];
                a.Dependencies.insert(a.Dependencies.end(), unit.Producers.begin(), unit.Producers.end());
                a.Changes.insert(a.Changes.end(), unit.Changes.begin(), unit.Changes.end());
            }
            Unique(a.Dependencies);
            Unique(a.Changes);
            a.DirtyDependencies = a.Dependencies;
            p.Actions.push_back(std::move(a));
            disk += bytes;
        }
        p.Saved.resize(d.Files.size());
        p.Load.resize(d.Files.size());
        for (size_t f = 0; f < d.Files.size(); ++f) {
            const auto& file = d.Files[f];
            auto& a = p.Actions[file.Action];
            auto c = Coeff(calibration, file.Class);
            int g = l.Assignment[file.Target];
            if (g < -1) { Fail("prediction: invalid PCH assignment"); }
            if (g >= 0) {
                const auto& h = l.Pchs.at(g);
                if (!d.Targets[file.Target].PchEligible) {
                    Fail("prediction: target has no GN global PCH hook");
                }
                if (h.Class != file.Class) {
                    Fail("prediction: incompatible PCH assignment");
                }
                p.Saved[f] = m.Saved(file, h.Units);
                p.Load[f] = m.Load(h.Units);
                a.Dependencies.push_back(offset + g);
                a.DirtyDependencies.push_back(offset + g);
                a.Io = p.PchBytes[g] + (h.Parent >= 0 ? p.PchBytes[h.Parent] : 0);
                a.Memory = (d.ActionMemory + a.Io) * c.ConsumerMemory;
            }
            a.Time = m.Time(file, p.Saved[f], p.Load[f]);
        }
        if (!measuredBytes.empty() && disk > d.Resources.Disk) {
            p.Objective = INF;
            return p;
        }
        // Scheduling and invalidation share the same immutable graph across
        // cold and all historical scenarios; only per-scenario state differs.
        const TTopology topology(p.Actions);
        p.Cold = MakespanPrepared(p.Actions, d.Resources, {}, topology);
        std::vector<double> incremental(std::max(0, d.Scenarios));
        ParallelFor(incremental.size(), analysisJobs, [&](size_t k) {
            incremental[k] = MakespanPrepared(p.Actions, d.Resources, InvalidatePrepared(p.Actions, int(k), topology), topology);
        });
        // Fixed scenario order preserves floating-point results and selection
        // ties regardless of worker count or completion order.
        p.Incremental = std::accumulate(incremental.begin(), incremental.end(), 0.0);
        p.Incremental /= std::max(1, d.Scenarios);
        p.Feasible = std::isfinite(p.Cold) && std::isfinite(p.Incremental);
        p.Objective = p.Feasible ? d.Q * p.Cold + (1 - d.Q) * p.Incremental : INF;
        return p;
    }

    TPrediction ScoreCandidate(const TData& d, TLayout& layout, const TCalibration& c, size_t jobs, int width,
                               const std::vector<uint64_t>* measured) {
        // Keep all artifacts (including frozen bases), roots, class IDs and
        // parents fixed. Only assignments on consumer -> PCH edges may move.
        const std::vector<uint64_t> none;
        const auto& bytes = measured ? *measured : none;
        for (;;) {
            try {
                return Predict(d, layout, c, jobs, bytes);
            } catch (const TDependencyCycle& cycle) {
                Log(Format("selection width=%d: %s", width, cycle.what()));
                const int offset = int(d.Actions.size());
                std::map<int, int> edges; // consumer action -> assigned PCH on this cycle
                for (size_t i = 1; i < cycle.Nodes.size(); ++i) {
                    if (cycle.Nodes[i-1] < offset && cycle.Nodes[i] >= offset) {
                        edges.emplace(cycle.Nodes[i-1], cycle.Nodes[i] - offset);
                    }
                }
                std::set<int> targets;
                for (const auto& f : d.Files) {
                    auto it = edges.find(f.Action);
                    if (it != edges.end() && it->second == layout.Assignment[f.Target]) {
                        targets.insert(f.Target);
                    }
                }
                if (targets.empty()) { throw; } // Not repairable by changing a consumer assignment.

                int bestTarget = -1, bestGroup = -1, provisional = -1;
                double cheapestLoss = INF;
                TPrediction best;
                TConsumerModel model{d, c};
                // Expected consumer work of one owner under one leaf (NONE: -1).
                auto cost = [&](int t, int g) {
                    const auto& units = g >= 0 ? layout.Pchs[g].Units : std::vector<int>();
                    const double load = g >= 0 ? model.Load(units) : 0;
                    double total = 0;
                    for (int i : d.Targets[t].Files) {
                        const auto& f = d.Files[i];
                        const auto& a = d.Actions[f.Action];
                        const double freq = d.Q + (1 - d.Q) * a.Changes.size() / std::max(1, d.Scenarios);
                        total += freq * model.Time(f, g >= 0 ? model.Saved(f, units) : 0, load);
                    }
                    return total;
                };
                for (int t : targets) {
                    const int old = layout.Assignment[t];
                    for (int g = layout.Pchs[old].Parent;; g = layout.Pchs[g].Parent) {
                        auto trial = layout;
                        trial.Assignment[t] = g;
                        try {
                            auto p = Predict(d, trial, c, jobs, bytes);
                            Detail(Format("  %s PCH %d -> %d: objective %.3f s",
                                          d.Targets[t].Label.c_str(), old, g, p.Objective));
                            if (bestTarget < 0 || p.Objective < best.Objective) {
                                bestTarget = t;
                                bestGroup = g;
                                best = std::move(p);
                            }
                        } catch (const TDependencyCycle&) {
                            Detail(Format("  %s PCH %d -> %d: still cyclic", d.Targets[t].Label.c_str(), old, g));
                        }
                        if (g < 0) { break; }
                    }
                    // Several cycles may preclude scoring any single move: the
                    // other members of a generator keep each of its targets on
                    // the cycle. Move one witnessed owner to the parent of its
                    // leaf, NONE off a base: the least estimated loss among
                    // them, and it keeps what the parent saves. A cycle through
                    // the parent itself comes back and is cut the same way one
                    // level down. Each step moves one assignment down its
                    // chain, so it terminates.
                    const double loss = cost(t, layout.Pchs[old].Parent) - cost(t, old);
                    if (provisional < 0 || loss < cheapestLoss) { provisional = t; cheapestLoss = loss; }
                }
                const int t = bestTarget >= 0 ? bestTarget : provisional;
                const int g = bestTarget >= 0 ? bestGroup : layout.Pchs[layout.Assignment[t]].Parent;
                Log(Format("selection width=%d: cycle repair %s PCH %d -> %d (%s)", width,
                           d.Targets[t].Label.c_str(), layout.Assignment[t], g,
                           bestTarget >= 0 ? "best acyclic local move" : "multiple cycles; least estimated loss on witnessed cycle, to the parent"));
                layout.Assignment[t] = g;
                if (bestTarget >= 0) { return best; }
            }
        }
    }

    TLayout Select(const TData& d, const TCalibration& c, const TLayout* frozen,
                   size_t analysisJobs, TSelectionDiagnostics* diagnostics) {
        if (diagnostics) { *diagnostics = {}; }
        ValidateTargets(d);
        std::map<int, std::vector<int>> closures;
        std::vector<std::vector<int>> classes(d.Classes);
        for (size_t t = 0; t < d.Targets.size(); ++t) {
            if (d.Targets[t].PchEligible && d.Targets[t].Class >= 0) {
                classes[d.Targets[t].Class].push_back(int(t));
            }
        }
        auto filesOf = [&](const std::vector<int>& targets) {
            std::vector<int> files;
            for (int t : targets) {
                const auto& members = d.Targets[t].Files;
                files.insert(files.end(), members.begin(), members.end());
            }
            return files;
        };
        TLayout bases;
        bases.Assignment.assign(d.Targets.size(), -1);
        std::vector<TDensity> baseDensities;
        // The classes the initial plan left without a base. The correction
        // builds one for each of them as the initial plan would have, priced
        // by what the measurement taught: a class evicted by an estimated
        // size 60% short of the real one, or by a price set with the
        // inflated densities of others, is not a class PCH does not pay in.
        std::vector<int> openClasses;
        if (frozen) {
            bases.Bases = frozen->Bases;
            bases.Frozen = frozen->Bases;
            bases.Pchs.assign(frozen->Pchs.begin(), frozen->Pchs.begin() + frozen->Bases);
            std::vector<char> hasBase(d.Classes, 0);
            for (const auto& pch : bases.Pchs) { hasBase[pch.Class] = 1; }
            for (int cls = 0; cls < d.Classes; ++cls) {
                if (!hasBase[cls] && !classes[cls].empty()) { openClasses.push_back(cls); }
            }
        } else {
            for (int cls = 0; cls < d.Classes; ++cls) {
                auto p = Pick(d, c, cls, filesOf(classes[cls]), nullptr, true, closures, 0.0, &baseDensities);
                if (!p.Roots.empty()) {
                    bases.Pchs.push_back(std::move(p));
                }
            }
            bases.Bases = int(bases.Pchs.size());
        }
        TLayout best = frozen ? *frozen : TLayout();
        if (!frozen) {
            best.Assignment.assign(d.Targets.size(), -1);
        } else {
            best.Frozen = best.Bases;
        }
        // Recheck if cycle repair demotes an owner: the proof for the old
        // consumer set no longer holds. Keep report IDs in the candidate's namespace.
        auto scoreAssigned = [&](TLayout& l, const std::string& source, int width, std::vector<std::string>& reasons, unsigned factor) {
            std::vector<int> ids(l.Pchs.size());
            std::iota(ids.begin(), ids.end(), 0);
            std::ostringstream payback;
            payback.precision(17);
            for (int pass = 1;; ++pass) {
                CheckPayback(d, l, c, source, pass, ids, diagnostics ? &reasons : nullptr,
                             diagnostics && factor == 1 ? &payback : nullptr);
                std::vector<uint64_t> estimates;
                TConsumerModel model{d, c};
                for (size_t g = 0; g < l.Pchs.size(); ++g) {
                    estimates.push_back(uint64_t(std::ceil(model.ArtifactBytes(l, g))));
                }
                const auto beforeFit = l.Assignment;
                FitDisk(d, l, c, estimates, factor * d.Resources.Disk, l.Frozen, {}, nullptr);
                if (diagnostics) {
                    for (size_t t = 0; t < reasons.size(); ++t) {
                        if (l.Assignment[t] != beforeFit[t]) {
                            reasons[t] = l.Assignment[t] < 0 ? "disk-budget" : "disk-fallback";
                        }
                    }
                }
                // Unused groups leave at once, frozen bases included: their
                // measured records are found by content, so their IDs are free
                // to move, and a family nobody is left on must not be scored
                // as built in the makespan or the bytes of any candidate.
                const auto remap = CompactLayout(l);
                std::vector<int> kept(l.Pchs.size());
                for (size_t g = 0; g < remap.size(); ++g) { if (remap[g] >= 0) { kept[remap[g]] = ids[g]; } }
                ids = std::move(kept);
                const auto before = l.Assignment;
                auto prediction = ScoreCandidate(d, l, c, analysisJobs, width);
                if (before == l.Assignment) {
                    if (diagnostics) { diagnostics->Payback += payback.str(); }
                    return prediction;
                }
                if (diagnostics) {
                    for (size_t t = 0; t < before.size(); ++t) {
                        if (before[t] != l.Assignment[t]) {
                            reasons[t] = l.Assignment[t] < 0 ? "cycle-no-pch" : "cycle-fallback";
                        }
                    }
                }
            }
        };
        std::vector<std::string> seedReasons(d.Targets.size());
        const auto seedAssignment = best.Assignment;
        const auto seedPrediction = scoreAssigned(best, "seed", -1, seedReasons, 1);
        const TLayout seedPlan = best;
        double score = seedPrediction.Objective;
        const std::vector<int> widths = {0, 32, 128, 512};
        struct TCandidate {
            TLayout Layout;
            std::vector<std::string> Reasons;
            std::vector<char> Roles; // TLayout::Controls of this candidate
            std::vector<int> BeforeDisk;
            std::string Choices;
            std::string Pchs;
        };
        struct TCandidateSpec {
            int Width = 0;
            double Lambda = 0;
        };
        auto specLabel = [&](const TCandidateSpec& spec) {
            std::ostringstream out;
            out.precision(6);
            out << spec.Width;
            if (spec.Lambda > 0) {
                out << "@lambda=" << spec.Lambda;
            }
            return out.str();
        };
        auto describePchs = [&](const TCandidateSpec& spec, const TLayout& layout) {
            const auto label = specLabel(spec);
            std::ostringstream out;
            for (size_t g = 0; g < layout.Pchs.size(); ++g) {
                const auto& p = layout.Pchs[g];
                out << label << '\t' << g << '\t' << p.Class << '\t' << p.Parent << '\t' << p.Units.size() << '\t';
                for (size_t r = 0; r < p.Roots.size(); ++r) {
                    if (r) { out << ';'; }
                    out << d.Units[p.Roots[r]].Include;
                }
                out << '\n';
            }
            return out.str();
        };
        // One (width, lambda) candidate: cluster each class's owners by their
        // residual parse profile, pick a delta per cluster, then refine with
        // expectation/maximization rounds on the true work-cost objective. A
        // target may end on any delta of its class, not only the partition its
        // label happens to sort into.
        auto construct = [&](const TCandidateSpec& spec, TCandidate& observed,
                             std::map<int, std::vector<int>>& candidateClosures,
                             std::vector<TDensity>* densities) {
            TLayout candidate;
            if (frozen) {
                // The frozen bases stay as they are. Only the classes without
                // one are picked, at this price as every base of the initial
                // plan is.
                candidate = bases;
                for (int cls : openClasses) {
                    auto p = Pick(d, c, cls, filesOf(classes[cls]), nullptr, true, candidateClosures, spec.Lambda, densities);
                    if (!p.Roots.empty()) {
                        candidate.Pchs.push_back(std::move(p));
                    }
                }
                candidate.Bases = int(candidate.Pchs.size());
            } else if (spec.Lambda == 0) {
                candidate = bases;
            } else {
                // Disk-priced variant: rebuild the bases under the same price.
                candidate.Assignment.assign(d.Targets.size(), -1);
                for (int cls = 0; cls < d.Classes; ++cls) {
                    auto p = Pick(d, c, cls, filesOf(classes[cls]), nullptr, true, candidateClosures, spec.Lambda, densities);
                    if (!p.Roots.empty()) {
                        candidate.Pchs.push_back(std::move(p));
                    }
                }
                candidate.Bases = int(candidate.Pchs.size());
            }
            std::ostringstream choicesReport;
            choicesReport.precision(17);
            const auto label = specLabel(spec);
            observed.Roles.assign(d.Targets.size(), 0);
            if (diagnostics) {
                for (const auto& target : d.Targets) {
                    observed.Reasons.push_back(!target.PchEligible ? "no-global-pch-hook" :
                        target.Class < 0 ? "mixed-compiler-classes" : "no-candidate");
                }
            }
            for (int cls = 0; cls < d.Classes; ++cls) {
                int parent = -1;
                for (int g = 0; g < candidate.Bases; ++g) {
                    if (candidate.Pchs[g].Class == cls) {
                        parent = g;
                    }
                }
                if (parent < 0) { continue; }
                auto targets = classes[cls];
                std::stable_sort(targets.begin(), targets.end(), [&](int a, int b) {
                    return d.Targets[a].Label < d.Targets[b].Label;
                });
                std::vector<char> inBase(d.Units.size());
                for (int u : candidate.Pchs[parent].Units) { inBase[u] = true; }
                std::vector<TProfile> profiles(targets.size());
                for (size_t i = 0; i < targets.size(); ++i) {
                    profiles[i] = ResidualProfile(d, targets[i], inBase);
                }
                const size_t clusters = spec.Width
                    ? std::min(targets.size(), (targets.size() + size_t(spec.Width) - 1) / size_t(spec.Width))
                    : 0;
                std::vector<int> clusterOf = clusters
                    ? ClusterProfiles(profiles, clusters)
                    : std::vector<int>(targets.size(), -1);
                const size_t classStart = candidate.Pchs.size();
                // Three-point control of the initial plan. A few owners stay
                // without PCH (the class baseline: compiler speed, the ratio
                // of the traces to it and its spread) and as many owners that
                // would take a delta stay on the base (what a delta adds over
                // it). Both are spread evenly over a label order, which says
                // nothing of an owner's headers, and the sample measures them
                // first. The counts grow with sqrt(n): a large class gives up
                // a few percent of its coverage, not a tenth of it.
                std::vector<char> role(targets.size(), 0);
                if (!frozen) {
                    const size_t n = targets.size();
                    // A bare control pays nothing towards the payback of its
                    // class base in the plan that is measured. A class of
                    // fewer than four owners therefore has no bare control: a
                    // lone owner would be measured without any PCH at all, and
                    // one of two or three owners may well hold up their common
                    // base by itself. Such a class is too small to identify
                    // saved and load apart anyway, and its owners are still
                    // measured under their PCH and anchored.
                    const size_t controls = n < 4 ? 0 : std::clamp((size_t(std::sqrt(double(n))) + 1) / 2, size_t(1), size_t(6));
                    // Both kinds of control are spread evenly over a label
                    // order, which says nothing of an owner's headers. An
                    // owner carrying more than a third of the class, by the
                    // parse seconds a PCH can take off it, keeps its PCH: it
                    // holds up the base. Every other owner is pool, light and
                    // heavy alike: the lighter half alone is a biased sample,
                    // and a baseline read off the lightest owners does not
                    // describe the class.
                    std::vector<size_t> pool;
                    if (controls) {
                        std::vector<double> work(targets.size(), 0);
                        double total = 0;
                        for (size_t i = 0; i < targets.size(); ++i) {
                            for (int f : d.Targets[targets[i]].Files) {
                                const auto& file = d.Files[f];
                                const double frequency = d.Q + (1 - d.Q) * double(d.Actions[file.Action].Changes.size()) / std::max(1, d.Scenarios);
                                for (const auto& own : file.Own) { work[i] += own.second * frequency; }
                            }
                            total += work[i];
                        }
                        for (size_t i = 0; i < targets.size(); ++i) {
                            if (3 * work[i] <= total) { pool.push_back(i); }
                        }
                    }
                    for (size_t k = 0; !pool.empty() && k < controls; ++k) {
                        role[pool[k * pool.size() / controls]] = 1;
                        if (n >= 8) {
                            const size_t at = (2 * k + 1) * pool.size() / (2 * controls);
                            if (!role[pool[at]]) { role[pool[at]] = 2; }
                        }
                    }
                }
                // A target the measurement saw save nothing with any PCH (a
                // multiplier of zero) never takes one. Its files in the bucket
                // of a cluster would only add their load and extra compiles
                // to the delta the others have to pay for: a big one makes a
                // small cluster's delta fail where its real members would
                // have paid for it.
                auto takesPch = [&](int t) { return size_t(t) >= c.TargetGain.size() || c.TargetGain[t] > 0; };
                // The delta a target designs with, its cluster's, and after the
                // rebuild the one it was on: always among its choices, whatever
                // its overlap with the others ranks.
                std::vector<int> ownDelta(targets.size(), -1);
                // Expectation: every target takes the cheapest of NONE, the
                // base and the deltas whose roots overlap its profile. Only
                // the final round records diagnostics: an earlier round's
                // delta IDs die in the next maximization, and the reports must
                // match selection-candidates.tsv.
                auto expect = [&](bool record) {
                    TWorkCosts costs{d, candidate, c};
                    const auto postings = UnitPostings(candidate);
                    for (size_t i = 0; i < targets.size(); ++i) {
                        int t = targets[i];
                        std::vector<int> choices{parent};
                        for (int g : SiblingDeltas(candidate, postings, costs.M, cls, t, profiles[i], -1, SiblingLimit)) {
                            if (std::find(choices.begin(), choices.end(), g) == choices.end()) { choices.push_back(g); }
                        }
                        if (ownDelta[i] >= int(classStart) && std::find(choices.begin(), choices.end(), ownDelta[i]) == choices.end()) {
                            choices.push_back(ownDelta[i]);
                        }
                        double cost = costs.Target(t, -1);
                        const double withoutPch = cost;
                        const bool allowed = role[i] != 1;
                        int chosen = -1;
                        for (int g : choices) {
                            double value = costs.Target(t, g);
                            if (diagnostics && record) {
                                choicesReport << label << '\t' << d.Targets[t].Label << '\t' << g << '\t'
                                    << withoutPch << '\t' << value << '\t' << (value < cost && allowed) << '\n';
                            }
                            if (value < cost && allowed) {
                                cost = value;
                                chosen = g;
                            }
                        }
                        // A base control gives up its delta only: on the base it
                        // measures what the delta adds for an owner that would
                        // take one.
                        const bool demoted = role[i] == 2 && chosen >= int(classStart);
                        candidate.Assignment[t] = demoted ? parent : chosen;
                        // The role of the final round alone: one demoted in the
                        // interim round may keep its delta after the re-pick.
                        if (record && (role[i] == 1 || demoted)) { observed.Roles[t] = role[i]; }
                        if (diagnostics && record) {
                            observed.Reasons[t] = role[i] == 1 ? "calibration-control" : demoted ? "calibration-base-control" :
                                candidate.Assignment[t] >= 0 ? "selected" : choices.empty() ? "no-candidate" : "no-estimated-benefit";
                        }
                    }
                };
                // Maximization: rebuild each delta strictly from the files
                // currently on it; deltas with no members disappear. The first
                // round builds from the clusters, the second from the deltas
                // the expectation assigned. The densities of the final round
                // alone describe the candidate: the roots of an interim delta
                // that did not survive are no artifact of it.
                auto maximize = [&](bool fromClusters) {
                    std::vector<std::vector<int>> buckets;
                    std::vector<int> origin; // the cluster, or the delta, a bucket rebuilds
                    if (fromClusters) {
                        buckets.assign(clusters, {});
                        origin.resize(clusters);
                        std::iota(origin.begin(), origin.end(), 0);
                        for (size_t i = 0; i < targets.size(); ++i) {
                            if (clusterOf[i] >= 0 && takesPch(targets[i])) {
                                const auto& members = d.Targets[targets[i]].Files;
                                auto& files = buckets[clusterOf[i]];
                                files.insert(files.end(), members.begin(), members.end());
                            }
                        }
                    } else {
                        std::map<int, std::vector<int>> byDelta;
                        for (int t : targets) {
                            int g = candidate.Assignment[t];
                            if (g >= int(classStart)) {
                                const auto& members = d.Targets[t].Files;
                                auto& files = byDelta[g];
                                files.insert(files.end(), members.begin(), members.end());
                            }
                        }
                        for (auto& [g, files] : byDelta) {
                            buckets.push_back(std::move(files));
                            origin.push_back(g);
                        }
                    }
                    candidate.Pchs.resize(classStart);
                    std::map<int, int> rebuilt;
                    for (size_t b = 0; b < buckets.size(); ++b) {
                        if (buckets[b].empty()) { continue; }
                        // The interim deltas of a two-round class are no artifact
                        // of the candidate: their densities would only crowd
                        // the grid. The vector is one per candidate, shared by
                        // every class of it, so it is never cleared here.
                        auto p = Pick(d, c, cls, buckets[b], &candidate.Pchs[parent], false, candidateClosures, spec.Lambda,
                                      fromClusters && clusters ? nullptr : densities);
                        if (!p.Roots.empty()) {
                            p.Parent = parent;
                            rebuilt[origin[b]] = int(candidate.Pchs.size());
                            candidate.Pchs.push_back(std::move(p));
                        }
                    }
                    for (size_t i = 0; i < targets.size(); ++i) {
                        const int from = fromClusters ? clusterOf[i] : candidate.Assignment[targets[i]];
                        const auto found = rebuilt.find(from);
                        ownDelta[i] = found == rebuilt.end() ? -1 : found->second;
                    }
                };
                maximize(true);
                expect(!clusters); // without a second round this one is final
                if (clusters) {
                    maximize(false);
                    expect(true);
                }
            }
            if (diagnostics) {
                observed.BeforeDisk = candidate.Assignment;
                observed.Choices = choicesReport.str();
                observed.Pchs = describePchs(spec, candidate);
            }
            observed.Layout = std::move(candidate);
        };
        std::vector<TCandidateSpec> specs;
        for (int width : widths) {
            specs.push_back({width, 0.0});
        }
        std::vector<TCandidate> candidates(specs.size());
        int selectedSpec = -1;
        TConsumerModel model{d, c};
        auto estimatedBytes = [&](const TLayout& l) {
            double bytes = 0;
            for (size_t g = 0; g < l.Pchs.size(); ++g) { bytes += model.ArtifactBytes(l, g); }
            return bytes;
        };
        // Reference rows of the width report: the build without any PCH, and
        // what the plan the correction starts from is worth on this model.
        // Whether a corrected candidate is an improvement is a question about
        // these two numbers, not about the initial forecast of another model.
        TLayout without;
        without.Assignment.assign(d.Targets.size(), -1);
        const auto nonePrediction = Predict(d, without, c, analysisJobs);
        auto row = [&](const std::string& label, const TLayout& l, const TPrediction& p) {
            const double bytes = std::accumulate(p.PchBytes.begin(), p.PchBytes.end(), 0.0);
            return label + Format("\t%d\t%zu\t%.17g\t%.17g\t%.17g\t%.17g\t%d\n", l.Bases, l.Pchs.size(), bytes,
                                  p.Cold, p.Incremental, p.Objective, int(p.Feasible));
        };
        auto scoreOne = [&](size_t i) {
            const auto label = specLabel(specs[i]);
            auto candidate = candidates[i].Layout; // retain raw winner for the reserve pass
            auto reasons = candidates[i].Reasons;
            TPrediction prediction;
            try {
                prediction = scoreAssigned(candidate, label, specs[i].Width, reasons, 1);
            } catch (const TError& error) {
                Fail(Format("selection %s: %s", label.c_str(), error.what()));
            }
            double value = prediction.Objective;
            Detail(Format("selection %s: %d bases, %zu PCHs, %.3f GB, objective %.3f s%s",
                label.c_str(), candidate.Bases, candidate.Pchs.size(),
                std::accumulate(prediction.PchBytes.begin(), prediction.PchBytes.end(), 0.0) / 1e9,
                value, prediction.Feasible ? "" : " (infeasible)"));
            if (diagnostics) {
                diagnostics->Widths += row(label, candidate, prediction);
                diagnostics->Choices += candidates[i].Choices;
                diagnostics->Candidates += candidates[i].Pchs;
            }
            if (value < score) {
                score = value;
                selectedSpec = int(i);
                best = std::move(candidate);
                seedReasons = std::move(reasons);
            }
        };
        {
            // Wave one: free-disk candidates at every width. The collected
            // accept densities feed the disk-price grid of the second wave.
            std::vector<std::vector<TDensity>> densities(specs.size());
            ParallelFor(specs.size(), analysisJobs, [&](size_t i) {
                // A parallel candidate owns its mutable closure cache. With one
                // worker keep reusing the cache, as the serial selector did.
                auto localClosures = analysisJobs > 1 ? closures : decltype(closures){};
                auto& candidateClosures = analysisJobs > 1 ? localClosures : closures;
                construct(specs[i], candidates[i], candidateClosures, &densities[i]);
            });
            std::vector<char> pressured(specs.size());
            for (size_t i = 0; i < specs.size(); ++i) {
                scoreOne(i);
                pressured[i] = estimatedBytes(candidates[i].Layout) > d.Resources.Disk;
            }
            // Wave two: disk-priced variants only for the widths whose layout
            // exceeds the budget. Every lambda grid is personal to its
            // pressured candidate: base-root densities (collected over the
            // whole class) and small-cluster delta densities live on very
            // different scales, so pooling all widths would leave a wide
            // candidate untrimmed or burn a narrow one to the ground. Each
            // price shrinks every artifact of that candidate to its dense
            // prefix instead of evicting whole groups later.
            //
            // A cut is where the estimated bytes of the candidate's roots,
            // densest first, stop fitting a share of the budget: the price
            // that keeps exactly that prefix. The whole budget is the cut of
            // a knapsack; 60% leaves a margin to the prior that estimated
            // the bytes; 125% leaves the rest to the measured fit. By count
            // of roots instead, a quantile lands wherever a crowd of small
            // dense headers ends and drops every large one, most of the
            // budget unused.
            auto personalGrid = [&](size_t i) {
                std::vector<TDensity> pool = densities[i];
                if (!frozen) {
                    // The bases are rebuilt under the same price in wave two.
                    pool.insert(pool.end(), baseDensities.begin(), baseDensities.end());
                }
                // The frozen bases hold their bytes of the budget already: a
                // share of what is left prices the roots, not of the whole.
                // Counted from zero, the grid let as many delta bytes again
                // past the price as the bases occupy.
                const double held = frozen ? estimatedBytes(bases) : 0.0;
                std::sort(pool.begin(), pool.end(), [](const TDensity& a, const TDensity& b) {
                    return a.first != b.first ? a.first > b.first : a.second < b.second;
                });
                std::vector<double> grid;
                for (double share : {0.6, 1.0, 1.25}) {
                    double bytes = held, lambda = 0;
                    bool cut = false;
                    for (const auto& [density, size] : pool) {
                        if (bytes + size > share * d.Resources.Disk) {
                            cut = true;
                            break;
                        }
                        bytes += size;
                        lambda = density;
                    }
                    // Nothing to cut: the first root alone is over the share,
                    // or the whole candidate is under it.
                    if (!cut || lambda <= 0) { continue; }
                    // A price snapped to the cut root's own density must not
                    // reject that root by the strict admission inequality.
                    lambda *= 1 - 1e-9;
                    // Shares grow, so each cut is at a lower price than the one before.
                    if (grid.empty() || lambda < grid.back()) { grid.push_back(lambda); }
                }
                return grid;
            };
            const size_t waveOne = specs.size();
            for (size_t i = 0; i < waveOne; ++i) {
                if (!pressured[i]) {
                    continue;
                }
                for (double lambda : personalGrid(i)) {
                    specs.push_back({specs[i].Width, lambda});
                }
            }
            if (specs.size() > candidates.size()) {
                candidates.resize(specs.size());
                const size_t first = waveOne;
                ParallelFor(specs.size() - first, analysisJobs, [&](size_t j) {
                    auto localClosures = analysisJobs > 1 ? closures : decltype(closures){};
                    auto& candidateClosures = analysisJobs > 1 ? localClosures : closures;
                    construct(specs[first + j], candidates[first + j], candidateClosures, nullptr);
                });
                for (size_t i = first; i < specs.size(); ++i) {
                    scoreOne(i);
                }
            }
        }
        const int selectedWidth = selectedSpec >= 0 ? specs[selectedSpec].Width : -1;
        if (!std::isfinite(score)) {
            Fail("no layout can be scheduled under the worker and pool limits");
        }
        const auto primaryGroups = best.Pchs.size();
        const double scoredBytes = estimatedBytes(best);
        if (selectedSpec >= 0 || frozen) {
            auto reserve = selectedSpec >= 0 ? candidates[selectedSpec].Layout : *frozen;
            if (selectedSpec < 0) { reserve.Frozen = reserve.Bases; }
            auto reasons = selectedSpec >= 0 ? candidates[selectedSpec].Reasons : std::vector<std::string>(d.Targets.size());
            if (scoreAssigned(reserve, selectedSpec >= 0 ? specLabel(specs[selectedSpec]) : "frozen",
                              selectedWidth, reasons, ProbeBudgetFactor).Feasible) {
                best = std::move(reserve);
                seedReasons = std::move(reasons);
            }
        }
        // Every scored layout was compacted as it was scored, so the reported
        // groups and bytes are those of the returned plan: a frozen family
        // without an assigned target or surviving delta left it at once.
        if (selectedSpec >= 0) { best.Controls = candidates[selectedSpec].Roles; }
        if (diagnostics) {
            std::ostringstream targets;
            const std::string selectedLabel = selectedSpec >= 0 ? specLabel(specs[selectedSpec]) : std::string("-1");
            targets << "# selected_width=" << selectedWidth << " (-1: retained seed layout)\n"
                    << "target\treason\tbefore_disk_leaf\tselected_leaf\n";
            for (size_t t = 0; t < d.Targets.size(); ++t) {
                std::string reason;
                int beforeDisk = -1;
                if (selectedSpec >= 0) {
                    reason = seedReasons[t];
                    beforeDisk = candidates[selectedSpec].BeforeDisk[t];
                } else {
                    beforeDisk = seedAssignment[t];
                    reason = !seedReasons[t].empty() ? seedReasons[t] : !d.Targets[t].PchEligible ? "no-global-pch-hook" : d.Targets[t].Class < 0 ? "mixed-compiler-classes" :
                        frozen ? "retained-initial-plan" : "no-layout-improvement";
                }
                targets << d.Targets[t].Label << '\t' << reason << '\t' << beforeDisk << '\t' << best.Assignment[t] << '\n';
            }
            diagnostics->Targets = targets.str();
            diagnostics->Choices = "# Weighted action-time estimates, not CPU or build wall time. IDs are local to each candidate before disk pruning.\n"
                "# improved_at_visit is an intermediate greedy decision, not final selection. Unlisted headers/partitions were not scored here.\n"
                "width\ttarget\tcandidate_leaf\twithout_pch_work_seconds\twith_pch_work_seconds\timproved_at_visit\n" + diagnostics->Choices;
            diagnostics->Widths = "# selected_width=" + std::to_string(selectedWidth) + " selected_candidate=" + selectedLabel +
                Format("\n# scored_budget_bytes=%.0f scored_pchs=%zu scored_bytes=%.0f probe_budget_bytes=%.0f probe_pchs=%zu probe_bytes=%.0f",
                       d.Resources.Disk, primaryGroups, scoredBytes, ProbeBudgetFactor * d.Resources.Disk, best.Pchs.size(), estimatedBytes(best)) +
                "\n# Reference rows: none = the build without PCH; seed = the plan the correction starts from, on this model and after payback and the estimated fit.\n"
                "width_lambda\tbases\tpchs\tpch_bytes\tcold_seconds\tincremental_seconds\tobjective_seconds\tfeasible\n" +
                row("none", without, nonePrediction) + (frozen ? row("seed", seedPlan, seedPrediction) : std::string()) + diagnostics->Widths;
            if (frozen) { diagnostics->Candidates += describePchs({-1, 0.0}, *frozen); }
            diagnostics->Payback = "# selected_width=" + std::to_string(selectedWidth) + " selected_candidate=" + selectedLabel +
                "; predicted weighted action-work seconds; IDs match selection-candidates.tsv; net excludes mandatory builds\n"
                "width\tpass\tkind\tcandidate_leaf\tassigned_targets\tparse_saved_s\tload_s\textra_compile_s\tbuild_s\tmandatory_build_s\tnet_s\tdecision\n" +
                diagnostics->Payback;
            diagnostics->Candidates = "# IDs are local to each candidate before disk pruning; width -1 describes a retained seed.\n"
                "width\tcandidate_leaf\tclass\tparent\tclosure_units\troot_headers\n" + diagnostics->Candidates;
        }
        Log(Format("selection: %s; %zu PCHs at %.3f / %.3f GB for scoring; %zu at %.3f / %.3f GB for probes (estimated)",
                   selectedSpec < 0 ? "seed retained" : ("candidate " + specLabel(specs[selectedSpec])).c_str(),
                   primaryGroups, scoredBytes / 1e9, d.Resources.Disk / 1e9,
                   best.Pchs.size(), estimatedBytes(best) / 1e9, ProbeBudgetFactor * d.Resources.Disk / 1e9));
        return best;
    }

    // What the first measurement teaches the second selection.
    //
    // Per target: the sampled objects are a PPS draw of the target's strata
    // (SampleFiles), so the Horvitz-Thompson estimate of the target's whole
    // compile under its S0 leaf is unbiased. Every member is anchored at that
    // point: the model predicts another leaf for it by the headers that leaf
    // adds or removes, not from its trace. Baseline offsets of a target (old
    // distclang estimates, another machine's load) cancel identically.
    //
    // Per class: the controls (no PCH) give the baseline ratio and residual;
    // saved/load come from a weighted two-parameter fit of the measured PCH
    // targets on the centered data (no count column to collinearize with
    // load), optionally with the template-instantiation feature. Leave-one-
    // group-out is a diagnostic; the working fit uses every group.
    //
    // Per unit: each artifact S0 built is one observation over its own units,
    // decomposed by a weak ridge into PCH bytes, build seconds and the saved
    // time its consumers really saw. A target's residual is split by
    // empirical Bayes into what its leaf's headers explain (projected on the
    // units) and its own multiplier, both shrunk by the controls' noise.
    TCalibration Calibrate(const TData& d, const TLayout& l, const TPrediction& p,
                           const std::vector<TObservation>& observations, const std::vector<TMeasuredPch>& probes) {
        ValidateTargets(d);
        if (l.Assignment.size() != d.Targets.size()) { Fail("calibration: assignment size mismatch"); }
        if (observations.size() != p.Actions.size()) {
            Fail("calibration: incomplete observations");
        }
        TCalibration out;
        const size_t actions = d.Actions.size();
        std::vector<int> fileOf(actions, -1);
        for (size_t f = 0; f < d.Files.size(); ++f) { fileOf[d.Files[f].Action] = int(f); }
        // A sampled verification leaves unmeasured actions at zero: keep the
        // model's own estimate for them instead of a zero time. Compilations
        // start from their trace; a measured target's anchor replaces it below.
        for (size_t i = 0; i < actions; ++i) {
            const bool measured = observations[i].Time > 0;
            out.ActionTime.push_back(fileOf[i] >= 0 ? d.Actions[i].Time : measured ? observations[i].Time : p.Actions[i].Time);
            out.ActionMemory.push_back(measured ? observations[i].Memory : p.Actions[i].Memory);
        }
        out.ActionFloor.assign(actions, 0);
        // Every artifact S0 built, by content identity.
        std::vector<int> measuredOf(l.Pchs.size(), -1);
        for (size_t g = 0; g < l.Pchs.size(); ++g) {
            const auto& obs = observations.at(actions + g);
            if (obs.Time <= 0 || obs.Bytes <= 0) { continue; } // not measured by a sampled run
            TMeasuredPch m;
            m.Pch = l.Pchs[g];
            m.Parent = l.Pchs[g].Parent >= 0 ? l.Pchs[l.Pchs[g].Parent] : TPch{};
            m.Bytes = obs.Bytes;
            m.Time = obs.Time;
            m.Memory = obs.Memory;
            measuredOf[g] = int(out.Measured.size());
            out.Measured.push_back(std::move(m));
        }
        // The probes a plan built, weighed and left: evicted by the measured
        // fit or emptied by the checks, they are the bases and deltas of
        // exactly the classes the plan drops, and the next selection would
        // estimate those classes from the prior. Their sizes are the
        // artifacts' own; their builds were not timed. One per content: what
        // the measurement weighed in its own right is already in Measured.
        auto sameArtifact = [](const TMeasuredPch& a, const TMeasuredPch& b) {
            if ((a.Pch.Parent >= 0) != (b.Pch.Parent >= 0) || !SameContent(a.Pch, b.Pch)) { return false; }
            return a.Pch.Parent < 0 || SameContent(a.Parent, b.Parent);
        };
        for (const auto& probe : probes) {
            if (!(probe.Bytes > 0) || probe.Pch.Units.empty() || probe.Pch.Class < 0 || probe.Pch.Class >= d.Classes) { continue; }
            bool known = false;
            for (const auto& m : out.Measured) { known = known || sameArtifact(probe, m); }
            for (const auto& other : out.Probes) { known = known || sameArtifact(probe, other); }
            if (!known) { out.Probes.push_back(probe); }
        }
        // One observation of size per artifact a class has: the build the
        // measurement timed, or the probe it only weighed.
        struct TArtifact {
            const TPch* Pch = nullptr;
            const TPch* Parent = nullptr; // the content of the parent; none for a base
            double Bytes = 0;
            bool Timed = true; // false for a probe: Time and Memory say nothing
            double Time = 0, Memory = 0;
        };
        std::vector<std::vector<TArtifact>> artifacts(d.Classes);
        auto artifact = [&](const TMeasuredPch& m) {
            if (m.Pch.Class < 0 || m.Pch.Class >= d.Classes) { return; }
            artifacts[m.Pch.Class].push_back({&m.Pch, m.Pch.Parent >= 0 ? &m.Parent : nullptr, m.Bytes, m.Time > 0, m.Time, m.Memory});
        };
        for (const auto& m : out.Measured) { artifact(m); }
        for (const auto& m : out.Probes) { artifact(m); }
        // A class's scale is its artifacts' PCH bytes per source byte,
        // fitted by volume. A 2 MB base says 3 where the artifacts of a run
        // say 6: the fewer the bytes behind it, the more of it is what that
        // one artifact happened to contain. Every class's scale is therefore
        // moved towards the classes' common one, and that one towards the
        // prior, by the bytes behind each: past some hundred megabytes a class
        // is its own evidence. What a class measured of a unit itself is kept
        // apart from this, below.
        constexpr double SizeEvidence = 100e6;
        auto weighed = [](double own, double bytes, double common) {
            return std::exp((bytes * std::log(own) + SizeEvidence * std::log(common)) / (bytes + SizeEvidence));
        };
        std::vector<double> sizeOf(d.Classes, 0.0), sizeBytes(d.Classes, 0.0);
        double sourceAll = 0, artifactAll = 0;
        size_t weighedAll = 0;
        {
            TConsumerModel view{d, out};
            for (int cls = 0; cls < d.Classes; ++cls) {
                double numerator = 0, denominator = 0;
                for (const auto& a : artifacts[cls]) {
                    double volume = 0;
                    view.ForOwn(*a.Pch, a.Parent ? &a.Parent->Units : nullptr, [&](int u) { volume += d.Units[u].Bytes; });
                    if (!(volume > 0)) { continue; }
                    // Artifact bytes add up over a layout, so the ratio is
                    // fitted by total bytes instead of a per-artifact median.
                    numerator += a.Bytes * volume;
                    denominator += volume * volume;
                    sizeBytes[cls] += a.Bytes;
                    sourceAll += volume;
                    artifactAll += a.Bytes;
                    ++weighedAll;
                }
                sizeOf[cls] = denominator > 0 ? std::clamp(numerator / denominator, 0.05, 20.0) : 0.0;
            }
        }
        out.GlobalSize = sourceAll > 0 ? weighed(std::clamp(artifactAll / sourceAll, 0.05, 20.0), artifactAll, TCoefficients().Size)
                                       : TCoefficients().Size;
        const double meanArtifact = weighedAll ? artifactAll / double(weighedAll) : 0.0;
        out.Classes.resize(d.Classes);
        out.UnitSize.assign(d.Classes, {});
        out.UnitBuild.assign(d.Classes, {});
        out.UnitGain.assign(d.Classes, {});
        out.TargetGain.assign(d.Targets.size(), 1.0);
        // What the sample says of one target under its S0 leaf.
        struct TSample {
            size_t Count = 0; // measured members
            double Time = 0, Base = 0, Memory = 0; // over the measured members
            double Estimate = 0; // Horvitz-Thompson seconds of the whole target
            double Whole = 0; // whole-target trace seconds
            std::vector<double> Imputed; // per member: seconds under the S0 leaf
        };
        std::vector<TSample> samples(d.Targets.size());
        for (size_t t = 0; t < d.Targets.size(); ++t) {
            const auto& files = d.Targets[t].Files;
            auto& s = samples[t];
            s.Imputed.assign(files.size(), 0);
            std::vector<char> isMeasured(files.size());
            for (size_t i = 0; i < files.size(); ++i) {
                const int action = d.Files[files[i]].Action;
                s.Whole += d.Actions[action].Time;
                if (observations[action].Time > 0) {
                    isMeasured[i] = 1;
                    ++s.Count;
                    s.Time += observations[action].Time;
                    s.Base += d.Actions[action].Time;
                    s.Memory += observations[action].Memory;
                }
            }
            if (!s.Count) { continue; }
            // The strata of SampleFiles, cut exactly as it cut them: members
            // by baseline time, Count equal ranges with one draw each, won
            // with a probability proportional to the baseline time. Such a
            // draw estimates its stratum's seconds without bias: the
            // stratum's trace time times the winner's ratio of observed to
            // trace time.
            std::vector<size_t> order(files.size());
            std::iota(order.begin(), order.end(), 0);
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                const auto& x = d.Actions[d.Files[files[a]].Action];
                const auto& y = d.Actions[d.Files[files[b]].Action];
                return x.Time != y.Time ? x.Time < y.Time : x.Output < y.Output;
            });
            const size_t n = files.size(), k = s.Count;
            for (size_t j = 0; j < k; ++j) {
                // floor(j * n / k), not ceil: the same members as the draw's stratum when n is not a multiple of k.
                const size_t begin = j * n / k, end = (j + 1) * n / k;
                double observedTime = 0, observedBase = 0;
                for (size_t i = begin; i < end; ++i) {
                    const int action = d.Files[files[order[i]]].Action;
                    if (isMeasured[order[i]]) {
                        observedTime += observations[action].Time;
                        observedBase += d.Actions[action].Time;
                    }
                }
                const double ratio = observedBase > 0 ? observedTime / observedBase : (s.Base > 0 ? s.Time / s.Base : 1);
                for (size_t i = begin; i < end; ++i) {
                    const size_t member = order[i];
                    const int action = d.Files[files[member]].Action;
                    s.Imputed[member] = isMeasured[member] ? observations[action].Time : d.Actions[action].Time * ratio;
                    s.Estimate += s.Imputed[member];
                }
            }
        }
        // The compiler's speed against the traces and a compile's seconds
        // outside them belong to the machine and to the baseline, not to one
        // flag set: every owner measured without a PCH, of every class,
        // measures them. The baseline is the median ratio of the controls
        // that take ten seconds or more to trace, where the overhead is a few
        // percent; the seconds outside the traces are what the controls that
        // take less than five leave, as a median. A class with one or two
        // controls of its own moves this line by its own ratio; with three or
        // more it keeps their median. A class without any used to start from
        // the ratio 1 and no overhead at all, and read a host compiling small
        // files for half again their traces as a PCH that saved nothing.
        double lineBaseline = 1, lineResidual = 0;
        {
            std::vector<std::pair<double, double>> controls; // trace seconds, observed seconds
            for (size_t t = 0; t < d.Targets.size(); ++t) {
                if (l.Assignment[t] >= 0) { continue; }
                for (int f : d.Targets[t].Files) {
                    const int action = d.Files[f].Action;
                    const double time = d.Actions[action].Time;
                    if (observations[action].Time > 0 && time > 0) {
                        controls.emplace_back(time, observations[action].Time);
                    }
                }
            }
            for (int iteration = 0; iteration < 8 && !controls.empty(); ++iteration) {
                std::vector<double> ratios, outside;
                for (const auto& [time, observed] : controls) {
                    if (time >= 10) { ratios.push_back((observed - lineResidual) / time); }
                }
                if (ratios.empty()) {
                    for (const auto& [time, observed] : controls) { ratios.push_back((observed - lineResidual) / time); }
                }
                lineBaseline = std::clamp(Median(ratios), 1e-3, 20.0);
                for (const auto& [time, observed] : controls) {
                    if (time < 5) { outside.push_back(observed - lineBaseline * time); }
                }
                if (!outside.empty()) { lineResidual = std::clamp(Median(outside), 0.0, 5.0); }
            }
        }
        std::ostringstream report;
        report << "class\tstatus\ttraining_targets\tcv_targets\tcv_target_MAE_s\tbase_build\tdelta_build\tsize\tmemory"
                  "\tbaseline\tsaved\tload\tresidual\tinstantiate\tcontrols\tcontrol_sigma\tanchored_targets"
                  "\tunit_size_units\tunit_build_units\tunit_gain_units\tsize_evidence_bytes\tprobe_artifacts\n";
        std::ostringstream targets, units;
        targets << "# One row per measured target with a PCH. s0_compile_seconds: Horvitz-Thompson estimate of the target's\n"
                   "# whole compile under its S0 leaf (sampled strata imputed by their observed-to-trace ratio); trace_seconds:\n"
                   "# the traces' total. gain: the target's saved-time multiplier (1: unmeasured or on the class model).\n"
                   "target\tclass\ts0_leaf\tmembers\tmeasured\ts0_compile_seconds\ttrace_seconds\tgain\n";
        units << "# Header units a measurement calibrated individually; a table of the class prior applies to the others.\n"
                  "# pch_bytes_per_source_byte: 0 keeps the class size scale; build_seconds_per_parse_second: 0 the class build\n"
                  "# scale; saved_multiplier: -1 none (saving as the class coefficients say), 0 a leaf that helped its owners nothing, otherwise a factor on the unit's own time.\n"
                  "unit\tclass\tinclude\tpch_bytes_per_source_byte\tbuild_seconds_per_parse_second\tsaved_multiplier\n";
        auto nonZero = [](const std::vector<double>& values) {
            return size_t(std::count_if(values.begin(), values.end(), [](double x) { return x > 0; }));
        };
        auto calibrated = [](const std::vector<double>& values) {
            return size_t(std::count_if(values.begin(), values.end(), [](double x) { return x >= 0; }));
        };
        for (int cls = 0; cls < d.Classes; ++cls) {
            auto& c = out.Classes[cls];
            TConsumerModel view{d, out};
            std::vector<int> controls, users;
            for (size_t t = 0; t < d.Targets.size(); ++t) {
                if (d.Targets[t].Class == cls && samples[t].Count) {
                    (l.Assignment[t] < 0 ? controls : users).push_back(int(t));
                }
            }
            // Controls: the compiler's speed on this class without a PCH.
            // The ratio of observed to trace seconds is the fallback; with a
            // spread of trace times the baseline and the per-compile seconds
            // outside the traces (driver, startup) are one linear model of a
            // no-PCH compile, fitted on the controls' members. A compile pays
            // those seconds with and without its PCH, so PCH targets cannot
            // show them; the controls do.
            std::vector<double> ratios;
            for (int t : controls) {
                if (samples[t].Whole > 0) { ratios.push_back(samples[t].Estimate / samples[t].Whole); }
            }
            // Three controls or more: the class keeps the median ratio of its
            // own controls. With one or two (none for the smallest classes)
            // that ratio is its owners' noise as much as the class's speed:
            // the class starts from the common line and moves it by their
            // ratio to that line, shrunk by their number.
            if (ratios.size() >= 3) {
                c.Baseline = Ratio(ratios, 1, 1e-3);
                c.Residual = 0;
                // With a spread of trace times the baseline and the per-compile
                // seconds outside the traces (driver, startup) are one linear
                // model of a no-PCH compile, fitted on the controls' members.
                // Only a class with controls enough to speak for itself is
                // fitted: a line through the files of one or two controls is
                // their noise, and would undo the start from the common line.
                std::vector<double> a(6, 0.0);
                size_t members = 0;
                for (int t : controls) {
                    for (int f : d.Targets[t].Files) {
                        const int action = d.Files[f].Action;
                        const double time = d.Actions[action].Time;
                        if (observations[action].Time <= 0 || !(time > 0)) { continue; }
                        // y / time = B + R / time: unit weights.
                        a[0] += 1;
                        a[1] += 1 / time;
                        a[2] += observations[action].Time / time;
                        a[3] += 1 / time;
                        a[4] += 1 / (time * time);
                        a[5] += observations[action].Time / (time * time);
                        ++members;
                    }
                }
                std::vector<double> solution;
                // The intercept is the seconds of a compile outside its traces
                // (process and driver startup): never negative. A negative
                // slope is noise on a fast control; the ratio stands alone.
                if (members >= 3 && Solve(a, 2, 1e-3, solution) && solution[0] >= 1e-3 && solution[0] <= 20 &&
                    solution[1] >= 0 && solution[1] <= 5) {
                    c.Baseline = solution[0];
                    c.Residual = solution[1];
                }
            } else {
                c.Baseline = lineBaseline;
                c.Residual = lineResidual;
                std::vector<double> shift;
                for (int t : controls) {
                    const auto& s = samples[t];
                    // The whole target, measured or sampled: every file pays the seconds outside the traces.
                    const double expected = lineBaseline * s.Whole + lineResidual * double(d.Targets[t].Files.size());
                    if (expected > 0 && s.Estimate > 0) { shift.push_back(s.Estimate / expected); }
                }
                if (!shift.empty()) {
                    const double ratio = 1 + double(shift.size()) / (shift.size() + 2) * (Median(shift) - 1);
                    c.Baseline = lineBaseline * ratio;
                    c.Residual = lineResidual * ratio;
                }
            }
            // How much a whole control target deviates from that model (a
            // baseline offset of a target is noise to any PCH measurement on
            // it) and how much one file deviates from its target (sampling noise).
            std::vector<double> deviations, fitted;
            for (int t : controls) {
                double model = 0;
                for (int f : d.Targets[t].Files) {
                    const int action = d.Files[f].Action;
                    const double expected = c.Baseline * d.Actions[action].Time + c.Residual;
                    model += expected;
                    if (observations[action].Time > 0 && expected > 0) {
                        deviations.push_back(observations[action].Time / expected - 1);
                    }
                }
                if (model > 0) { fitted.push_back(samples[t].Estimate / model); }
            }
            const double sigma = fitted.size() >= 2 ? std::clamp(Spread(fitted), 0.02, 2.0) : 0.15;
            double square = 0;
            for (double v : deviations) { square += v * v; }
            const double nu = deviations.empty() ? 0.15 : std::clamp(std::sqrt(square / deviations.size()), 0.02, 1.0);
            std::vector<double> baseBuild, deltaBuild, memory, consumerMemory;
            size_t probesOf = 0;
            auto own = [&](const TArtifact& a, auto&& fn) {
                view.ForOwn(*a.Pch, a.Parent ? &a.Parent->Units : nullptr, fn);
            };
            for (const auto& a : artifacts[cls]) {
                const double parentParse = a.Parent ? Parse(d, a.Parent->Units) : 0;
                const double volume = Volume(d, a.Pch->Units);
                const double ownParse = Parse(d, a.Pch->Units) - parentParse;
                probesOf += !a.Timed;
                if (a.Timed && ownParse > 0) {
                    (a.Parent ? deltaBuild : baseBuild).push_back(std::max(0.001, a.Time - parentParse * 0.02 * c.Baseline) / ownParse);
                }
                if (a.Timed) { memory.push_back(a.Memory / (d.ActionMemory + volume)); }
            }
            // Zero until the classes that measured each are known: the
            // fallback below is theirs, not the prior's.
            c.BaseBuild = Ratio(baseBuild, 0.0);
            c.DeltaBuild = Ratio(deltaBuild, 0.0);
            c.Size = sizeOf[cls] > 0 ? weighed(sizeOf[cls], sizeBytes[cls], out.GlobalSize) : out.GlobalSize;
            c.Memory = Ratio(memory);
            // Unit decomposition of the same artifacts: which headers made a
            // group as large and as slow as it was. A group is one observation
            // over its own units: the ridge moves a lone group's units by the
            // same share and splits the evidence of units several groups share.
            // An artifact weighs as its bytes in the decomposition of size, the
            // ratio of a small one being the noisier; a probe that nothing
            // timed has no say in the decomposition of build time.
            {
                std::vector<std::map<int, double>> sizeRows, buildRows;
                std::vector<double> sizeTargets, buildTargets;
                std::map<int, double> buildPrior;
                for (const auto& a : artifacts[cls]) {
                    const bool isBase = a.Parent == nullptr;
                    const double scale = isBase ? c.BaseBuild : c.DeltaBuild;
                    const double parentParse = isBase ? 0 : Parse(d, a.Parent->Units);
                    double bytes = 0, work = 0;
                    own(a, [&](int u) { bytes += d.Units[u].Bytes; work += d.Units[u].Parse * scale; });
                    if (bytes > 0 && meanArtifact > 0) {
                        const double weight = std::clamp(std::sqrt(a.Bytes / meanArtifact), 0.05, 20.0);
                        std::map<int, double> row;
                        own(a, [&](int u) { row[u] = weight * d.Units[u].Bytes / bytes; });
                        sizeRows.push_back(std::move(row));
                        sizeTargets.push_back(weight * a.Bytes / (c.Size * bytes));
                    }
                    if (a.Timed && work > 0) {
                        std::map<int, double> row;
                        own(a, [&](int u) { row[u] = d.Units[u].Parse * scale / work; buildPrior.emplace(u, scale); });
                        buildRows.push_back(std::move(row));
                        buildTargets.push_back(std::max(0.001, a.Time - parentParse * 0.02 * c.Baseline) / work);
                    }
                }
                const auto sizeX = Ridge(sizeRows, sizeTargets, UnitRidge);
                const auto buildX = Ridge(buildRows, buildTargets, UnitRidge);
                if (!sizeX.empty()) {
                    out.UnitSize[cls].assign(d.Units.size(), 0);
                    for (const auto& [u, x] : sizeX) { out.UnitSize[cls][u] = c.Size * std::clamp(x, 0.05, 20.0); }
                }
                if (!buildX.empty()) {
                    out.UnitBuild[cls].assign(d.Units.size(), 0);
                    for (const auto& [u, x] : buildX) {
                        out.UnitBuild[cls][u] = buildPrior.at(u) * std::clamp(x, 0.01, 100.0);
                    }
                }
            }
            // One equation per measured target with a PCH, over its measured
            // members, centered by the controls' baseline and residual.
            struct TRow {
                int Target = 0, Group = 0;
                double Y = 0, Saved = 0, Templates = 0, Load = 0, Weight = 1;
                double Base = 0; // trace seconds of the files of the row
                std::vector<int> Files;
            };
            std::vector<TRow> rows;
            for (int t : users) {
                const auto& s = samples[t];
                const int g = l.Assignment[t];
                const double load = view.Load(l.Pchs[g].Units);
                TRow row;
                row.Target = t;
                row.Group = g;
                double square = 0;
                for (int f : d.Targets[t].Files) {
                    const int action = d.Files[f].Action;
                    if (observations[action].Time <= 0) { continue; }
                    // A member measured at its backend floor has nothing left
                    // for the saving scale to show: the PCH took all it
                    // could, and the linear model would read the floor as a
                    // lost saving. The anchor and the multiplier below still
                    // see it, floor included.
                    const double floor = c.Baseline * TConsumerModel::Backend(d.Actions[action].Time, d.Files[f].Frontend);
                    if (observations[action].Time <= 1.02 * floor) { continue; }
                    const double saved = Saved(d.Files[f], l.Pchs[g].Units);
                    row.Y += observations[action].Time - c.Baseline * d.Actions[action].Time - c.Residual;
                    row.Saved += saved;
                    row.Templates += saved * TConsumerModel::Templates(d.Files[f]);
                    row.Base += d.Actions[action].Time;
                    square += d.Actions[action].Time * d.Actions[action].Time;
                    row.Files.push_back(f);
                }
                row.Load = row.Files.size() * load;
                // The noise of a target's sum grows with the root of its
                // members' squared trace times. Each equation is normalized by
                // it: twenty short files no longer outweigh two hundred
                // ordinary ones four hundred times over.
                row.Weight = square > 0 ? 1 / square : 1;
                // The artifacts its members really read: the measured ones when S0 built them.
                auto artifactBytes = [&](int group) {
                    return measuredOf[group] >= 0 ? out.Measured[measuredOf[group]].Bytes : p.PchBytes[group];
                };
                double bytes = artifactBytes(g);
                if (l.Pchs[g].Parent >= 0) { bytes += artifactBytes(l.Pchs[g].Parent); }
                consumerMemory.push_back(s.Memory / (s.Count * (d.ActionMemory + bytes)));
                // A target measured only at its backend floor shows the fit no
                // saving at all: an empty equation would count as a target in
                // every shrinkage and as a zero in every scale of the fit.
                if (!row.Files.empty()) { rows.push_back(std::move(row)); }
            }
            double maxTemplates = 0;
            for (const auto& file : d.Files) {
                if (file.Class == cls) { maxTemplates = std::max(maxTemplates, TConsumerModel::Templates(file)); }
            }
            struct TFit {
                double Saved = 1, Load = 0.02, Instantiate = 0;
                int Status = -2; // 2 with instantiation, 1 saved/load, -1 combined, -2 prior
            };
            // Saved and load from the measured targets with a PCH, on data
            // centered by the controls' baseline and residual. The class
            // describes the typical target: Huber reweighting keeps a few
            // exceptional targets (which the empirical Bayes step attributes
            // individually below) from dragging the coefficients.
            auto fit = [&](const std::vector<TRow>& data) {
                const size_t n = data.size();
                TFit result;
                auto attempt = [&](bool withTemplates, double tolerance) {
                    const int k = withTemplates ? 3 : 2;
                    std::vector<double> x[3];
                    for (int i = 0; i < k; ++i) { x[i].resize(n); }
                    for (size_t r = 0; r < n; ++r) {
                        x[0][r] = -data[r].Saved;
                        if (withTemplates) {
                            x[1][r] = -data[r].Templates;
                            x[2][r] = data[r].Load;
                        } else {
                            x[1][r] = data[r].Load;
                        }
                        if (!std::isfinite(data[r].Y)) { return false; }
                        for (int i = 0; i < k; ++i) {
                            if (!std::isfinite(x[i][r])) { return false; }
                        }
                    }
                    std::vector<double> huber(n, 1.0), solution;
                    for (int iteration = 0;; ++iteration) {
                        std::vector<double> a(size_t(k) * (k + 1), 0.0);
                        for (size_t r = 0; r < n; ++r) {
                            const double w = data[r].Weight * huber[r];
                            for (int i = 0; i < k; ++i) {
                                for (int j = 0; j < k; ++j) { a[size_t(i) * (k + 1) + j] += w * x[i][r] * x[j][r]; }
                                a[size_t(i) * (k + 1) + k] += w * x[i][r] * data[r].Y;
                            }
                        }
                        if (!Solve(a, k, tolerance, solution)) { return false; }
                        if (iteration >= 5) { break; }
                        // Residuals in units of each equation's noise.
                        std::vector<double> absolute(n);
                        for (size_t r = 0; r < n; ++r) {
                            double fitted = 0;
                            for (int i = 0; i < k; ++i) { fitted += x[i][r] * solution[i]; }
                            absolute[r] = std::abs((data[r].Y - fitted) * std::sqrt(data[r].Weight));
                        }
                        const double scale = 1.4826 * Median(absolute);
                        if (scale < 1e-9) { break; }
                        double moved = 0;
                        for (size_t r = 0; r < n; ++r) {
                            const double next = std::min(1.0, 1.345 * scale / std::max(absolute[r], 1e-300));
                            moved += std::abs(next - huber[r]);
                            huber[r] = next;
                        }
                        if (moved < 1e-9) { break; }
                    }
                    if (solution[0] < 0 || solution[0] > 3) { return false; }
                    result.Saved = solution[0];
                    result.Load = withTemplates ? solution[2] : solution[1];
                    if (result.Load < 0 || result.Load > 3) { return false; }
                    result.Instantiate = 0;
                    if (withTemplates) {
                        // The extra saving cannot exceed the semantics outside
                        // the includes, nor flip a header's own saving into a cost.
                        const double instantiate = solution[0] > 1e-9 ? solution[1] / solution[0] : 0;
                        if (std::abs(solution[1]) > c.Baseline || instantiate > 5 || instantiate * maxTemplates < -1) { return false; }
                        result.Instantiate = instantiate;
                    }
                    result.Status = withTemplates ? 2 : 1;
                    return true;
                };
                if (n >= 3 && ratios.size() >= 3 && (attempt(true, 0.02) || attempt(false, 1e-3))) {
                    // Small-data shrinkage toward the prior, after rank testing.
                    const double weight = double(n) / (n + 2);
                    result.Saved = 1 + weight * (result.Saved - 1);
                    result.Load = 0.02 + weight * (result.Load - 0.02);
                    result.Instantiate *= weight;
                    return result;
                }
                // One observable combined saving, not separate load/parse
                // claims, and relative to the class baseline: a PCH takes
                // parse seconds off a compile at the rate the class compiles
                // them, so the prior scale is 1. Every row weighs as its gain
                // stands to the noise of its target. The median of the row
                // ratios had a 2 MB base on a 1200 s owner divide the host's
                // speed against the traces by next to nothing, and clamp at
                // 3: such a row carries no information and leaves the prior.
                const double prior = 4; // 1 / variance of the scale around 1
                std::vector<std::pair<double, double>> rows; // scale of a row, its information: 1 / its variance
                for (const auto& row : data) {
                    const double gain = row.Saved - 0.02 * row.Load;
                    if (gain <= 0.001 || c.Baseline <= 0) { continue; }
                    // The noise of a row at the scale of its own files: the
                    // target's offset over their seconds, and each file's own
                    // over the root of their squares. Taken from the whole
                    // target, it grew with every file a sample or the floor
                    // left out, and the row's information fell with it.
                    const double level = sigma * row.Base, sample = nu * std::sqrt(row.Weight > 0 ? 1 / row.Weight : 0);
                    rows.emplace_back(-row.Y / gain / c.Baseline, gain * gain / std::max(1e-12, level * level + sample * sample));
                }
                if (rows.empty()) { return TFit{c.Baseline, 0.02 * c.Baseline, 0, -2}; }
                // The median scale of the rows, each weighing as its gain
                // stands to the noise of its target: a few exceptional
                // targets do not drag the class, and a row with next to no
                // gain has next to no vote. What information the rows do
                // carry moves the prior scale by that share of the distance.
                std::sort(rows.begin(), rows.end());
                double total = 0;
                for (const auto& [r, w] : rows) { total += w; }
                double run = 0, median = 1;
                for (const auto& [r, w] : rows) {
                    run += w;
                    if (run * 2 >= total) {
                        median = r;
                        break;
                    }
                }
                const double scale = std::clamp(1 + total / (total + prior) * (median - 1), -3.0, 3.0);
                return TFit{scale * c.Baseline, 0.02 * scale * c.Baseline, 0, -1};
            };
            // Leave one group out, a diagnostic only: each measured group is
            // predicted from all the others. The working coefficients below
            // are fitted on every measured group and base.
            std::set<int> rowGroups;
            for (const auto& row : rows) { rowGroups.insert(row.Group); }
            double error = 0;
            size_t cv = 0;
            std::vector<TRow> training;
            for (int held : rowGroups) {
                training.clear();
                for (const auto& row : rows) {
                    if (row.Group != held) { training.push_back(row); }
                }
                const TFit fold = fit(training);
                for (const auto& row : rows) {
                    if (row.Group != held || row.Files.empty()) { continue; }
                    TCoefficients coefficients = c;
                    coefficients.Saved = fold.Saved;
                    coefficients.Load = fold.Load;
                    coefficients.Instantiate = fold.Instantiate;
                    double value = 0, actual = 0;
                    for (int f : row.Files) {
                        const int action = d.Files[f].Action;
                        const double saved = Saved(d.Files[f], l.Pchs[row.Group].Units);
                        const double theta = std::max(0.0, 1 + fold.Instantiate * TConsumerModel::Templates(d.Files[f]));
                        value += Consumer(d.Actions[action].Time,
                                          c.Baseline * TConsumerModel::Backend(d.Actions[action].Time, d.Files[f].Frontend),
                                          theta * saved, row.Load / double(row.Files.size()), coefficients);
                        actual += observations[action].Time;
                    }
                    error += std::abs(value - actual);
                    ++cv;
                }
            }
            const TFit final = fit(rows);
            c.Saved = final.Saved;
            c.Load = final.Load;
            c.Instantiate = final.Instantiate;
            c.Identified = final.Status > 0;
            c.ConsumerMemory = Ratio(consumerMemory);
            // Each measured PCH target against the class model, relative to
            // the parse seconds the PCH saved it. Two empirical Bayes steps
            // separate what the leaf's headers explain from what is the
            // target's own: the mean deviation of a leaf over its measured
            // owners, shrunk by their noise, is projected on the leaf's own
            // units; the remainder of each target's deviation is shrunk by the
            // controls' spread and the target's sampling noise. A target
            // measured no faster with its PCH than the controls are without
            // one, beyond that noise, ends with a multiplier of zero: the PCH
            // saved it nothing, so it loses the PCH at the payback test, and
            // its no-PCH time is what S0 measured minus the load it paid. A
            // PCH is never credited with slowing a compile beyond that load:
            // the excess of one measurement over the model is far more often
            // a stale baseline of the target than a negative saving, and
            // assuming the latter predicts, for a target S0 saw at 28 s, a
            // no-PCH compile of 18 s. One measured far faster than its own
            // time predicts keeps a multiplier above one and, with it, its delta.
            struct TEffect {
                int Target = 0, Group = 0;
                double Saved = 0, Delta = 0, Noise = 0, Implied = 1, Remainder = 0;
                std::vector<int> Files;
            };
            // Every measured PCH target takes part, whether its files left the
            // fit an equation or all sat on the floor: the model prices them
            // with the floor, so their saving counts here.
            std::vector<TEffect> effects;
            if (c.Saved > 0) {
                for (int t : users) {
                    const auto& s = samples[t];
                    const int g = l.Assignment[t];
                    const double load = view.Load(l.Pchs[g].Units);
                    double model = 0, saved = 0;
                    for (int f : d.Targets[t].Files) {
                        const int action = d.Files[f].Action;
                        const double effective = std::max(0.0, 1 + c.Instantiate * TConsumerModel::Templates(d.Files[f])) *
                            Saved(d.Files[f], l.Pchs[g].Units);
                        saved += effective;
                        // The compile time the selector computes for it, the
                        // backend floor included.
                        model += Consumer(d.Actions[action].Time,
                                          c.Baseline * TConsumerModel::Backend(d.Actions[action].Time, d.Files[f].Frontend),
                                          effective, load, c);
                    }
                    const double sav = c.Saved * saved;
                    // Without saved parse a deviation is not the PCH's doing.
                    if (!(s.Whole > 0) || sav < 0.02 * c.Baseline * s.Whole) { continue; }
                    TEffect e;
                    e.Target = t;
                    e.Group = g;
                    e.Saved = saved;
                    for (int f : d.Targets[t].Files) {
                        if (observations[d.Files[f].Action].Time > 0) { e.Files.push_back(f); }
                    }
                    e.Delta = (model - s.Estimate) / sav;
                    const double level = sigma * s.Whole, sample = nu * s.Estimate / std::sqrt(double(s.Count));
                    e.Noise = (level * level + sample * sample) / (sav * sav);
                    effects.push_back(std::move(e));
                }
            }
            // Variance of a real effect: what the squared deviations show beyond their noise.
            auto tauSquare = [](const std::vector<double>& squares, const std::vector<double>& noise) {
                if (squares.size() < 3) { return 0.25; }
                double a = 0, b = 0;
                for (size_t i = 0; i < squares.size(); ++i) { a += squares[i]; b += noise[i]; }
                return std::max(0.01, a / squares.size() - b / noise.size());
            };
            // Group level. A leaf's effect is what its measured owners share;
            // its evidence is their mean deviation, whose noise is the sum of
            // theirs over the square of their number.
            struct TGroupEffect {
                int Group = 0;
                double Effect = 0;
                std::vector<const TEffect*> Members;
            };
            std::vector<TGroupEffect> groupEffects;
            {
                std::map<int, std::vector<const TEffect*>> byGroup;
                for (const auto& e : effects) { byGroup[e.Group].push_back(&e); }
                std::vector<double> squares, noise;
                for (const auto& [g, members] : byGroup) {
                    double mean = 0, variance = 0;
                    for (const auto* e : members) { mean += e->Delta; variance += e->Noise; }
                    mean /= members.size();
                    variance /= members.size() * members.size();
                    groupEffects.push_back({g, 0, members});
                    squares.push_back(mean * mean);
                    noise.push_back(variance);
                    groupEffects.back().Effect = mean;
                }
                const double tau = tauSquare(squares, noise);
                for (size_t i = 0; i < groupEffects.size(); ++i) {
                    groupEffects[i].Effect = tau / (tau + noise[i]) * groupEffects[i].Effect;
                }
            }
            // The mean effect of each leaf is projected on its own units: the
            // saved time its owners' files have in them, as shares.
            if (!groupEffects.empty()) {
                std::vector<std::map<int, double>> gainRows;
                std::vector<double> gainTargets;
                for (const auto& group : groupEffects) {
                    const int g = group.Group;
                    const auto& members = group.Members;
                    std::vector<int> ownUnits;
                    view.ForOwn(l.Pchs[g], l.Pchs[g].Parent >= 0 ? &l.Pchs[l.Pchs[g].Parent].Units : nullptr,
                                [&](int u) { ownUnits.push_back(u); });
                    std::map<int, double> row;
                    double total = 0;
                    for (const auto* e : members) {
                        for (int f : e->Files) {
                            const double theta = std::max(0.0, 1 + c.Instantiate * TConsumerModel::Templates(d.Files[f]));
                            for (auto [u, time] : d.Files[f].Own) {
                                if (std::binary_search(ownUnits.begin(), ownUnits.end(), u)) {
                                    row[u] += time * theta;
                                    total += time * theta;
                                }
                            }
                        }
                    }
                    if (total <= 0) { continue; }
                    for (auto& [u, share] : row) { share /= total; }
                    gainRows.push_back(std::move(row));
                    gainTargets.push_back(1 + group.Effect);
                }
                const auto gainX = Ridge(gainRows, gainTargets, UnitRidge);
                if (!gainX.empty()) {
                    // -1 marks the units no measured leaf owns; a clamped zero
                    // is a leaf that slowed its owners down, not a missing value.
                    out.UnitGain[cls].assign(d.Units.size(), -1.0);
                    for (const auto& [u, x] : gainX) { out.UnitGain[cls][u] = std::clamp(x, 0.0, 3.0); }
                }
            }
            // Target level: what the units already explain of each target's
            // deviation, and what is left for the target alone.
            std::vector<double> squares, noise;
            for (auto& e : effects) {
                double raw = 0, measured = 0;
                for (int f : d.Targets[e.Target].Files) {
                    const double theta = std::max(0.0, 1 + c.Instantiate * TConsumerModel::Templates(d.Files[f]));
                    raw += theta * Saved(d.Files[f], l.Pchs[e.Group].Units);
                    measured += view.Saved(d.Files[f], l.Pchs[e.Group].Units);
                }
                e.Implied = raw > 0 ? measured / raw : 1;
                e.Remainder = e.Delta - (e.Implied - 1);
                squares.push_back(e.Remainder * e.Remainder);
                noise.push_back(e.Noise);
            }
            const double targetTau = tauSquare(squares, noise);
            for (const auto& e : effects) {
                // The multiplier applies on top of the units': with the full
                // weight, target times units reproduces the target's whole
                // deviation, (1 + delta) times its raw saving.
                const double weight = targetTau / (targetTau + e.Noise);
                // At worst the PCH saved this target nothing: never less.
                const double gain = e.Implied > 1e-9 ? 1 + weight * e.Remainder / e.Implied : 1;
                out.TargetGain[e.Target] = std::isfinite(gain) ? std::clamp(gain, 0.0, 4.0) : 1.0;
            }
            // Anchor every measured target of the class at what S0 really
            // took for it: the baseline that makes the model reproduce the
            // observed compile under the S0 leaf. Only the headers another
            // leaf adds or removes are modeled from here. The backend floor
            // of a measured file never exceeds its observed time.
            size_t anchored = 0;
            for (size_t t = 0; t < d.Targets.size(); ++t) {
                if (d.Targets[t].Class != cls || !samples[t].Count) { continue; }
                const auto& s = samples[t];
                const int g = l.Assignment[t];
                const double load = g >= 0 ? view.Load(l.Pchs[g].Units) : 0;
                const auto& files = d.Targets[t].Files;
                for (size_t i = 0; i < files.size(); ++i) {
                    const auto& file = d.Files[files[i]];
                    const double time = std::max(1e-6, s.Imputed[i]);
                    const auto k = view.Coefficients(file);
                    const double saved = g >= 0 ? view.Saved(file, l.Pchs[g].Units) : 0;
                    // The baseline that makes Consumer return the observed time
                    // on the S0 leaf, slowdown clamp included. The floor is
                    // never above that time, so the leaf reproduces it exactly.
                    out.ActionTime[file.Action] = std::max(1e-3, (time + PchGain(saved, load, k) - c.Residual) / c.Baseline);
                    out.ActionFloor[file.Action] = std::min(
                        std::max(0.0, c.Baseline * TConsumerModel::Backend(d.Actions[file.Action].Time, file.Frontend)), time);
                }
                ++anchored;
                if (g < 0) { continue; }
                targets << d.Targets[t].Label << '\t' << cls << '\t' << g << '\t' << files.size() << '\t' << s.Count << '\t'
                        << s.Estimate << '\t' << s.Whole << '\t' << out.TargetGain[t] << '\n';
            }
            // Unmeasured compilations keep their trace time, under the class backend floor.
            for (size_t f = 0; f < d.Files.size(); ++f) {
                const int action = d.Files[f].Action;
                if (d.Files[f].Class != cls || samples[d.Files[f].Target].Count) { continue; }
                out.ActionTime[action] = d.Actions[action].Time;
                out.ActionFloor[action] = std::max(0.0, c.Baseline * TConsumerModel::Backend(d.Actions[action].Time, d.Files[f].Frontend));
            }
            for (size_t u = 0; u < d.Units.size(); ++u) {
                const double size = u < out.UnitSize[cls].size() ? out.UnitSize[cls][u] : 0;
                const double build = u < out.UnitBuild[cls].size() ? out.UnitBuild[cls][u] : 0;
                const double gain = u < out.UnitGain[cls].size() ? out.UnitGain[cls][u] : -1;
                if (size <= 0 && build <= 0 && gain < 0) { continue; }
                units << u << '\t' << cls << '\t' << d.Units[u].Include << '\t' << size << '\t' << build << '\t' << gain << '\n';
            }
            report << cls << '\t' << (final.Status > 0 ? "identified" : final.Status == -1 ? "combined-only" : "prior-only")
                   << '\t' << rows.size() << '\t' << cv << '\t';
            if (cv) {
                report << error / cv;
            } else {
                report << "unavailable";
            }
            report << '\t' << (baseBuild.empty() ? "unmeasured" : std::to_string(c.BaseBuild))
                   << '\t' << (deltaBuild.empty() ? "unmeasured" : std::to_string(c.DeltaBuild))
                   << '\t' << c.Size
                   << '\t' << (memory.empty() ? "unmeasured" : std::to_string(c.Memory))
                   << '\t' << c.Baseline << '\t' << c.Saved << '\t' << c.Load << '\t' << c.Residual << '\t' << c.Instantiate
                   << '\t' << ratios.size() << '\t' << sigma << '\t' << anchored
                   << '\t' << nonZero(out.UnitSize[cls]) << '\t' << nonZero(out.UnitBuild[cls]) << '\t' << calibrated(out.UnitGain[cls])
                   << '\t' << sizeBytes[cls] << '\t' << probesOf << '\n';
        }
        // Across classes. The bytes a header adds to a PCH and the seconds it
        // takes to serialize are the header's and the compiler's; one flag
        // set does not change them. Every artifact every class weighed,
        // measured or only probed, is one observation over its own units,
        // decomposed together: a unit a class never measured itself (the
        // project headers of a new delta for a class whose initial plan held
        // a 2 MB base) takes the pooled estimate instead of the scale that
        // little artifact suggested. Bytes are divided by the bytes of the
        // artifact, each weighing as its size; build seconds by the common
        // scale of the role, base or delta. Against the scale of the class
        // itself they divided to one on every header: a class with a single
        // base has that base for its whole scale, and divides it by itself.
        {
            TConsumerModel view{d, out};
            struct TOwn {
                bool Base = true, Timed = true;
                double Source = 0, Parse = 0, Artifact = 0, Time = 0, Weight = 1;
                std::map<int, double> Bytes, Own; // unit -> source bytes, unit -> parse seconds
            };
            std::vector<TOwn> own;
            auto weigh = [&](const TMeasuredPch& m, bool timed) {
                const auto& coefficients = Coeff(out, m.Pch.Class);
                TOwn o;
                o.Base = m.Pch.Parent < 0;
                o.Timed = timed;
                const auto* parent = o.Base ? nullptr : &m.Parent.Units;
                view.ForOwn(m.Pch, parent, [&](int u) {
                    o.Bytes[u] += d.Units[u].Bytes;
                    o.Own[u] += d.Units[u].Parse;
                    o.Source += d.Units[u].Bytes;
                    o.Parse += d.Units[u].Parse;
                });
                o.Artifact = m.Bytes;
                o.Weight = meanArtifact > 0 ? std::clamp(std::sqrt(m.Bytes / meanArtifact), 0.05, 20.0) : 1.0;
                o.Time = std::max(0.001, m.Time - (parent ? Parse(d, *parent) * 0.02 * coefficients.Baseline : 0));
                if (o.Source > 0 && o.Parse > 0) { own.push_back(std::move(o)); }
            };
            for (const auto& m : out.Measured) { weigh(m, true); }
            for (const auto& m : out.Probes) { weigh(m, false); }
            double source = 0, artifact = 0;
            std::vector<double> baseTimes, deltaTimes;
            for (const auto& o : own) {
                source += o.Source;
                artifact += o.Artifact;
                if (o.Timed) { (o.Base ? baseTimes : deltaTimes).push_back(o.Time / o.Parse); }
            }
            const double size = source > 0 ? std::clamp(artifact / source, 0.05, 20.0) : 0.0;
            out.GlobalBaseBuild = Ratio(baseTimes, 0.0);
            out.GlobalDeltaBuild = Ratio(deltaTimes, 0.0);
            std::vector<std::map<int, double>> sizeRows, buildRows;
            std::vector<double> sizeTargets, buildTargets;
            for (const auto& o : own) {
                double bytes = 0;
                for (const auto& [u, b] : o.Bytes) { bytes += b; }
                std::map<int, double> sizeRow;
                for (const auto& [u, b] : o.Bytes) { sizeRow[u] = o.Weight * b / bytes; }
                sizeRows.push_back(std::move(sizeRow));
                sizeTargets.push_back(o.Weight * o.Artifact / o.Source / size);
                if (!o.Timed || !(o.Parse > 0)) { continue; }
                const double common = o.Base ? out.GlobalBaseBuild : out.GlobalDeltaBuild;
                if (!(common > 0)) { continue; } // no class timed this role
                std::map<int, double> buildRow;
                for (const auto& [u, parse] : o.Own) { buildRow[u] = parse / o.Parse; }
                buildRows.push_back(std::move(buildRow));
                buildTargets.push_back(o.Time / (o.Parse * common));
            }
            if (size > 0) {
                const auto sizeX = Ridge(sizeRows, sizeTargets, UnitRidge);
                out.GlobalUnitSize.assign(d.Units.size(), 0.0);
                for (const auto& [u, x] : sizeX) { out.GlobalUnitSize[u] = size * std::clamp(x, 0.05, 20.0); }
            }
            if (!buildRows.empty()) {
                // A header's share of the role's common seconds per parse second.
                const auto buildX = Ridge(buildRows, buildTargets, UnitRidge);
                out.GlobalUnitBuild.assign(d.Units.size(), 0.0);
                for (const auto& [u, x] : buildX) { out.GlobalUnitBuild[u] = std::clamp(x, 0.01, 100.0); }
            }
            // A class that measured a role of artifact in no group of its own
            // takes the classes that did; with none, the prior stands.
            for (auto& coefficients : out.Classes) {
                if (coefficients.BaseBuild <= 0) { coefficients.BaseBuild = out.GlobalBaseBuild > 0 ? out.GlobalBaseBuild : TCoefficients().BaseBuild; }
                if (coefficients.DeltaBuild <= 0) { coefficients.DeltaBuild = out.GlobalDeltaBuild > 0 ? out.GlobalDeltaBuild : TCoefficients().DeltaBuild; }
            }
            report << "# line_baseline\t" << lineBaseline << "\tline_residual\t" << lineResidual
                   << "\tglobal_size\t" << size << "\tglobal_base_build\t" << out.GlobalBaseBuild
                   << "\tglobal_delta_build\t" << out.GlobalDeltaBuild << "\tglobal_size_units\t" << nonZero(out.GlobalUnitSize)
                   << "\tglobal_build_units\t" << nonZero(out.GlobalUnitBuild)
                   << "\tglobal_size_scale\t" << out.GlobalSize << "\tprobe_artifacts\t" << out.Probes.size() << '\n';
            for (size_t u = 0; u < d.Units.size(); ++u) {
                if (out.GlobalUnitSize.size() > u && (out.GlobalUnitSize[u] > 0 || out.GlobalUnitBuild[u] > 0)) {
                    units << u << "\tglobal\t" << d.Units[u].Include << '\t' << out.GlobalUnitSize[u] << '\t' << out.GlobalUnitBuild[u] << "\t-1\n";
                }
            }
        }
        size_t measured = 0;
        for (const auto& o : observations) { measured += o.Time > 0; }
        if (measured < observations.size()) {
            report << "# measured_actions\t" << measured << '\t' << observations.size() << '\n';
        }
        size_t complete = 0;
        for (size_t t = 0; t < samples.size(); ++t) { complete += samples[t].Count == d.Targets[t].Files.size(); }
        report << "# measured_targets\t" << complete << '\t' << d.Targets.size() << '\n';
        out.Report = report.str();
        out.TargetReport = targets.str();
        out.UnitReport = units.str();
        return out;
    }
} // namespace NPchUpdate::NTwoStep
