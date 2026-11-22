#include "two_step.h"
#include "common.h"

#include <algorithm>
#include <cmath>
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
        TPch Pick(const TData& d, const TCalibration& calibration, int cls,
                  const std::vector<int>& files, const TPch* base, bool stable,
                  std::map<int, std::vector<int>>& closures) {
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
            std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b) {
                return mass[a] > mass[b];
            });
            // TODO: investigate and document why the former 128-candidate
            // limit was chosen, including measured selection cost and quality.
            // Leave the search uncapped until that limit can be justified.
            std::vector<char> selected(d.Units.size());
            for (int u : p.Units) {
                selected[u] = true;
            }
            for (int root : candidates) {
                if (selected[root]) {
                    continue;
                }
                auto [cached, added] = closures.try_emplace(root);
                if (added) {
                    cached->second = Closure(d, {root});
                }
                const auto& closure = cached->second;
                bool valid = true;
                double work = 0, gain = 0;
                std::set<int> changes;
                for (int u : closure) {
                    // Shared bases stay conservative. Specialized leaves may
                    // require libraries: Preflight checks the actual PCH against
                    // every final link of each assigned owner before accepting it.
                    valid &= !stable || (d.Units[u].Safe && d.Units[u].Stable);
                    if (!selected[u]) {
                        work += d.Units[u].Parse;
                        gain += c.Saved * mass[u];
                        changes.insert(d.Units[u].Changes.begin(), d.Units[u].Changes.end());
                    }
                }
                if (!valid) {
                    continue;
                }
                double frequency = d.Q + (1 - d.Q) * double(changes.size()) / std::max(1, d.Scenarios);
                gain -= work * (base ? c.DeltaBuild : c.BaseBuild) * frequency;
                for (int f : files) {
                    const auto& a = d.Actions[d.Files[f].Action];
                    double consumerFrequency = d.Q + (1 - d.Q) * double(a.Changes.size()) / std::max(1, d.Scenarios);
                    gain -= c.Load * work * consumerFrequency;
                    for (int k : changes) {
                        if (!std::binary_search(a.Changes.begin(), a.Changes.end(), k)) {
                            gain -= (1 - d.Q) * a.Time / std::max(1, d.Scenarios);
                        }
                    }
                }
                if (gain > 0) {
                    p.Roots.push_back(root);
                    for (int u : closure) {
                        if (!selected[u]) {
                            selected[u] = true;
                            p.Units.push_back(u);
                        }
                    }
                }
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

        // Price a delta against each assigned owner's better fallback. Only
        // surviving children contribute to the subsequent family-vs-NONE test.
        void CheckPayback(const TData& d, TLayout& l, const TCalibration& c, bool frozen,
                          int width, int pass, const std::vector<int>& ids,
                          std::vector<std::string>* reasons, std::ostream* report) {
            TWorkCosts costs{d, l, c, {}};
            struct TGroup {
                std::vector<int> Owners;
                TConsumerBalance Relative, VsNone, Fallback;
                double ChildBuild = 0;
            };
            std::vector<TGroup> groups(l.Pchs.size());
            std::vector<int> fallback(d.Targets.size(), -1);
            for (size_t t = 0; t < l.Assignment.size(); ++t) {
                int g = l.Assignment[t];
                if (g < 0) { continue; }
                auto& group = groups[g];
                group.Owners.push_back(t);
                TConsumerBalance benefit, base;
                costs.Target(t, g, &benefit);
                group.VsNone.Add(benefit);
                int parent = l.Pchs[g].Parent;
                if (parent >= 0) {
                    costs.Target(t, parent, &base);
                    if (base.Net() > 0) {
                        fallback[t] = parent;
                        group.Fallback.Add(base);
                    }
                }
            }
            auto admit = [&](int g, const TConsumerBalance& benefit, double build, double mandatory) {
                double net = benefit.Net() - (build - mandatory);
                if (report) {
                    *report << width << '\t' << pass << '\t' << (g < l.Bases ? "family" : "delta") << '\t'
                            << ids[g] << '\t' << groups[g].Owners.size() << '\t' << benefit.ParseSaved << '\t'
                            << benefit.Load << '\t' << benefit.ExtraCompiles << '\t' << build << '\t' << mandatory
                            << '\t' << net << '\t' << (net > 0 ? "keep" : "drop-assignments") << '\n';
                }
                return net > 0;
            };
            auto assign = [&](int t, int g, const char* reason) {
                l.Assignment[t] = g;
                if (reasons) { reasons->at(t) = reason; }
            };
            for (int g = l.Bases; g < int(groups.size()); ++g) {
                auto& child = groups[g];
                if (child.Owners.empty()) { continue; }
                int parent = l.Pchs[g].Parent;
                auto& family = groups.at(parent);
                auto marginal = child.VsNone;
                marginal.Add(child.Fallback, -1);
                const double build = costs.Build(g);
                if (admit(g, marginal, build, 0)) {
                    family.ChildBuild += build;
                    family.VsNone.Add(child.VsNone);
                    family.Owners.insert(family.Owners.end(), child.Owners.begin(), child.Owners.end());
                } else {
                    family.VsNone.Add(child.Fallback);
                    for (int t : child.Owners) {
                        assign(t, fallback[t], fallback[t] < 0 ? "delta-payback-none" : "delta-payback-parent");
                        if (fallback[t] >= 0) { family.Owners.push_back(t); }
                    }
                }
            }
            for (int g = 0; g < l.Bases; ++g) {
                auto& family = groups[g];
                if (family.Owners.empty() && !frozen) { continue; }
                double base = costs.Build(g);
                // Frozen bases are built even under NONE. Report their cost
                // but charge only avoidable builds for the admission decision.
                if (!admit(g, family.VsNone, base + family.ChildBuild, frozen ? base : 0)) {
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
    void FitDisk(const TData& d, TLayout& l, const TCalibration& c,
                 const std::vector<uint64_t>& bytes, double budget, bool frozen,
                 const std::function<bool(int, int)>& compatible, std::ostream* report) {
        if (bytes.size() != l.Pchs.size()) { Fail("missing PCH artifact sizes"); }
        std::vector<std::vector<int>> owners(bytes.size());
        std::vector<std::vector<int>> children(l.Bases);
        std::vector<char> used(bytes.size());
        if (frozen) { std::fill_n(used.begin(), l.Bases, true); }
        for (size_t t = 0; t < l.Assignment.size(); ++t) {
            int g = l.Assignment[t];
            if (g < 0) { continue; }
            owners.at(g).push_back(t);
            used[g] = true;
            if (g >= l.Bases) { used.at(l.Pchs[g].Parent) = true; }
        }
        uint64_t total = 0, bases = 0;
        for (size_t g = 0; g < bytes.size(); ++g) {
            if (!used[g]) { continue; }
            if (!bytes[g]) { Fail("no size for retained PCH " + std::to_string(g)); }
            total += bytes[g];
            if (int(g) < l.Bases) { bases += bytes[g]; }
            else { children.at(l.Pchs[g].Parent).push_back(int(g)); }
        }
        if (frozen && bases > budget) {
            Fail(Format("frozen bases alone require %.0f bytes; disk budget %.0f bytes", double(bases), budget));
        }
        if (report) {
            *report << "# measured preflight PCH bytes; IDs before compaction; weighted action seconds, not wall time\n"
                    << "# candidate_bytes=" << total << " base_bytes=" << bases << " budget_bytes=" << uint64_t(budget)
                    << "\nkind\tgroup\tbytes\tloss_work_seconds\tto_parent\tto_none\n";
        }
        TWorkCosts costs{d, l, c, {}};
        std::vector<int> fallback(l.Assignment.size(), -1);
        std::vector<double> fallbackGain(l.Assignment.size()), loss(bytes.size());
        std::vector<uint64_t> freed = bytes;
        std::vector<char> verified(bytes.size(), !compatible);
        using TEntry = std::pair<double, int>;
        std::set<TEntry> order;
        auto entry = [&](int g) -> TEntry { return {loss[g] / freed[g], g}; };
        for (int g = 0; g < int(bytes.size()); ++g) {
            if (!used[g]) { continue; }
            const int parent = l.Pchs[g].Parent;
            double vsNone = -costs.Build(g), vsParent = vsNone;
            for (int t : owners[g]) {
                double none = costs.Target(t, -1), current = costs.Target(t, g);
                vsNone += none - current;
                if (parent >= 0) {
                    fallbackGain[t] = std::max(0., none - costs.Target(t, parent));
                    if (fallbackGain[t] > 0) { fallback[t] = parent; }
                    vsParent += none - current - fallbackGain[t];
                }
            }
            if (parent < 0) { loss[g] = vsNone; }
            else {
                loss[g] = vsParent;
                loss[parent] += vsNone;
                freed[parent] += bytes[g];
            }
        }
        for (int g = 0; g < int(bytes.size()); ++g) {
            if (used[g] && (!frozen || g >= l.Bases)) { order.insert(entry(g)); }
        }
        while (!order.empty()) {
            auto [value, g] = *order.begin();
            if (total <= budget && value > 0) { break; }
            order.erase(order.begin());
            const int parent = l.Pchs[g].Parent;
            if (parent >= 0 && !verified[g]) {
                // A failed probe only increases the leaf eviction loss. Its
                // family's alternative is NONE and is independent of this probe.
                verified[g] = true;
                for (int t : owners[g]) {
                    if (fallback[t] >= 0 && !compatible(t, parent)) {
                        fallback[t] = -1;
                        loss[g] += fallbackGain[t];
                    }
                }
                order.insert(entry(g));
                continue;
            }
            int parentCount = 0;
            size_t noneCount = 0;
            if (parent < 0) {
                auto drop = [&](int h) {
                    for (int t : owners[h]) { l.Assignment[t] = -1; ++noneCount; }
                    used[h] = false;
                };
                for (int h : children[g]) {
                    if (used[h]) { order.erase(entry(h)); drop(h); }
                }
                drop(g);
            } else {
                if (!frozen) { order.erase(entry(parent)); }
                for (int t : owners[g]) {
                    l.Assignment[t] = fallback[t];
                    if (fallback[t] >= 0) { ++parentCount; owners[parent].push_back(t); }
                    else { ++noneCount; }
                }
                used[g] = false;
                freed[parent] -= bytes[g];
                loss[parent] -= loss[g];
                if (!frozen) { order.insert(entry(parent)); }
            }
            total -= freed[g];
            if (report) {
                *report << (parent < 0 ? "family" : "delta") << '\t' << g << '\t' << freed[g] << '\t'
                        << loss[g] << '\t' << parentCount << '\t' << noneCount << '\n';
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
        int selectedWidth = -1;
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
        if (frozen) {
            bases.Bases = frozen->Bases;
            bases.Pchs.assign(frozen->Pchs.begin(), frozen->Pchs.begin() + frozen->Bases);
        } else {
            for (int cls = 0; cls < d.Classes; ++cls) {
                auto p = Pick(d, c, cls, filesOf(classes[cls]), nullptr, true, closures);
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
        // consumer set no longer holds. Keep report IDs in the width's namespace.
        auto scoreAssigned = [&](TLayout& l, int width, std::vector<std::string>& reasons, unsigned factor) {
            std::vector<int> ids(l.Pchs.size());
            std::iota(ids.begin(), ids.end(), 0);
            std::ostringstream payback;
            payback.precision(17);
            for (int pass = 1;; ++pass) {
                CheckPayback(d, l, c, frozen != nullptr, width, pass, ids, diagnostics ? &reasons : nullptr,
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
                            reasons[t] = l.Assignment[t] < 0 ? "disk-budget" : "disk-parent";
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
        double score = scoreAssigned(best, -1, seedReasons, 1).Objective;
        const std::vector<int> widths = {0, 32, 128, 512};
        struct TCandidate {
            TLayout Layout;
            std::vector<std::string> Reasons;
            std::vector<int> BeforeDisk;
            std::string Choices;
            std::string Pchs;
        };
        auto describePchs = [&](int width, const TLayout& layout) {
            std::ostringstream out;
            for (size_t g = 0; g < layout.Pchs.size(); ++g) {
                const auto& p = layout.Pchs[g];
                out << width << '\t' << g << '\t' << p.Class << '\t' << p.Parent << '\t' << p.Units.size() << '\t';
                for (size_t r = 0; r < p.Roots.size(); ++r) {
                    if (r) { out << ';'; }
                    out << d.Units[p.Roots[r]].Include;
                }
                out << '\n';
            }
            return out.str();
        };
        std::vector<TCandidate> candidates(widths.size());
        ParallelFor(widths.size(), analysisJobs, [&](size_t i) {
            const int width = widths[i];
            // A parallel width owns its mutable closure cache. With one worker
            // keep reusing the cache across widths, as the serial selector did.
            auto localClosures = analysisJobs > 1 ? closures : decltype(closures){};
            auto& candidateClosures = analysisJobs > 1 ? localClosures : closures;
            auto& candidate = candidates[i].Layout;
            candidate = bases;
            TWorkCosts costs{d, candidate, c, {}};
            auto& observed = candidates[i];
            std::ostringstream choicesReport;
            choicesReport.precision(17);
            if (diagnostics) {
                for (const auto& target : d.Targets) {
                    observed.Reasons.push_back(!target.PchEligible ? "no-global-pch-hook" :
                        target.Class < 0 ? "mixed-compiler-classes" : "no-candidate");
                }
            }
            for (int cls = 0; cls < d.Classes; ++cls) {
                int parent = -1;
                for (int g = 0; g < bases.Bases; ++g) {
                    if (bases.Pchs[g].Class == cls) {
                        parent = g;
                    }
                }
                if (parent < 0) { continue; }
                auto targets = classes[cls];
                std::stable_sort(targets.begin(), targets.end(), [&](int a, int b) {
                    return d.Targets[a].Label < d.Targets[b].Label;
                });
                std::vector<int> chunks;
                if (width) {
                    for (size_t begin = 0; begin < targets.size(); begin += width) {
                        std::vector<int> group(targets.begin() + begin, targets.begin() + std::min(targets.size(), begin + width));
                        auto p = Pick(d, c, cls, filesOf(group), &bases.Pchs[parent], false, candidateClosures);
                        chunks.push_back(p.Roots.empty() ? -1 : int(candidate.Pchs.size()));
                        if (!p.Roots.empty()) {
                            p.Parent = parent;
                            candidate.Pchs.push_back(std::move(p));
                        }
                    }
                }
                for (size_t position = 0; position < targets.size(); ++position) {
                    int t = targets[position];
                    auto targetCost = [&](int g) { return costs.Target(t, g); };
                    double cost = targetCost(-1);
                    const double withoutPch = cost;
                    // Nearby target partitions plus the base form a bounded
                    // assignment shortlist; never split a target at a boundary.
                    std::vector<int> choices;
                    if (parent >= 0) { choices.push_back(parent); }
                    if (width) {
                        int chunk = int(position / width);
                        for (int k = std::max(0, chunk - 1); k <= std::min(int(chunks.size()) - 1, chunk + 1); ++k) {
                            if (chunks[k] >= 0) { choices.push_back(chunks[k]); }
                        }
                    }
                    for (int g : choices) {
                        double value = targetCost(g);
                        if (diagnostics) {
                            choicesReport << width << '\t' << d.Targets[t].Label << '\t' << g << '\t'
                                << withoutPch << '\t' << value << '\t' << (value < cost && (frozen || t % 10 != 0)) << '\n';
                        }
                        if (value < cost && (frozen || t % 10 != 0)) {
                            cost = value;
                            candidate.Assignment[t] = int(g);
                        }
                    }
                    if (diagnostics) {
                        observed.Reasons[t] = !frozen && t % 10 == 0 ? "calibration-control" :
                            candidate.Assignment[t] >= 0 ? "selected" : choices.empty() ? "no-candidate" : "no-estimated-benefit";
                    }
                }
            }
            if (diagnostics) {
                observed.BeforeDisk = candidate.Assignment;
                observed.Choices = choicesReport.str();
                observed.Pchs = describePchs(width, candidate);
            }
        });
        // Reuse the full analysis budget for scenario scoring after grouping;
        // never multiply a width pool by nested scenario pools. Only one full
        // prediction graph is resident at a time. Reduce in the original order
        // to retain tie-breaking, diagnostics and progress output determinism.
        for (size_t i = 0; i < widths.size(); ++i) {
            const int width = widths[i];
            auto candidate = candidates[i].Layout; // retain raw winner for the reserve pass
            auto reasons = candidates[i].Reasons;
            TPrediction prediction;
            try {
                prediction = scoreAssigned(candidate, width, reasons, 1);
            } catch (const TError& error) {
                Fail(Format("selection width=%d: %s", width, error.what()));
            }
            double value = prediction.Objective;
            Detail(Format("selection width=%d: %d bases, %zu PCHs, %.3f GB, objective %.3f s%s",
                width, candidate.Bases, candidate.Pchs.size(),
                std::accumulate(prediction.PchBytes.begin(), prediction.PchBytes.end(), 0.0) / 1e9,
                value, prediction.Feasible ? "" : " (infeasible)"));
            if (diagnostics) {
                diagnostics->Widths += Format("%d\t%d\t%zu\t%.17g\t%.17g\t%d\n", width, candidate.Bases, candidate.Pchs.size(),
                    std::accumulate(prediction.PchBytes.begin(), prediction.PchBytes.end(), 0.0), value, int(prediction.Feasible));
                diagnostics->Choices += candidates[i].Choices;
                diagnostics->Candidates += candidates[i].Pchs;
            }
            if (value < score) {
                score = value;
                selectedWidth = width;
                best = std::move(candidate);
                seedReasons = std::move(reasons);
            }
        }
        if (!std::isfinite(score)) {
            Fail("no layout fits the worker/pool/RAM/disk constraints");
        }
        const auto primaryGroups = best.Pchs.size();
        const auto primaryBytes = [&](const TLayout& l) {
            double bytes = 0;
            for (size_t g = 0; g < l.Pchs.size(); ++g) { bytes += ArtifactBytes(d, l, g, c); }
            return bytes;
        };
        const double scoredBytes = primaryBytes(best);
        if (selectedWidth >= 0 || frozen) {
            const auto i = std::find(widths.begin(), widths.end(), selectedWidth) - widths.begin();
            auto reserve = selectedWidth >= 0 ? candidates[i].Layout : *frozen;
            auto reasons = selectedWidth >= 0 ? candidates[i].Reasons : std::vector<std::string>(d.Targets.size());
            if (scoreAssigned(reserve, selectedWidth, reasons, ProbeBudgetFactor).Feasible) {
                best = std::move(reserve);
                seedReasons = std::move(reasons);
            }
        }
        if (diagnostics) {
            std::ostringstream targets;
            targets << "# selected_width=" << selectedWidth << " (-1: retained seed layout)\n"
                    << "target\treason\tbefore_disk_leaf\tselected_leaf\n";
            for (size_t t = 0; t < d.Targets.size(); ++t) {
                std::string reason;
                int beforeDisk = -1;
                if (selectedWidth >= 0) {
                    const auto i = size_t(std::find(widths.begin(), widths.end(), selectedWidth) - widths.begin());
                    reason = seedReasons[t];
                    beforeDisk = candidates[i].BeforeDisk[t];
                } else {
                    beforeDisk = seedAssignment[t];
                    reason = !seedReasons[t].empty() ? seedReasons[t] : !d.Targets[t].PchEligible ? "no-global-pch-hook" : d.Targets[t].Class < 0 ? "mixed-compiler-classes" :
                        frozen ? "retained-initial-plan" : "no-layout-improvement";
                }
                targets << d.Targets[t].Label << '\t' << reason << '\t' << beforeDisk << '\t' << best.Assignment[t] << '\n';
            }
            diagnostics->Targets = targets.str();
            diagnostics->Choices = "# Weighted action-time estimates, not CPU or build wall time. IDs are local to each width before disk pruning.\n"
                "# improved_at_visit is an intermediate greedy decision, not final selection. Unlisted headers/partitions were not scored here.\n"
                "width\ttarget\tcandidate_leaf\twithout_pch_work_seconds\twith_pch_work_seconds\timproved_at_visit\n" + diagnostics->Choices;
            diagnostics->Widths = "# selected_width=" + std::to_string(selectedWidth) +
                Format("\n# scored_budget_bytes=%.0f scored_pchs=%zu scored_bytes=%.0f probe_budget_bytes=%.0f probe_pchs=%zu probe_bytes=%.0f",
                       d.Resources.Disk, primaryGroups, scoredBytes, ProbeBudgetFactor * d.Resources.Disk, best.Pchs.size(), primaryBytes(best)) +
                "\nwidth\tbases\tpchs\tpch_bytes\tobjective_seconds\tfeasible\n" + diagnostics->Widths;
            if (frozen) { diagnostics->Candidates += describePchs(-1, *frozen); }
            diagnostics->Payback = "# selected_width=" + std::to_string(selectedWidth) +
                "; predicted weighted action-work seconds; IDs match selection-candidates.tsv; net excludes mandatory builds\n"
                "width\tpass\tkind\tcandidate_leaf\tassigned_targets\tparse_saved_s\tload_s\textra_compile_s\tbuild_s\tmandatory_build_s\tnet_s\tdecision\n" +
                diagnostics->Payback;
            diagnostics->Candidates = "# IDs are local to each width before disk pruning; width -1 describes a retained seed.\n"
                "width\tcandidate_leaf\tclass\tparent\tclosure_units\troot_headers\n" + diagnostics->Candidates;
        }
        Log(Format("selection: %s; %zu PCHs at %.3f / %.3f GB for scoring; %zu at %.3f / %.3f GB for probes (estimated)",
                   selectedWidth < 0 ? "seed retained" : ("width " + std::to_string(selectedWidth)).c_str(),
                   primaryGroups, scoredBytes / 1e9, d.Resources.Disk / 1e9,
                   best.Pchs.size(), primaryBytes(best) / 1e9, ProbeBudgetFactor * d.Resources.Disk / 1e9));
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
            // Parent artifacts shared with a holdout are also excluded from fit.
            std::set<int> held;
            if (groups.size() >= 3) {
                for (size_t i = 2; i < groups.size(); i += 3) {
                    held.insert(groups[i]);
                }
            }
            std::set<int> excluded = held;
            for (int g : held) {
                if (l.Pchs[g].Parent >= 0) {
                    excluded.insert(l.Pchs[g].Parent);
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
                if (excluded.count(g)) {
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
