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
            // Another base serializes other units: it keeps the measured
            // weight of the unit S0 saw and the class prior for the rest,
            // never the measured total of a different artifact.
            changed.Pchs[0].Roots = {0, 1};
            changed.Pchs[0].Units = {0, 1};
            Near(fit.UnitSize[0][0] * 3, 99, "the measured bytes of a base land on its own units");
            Near(Predict(d, changed, fit).PchBytes[0], 3 * fit.UnitSize[0][0] + 4 * fit.Classes[0].Size,
                 "different base cannot reuse measured size");
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
            auto remap = CompactLayout(corrected);
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
                  diag.Payback.find("\tfamily\t0\t2\t") != std::string::npos &&
                  diag.Payback.find("\tdelta\t") == std::string::npos,
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
            CompactLayout(families);
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
            // Controls of varied trace time: only a spread of them separates a
            // proportional baseline from per-compile seconds.
            for (int j = 0; j < 4; ++j) {
                TAction a;
                a.Time = 10.0 * (1 << j);
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
            // A compile's seconds outside the traces (driver, startup) are
            // paid with and without a PCH alike: the controls carry them too.
            const auto generate = [&] {
                for (size_t i = 0; i < obs.size(); ++i) {
                    obs[i].Time = prediction.Actions[i].Time;
                    obs[i].Memory = prediction.Actions[i].Memory;
                    if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
                }
                for (size_t f = 0; f < d.Files.size(); ++f) {
                    obs[f].Time = layout.Assignment[d.Files[f].Target] < 0 ? d.Actions[d.Files[f].Action].Time + 0.5
                        : 20 - 1.4 * d.Files[f].Own[0].second + 0.03 * d.Units[layout.Assignment[d.Files[f].Target]].Parse + 0.5;
                }
            };
            auto cvError = [](const TCalibration& fit) {
                for (auto line : Split(fit.Report, '\n')) {
                    const auto row = Split(line, '\t');
                    if (row.size() > 4 && row[0] == "0") { return std::stod(std::string(row[4])); }
                }
                return -1.0;
            };
            generate();
            auto fit = Calibrate(d, layout, prediction, obs);
            const auto& c = fit.Classes[0];
            Check(c.Identified, "independent parse/load variation identified");
            Near(c.Residual, 0.5, "the controls show a compile's seconds outside the traces");
            Near(c.Baseline, 1, "the controls show the compiler's speed against the traces");
            // All twelve targets with a PCH train the working fit: the
            // shrinkage counts every measured group, none is held back.
            Near(c.Saved, 1 + (12.0 / 14) * 0.4, "full-data rank-tested shrinkage fit");
            Near(c.Load, 0.02 + (12.0 / 14) * 0.01, "load is fitted next to saved on the centered data");
            Check(cvError(fit) > 0 && cvError(fit) < 0.2,
                  "leave-one-group-out scores each group with the shrunk model that never saw it");
            Check(fit.TargetReport.find("target\tclass\ts0_leaf\tmembers\tmeasured\ts0_compile_seconds") != std::string::npos,
                  "per-target anchors and multipliers are reported");
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
            // uses what it has, and an unmeasured target keeps its trace time.
            std::vector<TObservation> sampled = obs;
            for (size_t f = 0; f < 12; ++f) { sampled[f] = TObservation{}; }
            for (int g : {1, 3, 5}) { sampled[d.Actions.size() + g] = TObservation{}; }
            auto sampledFit = Calibrate(d, layout, prediction, sampled);
            Check(sampledFit.ActionTime[0] == d.Actions[0].Time && sampledFit.ActionMemory[0] == prediction.Actions[0].Memory,
                  "unmeasured compilations keep their trace time and the model's memory");
            // A measured control is anchored so that the model reproduces its observation.
            Near(sampledFit.ActionTime[24] * sampledFit.Classes[0].Baseline + sampledFit.Classes[0].Residual, obs[24].Time,
                 "a measured control is anchored at its observation");
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
            Check(changed.Classes[0].Saved != c.Saved && changed.Classes[0].Size != c.Size,
                  "every measured group and artifact trains the final coefficients");
            Check(cvError(changed) > 1, "cross-validation scores each group without training on it");
            // A count column constant on the PCH targets is what used to
            // collide with a load shared by every group. With the residual on
            // the controls, one load for all six groups is still identified.
            for (auto& unit : d.Units) { unit.Parse = 5; }
            generate();
            auto shared = Calibrate(d, layout, prediction, obs);
            Check(shared.Classes[0].Identified && std::abs(shared.Classes[0].Load - c.Load) < 1e-9,
                  "a load identical for every group is identified without a count column");
            // Groups 0-4 alone cannot tell saved from load; the sixth can, and
            // only a fit trained on every group sees it.
            for (auto& unit : d.Units) { unit.Parse = 5; }
            for (size_t f = 0; f < 24; ++f) { d.Files[f].Own[0].second = f < 20 ? 0.5 : 2.0; }
            generate();
            auto full = Calibrate(d, layout, prediction, obs);
            Check(full.Classes[0].Identified && std::abs(full.Classes[0].Saved - (1 + (12.0 / 14) * 0.4)) < 1e-9,
                  "the group that separates saved from load trains the working fit");
            // Collinear features are not independently identified, and the
            // combined fit keeps a measured slowdown.
            for (size_t f = 0; f < 24; ++f) { d.Files[f].Own[0].second = 2.5; }
            generate();
            for (size_t f = 0; f < 24; ++f) { obs[f].Time = 22; }
            auto deficient = Calibrate(d, layout, prediction, obs);
            Check(!deficient.Classes[0].Identified, "collinear features are not independently identified");
            Check(deficient.Classes[0].Saved < 0, "combined fallback retains measured slowdowns");
            auto slower = Predict(d, layout, deficient);
            for (size_t f = 0; f < 24; ++f) {
                Near(slower.Actions[f].Time, 22, "the anchor reproduces a measured PCH compile under a slowdown fit");
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

        // A delta over a non-empty base is priced with the base's own savings
        // and re-read cost already carried, and a small frequently-changing
        // root does not join a large stable body the delta has already
        // accepted: doubling the body's rebuild rate costs more than the root
        // saves, exactly as TWorkCosts::Build prices the finished artifact.
        void PhysicalDeltaCosts() {
            auto d = Data();
            d.Q = 0;
            d.Scenarios = 4;
            d.Units.resize(4);
            d.Units[0].Include = "<base_header>";
            d.Units[0].Stable = true; // base material only
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 1;
            d.Units[0].Changes = {0};
            d.Units[1].Include = "<churny>";
            d.Units[1].Parse = 0.5;
            d.Units[1].Bytes = 1;
            d.Units[1].Changes = {2};
            d.Units[2].Include = "<feature>";
            d.Units[2].Parse = 2;
            d.Units[2].Bytes = 1;
            d.Units[2].Changes = {0};
            d.Units[3].Include = "<stable_body>";
            d.Units[3].Parse = 20;
            d.Units[3].Bytes = 1;
            d.Units[3].Changes = {0};
            const int targets = 40;
            std::vector<std::string> owners;
            for (int i = 0; i < targets; ++i) {
                const bool featureSide = i % 2;
                TAction a;
                a.Time = 10; // own parse times below stay below it
                a.Memory = 1e6;
                // Commit 2 is a consumer commit too: the churny root costs no
                // extra compiles, only the doubled rebuild rate of the body.
                a.Changes = {0, 2, 3};
                d.Actions.push_back(a);
                std::vector<std::pair<int, double>> own{{0, 2}};
                if (featureSide) {
                    own.emplace_back(2, 2.5);
                } else {
                    own.emplace_back(3, 6.5);
                    own.emplace_back(1, 0.3);
                }
                d.Files.push_back({i, 0, std::to_string(i), std::move(own)});
                owners.push_back("//x" + std::string(i < 10 ? "0" : "") + std::to_string(i));
            }
            GroupTargets(d, owners);
            auto selected = Select(d, {});
            Check(selected.Pchs.size() == 3 && selected.Pchs[0].Roots == std::vector<int>({0}) &&
                      selected.Pchs[1].Parent == 0 && selected.Pchs[1].Roots == std::vector<int>({3}) &&
                      selected.Pchs[2].Parent == 0 && selected.Pchs[2].Roots == std::vector<int>({2}),
                  "a stable body and a stable feature form deltas over the non-empty base");
            for (const auto& pch : selected.Pchs) {
                Check(std::find(pch.Roots.begin(), pch.Roots.end(), 1) == pch.Roots.end(),
                      "a churning root that would double the body's rebuild rate stays out");
            }
            // Forty owners: three controls without PCH and three on the base
            // by the label order (sqrt(40) rounds to three pairs), the feature
            // owners on their delta, the others on the base.
            std::vector<int> expected(targets);
            for (int t = 0; t < targets; ++t) {
                const bool bare = t == 0 || t == 13 || t == 26, control = t == 6 || t == 20 || t == 33;
                expected[t] = bare ? -1 : control ? 0 : t % 2 ? 2 : 1;
            }
            Check(selected.Assignment == expected,
                  "feature owners take the delta, the others the base, controls stay bare or on the base");
        }

        // The first measurement anchors every target at what S0 really took
        // for it. The sampled members of a target are a PPS draw of its
        // strata, so their observed-to-trace ratios impute the unsampled ones
        // without bias (Horvitz-Thompson). A target measured no faster with
        // its PCH than the controls are without one loses the PCH at the
        // correction; one measured far faster keeps it, with a multiplier.
        void AnchoredTargets() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(2);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 4;
            d.Units[0].Bytes = 1;
            d.Units[1].Include = "<feature>";
            d.Units[1].Parse = 6;
            d.Units[1].Bytes = 1;
            TLayout initial;
            initial.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            TPch delta;
            delta.Class = 0;
            delta.Parent = 0;
            delta.Roots = {1};
            delta.Units = {0, 1};
            initial.Pchs = {base, delta};
            // t0..t2: controls (t0 has four members, two measured). t3..t5:
            // ordinary users, exactly on the class model. t6: measured slower
            // with the PCH than the controls without one. t7: measured at a
            // tenth of the model. t8: on the delta for its base part only.
            const double time[] = {22, 20, 20, 10.2, 10.2, 10.2, 21, 1, 16.2};
            std::vector<std::string> labels;
            for (int t = 0; t < 9; ++t) {
                for (int j = 0; j < (t == 0 ? 4 : 2); ++j) {
                    TAction a;
                    a.Time = 20;
                    a.Memory = 1e6;
                    a.Output = std::to_string(10 * t + j) + ".o";
                    d.Actions.push_back(a);
                    const bool whole = t >= 3 && t != 8;
                    d.Files.push_back({int(d.Actions.size()) - 1, 0, a.Output,
                                       whole ? std::vector<std::pair<int, double>>{{0, 4}, {1, 6}}
                                             : std::vector<std::pair<int, double>>{{0, 4}}});
                    labels.push_back("//t" + std::to_string(t));
                }
            }
            GroupTargets(d, labels);
            initial.Assignment = {-1, -1, -1, 1, 1, 1, 1, 1, 1};
            auto prediction = Predict(d, initial, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            for (size_t f = 0; f < d.Files.size(); ++f) { obs[f].Time = time[d.Files[f].Target]; }
            obs[d.Files[1].Action] = {};
            obs[d.Files[3].Action] = {};
            auto fit = Calibrate(d, initial, prediction, obs);
            // Two exceptional targets of six do not drag the class: without
            // the robust weights the collinear features give 1.15 and 0.28.
            Check(std::abs(fit.Classes[0].Saved - 1) < 0.05 && std::abs(fit.Classes[0].Load - 0.02) < 0.01,
                  "a parasite and a super-beneficiary do not drag the class coefficients");
            // Horvitz-Thompson: both strata of t0 show 1.1, so its unsampled
            // members are imputed at the observed ratio, 22 s.
            for (int f : d.Targets[0].Files) {
                Near(fit.ActionTime[d.Files[f].Action], 22, "unsampled members take their stratum's observed ratio");
            }
            Near(fit.ActionFloor[d.Files[0].Action], 0.4, "a file without a frontend trace keeps the 2% prior floor");
            Check(fit.TargetGain[6] < 0.1 && fit.TargetGain[7] > 1.6,
                  "a parasite keeps a multiplier at zero, a super-beneficiary one above 1.6");
            for (int t : {3, 4, 5, 8}) {
                Check(std::abs(fit.TargetGain[t] - 1) < 0.05, "targets on the class model keep a multiplier of one");
            }
            // The anchor reproduces the observed S0 compile under the S0 leaf.
            auto s0 = Predict(d, initial, fit);
            for (int t = 0; t < 9; ++t) {
                for (int f : d.Targets[t].Files) { Near(s0.Actions[d.Files[f].Action].Time, time[t], "anchor reproduces S0"); }
            }
            auto corrected = Select(d, fit, &initial);
            // Controls and t8 parse nothing beyond the base: like every owner
            // whose residual profile no delta overlaps, they are offered the
            // base only, now priced against their measured no-PCH time.
            Check(corrected.Bases == 1 && corrected.Pchs.size() == 2 && corrected.Pchs[1].Roots == std::vector<int>{1} &&
                  corrected.Assignment == std::vector<int>({0, 0, 0, 1, 1, 1, -1, 1, 0}),
                  "the correction keeps the PCH for every measured beneficiary, controls included, and drops the parasite");
            // Anchor and delta: the controls take the base at their observed
            // no-PCH time minus the calibrated saving; the parasite's no-PCH
            // time is what S0 measured with the PCH minus next to nothing;
            // t8 moves from the delta to the base and loses only the delta's
            // extra load (16.2 - 0.2 + 0.08).
            const double expected[] = {18.08, 16.08, 16.08, 10.2, 10.2, 10.2, 0, 1, 16.08};
            auto s1 = Predict(d, corrected, fit);
            for (size_t f = 0; f < d.Files.size(); ++f) {
                const int t = d.Files[f].Target;
                const double actual = s1.Actions[d.Files[f].Action].Time;
                if (t == 6) {
                    Check(actual > 20 && actual < 21, "the parasite's anchored no-PCH time is its observed PCH time");
                } else {
                    Near(actual, expected[t], "corrected times follow the anchors");
                }
            }
            // A target S0 saw far slower with its PCH than the controls
            // without one is not credited with a negative saving: its PCH
            // saved it nothing, and its no-PCH time is what S0 measured less
            // the load, never less than a plain compile of the class.
            for (int f : d.Targets[6].Files) { obs[f].Time = 28; }
            auto slow = Calibrate(d, initial, prediction, obs);
            Near(slow.TargetGain[6], 0, "a target the PCH did not help keeps a multiplier of zero, not below");
            auto bare = corrected;
            bare.Assignment[6] = -1;
            const double withoutPch = Predict(d, bare, slow).Actions[d.Files[d.Targets[6].Files[0]].Action].Time;
            Check(withoutPch > 27 && withoutPch < 28.2, "its no-PCH time is what S0 measured, less the load");
            Check(Select(d, slow, &initial).Assignment[6] == -1, "and it loses its PCH");
        }

        // Three owners measured at a fifth of what their own parse time
        // predicts: the mean effect of their leaf is projected on its own
        // unit, and under a budget that fits one of two deltas the ordinary
        // one is evicted, not theirs.
        void SuperBeneficiaryDelta() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(3);
            for (int u = 0; u < 3; ++u) {
                d.Units[u].Include = "<unit" + std::to_string(u) + ">";
                d.Units[u].Stable = true;
                d.Units[u].Bytes = 1;
                d.Units[u].Parse = u ? 5 : 4;
            }
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            layout.Pchs = {base};
            for (int u : {1, 2}) {
                TPch delta;
                delta.Class = 0;
                delta.Parent = 0;
                delta.Roots = {u};
                delta.Units = {0, u};
                layout.Pchs.push_back(delta);
            }
            // t0..t2 controls; t3..t6 on delta 1, on the class model; t7..t9
            // on delta 2, each member measured at 2 s instead of 11.18;
            // t10..t11 on the base.
            const int leaf[] = {-1, -1, -1, 1, 1, 1, 1, 2, 2, 2, 0, 0};
            std::vector<std::string> labels;
            for (int t = 0; t < 12; ++t) {
                for (int j = 0; j < 2; ++j) {
                    TAction a;
                    a.Time = 20;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    std::vector<std::pair<int, double>> own{{0, 4}};
                    if (t >= 3 && t <= 9) { own.emplace_back(leaf[t], 5); }
                    d.Files.push_back({int(d.Actions.size()) - 1, 0, std::to_string(10 * t + j), own});
                    labels.push_back("//t" + std::string(t < 10 ? "0" : "") + std::to_string(t));
                }
            }
            GroupTargets(d, labels);
            layout.Assignment = {-1, -1, -1, 1, 1, 1, 1, 2, 2, 2, 0, 0};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            for (size_t f = 0; f < d.Files.size(); ++f) {
                const int t = d.Files[f].Target;
                if (t >= 7 && t <= 9) { obs[f].Time = 2; }
            }
            auto fit = Calibrate(d, layout, prediction, obs);
            Check(fit.UnitGain[0][2] > 1.8 && std::abs(fit.UnitGain[0][1] - 1) < 0.1 && std::abs(fit.UnitGain[0][0] - 1) < 0.1,
                  "the mean effect of a leaf lands on its own unit, the other units keep their prior");
            for (int t = 7; t <= 9; ++t) {
                // Target times units reproduces the whole deviation: the own
                // unit carries the leaf's mean effect, the multiplier the part
                // of the saving that sits in the base units.
                const double effective = fit.TargetGain[t] * (4 + 5 * fit.UnitGain[0][2]) / 9;
                Check(std::abs(effective - 2.02) < 0.1 && fit.TargetGain[t] > 1.1,
                      "target and unit multipliers add up to the measured effect");
            }
            d.Resources.Disk = 8.5; // the base and one delta of three bytes
            auto ordinary = layout;
            FitMeasuredDisk(d, ordinary, {}, {4, 4, 4}, true, [](int, int) { return true; });
            CompactLayout(ordinary);
            Check(ordinary.Assignment == std::vector<int>({-1, -1, -1, 1, 1, 1, 1, 0, 0, 0, 0, 0}) && ordinary.Pchs.size() == 2,
                  "under the class prior the delta of three owners is the cheaper one to evict");
            auto measured = layout;
            auto report = FitMeasuredDisk(d, measured, fit, {4, 4, 4}, true, [](int, int) { return true; });
            CompactLayout(measured);
            Check(measured.Assignment == std::vector<int>({-1, -1, -1, 0, 0, 0, 0, 1, 1, 1, 0, 0}) && measured.Pchs.size() == 2 &&
                  measured.Pchs[1].Roots == std::vector<int>{2} && report.find("\ndelta\t1\t") != std::string::npos,
                  "a delta of measured super-beneficiaries outbids a larger ordinary one per byte");
        }

        // A frozen base nobody uses any more is not part of the corrected
        // plan: it takes no budget and is not exported.
        void EmptyFrozenFamily() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Classes = 2;
            d.Units.resize(2);
            d.Units[0].Include = "<harmful>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 4;
            d.Units[0].Bytes = 1;
            d.Units[1].Include = "<useful>";
            d.Units[1].Parse = 6;
            d.Units[1].Bytes = 1;
            TLayout initial;
            initial.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            initial.Pchs = {base};
            std::vector<std::string> labels;
            for (int t = 0; t < 6; ++t) {
                for (int j = 0; j < 2; ++j) {
                    TAction a;
                    a.Time = 20;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    // The last three owners also parse what the second class's delta serializes.
                    d.Files.push_back({int(d.Actions.size()) - 1, 0, std::to_string(10 * t + j),
                                       t < 3 ? std::vector<std::pair<int, double>>{{0, 4}}
                                             : std::vector<std::pair<int, double>>{{0, 4}, {1, 6}}});
                    labels.push_back("//t" + std::to_string(t));
                }
            }
            GroupTargets(d, labels);
            initial.Assignment.assign(6, 0);
            auto prediction = Predict(d, initial, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            // Everyone measured slower with the base than without it.
            for (size_t f = 0; f < d.Files.size(); ++f) { obs[f].Time = 24; }
            auto fit = Calibrate(d, initial, prediction, obs);
            TSelectionDiagnostics diag;
            auto corrected = Select(d, fit, &initial, 1, &diag);
            Check(corrected.Pchs.empty() && corrected.Bases == 0 && corrected.Assignment == std::vector<int>(6, -1),
                  "a frozen base without a single beneficiary leaves the corrected plan");
            // It leaves while the candidates are scored, not afterwards: no
            // candidate paid for building it, and the reported groups are the plan's.
            Check(diag.Widths.find("scored_pchs=0 ") != std::string::npos && diag.Widths.find("probe_pchs=0 ") != std::string::npos,
                  "an emptied frozen family is never scored as built");
            Check(diag.Widths.find("scored_bytes=0 ") != std::string::npos, "nor counted in the scored bytes");

            // A frozen base of another class is empty: its 500 bytes do not
            // count against the budget of the family that is alive.
            d.Classes = 2;
            TLayout two;
            two.Bases = 2;
            two.Pchs.resize(3);
            two.Pchs[0].Class = 0;
            two.Pchs[0].Roots = two.Pchs[0].Units = {0};
            two.Pchs[1].Class = 1;
            two.Pchs[1].Roots = two.Pchs[1].Units = {0};
            two.Pchs[2].Class = 1;
            two.Pchs[2].Parent = 1;
            two.Pchs[2].Roots = {1};
            two.Pchs[2].Units = {0, 1};
            two.Assignment = {-1, -1, -1, 2, 2, 2};
            for (auto& file : d.Files) { file.Class = file.Target < 3 ? 0 : 1; }
            GroupTargets(d, labels);
            d.Resources.Disk = 160;
            FitMeasuredDisk(d, two, {}, {500, 50, 100}, true, [](int, int) { return true; });
            CompactLayout(two);
            Check(two.Pchs.size() == 2 && two.Pchs[0].Roots == std::vector<int>{0} && two.Pchs[0].Class == 1 &&
                  two.Assignment == std::vector<int>({-1, -1, -1, 1, 1, 1}),
                  "an empty frozen family reserves no disk and leaves the plan");
        }
        // What the PCH removes beyond the includes (-fpch-instantiate-
        // templates) calibrates on the time a file spends in semantics outside
        // them: Frontend minus the outermost includes, relative to them.
        void TemplateInstantiation() {
            auto d = Data();
            TLayout layout;
            for (int g = 0; g < 6; ++g) {
                TUnit unit;
                unit.Parse = g + 1;
                unit.Bytes = 1;
                d.Units.push_back(unit);
                TPch pch;
                pch.Class = 0;
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
                    file.Outer = 10;
                    file.Frontend = 10.0 + g; // g tenths of the include time outside the includes
                    d.Actions.push_back(action);
                    d.Files.push_back(file);
                }
            }
            for (int j = 0; j < 4; ++j) {
                TAction a;
                a.Time = 10.0 * (1 << j);
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
                const int f = target.Files.front();
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
                const int g = layout.Assignment[d.Files[f].Target];
                const double saved = d.Files[f].Own.empty() ? 0 : d.Files[f].Own[0].second;
                obs[f].Time = g < 0 ? d.Actions[f].Time + 0.5
                                    : 20 - 1.4 * saved - 0.5 * saved * (d.Files[f].Frontend - d.Files[f].Outer) / d.Files[f].Outer + 0.03 * (g + 1) + 0.5;
            }
            auto fit = Calibrate(d, layout, prediction, obs);
            const auto& c = fit.Classes[0];
            const double weight = 12.0 / 14;
            Check(c.Identified, "the instantiation feature is identified next to saved and load");
            Near(c.Saved, 1 + weight * 0.4, "saved time with the instantiation feature");
            Near(c.Load, 0.02 + weight * 0.01, "load with the instantiation feature");
            Near(c.Instantiate, weight * 0.5 / 1.4, "the saving outside the includes calibrates per unit of frontend time beyond them");
            // What the class learned generalizes: a member of a target S0 never
            // measured is priced with the same feature, three template-heavy
            // targets among six keeping their deltas only because of it.
            auto s0 = Predict(d, layout, fit);
            for (size_t f = 0; f < 24; ++f) {
                Near(s0.Actions[f].Time, obs[f].Time, "the anchor reproduces S0 with the feature");
            }
        }

        // A trace that shows the frontend leaves the backend as the floor of
        // a compile: no PCH removes the LLVM work of a member.
        void BackendFloor() {
            auto d = Data();
            d.Units.resize(1);
            d.Units[0].Parse = 100;
            d.Units[0].Include = "<all>";
            d.Units[0].Stable = true;
            TAction a;
            a.Time = 20;
            a.Memory = 1e6;
            d.Actions.push_back(a);
            d.Files.push_back({0, 0, "a.cpp", {{0, 19.9}}, 0, 18.0, 0.0});
            GroupTargets(d, {"//a:lib"});
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            layout.Pchs = {base};
            layout.Assignment = {0};
            TCalibration calibration;
            calibration.Classes.resize(1);
            calibration.Classes[0].Load = 0;
            Near(Predict(d, layout, calibration).Actions[0].Time, 2, "the backend is the floor of a compile whose includes a PCH removes");
            d.Files[0].Frontend = 0;
            Near(Predict(d, layout, calibration).Actions[0].Time, 0.4, "without a frontend trace the floor is the 2% prior");
        }

        // Strata are cut exactly as SampleFiles cut them, floor(j * n / k).
        // With five members and two measured they are {0, 1} and {2, 3, 4};
        // grouping by floor(i * k / n) would make them {0, 1, 2} and {3, 4},
        // weigh a winner's ratio with members outside its stratum and lose
        // the unbiased estimate (33 s instead of 60 s for the third member).
        void StratumImputation() {
            auto d = Data();
            TLayout layout;
            std::vector<std::string> labels;
            for (int i = 0; i < 5; ++i) {
                TAction a;
                a.Time = 10.0 * (i + 1);
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({i, 0, std::to_string(i), {}});
                labels.push_back("//t:owner");
            }
            GroupTargets(d, labels);
            layout.Assignment = {-1};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            obs[1].Time = 22; // the draw's winner of {0, 1}: ratio 1.1
            obs[3].Time = 80; // the draw's winner of {2, 3, 4}: ratio 2.0
            auto fit = Calibrate(d, layout, prediction, obs);
            const double expected[] = {11, 22, 60, 80, 100};
            for (int f = 0; f < 5; ++f) {
                Near(fit.ActionTime[f] * fit.Classes[0].Baseline, expected[f], "strata are cut as the draw cut them");
            }
        }

        // The empirical Bayes step compares a target with the compile time
        // the selector computes for it, backend floor included. A member
        // whose PCH removes almost its whole frontend is measured at that
        // floor; the linear model without it would fall below the measurement
        // and read the difference as a lost saving.
        void FlooredModel() {
            auto d = Data();
            d.Units.resize(2);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 10;
            d.Units[0].Bytes = 1;
            d.Units[1].Include = "<heavy>";
            d.Units[1].Stable = true;
            d.Units[1].Parse = 19.5;
            d.Units[1].Bytes = 1;
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = {0, 1};
            base.Units = {0, 1};
            layout.Pchs = {base};
            std::vector<std::string> labels;
            // c0..c2: controls. f: floored, its frontend is 18 of its 20 trace
            // seconds, so its backend floor is 2 s and it was measured at it.
            // o0..o2: ordinary users, exactly on the class model (20 - 10 +
            // 0.02 * 29.5); f: measured at its floor, 2 s.
            const char* names[] = {"//c0", "//c1", "//c2", "//f", "//o0", "//o1", "//o2"};
            for (const char* name : names) {
                const bool control = name[2] == 'c', floored = name[2] == 'f';
                for (int j = 0; j < 2; ++j) {
                    TAction a;
                    a.Time = 20;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    TFile file;
                    file.Action = int(d.Actions.size()) - 1;
                    file.Class = 0;
                    file.Source = std::string(name) + std::to_string(j);
                    if (!control) { file.Own = {{floored ? 1 : 0, floored ? 19.5 : 10}}; }
                    file.Frontend = floored ? 18 : 0;
                    d.Files.push_back(file);
                    labels.push_back(name);
                }
            }
            GroupTargets(d, labels);
            layout.Assignment = {-1, -1, -1, 0, 0, 0, 0};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            for (size_t f = 0; f < d.Files.size(); ++f) {
                const bool control = d.Files[f].Source[2] == 'c', floored = d.Files[f].Source[2] == 'f';
                obs[f].Time = control ? 20 : floored ? 2 : 10.59;
            }
            auto fit = Calibrate(d, layout, prediction, obs);
            Near(fit.Classes[0].Saved, 1, "the class coefficients survive a member measured at its floor");
            Near(fit.Classes[0].Load, 0.02, "and its load");
            Near(fit.TargetGain[3], 1, "a member measured at the backend floor is on the model, not below it");
            for (int t : {4, 5, 6}) { Near(fit.TargetGain[t], 1, "ordinary users keep a multiplier of one"); }
        }

        // The anchor inverts the same compile-time formula the selector
        // uses, slowdown clamp included. Under a combined fit that shows a
        // slowdown, a member parsing none of the PCH's headers owes it only
        // its load: the clamp cancels that apparent gain, and so must the
        // anchor, or the S0 layout no longer reproduces what was measured.
        void SlowdownAnchor() {
            auto d = Data();
            d.Units.resize(1);
            d.Units[0].Include = "<h>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 5;
            d.Units[0].Bytes = 1;
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            layout.Pchs = {base};
            std::vector<std::string> labels;
            // c0, c1: controls. p0..p2: two members each, the first parses the
            // header, the second only loads the PCH; both measured slower
            // with it than the controls are without one.
            for (const char* name : {"//c0", "//c1", "//p0", "//p1", "//p2"}) {
                const bool control = name[2] == 'c';
                for (int j = 0; j < (control ? 1 : 2); ++j) {
                    TAction a;
                    a.Time = 20;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    TFile file;
                    file.Action = int(d.Actions.size()) - 1;
                    file.Class = 0;
                    file.Source = std::string(name) + std::to_string(j);
                    if (!control && j == 0) { file.Own = {{0, 5}}; }
                    d.Files.push_back(file);
                    labels.push_back(name);
                }
            }
            GroupTargets(d, labels);
            layout.Assignment = {-1, -1, 0, 0, 0};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            for (size_t f = 0; f < d.Files.size(); ++f) {
                if (d.Files[f].Source[2] == 'c') { obs[f].Time = 20; }
                else { obs[f].Time = d.Files[f].Own.empty() ? 21 : 24; }
            }
            auto fit = Calibrate(d, layout, prediction, obs);
            Check(!fit.Classes[0].Identified && fit.Classes[0].Saved < 0, "a PCH that slowed its users down is fitted as one");
            auto s0 = Predict(d, layout, fit);
            for (size_t f = 0; f < d.Files.size(); ++f) {
                Near(s0.Actions[d.Files[f].Action].Time, obs[f].Time, "the anchor reproduces S0 under a slowdown fit");
            }
        }

        // The seconds of a compile outside its traces (process and driver
        // startup) are never negative. Controls that would fit a negative
        // intercept leave the class on the ratio alone.
        void NegativeResidual() {
            auto d = Data();
            TLayout layout;
            std::vector<std::string> labels;
            const double time[] = {1, 2, 10}, seen[] = {0.3, 0.8, 4.8}; // 0.5 * time - 0.2
            for (int i = 0; i < 3; ++i) {
                TAction a;
                a.Time = time[i];
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({i, 0, std::to_string(i), {}});
                labels.push_back("//c" + std::to_string(i));
            }
            GroupTargets(d, labels);
            layout.Assignment = {-1, -1, -1};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (int i = 0; i < 3; ++i) {
                obs[i].Time = seen[i];
                obs[i].Memory = prediction.Actions[i].Memory;
            }
            auto fit = Calibrate(d, layout, prediction, obs);
            Near(fit.Classes[0].Residual, 0, "a negative intercept is noise, not a saving outside the traces");
            Near(fit.Classes[0].Baseline, 0.4, "the class keeps the median ratio of its controls");
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
        // The disk price trims a fat low-density tail from a base instead of
        // losing the whole family to an all-or-nothing eviction.
        void DiskPricedTrimming() {
            auto d = Data();
            d.Units.resize(2);
            d.Units[0].Include = "<dense>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 1e6;
            d.Units[1].Include = "<fat>";
            d.Units[1].Stable = true;
            d.Units[1].Parse = 1;
            d.Units[1].Bytes = 100e6;
            for (int f = 0; f < 4; ++f) {
                TAction a;
                a.Time = 50;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, 0, std::to_string(f), {{0, 30}, {1, 2}}});
            }
            TestTargets(d);
            d.Resources.Disk = 8e6; // fits the dense root, not the fat tail
            auto selected = Select(d, {});
            Check(selected.Pchs.size() == 1 && selected.Pchs[0].Roots == std::vector<int>({0}),
                  "a disk-priced candidate keeps the dense prefix and drops the fat tail");
            Check(selected.Assignment == std::vector<int>({-1, 0, 0, 0}),
                  "trimmed base still serves every owner except the calibration control");
        }

        // Owners with the same heavy-header profile share one delta even when
        // their labels sort far apart: interleaved strangers do not split the
        // profile into chunks, and a mixed delta loses to the false-rebuild
        // penalties of members it does not fit.
        void ClusteredAcrossAlphabet() {
            auto d = Data();
            d.Q = 0;
            d.Scenarios = 2;
            d.Units.resize(3);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 1;
            d.Units[1].Include = "<feature_a>";
            d.Units[1].Parse = 2;
            d.Units[1].Bytes = 1;
            d.Units[1].Changes = {0};
            d.Units[2].Include = "<feature_b>";
            d.Units[2].Parse = 2;
            d.Units[2].Bytes = 1;
            d.Units[2].Changes = {1};
            // Labels alternate profiles across the whole alphabet; an owner
            // rebuilds exactly on its own feature's commit.
            const int targets = 36;
            std::vector<std::string> owners;
            for (int i = 0; i < targets; ++i) {
                const int unit = i % 2 ? 2 : 1;
                TAction a;
                a.Time = 100;
                a.Memory = 1e6;
                a.Changes = {unit - 1};
                d.Actions.push_back(a);
                d.Files.push_back({i, 0, std::to_string(i), {{0, 1}, {unit, 30}}});
                owners.push_back("//x" + std::string(i < 10 ? "0" : "") + std::to_string(i));
            }
            GroupTargets(d, owners);
            auto selected = Select(d, {});
            Check(selected.Pchs.size() == 3 &&
                  selected.Pchs[1].Roots == std::vector<int>({1}) &&
                  selected.Pchs[2].Roots == std::vector<int>({2}),
                  "profile clustering yields one delta per heavy-header profile, not label-chunk duplicates");
            // Thirty-six owners: controls at 0, 12 and 24, base controls at 6, 18 and 30.
            std::vector<int> expected(targets);
            for (int t = 0; t < targets; ++t) {
                const bool bare = t == 0 || t == 12 || t == 24, control = t == 6 || t == 18 || t == 30;
                expected[t] = bare ? -1 : control ? 0 : t % 2 ? 2 : 1;
            }
            Check(selected.Assignment == expected,
                  "owners land on the delta matching their profile");
        }

        // A near-duplicate delta evicts into its sibling, and the unique
        // specialized delta survives the same budget cut.
        void SiblingFallbackEviction() {
            auto d = Data();
            d.Units.resize(3);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 1;
            d.Units[1].Include = "<shared_feature>";
            d.Units[1].Parse = 2;
            d.Units[1].Bytes = 1;
            d.Units[2].Include = "<unique_feature>";
            d.Units[2].Parse = 4;
            d.Units[2].Bytes = 2;
            const int unit[] = {1, 1, 1, 1, 2, 2};
            const char* owners[] = {"//dup:a", "//dup:a", "//dup:b", "//dup:b", "//unique:u", "//unique:u"};
            std::vector<std::string> labels;
            for (int f = 0; f < 6; ++f) {
                TAction a;
                a.Time = 50;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, 0, std::to_string(f), {{0, 1}, {unit[f], unit[f] == 1 ? 10 : 9}}});
                labels.push_back(owners[f]);
            }
            GroupTargets(d, labels);
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Representative = 0;
            base.Roots = base.Units = {0};
            layout.Pchs.push_back(base);
            for (int g : {1, 2, 3}) {
                TPch delta;
                delta.Class = 0;
                delta.Parent = 0;
                delta.Representative = g == 3 ? 4 : g - 1;
                delta.Roots = {g == 3 ? 2 : 1};
                delta.Units = {0, g == 3 ? 2 : 1};
                layout.Pchs.push_back(delta);
            }
            layout.Assignment = {1, 2, 3}; // dup:a -> delta A, dup:b -> delta B, unique -> delta C
            d.Resources.Disk = 13; // base(4) + two of the three deltas (3,3,6)
            auto merged = layout;
            auto report = FitMeasuredDisk(d, merged, {}, {4, 3, 3, 6}, false, [](int, int) { return true; });
            CompactLayout(merged);
            Check(merged.Assignment == std::vector<int>({1, 1, 2}) && merged.Pchs.size() == 3 &&
                  report.find("\ndelta\t1\t") != std::string::npos,
                  "a near-duplicate delta merges into its sibling; the unique delta survives");
        }
        // A sibling fallback never crosses compiler classes: a delta of
        // another class may share the same roots, but its PCH cannot serve
        // these files, and an assignment to it would fail prediction.
        void CrossClassSiblings() {
            auto d = Data();
            d.Classes = 2;
            d.Units.resize(3);
            d.Units[0].Include = "<base0>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 1;
            d.Units[1].Include = "<base1>";
            d.Units[1].Stable = true;
            d.Units[1].Parse = 1;
            d.Units[1].Bytes = 1;
            d.Units[2].Include = "<shared>";
            d.Units[2].Parse = 2;
            d.Units[2].Bytes = 2;
            std::vector<std::string> owners;
            for (int f = 0; f < 4; ++f) {
                const int cls = f / 2;
                TAction a;
                a.Time = 50;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                // A well-covered base keeps both families more expensive to
                // evict per byte than their deltas.
                d.Files.push_back({f, cls, std::to_string(f), {{cls, 8}, {2, 10}}});
                owners.push_back(cls ? "//cls1:t" + std::to_string(f) : "//cls0:t" + std::to_string(f));
            }
            GroupTargets(d, owners);
            TLayout layout;
            layout.Bases = 2;
            for (int b = 0; b < 2; ++b) {
                TPch base;
                base.Class = b;
                base.Representative = 2 * b;
                base.Roots = base.Units = {b};
                layout.Pchs.push_back(base);
            }
            for (int g = 0; g < 2; ++g) {
                TPch delta;
                delta.Class = g;
                delta.Parent = g;
                delta.Representative = 2 * g + 1;
                delta.Roots = {2};
                delta.Units = {g, 2};
                layout.Pchs.push_back(delta);
            }
            layout.Assignment = {2, 2, 3, 3};
            d.Resources.Disk = 12; // base0+delta0+base1 fit; the class-1 delta does not
            auto merged = layout;
            FitMeasuredDisk(d, merged, {}, {4, 4, 3, 6}, false, [](int, int) { return true; });
            CompactLayout(merged);
            Check(merged.Assignment == std::vector<int>({2, 2, 1, 1}) && merged.Pchs.size() == 3,
                  "an evicted delta falls back inside its own compiler class");
            bool compatible = true;
            try { Predict(d, merged, {}); } catch (const TError&) { compatible = false; }
            Check(compatible, "no cross-class assignment survives disk eviction");
        }

        // Duplicate deltas merge at payback: the weaker copy is priced against
        // its still-open sibling, hands its owners over and disappears —
        // instead of both being admitted against the base alone.
        void DuplicateDeltaMerge() {
            auto d = Data();
            d.Units.resize(2);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 1;
            d.Units[1].Include = "<feature>";
            d.Units[1].Parse = 2;
            d.Units[1].Bytes = 1;
            for (int f = 0; f < 6; ++f) {
                TAction a;
                a.Time = 10;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, 0, std::to_string(f), {{0, 1}, {1, 6}}});
            }
            TestTargets(d);
            TLayout frozen;
            frozen.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Representative = 0;
            base.Roots = base.Units = {0};
            frozen.Pchs.push_back(base);
            for (int g = 0; g < 2; ++g) {
                TPch delta;
                delta.Class = 0;
                delta.Parent = 0;
                delta.Representative = g;
                delta.Roots = {1};
                delta.Units = {0, 1};
                frozen.Pchs.push_back(delta);
            }
            frozen.Assignment = {1, 1, 1, 2, 2, 2};
            TSelectionDiagnostics diag;
            auto selected = Select(d, {}, &frozen, 1, &diag);
            Check(selected.Pchs.size() == 2 && selected.Assignment == std::vector<int>(6, 1),
                  "a duplicate delta hands its owners to its sibling and disappears");
            Check(diag.Targets.find("delta-payback-sibling") != std::string::npos,
                  "the weaker duplicate is priced against its open sibling, not the base");
        }

        // The per-evaluation scratch (deltaSaved) must never leak into an
        // acceptance: the last candidate evaluated while seeding the queue
        // used to leave its savings behind, and the first accepted root folded
        // them instead of its own — crediting files with a header the PCH does
        // not hold and flattering the extra-compile penalty of later roots.
        void PickScratchIsolation() {
            auto d = Data();
            d.Q = .5;
            d.Scenarios = 2;
            d.Units.resize(3);
            for (int u = 0; u < 3; ++u) {
                d.Units[u].Include = "<unit" + std::to_string(u) + ">";
                d.Units[u].Stable = true;
                d.Units[u].Bytes = 1;
            }
            d.Units[0].Parse = 1; // accepted first: the densest root
            d.Units[1].Parse = 1;
            d.Units[1].Changes = {0}; // extra compiles for every consumer
            d.Units[2].Parse = 60; // far too expensive to build: always rejected
            for (int f = 0; f < 4; ++f) {
                TAction a;
                a.Time = 10;
                a.Memory = 1e6;
                a.Changes = {1}; // consumers rebuild on scenario 1 only
                d.Actions.push_back(a);
                // own: 2 for the cheap roots, 9 for the expensive last one.
                d.Files.push_back({f, 0, std::to_string(f), {{0, 2}, {1, 2}, {2, 9}}});
            }
            TestTargets(d);
            auto selected = Select(d, {});
            Check(selected.Pchs.size() == 1 && selected.Pchs[0].Roots == std::vector<int>({0}),
                  "an accepted root folds only its own savings; another root's extra compiles stay fully priced");
        }

        // A delta rooted at an umbrella header must be found by targets that
        // parse only the nested headers it transitively serializes.
        void UmbrellaRootSibling() {
            auto d = Data();
            d.Units.resize(3);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 1;
            d.Units[1].Include = ""; // nested header: never an injected root
            d.Units[1].Parse = 5;
            d.Units[1].Bytes = 1;
            d.Units[2].Include = "<umbrella>";
            d.Units[2].Children = {1};
            d.Units[2].Parse = 0.5;
            d.Units[2].Bytes = 1;
            const int targets = 30;
            std::vector<std::string> owners;
            for (int i = 0; i < targets; ++i) {
                TAction a;
                a.Time = 10;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                // Five owners include the umbrella directly; the rest parse
                // only the nested header it pulls in.
                std::vector<std::pair<int, double>> own{{0, 1}, {1, 8}};
                if (i < 5) {
                    own.emplace_back(2, 0.5);
                }
                d.Files.push_back({i, 0, std::to_string(i), std::move(own)});
                owners.push_back("//x" + std::string(i < 10 ? "0" : "") + std::to_string(i));
            }
            GroupTargets(d, owners);
            auto selected = Select(d, {});
            Check(selected.Pchs.size() == 2 && selected.Pchs[1].Roots == std::vector<int>({2}) &&
                      selected.Pchs[1].Units == std::vector<int>({0, 1, 2}),
                  "the umbrella root serializes the nested header transitively");
            for (size_t t = 0; t < selected.Assignment.size(); ++t) {
                // Thirty owners: controls without PCH at 0, 10 and 20, kept on
                // the base at 5, 15 and 25.
                Check(selected.Assignment[t] == (t % 10 == 0 ? -1 : t % 10 == 5 ? 0 : 1),
                      "targets parsing only the nested header still find the umbrella delta");
            }
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
        TemplateInstantiation();
        BackendFloor();
        StratumImputation();
        FlooredModel();
        SlowdownAnchor();
        NegativeResidual();
        AnchoredTargets();
        SuperBeneficiaryDelta();
        EmptyFrozenFamily();
        CoChangedRoots();
        PhysicalDeltaCosts();
        HeldOutParentCalibration();
        DiskPricedTrimming();
        ClusteredAcrossAlphabet();
        SiblingFallbackEviction();
        CrossClassSiblings();
        DuplicateDeltaMerge();
        UmbrellaRootSibling();
        PickScratchIsolation();
        AtomicTargets();
        IoSelfTest();
        Report("two-step self-tests: OK");
        return 0;
    }
} // namespace NPchUpdate::NTwoStep
