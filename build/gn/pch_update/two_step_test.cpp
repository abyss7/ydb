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
                    "build shared.so shared.so.TOC: solink shared.o\n"
                    "build app: link first.o archive.a | shared.so.TOC || order.o\n"
                    "build test: link second.o archive.a | shared.so\n");
                TNinjaManifest manifest(dir);
                auto inputs = CollectLinkInputs(manifest, {
                    {"first.o", "//same:owner"}, {"second.o", "//same:owner"},
                    {"member.o", "//archive:member"}, {"archive.a", "//archive:archive"},
                    {"shared.o", "//shared:member"}, {"shared.so", "//shared:shared"},
                    {"order.o", "//order:only"}, {"app", "//app:app"}, {"test", "//test:test"}
                });
                const auto& owner = inputs.Targets.at("//same:owner");
                Check(owner.size() == 2 && inputs.Lines.at(owner[0]).Inputs == std::set<std::string>({"first.o", "archive.a", "shared.so"}),
                      "link lines resolve GN TOC dependencies to actual libraries, excluding order-only inputs");
                Check(CheckLinkRequirements({"exported"}, {{"exported", {"shared.so"}}}, inputs.Lines, owner).Available(),
                      "TOC and direct shared-library dependencies identify the same symbol provider");
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
            graph[0].Output = "unrelated-consumer";
            graph[1].Output = "generator.o";
            graph[2].Output = "pch/3.pch";
            graph[0].Dependencies = {2};
            graph[1].Dependencies = {2};
            graph[2].Dependencies = {1};
            std::string cycle;
            try { Makespan(graph, r); } catch (const TError& error) { cycle = error.what(); }
            Check(cycle.find("[2] pch/3.pch\n  [1] generator.o\n  [2] pch/3.pch") != std::string::npos &&
                  cycle.find("unrelated-consumer") == std::string::npos, "report the closed dependency cycle only");
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

        void CycleRepair() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(2);
            d.Units[0].Parse = d.Units[1].Parse = 4;
            d.Units[1].Producers = {0};
            d.Actions.resize(3);
            for (int i = 0; i < 3; ++i) {
                d.Actions[i].Time = 20;
                d.Files.push_back({i, 0, std::to_string(i), {{0, 4}, {1, 4}}});
            }
            GroupTargets(d, {"//a:generator", "//a:generator", "//b:consumer"});
            TLayout l;
            l.Bases = 1;
            l.Pchs = {{0, -1, 0, {0}, {0}}, {0, 0, 0, {1}, {0, 1}}};
            l.Assignment = {1, 1};
            auto p = ScoreCandidate(d, l, {}, 1, 32);
            Check(p.Feasible && l.Assignment == std::vector<int>{0, 1} && l.Pchs.size() == 2,
                  "generator target uses profitable safe parent, other target keeps delta");
            d.Units[0].Producers = {0};
            l.Assignment = {1, 1};
            p = ScoreCandidate(d, l, {}, 1, 32);
            Check(p.Feasible && l.Assignment == std::vector<int>{-1, 1},
                  "cyclic parent forces NONE only on generator target");
            d.Units[0].Producers.push_back(2);
            l.Assignment = {1, 1};
            p = ScoreCandidate(d, l, {}, 1, 32);
            Check(p.Feasible && l.Assignment == std::vector<int>{-1, -1},
                  "independent cyclic consumers require successive repairs");
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

            auto initialPrediction = Predict(d, initial, {});
            std::vector<TObservation> obs(initialPrediction.Actions.size());
            obs.back().Time = 1;
            obs.back().Bytes = 99; // even the capped class estimate is only 3 * 20
            auto fit = Calibrate(d, initial, initialPrediction, obs);
            auto measured = Predict(d, corrected, fit);
            Near(measured.PchBytes[0], 99, "frozen base uses its measured size");
            Near(measured.PchBytes[1], 4 * fit.Classes[0].Size, "delta still uses the class estimate");
            Near(measured.Actions[d.Actions.size() + 1].Io, 99 + measured.PchBytes[1], "delta reads the measured base");
            auto changed = corrected;
            changed.Pchs[0].Roots = {1};
            Near(Predict(d, changed, fit).PchBytes[0], 3 * fit.Classes[0].Size, "different base cannot reuse measured size");
            d.Resources.Disk = 1;
            bool frozenOverBudget = false;
            try { Select(d, fit, &initial); } catch (const TError&) { frozenOverBudget = true; }
            Check(frozenOverBudget, "a frozen base must fit the final scoring budget, not just the probe allowance");
            auto refused = corrected;
            d.Resources.Disk = 100;
            FitMeasuredDisk(d, refused, {}, {99, 2}, true, [](int, int) { return false; });
            for (size_t t = 0; t < refused.Assignment.size(); ++t) {
                if (corrected.Assignment[t] == 1) { Check(refused.Assignment[t] == -1, "incompatible fallback uses NONE"); }
            }
            FitMeasuredDisk(d, corrected, {}, {99, 2}, true, [](int, int) { return true; });
            auto remap = CompactLayout(corrected, true);
            Check(remap == std::vector<int>({0,-1}) && corrected.Assignment == initial.Assignment,
                  "an over-budget measured delta falls back atomically to its profitable base");
            bool tooLarge = false;
            try { FitMeasuredDisk(d, corrected, {}, {101}, true, [](int, int) { return true; }); }
            catch (const TError&) { tooLarge = true; }
            Check(tooLarge, "fail explicitly when mandatory measured bases exceed the budget");
        }

        void AssignedPayback() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(2);
            d.Units[0].Stable = true;
            d.Units[0].Include = "<base>";
            d.Units[0].Parse = 4;
            d.Units[1].Include = "<child>";
            d.Units[1].Parse = 5;
            d.Actions.resize(3);
            d.Files.resize(3);
            for (int i = 0; i < 3; ++i) {
                d.Actions[i].Time = 20;
                d.Files[i].Action = i;
                d.Files[i].Class = 0;
                // t0 helps design both headers but is a no-PCH calibration control.
                d.Files[i].Own = {{0, i ? 3.0 : 4.0}, {1, i ? 2.5 : 5.0}};
            }
            TestTargets(d);
            TSelectionDiagnostics diag;
            auto baseOnly = Select(d, {}, nullptr, 1, &diag);
            Check(baseOnly.Pchs.size() == 1 && baseOnly.Assignment == std::vector<int>({-1, 0, 0}) &&
                  diag.Payback.find("\tdelta\t1\t2\t") != std::string::npos &&
                  diag.Payback.find("\tdrop-assignments\n") != std::string::npos,
                  "delta cannot use the control's parsing savings to pay for its build");
            for (int i : {1,2}) { d.Files[i].Own = {{0, 1}, {1, 3}}; }
            auto none = Select(d, {});
            Check(none.Pchs.empty(), "profitable child does not cover the cost of the whole family");
            for (int i : {1,2}) { d.Files[i].Own = {{0, 1}, {1, 4}}; }
            auto family = Select(d, {});
            Check(family.Pchs.size() == 2 && family.Assignment == std::vector<int>({-1, 1, 1}),
                  "a base with no direct consumers is paid for once by its descendants");
        }

        TLayout CheckParallelSelection(const TData& d, const TCalibration& calibration = {}, const TLayout* frozen = nullptr) {
            auto serial = Select(d, calibration, frozen, 1);
            auto pchFields = [](const TPch& p) {
                return std::tie(p.Class, p.Parent, p.Representative, p.Roots, p.Units);
            };
            std::string targetDiagnostics, choiceDiagnostics, widthDiagnostics, candidateDiagnostics;
            for (size_t jobs : {1, 2, 4, 7}) {
                TSelectionDiagnostics diagnostics;
                auto parallel = Select(d, calibration, frozen, jobs, &diagnostics);
                if (jobs == 1) {
                    targetDiagnostics = diagnostics.Targets;
                    choiceDiagnostics = diagnostics.Choices;
                    widthDiagnostics = diagnostics.Widths;
                    candidateDiagnostics = diagnostics.Candidates;
                } else {
                    Check(targetDiagnostics == diagnostics.Targets && choiceDiagnostics == diagnostics.Choices &&
                          widthDiagnostics == diagnostics.Widths && candidateDiagnostics == diagnostics.Candidates,
                          "selection observer is deterministic across analysis concurrency");
                }
                Check(!diagnostics.Targets.empty() && !diagnostics.Choices.empty() && !diagnostics.Widths.empty(),
                      "selection observer produces target, shortlist and width reports");
                Check(serial.Bases == parallel.Bases && serial.Assignment == parallel.Assignment &&
                      serial.Pchs.size() == parallel.Pchs.size(), "parallel widths preserve the winning layout and target assignment");
                for (size_t g = 0; g < serial.Pchs.size(); ++g) {
                    Check(pchFields(serial.Pchs[g]) == pchFields(parallel.Pchs[g]),
                          "parallel widths preserve exact PCH contents, IDs, parents and representatives");
                }

            }
            return serial;
        }

        void SharedBaseDiskDiagnostics() {
            auto d = Data();
            d.Resources.Disk = 8e6;
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
            TSelectionDiagnostics diag;
            auto full = Select(d, {}, nullptr, 1, &diag);
            Check(full.Bases == 1 && full.Pchs.size() == 3 &&
                  diag.Widths.find("scored_budget_bytes=8000000 scored_pchs=2 scored_bytes=7800000 probe_budget_bytes=16000000 probe_pchs=3 probe_bytes=11700000") != std::string::npos,
                  "width scored with one delta at 1x; winner probes both deltas within 2x, charging the base only once");
            auto before = full.Assignment;
            auto families = full;
            d.Resources.Disk = 500000;
            auto evictions = FitMeasuredDisk(d, families, {}, {1000000, 1000000, 5000000}, false, [](int, int) { return true; });
            CompactLayout(families, false);
            Check(families.Pchs.empty() && evictions.find("\nfamily\t0\t") != std::string::npos,
                  "oversized initial base can be evicted with all remaining owners on measured sizes");
            d.Resources.Disk = 6e6;
            FitMeasuredDisk(d, full, {}, {1000000, 1000000, 5000000}, false, [](int, int) { return true; });
            for (size_t t = 0; t < before.size(); ++t) {
                Check(full.Assignment[t] == (before[t] == 2 ? 0 : before[t]), "evict lowest marginal seconds per measured byte");
            }
            d.Units[0].Stable = false;
            Check(Select(d, {}).Pchs.empty(), "class without a safe stable base cannot create standalone deltas");
            d.Units[0].Stable = true;
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
            Check(changedPartial.Classes[0].Saved != partialFit.Classes[0].Saved ||
                  changedPartial.Classes[0].Identified != partialFit.Classes[0].Identified ||
                  changedPartial.Classes[0].Load != partialFit.Classes[0].Load,
                  "observed members of partially measured targets contribute to calibration");
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
            labels.back() = "//c:winning_enum_objs";
            GroupTargets(d, labels);
            auto split = Select(d, {});
            Check(d.Targets.size() == 4 && d.Targets[2].Class == 0 && d.Targets[3].Class == 1 &&
                  split.Assignment[2] >= 0 && split.Assignment[3] < 0,
                  "an enum source_set with different flags does not veto its consumer's PCH");
        }
        // A commit shared by several roots invalidates the PCH once: the
        // second root must not pay the extra-rebuild penalty again, and the
        // penalty must use the accelerated consumer time of the PCH that
        // already includes both roots. A root whose fresh commits would
        // reprice rebuilds of the whole accepted body stays out.
        void CoChangedRoots() {
            for (int second : {0, 1}) {
                auto d = Data();
                d.Resources.Workers = 1;
                d.Q = 0;
                d.Scenarios = 2;
                d.Units.resize(2);
                for (int u = 0; u < 2; ++u) {
                    d.Units[u].Include = "<unit" + std::to_string(u) + ">";
                    d.Units[u].Stable = true;
                    d.Units[u].Parse = u ? 1 : 5;
                    d.Units[u].Bytes = 1;
                    // second=0: one commit touches both roots (its extra
                    // rebuild is already paid at the first); second=1: a fresh
                    // consumer commit that also doubles the PCH rebuild rate,
                    // repricing the rebuilds of the whole accepted body.
                    d.Units[u].Changes = {u ? second : 0};
                }
                for (int f = 0; f < 4; ++f) {
                    TAction a;
                    a.Time = 10; // own parse times below stay below it
                    a.Memory = 1e6;
                    a.Changes = {1}; // consumers rebuild on their own commit only
                    d.Actions.push_back(a);
                    d.Files.push_back({f, 0, std::to_string(f), {{0, 7}, {1, 1.5}}});
                }
                TestTargets(d);
                auto selected = Select(d, {});
                Check(!selected.Pchs.empty() &&
                          selected.Pchs[0].Roots == (second == 0 ? std::vector<int>({0, 1}) : std::vector<int>({0})),
                      second == 0 ? "an already-paying commit keeps the co-changed root"
                                  : "a fresh churning root cannot reprice the whole accepted body");
            }
        }

        // The base shared with a held-out delta stays in the artifact fits:
        // the holdout scores consumer time only, and each class has one base.
        void HeldOutParentCalibration() {
            auto d = Data();
            d.Units.resize(3);
            for (int u = 0; u < 3; ++u) {
                d.Units[u].Include = "<unit" + std::to_string(u) + ">";
                d.Units[u].Parse = 2;
                d.Units[u].Bytes = 1;
                d.Units[u].Stable = true;
            }
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Representative = 0;
            base.Roots = base.Units = {0};
            layout.Pchs.push_back(base);
            for (int g = 1; g <= 2; ++g) {
                TPch delta;
                delta.Class = 0;
                delta.Parent = 0;
                delta.Representative = g;
                delta.Roots = {g};
                delta.Units = {0, g};
                layout.Pchs.push_back(delta);
            }
            for (int t = 0; t < 3; ++t) {
                TAction a;
                a.Time = 10;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({int(d.Actions.size()) - 1, 0, std::to_string(t), {{t, 3}}});
            }
            TestTargets(d);
            layout.Assignment = {0, 1, 2};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            obs[d.Actions.size()].Time = 2 * d.Units[0].Parse;
            auto fit = Calibrate(d, layout, prediction, obs);
            Near(fit.Classes[0].BaseBuild, 2, "the shared base calibrates BaseBuild despite the held-out delta");
            Check(fit.Report.find("unmeasured") == std::string::npos,
                  "the base build observation is not reported as unmeasured");
        }
    } // namespace

    int SelfTest() {
        LinkRequirements();
        LinkInputGraph();
        Scheduling();
        Invalidation();
        LibraryDependentLeaves();
        CycleRepair();
        FrozenBase();
        AssignedPayback();
        SharedBaseDiskDiagnostics();
        AbsoluteSystemRoot();
        Calibration();
        CoChangedRoots();
        HeldOutParentCalibration();
        AtomicTargets();
        IoSelfTest();
        Report("two-step self-tests: OK");
        return 0;
    }
} // namespace NPchUpdate::NTwoStep
