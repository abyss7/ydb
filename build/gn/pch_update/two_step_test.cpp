#include "two_step.h"
#include "checks.h"
#include "common.h"
#include "ninja.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <tuple>

namespace NPchUpdate::NTwoStep {
    namespace {
        void Check(bool condition, const std::string& message) {
            if (!condition) { Fail("self-test: " + message); }
        }

        void Near(double actual, double expected, const std::string& message) {
            Check(std::abs(actual - expected) < 1e-7, message + ": " + std::to_string(actual));
        }

        void LinkRequirements() {
            TSymbolProviders providers = {{"needed", {"provider.a", "alternative.a"}}, {"unknown", {}}};
            std::vector<TLinkLine> links = {{"app", {"consumer.o", "provider.a"}},
                                          {"test", {"consumer.o", "alternative.a"}}};
            Check(CheckLinkRequirements({"needed"}, providers, links).Available(),
                  "different providers may satisfy the same symbol in different final links");
            links[1].Inputs.erase("alternative.a");
            auto missing = CheckLinkRequirements({"needed", "needed"}, providers, links);
            Check(!missing.Available() && missing.Missing.at("needed") == std::vector<std::string>{"test"} &&
                  missing.UnknownSymbols.empty() && !missing.UnknownLinks,
                  "every final consumer must have a provider; duplicate requirements do not duplicate failures");
            auto unknown = CheckLinkRequirements({"needed", "unknown", "not-indexed"}, providers, links);
            Check(!unknown.Available() && unknown.UnknownSymbols == std::vector<std::string>({"not-indexed", "unknown"}) &&
                  unknown.Missing.count("needed"), "unknown providers cannot disappear or hide known conflicts");
            auto noLinks = CheckLinkRequirements({"needed"}, providers, {});
            Check(!noLinks.Available() && noLinks.UnknownLinks && noLinks.Missing.empty(),
                  "absent final links are unknown, not proof of availability or a missing library");
            Check(CheckLinkRequirements({}, {}, {}).Available(), "no external requirements need no providers");
        }

