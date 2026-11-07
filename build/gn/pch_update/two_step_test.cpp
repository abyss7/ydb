#include "two_step.h"
#include "common.h"

#include <cmath>
#include <string>

namespace NPchUpdate::NTwoStep {
    namespace {
        void Check(bool condition, const std::string& message) {
            if (!condition) { Fail("self-test: " + message); }
        }

        void Near(double actual, double expected, const std::string& message) {
            Check(std::abs(actual - expected) < 1e-7, message + ": " + std::to_string(actual));
        }

        void Scheduling() {
            TResources r;
            r.Workers = 2;
            r.Ram = 20;
            r.Disk = 1000;
            r.Bandwidth = 100;
            std::vector<TAction> graph(3);
            graph[0].Time = 3;
            graph[1].Time = 5;
            graph[2].Time = 2;
            for (auto& a : graph) { a.Memory = 6; }
            graph[2].Dependencies = {0, 1};
            graph[2].DirtyDependencies = {0, 1};
            Near(Makespan(graph, r), 7, "parallel prerequisites and link");
            r.Ram = 10;
            Near(Makespan(graph, r), 10, "memory admission serializes ready actions");
            r.Ram = 20;
            r.Pools["link"] = 1;
            graph[0].Pool = graph[1].Pool = "link";
            Near(Makespan(graph, r), 10, "pool admission");
            graph[0].Pool = graph[1].Pool = "";
            graph[0].Io = graph[1].Io = 30;
            graph[0].Time = graph[1].Time = 1;
            r.Bandwidth = 10;
            Near(Makespan(graph, r), 8, "shared bandwidth and minimum transfer time");
            graph[0].Memory = 21;
            Check(!std::isfinite(Makespan(graph, r)), "oversized action rejected");
            graph[0].Memory = 6;
            graph[0].Dependencies = {2};
            bool cycle = false;
            try { Makespan(graph, r); } catch (const TError&) { cycle = true; }
            Check(cycle, "dependency cycle rejected");
        }

        void Invalidation() {
            // base -> delta -> TU -> link, with an order-only generator for TU.
            std::vector<TAction> graph(5);
            graph[0].Changes = {0};
            graph[1].Dependencies = graph[1].DirtyDependencies = {0};
            graph[1].Changes = {1};
            graph[2].Dependencies = {1, 4};
            graph[2].DirtyDependencies = {1};
            graph[3].Dependencies = graph[3].DirtyDependencies = {2};
            graph[4].Changes = {2};
            auto base = Invalidate(graph, 0);
            Check(base == std::vector<char>({1, 1, 1, 1, 0}), "base invalidation reaches every consumer and link");
            auto delta = Invalidate(graph, 1);
            Check(delta == std::vector<char>({0, 1, 1, 1, 0}), "delta preserves base");
            auto orderOnly = Invalidate(graph, 2);
            Check(orderOnly == std::vector<char>({0, 0, 0, 0, 1}), "order-only does not dirty consumers");
        }

        TData Data() {
            TData d;
            d.Q = 1;
            d.Classes = 1;
            d.Scenarios = 1;
            d.Resources.Workers = 2;
            d.Resources.Ram = 1e12;
            d.Resources.Disk = 1e12;
            d.Resources.Bandwidth = 1e12;
            d.ActionMemory = 1e6;
            return d;
        }

        void FrozenBase() {
            auto d = Data();
            d.Units.resize(2);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 3;
            d.Units[0].Bytes = 3;
            d.Units[1].Include = "<delta>";
            d.Units[1].Parse = 4;
            d.Units[1].Bytes = 4;
            d.Units[1].Changes = {0};
            d.Actions.resize(8);
            d.Files.resize(8);
            for (int f = 0; f < 8; ++f) {
                d.Actions[f].Time = 10;
                d.Actions[f].Memory = 1e6;
                d.Files[f].Action = f;
                d.Files[f].Class = 0;
                d.Files[f].Own = {{0, 3}, {1, 4}};
                d.Files[f].Source = std::to_string(f);
            }
            TLayout initial;
            TPch base;
            base.Class = 0;
            base.Representative = 0;
            base.Roots = base.Units = {0};
            initial.Bases = 1;
            initial.Pchs = {base};
            initial.Assignment.assign(8, 0);
            auto corrected = Select(d, {}, &initial);
            Check(corrected.Bases == 1 && corrected.Pchs[0].Roots == base.Roots &&
                  corrected.Pchs[0].Units == base.Units && corrected.Pchs[0].Representative == 0,
                  "correction must freeze base contents and compiler representative");
            Check(corrected.Pchs.size() > 1, "profitable delta selected under frozen base");
            auto p = Predict(d, corrected, {});
            const double k = TCoefficients().Size;
            Near(p.PchBytes[0], 3 * k, "base artifact size follows header source bytes");
            Near(p.PchBytes[1], 4 * k, "delta stores only incremental source bytes");
            auto dirty = Invalidate(p.Actions, 0);
            Check(!dirty[d.Actions.size()], "delta edit does not invalidate stable base");
            Check(dirty[d.Actions.size() + 1], "delta edit invalidates delta artifact");
        }

