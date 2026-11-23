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

        double ArtifactBytes(const TData& d, const TLayout& layout, size_t g, const TCalibration& calibration) {
            const auto& p = layout.Pchs[g];
            if (int(g) < layout.Bases && p.Parent < 0 && g < calibration.MeasuredBases.size()) {
                const auto& [base, bytes] = calibration.MeasuredBases[g];
                if (bytes > 0 && base.Class == p.Class && base.Representative == p.Representative &&
                    base.Roots == p.Roots && base.Units == p.Units) {
                    return bytes;
                }
            }
            const double parent = p.Parent < 0 ? 0 : Volume(d, layout.Pchs[p.Parent].Units);
            return std::max(1.0, (Volume(d, p.Units) - parent) * Coeff(calibration, p.Class).Size);
        }

        double Consumer(const TAction& a, double saved, double load, const TCoefficients& c) {
            double gain = c.Saved * saved - c.Load * load;
            // A combined fit showing a slowdown must not turn extra, unused
            // headers into an apparent speedup outside the measured feature mix.
            if (c.Saved < 0) { gain = std::min(0.0, gain); }
            return std::max(a.Time * 0.02, c.Baseline * a.Time - gain + c.Residual);
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
                  std::vector<double>* densities = nullptr) {
            TPch p;
            p.Class = cls;
            if (files.empty()) { return p; }
            p.Representative = files.front();
            if (base) {
                p.Units = base->Units;
            }
            auto c = Coeff(calibration, cls);
            std::vector<double> mass(d.Units.size());
            for (int f : files) {
                double frequency = d.Q + (1 - d.Q) *
                    double(d.Actions[d.Files[f].Action].Changes.size()) / std::max(1, d.Scenarios);
                for (auto [u, own] : d.Files[f].Own) {
                    mass[u] += own * frequency;
                }
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
                if (!include.empty() && !absolute && !fragment && mass[u] > 0) {
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
            // not only of the new root.
            const double parentParse = base ? Parse(d, base->Units) : 0;
            const double buildScale = base ? c.DeltaBuild : c.BaseBuild;
            double loadSoFar = parentParse;
            auto buildCost = [&](double work, size_t changes) {
                double frequency = d.Q + (1 - d.Q) * double(changes) / std::max(1, d.Scenarios);
                return frequency * std::max(.001, (work - parentParse) * buildScale + parentParse * .02);
            };
            double buildCostSoFar = buildCost(loadSoFar, invalidating.size());
            // Reverse index over the local file list: an evaluation and an
            // acceptance touch only the files that actually parse the closure
            // units of the root at hand, never the whole Own table.
            const int fileCount = int(files.size());
            std::vector<std::vector<std::pair<int, double>>> unitFiles(d.Units.size());
            std::vector<double> savedSoFar(fileCount);
            for (int i = 0; i < fileCount; ++i) {
                double saved = 0;
                for (auto [u, own] : d.Files[files[i]].Own) {
                    unitFiles[u].emplace_back(i, own);
                    if (selected[u]) {
                        saved += own;
                    }
                }
                savedSoFar[i] = saved;
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
            // Full marginal evaluation of one root against the current state.
            struct TEval {
                bool Valid = true;
                double Work = 0, Gain = 0, Bytes = 0, BuildCost = 0;
                std::set<int> Fresh; // commits that would newly invalidate the PCH
            };
            auto artifactBytes = [&](const TEval& e) { return std::max(1.0, c.Size * e.Bytes); };
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
                    e.Valid &= !stable || (d.Units[u].Safe && d.Units[u].Stable);
                    if (!selected[u]) {
                        e.Work += d.Units[u].Parse;
                        e.Gain += c.Saved * mass[u];
                        e.Bytes += d.Units[u].Bytes;
                        for (int k : d.Units[u].Changes) {
                            if (!invalidating.count(k)) {
                                e.Fresh.insert(k);
                            }
                        }
                        for (auto [i, own] : unitFiles[u]) {
                            if (!touched[i]) {
                                touched[i] = 1;
                                touchedFiles.push_back(i);
                            }
                            deltaSaved[i] += own;
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
                e.BuildCost = buildCost(loadSoFar + e.Work, invalidating.size() + e.Fresh.size());
                e.Gain -= e.BuildCost - buildCostSoFar;
                for (int i = 0; i < fileCount; ++i) {
                    const auto& a = d.Actions[d.Files[files[i]].Action];
                    double consumerFrequency = d.Q + (1 - d.Q) * double(a.Changes.size()) / std::max(1, d.Scenarios);
                    e.Gain -= c.Load * e.Work * consumerFrequency;
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
                            Consumer(a, savedSoFar[i] + deltaSaved[i], loadSoFar + e.Work, c);
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
                    densities->push_back(e.Gain / artifactBytes(e));
                }
                const auto& closure = closures.at(root);
                for (int u : closure) {
                    if (!selected[u]) {
                        selected[u] = true;
                        p.Units.push_back(u);
                        for (auto [i, own] : unitFiles[u]) {
                            savedSoFar[i] += own;
                        }
                    }
                }
                invalidating.insert(e.Fresh.begin(), e.Fresh.end());
                loadSoFar += e.Work;
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
            std::map<int, std::pair<double, std::set<int>>> Features;

            const auto& Feature(int g) {
                auto [it, inserted] = Features.try_emplace(g);
                if (inserted && g >= 0) {
                    it->second.first = Parse(D, L.Pchs[g].Units);
                    for (int u : L.Pchs[g].Units) {
                        it->second.second.insert(D.Units[u].Changes.begin(), D.Units[u].Changes.end());
                    }
                }
                return it->second;
            }
            double Target(int t, int g, TConsumerBalance* balance = nullptr) {
                const auto& [load, changes] = Feature(g);
                const auto c = Coeff(C, D.Targets[t].Class);
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
                    double saved = g < 0 ? 0 : Saved(file, L.Pchs[g].Units);
                    double withLoad = Consumer(a, saved, load, c);
                    total += frequency * withLoad;
                    if (balance) {
                        // Apply the same calibrated floor/slowdown rules as the selector.
                        double withoutLoad = Consumer(a, saved, 0, c);
                        balance->ParseSaved += baselineFrequency * (Consumer(a, 0, 0, c) - withoutLoad);
                        balance->Load += baselineFrequency * (withLoad - withoutLoad);
                        balance->ExtraCompiles += (frequency - baselineFrequency) * withLoad;
                    }
                }
                return total;
            }
            double Build(int g) {
                const auto& [work, changes] = Feature(g);
                const int parentId = L.Pchs[g].Parent;
                const double parent = Feature(parentId).first;
                const auto c = Coeff(C, L.Pchs[g].Class);
                return (D.Q + (1 - D.Q) * changes.size() / std::max(1, D.Scenarios)) *
                    std::max(.001, (work - parent) * (parentId < 0 ? c.BaseBuild : c.DeltaBuild) + parent * .02);
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

        // Deltas of one compiler class overlapping a residual profile, most
        // shared mass first: candidates to absorb a target when its own leaf
        // is evicted or rejected, instead of falling all the way to the base
        // or NONE. Never crosses compiler classes: a PCH of another class
        // cannot serve these files.
        std::vector<int> SiblingDeltas(const TLayout& l, const std::map<int, std::vector<int>>& postings,
                                       int cls, const TProfile& profile, int exclude, int limit) {
            std::map<int, double> overlap;
            for (const auto& [u, m] : profile) {
                auto it = postings.find(u);
                if (it == postings.end()) {
                    continue;
                }
                for (int g : it->second) {
                    if (g != exclude && l.Pchs[g].Class == cls) {
                        overlap[g] += m;
                    }
                }
            }
            std::vector<std::pair<double, int>> ranked;
            for (const auto& [g, m] : overlap) {
                if (m > 0) {
                    ranked.emplace_back(m, g);
                }
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
        void CheckPayback(const TData& d, TLayout& l, const TCalibration& c, bool frozen,
                          const std::string& source, int pass, const std::vector<int>& ids,
                          std::vector<std::string>* reasons, std::ostream* report) {
            TWorkCosts costs{d, l, c, {}};
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
                const auto profile = ResidualProfile(d, int(t), maskOf(parent));
                for (int sibling : SiblingDeltas(l, postings, d.Targets[t].Class, profile, g, 3)) {
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
                TConsumerBalance vsNone, fallback;
                for (int t : owners) {
                    vsNone.Add(benefit(t, g));
                    if (const TAlternative* alternative = bestOpen(t, g)) {
                        fallback.Add(alternative->Balance);
                    }
                }
                auto marginal = vsNone;
                marginal.Add(fallback, -1);
                const double build = costs.Build(g);
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
            for (int g = 0; g < l.Bases; ++g) {
                auto& family = groups[g];
                if (family.Owners.empty() && !frozen) { continue; }
                double base = costs.Build(g);
                // Frozen bases are built even under NONE. Report their cost
                // but charge only avoidable builds for the admission decision.
                if (!admit(g, family.VsNone, base + family.ChildBuild, frozen ? base : 0, family.Owners.size())) {
                    for (int t : family.Owners) { assign(t, -1, "family-no-payback"); }
                }
            }
        }

        double Ratio(const std::vector<double>& values, double fallback = 1) {
            if (values.empty()) {
                return fallback;
            }
            auto sorted = values;
            std::sort(sorted.begin(), sorted.end());
            return std::clamp(sorted[sorted.size() / 2], 0.05, 20.0);
        }

        // Three scaled regressors, pivoted elimination with a rank threshold.
        // No ridge term concealing non-identifiability of parse and load.
        bool Solve(double matrix[3][4], double result[3]) {
            double scale[3];
            for (int i = 0; i < 3; ++i) {
                scale[i] = std::sqrt(matrix[i][i]);
                if (scale[i] < 1e-9) {
                    return false;
                }
            }
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    matrix[i][j] /= scale[i] * scale[j];
                }
                matrix[i][3] /= scale[i];
            }
            for (int i = 0; i < 3; ++i) {
                int pivot = i;
                for (int j = i + 1; j < 3; ++j) {
                    if (std::abs(matrix[j][i]) > std::abs(matrix[pivot][i])) {
                        pivot = j;
                    }
                }
                if (std::abs(matrix[pivot][i]) < 1e-5) {
                    return false;
                }
                for (int j = 0; j < 4; ++j) {
                    std::swap(matrix[i][j], matrix[pivot][j]);
                }
                double div = matrix[i][i];
                for (int j = i; j < 4; ++j) {
                    matrix[i][j] /= div;
                }
                for (int row = 0; row < 3; ++row) {
                    if (row != i) {
                        double mul = matrix[row][i];
                        for (int j = i; j < 4; ++j) {
                            matrix[row][j] -= mul * matrix[i][j];
                        }
                    }
                }
            }
            for (int i = 0; i < 3; ++i) {
                result[i] = matrix[i][3] / scale[i];
            }
            return true;
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

    static double MakespanPrepared(const std::vector<TAction>& actions, const TResources& r,
                                   const std::vector<char>& dirty, const TTopology& topology) {
        if (r.Workers < 1 || r.Ram <= 0 || r.Bandwidth <= 0 || (!dirty.empty() && dirty.size() != actions.size())) {
            Fail("scheduler: invalid resource limits or dirty mask");
        }
        const auto& users = topology.Users;
        const auto& order = topology.Order;
        std::vector<double> time(actions.size()), critical(actions.size()), io(actions.size());
        double minimumMemory = INF;
        std::vector<int> pending(actions.size());
        for (int i : order) {
            const auto& a = actions[i];
            bool active = dirty.empty() || dirty[i];
            time[i] = active && !a.Phony ? std::max(a.Time, a.Io / r.Bandwidth) : 0;
            io[i] = time[i] > 0 ? a.Io / time[i] : 0;
            if (!std::isfinite(time[i]) || time[i] < 0 || a.Memory < 0) {
                Fail("scheduler: non-finite or negative cost");
            }
            if (time[i] > 0 && a.Memory > r.Ram) {
                return INF;
            }
            if (time[i] > 0) {
                minimumMemory = std::min(minimumMemory, a.Memory);
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
        double now = 0, memory = 0, bandwidth = 0;
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
                const auto& a = actions[i];
                if (time[i] == 0) {
                    ready.erase(it);
                    complete(i);
                    it = ready.begin();
                    continue;
                }
                if (running.size() >= size_t(r.Workers) || memory + minimumMemory > r.Ram) {
                    break;
                }
                auto pool = r.Pools.find(a.Pool);
                if (!a.Pool.empty() && pool == r.Pools.end()) {
                    Fail("scheduler: unknown pool " + a.Pool);
                }
                bool fits = running.size() < size_t(r.Workers) && memory + a.Memory <= r.Ram &&
                    bandwidth + io[i] <= r.Bandwidth * (1 + 1e-9) &&
                    (pool == r.Pools.end() || poolUse[a.Pool] < pool->second);
                if (!fits) {
                    ++it;
                    continue;
                }
                it = ready.erase(it);
                memory += a.Memory;
                bandwidth += io[i];
                ++poolUse[a.Pool];
                running.emplace(now + time[i], i);
            }
            if (running.empty()) {
                return finished == actions.size() ? now : INF;
            }
            now = running.top().first;
            do {
                int i = running.top().second;
                running.pop();
                memory -= actions[i].Memory;
                bandwidth -= io[i];
                --poolUse[actions[i].Pool];
                complete(i);
            } while (!running.empty() && running.top().first <= now);
            memory = std::max(0.0, memory);
            bandwidth = std::max(0.0, bandwidth);
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

    std::vector<int> CompactLayout(TLayout& layout, bool frozen) {
        std::vector<char> keep(layout.Pchs.size());
        if (frozen) { std::fill(keep.begin(), keep.begin() + layout.Bases, true); }
        for (int g : layout.Assignment) {
            for (; g >= 0 && !keep[g]; g = layout.Pchs[g].Parent) { keep[g] = true; }
        }
        std::vector<int> remap(keep.size(), -1);
        std::vector<TPch> pchs;
        int bases = 0;
        for (size_t g = 0; g < keep.size(); ++g) {
            if (!keep[g]) { continue; }
            auto pch = std::move(layout.Pchs[g]);
            remap[g] = int(pchs.size());
            if (pch.Parent >= 0) { pch.Parent = remap[pch.Parent]; }
            pchs.push_back(std::move(pch));
            bases += int(g) < layout.Bases;
        }
        for (int& g : layout.Assignment) { if (g >= 0) { g = remap[g]; } }
        layout.Pchs = std::move(pchs);
        layout.Bases = bases;
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
    // evictions close alternatives and move owners around.
    void FitDisk(const TData& d, TLayout& l, const TCalibration& c,
                 const std::vector<uint64_t>& bytes, double budget, bool frozen,
                 const std::function<bool(int, int)>& compatible, std::ostream* report) {
        if (bytes.size() != l.Pchs.size()) { Fail("missing PCH artifact sizes"); }
        std::vector<std::vector<int>> owners(bytes.size());
        std::vector<std::vector<int>> children(l.Bases);
        std::vector<char> used(bytes.size());
        std::vector<char> open(bytes.size(), false);
        if (frozen) { std::fill_n(used.begin(), l.Bases, true); }
        for (size_t t = 0; t < l.Assignment.size(); ++t) {
            int g = l.Assignment[t];
            if (g < 0) { continue; }
            owners.at(g).push_back(int(t));
            used[g] = true;
            open[g] = true;
            if (g >= l.Bases) { used.at(l.Pchs[g].Parent) = true; }
        }
        for (int b = 0; b < l.Bases; ++b) { open[b] = true; }
        uint64_t total = 0, bases = 0;
        std::vector<uint64_t> freed = bytes;
        for (size_t g = 0; g < bytes.size(); ++g) {
            if (!used[g]) { continue; }
            if (!bytes[g]) { Fail("no size for retained PCH " + std::to_string(g)); }
            total += bytes[g];
            if (int(g) < l.Bases) { bases += bytes[g]; }
            else {
                children.at(l.Pchs[g].Parent).push_back(int(g));
                freed[l.Pchs[g].Parent] += bytes[g];
            }
        }
        if (frozen && bases > budget) {
            Fail(Format("frozen bases alone require %.0f bytes; disk budget %.0f bytes", double(bases), budget));
        }
        if (report) {
            *report << "# measured preflight PCH bytes; IDs before compaction; weighted action seconds, not wall time\n"
                    << "# candidate_bytes=" << total << " base_bytes=" << bases << " budget_bytes=" << uint64_t(budget)
                    << "\nkind\tgroup\tbytes\tloss_work_seconds\tto_fallback\tto_none\n";
        }
        TWorkCosts costs{d, l, c, {}};
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
            for (int sibling : SiblingDeltas(l, postings, l.Pchs[g].Class, profile, g, 3)) {
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
            if (!used[g] || (frozen && g < l.Bases)) { continue; }
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
                                const std::vector<uint64_t>& bytes, bool frozen,
                                const std::function<bool(int, int)>& compatible) {
        std::ostringstream report;
        FitDisk(d, l, c, bytes, d.Resources.Disk, frozen, compatible, &report);
        return report.str();
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
        double disk = 0;
        int offset = int(d.Actions.size());
        for (size_t g = 0; g < l.Pchs.size(); ++g) {
            const auto& h = l.Pchs[g];
            if (h.Parent >= int(g)) {
                Fail("prediction: PCH parent must precede delta");
            }
            auto c = Coeff(calibration, h.Class);
            double total = Parse(d, h.Units);
            double parent = h.Parent >= 0 ? Parse(d, l.Pchs[h.Parent].Units) : 0;
            TAction a;
            a.Output = "pch/" + std::to_string(g) + ".pch";
            // Consumer load fits do not identify the compiler's PCH-to-PCH load
            // cost. Keep that prior separate, including in combined-only fits.
            a.Time = std::max(0.001, (total - parent) * (h.Parent < 0 ? c.BaseBuild : c.DeltaBuild) + parent * 0.02);
            double bytes = measuredBytes.empty() ? ArtifactBytes(d, l, g, calibration) : measuredBytes[g];
            p.PchBytes.push_back(bytes);
            a.Io = bytes;
            if (h.Parent >= 0) {
                a.Dependencies.push_back(offset + h.Parent);
                a.Io += p.PchBytes[h.Parent];
            }
            a.Memory = (d.ActionMemory + a.Io) * c.Memory;
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
                p.Saved[f] = Saved(file, h.Units);
                p.Load[f] = Parse(d, h.Units);
                a.Dependencies.push_back(offset + g);
                a.DirtyDependencies.push_back(offset + g);
                a.Io = p.PchBytes[g] + (h.Parent >= 0 ? p.PchBytes[h.Parent] : 0);
                a.Memory = (d.ActionMemory + a.Io) * c.ConsumerMemory;
            }
            a.Time = Consumer(d.Actions[file.Action], p.Saved[f], p.Load[f], c);
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

    TPrediction ScoreCandidate(const TData& d, TLayout& layout, const TCalibration& c, size_t jobs, int width) {
        // Keep all artifacts (including frozen bases), roots, class IDs and
        // parents fixed. Only assignments on consumer -> PCH edges may move.
        for (;;) {
            try {
                return Predict(d, layout, c, jobs);
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
                for (int t : targets) {
                    const int old = layout.Assignment[t];
                    for (int g = layout.Pchs[old].Parent;; g = layout.Pchs[g].Parent) {
                        auto trial = layout;
                        trial.Assignment[t] = g;
                        try {
                            auto p = Predict(d, trial, c, jobs);
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
                    // Several cycles may preclude scoring any single move.
                    // Drop one witnessed edge with the least estimated consumer
                    // benefit, then diagnose the remaining graph. Each such
                    // step removes one assignment permanently, so it terminates.
                    const auto& leaf = layout.Pchs[old];
                    const auto coeff = Coeff(c, leaf.Class);
                    const double load = Parse(d, leaf.Units);
                    double loss = 0;
                    for (int i : d.Targets[t].Files) {
                        const auto& f = d.Files[i];
                        const auto& a = d.Actions[f.Action];
                        const double freq = d.Q + (1 - d.Q) * a.Changes.size() / std::max(1, d.Scenarios);
                        loss += freq * (Consumer(a, 0, 0, coeff) - Consumer(a, Saved(f, leaf.Units), load, coeff));
                    }
                    if (provisional < 0 || loss < cheapestLoss) { provisional = t; cheapestLoss = loss; }
                }
                const int t = bestTarget >= 0 ? bestTarget : provisional;
                const int g = bestTarget >= 0 ? bestGroup : -1;
                Log(Format("selection width=%d: cycle repair %s PCH %d -> %d (%s)", width,
                           d.Targets[t].Label.c_str(), layout.Assignment[t], g,
                           bestTarget >= 0 ? "best acyclic local move" : "multiple cycles; least estimated loss on witnessed cycle"));
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
        std::vector<double> baseDensities;
        if (frozen) {
            bases.Bases = frozen->Bases;
            bases.Pchs.assign(frozen->Pchs.begin(), frozen->Pchs.begin() + frozen->Bases);
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
        }
        // Recheck if cycle repair demotes an owner: the proof for the old
        // consumer set no longer holds. Keep report IDs in the candidate's namespace.
        auto scoreAssigned = [&](TLayout& l, const std::string& source, int width, std::vector<std::string>& reasons, unsigned factor) {
            std::vector<int> ids(l.Pchs.size());
            std::iota(ids.begin(), ids.end(), 0);
            std::ostringstream payback;
            payback.precision(17);
            for (int pass = 1;; ++pass) {
                CheckPayback(d, l, c, frozen != nullptr, source, pass, ids, diagnostics ? &reasons : nullptr,
                             diagnostics && factor == 1 ? &payback : nullptr);
                std::vector<uint64_t> estimates;
                for (size_t g = 0; g < l.Pchs.size(); ++g) {
                    estimates.push_back(uint64_t(std::ceil(ArtifactBytes(d, l, g, c))));
                }
                const auto beforeFit = l.Assignment;
                FitDisk(d, l, c, estimates, factor * d.Resources.Disk, frozen != nullptr, {}, nullptr);
                if (diagnostics) {
                    for (size_t t = 0; t < reasons.size(); ++t) {
                        if (l.Assignment[t] != beforeFit[t]) {
                            reasons[t] = l.Assignment[t] < 0 ? "disk-budget" : "disk-fallback";
                        }
                    }
                }
                const auto remap = CompactLayout(l, frozen != nullptr);
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
        double score = scoreAssigned(best, "seed", -1, seedReasons, 1).Objective;
        const std::vector<int> widths = {0, 32, 128, 512};
        struct TCandidate {
            TLayout Layout;
            std::vector<std::string> Reasons;
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
                             std::vector<double>* densities) {
            TLayout candidate;
            if (frozen || spec.Lambda == 0) {
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
                // Expectation: every target takes the cheapest of NONE, the
                // base and the deltas whose roots overlap its profile. Only
                // the final round records diagnostics: an earlier round's
                // delta IDs die in the next maximization, and the reports must
                // match selection-candidates.tsv.
                auto expect = [&](bool record) {
                    TWorkCosts costs{d, candidate, c, {}};
                    const auto postings = UnitPostings(candidate);
                    for (size_t i = 0; i < targets.size(); ++i) {
                        int t = targets[i];
                        std::vector<int> choices{parent};
                        for (int g : SiblingDeltas(candidate, postings, cls, profiles[i], -1, 3)) {
                            if (candidate.Pchs[g].Class == cls && std::find(choices.begin(), choices.end(), g) == choices.end()) {
                                choices.push_back(g);
                            }
                        }
                        double cost = costs.Target(t, -1);
                        const double withoutPch = cost;
                        const bool allowed = frozen || t % 10 != 0;
                        candidate.Assignment[t] = -1;
                        for (int g : choices) {
                            double value = costs.Target(t, g);
                            if (diagnostics && record) {
                                choicesReport << label << '\t' << d.Targets[t].Label << '\t' << g << '\t'
                                    << withoutPch << '\t' << value << '\t' << (value < cost && allowed) << '\n';
                            }
                            if (value < cost && allowed) {
                                cost = value;
                                candidate.Assignment[t] = g;
                            }
                        }
                        if (diagnostics && record) {
                            observed.Reasons[t] = !frozen && t % 10 == 0 ? "calibration-control" :
                                candidate.Assignment[t] >= 0 ? "selected" : choices.empty() ? "no-candidate" : "no-estimated-benefit";
                        }
                    }
                };
                // Maximization: rebuild each delta strictly from the files
                // currently on it; deltas with no members disappear.
                auto maximize = [&](bool fromClusters) {
                    std::vector<std::vector<int>> buckets;
                    if (fromClusters) {
                        buckets.assign(clusters, {});
                        for (size_t i = 0; i < targets.size(); ++i) {
                            if (clusterOf[i] >= 0) {
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
                        }
                    }
                    candidate.Pchs.resize(classStart);
                    for (const auto& files : buckets) {
                        if (files.empty()) { continue; }
                        auto p = Pick(d, c, cls, files, &candidate.Pchs[parent], false, candidateClosures, spec.Lambda, densities);
                        if (!p.Roots.empty()) {
                            p.Parent = parent;
                            candidate.Pchs.push_back(std::move(p));
                        }
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
        auto estimatedBytes = [&](const TLayout& l) {
            double bytes = 0;
            for (size_t g = 0; g < l.Pchs.size(); ++g) { bytes += ArtifactBytes(d, l, g, c); }
            return bytes;
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
                diagnostics->Widths += label + Format("\t%d\t%zu\t%.17g\t%.17g\t%d\n", candidate.Bases, candidate.Pchs.size(),
                    std::accumulate(prediction.PchBytes.begin(), prediction.PchBytes.end(), 0.0), value, int(prediction.Feasible));
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
            std::vector<std::vector<double>> densities(specs.size());
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
            auto personalGrid = [&](size_t i) {
                std::vector<double> pool = densities[i];
                if (!frozen) {
                    // The bases are rebuilt under the same price in wave two.
                    pool.insert(pool.end(), baseDensities.begin(), baseDensities.end());
                }
                std::sort(pool.begin(), pool.end(), std::greater<double>());
                std::vector<double> grid;
                for (double quantile : {0.4, 0.7, 0.9}) {
                    if (pool.empty()) { break; }
                    // A quantile snapped to an accepted root's own density must
                    // not reject that root by the strict admission inequality.
                    double lambda = pool[std::min(pool.size() - 1, size_t(double(pool.size()) * quantile))];
                    if (lambda > 0) {
                        lambda *= 1 - 1e-9;
                    }
                    if (lambda > 0 && (grid.empty() || lambda > grid.back())) {
                        grid.push_back(lambda);
                    }
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
            Fail("no layout fits the worker/pool/RAM/disk constraints");
        }
        const auto primaryGroups = best.Pchs.size();
        const double scoredBytes = estimatedBytes(best);
        if (selectedSpec >= 0 || frozen) {
            auto reserve = selectedSpec >= 0 ? candidates[selectedSpec].Layout : *frozen;
            auto reasons = selectedSpec >= 0 ? candidates[selectedSpec].Reasons : std::vector<std::string>(d.Targets.size());
            if (scoreAssigned(reserve, selectedSpec >= 0 ? specLabel(specs[selectedSpec]) : "frozen",
                              selectedWidth, reasons, ProbeBudgetFactor).Feasible) {
                best = std::move(reserve);
                seedReasons = std::move(reasons);
            }
        }
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
                "\nwidth_lambda\tbases\tpchs\tpch_bytes\tobjective_seconds\tfeasible\n" + diagnostics->Widths;
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

    TCalibration Calibrate(const TData& d, const TLayout& l, const TPrediction& p,
                           const std::vector<TObservation>& observations) {
        ValidateTargets(d);
        if (l.Assignment.size() != d.Targets.size()) { Fail("calibration: assignment size mismatch"); }
        if (observations.size() != p.Actions.size()) {
            Fail("calibration: incomplete observations");
        }
        TCalibration out;
        for (int g = 0; g < l.Bases; ++g) {
            const auto& obs = observations.at(d.Actions.size() + g);
            out.MeasuredBases.emplace_back(l.Pchs.at(g), obs.Time > 0 ? obs.Bytes : 0.0);
        }
        out.Classes.resize(d.Classes);
        for (size_t i = 0; i < d.Actions.size(); ++i) {
            // A sampled verification leaves unmeasured actions at zero: keep
            // the model's own estimate for them instead of a zero time.
            const bool measured = observations[i].Time > 0;
            out.ActionTime.push_back(measured ? observations[i].Time : p.Actions[i].Time);
            out.ActionMemory.push_back(measured ? observations[i].Memory : p.Actions[i].Memory);
        }
        std::ostringstream report;
        report << "class\tstatus\ttraining_targets\tholdout_targets\tholdout_target_MAE_s\tbase_build\tdelta_build\tsize\tmemory\n";
        struct TTargetObservation {
            size_t Count = 0;
            double Baseline = 0, Time = 0, Saved = 0, Load = 0, Memory = 0;
        };
        std::vector<TTargetObservation> targets(d.Targets.size());
        for (size_t t = 0; t < d.Targets.size(); ++t) {
            auto& target = targets[t];
            for (int f : d.Targets[t].Files) {
                const auto& obs = observations[d.Files[f].Action];
                if (obs.Time <= 0) { continue; }
                ++target.Count;
                target.Baseline += d.Actions[d.Files[f].Action].Time;
                target.Time += obs.Time;
                target.Saved += p.Saved[f];
                target.Load += p.Load[f];
                target.Memory += obs.Memory;
            }
        }
        for (int cls = 0; cls < d.Classes; ++cls) {
            auto& c = out.Classes[cls];
            std::vector<int> groups;
            for (size_t g = 0; g < l.Pchs.size(); ++g) {
                if (l.Pchs[g].Class == cls) {
                    groups.push_back(int(g));
                }
            }
            // Entire leaf groups are held out, including their artifact metrics.
            // Their parents stay in the artifact fits: those fits never predict
            // holdout consumer time (the holdout error uses Consumer only),
            // while each class has a single base (groups[0]); excluding it
            // would leave BaseBuild and Size unmeasured for every class with
            // at least two deltas.
            std::set<int> held;
            if (groups.size() >= 3) {
                for (size_t i = 2; i < groups.size(); i += 3) {
                    held.insert(groups[i]);
                }
            }
            std::vector<double> controls, baseBuild, deltaBuild, memory, consumerMemory, combined;
            double sizeNumerator = 0, sizeDenominator = 0;
            for (size_t t = 0; t < d.Targets.size(); ++t) {
                if (d.Targets[t].Class == cls && l.Assignment[t] < 0) {
                    const auto& target = targets[t];
                    if (target.Count && target.Baseline > 0) {
                        controls.push_back(target.Time / target.Baseline);
                    }
                }
            }
            c.Baseline = Ratio(controls);
            for (int g : groups) {
                if (held.count(g)) {
                    continue;
                }
                const auto& obs = observations[d.Actions.size() + g];
                if (obs.Time <= 0) { continue; } // not measured by a sampled run
                double total = Parse(d, l.Pchs[g].Units);
                int parent = l.Pchs[g].Parent;
                double parentParse = parent >= 0 ? Parse(d, l.Pchs[parent].Units) : 0;
                double own = total - parentParse;
                double volume = Volume(d, l.Pchs[g].Units);
                double ownVolume = volume - (parent >= 0 ? Volume(d, l.Pchs[parent].Units) : 0);
                if (own > 0) {
                    (parent >= 0 ? deltaBuild : baseBuild).push_back(std::max(0.001, obs.Time - parentParse * c.Load) / own);
                }
                // Artifact bytes add up over the layout, so the ratio is fitted
                // by total bytes instead of a per-artifact median.
                if (ownVolume > 0) {
                    sizeNumerator += obs.Bytes * ownVolume;
                    sizeDenominator += ownVolume * ownVolume;
                }
                memory.push_back(obs.Memory / (d.ActionMemory + volume));
            }
            c.BaseBuild = Ratio(baseBuild);
            c.DeltaBuild = Ratio(deltaBuild);
            if (sizeDenominator > 0) {
                c.Size = std::clamp(sizeNumerator / sizeDenominator, 0.05, 20.0);
            }
            c.Memory = Ratio(memory);
            double matrix[3][4] = {};
            std::set<int> training;
            for (size_t t = 0; t < d.Targets.size(); ++t) {
                int g = l.Assignment[t];
                const auto& target = targets[t];
                if (d.Targets[t].Class != cls || g < 0 || held.count(g) || !target.Count) {
                    continue;
                }
                training.insert(int(t));
                double bytes = p.PchBytes[g];
                if (l.Pchs[g].Parent >= 0) { bytes += p.PchBytes[l.Pchs[g].Parent]; }
                const double count = target.Count;
                consumerMemory.push_back(target.Memory / (count * (d.ActionMemory + bytes)));
                double y = target.Time - c.Baseline * target.Baseline;
                // Residual remains per compiler action in Predict; the target
                // regression therefore uses member count, not a constant one.
                double x[3] = {-target.Saved, target.Load, count};
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        matrix[i][j] += x[i] * x[j];
                    }
                    matrix[i][3] += x[i] * y;
                }
                double gain = target.Saved - 0.02 * target.Load;
                if (gain > 0.001) {
                    combined.push_back(-y / gain);
                }
            }
            double solution[3];
            c.Identified = training.size() >= 3 && controls.size() >= 3 && Solve(matrix, solution) &&
                solution[0] >= 0 && solution[0] <= 3 && solution[1] >= 0 && solution[1] <= 3;
            if (c.Identified) {
                // Small-data shrinkage toward the prior, after rank testing.
                double weight = double(training.size()) / (training.size() + 2);
                c.Saved = 1 + weight * (solution[0] - 1);
                c.Load = 0.02 + weight * (solution[1] - 0.02);
                c.Residual = weight * solution[2];
            } else if (!combined.empty()) {
                // One observable combined saving, not separate load/parse claims.
                std::sort(combined.begin(), combined.end());
                double scale = std::clamp(combined[combined.size() / 2], -3.0, 3.0);
                c.Saved = scale;
                c.Load = 0.02 * scale;
            }
            c.ConsumerMemory = Ratio(consumerMemory);
            double error = 0;
            size_t n = 0;
            for (size_t t = 0; t < d.Targets.size(); ++t) {
                if (d.Targets[t].Class == cls && held.count(l.Assignment[t]) && targets[t].Count == d.Targets[t].Files.size()) {
                    double value = 0;
                    for (int f : d.Targets[t].Files) {
                        value += Consumer(d.Actions[d.Files[f].Action], p.Saved[f], p.Load[f], c);
                    }
                    error += std::abs(value - targets[t].Time);
                    ++n;
                }
            }
            report << cls << '\t' << (c.Identified ? "identified" : combined.empty() ? "prior-only" : "combined-only")
                   << '\t' << training.size() << '\t' << n << '\t';
            if (n) {
                report << error / n;
            } else {
                report << "unavailable";
            }
            report << '\t' << (baseBuild.empty() ? "unmeasured" : std::to_string(c.BaseBuild))
                   << '\t' << (deltaBuild.empty() ? "unmeasured" : std::to_string(c.DeltaBuild))
                   << '\t' << (sizeDenominator > 0 ? std::to_string(c.Size) : "unmeasured")
                   << '\t' << (memory.empty() ? "unmeasured" : std::to_string(c.Memory)) << '\n';
        }
        size_t measured = 0;
        for (const auto& o : observations) { measured += o.Time > 0; }
        if (measured < observations.size()) {
            report << "# measured_actions\t" << measured << '\t' << observations.size() << '\n';
        }
        size_t complete = 0;
        for (size_t t = 0; t < targets.size(); ++t) { complete += targets[t].Count == d.Targets[t].Files.size(); }
        report << "# measured_targets\t" << complete << '\t' << d.Targets.size() << '\n';
        out.Report = report.str();
        return out;
    }
} // namespace NPchUpdate::NTwoStep
