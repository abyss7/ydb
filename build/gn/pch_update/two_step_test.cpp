#include "two_step.h"
#include "checks.h"
#include "common.h"
#include "ninja.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <tuple>
#include <utility>

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
            r.Pools["link"] = 1;
            graph[0].Pool = graph[1].Pool = "link";
            Near(Makespan(graph, r), 10, "pool admission");
            graph[0].Pool = graph[1].Pool = "";
            // RAM and bandwidth schedule nothing, as in a Ninja build: more
            // concurrent memory than the budget admits, and an I/O volume the
            // bandwidth would stretch over ten seconds, leave the schedule.
            r.Ram = 10;
            r.Bandwidth = 1;
            graph[0].Io = graph[1].Io = 30;
            Near(Makespan(graph, r), 7, "RAM and bandwidth are no schedule inputs");
            graph[0].Memory = 21;
            Check(std::isfinite(Makespan(graph, r)), "an action above the RAM is not infeasible: the build decides");
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

        // A generator made of several owners: no single move breaks the cycle
        // each of its targets closes with the delta holding the generated
        // header, so they move one at a time. They land on the parent, which
        // does not hold the header, not on NONE: the base still serves them.
        void GeneratorCycleKeepsBase() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(2);
            d.Units[0].Parse = d.Units[1].Parse = 4;
            d.Units[1].Producers = {0, 1}; // the delta's header comes from both generator targets
            d.Actions.resize(3);
            for (int i = 0; i < 3; ++i) {
                d.Actions[i].Time = 20;
                d.Files.push_back({i, 0, std::to_string(i), {{0, 4}, {1, 4}}});
            }
            GroupTargets(d, {"//a:gen_one", "//a:gen_two", "//b:consumer"});
            TLayout l;
            l.Bases = 1;
            l.Pchs = {{0, -1, 0, {0}, {0}}, {0, 0, 0, {1}, {0, 1}}};
            l.Assignment = {1, 1, 1};
            auto p = ScoreCandidate(d, l, {}, 1, 32);
            Check(p.Feasible && l.Assignment == std::vector<int>{0, 0, 1} && l.Pchs.size() == 2,
                  "the targets of a multi-target generator keep the base when their delta is cyclic");
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
            FitMeasuredDisk(d, refused, {}, {99, 2}, refused.Bases, [](int, int) { return false; });
            for (size_t t = 0; t < refused.Assignment.size(); ++t) {
                if (corrected.Assignment[t] == 1) { Check(refused.Assignment[t] == -1, "incompatible fallback uses NONE"); }
            }
            FitMeasuredDisk(d, corrected, {}, {99, 2}, corrected.Bases, [](int, int) { return true; });
            auto remap = CompactLayout(corrected);
            Check(remap == std::vector<int>({0,-1}) && corrected.Assignment == initial.Assignment,
                  "an over-budget measured delta falls back atomically to its profitable base");
            bool tooLarge = false;
            try { FitMeasuredDisk(d, corrected, {}, {101}, corrected.Bases, [](int, int) { return true; }); }
            catch (const TError&) { tooLarge = true; }
            Check(tooLarge, "fail explicitly when mandatory measured bases exceed the budget");
        }

        // A frozen base is frozen in content, not in cost. Measured, its build
        // may cost more than the family saves on it: the correction then drops
        // the family instead of building a base for nothing.
        void FrozenBasePayback() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(1);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 4;
            d.Units[0].Bytes = 1;
            TLayout initial;
            initial.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            initial.Pchs = {base};
            for (int t = 0; t < 4; ++t) {
                for (int j = 0; j < 2; ++j) {
                    TAction a;
                    a.Time = 20;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    d.Files.push_back({int(d.Actions.size()) - 1, 0, std::to_string(10 * t + j), {{0, 1}}});
                }
            }
            TestTargets(d, 2);
            initial.Assignment.assign(4, 0);
            auto prediction = Predict(d, initial, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            // The base saves its eight members under eight seconds together.
            obs[d.Actions.size()].Time = 500;
            auto fit = Calibrate(d, initial, prediction, obs);
            auto corrected = Select(d, fit, &initial);
            Check(corrected.Pchs.empty() && corrected.Assignment == std::vector<int>(4, -1),
                  "a frozen base the family does not pay for leaves the corrected plan");
            obs[d.Actions.size()].Time = 1;
            fit = Calibrate(d, initial, prediction, obs);
            corrected = Select(d, fit, &initial);
            Check(corrected.Bases == 1 && corrected.Pchs.size() == 1 && corrected.Assignment == std::vector<int>(4, 0),
                  "a frozen base the family pays for stays");
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
            d.Actions.resize(4);
            d.Files.resize(4);
            // Four equal owners, the smallest class with a bare control. t0
            // helps design both headers but stays without PCH: only the other
            // three pay for what they keep.
            for (int i = 0; i < 4; ++i) {
                d.Actions[i].Time = 20;
                d.Files[i].Action = i;
                d.Files[i].Class = 0;
                d.Files[i].Own = {{0, 2.0}, {1, 1.5}};
            }
            TestTargets(d);
            TSelectionDiagnostics diag;
            auto baseOnly = Select(d, {}, nullptr, 1, &diag);
            Check(baseOnly.Pchs.size() == 1 && baseOnly.Assignment == std::vector<int>({-1, 0, 0, 0}) &&
                  diag.Payback.find("\tfamily\t0\t3\t") != std::string::npos &&
                  diag.Payback.find("\tdelta\t") == std::string::npos,
                  "delta cannot use the control's parsing savings to pay for its build");
            // The child pays for itself on three owners, the base for by none:
            // under the build priors (base 1.1, delta 1.75) the base needs
            // 1.18 per owner of four to be picked and 1.55 to pay for by three.
            for (int i = 0; i < 4; ++i) { d.Files[i].Own = {{0, 1.3}, {1, 3.1}}; }
            auto none = Select(d, {}, nullptr, 1, &diag);
            Check(none.Pchs.empty() && diag.Payback.find("drop-assignments") != std::string::npos,
                  "profitable child does not cover the cost of the whole family");
            for (int i = 0; i < 4; ++i) { d.Files[i].Own = {{0, 1.3}, {1, 4}}; }
            auto family = Select(d, {}, nullptr, 1, &diag);
            Check(family.Pchs.size() == 2 && family.Assignment == std::vector<int>({-1, 1, 1, 1}),
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
            // A base and one delta of the size prior fit the budget, two deltas do not.
            const double k = TCoefficients().Size;
            d.Resources.Disk = 2 * k * 1e6 + 2e5;
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
                  diag.Widths.find(Format("scored_budget_bytes=%.0f scored_pchs=2 scored_bytes=%.0f probe_budget_bytes=%.0f probe_pchs=3 probe_bytes=%.0f",
                                          d.Resources.Disk, 2 * k * 1e6, 2 * d.Resources.Disk, 3 * k * 1e6)) != std::string::npos,
                  "width scored with one delta at 1x; winner probes both deltas within 2x, charging the base only once");
            auto before = full.Assignment;
            auto families = full;
            d.Resources.Disk = 500000;
            auto evictions = FitMeasuredDisk(d, families, {}, {1000000, 1000000, 5000000}, 0, [](int, int) { return true; });
            CompactLayout(families);
            Check(families.Pchs.empty() && evictions.find("\nfamily\t0\t") != std::string::npos,
                  "oversized initial base can be evicted with all remaining owners on measured sizes");
            d.Resources.Disk = 6e6;
            FitMeasuredDisk(d, full, {}, {1000000, 1000000, 5000000}, 0, [](int, int) { return true; });
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
            Check(!prior.Classes[0].Identified && prior.Classes[0].Baseline == 1 && prior.Classes[0].Residual == 0 &&
                      prior.Classes[0].Size == TCoefficients().Size && prior.Classes[0].Saved == 1,
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
            FitMeasuredDisk(d, ordinary, {}, {4, 4, 4}, ordinary.Bases, [](int, int) { return true; });
            CompactLayout(ordinary);
            Check(ordinary.Assignment == std::vector<int>({-1, -1, -1, 1, 1, 1, 1, 0, 0, 0, 0, 0}) && ordinary.Pchs.size() == 2,
                  "under the class prior the delta of three owners is the cheaper one to evict");
            auto measured = layout;
            auto report = FitMeasuredDisk(d, measured, fit, {4, 4, 4}, measured.Bases, [](int, int) { return true; });
            CompactLayout(measured);
            Check(measured.Assignment == std::vector<int>({-1, -1, -1, 0, 0, 0, 0, 1, 1, 1, 0, 0}) && measured.Pchs.size() == 2 &&
                  measured.Pchs[1].Roots == std::vector<int>{2} && report.find("\ndelta\t1\t") != std::string::npos,
                  "a delta of measured super-beneficiaries outbids a larger ordinary one per byte");
        }

        // A leaf whose owners S0 saw far slower than the class model gets a
        // saved-time multiplier of zero. Zero is a measurement, not the mark
        // of a unit nobody measured: for an owner S0 did not see the leaf then
        // saves nothing, where the mark would credit it with the full class
        // saving of the leaf.
        void HarmfulLeafGain() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(3);
            for (int u = 0; u < 3; ++u) {
                d.Units[u].Include = u ? "<leaf" + std::to_string(u) + ">" : "<base>";
                d.Units[u].Parse = u ? 5 : 4;
                d.Units[u].Bytes = 1;
            }
            d.Units[0].Stable = true;
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
            // t0..t2: controls at their trace time. t3..t8: on leaf 1, exactly
            // on the class model. t9..t11: on leaf 2, measured at 60 s. t12: on
            // leaf 2 too, never measured.
            const int leaf[] = {-1, -1, -1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2};
            std::vector<std::string> labels;
            for (int t = 0; t < 13; ++t) {
                for (int j = 0; j < 2; ++j) {
                    TAction a;
                    a.Time = 20;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    TFile file;
                    file.Action = int(d.Actions.size()) - 1;
                    file.Class = 0;
                    if (leaf[t] >= 0) { file.Own = {{0, 4}, {leaf[t], 5}}; }
                    d.Files.push_back(file);
                    labels.push_back("//t" + std::to_string(100 + t));
                }
            }
            GroupTargets(d, labels);
            layout.Assignment.assign(leaf, leaf + 13);
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            for (size_t f = 0; f < d.Files.size(); ++f) {
                const int t = d.Files[f].Target;
                if (t >= 9) { obs[f].Time = t <= 11 ? 60 : 0; }
            }
            auto fit = Calibrate(d, layout, prediction, obs);
            Check(fit.UnitGain[0].size() == 3 && fit.UnitGain[0][2] == 0 && fit.UnitGain[0][1] > 0,
                  "a leaf that slowed its owners down is calibrated at a multiplier of zero, a good leaf above zero");
            const auto& c = fit.Classes[0];
            const int plain = d.Targets[12].Files[0];
            Near(Predict(d, layout, fit).Actions[plain].Time, 20 - 4 * c.Saved + 9 * c.Load,
                 "an owner S0 never measured is credited with nothing from the harmful leaf");
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
            FitMeasuredDisk(d, two, {}, {500, 50, 100}, two.Bases, [](int, int) { return true; });
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

        // A base of twenty small dense headers and five large thin ones under
        // a budget that fits the small ones and one of the large. The disk
        // price is cut where the bytes of the roots, densest first, reach the
        // budget: the small ones and the large one stay. A quantile by count
        // of roots lands among the small ones and drops every large one, with
        // most of the budget unused.
        void DiskPricedByBytes() {
            auto d = Data();
            d.Units.resize(25);
            for (int u = 0; u < 25; ++u) {
                const bool small = u < 20;
                d.Units[u].Include = small ? "<small" + std::to_string(u) + ">" : "<large" + std::to_string(u) + ">";
                d.Units[u].Stable = true;
                d.Units[u].Parse = small ? 0.5 : 3.0 - 0.1 * (u - 20);
                d.Units[u].Bytes = small ? 100000 : 5000000;
            }
            for (int i = 0; i < 8; ++i) {
                TAction a;
                a.Time = 60;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                std::vector<std::pair<int, double>> own;
                for (int u = 0; u < 25; ++u) { own.emplace_back(u, d.Units[u].Parse); }
                d.Files.push_back({i, 0, std::to_string(i), std::move(own)});
            }
            TestTargets(d);
            // The small headers and one large one at the size prior, not two.
            d.Resources.Disk = (20 * 100000 + 1.5 * 5000000) * TCoefficients().Size;
            TSelectionDiagnostics diag;
            auto selected = Select(d, {}, nullptr, 1, &diag);
            Check(selected.Bases == 1 && selected.Pchs.size() == 1 && selected.Pchs[0].Roots.size() == 21 &&
                      std::find(selected.Pchs[0].Roots.begin(), selected.Pchs[0].Roots.end(), 20) != selected.Pchs[0].Roots.end(),
                  "the disk price keeps every small header and the large one that fits the budget with them");
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
            auto report = FitMeasuredDisk(d, merged, {}, {4, 3, 3, 6}, 0, [](int, int) { return true; });
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
            FitMeasuredDisk(d, merged, {}, {4, 4, 3, 6}, 0, [](int, int) { return true; });
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
                // Thirty owners, the first five a little heavier: they include
                // the umbrella itself, and none of them carries the class. The
                // controls are spread evenly over all thirty, light and heavy
                // alike: without PCH at 0, 10 and 20, kept on the base at 5,
                // 15 and 25.
                const bool bare = t == 0 || t == 10 || t == 20, control = t == 5 || t == 15 || t == 25;
                Check(selected.Assignment[t] == (bare ? -1 : control ? 0 : 1),
                      "targets parsing only the nested header still find the umbrella delta");
            }
        }

        // A pure umbrella: no own time at all, it only includes headers that
        // cannot stand alone. The files parse it, so it is a root candidate,
        // and through it their time in the nested headers reaches a delta.
        void ZeroOwnUmbrellaRoot() {
            auto d = Data();
            d.Units.resize(3);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[1].Include = ""; // nested: never an injected root
            d.Units[2].Include = "<umbrella>";
            d.Units[2].Children = {1};
            for (int u = 0; u < 3; ++u) {
                d.Units[u].Parse = u == 2 ? 0 : u ? 8 : 1;
                d.Units[u].Bytes = 1;
            }
            for (int i = 0; i < 8; ++i) {
                TAction a;
                a.Time = 10;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({i, 0, std::to_string(i), {{0, 1}, {1, 8}, {2, 0}}});
            }
            TestTargets(d);
            auto selected = Select(d, {});
            Check(selected.Bases == 1 && selected.Pchs.size() == 2 && selected.Pchs[1].Roots == std::vector<int>({2}) &&
                      selected.Pchs[1].Units == std::vector<int>({0, 1, 2}),
                  "an umbrella the files parse with no own time of its own is still a root");
        }

        // A header with one change in a hundred commits is base material: the
        // rebuilds it causes are priced, and they cost next to nothing. A
        // cold-only objective does not count changes at all. A short history
        // keeps the strict rule, and a header most commits touch stays out.
        void ChangedHeaderBaseMaterial() {
            auto d = Data();
            d.Q = .5;
            d.Units.resize(2);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 2;
            d.Units[1].Include = "<rare>";
            d.Units[1].Parse = 6;
            d.Units[1].Changes = {0, 3};
            for (int u = 0; u < 2; ++u) { d.Units[u].Bytes = 1; }
            for (int i = 0; i < 8; ++i) {
                TAction a;
                a.Time = 10;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({i, 0, std::to_string(i), {{0, 2}, {1, 6}}});
            }
            TestTargets(d, 2);
            auto roots = [](TLayout layout) {
                CompactLayout(layout);
                auto result = layout.Pchs.empty() ? std::vector<int>() : layout.Pchs[0].Roots;
                std::sort(result.begin(), result.end());
                return result;
            };
            d.Scenarios = 100;
            Check(roots(Select(d, {})) == std::vector<int>({0, 1}), "a header changed in two commits of a hundred is base material");
            d.Scenarios = 50;
            Check(roots(Select(d, {})) == std::vector<int>({0}), "the same share of commits is not rare in a short history");
            d.Scenarios = 100;
            d.Units[1].Changes = {0, 1, 2, 3, 4};
            Check(roots(Select(d, {})) == std::vector<int>({0}), "a header most builds rebuild is not base material");
            d.Q = 1;
            Check(roots(Select(d, {})) == std::vector<int>({0, 1}), "a cold-only objective does not count rebuilds");
            // Generated output is never base material, however rarely its
            // source changed: the history lists it with every commit touching
            // the source, which makes it look like any other changed header.
            d.Q = .5;
            d.Units[1].Changes = {0, 3};
            d.Units[1].Producers = {0};
            d.Units[1].Headers = {"<out>/gen/thing.pb.h"};
            Check(roots(Select(d, {})) == std::vector<int>({0}), "generated output is no base material when its source changed");
            d.Q = 1;
            Check(roots(Select(d, {})) == std::vector<int>({0}), "nor under a cold-only objective");
            d.Q = .5;
            d.Units[1].Producers.clear();
            d.Units[1].Headers = {"util/directory/rare.h"};
            Check(roots(Select(d, {})) == std::vector<int>({0, 1}), "a header of util/ that rarely changes is base material");
        }

        // Two compiler classes. The main one has a base and two deltas too
        // large together for the budget, one of a dense and a thin root, the
        // other a base and a tiny delta of a very high density. The disk price
        // of the candidate is cut where the bytes of all its roots add up to
        // the budget, and the main class keeps its dense root. Its densities
        // used to be cleared when the second class was constructed: the price
        // read off the tiny delta of that class alone then cut nothing, and
        // eviction dropped the dense delta for the thinner one.
        void DensitiesSpanClasses() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Classes = 2;
            d.Units.resize(6);
            // 0: the base of the first class. 1: a dense root of its first
            // cluster, 3: a thin root of the same. 2: the root of its second
            // cluster. 4 and 5: the base and a delta of the second class.
            const char* include[] = {"<base>", "<dense>", "<second>", "<thin>", "<other>", "<tiny>"};
            const double parse[] = {1, 10, 5, 1, 1, 5};
            const double bytes[] = {1, 1e6, 1e6, 2e6, 1, 1};
            for (int u = 0; u < 6; ++u) {
                d.Units[u].Include = include[u];
                d.Units[u].Parse = parse[u];
                d.Units[u].Bytes = bytes[u];
            }
            d.Units[0].Stable = d.Units[4].Stable = true;
            std::vector<std::string> labels;
            for (int t = 0; t < 40; ++t) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({t, 0, std::to_string(t),
                                   t < 20 ? std::vector<std::pair<int, double>>{{0, 1}, {1, 10}, {3, 1}}
                                          : std::vector<std::pair<int, double>>{{0, 1}, {2, 5}}});
                labels.push_back("//a" + std::to_string(100 + t));
            }
            for (int t = 0; t < 4; ++t) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({40 + t, 1, std::to_string(40 + t), {{4, 1}, {5, 5}}});
                labels.push_back("//b" + std::to_string(t));
            }
            GroupTargets(d, labels);
            d.Resources.Disk = (1e6 + 10) * TCoefficients().Size; // the base of the main class and one dense delta
            auto selected = Select(d, {});
            bool dense = false;
            for (size_t g = 1; g < selected.Pchs.size(); ++g) {
                if (selected.Pchs[g].Class != 0) { continue; }
                Check(selected.Pchs[g].Roots == std::vector<int>({1}),
                      "under the disk price the main class keeps its dense root, without the thin one or the other delta");
                dense = true;
            }
            Check(dense && selected.Bases == 2, "the disk price of a candidate is read off the roots of every class of it");
        }

        // A base control demoted from its cluster's delta in the interim round
        // may keep none after the re-pick: rebuilt from the others alone, the
        // delta no longer pays for it, and it stays on the base as an ordinary
        // owner. Its role is the one of the final round, none: the sample then
        // treats it like any other owner.
        void DemotedControlRole() {
            auto d = Data();
            d.Units.resize(3);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[1].Include = "<shared>";
            d.Units[1].Parse = 5;
            d.Units[2].Include = "<controlOnly>";
            d.Units[2].Parse = 5;
            std::vector<std::string> labels;
            // Eight owners of ten files each and one weight: the first is the
            // bare control, the fifth the base control, the only one the second
            // header saves anything to.
            for (int t = 0; t < 8; ++t) {
                for (int j = 0; j < 10; ++j) {
                    TAction a;
                    a.Time = 20;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    std::vector<std::pair<int, double>> own{{0, 1}};
                    if (t < 4) {
                        own.emplace_back(1, 5);
                    } else {
                        own.emplace_back(2, t == 4 ? 5 : 0.2);
                    }
                    d.Files.push_back({int(d.Actions.size()) - 1, 0, std::to_string(d.Actions.size()), std::move(own)});
                    labels.push_back("//t" + std::to_string(t));
                }
            }
            GroupTargets(d, labels);
            auto selected = Select(d, {});
            Check(selected.Bases == 1 && selected.Pchs.size() == 2 && selected.Pchs[1].Roots == std::vector<int>({1}) &&
                      selected.Controls[0] == 1 && selected.Controls[4] == 0 && selected.Assignment[4] == 0,
                  "a base control that keeps no delta after the re-pick is no control any more");
        }

        // A target S0 saw save nothing with any PCH never takes one again. Its
        // files must not design the delta of its cluster: they bring their load
        // and no saving, enough to fail a small delta the targets that would
        // use it do pay for.
        void ParasiteLeavesClusterAlone() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Q = .5;
            d.Units.resize(3);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[1].Include = "<small>";
            d.Units[2].Include = "<other>";
            for (int u = 0; u < 3; ++u) {
                d.Units[u].Parse = u == 2 ? 5 : u == 1 ? 6 : 1;
                d.Units[u].Bytes = 1;
            }
            std::vector<std::string> labels;
            // Thirty-one owners of a small header, one parasite parsing it in
            // three hundred files, thirty-two owners of another one.
            for (int t = 0; t < 64; ++t) {
                const int count = t == 31 ? 300 : 1;
                for (int j = 0; j < count; ++j) {
                    TAction a;
                    a.Time = 10;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    const int unit = t < 32 ? 1 : 2;
                    d.Files.push_back({int(d.Actions.size()) - 1, 0, std::to_string(d.Actions.size()), {{0, 2}, {unit, t == 31 ? 6 : unit == 1 ? 0.5 : 5}}});
                    labels.push_back("//t" + std::to_string(100 + t));
                }
            }
            GroupTargets(d, labels);
            TLayout frozen;
            frozen.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            frozen.Pchs = {base};
            frozen.Assignment.assign(64, 0);
            TCalibration fit;
            fit.Classes.resize(1);
            fit.TargetGain.assign(64, 1.0);
            fit.TargetGain[31] = 0;
            auto selected = Select(d, fit, &frozen);
            int small = -1;
            for (size_t g = 0; g < selected.Pchs.size(); ++g) {
                if (selected.Pchs[g].Roots == std::vector<int>({1})) { small = int(g); }
            }
            Check(selected.Bases == 1 && selected.Pchs.size() == 3 && small >= 0 &&
                      std::count(selected.Assignment.begin(), selected.Assignment.begin() + 31, small) == 31 &&
                      selected.Assignment[31] == -1,
                  "a delta is designed for the owners that can take it, without the files of a parasite");
        }

        // Four clusters, three around a heavy header that also pulls in two
        // nested headers. For the fourth cluster those two are most of what
        // it shares with the others, and its own lean delta overlaps it less
        // than any of theirs: by raw overlap it loses their three and never
        // sees the one built for it. Its own delta is always among its
        // choices, and the others rank by what they net after their load.
        void OwnClusterDeltaChosen() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(7);
            // 0: the base. 1 and 2: nested headers, no roots of their own, the
            // first common to every cluster. 3, 4, 5: a heavy header per
            // cluster, pulling both in. 6: a lean umbrella of the first.
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[1].Include = "";
            d.Units[1].Parse = 4;
            d.Units[2].Include = "";
            d.Units[2].Parse = 45;
            for (int u = 3; u <= 5; ++u) {
                d.Units[u].Include = "<heavy" + std::to_string(u) + ">";
                d.Units[u].Children = {1, 2};
                d.Units[u].Parse = 500;
            }
            d.Units[6].Include = "<lean>";
            d.Units[6].Children = {1};
            d.Units[6].Parse = 1;
            for (int u = 0; u < 7; ++u) { d.Units[u].Bytes = 1; }
            std::vector<std::string> labels;
            for (int t = 0; t < 100; ++t) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                const int group = t / 25;
                d.Files.push_back({t, 0, std::to_string(t), {{0, 1}, {1, 5}, {2, 45}, {group < 3 ? 3 + group : 6, group < 3 ? 40 : 1}}});
                labels.push_back("//t" + std::to_string(1000 + t));
            }
            GroupTargets(d, labels);
            // A load ten times the prior: a heavy delta is worth its own
            // cluster's while and a loss for everybody else.
            TCalibration fit;
            fit.Classes.resize(1);
            fit.Classes[0].Load = .1;
            fit.Classes[0].DeltaBuild = .1;
            auto selected = Select(d, fit);
            Check(selected.Bases == 1 && selected.Pchs.size() == 5, "a base and a delta per cluster");
            // One hundred owners: without PCH at 0, 20, ..., kept on the base
            // at 10, 30, ..., the rest on the delta of their cluster.
            const int root[] = {3, 4, 5, 6};
            for (int t = 0; t < 100; ++t) {
                int own = -1;
                for (size_t g = 1; g < selected.Pchs.size(); ++g) {
                    if (selected.Pchs[g].Roots == std::vector<int>({root[t / 25]})) { own = int(g); }
                }
                Check(own >= 0 && selected.Assignment[t] == (t % 20 == 0 ? -1 : t % 10 == 0 ? 0 : own),
                      "every cluster takes the delta built for it, whatever it shares with the others");
            }
        }

        // Nine clusters around heavy churning headers that all pull in the two
        // nested headers the tenth cluster parses. For a target of that tenth
        // cluster each of the nine nets more than its own lean delta, whose
        // extra header it does not parse: they fill the whole shortlist. Its
        // own delta is still among its choices, and it is the one that pays:
        // the others rebuild on every second commit.
        void LeanDeltaBeyondSiblings() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Q = .5;
            d.Scenarios = 2;
            const int noisy = 9, perCluster = 32, targets = 10 * perCluster;
            d.Units.resize(3 + noisy + 1);
            // 0: the base. 1 and 2: nested headers, no roots of their own.
            // 3..11: a churning header per noisy cluster, pulling both in.
            // 12: a lean umbrella of the first for the quiet cluster.
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[1].Include = "";
            d.Units[1].Parse = 4;
            d.Units[2].Include = "";
            d.Units[2].Parse = 5;
            for (int u = 0; u < noisy; ++u) {
                d.Units[3 + u].Include = "<churn" + std::to_string(u) + ">";
                d.Units[3 + u].Children = {1, 2};
                d.Units[3 + u].Parse = 4;
                d.Units[3 + u].Changes = {0};
            }
            d.Units.back().Include = "<lean>";
            d.Units.back().Children = {1};
            d.Units.back().Parse = 1;
            for (auto& unit : d.Units) { unit.Bytes = 1; }
            std::vector<std::string> labels;
            for (int t = 0; t < targets; ++t) {
                TAction a;
                a.Time = 100;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                const int group = t / perCluster;
                std::vector<std::pair<int, double>> own{{0, 1}, {1, 5}, {2, 5}};
                if (group < noisy) {
                    own.emplace_back(3 + group, 40);
                } else {
                    own.emplace_back(int(d.Units.size()) - 1, 1);
                }
                d.Files.push_back({t, 0, std::to_string(t), std::move(own)});
                labels.push_back("//t" + std::to_string(1000 + t));
            }
            GroupTargets(d, labels);
            TCalibration fit;
            fit.Classes.resize(1);
            fit.Classes[0].Load = .1;
            fit.Classes[0].DeltaBuild = .1;
            auto selected = Select(d, fit);
            Check(selected.Bases == 1 && selected.Pchs.size() == 11, "a base and a delta per cluster");
            // Three hundred and twenty owners, all of one weight: six without
            // PCH and six kept on the base, the rest on the delta of their cluster.
            std::vector<int> expected(targets, -2);
            for (size_t k = 0; k < 6; ++k) {
                expected[k * targets / 6] = -1;
                if (expected[(2 * k + 1) * targets / 12] == -2) { expected[(2 * k + 1) * targets / 12] = 0; }
            }
            const int roots[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, int(d.Units.size()) - 1};
            for (int t = 0; t < targets; ++t) {
                int own = -1;
                for (size_t g = 1; g < selected.Pchs.size(); ++g) {
                    if (selected.Pchs[g].Roots == std::vector<int>({roots[t / perCluster]})) { own = int(g); }
                }
                Check(own >= 0 && selected.Assignment[t] == (expected[t] == -2 ? own : expected[t]),
                      "the lean delta of the quiet cluster is not crowded out of its own shortlist");
            }
        }

        // A lone owner made a bare calibration control kept no PCH at all:
        // nobody was left to pay for its base, the base left the initial plan,
        // and the correction, which never builds a base, found no parent for
        // a delta. A class of fewer than four owners has no bare control: the
        // owner takes its PCH, S0 measures the artifacts, and the correction
        // refines on the frozen base.
        void LoneOwnerClass() {
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
            for (int f = 0; f < 20; ++f) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, 0, std::to_string(f), {{0, 4}, {1, 6}}});
            }
            TestTargets(d, 20);
            TSelectionDiagnostics diag;
            auto initial = Select(d, {}, nullptr, 1, &diag);
            Check(initial.Bases == 1 && initial.Pchs.size() == 2 && initial.Assignment == std::vector<int>({1}) &&
                      initial.Controls == std::vector<char>({0}) && diag.Targets.find("calibration-control") == std::string::npos,
                  "a lone owner takes its base and delta instead of staying without PCH as a calibration control");
            auto prediction = Predict(d, initial, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            auto fit = Calibrate(d, initial, prediction, obs);
            Check(fit.Measured.size() == 2, "S0 builds and measures both artifacts of a small class");
            auto corrected = Select(d, fit, &initial, 1, &diag);
            Check(corrected.Bases == 1 && corrected.Pchs.size() == 2 && corrected.Assignment == std::vector<int>({1}) &&
                      diag.Targets.find("no-candidate") == std::string::npos,
                  "the correction finds the frozen base of the lone owner and keeps its delta");
        }

        // Two owners whose base pays off only together: neither is a bare
        // control, so the base survives the initial plan for both.
        void PairedOwnersClass() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(1);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 10;
            d.Units[0].Bytes = 1;
            for (int f = 0; f < 2; ++f) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, 0, std::to_string(f), {{0, 10}}});
            }
            TestTargets(d);
            auto initial = Select(d, {});
            Check(initial.Bases == 1 && initial.Pchs.size() == 1 && initial.Assignment == std::vector<int>({0, 0}) &&
                      initial.Controls == std::vector<char>({0, 0}),
                  "two owners keep the base that neither of them pays for alone");
            auto prediction = Predict(d, initial, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            auto fit = Calibrate(d, initial, prediction, obs);
            auto corrected = Select(d, fit, &initial);
            Check(corrected.Bases == 1 && corrected.Pchs.size() == 1 && corrected.Assignment == std::vector<int>({0, 0}),
                  "the correction serves both owners from their frozen base");
        }

        // Four owners, one of them carrying the class: the bare control comes
        // from the lighter half of the class, never from its heaviest owner,
        // whose PCH pays for a base the others cannot afford alone.
        void HeavyOwnerKeepsBase() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(2);
            for (int u = 0; u < 2; ++u) {
                d.Units[u].Include = "<base" + std::to_string(u) + ">";
                d.Units[u].Stable = true;
                d.Units[u].Parse = u ? 300 : 100;
                d.Units[u].Bytes = 1;
            }
            std::vector<std::string> labels;
            for (int f = 0; f < 13; ++f) {
                TAction a;
                a.Time = 500;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                // The heavy owner parses both headers in ten files; each light
                // owner parses the cheap one in a single file.
                std::vector<std::pair<int, double>> own{{0, 100}};
                if (f < 10) {
                    own.emplace_back(1, 300);
                }
                d.Files.push_back({f, 0, std::to_string(f), std::move(own)});
                labels.push_back("//t" + std::to_string(f < 10 ? 0 : f - 9));
            }
            GroupTargets(d, labels);
            auto initial = Select(d, {});
            Check(initial.Bases == 1 && initial.Pchs.size() == 1 && initial.Assignment == std::vector<int>({0, -1, 0, 0}) &&
                      initial.Controls == std::vector<char>({0, 1, 0, 0}),
                  "the bare control is a light owner: the heaviest one stays to pay for the class base");
            auto prediction = Predict(d, initial, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            auto fit = Calibrate(d, initial, prediction, obs);
            Check(fit.Measured.size() == 1, "S0 builds and measures the base the heavy owner paid for");
            auto corrected = Select(d, fit, &initial);
            Check(corrected.Bases == 1 && corrected.Pchs.size() == 1 && corrected.Assignment == std::vector<int>({0, 0, 0, 0}),
                  "the correction gives the measured control its base back");
        }

        // The compiler's speed against the traces and a compile's seconds
        // outside them belong to the machine. A class without one control
        // used to start from the ratio 1 and no overhead at all, and a host
        // compiling short files for twice their traces made a PCH that saved
        // a quarter of them read as one that saved nothing.
        void CommonLineOfTheControls() {
            auto d = Data();
            d.Classes = 2;
            d.Units.resize(1);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 1;
            std::vector<std::string> labels;
            // Four short files of one owner on the PCH: half a second in the
            // traces each, half a second outside them, a quarter saved.
            for (int j = 0; j < 4; ++j) {
                TAction a;
                a.Time = 0.5;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({int(d.Actions.size()) - 1, 0, "short" + std::to_string(j), {{0, 0.25}}});
                labels.push_back("//short");
            }
            // Two controls of another class: the same half second outside the traces.
            for (double time : {1.0, 20.0}) {
                TAction a;
                a.Time = time;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({int(d.Actions.size()) - 1, 1, "control" + std::to_string(int(time)), {}});
                labels.push_back("//control" + std::to_string(int(time)));
            }
            GroupTargets(d, labels);
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            layout.Pchs = {base};
            layout.Assignment = {-1, -1, 0}; // targets in label order: the controls, then the owner
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            for (int f = 0; f < 4; ++f) { obs[f].Time = 0.5 + 0.5 - 0.25 + 0.02; }
            obs[4].Time = 1.5;
            obs[5].Time = 20.5;
            auto fit = Calibrate(d, layout, prediction, obs);
            const auto& c = fit.Classes[0];
            Check(c.Saved > 0.9 && c.Saved < 1.1, "a class without controls reads the saving of its PCH at the common rate: " + std::to_string(c.Saved));
            Check(std::abs(c.Residual - 0.5) < 1e-3, "and pays a compile's seconds outside the traces: " + std::to_string(c.Residual));
            TLayout bare = layout;
            bare.Assignment[2] = -1;
            const auto without = Predict(d, bare, fit);
            for (int f = 0; f < 4; ++f) {
                Check(std::abs(without.Actions[f].Time - 1.0) < 1e-3,
                      "without the PCH the short files take their trace time and the overhead: " + std::to_string(without.Actions[f].Time));
            }
        }

        // A 2 MB base on a 1200 s owner: its combined gain is next to nothing
        // against the noise of its target, and the speed of the host against
        // the traces is not a saving. The median of the row ratios divided
        // one by the other and clamped at three times the class baseline, and
        // every new delta of such a class was priced as saving three times
        // its parse seconds. A row weighs as its gain stands to that noise.
        void TinyGainKeepsTheScale() {
            auto d = Data();
            d.Classes = 2;
            d.Units.resize(1);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 20;
            d.Units[0].Bytes = 1;
            std::vector<std::string> labels;
            // One owner of the first class on the small base: 1200 s in the
            // traces, a host compiling at 0.8 of them, 20 s saved.
            TAction a;
            a.Time = 1200;
            a.Memory = 1e6;
            d.Actions.push_back(a);
            d.Files.push_back({0, 0, "owner", {{0, 20}}});
            labels.push_back("//owner");
            // Two controls of the second class: this host compiles large
            // files at 0.9 of their traces.
            for (double time : {1200.0, 2400.0}) {
                TAction control;
                control.Time = time;
                control.Memory = 1e6;
                d.Actions.push_back(control);
                d.Files.push_back({int(d.Actions.size()) - 1, 1, "control" + std::to_string(int(time)), {}});
                labels.push_back("//control" + std::to_string(int(time)));
            }
            GroupTargets(d, labels);
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            layout.Pchs = {base};
            layout.Assignment = {-1, -1, 0}; // targets in label order: the controls, then the owner
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            obs[1].Time = 1080;
            obs[2].Time = 2160;
            obs[0].Time = 0.8 * 1200 - 20 + 0.4;
            auto fit = Calibrate(d, layout, prediction, obs);
            Check(fit.Classes[0].Saved > 0.8 && fit.Classes[0].Saved < 1.0,
                  "a row whose gain the noise of its target swallows leaves the prior scale: " + std::to_string(fit.Classes[0].Saved));
            // The same owner with a delta that saves 300 of its trace seconds:
            // the row carries its scale, as far as its information goes.
            d.Units[0].Parse = 300;
            d.Files[0].Own = {{0, 300}};
            prediction = Predict(d, layout, {});
            obs[0].Time = 0.8 * 1200 - 0.8 * 300 + 6;
            fit = Calibrate(d, layout, prediction, obs);
            Check(fit.Classes[0].Saved > 0.95 && fit.Classes[0].Saved < 1.05,
                  "a row with a gain its noise cannot swallow moves the scale: " + std::to_string(fit.Classes[0].Saved));
        }

        // The bytes a header adds to a PCH are the header's. A class that
        // measured an artifact knows its own units; a unit it never measured
        // takes what any class measured, not the scale of whatever small
        // artifact that class did measure.
        void PooledUnitSize() {
            auto d = Data();
            d.Classes = 2;
            d.Units.resize(2);
            for (int u = 0; u < 2; ++u) {
                d.Units[u].Include = "<unit" + std::to_string(u) + ">";
                d.Units[u].Stable = true;
                d.Units[u].Parse = 1;
                d.Units[u].Bytes = 1;
            }
            for (int f = 0; f < 2; ++f) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, f, std::to_string(f), {{f, 1}}});
            }
            TestTargets(d);
            TLayout layout;
            layout.Bases = 2;
            TPch first;
            first.Class = 0;
            first.Representative = 0;
            first.Roots = first.Units = {0};
            TPch second;
            second.Class = 1;
            second.Representative = 1;
            second.Roots = second.Units = {1};
            layout.Pchs = {first, second};
            layout.Assignment = {0, 1};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            obs[d.Actions.size()].Bytes = 8;
            obs[d.Actions.size() + 1].Bytes = 2;
            auto fit = Calibrate(d, layout, prediction, obs);
            Near(fit.UnitSize[1][1], 2, "the units a class measured itself keep what it measured");
            Check(std::abs(fit.Classes[1].Size - TCoefficients().Size) < 1e-6,
                  "with next to no bytes behind it a class's scale is the common one, not its artifact's: " + std::to_string(fit.Classes[1].Size));
            Check(std::abs(fit.GlobalUnitSize[0] - 8) < 1e-6 && std::abs(fit.GlobalUnitSize[1] - 2) < 1e-6,
                  "the artifacts of every class decompose together");
            TLayout wide = layout;
            wide.Pchs[1].Roots = wide.Pchs[1].Units = {0, 1};
            Check(std::abs(Predict(d, wide, fit).PchBytes[1] - 10) < 1e-6,
                  "a unit the class never measured takes the pooled bytes: " + std::to_string(Predict(d, wide, fit).PchBytes[1]));
        }

        // The classes the initial plan left without a base are not lost to the
        // correction: it builds one for each of them, priced as the initial
        // plan prices a base. Such a base is not frozen: the measured fit
        // evicts it like any initial base, where the initial plan's own bases
        // must fit.
        void CorrectionBuildsOpenBases() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Classes = 2;
            d.Units.resize(2);
            for (int u = 0; u < 2; ++u) {
                d.Units[u].Include = "<base" + std::to_string(u) + ">";
                d.Units[u].Stable = true;
                d.Units[u].Parse = 5;
                d.Units[u].Bytes = 1;
            }
            std::vector<std::string> labels;
            for (int f = 0; f < 7; ++f) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, f < 4 ? 0 : 1, std::to_string(f), {{f < 4 ? 0 : 1, 5}}});
                labels.push_back(f < 4 ? "//kept" : "//open");
            }
            GroupTargets(d, labels);
            TLayout initial;
            initial.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Representative = 0;
            base.Roots = base.Units = {0};
            initial.Pchs = {base};
            initial.Assignment = {0, -1};
            TSelectionDiagnostics diag;
            auto corrected = Select(d, {}, &initial, 1, &diag);
            Check(corrected.Bases == 2 && corrected.Pchs.size() == 2 && corrected.Frozen == 1 &&
                      corrected.Pchs[1].Class == 1 && corrected.Pchs[1].Roots == std::vector<int>({1}) &&
                      corrected.Assignment == std::vector<int>({0, 1}),
                  "the correction builds a base for the class the initial plan left without one");
            Check(diag.Widths.find("\nseed\t1\t1\t") != std::string::npos && diag.Widths.find("\nnone\t0\t0\t0\t") != std::string::npos &&
                      diag.Widths.find("cold_seconds\tincremental_seconds\tobjective_seconds") != std::string::npos,
                  "the width report shows the build without PCH and the initial plan on the correction's model, with cold and incremental");
            d.Resources.Disk = 15;
            auto fitted = corrected;
            // Ten bytes each: the initial plan's base has to fit, the new one
            // is evicted like any initial base.
            const auto report = FitMeasuredDisk(d, fitted, {}, {10, 10}, fitted.Frozen, [](int, int) { return true; });
            CompactLayout(fitted);
            Check(fitted.Bases == 1 && fitted.Pchs.size() == 1 && fitted.Assignment == std::vector<int>({0, -1}) &&
                      report.find("\nfamily\t1\t") != std::string::npos,
                  "a base the correction built competes for the disk; only the initial plan's bases must fit");
        }

        // The layout of the payback test: a base, a thin delta of one parse
        // second and a wide one of ten; eight thin owners on the thin delta and
        // eight wide ones on the wide one.
        std::pair<TData, TLayout> ThinAndWide() {
            auto d = Data();
            d.Units.resize(3);
            for (int u = 0; u < 3; ++u) {
                d.Units[u].Include = "<unit" + std::to_string(u) + ">";
                d.Units[u].Stable = u == 0;
                d.Units[u].Parse = u == 1 ? 1 : u == 2 ? 10 : 4;
                d.Units[u].Bytes = 1;
            }
            std::vector<std::string> labels;
            for (int t = 0; t < 2; ++t) {
                for (int j = 0; j < 8; ++j) {
                    TAction a;
                    a.Time = 20;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    std::vector<std::pair<int, double>> own{{0, 4}, {2, 10}};
                    if (t == 0) { own.emplace_back(1, 0.1); }
                    d.Files.push_back({int(d.Actions.size()) - 1, 0, std::to_string(t * 8 + j), own});
                    labels.push_back(t == 0 ? "//thin" : "//wide");
                }
            }
            GroupTargets(d, labels);
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
            layout.Assignment = {1, 2}; // the thin owner on the thin delta, the wide one on the wide one
            return {std::move(d), std::move(layout)};
        }

        // Priced again on its final leaf, an owner leaves a delta that no
        // longer pays for its parent or for NONE, never for a sibling nobody
        // checked it with.
        void FinalPaybackStepsDown() {
            auto dw = ThinAndWide();
            const auto report = FinalPayback(dw.first, dw.second, {});
            Check(dw.second.Assignment == std::vector<int>({0, 2}) && report.find("drop-assignments") != std::string::npos,
                  "an owner whose delta no longer pays steps down to its parent, not over to a sibling");
        }

        // What a layout is worth under a calibration, as the correction
        // compares its candidates with the plan it started from.
        void ScoredLayoutOfAPlan() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Units.resize(2);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 4;
            d.Units[1].Include = "<delta>";
            d.Units[1].Parse = 6;
            for (int f = 0; f < 20; ++f) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, 0, std::to_string(f), {{0, 4}, {1, 6}}});
            }
            TestTargets(d, 20);
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            TPch delta;
            delta.Class = 0;
            delta.Parent = 0;
            delta.Roots = {1};
            delta.Units = {0, 1};
            layout.Pchs = {base, delta};
            layout.Assignment = {1};
            double objective = -1;
            const auto scored = ScoredLayout(d, layout, {}, 1, &objective);
            Near(objective, Predict(d, layout, {}).Objective, "a layout that pays scores what its schedule scores");
            Check(scored.Bases == 1 && scored.Pchs.size() == 2 && scored.Assignment == std::vector<int>({1}) && scored.Frozen == 1,
                  "with every base of it frozen");
            // The artifacts a plan built were measured: a delta twice the
            // budget is not estimated into it.
            TLayout onBase = layout;
            onBase.Assignment = {0};
            CompactLayout(onBase);
            const std::vector<uint64_t> measured = {uint64_t(1), uint64_t(2 * d.Resources.Disk)};
            double fitted = -1;
            const auto evicted = ScoredLayout(d, layout, {}, 1, &fitted, &measured);
            Near(fitted, Predict(d, onBase, {}).Objective, "a measured delta over the budget leaves, and its owner pays for the base alone");
            Check(evicted.Bases == 1 && evicted.Pchs.size() == 1 && evicted.Assignment == std::vector<int>({0}),
                  "the fit of a scored layout takes the measured sizes");
            double kept = -1;
            const std::vector<uint64_t> small = {1, 1};
            ScoredLayout(d, layout, {}, 1, &kept, &small);
            Near(kept, Predict(d, layout, {}, 1, small).Objective, "a measured delta that fits stays, scored at its measured size");
        }

        // The frozen bases already hold their bytes of the budget. A price cut
        // where the delta roots reach a share of the whole budget let as many
        // delta bytes again past it, to be evicted wholesale afterwards;
        // counted from the bases, it cuts them to a share of what is left.
        void GridCountsFrozenBases() {
            auto d = Data();
            d.Resources.Workers = 1;
            d.Resources.Disk = 100;
            d.Units.resize(4);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            for (int u = 1; u < 4; ++u) {
                d.Units[u].Include = "<leaf" + std::to_string(u) + ">";
                d.Units[u].Parse = 11 - u;
                d.Units[u].Bytes = 5; // 31 bytes each as an artifact: three do not fit next to a base of 60 in 100
            }
            std::vector<std::string> labels;
            for (int f = 0; f < 30; ++f) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, 0, std::to_string(f), {{0, 1}, {1 + f / 10, double(10 - f / 10)}}});
                labels.push_back("//t" + std::to_string(100 + f));
            }
            GroupTargets(d, labels);
            TLayout initial;
            initial.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Representative = 0;
            base.Roots = base.Units = {0};
            initial.Pchs = {base};
            initial.Assignment.assign(30, 0);
            TCalibration fit;
            fit.Classes.resize(1);
            TMeasuredPch measured;
            measured.Pch = base;
            measured.Bytes = 60;
            measured.Time = 1;
            fit.Measured.push_back(measured);
            TSelectionDiagnostics diag;
            Select(d, fit, &initial, 1, &diag);
            size_t priced = 0;
            for (const auto& line : Split(diag.Widths, '\n')) { priced += line.starts_with("32@lambda="); }
            // Held 60 of 100: nothing reaches 60%; one root reaches 100%; two reach 125%.
            Check(priced == 2, "the price grid cuts the deltas to a share of what the frozen bases leave: " + std::to_string(priced));
        }

        // A class that built no base or no delta of its own builds at the
        // rate of the classes that did. The prior of 1 stood while every
        // measured class built its base for a tenth longer than its parse
        // seconds.
        void UnmeasuredClassBuildsLikeTheOthers() {
            auto d = Data();
            d.Classes = 3;
            d.Units.resize(1);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 10;
            d.Units[0].Bytes = 1;
            std::vector<std::string> labels;
            // One owner of 300 s on a base that took 12 s to build for 10 s
            // of parse, and one owner of 300 s of another class without any PCH.
            for (int f = 0; f < 2; ++f) {
                TAction a;
                a.Time = 300;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, f == 0 ? 0 : 2, f == 0 ? "a-owner" : "b-bare", f == 0 ? std::vector<std::pair<int, double>>{{0, 20}} : std::vector<std::pair<int, double>>{}});
                labels.push_back(f == 0 ? "//a-owner" : "//b-bare");
            }
            GroupTargets(d, labels);
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            layout.Pchs = {base};
            layout.Assignment = {0, -1};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            obs[d.Actions.size()].Time = 12;
            auto fit = Calibrate(d, layout, prediction, obs);
            Near(fit.Classes[0].BaseBuild, 1.2, "a class measures the build of its own base");
            Near(fit.Classes[1].BaseBuild, 1.2, "a class without targets builds at the rate of the classes that did");
            Near(fit.Classes[2].BaseBuild, 1.2, "a class that built no base builds at the rate of the classes that did, not at the prior");
            Near(fit.Classes[0].DeltaBuild, TCoefficients().DeltaBuild, "a class that built no delta, and no class did, keeps the prior");
        }

        // One or two controls of a class are its owners' noise as much as the
        // class's speed: they move the common line by their own ratio to it,
        // by the share their number carries, and no further.
        void FewControlsMoveTheCommonLine() {
            auto d = Data();
            d.Classes = 2;
            d.Units.resize(1);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 5;
            d.Units[0].Bytes = 1;
            std::vector<std::string> labels;
            // Two owners of the first class without PCH: 100 s in the traces, 80 s on this host.
            for (int j = 0; j < 2; ++j) {
                TAction a;
                a.Time = 100;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({int(d.Actions.size()) - 1, 0, "a" + std::to_string(j), {}});
                labels.push_back("//a" + std::to_string(j));
            }
            // One owner of the second class without PCH: 100 s in the traces and as many on this host.
            {
                TAction a;
                a.Time = 100;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({int(d.Actions.size()) - 1, 1, "b", {}});
                labels.push_back("//b");
            }
            GroupTargets(d, labels);
            TLayout layout;
            layout.Assignment.assign(3, -1);
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
            }
            obs[0].Time = 80;
            obs[1].Time = 80;
            obs[2].Time = 100;
            auto fit = Calibrate(d, layout, prediction, obs);
            Near(fit.Classes[0].Baseline, 0.8, "two controls at the common line leave it where it is: " + std::to_string(fit.Classes[0].Baseline));
            Near(fit.Classes[1].Baseline, 0.8 * (1 + 1.0 / 3 * 0.25),
                 "one control a quarter above the common line moves the class a third of the way: " + std::to_string(fit.Classes[1].Baseline));
        }

        // Controls come from the whole class, not from its lighter half: a
        // baseline read off the lightest owners alone does not describe the
        // class, and the largest class of a run read as compiling three times
        // slower than its traces. An owner carrying more than a third of the
        // class keeps its PCH: it holds up the base.
        void ControlsSpanTheClass() {
            auto data = [&](double extra) {
                auto d = Data();
                d.Units.resize(2);
                d.Units[0].Include = "<base>";
                d.Units[0].Stable = true;
                d.Units[0].Parse = 1;
                d.Units[1].Include = "<leaf>";
                d.Units[1].Parse = 8;
                for (int u = 0; u < 2; ++u) { d.Units[u].Bytes = 1; }
                std::vector<std::string> owners;
                for (int i = 0; i < 8; ++i) {
                    TAction a;
                    a.Time = 10 + extra;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    d.Files.push_back({i, 0, std::to_string(i), {{0, 1}, {1, i == 0 ? 8 + extra : 8}}});
                    owners.push_back("//x" + std::to_string(i));
                }
                GroupTargets(d, owners);
                return d;
            };
            auto describe = [&](const TLayout& l) {
                std::string roles, leaves;
                for (char x : l.Controls) { roles += std::to_string(int(x)) + ","; }
                for (int x : l.Assignment) { leaves += std::to_string(x) + ","; }
                return "controls=" + roles + " leaves=" + leaves;
            };
            // The first owner parses more than the others and still carries a
            // sixth of the class: it is pool like the rest.
            auto selected = Select(data(2), {});
            Check(selected.Controls == std::vector<char>({1, 0, 0, 0, 2, 0, 0, 0}) &&
                      selected.Assignment == std::vector<int>({-1, 1, 1, 1, 0, 1, 1, 1}),
                  "the controls are spread over the whole class, the heavier owners included: " + describe(selected));
            // Carrying half the class, it keeps its PCH.
            selected = Select(data(60), {});
            Check(selected.Controls == std::vector<char>({0, 1, 0, 0, 2, 0, 0, 0}) &&
                      selected.Assignment[0] == 1 && selected.Assignment[1] == -1 && selected.Assignment[4] == 0,
                  "an owner carrying more than a third of the class keeps its PCH: " + describe(selected));
        }

        // The probes a plan built, weighed and left are the bases and deltas
        // of exactly the classes it drops. Their sizes teach the next
        // selection what those classes cost; nothing timed their builds, and
        // a probe the measurement weighed in its own right is no second
        // observation.
        void EvictedProbeFeedsTheCalibration() {
            auto d = Data();
            d.Classes = 2;
            d.Units.resize(3);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 2e8;
            d.Units[1].Include = "<kept>";
            d.Units[1].Parse = 4;
            d.Units[1].Bytes = 3e8;
            d.Units[2].Include = "<evicted>";
            d.Units[2].Parse = 10;
            d.Units[2].Bytes = 1e9;
            TAction a;
            a.Time = 20;
            a.Memory = 1e6;
            d.Actions.push_back(a);
            d.Files.push_back({0, 0, "owner", {{0, 1}, {1, 4}}});
            TestTargets(d);
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0, 1};
            layout.Pchs = {base};
            layout.Assignment = {0};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            obs[d.Actions.size()].Bytes = 3e9; // the base: six PCH bytes per source byte
            // The delta the plan built next to the base and then left: the
            // base's units and one root of its own, twice their volume, at
            // sixteen PCH bytes per source byte.
            TMeasuredPch probe;
            probe.Pch.Class = 0;
            probe.Pch.Parent = 0;
            probe.Pch.Roots = {2};
            probe.Pch.Units = {0, 1, 2};
            probe.Parent = base;
            probe.Bytes = 1.6e10;
            const auto without = Calibrate(d, layout, prediction, obs);
            const auto fit = Calibrate(d, layout, prediction, obs, {probe, probe});
            Check(fit.Measured.size() == 1 && fit.Probes.size() == 1, "one observation per content: the measured build and the probe");
            Near(fit.UnitSize[0][2], 16, "the root only the evicted delta had is calibrated by its probe");
            Near(fit.GlobalUnitSize[2], 16, "and so in the pool of every class");
            Check(fit.Classes[0].Size > without.Classes[0].Size && fit.GlobalSize > without.GlobalSize,
                  "the probe's size counts for its class and for the common scale: " + std::to_string(fit.Classes[0].Size) + " " +
                  std::to_string(without.Classes[0].Size));
            Near(fit.Classes[0].DeltaBuild, TCoefficients().DeltaBuild, "nothing timed the probe's build: the delta scale stays the prior");
            Near(fit.GlobalDeltaBuild, 0, "and no class measured a delta");
            Near(fit.Classes[0].Memory, without.Classes[0].Memory, "the memory scale is the measured build's alone");
            Check(fit.UnitBuild[0].size() <= 2 || fit.UnitBuild[0][2] == 0, "the probe's root gets no build weight of its class");
            Check(fit.GlobalUnitBuild.size() <= 2 || fit.GlobalUnitBuild[2] == 0, "and none in the pool of every class");
            TLayout next = layout;
            TPch delta;
            delta.Class = 0;
            delta.Parent = 0;
            delta.Roots = {2};
            delta.Units = {0, 1, 2};
            next.Pchs = {base, delta};
            next.Assignment = {1};
            Check(std::abs(Predict(d, next, fit).PchBytes[1] / 1.6e10 - 1) < 1e-9,
                  "a new delta of the class is estimated from what the probe weighed of its root: " + std::to_string(Predict(d, next, fit).PchBytes[1]));
            TMeasuredPch same;
            same.Pch = base;
            same.Bytes = 3e9;
            Check(Calibrate(d, layout, prediction, obs, {same, probe}).Probes.size() == 1,
                  "a probe the measurement weighed in its own right is no second observation");
            // A sibling leaf read the same root at ten bytes a byte, in an
            // artifact of 10 GB next to the probe's 16 GB: the class takes the
            // reading its bytes weigh, (16 x 1.6 + 10 x 1.0) / 2.6, not the
            // plain 13 of two observations.
            d.Units.resize(4);
            d.Units[3].Include = "<tiny>";
            d.Units[3].Parse = 1;
            d.Units[3].Bytes = 1;
            TMeasuredPch sibling = probe;
            sibling.Pch.Roots = {2, 3};
            sibling.Pch.Units = {0, 1, 2, 3};
            sibling.Bytes = 1e10;
            const auto pair = Calibrate(d, layout, prediction, obs, {probe, sibling});
            Check(std::abs(pair.UnitSize[0][2] - (16 * 1.6 + 10 * 1.0) / 2.6) < 0.01,
                  "within a class an artifact weighs as its bytes too: " + std::to_string(pair.UnitSize[0][2]));
        }

        // A class's scale is its artifacts' PCH bytes per source byte, as far
        // as the bytes behind it go: it is moved towards the common scale of
        // every class, and that one towards the prior, by the bytes behind
        // each. A hundred megabytes reading 3 weigh as much as the prior: the
        // class's scale is the geometric mean of its own and the common one.
        // The units the artifact measured keep what it measured of them, and
        // the headers the class never measured take the shrunk scale.
        void SmallSampleScaleShrinks() {
            auto d = Data();
            d.Classes = 3;
            d.Units.resize(3);
            d.Units[0].Include = "<small>";
            d.Units[0].Stable = true;
            d.Units[1].Include = "<large>";
            d.Units[1].Stable = true;
            d.Units[2].Include = "<unmeasured>";
            d.Units[0].Bytes = 1e8 / 3; // 100 MB of PCH at three bytes per source byte
            d.Units[1].Bytes = 112.5e6; // 900 MB at eight
            d.Units[2].Bytes = 1;
            for (int u = 0; u < 3; ++u) { d.Units[u].Parse = 1; }
            for (int f = 0; f < 2; ++f) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({f, f, std::to_string(f), {{f, 1}}});
            }
            TestTargets(d);
            TLayout layout;
            layout.Bases = 2;
            TPch small;
            small.Class = 0;
            small.Roots = small.Units = {0};
            TPch large;
            large.Class = 1;
            large.Roots = large.Units = {1};
            layout.Pchs = {small, large};
            layout.Assignment = {0, 1};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            obs[d.Actions.size()].Bytes = 1e8;
            obs[d.Actions.size() + 1].Bytes = 9e8;
            const auto fit = Calibrate(d, layout, prediction, obs);
            const auto ratio = [](double value, double expected) { return value / expected - 1; };
            // Forty-eight sevenths over all, ten elevenths of the way from the prior of 6.2.
            Check(std::abs(ratio(fit.GlobalSize, std::pow(48.0 / 7, 10.0 / 11) * std::pow(TCoefficients().Size, 1.0 / 11))) < 1e-12,
                  "the common scale is every artifact's ratio, moved towards the prior by the bytes behind it: " + std::to_string(fit.GlobalSize));
            Near(fit.Classes[2].Size, fit.GlobalSize, "a class that weighed nothing takes the common scale");
            Check(std::abs(ratio(fit.Classes[0].Size, std::sqrt(3 * fit.GlobalSize))) < 1e-12,
                  "a hundred megabytes weigh as much as the prior: the scale is the geometric mean of its own and the common one: " + std::to_string(fit.Classes[0].Size));
            Check(std::abs(ratio(fit.Classes[1].Size, fit.GlobalSize * std::pow(8 / fit.GlobalSize, 0.9))) < 1e-12,
                  "nine hundred megabytes: nine parts in ten the class's own: " + std::to_string(fit.Classes[1].Size));
            Near(fit.UnitSize[0][0], 3, "the units the small artifact measured keep what it measured");
            Near(fit.UnitSize[1][1], 8, "and so the large one's");
            TLayout next = layout;
            TPch delta;
            delta.Class = 0;
            delta.Parent = 0;
            delta.Roots = {2};
            delta.Units = {0, 2};
            next.Pchs = {small, large, delta};
            next.Assignment = {2, 1};
            Near(Predict(d, next, fit).PchBytes[2], fit.Classes[0].Size, "a header the small class never measured takes the shrunk scale, not the base's");
            auto nothing = obs;
            for (auto& o : nothing) { o = {}; }
            const auto prior = Calibrate(d, layout, prediction, nothing);
            Near(prior.GlobalSize, TCoefficients().Size, "with nothing weighed the common scale is the prior");
            Near(prior.Classes[0].Size, TCoefficients().Size, "and so is every class's");
        }

        // An artifact weighs as its bytes when the headers are decomposed
        // together: the ratio of a 100 MB probe is the noisier next to one of
        // 400 MB that weighed the same header.
        void ProbesWeighAsTheirBytes() {
            auto d = Data();
            d.Classes = 2;
            d.Units.resize(1);
            d.Units[0].Include = "<shared>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 1e8;
            TAction a;
            a.Time = 20;
            a.Memory = 1e6;
            d.Actions.push_back(a);
            d.Files.push_back({0, 1, "owner", {{0, 1}}});
            TestTargets(d);
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 1;
            base.Roots = base.Units = {0};
            layout.Pchs = {base};
            layout.Assignment = {0};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            obs[d.Actions.size()].Bytes = 4e8; // four PCH bytes per source byte, 400 MB of them
            TMeasuredPch probe;
            probe.Pch.Class = 0;
            probe.Pch.Roots = probe.Pch.Units = {0};
            probe.Bytes = 1e8; // the same header under another flag set: one, 100 MB
            const auto fit = Calibrate(d, layout, prediction, obs, {probe});
            Near(fit.UnitSize[0][0], 1, "the class of the probe measures the header at its own ratio");
            Near(fit.UnitSize[1][0], 4, "and the class of the measured build at its");
            // (4 x 4 + 1 x 1) / (4 + 1), not the plain 2.5 of two observations.
            Check(std::abs(fit.GlobalUnitSize[0] - 3.4) < 1e-3,
                  "the pooled weight of the header follows the bytes behind each: " + std::to_string(fit.GlobalUnitSize[0]));
        }

        // Nothing timed a probe's build. In a class that measured a delta of
        // its own, the root only the evicted delta had gets a size and no
        // build weight: the zero of an untimed build read as a time would
        // price every new delta with that root a hundred times too cheap.
        void ProbeBuildIsNotTimed() {
            auto d = Data();
            d.Classes = 1;
            d.Units.resize(4);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[0].Bytes = 2e8;
            d.Units[1].Include = "<kept>";
            d.Units[1].Parse = 4;
            d.Units[1].Bytes = 3e8;
            for (int u = 2; u < 4; ++u) {
                d.Units[u].Include = "<leaf" + std::to_string(u) + ">";
                d.Units[u].Parse = 10;
                d.Units[u].Bytes = 1e9;
            }
            TAction a;
            a.Time = 20;
            a.Memory = 1e6;
            d.Actions.push_back(a);
            d.Files.push_back({0, 0, "owner", {{0, 1}, {1, 4}, {2, 10}}});
            TestTargets(d);
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0, 1};
            TPch kept;
            kept.Class = 0;
            kept.Parent = 0;
            kept.Roots = {2};
            kept.Units = {0, 1, 2};
            layout.Pchs = {base, kept};
            layout.Assignment = {1};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            obs[d.Actions.size()].Bytes = 3e9;
            obs[d.Actions.size() + 1].Bytes = 1.6e10;
            obs[d.Actions.size() + 1].Time = 5; // the measured delta: five seconds for ten of parse
            TMeasuredPch probe;
            probe.Pch = kept;
            probe.Pch.Roots = {3};
            probe.Pch.Units = {0, 1, 3};
            probe.Parent = base;
            probe.Bytes = 1.6e10;
            const auto fit = Calibrate(d, layout, prediction, obs, {probe});
            Check(fit.Measured.size() == 2 && fit.Probes.size() == 1, "the base and the kept delta were timed, the evicted one only weighed");
            Check(fit.Classes[0].DeltaBuild > 0.4 && fit.Classes[0].DeltaBuild < 0.6,
                  "the class measures the build of its kept delta: " + std::to_string(fit.Classes[0].DeltaBuild));
            Check(!fit.UnitBuild[0].empty() && fit.UnitBuild[0][2] > 0, "the kept delta's root has a build weight");
            Check(fit.UnitBuild[0][3] == 0, "the evicted delta's root has none: " + std::to_string(fit.UnitBuild[0][3]));
            Check(fit.GlobalUnitBuild.size() <= 3 || fit.GlobalUnitBuild[3] == 0,
                  "nor in the pool of every class: " + std::to_string(fit.GlobalUnitBuild.size() > 3 ? fit.GlobalUnitBuild[3] : 0.0));
            Check(fit.UnitSize[0][3] > 0 && (fit.GlobalUnitSize.size() <= 3 || fit.GlobalUnitSize[3] > 0), "and a size from both");
        }

        // The seconds a header takes to serialize are the header's. Three
        // classes, a base each, every base two headers of one parse second:
        // the shared one, and one of the two others. The bases took 3, 5 and
        // 4 seconds, so against the common scale of two seconds a header the
        // slow class measured alone comes out half again as heavy, and a class
        // that measured nothing builds it at that pace. Against the scale of
        // its own class every header divided to one.
        void GlobalBuildWeightsFollowTheHeaders() {
            auto d = Data();
            d.Classes = 4;
            d.Units.resize(3);
            const char* names[] = {"<shared>", "<first>", "<second>"};
            for (int u = 0; u < 3; ++u) {
                d.Units[u].Include = names[u];
                d.Units[u].Stable = true;
                d.Units[u].Parse = 1;
                d.Units[u].Bytes = 1e6;
            }
            const int bases[3][2] = {{0, 1}, {0, 2}, {1, 2}};
            const double seconds[3] = {3, 5, 4};
            std::vector<std::string> labels;
            for (int c = 0; c < 3; ++c) {
                TAction a;
                a.Time = 20;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({c, c, std::to_string(c), {{bases[c][0], 1}, {bases[c][1], 1}}});
                labels.push_back("//c" + std::to_string(c));
            }
            GroupTargets(d, labels);
            TLayout layout;
            layout.Bases = 4;
            layout.Pchs.resize(4);
            for (int c = 0; c < 4; ++c) {
                layout.Pchs[c].Class = c;
                layout.Pchs[c].Units = c < 3 ? std::vector<int>({bases[c][0], bases[c][1]}) : std::vector<int>({2});
                layout.Pchs[c].Roots = {layout.Pchs[c].Units.back()};
            }
            layout.Assignment = {0, 1, 2};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
                if (i >= d.Actions.size()) { obs[i].Bytes = prediction.PchBytes[i - d.Actions.size()]; }
            }
            for (int c = 0; c < 3; ++c) { obs[d.Actions.size() + c].Time = seconds[c]; }
            obs[d.Actions.size() + 3].Time = 0; // the fourth base was never built
            obs[d.Actions.size() + 3].Bytes = 0;
            const auto fit = Calibrate(d, layout, prediction, obs);
            Near(fit.Classes[0].BaseBuild, 1.5, "a class measures the build of its own base");
            Near(fit.Classes[1].BaseBuild, 2.5, "and so the second");
            Near(fit.GlobalBaseBuild, 2, "the common scale of a role is the classes' median");
            Check(std::abs(fit.GlobalUnitBuild[0] - 1) < 1e-6 && std::abs(fit.GlobalUnitBuild[1] - 0.5) < 1e-6 &&
                      std::abs(fit.GlobalUnitBuild[2] - 1.5) < 1e-6,
                  "against the common scale of the role a header shows how heavy it is: " + std::to_string(fit.GlobalUnitBuild[0]) + " " +
                  std::to_string(fit.GlobalUnitBuild[1]) + " " + std::to_string(fit.GlobalUnitBuild[2]));
            Near(Predict(d, layout, fit).Actions[d.Actions.size() + 3].Time, 3,
                 "a class that measured nothing builds a measured header at the common scale by the header's share");
        }

        // Controls for the common line: twenty of a hundred trace seconds at
        // ninety-four and ten of one second at 4.9, so that the line is 0.9
        // with four seconds outside the traces whatever one other class adds.
        void LineControls(TData& d, int cls, std::vector<std::string>& labels) {
            for (int i = 0; i < 30; ++i) {
                TAction a;
                a.Time = i < 20 ? 100 : 1;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({int(d.Actions.size()) - 1, cls, "line" + std::to_string(i), {}});
                labels.push_back("//line" + std::to_string(i));
            }
        }

        // One control is no line. Its four files span a second to a hundred,
        // and a line through them fits exactly; fitted, it would replace the
        // start from the common line with the noise of one target. The line
        // is fitted for a class with controls enough to speak for itself alone.
        void OneControlIsNoLine() {
            auto d = Data();
            d.Classes = 2;
            std::vector<std::string> labels;
            const double trace[4] = {1, 10, 50, 100};
            for (int i = 0; i < 4; ++i) {
                TAction a;
                a.Time = trace[i];
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({int(d.Actions.size()) - 1, 0, "one" + std::to_string(i), {}});
                labels.push_back("//one");
            }
            LineControls(d, 1, labels);
            GroupTargets(d, labels);
            TLayout layout;
            layout.Assignment.assign(d.Targets.size(), -1);
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
            }
            for (int i = 0; i < 4; ++i) { obs[i].Time = 0.5 * trace[i] + 0.4; }
            for (int i = 0; i < 30; ++i) { obs[4 + i].Time = i < 20 ? 94 : 4.9; }
            const auto fit = Calibrate(d, layout, prediction, obs);
            const auto& line = fit.Classes[1];
            Check(std::abs(line.Baseline - 0.9) < 1e-9 && std::abs(line.Residual - 4) < 1e-9,
                  "the common line is the controls': " + std::to_string(line.Baseline) + " " + std::to_string(line.Residual));
            // 82.1 observed where the line expects 160.9: a third of the way there, both coefficients.
            const double moved = 1 + (82.1 / 160.9 - 1) / 3;
            const auto& one = fit.Classes[0];
            Check(std::abs(one.Baseline - 0.9 * moved) < 1e-9 && std::abs(one.Residual - 4 * moved) < 1e-9,
                  "one control moves the common line and is not fitted a line of its own: " + std::to_string(one.Baseline) + " " + std::to_string(one.Residual));
        }

        // A sampled control is a whole target all the same: every file pays
        // the seconds outside the traces, the measured and the imputed alike.
        void SampledControlCountsEveryFile() {
            auto d = Data();
            d.Classes = 2;
            std::vector<std::string> labels;
            const double trace[2] = {10, 100};
            for (int i = 0; i < 2; ++i) {
                TAction a;
                a.Time = trace[i];
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({int(d.Actions.size()) - 1, 0, "pair" + std::to_string(i), {}});
                labels.push_back("//pair");
            }
            LineControls(d, 1, labels);
            GroupTargets(d, labels);
            TLayout layout;
            layout.Assignment.assign(d.Targets.size(), -1);
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
            }
            obs[0].Time = 13; // the short file, on the line
            obs[1].Time = 0;  // the long one is not measured
            for (int i = 0; i < 30; ++i) { obs[2 + i].Time = i < 20 ? 94 : 4.9; }
            const auto fit = Calibrate(d, layout, prediction, obs);
            // The short file's ratio imputes the long one: 143 observed where
            // the line expects 107, the seconds outside the traces counted twice.
            const double moved = 1 + (143.0 / 107.0 - 1) / 3;
            Check(std::abs(fit.Classes[0].Baseline - 0.9 * moved) < 1e-9 && std::abs(fit.Classes[0].Residual - 4 * moved) < 1e-9,
                  "every file of a sampled control pays the seconds outside the traces: " + std::to_string(fit.Classes[0].Baseline) + " " +
                  std::to_string(fit.Classes[0].Residual));
        }

        // A target measured only at its backend floor leaves the fit nothing:
        // whether it is there or not, the coefficients are the same. An empty
        // equation counted as a target in the shrinkage and as a zero among
        // the residuals the robust fit is scaled by.
        void FloorTargetLeavesTheFitAlone() {
            auto data = [&](bool withFloor) {
                auto d = Data();
                d.Units.resize(2);
                d.Units[0].Include = "<base>";
                d.Units[0].Stable = true;
                d.Units[0].Parse = 1;
                d.Units[1].Include = "<leaf>";
                for (int u = 0; u < 2; ++u) { d.Units[u].Bytes = 1; }
                const double parse[4] = {5, 10, 15, 10}, noise[4] = {0.7, -0.5, 0.3, 0};
                const int owners = withFloor ? 4 : 3;
                std::vector<std::string> labels;
                for (int t = 0; t < owners; ++t) {
                    for (int j = 0; j < 3; ++j) {
                        TAction a;
                        a.Time = 20;
                        a.Memory = 1e6;
                        d.Actions.push_back(a);
                        d.Files.push_back({int(d.Actions.size()) - 1, 0, std::to_string(t) + "_" + std::to_string(j), {{0, 1}, {1, parse[t]}}});
                        d.Files.back().Frontend = 19; // the backend keeps a second of the trace
                        labels.push_back("//owner" + std::to_string(t));
                    }
                }
                for (int t = 0; t < 3; ++t) {
                    TAction a;
                    a.Time = 20;
                    a.Memory = 1e6;
                    d.Actions.push_back(a);
                    d.Files.push_back({int(d.Actions.size()) - 1, 0, "control" + std::to_string(t), {{0, 1}}});
                    labels.push_back("//control" + std::to_string(t));
                }
                GroupTargets(d, labels);
                TLayout layout;
                layout.Bases = 1;
                TPch base;
                base.Class = 0;
                base.Roots = base.Units = {0};
                TPch delta;
                delta.Class = 0;
                delta.Parent = 0;
                delta.Roots = {1};
                delta.Units = {0, 1};
                layout.Pchs = {base, delta};
                layout.Assignment.assign(3, -1); // the controls, first in label order
                layout.Assignment.resize(d.Targets.size(), 1);
                auto prediction = Predict(d, layout, {});
                std::vector<TObservation> obs(prediction.Actions.size());
                for (size_t i = 0; i < obs.size(); ++i) {
                    obs[i].Time = prediction.Actions[i].Time;
                    obs[i].Memory = prediction.Actions[i].Memory;
                }
                for (int t = 0; t < owners; ++t) {
                    // A tenth more saved than the model expects, and a noise of its own; the fourth at the floor.
                    const double time = withFloor && t == 3 ? 0.5 : 20 - 1.1 * parse[t] - 0.5 + noise[t];
                    for (int j = 0; j < 3; ++j) { obs[3 * t + j].Time = time; }
                }
                for (int t = 0; t < 3; ++t) { obs[3 * owners + t].Time = 20; }
                TCalibration fit = Calibrate(d, layout, prediction, obs);
                return fit;
            };
            const auto with = data(true), without = data(false);
            Check(with.Classes[0].Identified && without.Classes[0].Identified, "three controls and three targets identify the class");
            Near(with.Classes[0].Saved, without.Classes[0].Saved, "a target on the floor leaves the fit as it found it");
            Near(with.Classes[0].Load, without.Classes[0].Load, "and its load");
        }

        // A target of five files of which one was measured: the row is that
        // file's, and its noise is that file's own. Taken from the whole
        // target, the noise was five times the saving's scale, and a real
        // saving in the measured file moved the class by a quarter of what it
        // shows.
        void SampledRowWeighsAsItsFiles() {
            auto d = Data();
            d.Units.resize(2);
            d.Units[0].Include = "<base>";
            d.Units[0].Stable = true;
            d.Units[0].Parse = 1;
            d.Units[1].Include = "<leaf>";
            d.Units[1].Parse = 60;
            for (int u = 0; u < 2; ++u) { d.Units[u].Bytes = 1; }
            std::vector<std::string> labels;
            for (int j = 0; j < 5; ++j) {
                TAction a;
                a.Time = 240;
                a.Memory = 1e6;
                d.Actions.push_back(a);
                d.Files.push_back({j, 0, std::to_string(j), {{0, 1}, {1, 60}}});
                labels.push_back("//owner");
            }
            GroupTargets(d, labels);
            TLayout layout;
            layout.Bases = 1;
            TPch base;
            base.Class = 0;
            base.Roots = base.Units = {0};
            TPch delta;
            delta.Class = 0;
            delta.Parent = 0;
            delta.Roots = {1};
            delta.Units = {0, 1};
            layout.Pchs = {base, delta};
            layout.Assignment = {1};
            auto prediction = Predict(d, layout, {});
            std::vector<TObservation> obs(prediction.Actions.size());
            for (size_t i = 0; i < obs.size(); ++i) {
                obs[i].Time = prediction.Actions[i].Time;
                obs[i].Memory = prediction.Actions[i].Memory;
            }
            obs[0].Time = 120; // twice the saving the model expects
            for (int j = 1; j < 5; ++j) { obs[j].Time = 0; }
            const auto fit = Calibrate(d, layout, prediction, obs);
            Check(!fit.Classes[0].Identified && fit.Classes[0].Saved > 1.2 && fit.Classes[0].Saved < 1.32,
                  "the noise of a sampled row is the noise of its own files: " + std::to_string(fit.Classes[0].Saved));
        }

        // A parent that does not compile an owner is no fallback: priced
        // against NONE alone, the delta stays where it pays there.
        void IncompatibleParentIsNoFallback() {
            auto dw = ThinAndWide();
            const auto layout = dw.second;
            auto onBase = layout;
            FinalPayback(dw.first, onBase, {});
            Check(onBase.Assignment == std::vector<int>({0, 2}), "with its parent for a fallback the thin owner steps down");
            auto kept = layout;
            const auto report = FinalPayback(dw.first, kept, {}, [](int t, int parent) { return t >= 8 || parent != 0; });
            Check(kept.Assignment == std::vector<int>({1, 2}) && report.find("keep") != std::string::npos,
                  "a parent that does not compile the owner leaves the delta priced against NONE, where it pays");
            auto nothing = layout;
            auto unprofitable = dw.first;
            unprofitable.Units[1].Parse = 30;
            FinalPayback(unprofitable, nothing, {}, [](int, int parent) { return parent != 0; });
            Check(nothing.Assignment == std::vector<int>({-1, 2}), "and a delta that pays neither way sends the owner to NONE");
        }

        // A layout scored after the checks steps its owners down only: to a
        // sibling delta nobody checked them with, never.
        void ScoredLayoutStepsDownOnly() {
            auto dw = ThinAndWide();
            double objective = -1;
            const auto scored = ScoredLayout(dw.first, dw.second, {}, 1, &objective);
            Check(scored.Assignment == std::vector<int>({0, 1}),
                  "a scored layout moves an owner to its parent, not over to a sibling: " + std::to_string(scored.Assignment[0]));
        }
    } // namespace

    int SelfTest() {
        LinkRequirements();
        LinkInputGraph();
        Scheduling();
        Invalidation();
        LibraryDependentLeaves();
        CycleRepair();
        GeneratorCycleKeepsBase();
        FrozenBase();
        FrozenBasePayback();
        AssignedPayback();
        LoneOwnerClass();
        PairedOwnersClass();
        HeavyOwnerKeepsBase();
        CommonLineOfTheControls();
        TinyGainKeepsTheScale();
        PooledUnitSize();
        CorrectionBuildsOpenBases();
        FinalPaybackStepsDown();
        ScoredLayoutOfAPlan();
        GridCountsFrozenBases();
        UnmeasuredClassBuildsLikeTheOthers();
        FewControlsMoveTheCommonLine();
        ControlsSpanTheClass();
        EvictedProbeFeedsTheCalibration();
        SmallSampleScaleShrinks();
        ProbesWeighAsTheirBytes();
        ProbeBuildIsNotTimed();
        GlobalBuildWeightsFollowTheHeaders();
        OneControlIsNoLine();
        SampledControlCountsEveryFile();
        FloorTargetLeavesTheFitAlone();
        SampledRowWeighsAsItsFiles();
        IncompatibleParentIsNoFallback();
        ScoredLayoutStepsDownOnly();
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
        HarmfulLeafGain();
        EmptyFrozenFamily();
        CoChangedRoots();
        PhysicalDeltaCosts();
        HeldOutParentCalibration();
        DiskPricedTrimming();
        DiskPricedByBytes();
        DensitiesSpanClasses();
        ClusteredAcrossAlphabet();
        SiblingFallbackEviction();
        CrossClassSiblings();
        DuplicateDeltaMerge();
        UmbrellaRootSibling();
        ZeroOwnUmbrellaRoot();
        ChangedHeaderBaseMaterial();
        DemotedControlRole();
        ParasiteLeavesClusterAlone();
        OwnClusterDeltaChosen();
        LeanDeltaBeyondSiblings();
        PickScratchIsolation();
        AtomicTargets();
        IoSelfTest();
        Report("two-step self-tests: OK");
        return 0;
    }
} // namespace NPchUpdate::NTwoStep
