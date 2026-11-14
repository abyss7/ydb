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
                Fail("scheduler: dependency cycle (possibly a PCH depending on its consumer)");
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
            if (candidates.size() > 128) {
                candidates.resize(128);
            }
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
                    valid &= d.Units[u].Safe && (!stable || d.Units[u].Stable);
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

        // Admit useful subtrees instead of rejecting the entire shortlist when
        // the aggregate artifacts exceed the disk budget. Prune leaves first;
        // a parent becomes removable only after its last child is removed.
        void FitDisk(const TData& d, TLayout& layout, const TCalibration& calibration, bool frozen) {
            const int count = int(layout.Pchs.size());
            std::vector<double> bytes(count), work(count), gain(count);
            std::vector<std::set<int>> changes(count);
            std::vector<int> children(count), consumers(count);
            std::vector<char> keep(count, true);
            double disk = 0;
            for (int g = 0; g < count; ++g) {
                const auto& p = layout.Pchs[g];
                auto c = Coeff(calibration, p.Class);
                work[g] = Parse(d, p.Units);
                double parent = p.Parent < 0 ? 0 : work[p.Parent];
                double parentBytes = p.Parent < 0 ? 0 : Volume(d, layout.Pchs[p.Parent].Units);
                if (p.Parent >= 0) { ++children[p.Parent]; }
                bytes[g] = std::max(1.0, (Volume(d, p.Units) - parentBytes) * c.Size);
                disk += bytes[g];
                for (int u : p.Units) {
                    changes[g].insert(d.Units[u].Changes.begin(), d.Units[u].Changes.end());
                }
                double frequency = d.Q + (1 - d.Q) * double(changes[g].size()) / std::max(1, d.Scenarios);
                gain[g] = -frequency * ((work[g] - parent) * (p.Parent < 0 ? c.BaseBuild : c.DeltaBuild) + parent * 0.02);
            }
            for (size_t f = 0; f < d.Files.size(); ++f) {
                int g = layout.Assignment[d.Files[f].Target];
                if (g < 0) { continue; }
                ++consumers[g];
                const auto& a = d.Actions[d.Files[f].Action];
                auto c = Coeff(calibration, d.Files[f].Class);
                double frequency = d.Q + (1 - d.Q) * double(a.Changes.size()) / std::max(1, d.Scenarios);
                double extra = 0;
                for (int k : changes[g]) {
                    extra += !std::binary_search(a.Changes.begin(), a.Changes.end(), k);
                }
                double withPch = Consumer(a, Saved(d.Files[f], layout.Pchs[g].Units), work[g], c);
                gain[g] += Consumer(a, 0, 0, c) * frequency - withPch *
                    (frequency + (1 - d.Q) * extra / std::max(1, d.Scenarios));
            }
            using TEntry = std::pair<double, int>;
            std::priority_queue<TEntry, std::vector<TEntry>, std::greater<TEntry>> leaves;
            auto enqueue = [&](int g) {
                if (!children[g] && !(frozen && g < layout.Bases)) {
                    leaves.emplace(consumers[g] ? gain[g] / bytes[g] : -INF, g);
                }
            };
            for (int g = 0; g < count; ++g) { enqueue(g); }
            while (!leaves.empty()) {
                int g = leaves.top().second;
                leaves.pop();
                if (consumers[g] && disk <= d.Resources.Disk) { break; }
                keep[g] = false;
                disk -= bytes[g];
                int parent = layout.Pchs[g].Parent;
                if (parent >= 0 && !--children[parent]) { enqueue(parent); }
            }
            std::vector<int> remap(count, -1);
            std::vector<TPch> kept;
            int bases = 0;
            for (int g = 0; g < count; ++g) {
                if (!keep[g]) { continue; }
                remap[g] = int(kept.size());
                auto p = std::move(layout.Pchs[g]);
                if (p.Parent >= 0) { p.Parent = remap[p.Parent]; }
                kept.push_back(std::move(p));
                bases += g < layout.Bases;
            }
            for (int& g : layout.Assignment) { if (g >= 0) { g = remap[g]; } }
            layout.Pchs = std::move(kept);
            layout.Bases = bases;
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

    double Makespan(const std::vector<TAction>& actions, const TResources& r, const std::vector<char>& dirty) {
        if (r.Workers < 1 || r.Ram <= 0 || r.Bandwidth <= 0 || (!dirty.empty() && dirty.size() != actions.size())) {
            Fail("scheduler: invalid resource limits or dirty mask");
        }
        std::vector<std::vector<int>> users;
        auto order = Topology(actions, users);
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

    std::vector<char> Invalidate(const std::vector<TAction>& actions, int scenario) {
        std::vector<std::vector<int>> users;
        auto order = Topology(actions, users);
        std::vector<char> dirty(actions.size());
        for (int i : order) {
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

    TPrediction Predict(const TData& d, const TLayout& l, const TCalibration& calibration) {
        ValidateTargets(d);
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
            double volume = Volume(d, h.Units);
            double parentVolume = h.Parent >= 0 ? Volume(d, l.Pchs[h.Parent].Units) : 0;
            TAction a;
            a.Output = "pch/" + std::to_string(g) + ".pch";
            // Consumer load fits do not identify the compiler's PCH-to-PCH load
            // cost. Keep that prior separate, including in combined-only fits.
            a.Time = std::max(0.001, (total - parent) * (h.Parent < 0 ? c.BaseBuild : c.DeltaBuild) + parent * 0.02);
            double bytes = std::max(1.0, (volume - parentVolume) * c.Size);
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
        if (disk > d.Resources.Disk) {
            p.Objective = INF;
            return p;
        }
        p.Cold = Makespan(p.Actions, d.Resources);
        for (int k = 0; k < d.Scenarios; ++k) {
            p.Incremental += Makespan(p.Actions, d.Resources, Invalidate(p.Actions, k));
        }
        p.Incremental /= std::max(1, d.Scenarios);
        p.Feasible = std::isfinite(p.Cold) && std::isfinite(p.Incremental);
        p.Objective = p.Feasible ? d.Q * p.Cold + (1 - d.Q) * p.Incremental : INF;
        return p;
    }

    TLayout Select(const TData& d, const TCalibration& c, const TLayout* frozen) {
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
        double score = Predict(d, best, c).Objective;
        for (int width : {0, 32, 128, 512}) {
            TLayout candidate = bases;
            for (int cls = 0; cls < d.Classes; ++cls) {
                int parent = -1;
                for (int g = 0; g < bases.Bases; ++g) {
                    if (bases.Pchs[g].Class == cls) {
                        parent = g;
                    }
                }
                auto targets = classes[cls];
                std::stable_sort(targets.begin(), targets.end(), [&](int a, int b) {
                    return d.Targets[a].Label < d.Targets[b].Label;
                });
                std::vector<int> chunks;
                if (width) {
                    for (size_t begin = 0; begin < targets.size(); begin += width) {
                        std::vector<int> group(targets.begin() + begin, targets.begin() + std::min(targets.size(), begin + width));
                        auto p = Pick(d, c, cls, filesOf(group), parent < 0 ? nullptr : &bases.Pchs[parent], false, closures);
                        chunks.push_back(p.Roots.empty() ? -1 : int(candidate.Pchs.size()));
                        if (!p.Roots.empty()) {
                            p.Parent = parent;
                            // A delta without a base is still a single-level PCH;
                            // it is not promoted to a frozen base in correction.
                            candidate.Pchs.push_back(std::move(p));
                        }
                    }
                }
                auto coeff = Coeff(c, cls);
                std::map<int, std::pair<double, std::set<int>>> features;
                auto feature = [&](int g) -> const std::pair<double, std::set<int>>& {
                    auto [it, added] = features.try_emplace(g);
                    if (added) {
                        const auto& p = candidate.Pchs[g];
                        it->second.first = Parse(d, p.Units);
                        for (int u : p.Units) {
                            it->second.second.insert(d.Units[u].Changes.begin(), d.Units[u].Changes.end());
                        }
                    }
                    return it->second;
                };
                for (size_t position = 0; position < targets.size(); ++position) {
                    int t = targets[position];
                    // Sum expected work, including every member's PCH load and
                    // extra invalidations. No individual TU can opt out.
                    auto targetCost = [&](int g) {
                        double total = 0;
                        for (int f : d.Targets[t].Files) {
                            const auto& a = d.Actions[d.Files[f].Action];
                            double frequency = d.Q + (1 - d.Q) * double(a.Changes.size()) / std::max(1, d.Scenarios);
                            double saved = 0, load = 0;
                            if (g >= 0) {
                                const auto& features = feature(g);
                                load = features.first;
                                saved = Saved(d.Files[f], candidate.Pchs[g].Units);
                                for (int k : features.second) {
                                    if (!std::binary_search(a.Changes.begin(), a.Changes.end(), k)) {
                                        frequency += (1 - d.Q) / std::max(1, d.Scenarios);
                                    }
                                }
                            }
                            total += Consumer(a, saved, load, coeff) * frequency;
                        }
                        return total;
                    };
                    double cost = targetCost(-1);
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
                        if (value < cost && (frozen || t % 10 != 0)) {
                            cost = value;
                            candidate.Assignment[t] = int(g);
                        }
                    }
                }
            }
            FitDisk(d, candidate, c, frozen != nullptr);
            auto prediction = Predict(d, candidate, c);
            double value = prediction.Objective;
            Log(Format("selection width=%d: %d bases, %zu PCHs, %.3f GB, objective %.3f s%s",
                width, candidate.Bases, candidate.Pchs.size(),
                std::accumulate(prediction.PchBytes.begin(), prediction.PchBytes.end(), 0.0) / 1e9,
                value, prediction.Feasible ? "" : " (infeasible)"));
            if (value < score) {
                score = value;
                best = std::move(candidate);
            }
        }
        if (!std::isfinite(score)) {
            Fail("no layout fits the worker/pool/RAM/disk constraints");
        }
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
            bool Complete = true;
            double Baseline = 0, Time = 0, Saved = 0, Load = 0, Memory = 0;
        };
        std::vector<TTargetObservation> targets(d.Targets.size());
        for (size_t t = 0; t < d.Targets.size(); ++t) {
            auto& target = targets[t];
            for (int f : d.Targets[t].Files) {
                const auto& obs = observations[d.Files[f].Action];
                target.Complete &= obs.Time > 0;
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
                    if (target.Complete && target.Baseline > 0) {
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
                if (d.Targets[t].Class != cls || g < 0 || held.count(g) || !target.Complete) {
                    continue;
                }
                training.insert(int(t));
                double bytes = p.PchBytes[g];
                if (l.Pchs[g].Parent >= 0) { bytes += p.PchBytes[l.Pchs[g].Parent]; }
                const double count = d.Targets[t].Files.size();
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
                if (d.Targets[t].Class == cls && held.count(l.Assignment[t]) && targets[t].Complete) {
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
        for (const auto& target : targets) { complete += target.Complete; }
        report << "# measured_targets\t" << complete << '\t' << d.Targets.size() << '\n';
        out.Report = report.str();
        return out;
    }
} // namespace NPchUpdate::NTwoStep