        void LinkInputGraph() {
            const char* temp = std::getenv("TMPDIR");
            auto dir = MakeTempDir(temp ? temp : ".", "pch-link-inputs-");
            try {
                WriteFile(JoinPath(dir, "build.ninja"),
                    "rule cxx\n  command = c++ -c $in -o $out\n"
                    "rule alink\n  command = ar rcs $out $in\n"
                    "rule link\n  command = c++ $in -o $out\n"
                    "rule solink\n  command = c++ -shared $in -o $out\n"
                    "build first.o: cxx first.cpp\n"
                    "build second.o: cxx second.cpp\n"
                    "build member.o: cxx member.cpp\n"
                    "build shared.o: cxx shared.cpp\n"
                    "build order.o: cxx order.cpp\n"
                    "build archive.a: alink member.o\n"
                    "build shared.so: solink shared.o\n"
                    "build app: link first.o archive.a | shared.so || order.o\n"
                    "build test: link second.o archive.a\n");
                TNinjaManifest manifest(dir);
                auto inputs = CollectLinkInputs(manifest, {
                    {"first.o", "//same:owner"}, {"second.o", "//same:owner"},
                    {"member.o", "//archive:member"}, {"archive.a", "//archive:archive"},
                    {"shared.o", "//shared:member"}, {"shared.so", "//shared:shared"},
                    {"order.o", "//order:only"}, {"app", "//app:app"}, {"test", "//test:test"}
                });
                const auto& owner = inputs.Targets.at("//same:owner");
                Check(owner.size() == 2 && inputs.Lines.at(owner[0]).Inputs == std::set<std::string>({"first.o", "archive.a", "shared.so"}),
                      "link lines keep actual explicit/implicit objects and libraries, excluding order-only inputs");
                auto sameLabel = CheckLinkRequirements({"second_only"}, {{"second_only", {"second.o"}}}, inputs.Lines, owner);
                Check(!sameLabel.Available() && sameLabel.Missing.at("second_only") == std::vector<std::string>{"app"},
                      "another object with the same target label cannot falsely supply a symbol");
                Check(inputs.Targets.at("//archive:member").size() == 2 && inputs.Targets.at("//archive:archive").size() == 2,
                      "archive members are checked in every final link using the archive");
                const auto& shared = inputs.Targets.at("//shared:member");
                Check(shared.size() == 1 && inputs.Lines.at(shared[0]).Output == "shared.so" &&
                      inputs.Targets.at("//shared:shared").size() == 1,
                      "shared-library members are checked at their own link boundary");
                Check(!inputs.Targets.count("//order:only") &&
                      std::set<std::string>(inputs.Libraries.begin(), inputs.Libraries.end()) ==
                          std::set<std::string>({"first.o", "second.o", "archive.a", "shared.o", "shared.so"}),
                      "symbol providers use linked artifacts, not unlinked archive members or order-only objects");
            } catch (...) {
                RemoveTree(dir);
                throw;
            }
            RemoveTree(dir);
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

            // Deliberately not in topological index order: action 2 waits for 4.
            TData d;
            d.Actions = graph;
            d.Scenarios = 3;
            d.Q = .25;
            d.Resources.Workers = 2;
            d.Resources.Ram = 12;
            d.Resources.Disk = d.Resources.Bandwidth = 1000;
            const double times[] = {3, 5, 2, 1, 4};
            for (size_t i = 0; i < graph.size(); ++i) {
                d.Actions[i].Time = times[i];
                d.Actions[i].Memory = 6;
            }
            auto prediction = Predict(d, {}, {});
            Near(prediction.Cold, 11, "shared topology cold schedule");
            Near(prediction.Incremental, 23.0 / 3, "shared topology preserves scenario-specific invalidation");
            Near(prediction.Objective, .25 * 11 + .75 * 23 / 3, "shared topology objective");
            double incremental = 0;
            for (int k = 0; k < d.Scenarios; ++k) {
                incremental += Makespan(d.Actions, d.Resources, Invalidate(d.Actions, k));
            }
            Near(prediction.Incremental, incremental / d.Scenarios, "prepared graph agrees with standalone scheduling");
            auto scenarios = d;
            scenarios.Scenarios = 37;
            for (size_t i = 0; i < scenarios.Actions.size(); ++i) {
                auto& action = scenarios.Actions[i];
                action.Time *= .137;
                action.Changes.clear();
                for (int k = 0; k < scenarios.Scenarios; ++k) {
                    if (k % 5 == int(i)) { action.Changes.push_back(k); }
                }
            }
            const auto serial = Predict(scenarios, {}, {}, 1);
            for (size_t jobs : {2, 8, 64}) {
                const auto parallel = Predict(scenarios, {}, {}, jobs);
                Check(serial.Cold == parallel.Cold && serial.Incremental == parallel.Incremental &&
                      serial.Objective == parallel.Objective && serial.Feasible == parallel.Feasible,
                      "parallel scenarios retain exact serial reduction results");
            }
            scenarios.Scenarios = 0;
            Check(Predict(scenarios, {}, {}, 8).Incremental == 0, "empty history needs no scenario workers");
            d.Actions[2].DirtyDependencies = {5};
            bool rejected = false;
            try { Predict(d, {}, {}, 3); } catch (const TError&) { rejected = true; }
            Check(rejected, "parallel invalidation propagates malformed dependency errors");
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

        void TestTargets(TData& d, int members = 1) {
            std::vector<std::string> labels;
            for (size_t f = 0; f < d.Files.size(); ++f) {
                labels.push_back("//test:t" + std::to_string(1000000 + f / members));
            }
            GroupTargets(d, labels);
        }

        void LibraryDependentLeaves() {
            auto d = Data();
            d.Units.resize(2);
            for (int u = 0; u < 2; ++u) {
                d.Units[u].Include = u ? "<requires_library>" : "<independent>";
                d.Units[u].Stable = true;
                d.Units[u].Parse = 1;
                d.Units[u].Bytes = 1;
            }
            d.Units[1].Safe = false;
            d.Actions.resize(12);
            d.Files.resize(12);
            for (int f = 0; f < 12; ++f) {
                d.Actions[f].Time = 20;
                d.Actions[f].Memory = 1e6;
                d.Files[f] = {f, 0, std::to_string(f), {{0, 5}, {1, 5}}};
            }
            TestTargets(d);
            auto selected = Select(d, {});
            bool assignedLeaf = false;
            for (int g = 0; g < int(selected.Pchs.size()); ++g) {
                const auto& units = selected.Pchs[g].Units;
                bool needsLibrary = std::find(units.begin(), units.end(), 1) != units.end();
                if (g < selected.Bases) {
                    Check(!needsLibrary, "library-dependent headers never enter conservative shared bases");
                } else if (needsLibrary) {
                    assignedLeaf |= std::find(selected.Assignment.begin(), selected.Assignment.end(), g) != selected.Assignment.end();
                }
            }
            Check(selected.Bases > 0 && assignedLeaf,
                  "profitable library-dependent leaves reach target-aware compatibility instead of global rejection");
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
            TestTargets(d, 2);
            TLayout initial;
            TPch base;
            base.Class = 0;
            base.Representative = 0;
            base.Roots = base.Units = {0};
            initial.Bases = 1;
            initial.Pchs = {base};
            initial.Assignment.assign(d.Targets.size(), 0);
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

        TLayout CheckParallelSelection(const TData& d, const TCalibration& calibration = {}, const TLayout* frozen = nullptr) {
            std::vector<TDiskAlternative> expected;
            auto serial = Select(d, calibration, frozen, &expected, 1);
            auto pchFields = [](const TPch& p) {
                return std::tie(p.Class, p.Parent, p.Representative, p.Roots, p.Units);
            };
            auto alternativeFields = [](const TDiskAlternative& a) {
                return std::tie(a.Width, a.Step, a.Selected, a.Pchs, a.Targets, a.BaseBytes, a.AddedBytes,
                                a.TotalBytes, a.RequiredBudget, a.SavedWork, a.TotalSavedWork);
            };
            for (size_t jobs : {2, 4, 7}) {
                std::vector<TDiskAlternative> actual;
                auto parallel = Select(d, calibration, frozen, &actual, jobs);
                Check(serial.Bases == parallel.Bases && serial.Assignment == parallel.Assignment &&
                      serial.Pchs.size() == parallel.Pchs.size(), "parallel widths preserve the winning layout and target assignment");
                for (size_t g = 0; g < serial.Pchs.size(); ++g) {
                    Check(pchFields(serial.Pchs[g]) == pchFields(parallel.Pchs[g]),
                          "parallel widths preserve exact PCH contents, IDs, parents and representatives");
                }
                Check(expected.size() == actual.size(), "parallel widths preserve all disk alternatives");
                for (size_t i = 0; i < expected.size(); ++i) {
                    Check(alternativeFields(expected[i]) == alternativeFields(actual[i]),
                          "parallel width diagnostics retain exact values, ordering and tie winner");
                }
            }
            return serial;
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
            TestTargets(d, 2);
            TLayout none;
            none.Assignment.assign(d.Targets.size(), -1);
            std::vector<TDiskAlternative> alternatives;
            auto selected = Select(d, {}, nullptr, &alternatives);
            Check(selected.Bases == 1 && selected.Pchs.size() == 1 && selected.Pchs[0].Class == 0,
                  "disk budget keeps the profitable subset instead of rejecting all bases");
            CheckParallelSelection(d);
            auto same = Select(d, {});
            Check(same.Assignment == selected.Assignment && same.Pchs.size() == selected.Pchs.size(),
                  "collecting expansion diagnostics does not change selection");
            auto baseOption = std::find_if(alternatives.begin(), alternatives.end(), [](const auto& a) { return a.Width == 0; });
            Check(baseOption != alternatives.end() && baseOption->Selected && baseOption->Step == 1 &&
                  baseOption->Targets.size() == 4, "report identifies the winning width and restored target owners");
            Near(baseOption->AddedBytes, 3.9e6, "diagnostic restores the excluded PCH bytes");
            Near(baseOption->RequiredBudget, 7.8e6, "new budget includes already retained artifacts");
            Near(baseOption->RequiredBudget - d.Resources.Disk, 3.6e6,
                 "required budget increase accounts for existing free space");
            Near(baseOption->SavedWork, 12.52, "work estimate includes all consumers and the PCH build cost");
            auto p = Predict(d, selected, {});
            Check(p.Feasible && p.Objective < Predict(d, none, {}).Objective,
                  "disk-constrained subset improves the complete graph");
            auto corrected = Select(d, {}, &selected);
            Check(corrected.Bases == 1 && corrected.Pchs[0].Roots == selected.Pchs[0].Roots,
                  "budget pruning preserves the frozen base");
            CheckParallelSelection(d, {}, &selected);
            // A measured slowdown may remove consumers, but cannot discard the
            // frozen base artifact merely because it becomes unused.
            TCalibration slow;
            slow.Classes.resize(2);
            slow.Classes[0].Saved = -1;
            auto unused = Select(d, slow, &selected);
            Check(unused.Bases == 1 && unused.Pchs[0].Roots == selected.Pchs[0].Roots,
                  "an unused frozen base survives correction");
            d.Resources.Disk = 1.95e6;
            Select(d, {}, nullptr, &alternatives);
            CheckParallelSelection(d);
            Check(!alternatives.empty(), "a restoration exactly at twice the budget is reported");
            for (const auto& option : alternatives) {
                Near(option.RequiredBudget, 2 * d.Resources.Disk, "inclusive 2x diagnostic boundary");
                Check(!option.Selected, "no width is marked selected when all candidates only tie the incumbent");
            }
            d.Resources.Disk -= 1;
            Select(d, {}, nullptr, &alternatives);
            Check(alternatives.empty(), "packages exceeding twice the budget are omitted, not partially restored");
        }

        void SharedBaseDiskDiagnostics() {
            auto d = Data();
            d.Resources.Disk = 6e6;
            d.Units.resize(3);
            for (int u = 0; u < 3; ++u) {
                d.Units[u].Include = "<header" + std::to_string(u) + ">";
                d.Units[u].Stable = u == 0;
                d.Units[u].Parse = 1;
                d.Units[u].Bytes = 1e6;
            }
            for (int f = 0; f < 64; ++f) {
                TAction action;
                action.Time = 20;
                action.Memory = 1e6;
                d.Actions.push_back(action);
                d.Files.push_back({f, 0, std::to_string(f), {{0, 3}, {f < 32 ? 1 : 2, 4}}});
            }
            TestTargets(d);
            std::vector<TDiskAlternative> alternatives;
            Select(d, {}, nullptr, &alternatives);
            std::vector<TDiskAlternative> chain;
            for (const auto& option : alternatives) {
                Check(option.RequiredBudget <= 2 * d.Resources.Disk, "all reported variants respect the 2x bound");
                if (option.Width == 32) { chain.push_back(option); }
            }
            Check(chain.size() == 2 && chain[0].Pchs.size() == 2 && chain[1].Pchs.size() == 1,
                  "first restored leaf brings its missing base; the next reuses it");
            Check(chain[0].Pchs[0] == 0 && chain[0].Step == 1 && chain[1].Step == 2,
                  "restored dependencies precede children and steps are cumulative within a width");
            Near(chain[0].AddedBytes, 7.8e6, "first package includes the shared base");
            Near(chain[1].AddedBytes, 3.9e6, "shared base is not charged twice");
            Near(chain[1].TotalBytes, 11.7e6, "cumulative bytes include both leaves and one base");
            Near(chain[1].TotalSavedWork, chain[0].SavedWork + chain[1].SavedWork,
                  "cumulative work counts each restored artifact once");
            d.Resources.Disk = 9e6;
            d.Q = .3;
            d.Scenarios = 5;
            d.Units[1].Children = d.Units[2].Children = {0};
            d.Units[1].Changes = {0, 2};
            d.Units[2].Changes = {1, 3};
            for (size_t i = 0; i < d.Actions.size(); ++i) { d.Actions[i].Changes = {int(i % 5)}; }
            auto selected = CheckParallelSelection(d);
            CheckParallelSelection(d, {}, &selected);
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
            TestTargets(d);
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
            }
            std::vector<std::string> owners;
            for (size_t f = 0; f < d.Files.size(); ++f) {
                owners.push_back("//test:t" + std::to_string(1000000 + (f < 24 ? f / 2 : f)));
            }
            GroupTargets(d, owners);
            for (const auto& target : d.Targets) {
                int f = target.Files.front();
                layout.Assignment.push_back(f < 24 ? f / 4 : -1);
            }
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            for (size_t f = 0; f < d.Files.size(); ++f) {
                obs[f].Time = layout.Assignment[d.Files[f].Target] < 0 ? 20 : 20 - 1.4 * prediction.Saved[f] + 0.03 * prediction.Load[f] + 0.5;
            }
            auto fit = Calibrate(d, layout, prediction, obs);
            Check(fit.Classes[0].Identified, "independent parse/load variation identified");
            Near(fit.Classes[0].Saved, 1 + (8.0 / 10) * 0.4, "target-aggregated rank-tested shrinkage fit");
            auto partial = obs;
            partial[1] = {};
            auto partialFit = Calibrate(d, layout, prediction, partial);
            partial[0].Time *= 100;
            partial[0].Memory *= 100;
            auto changedPartial = Calibrate(d, layout, prediction, partial);
            Near(changedPartial.Classes[0].Saved, partialFit.Classes[0].Saved,
                 "a partially measured target contributes no training rows");
            Near(changedPartial.Classes[0].ConsumerMemory, partialFit.Classes[0].ConsumerMemory,
                 "partial target RSS cannot leak into calibration");
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

        void AtomicTargets() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(1);
            d.Units[0].Include = "<shared>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 10;
            d.Units[0].Bytes = 1;
            std::vector<std::string> labels;
            auto add = [&](const std::string& label, double saved) {
                int id = int(d.Files.size());
                TAction a;
                a.Time = 10;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({id, 0, std::to_string(id), saved > 0 ?
                    std::vector<std::pair<int, double>>{{0, saved}} : std::vector<std::pair<int, double>>{}});
                labels.push_back(label);
            };
            add("//a:control", 0);
            // One profitable file cannot opt in when its siblings' load loses more.
            for (int f = 0; f < 25; ++f) { add("//b:losing", f == 0 ? 4 : 0); }
            // The target is profitable in aggregate, although half its files lose.
            for (int f = 0; f < 8; ++f) { add("//c:winning", f < 4 ? 4 : 0); }
            GroupTargets(d, labels);
            auto selected = Select(d, {});
            Check(selected.Assignment.size() == 3 && selected.Assignment[0] < 0 &&
                  selected.Assignment[1] < 0 && selected.Assignment[2] >= 0,
                  "assignment sums benefits and load over every target member");
            auto prediction = Predict(d, selected, {});
            int leaf = int(d.Actions.size()) + selected.Assignment[2];
            for (int f : d.Targets[2].Files) {
                const auto& a = prediction.Actions[d.Files[f].Action];
                Check(a.Dependencies == std::vector<int>{leaf}, "all members use the same leaf, including losing members");
            }
            Check(prediction.Actions.back().Time > 0 && prediction.Actions[33].Time > d.Actions[33].Time,
                  "target cost includes PCH load in files with no parse savings");
            d.Targets[2].PchEligible = false;
            Check(Select(d, {}).Assignment[2] == -1, "profitable target without a GN hook cannot be assigned");
            bool noHook = false;
            try { Predict(d, selected, {}); } catch (const TError&) { noHook = true; }
            Check(noHook, "prediction rejects an assignment GN cannot apply");
            d.Classes = 2;
            d.Files.back().Class = 1;
            GroupTargets(d, labels);
            auto mixed = Select(d, {});
            Check(d.Targets[2].Class == -1 && mixed.Assignment[2] == -1,
                  "mixed compiler classes leave the whole owner without PCH");
            bool rejected = false;
            try { Predict(d, selected, {}); } catch (const TError&) { rejected = true; }
            Check(rejected, "prediction rejects a PCH incompatible with any target member");
            d.Targets[2].Files.pop_back();
            rejected = false;
            try { Select(d, {}); } catch (const TError&) { rejected = true; }
            Check(rejected, "missing target members are never silently ignored");
        }
    } // namespace

    int SelfTest() {
        LinkRequirements();
        LinkInputGraph();
        Scheduling();
        Invalidation();
        LibraryDependentLeaves();
        FrozenBase();
        DiskBudgetSelection();
        SharedBaseDiskDiagnostics();
        AbsoluteSystemRoot();
        Calibration();
        AtomicTargets();
        IoSelfTest();
        Report("two-step self-tests: OK");
        return 0;
    }
} // namespace NPchUpdate::NTwoStep