        void DiskBudgetSelection() {
            auto d = Data();
            d.Classes = 2;
            d.Resources.Disk = 4.2e6;
            d.Units.resize(2);
            for (int cls = 0; cls < 2; ++cls) {
                d.Units[cls].Include = "<base" + std::to_string(cls) + ">";
                d.Units[cls].Stable = true;
                d.Units[cls].Parse = 3;
                d.Units[cls].Bytes = 1e6;
                for (int i = 0; i < 8; ++i) {
                    TAction a;
                    a.Time = 10;
                    a.Memory = 1e6;
                    TFile f;
                    f.Action = int(d.Actions.size());
                    f.Class = cls;
                    f.Source = std::to_string(f.Action);
                    f.Own = {{cls, cls ? 2.0 : 6.0}};
                    d.Actions.push_back(a);
                    d.Files.push_back(f);
                }
            }
            TLayout none;
            none.Assignment.assign(d.Files.size(), -1);
            auto selected = Select(d, {});
            Check(selected.Bases == 1 && selected.Pchs.size() == 1 && selected.Pchs[0].Class == 0,
                  "disk budget keeps the profitable subset instead of rejecting all bases");
            auto p = Predict(d, selected, {});
            Check(p.Feasible && p.Objective < Predict(d, none, {}).Objective,
                  "disk-constrained subset improves the complete graph");
            auto corrected = Select(d, {}, &selected);
            Check(corrected.Bases == 1 && corrected.Pchs[0].Roots == selected.Pchs[0].Roots,
                  "budget pruning preserves the frozen base");
            // A measured slowdown may remove consumers, but cannot discard the
            // frozen base artifact merely because it becomes unused.
            TCalibration slow;
            slow.Classes.resize(2);
            slow.Classes[0].Saved = -1;
            auto unused = Select(d, slow, &selected);
            Check(unused.Bases == 1 && unused.Pchs[0].Roots == selected.Pchs[0].Roots,
                  "an unused frozen base survives correction");
        }

        void AbsoluteSystemRoot() {
            auto d = Data();
            d.Units.resize(2);
            d.Units[0].Include = "\"/sysroot/usr/include/stdlib.h\"";
            d.Units[0].Parse = 3;
            d.Units[0].Stable = true;
            d.Units[1].Include = "<wrapper>";
            d.Units[1].Children = {0};
            d.Units[1].Parse = 1;
            d.Units[1].Stable = true;
            for (int i = 0; i < 8; ++i) {
                TAction a;
                a.Time = 10;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({i, 0, std::to_string(i), {{0, 6}, {1, 1}}});
            }
            for (const char* include : {"\"/sysroot/usr/include/stdlib.h\"",
                                        "\"google/protobuf/port_def.inc\"", "<macros.def>"}) {
                d.Units[0].Include = include;
                auto selected = Select(d, {});
                Check(!selected.Pchs.empty(), "normal wrapper remains a useful PCH root");
                for (const auto& pch : selected.Pchs) {
                    Check(pch.Roots == std::vector<int>{1} && pch.Units == std::vector<int>({0, 1}),
                          "absolute headers and macro fragments are only included transitively through a normal root");
                }
            }
        }

        void Calibration() {
            auto d = Data();
            TLayout layout;
            for (int g = 0; g < 6; ++g) {
                TUnit unit;
                unit.Parse = g + 1;
                unit.Bytes = g + 1;
                d.Units.push_back(unit);
                TPch pch;
                pch.Class = 0;
                pch.Representative = int(d.Files.size());
                pch.Roots = pch.Units = {g};
                layout.Pchs.push_back(pch);
                for (int j = 0; j < 4; ++j) {
                    TAction action;
                    action.Time = 20;
                    action.Memory = 1e6;
                    TFile file;
                    file.Action = int(d.Actions.size());
                    file.Class = 0;
                    file.Own = {{g, 0.5 + 0.1 * j}};
                    d.Actions.push_back(action);
                    d.Files.push_back(file);
                    layout.Assignment.push_back(g);
                }
            }
            for (int j = 0; j < 4; ++j) {
                TAction a;
                a.Time = 20;
                TFile f;
                f.Action = int(d.Actions.size());
                f.Class = 0;
                d.Actions.push_back(a);
                d.Files.push_back(f);
                layout.Assignment.push_back(-1);
            }
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            for (size_t f = 0; f < d.Files.size(); ++f) {
                obs[f].Time = layout.Assignment[f] < 0 ? 20 : 20 - 1.4 * prediction.Saved[f] + 0.03 * prediction.Load[f] + 0.5;
            }
            auto fit = Calibrate(d, layout, prediction, obs);
            Check(fit.Classes[0].Identified, "independent parse/load variation identified");
            Near(fit.Classes[0].Saved, 1 + (4.0 / 6) * 0.4, "rank-tested shrinkage fit");
            // A sampled verification measures only part of the graph: the fit
            // must use what it has, and the rest keeps the model's own time.
            std::vector<TObservation> sampled = obs;
            for (size_t f = 0; f < 12; ++f) { sampled[f] = TObservation{}; }
            for (int g : {1, 3, 5}) { sampled[d.Actions.size() + g] = TObservation{}; }
            auto sampledFit = Calibrate(d, layout, prediction, sampled);
            Check(sampledFit.ActionTime[0] == prediction.Actions[0].Time && sampledFit.ActionMemory[0] == prediction.Actions[0].Memory,
                  "unmeasured actions keep the model time and memory");
            Check(sampledFit.ActionTime[24] == obs[24].Time, "measured actions keep the observation");
            Check(sampledFit.Report.find("# measured_actions") != std::string::npos,
                  "sampled calibration reports its coverage");
            std::vector<TObservation> nothing(prediction.Actions.size());
            auto prior = Calibrate(d, layout, prediction, nothing);
            Check(!prior.Classes[0].Identified && prior.Classes[0].Baseline == 1 && prior.Classes[0].Size == 3.9,
                  "a run without measurements falls back to priors");
            for (int g : {2, 5}) {
                for (int f = g * 4; f < g * 4 + 4; ++f) { obs[f].Time *= 100; obs[f].Memory *= 100; }
                obs[d.Actions.size() + g].Bytes *= 100;
                obs[d.Actions.size() + g].Time *= 100;
            }
            auto changed = Calibrate(d, layout, prediction, obs);
            Near(changed.Classes[0].Saved, fit.Classes[0].Saved, "held-out consumer data cannot leak into fit");
            Near(changed.Classes[0].Size, fit.Classes[0].Size, "held-out artifacts cannot leak into fit");
            Near(changed.Classes[0].ConsumerMemory, fit.Classes[0].ConsumerMemory, "held-out RSS cannot leak into fit");
            Check(changed.Report != fit.Report, "held-out error responds to changed observations");
            for (size_t f = 0; f < 24; ++f) {
                prediction.Saved[f] = prediction.Load[f] * 0.5;
                obs[f].Time = 22;
            }
            auto deficient = Calibrate(d, layout, prediction, obs);
            Check(!deficient.Classes[0].Identified, "collinear features are not independently identified");
            Check(deficient.Classes[0].Saved < 0, "combined fallback retains measured slowdowns");
            auto slower = Predict(d, layout, deficient);
            for (size_t f = 0; f < 24; ++f) {
                Check(slower.Actions[f].Time >= 20, "negative combined fit cannot reward unused PCH bytes");
            }
        }
    } // namespace

    int SelfTest() {
        Scheduling();
        Invalidation();
        FrozenBase();
        DiskBudgetSelection();
        AbsoluteSystemRoot();
        Calibration();
        IoSelfTest();
        Report("two-step self-tests: OK");
        return 0;
    }
} // namespace NPchUpdate::NTwoStep
