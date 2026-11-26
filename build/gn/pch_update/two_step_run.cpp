#include "two_step.h"
#include "checks.h"
#include "common.h"
#include "elf.h"
#include "gni.h"
#include "ninja.h"
#include "steps.h"
#include "trace.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>

namespace NPchUpdate::NTwoStep {
    namespace {
        constexpr const char* VERSION = "pch-two-step-11";
        constexpr const char* OPEN_CLASSES_VERSION = "pch-two-step-10";
        constexpr const char* CONTROLS_VERSION = "pch-two-step-9";
        constexpr const char* ANCHORED_VERSION = "pch-two-step-8";
        constexpr const char* MERGED_OWNER_VERSION = "pch-two-step-7";
        constexpr const char* OWNER_VERSION = "pch-two-step-6";
        constexpr const char* TARGET_VERSION = "pch-two-step-5";
        constexpr const char* PREVIOUS_VERSION = "pch-two-step-4";
        constexpr const char* LEGACY_VERSION = "pch-two-step-3";
        // Bump when extraction or the meaning of cached model features changes.
        constexpr const char* ANALYSIS_VERSION = "pch-analysis-11";

        const char* HELP =
            "pch_update --baseline DIR [--predict | --correct | --apply] [options]\n"
            "  Default: predict, correct, apply; one correction, including both measured builds.\n"
            "  --predict            build baseline, analyze, select and check PCH groups\n"
            "  --correct            measure initial, calibrate/correct, measure corrected\n"
            "  --apply              export, replace old groups, validate/build normal GN global graph\n"
            "  Run from the source worktree. One unfinished run lives in BASELINE/.pch_update/.\n"
            "  Retry to resume; successful apply removes all scratch state and parameters.\n"
            "  --workers N          -j for the whole run: baseline, MEASURE, compatibility checks and\n"
            "                       analysis. Never reduced when given. Default: the CPU count at run\n"
            "                       start, at most what --ram-gb admits at --action-memory-gb.\n"
            "                       The PCHs themselves build at most N/4 at once.\n"
            "  --ram-gb N           RAM behind the default of --workers (default: 75% available RAM at run start)\n"
            "  --disk SIZE          resident PCH budget (required on first predict; e.g. 6G)\n"
            "  --action-memory-gb N unmeasured action RSS prior (default: 1); with --ram-gb shapes that default\n"
            "  --mode MODE         global or incremental (default: incremental)\n"
            "  --q Q               cold fraction; overrides mode default (global: 1, incremental: 0.1)\n"
            "  --target T          repeat for each target (default: all, tests)\n"
            "  --main-ref REF --gn PATH --ninja PATH\n"
            "  --analysis-jobs N    parallel analysis jobs (default: --workers)\n"
            "  --verbose           show selection details and full child output (presentation only)\n"
            "  --correct --sample PERCENT   share of C++ objects measured (10..100,\n"
            "                       default 100); PCHs and required generator tools always built\n"
            "pch_update self-test\n"
            "Measurement compilers run uncached. Retry --correct to resume a failed or interrupted measurement.\n"
            "Reports survive cleanup under BASELINE/pch-report/; latest.txt points to this run.\n"
            "Apply keeps a recoverable backup and restores it when GN validation fails.\n";

        int64_t Now() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        size_t SampleBudget(const TData& d, int percent) {
            return (d.Files.size() * percent + 99) / 100;
        }

        // The sampling frame is the frozen baseline, including targets without PCH.
        // Calibration controls (TLayout::Controls) take their first member
        // before any other target: they inform their whole compiler class,
        // not only their own anchor. A control without PCH, which the class
        // baseline is read from, also takes an extra member ahead of the
        // queue; a control kept on the base is an ordinary row of the consumer
        // fit and takes its turn in it. Extra slots follow baseline compile
        // work rather than file count (Neyman allocation): fifty generated
        // stubs of 0.2 s are worth less to the calibration than fifty core
        // files of 25 s.
        std::vector<char> SampleFiles(const TData& d, const std::string& nonce, int percent,
                                      const std::vector<char>& controls = {}) {
            const size_t budget = SampleBudget(d, percent);
            if (budget == d.Files.size()) { return std::vector<char>(budget, true); }
            const auto action = [&](int f) -> const TAction& { return d.Actions[d.Files[f].Action]; };
            const auto& targets = d.Targets;
            const bool hasControls = controls.size() == targets.size();
            auto control = [&](int t) { return hasControls && controls[t] != 0; };
            std::vector<int> order(targets.size());
            std::iota(order.begin(), order.end(), 0);
            std::vector<double> work(targets.size());
            for (int t : order) {
                for (int f : targets[t].Files) { work[t] += action(f).Time; }
            }
            std::sort(order.begin(), order.end(), [&](int a, int b) {
                if (control(a) != control(b)) { return control(a); }
                return work[a] != work[b] ? work[a] > work[b] : targets[a].Label < targets[b].Label;
            });
            std::vector<size_t> quota(targets.size());
            const size_t covered = std::min(budget, targets.size());
            for (size_t i = 0; i < covered; ++i) { quota[order[i]] = 1; }
            size_t assigned = covered;
            for (int t : order) {
                if (assigned >= budget) { break; }
                if (hasControls && controls[t] == 1 && quota[t] && quota[t] < targets[t].Files.size()) {
                    ++quota[t];
                    ++assigned;
                }
            }

            // Assign each extra slot by largest baseline work / (slots + 1).
            // Only the popped target's quota changes before reinsertion.
            auto lowerPriority = [&](int a, int b) {
                auto left = work[a] * (quota[b] + 1);
                auto right = work[b] * (quota[a] + 1);
                return left != right ? left < right : targets[a].Label > targets[b].Label;
            };
            std::priority_queue<int, std::vector<int>, decltype(lowerPriority)> queue(lowerPriority);
            for (int t : order) {
                if (quota[t] && quota[t] < targets[t].Files.size()) { queue.push(t); }
            }
            for (; assigned < budget; ++assigned) {
                int t = queue.top();
                queue.pop();
                if (++quota[t] < targets[t].Files.size()) { queue.push(t); }
            }

            std::vector<char> selected(d.Files.size());
            for (int t : order) {
                const size_t k = quota[t];
                if (!k) { continue; }
                auto files = targets[t].Files;
                std::sort(files.begin(), files.end(), [&](int a, int b) {
                    const auto& x = action(a);
                    const auto& y = action(b);
                    return x.Time != y.Time ? x.Time < y.Time : x.Output < y.Output;
                });
                for (size_t j = 0; j < k; ++j) {
                    const size_t begin = j * files.size() / k, end = (j + 1) * files.size() / k;
                    int winner = files[begin];
                    double best = std::numeric_limits<double>::infinity();
                    for (size_t i = begin; i < end && end - begin > 1; ++i) {
                        const auto& a = action(files[i]);
                        // Exponential race: chance of winning is proportional to
                        // baseline work. Open interval (0,1), stable per output.
                        auto bits = std::stoull(HashHex(nonce + "\n" + a.Output).substr(0, 16), nullptr, 16);
                        const double u = ((bits >> 12) + 0.5) * 0x1p-52;
                        const double score = -std::log(u) / a.Time;
                        if (score < best) { best = score; winner = files[i]; }
                    }
                    selected[winner] = true;
                }
            }
            return selected;
        }

        std::vector<char> FrozenSampleFiles(const TData& d, int percent, const std::vector<int>& measured) {
            // CorrectPlan keeps the same baseline actions. The initial ledger is
            // sorted by action ID and includes initial PCH builds (not file IDs).
            std::vector<char> selected;
            for (const auto& file : d.Files) {
                selected.push_back(std::binary_search(measured.begin(), measured.end(), file.Action));
            }
            if (size_t(std::count(selected.begin(), selected.end(), true)) != SampleBudget(d, percent)) {
                Fail("initial measurement does not contain the frozen C++ object sample");
            }
            return selected;
        }

        std::vector<char> MeasuredActions(size_t count, const std::vector<int>& ids) {
            std::vector<char> selected(count);
            for (int id : ids) {
                if (id < 0 || size_t(id) >= count || selected[id]) {
                    Fail("invalid measured action in plan");
                }
                selected[id] = true;
            }
            return selected;
        }

        template <class T>
        void Unique(std::vector<T>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        struct TSnapshot {
            std::string Root, Baseline, Directory, Args, Gn, Ninja, Nonce, Tree, Tool;
            std::string Mode = "incremental";
            std::vector<std::string> Targets;
            std::vector<std::string> GnSwitches;
            std::vector<std::pair<std::string, std::string>> Inputs;
            std::vector<std::vector<std::string>> Flags;
            // Keys are actual object outputs, so inherited helper targets and
            // partially linked owners cannot silently lose a consumer's links.
            TLinkInputs LinkInputs;
            TData Data;
            TLayout Layout;
            TPrediction Prediction;
            std::vector<std::string> Commands; // digests of small action configs
            bool Corrected = false;
            // The correction kept the initial plan: no candidate beat it after the checks (corrected plans only).
            bool RetainedInitial = false;
            // The PCHs a plan built, weighed and left before the measurement: their sizes teach the next selection.
            std::vector<TMeasuredPch> ProbeSizes;
            // Percentage of C++ objects measured; PCH actions are
            // always measured. 100 measures every action, links included.
            int Sample = 100;
            // Calibration sample, not its prerequisite closure. Extra tool
            // compilations must not bias the randomly selected observations.
            // Empty means no actions were selected.
            std::vector<int> Measured;
        };

        double ModeQ(const std::string& mode) {
            if (mode == "global") { return 1; }
            if (mode == "incremental") { return 0.1; }
            Fail("--mode must be global or incremental: " + mode);
        }

        // Observers only: diagnostic failures cannot invalidate a plan or undo an installation.
        template <class F> void Diagnostic(F&& fn) {
            try { fn(); }
            catch (const std::exception& e) { Log("warning: diagnostics unavailable: " + std::string(e.what())); }
        }

        std::string PhaseName(const TSnapshot& s) { return s.Corrected ? "corrected" : "initial"; }

        std::string ReportRoot(const TSnapshot& s) {
            return OutputDirectory().empty() ? JoinPath(s.Baseline, "pch-report") : OutputDirectory();
        }

        std::string JsonText(const std::string& text) {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            writer.String(text.data(), text.size());
            return buffer.GetString();
        }

        std::map<std::string, std::string> TargetReasons(const std::string& path) {
            std::map<std::string, std::string> result;
            std::string text;
            if (!TryReadFile(path, &text)) { return result; }
            for (auto line : Split(text, '\n')) {
                if (line.empty() || line.front() == '#') { continue; }
                const auto row = Split(line, '\t');
                if (row.size() >= 2 && row[0] != "target") { result[std::string(row[0])] = std::string(row[1]); }
            }
            return result;
        }

        void PublishReports(const TSnapshot& s) {
            Diagnostic([&] {
                if (s.Baseline.empty()) { return; }
                const auto destination = JoinPath(ReportRoot(s), PhaseName(s));
                for (const char* name : {"summary.tsv", "summary.txt", "summary.json", "assignments.tsv", "target-members.tsv",
                     "selection-targets.tsv", "selection-choices.tsv", "selection-widths.tsv", "selection-candidates.tsv", "selection-payback.tsv", "compatibility.tsv",
                     "compatibility-targets.tsv", "calibration.tsv", "calibration.txt", "calibration-parameters.tsv", "sampling.tsv", "disk-budget.tsv",
                     "final-payback.tsv", "calibration-targets.tsv", "calibration-units.tsv",
                     "initial-errors.tsv", "initial-action-errors.tsv", "measurement.tsv", "measurement.txt", "measurement.json",
                     "target-errors.tsv", "action-errors.tsv", "assignment-changes.tsv"}) {
                    std::string text;
                    if (TryReadFile(JoinPath(s.Directory, name), &text)) { WriteFile(JoinPath(destination, name), text); }
                }
                std::string text;
                if (TryReadFile(JoinPath(s.Directory, "summary.txt"), &text)) {
                    std::string measured;
                    TryReadFile(JoinPath(s.Directory, "measurement.txt"), &measured);
                    WriteFile(JoinPath(ReportRoot(s), "summary.txt"), text + "\n" + measured);
                }
                if (TryReadFile(JoinPath(s.Directory, "summary.json"), &text)) { WriteFile(JoinPath(ReportRoot(s), "summary.json"), text); }
                WriteFile(JoinPath(ReportRoot(s), "README.txt"),
                    "Reports are observations, never selector inputs. initial/ and corrected/ are independent plans.\n"
                    "summary: model predictions; measurement: validated sample; final-result: installation/build outcome.\n"
                    "phases.tsv: phase, status, wall_seconds (one row per attempt; logs append across retries).\n"
                    "assignments.tsv: final compact PCH IDs; selection-targets: before compatibility; compatibility-targets: ID mapping.\n"
                    "selection-choices.tsv: bounded shortlist; IDs local to width before disk pruning, not all possible headers.\n"
                    "selection-candidates.tsv maps those IDs to root headers, parents and compiler classes.\n"
                    "selection-payback.tsv: predicted parse saving, load, extra compilation, build cost and net work for assigned owners.\n"
                    "no-candidate: none in this shortlist; no-estimated-benefit: none beats no-PCH target cost.\n"
                    "calibration-control: initial target deliberately left without PCH to measure the class baseline;\n"
                    "calibration-base-control: initial target deliberately kept on the base to measure a delta's marginal value.\n"
                    "disk-fallback/disk-budget: measured eviction to the best open fallback or NONE.\n"
                    "no-layout-improvement: seed won global objective; retained-initial-plan: correction retained initial seed.\n"
                    "compatibility-fallback/rejected: target moved to an ancestor/no PCH; see compatibility.tsv and retained probe logs.\n"
                    "final-payback-parent/none: owner stepped down by the payback of the final assignments (final-payback.tsv).\n"
                    "selection-widths.tsv: none = the build without PCH; seed = the initial plan on the correction's model; cold and incremental per candidate.\n"
                    "Historical checkpoints may lack reasons; unavailable is not a guessed rejection cause.\n"
                    "GB=1000000000 bytes; disk budget covers resident PCH artifacts, not the complete build directory.\n"
                    "Sample percentage applies to C++ objects; every PCH and prerequisite tool is built.\n"
                    "Work sums are not wall time. WAPE=sum(abs(predicted-actual))/sum(actual).\n"
                    "Holdout MAE is leave-one-group-out cross-validated error of aggregate target compile seconds, not target wall time.\n"
                    "calibration.tsv: every measured group trains the final coefficients; controls give baseline and residual;\n"
                    "anchored_targets are targets whose observed S0 compile pins their predicted level; target_gain is their saved-time multiplier.\n"
                    "Plans have different samples; comparing their measured walls does not establish a speedup.\n"
                    "The final GN build checks integration/linking; tests are built, not executed.\n");
            });
        }

        std::string KeepProbeLog(const TSnapshot& s, const std::string& path) {
            std::string retained = path;
            Diagnostic([&] {
                if (s.Baseline.empty()) { return; }
                const auto destination = JoinPath(ReportRoot(s), PhaseName(s) + "/logs/compatibility/" + HashHex(path) + ".log");
                WriteFile(destination, ReadFile(path));
                retained = destination;
            });
            return retained;
        }

        struct TProfileOptions {
            std::string Mode = "incremental";
            double Q = 0.1;
            bool ModeGiven = false, QGiven = false;

            void SetMode(const std::string& mode) {
                (void)ModeQ(mode);
                Mode = mode;
                ModeGiven = true;
            }
            void SetQ(double q) {
                if (!std::isfinite(q) || q < 0 || q > 1) { Fail("--q must be between 0 and 1"); }
                Q = q;
                QGiven = true;
            }
            void Apply(TSnapshot& snapshot, bool inherit = false) const {
                if (!inherit || ModeGiven) { snapshot.Mode = Mode; }
                const double defaultQ = ModeQ(snapshot.Mode);
                if (QGiven) { snapshot.Data.Q = Q; }
                else if (!inherit || ModeGiven) { snapshot.Data.Q = defaultQ; }
            }
        };

        std::pair<std::string, std::string> ReadOption(const std::vector<std::string>& args, size_t& i) {
            const auto key = args[i];
            const auto equal = key.find('=');
            if (equal != std::string::npos) { return {key.substr(0, equal), key.substr(equal + 1)}; }
            if (++i == args.size()) { Fail("missing value for " + key); }
            return {key, args[i]};
        }

        struct TAnalysis {
            std::string Version, Tree, Baseline;
            TSnapshot Snapshot;
        };

        struct TSelectionCheckpoint {
            std::string Key;
            TLayout Layout;
            std::string DiskReport;
        };

        struct TMeasureConfig {
            std::string Directory, Nonce, Command, Depfile, CacheMode;
            std::vector<std::string> Outputs;
            int Id = -1;
            double Memory = 0; // the model's estimate of the action's RSS, for the reports
        };

        struct TRecord {
            std::string Nonce, Digest;
            int Id = -1, Status = -1;
            TObservation Observation;
            std::vector<std::pair<int64_t, int64_t>> Outputs; // size, mtime
        };

        struct TRunRecord {
            std::string Nonce, Token, Snapshot;
            int Status = -1;
            int64_t Start = 0, End = 0;
            // Earlier attempts of this snapshot only: token -> valid observation interval.
            std::map<std::string, std::pair<int64_t, int64_t>> Earlier;

            bool Contains(const TObservation& o, int64_t now) const {
                int64_t start = Start, end = End ? End : now;
                if (o.Invocation != Token) {
                    auto it = Earlier.find(o.Invocation);
                    if (it == Earlier.end()) { return false; }
                    start = it->second.first; end = it->second.second;
                }
                return o.Time > 0 && o.Start >= start && o.End > o.Start && o.End <= end;
            }
        };

        struct TCompatibilityRecord {
            std::string Command;
            std::vector<std::pair<std::string, std::string>> Inputs;
            int64_t Size = -1, Mtime = -1;
        };

        // One schema for read/write; bounds-checked binary storage avoids JSON
        // overhead for the large sparse tables. Envelopes detect partial edits.
        template <class A> void Fields(A& a, TResources& v) { a(v.Workers, v.Ram, v.Disk, v.Bandwidth, v.Pools); }
        template <class A> void Fields(A& a, TAction& v) { a(v.Output, v.Pool, v.Time, v.Memory, v.Io, v.Phony, v.Dependencies, v.DirtyDependencies, v.Changes); }
        template <class A> void Fields(A& a, TUnit& v) { a(v.Include, v.Headers, v.Children, v.Changes, v.Producers, v.Parse, v.Bytes, v.Stable, v.Safe); }
        template <class A> void Fields(A& a, TFile& v) { a(v.Action, v.Class, v.Source, v.Own, v.Target, v.Frontend, v.Outer); }
        template <class A> void Fields(A& a, TTarget& v) { a(v.Label, v.Class, v.Files, v.PchEligible); }
        template <class A> void Fields(A& a, TData& v) { a(v.Actions, v.Units, v.Files, v.Targets, v.Classes, v.Scenarios, v.Q, v.Resources, v.ActionMemory); }
        template <class A> void Fields(A& a, TPch& v) { a(v.Class, v.Parent, v.Representative, v.Roots, v.Units); }
        template <class A> void Fields(A& a, TLayout& v) { a(v.Pchs, v.Bases, v.Frozen, v.Assignment, v.Controls); }
        template <class A> void Fields(A& a, TPrediction& v) { a(v.Actions, v.Saved, v.Load, v.PchBytes, v.Cold, v.Incremental, v.Objective, v.Feasible); }
        template <class A> void Fields(A& a, TObservation& v) { a(v.Time, v.Cpu, v.Memory, v.Bytes, v.Start, v.End, v.Invocation); }
        template <class A> void Fields(A& a, TCoefficients& v) {
            a(v.Baseline, v.Saved, v.Load, v.Residual, v.BaseBuild, v.DeltaBuild,
              v.Size, v.Memory, v.ConsumerMemory, v.Instantiate, v.Identified);
        }
        template <class A> void Fields(A& a, TMeasuredPch& v) { a(v.Pch, v.Parent, v.Bytes, v.Time, v.Memory); }
        template <class A> void Fields(A& a, TCalibration& v) {
            a(v.Classes, v.Measured, v.Probes, v.ActionTime, v.ActionMemory, v.ActionFloor, v.TargetGain, v.UnitSize, v.UnitBuild, v.UnitGain,
              v.GlobalUnitSize, v.GlobalUnitBuild, v.GlobalBaseBuild, v.GlobalDeltaBuild, v.GlobalSize, v.Report, v.TargetReport, v.UnitReport);
        }
        template <class A> void Fields(A& a, TLinkLine& v) { a(v.Output, v.Inputs); }
        template <class A> void Fields(A& a, TLinkInputs& v) { a(v.Lines, v.Targets, v.Libraries); }
        template <class A> void Fields(A& a, TSnapshot& v) {
            a(v.Root, v.Baseline, v.Directory, v.Args, v.Gn, v.Ninja, v.Nonce, v.Tree, v.Tool, v.Targets, v.GnSwitches, v.Inputs, v.Flags, v.Data, v.Layout, v.Prediction, v.Commands, v.Corrected, v.RetainedInitial, v.Sample, v.Measured, v.ProbeSizes);
            if (a.HasProfile) { a(v.Mode); }
            a(v.LinkInputs);
        }
        template <class A> void Fields(A& a, TAnalysis& v) { a(v.Version, v.Tree, v.Baseline, v.Snapshot); }
        template <class A> void Fields(A& a, TSelectionCheckpoint& v) { a(v.Key, v.Layout, v.DiskReport); }
        template <class A> void Fields(A& a, TMeasureConfig& v) { a(v.Directory, v.Nonce, v.Command, v.Depfile, v.CacheMode, v.Outputs, v.Id, v.Memory); }
        template <class A> void Fields(A& a, TRecord& v) { a(v.Nonce, v.Digest, v.Id, v.Status, v.Observation, v.Outputs); }
        struct TLoad;
        template <class A> void Fields(A& a, TRunRecord& v) {
            a(v.Nonce, v.Token, v.Snapshot, v.Status, v.Start, v.End);
            if constexpr (std::is_same_v<A, TLoad>) {
                if (a.In.AtEnd()) { return; } // original single-attempt record
            }
            a(v.Earlier);
        }
        template <class A> void Fields(A& a, TCompatibilityRecord& v) { a(v.Command, v.Inputs, v.Size, v.Mtime); }

        struct TSave {
            TBinWriter Out;
            bool HasProfile = true;
            template <class... T> void operator()(T&... v) { (One(v), ...); }
            template <class T> void One(T& v) {
                if constexpr (std::is_same_v<T, std::string>) {
                    Out.Str(v);
                } else if constexpr (std::is_same_v<T, double>) {
                    uint64_t bits;
                    static_assert(sizeof(bits) == sizeof(v));
                    std::memcpy(&bits, &v, sizeof(bits));
                    Out.U64(bits);
                } else if constexpr (std::is_integral_v<T>) {
                    Out.I64(int64_t(v));
                } else {
                    Fields(*this, v);
                }
            }
            template <class T> void One(std::vector<T>& v) {
                Out.U64(v.size());
                for (auto& x : v) { One(x); }
            }
            template <class X, class Y> void One(std::pair<X, Y>& v) { One(v.first); One(v.second); }
            template <class T> void One(std::set<T>& v) {
                Out.U64(v.size());
                for (auto x : v) { One(x); }
            }
            template <class X, class Y> void One(std::map<X, Y>& v) {
                Out.U64(v.size());
                for (auto& [key, value] : v) { auto k = key; One(k); One(value); }
            }
        };

        struct TLoad {
            TBinReader In;
            bool HasProfile;
            explicit TLoad(std::string_view data, bool hasProfile) : In(data), HasProfile(hasProfile) {}
            template <class... T> void operator()(T&... v) { (One(v), ...); }
            template <class T> void One(T& v) {
                if constexpr (std::is_same_v<T, std::string>) {
                    v = std::string(In.Str());
                } else if constexpr (std::is_same_v<T, double>) {
                    uint64_t bits = In.U64();
                    std::memcpy(&v, &bits, sizeof(v));
                    if (!std::isfinite(v)) { Fail("snapshot: non-finite number"); }
                } else if constexpr (std::is_integral_v<T>) {
                    int64_t x = In.I64();
                    if (x < int64_t(std::numeric_limits<T>::min()) || x > int64_t(std::numeric_limits<T>::max())) {
                        Fail("snapshot: integer out of range");
                    }
                    v = T(x);
                } else {
                    Fields(*this, v);
                }
            }
            size_t Count() {
                uint64_t n = In.U64();
                if (n > 100000000) { Fail("snapshot: oversized table"); }
                return size_t(n);
            }
            template <class T> void One(std::vector<T>& v) {
                v.resize(Count());
                for (auto& x : v) { One(x); }
            }
            template <class X, class Y> void One(std::pair<X, Y>& v) { One(v.first); One(v.second); }
            template <class T> void One(std::set<T>& v) {
                size_t n = Count();
                for (size_t i = 0; i < n; ++i) { T x; One(x); v.insert(std::move(x)); }
            }
            template <class X, class Y> void One(std::map<X, Y>& v) {
                size_t n = Count();
                for (size_t i = 0; i < n; ++i) { X k; Y x; One(k); One(x); v.emplace(std::move(k), std::move(x)); }
            }
        };

        template <class T>
        std::string Encode(T& value) {
            TSave body;
            body(value);
            TBinWriter envelope;
            envelope.Str(VERSION);
            envelope.Str(HashHex(body.Out.Data()));
            envelope.Str(body.Out.Data());
            return envelope.Data();
        }

        template <class T>
        T Decode(const std::string& text) {
            TBinReader envelope(text);
            const auto version = envelope.Str();
            if (version != VERSION && version != OPEN_CLASSES_VERSION && version != CONTROLS_VERSION && version != ANCHORED_VERSION && version != MERGED_OWNER_VERSION && version != OWNER_VERSION && version != TARGET_VERSION && version != PREVIOUS_VERSION && version != LEGACY_VERSION) {
                Fail("unsupported two-step snapshot version");
            }
            if constexpr (std::is_same_v<T, TSnapshot> || std::is_same_v<T, TAnalysis>) {
                if (version != VERSION) {
                    Fail("plan/analysis schema changed (GN target assignments); run predict again (old results are preserved)");
                }
            }
            if constexpr (std::is_same_v<T, TSelectionCheckpoint>) {
                // A layout of another schema would decode into another layout.
                if (version != VERSION) { Fail("selection checkpoint schema changed; the layout is recomputed"); }
            }
            std::string hash(envelope.Str());
            auto body = envelope.Str();
            if (!envelope.AtEnd() || HashHex(body) != hash) { Fail("snapshot checksum mismatch"); }
            TLoad load(body, version != LEGACY_VERSION);
            T result;
            load(result);
            if (!load.In.AtEnd()) { Fail("snapshot: trailing data"); }
            return result;
        }

        std::string DigestFile(const std::string& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) { Fail("cannot fingerprint " + path); }
            THasher hash;
            char buffer[65536];
            while (in) {
                in.read(buffer, sizeof(buffer));
                hash.Update(std::string_view(buffer, size_t(in.gcount())));
            }
            if (!in.eof()) { Fail("cannot read " + path); }
            return hash.Hex();
        }

        // A byte stream fed to THasher across arbitrary chunk boundaries. The
        // hasher pads a partial word on every update, so the stream is fed in
        // whole words and the digest is that of the whole output.
        struct TStreamHash {
            THasher Hash;
            std::string Pending;

            void Feed(std::string_view chunk) {
                Pending.append(chunk);
                const size_t whole = Pending.size() & ~size_t(7);
                Hash.Update(std::string_view(Pending).substr(0, whole));
                Pending.erase(0, whole);
            }
            std::string Hex() {
                Hash.Update(Pending);
                Pending.clear();
                return Hash.Hex();
            }
        };

        // Identity of a source tree: its HEAD and the changes against it. The
        // diff is hashed as it streams instead of being held whole: it can be
        // gigabytes (a missing index makes git report every file of the
        // repository as deleted, binaries included).
        std::string TreeIdentity(const std::string& root, bool analysis = false) {
            TRunOptions run;
            run.Cwd = root;
            std::vector<std::string> diff = {"git", "diff", "--binary", "HEAD", "--"};
            if (analysis) { diff.insert(diff.end(), {".", ":(exclude)build/gn/pch_update/**"}); }
            TStreamHash hash;
            hash.Feed(RunChecked({"git", "rev-parse", "HEAD"}, run));
            run.OnStdout = [&](std::string_view chunk) { hash.Feed(chunk); };
            RunChecked(diff, run);
            return hash.Hex();
        }

        std::string Program(const std::string& name, const std::string& cwd = {}, bool resolveLinks = true) {
            auto resolve = [&](const std::string& path) {
                auto absolute = cwd.empty() ? AbsPath(path) : NormPath(JoinPath(cwd, path));
                return resolveLinks ? RealPath(absolute) : absolute;
            };
            if (name.find('/') != std::string::npos) { return resolve(name); }
            const char* path = getenv("PATH");
            for (auto directory : Split(path ? path : "", ':')) {
                auto candidate = resolve(JoinPath(directory.empty() ? "." : directory, name));
                if (access(candidate.c_str(), X_OK) == 0) { return candidate; }
            }
            Fail("executable not found: " + name);
        }

        bool Under(const std::string& path, const std::string& dir) {
            return path == dir || StartsWith(path, dir + "/");
        }

        std::string StateDirectory(const std::string& baseline) {
            return JoinPath(RealPath(baseline), ".pch_update");
        }

        std::string ProfileDirectory(const std::string& baseline, const std::string& mode) {
            (void)ModeQ(mode);
            return JoinPath(JoinPath(StateDirectory(baseline), "modes"), mode);
        }

        void CheckProfileOutput(const std::string& baseline, const std::string& mode, const std::string& output) {
            const auto profiles = JoinPath(StateDirectory(baseline), "modes");
            const auto profile = ProfileDirectory(baseline, mode);
            const auto path = NormPath(output);
            if (Under(path, profiles) && (path == profile || !Under(path, profile))) {
                Fail("--output must not overwrite a profile root or another mode's state: " + output);
            }
        }

        int64_t WallNow() {
            return std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // The source root GN recorded in the baseline's build.ninja. Read here
        // without parsing the manifest so the state generation can be checked
        // before any expensive work.
        std::string BuildRoot(const std::string& buildDir) {
            std::ifstream in(JoinPath(buildDir, "build.ninja"));
            std::string line;
            for (int n = 0; n < 200 && std::getline(in, line); ++n) {
                auto pos = line.find("--root=");
                if (pos == std::string::npos) { continue; }
                std::string value;
                for (size_t i = pos + 7; i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])); ++i) {
                    value.push_back(line[i]);
                }
                if (!value.empty()) { return NormPath(JoinPath(buildDir, value)); }
            }
            return {};
        }

        // What gn regenerates, not the ninja logs that every measured build
        // rewrites: the configuration the state was computed for.
        std::string BaselineStamp(const std::string& baseline) {
            TBinWriter stamp;
            for (const char* name : {"args.gn", "build.ninja", "build.ninja.d", "compile_commands.json"}) {
                auto path = JoinPath(baseline, name);
                stamp.Str(path);
                stamp.Str(Exists(path) ? DigestFile(path) : std::string("missing"));
            }
            return HashHex(stamp.Data());
        }

        // One generation of derived state: the analysis schema, the source tree
        // and the baseline configuration it was computed from. Data of another
        // generation cannot be reused, so it is deleted instead of piling up.
        struct TGeneration {
            std::string Schema, Root, Tree, Baseline;
            int64_t Created = 0;
        };

        bool TryReadGeneration(const std::string& path, TGeneration* out) {
            std::string text;
            if (!TryReadFile(path, &text)) { return false; }
            std::istringstream in(text);
            std::string line;
            if (!std::getline(in, line) || line != "pch-update-generation 1") { return false; }
            while (std::getline(in, line)) {
                auto tab = line.find('\t');
                if (tab == std::string::npos) { return false; }
                const auto key = line.substr(0, tab);
                const auto value = line.substr(tab + 1);
                if (key == "schema") { out->Schema = value; }
                else if (key == "root") { out->Root = value; }
                else if (key == "tree") { out->Tree = value; }
                else if (key == "baseline") { out->Baseline = value; }
                else if (key == "created") { out->Created = std::atoll(value.c_str()); }
                else { return false; }
            }
            return !out->Schema.empty() && !out->Baseline.empty() && out->Created > 0;
        }

        void WriteGeneration(const std::string& state, const TGeneration& generation) {
            std::string text = "pch-update-generation 1\n";
            text += "schema\t" + generation.Schema + "\n";
            text += "root\t" + generation.Root + "\n";
            text += "tree\t" + generation.Tree + "\n";
            text += "baseline\t" + generation.Baseline + "\n";
            text += "created\t" + std::to_string(generation.Created) + "\n";
            WriteFile(JoinPath(state, "generation"), text);
        }

        std::string AgeText(int64_t seconds) {
            seconds = std::max<int64_t>(0, seconds);
            if (seconds < 7200) { return std::to_string(seconds / 60) + "m"; }
            if (seconds < 172800) { return std::to_string(seconds / 3600) + "h"; }
            return std::to_string(seconds / 86400) + "d";
        }

        // Why the state of `stored` cannot serve `current`, or "" when it can.
        std::string GenerationMismatch(const TGeneration& stored, const TGeneration& current, int64_t maxAge) {
            if (stored.Schema != current.Schema) { return "analysis schema changed to " + current.Schema; }
            if (stored.Baseline != current.Baseline) { return "baseline configuration changed"; }
            if (!stored.Tree.empty() && !current.Tree.empty() && stored.Tree != current.Tree) { return "source tree changed"; }
            const int64_t age = WallNow() - stored.Created;
            if (maxAge > 0 && age > maxAge) { return "last successful cycle was " + AgeText(age) + " ago"; }
            return {};
        }


        std::string ReadPointer(const std::string& path) {
            std::string text;
            if (!TryReadFile(path, &text)) { return {}; }
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) { text.pop_back(); }
            return text;
        }

        // The measurements and reports of a cycle are a few megabytes; the
        // build directory it produced is tens of gigabytes. Keep the records,
        // drop the objects: an archived run stays explainable and comparable.
        void ShrinkRun(const std::string& run) {
            for (const auto& name : ListDir(run)) {
                const auto plan = JoinPath(run, name);
                if (!IsDir(plan)) { continue; }
                RemoveTree(JoinPath(plan, "build"));
                RemoveTree(JoinPath(plan, "actions"));
                RemoveFile(JoinPath(plan, "snapshot.bin"));
                RemoveFile(JoinPath(plan, "analysis.bin"));
            }
        }

        void PruneRuns(const std::string& state, const std::string& cycle) {
            const auto runs = JoinPath(state, "runs");
            if (!IsDir(runs)) { return; }
            const auto current = cycle.empty() ? std::string() : RealPath(cycle);
            const auto checkpoint = ReadPointer(JoinPath(state, "analysis.path"));
            std::vector<std::pair<int64_t, std::string>> old;
            for (const auto& name : ListDir(runs)) {
                const auto path = JoinPath(runs, name);
                if (!current.empty() && RealPath(path) == current) { continue; }
                // A diagnostic --output keeps the checkpoint it reuses.
                if (!checkpoint.empty() && Under(RealPath(checkpoint), RealPath(path))) { continue; }
                old.emplace_back(MtimeNs(path), path);
            }
            std::sort(old.begin(), old.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            size_t shrunk = 0, removed = 0;
            for (size_t i = 0; i < old.size(); ++i) {
                if (i == 0) {
                    // The last completed cycle keeps its observations and reports.
                    ShrinkRun(old[i].second);
                    ++shrunk;
                } else {
                    RemoveTree(old[i].second);
                    ++removed;
                }
            }
            if (shrunk || removed) {
                Log(Format("state keeps one cycle: %zu previous run(s) archived without build, %zu deleted",
                           shrunk, removed));
            }
        }

        void ClearState(const std::string& state, const std::string& reason) {
            Log("profile state unusable (" + reason + "): retiring " + state);
            for (const char* name : {"analysis.path", "current.path", "result.path"}) {
                RemoveFile(JoinPath(state, name));
            }
            // Measurements stay comparable across generations; their builds do not.
            PruneRuns(state, {});
        }

        void PrepareProfileState(const std::string& shared, const std::string& profile,
                                 const TGeneration& current, int64_t maxAge) {
            // Shared caches follow source/configuration identity, not a mode's
            // age. Switching profiles alone must not invalidate cached work.
            TGeneration common;
            if (!TryReadGeneration(JoinPath(shared, "generation"), &common) ||
                !GenerationMismatch(common, current, 0).empty()) {
                RemoveFile(JoinPath(shared, "analysis.path"));
                WriteGeneration(shared, current);
            }
            TGeneration stored;
            const bool known = TryReadGeneration(JoinPath(profile, "generation"), &stored);
            const auto reason = known ? GenerationMismatch(stored, current, maxAge) : "no profile generation recorded";
            if (!reason.empty()) {
                if (Under(ReadPointer(JoinPath(shared, "analysis.path")), profile)) {
                    RemoveFile(JoinPath(shared, "analysis.path"));
                }
                ClearState(profile, reason);
                WriteGeneration(profile, current);
            }
        }

        // A migrated Clang PCH still embeds its old absolute header paths. The
        // files live in the baseline, while an explicitly recorded symlink
        // alias preserves command/cache identity until those entries expire.
        std::string CacheDirectory(const std::string& path) {
            const auto real = RealPath(path);
            const auto alias = ReadPointer(JoinPath(real, ".path-alias"));
            if (alias.empty()) { return real; }
            if (!IsAbs(alias) || !Exists(alias) || RealPath(alias) != real) {
                Fail("invalid migrated-cache alias: " + alias);
            }
            return NormPath(alias);
        }

        bool StorageAllowed(const std::string& path, const std::string& root, const std::string& baseline) {
            if (Under(path, root) || Under(root, path) || Under(baseline, path)) { return false; }
            const auto managed = StateDirectory(baseline);
            return !Under(path, baseline) || (path != managed && Under(path, managed));
        }

        double DefaultRam() {
            double available = 0;
            std::ifstream memory("/proc/meminfo");
            std::string line;
            while (std::getline(memory, line)) {
                std::istringstream fields(line);
                std::string key;
                double kb = 0;
                if (fields >> key >> kb && key == "MemAvailable:") { available = kb * 1024; break; }
            }
            if (available <= 0) {
                long pages = sysconf(_SC_AVPHYS_PAGES), pageSize = sysconf(_SC_PAGESIZE);
                if (pages > 0 && pageSize > 0) { available = double(pages) * pageSize; }
            }
            // Honor a tighter cgroup-v2 limit when exposed at the cgroup root.
            std::string limit, current;
            if (TryReadFile("/sys/fs/cgroup/memory.max", &limit) &&
                TryReadFile("/sys/fs/cgroup/memory.current", &current)) {
                double maximum = 0, used = 0;
                if (std::istringstream(limit) >> maximum && std::istringstream(current) >> used) {
                    available = std::min(available, std::max(0.0, maximum - used));
                }
            }
            return available * 0.75;
        }

        struct TCycleLock {
            int Fd = -1;
            ~TCycleLock() { if (Fd >= 0) { close(Fd); } }
            void Acquire(const std::string& directory) {
                MakeDirs(directory);
                Fd = open(JoinPath(directory, "cycle.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
                if (Fd < 0) { Fail("cannot open cycle lock in " + directory + ": " + strerror(errno)); }
                if (flock(Fd, LOCK_EX | LOCK_NB) != 0) {
                    if (errno == EWOULDBLOCK) { Fail("another cycle is active in " + directory); }
                    Fail("cannot acquire cycle lock in " + directory + ": " + strerror(errno));
                }
            }
        };

        void NewDirectory(const std::string& path, const std::string& root, const std::string& baseline) {
            MakeDirs(DirName(path));
            const auto real = JoinPath(RealPath(DirName(path)), BaseName(path));
            if (!StorageAllowed(real, root, baseline)) {
                Fail("plan directory must be outside the source tree and baseline, or inside BASELINE/.pch_update/");
            }
            if (mkdir(path.c_str(), 0755) != 0) {
                Fail("a new, nonexistent --output directory is required: " + path);
            }
        }

        // The digests of many files, in the order of their paths. The reads
        // are independent and the files are many, every header and source of
        // the workload: they take the worker count of the run.
        std::vector<std::string> DigestFiles(const std::vector<std::string>& paths, int workers) {
            std::vector<std::string> digests(paths.size());
            ParallelFor(paths.size(), AnalysisThreads(workers), [&](size_t i) { digests[i] = DigestFile(paths[i]); });
            return digests;
        }

        void ValidateInputs(const TSnapshot& s) {
            std::vector<std::string> paths;
            for (const auto& [path, hash] : s.Inputs) { paths.push_back(path); }
            const auto digests = DigestFiles(paths, s.Data.Resources.Workers);
            for (size_t i = 0; i < paths.size(); ++i) {
                if (digests[i] != s.Inputs[i].second) { Fail("input changed since prediction: " + paths[i]); }
            }
        }

        void ValidateIdentity(const TSnapshot& s) {
            if (s.Tool != DigestFile(SelfExe())) { Fail("pch_update executable changed since prediction"); }
            if (s.Tree != TreeIdentity(s.Root)) { Fail("source/configuration tree changed since prediction"); }
            ValidateInputs(s);
        }

        std::string BaselineIdentity(const TSnapshot& s) {
            TBinWriter stamp;
            for (const char* name : {"args.gn", "build.ninja", "build.ninja.d", "compile_commands.json", ".ninja_log", ".ninja_deps"}) {
                auto path = JoinPath(s.Baseline, name);
                stamp.Str(path);
                stamp.Str(Exists(path) ? DigestFile(path) : std::string("missing"));
            }
            // Link-provider caches and resumed analysis must also notice rebuilt
            // libraries, including untraced objects and generated helper targets.
            for (const auto& input : s.LinkInputs.Libraries) {
                const auto path = NormPath(JoinPath(s.Baseline, input));
                stamp.Str(path);
                stamp.I64(FileSize(path));
                stamp.I64(MtimeNs(path));
            }
            // Avoid rereading tens of GB of traces. A rebuild changes the Ninja
            // logs above; also reject replaced/touched object or trace files.
            for (const auto& file : s.Data.Files) {
                auto object = JoinPath(s.Baseline, s.Data.Actions.at(file.Action).Output);
                auto dot = object.rfind('.');
                auto trace = FindTrace(object.substr(0, dot) + ".json");
                for (const auto& path : {object, trace}) {
                    stamp.Str(path);
                    bool exists = Exists(path);
                    stamp.I64(exists);
                    if (exists) { stamp.I64(FileSize(path)); stamp.I64(MtimeNs(path)); }
                }
            }
            return HashHex(stamp.Data());
        }

        void SaveAnalysis(TSnapshot& s) {
            TAnalysis a;
            a.Version = ANALYSIS_VERSION;
            a.Tree = TreeIdentity(s.Root, true);
            a.Baseline = BaselineIdentity(s);
            a.Snapshot = s;
            a.Snapshot.Layout = {};
            a.Snapshot.Prediction = {};
            a.Snapshot.Commands.clear();
            a.Snapshot.Corrected = false;
            a.Snapshot.RetainedInitial = false;
            a.Snapshot.ProbeSizes.clear();
            WriteFile(JoinPath(s.Directory, "analysis.bin"), Encode(a));
        }

        TSnapshot LoadAnalysis(const std::string& path) {
            auto checkpoint = JoinPath(path, "analysis.bin");
            TSnapshot s;
            if (Exists(checkpoint)) {
                auto a = Decode<TAnalysis>(ReadFile(checkpoint));
                if (a.Version != ANALYSIS_VERSION) { Fail("analysis schema changed; run predict again"); }
                s = std::move(a.Snapshot);
                if (a.Tree != TreeIdentity(s.Root, true)) { Fail("analysis source/history/configuration changed; run predict again"); }
                if (a.Baseline != BaselineIdentity(s)) { Fail("analysis baseline changed; run predict again"); }
            } else {
                // Without a separate analysis identity require an unchanged tree.
                s = Decode<TSnapshot>(ReadFile(JoinPath(path, "snapshot.bin")));
                if (s.Tree != TreeIdentity(s.Root)) { Fail("legacy analysis tree changed; run predict again"); }
            }
            ValidateInputs(s);
            ValidateTargets(s.Data);
            (void)ModeQ(s.Mode);
            s.Layout = {};
            s.Prediction = {};
            s.Commands.clear();
            s.Corrected = false;
            s.RetainedInitial = false;
            s.ProbeSizes.clear();
            s.Tree = TreeIdentity(s.Root);
            s.Tool = DigestFile(SelfExe());
            s.Nonce = HashHex(s.Nonce + std::to_string(Now()));
            Log("reusing checked analysis: " + std::to_string(s.Data.Targets.size()) + " targets, " +
                std::to_string(s.Data.Files.size()) + " C++ files; skipping trace/header analysis");
            return s;
        }

        void Replace(std::string& text, const std::string& from, const std::string& to) {
            if (from.empty()) { return; }
            size_t pos = 0;
            while ((pos = text.find(from, pos)) != std::string::npos) {
                text.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        std::vector<std::string> CompileFlags(const std::vector<std::string>& argv, const std::string& source) {
            std::vector<std::string> flags;
            for (size_t i = 0; i < argv.size(); ++i) {
                const auto& a = argv[i];
                if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ" || a == "-x") {
                    if (++i == argv.size()) { Fail("incomplete compiler option"); }
                } else if (a != source && a != "-c" && a != "-MD" && a != "-MMD" && !StartsWith(a, "-ftime-trace")) {
                    if (a == "-include-pch" || a == "-emit-pch" || StartsWith(a, "@")) {
                        Fail("baseline compiler command contains a PCH or response file");
                    }
                    flags.push_back(a);
                }
            }
            if (flags.empty() || (BaseName(flags[0]) != "clang++" && BaseName(flags[0]) != "clang")) {
                Fail("verification needs a direct clang/clang++ compile command");
            }
            return flags;
        }

        std::vector<std::string> Canonical(std::vector<std::string> flags, const std::string& build, const std::string& root) {
            TPaths paths(build, root, "");
            bool pathArg = false;
            for (auto& flag : flags) {
                // cc1 options and their operands are each forwarded separately:
                // -Xclang -include -Xclang header. The wrapper is never a path.
                if (flag == "-Xclang") { continue; }
                if (pathArg) {
                    flag = paths.Normalize(flag);
                    pathArg = false;
                    continue;
                }
                if (flag == "-I" || flag == "-isystem" || flag == "-iquote" || flag == "-include" || flag == "-isysroot") {
                    pathArg = true;
                } else if (StartsWith(flag, "-I") && flag.size() > 2) {
                    flag = "-I" + paths.Normalize(flag.substr(2));
                } else {
                    Replace(flag, build, "<out>");
                    Replace(flag, RelPath(root, build), "<root>");
                    Replace(flag, root, "<root>");
                }
            }
            return flags;
        }

        struct TCompile {
            std::vector<std::string> Argv;
            std::string Source;
        };

        // GN can write a relative `file` and an absolute `-c` operand for
        // generated sources. Keep that spelling out of the compiler class.
        void NormalizeCompileInput(TCompile& c, const std::string& build) {
            for (size_t i = 1; i < c.Argv.size(); ++i) {
                if (c.Argv[i - 1] == "-c" && c.Argv[i] != c.Source &&
                    NormPath(JoinPath(build, c.Argv[i])) == NormPath(JoinPath(build, c.Source))) {
                    c.Argv[i] = c.Source;
                }
            }
        }

        std::map<std::string, TCompile> ReadCompiles(const std::string& build) {
            auto text = ReadFile(JoinPath(build, "compile_commands.json"));
            rapidjson::Document doc;
            doc.Parse(text.data(), text.size());
            if (doc.HasParseError() || !doc.IsArray()) { Fail("invalid compile_commands.json"); }
            std::map<std::string, TCompile> result;
            for (const auto& entry : doc.GetArray()) {
                if (!entry.IsObject() || !entry.HasMember("file") || !entry["file"].IsString() ||
                    !entry.HasMember("directory") || !entry["directory"].IsString()) {
                    Fail("invalid compilation database entry");
                }
                if (RealPath(entry["directory"].GetString()) != RealPath(build)) {
                    Fail("compile database uses an unexpected working directory");
                }
                TCompile c;
                c.Source = entry["file"].GetString();
                if (entry.HasMember("arguments") && entry["arguments"].IsArray()) {
                    for (const auto& word : entry["arguments"].GetArray()) {
                        if (!word.IsString()) { Fail("invalid compiler argument"); }
                        c.Argv.emplace_back(word.GetString());
                    }
                } else if (entry.HasMember("command") && entry["command"].IsString()) {
                    c.Argv = ShellSplit(entry["command"].GetString());
                } else {
                    Fail("compilation database entry has no command");
                }
                NormalizeCompileInput(c, build);
                std::string output;
                for (size_t i = 0; i + 1 < c.Argv.size(); ++i) {
                    if (c.Argv[i] == "-o") { output = c.Argv[i + 1]; }
                }
                if (output.empty()) { Fail("compiler output not found"); }
                if (IsAbs(output)) { output = RelPath(output, build); }
                result.emplace(NormPath(output), std::move(c));
            }
            return result;
        }

        // Whether an undefined symbol of a header compiled alone is supplied by
        // the C++ runtime of any C++ link: operator new and delete, the Itanium
        // C++ ABI, the unwinder and libc++. The entity the symbol names must
        // itself sit in the runtime, which a mangled name tells by its first
        // component: a project function taking a std::string names std in its
        // signature, and operator!= is no operator new. Anything else is a
        // symbol of the project.
        bool IsRuntimeSymbol(std::string_view s) {
            for (const auto* prefix : {"_Znw", "_Zna", "_Zdl", "_Zda", "__cxa_", "_Unwind_"}) {
                if (StartsWith(s, prefix)) { return true; }
            }
            if (s == "__gxx_personality_v0" || s == "__clang_call_terminate") { return true; }
            if (!StartsWith(s, "_Z")) { return false; }
            s.remove_prefix(2);
            // A vtable, a VTT, typeinfo or a guard variable names its class.
            for (const auto* kind : {"TV", "TT", "TI", "TS", "GV"}) {
                if (StartsWith(s, kind)) {
                    s.remove_prefix(2);
                    break;
                }
            }
            // A thunk: its offsets come before the target.
            if (StartsWith(s, "Th") || StartsWith(s, "Tv")) {
                const int offsets = s[1] == 'h' ? 1 : 2;
                for (int k = 0; k < offsets; ++k) {
                    const auto end = s.find('_');
                    if (end == std::string_view::npos) { return false; }
                    s.remove_prefix(end + 1);
                }
            }
            // Internal linkage; a local entity: its enclosing one follows.
            if (StartsWith(s, "L")) { s.remove_prefix(1); }
            if (StartsWith(s, "Z")) { s.remove_prefix(1); }
            // A member: its class follows the qualifiers of the member itself.
            if (StartsWith(s, "N")) {
                s.remove_prefix(1);
                while (!s.empty() && std::string_view("KVrRO").find(s.front()) != std::string_view::npos) { s.remove_prefix(1); }
            }
            return StartsWith(s, "St3__1") || StartsWith(s, "10__cxxabiv1") ||
                   (StartsWith(s, "St") && s.size() > 2 && std::isdigit(static_cast<unsigned char>(s[2])));
        }

        // What a header still needs when it is compiled alone, as far as the C++
        // runtime goes. HeaderFacts links a header alone without it and lists
        // its symbols as unresolved, although every final link of a C++ target
        // supplies them. A static of a non-trivial type, or the init object of
        // <iostream>, is no reason to keep a header out of a base; a symbol of
        // the project still is. Only deltas are link-checked again in
        // Preflight: for a base this is the only guard.
        bool RuntimeOnly(const std::vector<std::string>& unresolved) {
            for (const auto& symbol : unresolved) {
                if (!IsRuntimeSymbol(symbol)) { return false; }
            }
            return true;
        }

        void Extract(TFacts& facts, TSnapshot& s) {
            auto core = BuildModel(facts, 0);
            auto& d = s.Data;
            d.Scenarios = int(facts.CommitFiles.size());
            if (!d.Scenarios && d.Q != 1) { Fail("no history scenarios; use --q 1 for a cold-only objective"); }
            for (const auto& [name, depth] : facts.Manifest->PoolDepths()) { d.Resources.Pools[name] = depth; }
            const auto& edges = facts.Manifest->Edges();
            std::vector<char> wanted(edges.size());
            std::vector<int> stack;
            for (const auto& target : s.Targets) {
                int64_t i = facts.Manifest->Producer(target);
                if (i < 0) { Fail("unknown target " + target); }
                stack.push_back(int(i));
            }
            auto producer = [&](const std::string& normalized) {
                return int(facts.Manifest->Producer(RelPath(facts.Paths.Real(normalized), facts.BuildDir)));
            };
            for (const auto& header : facts.HeaderNames) {
                int i = producer(header);
                if (i >= 0) { stack.push_back(i); }
            }
            while (!stack.empty()) {
                int i = stack.back();
                stack.pop_back();
                if (wanted[i]) { continue; }
                wanted[i] = true;
                for (const auto* list : {&edges[i].Inputs, &edges[i].ImplicitInputs, &edges[i].OrderOnly, &edges[i].Validations}) {
                    for (const auto& path : *list) {
                        int j = int(facts.Manifest->Producer(path));
                        if (j >= 0 && edges[j].Outputs[0] != "build.ninja") { stack.push_back(j); }
                    }
                }
            }
            std::vector<int> remap(edges.size(), -1);
            for (size_t i = 0; i < edges.size(); ++i) {
                if (wanted[i]) { remap[i] = int(d.Actions.size()); d.Actions.emplace_back(); }
            }
            std::unordered_map<std::string, std::vector<int>> changes;
            for (int k = 0; k < d.Scenarios; ++k) {
                for (const auto& path : facts.CommitFiles[k]) { changes[path].push_back(k); }
            }
            auto touch = [&](TAction& a, const std::string& path) {
                auto it = changes.find(path);
                if (it != changes.end()) { a.Changes.insert(a.Changes.end(), it->second.begin(), it->second.end()); }
            };
            for (size_t i = 0; i < edges.size(); ++i) {
                if (remap[i] < 0) { continue; }
                auto& a = d.Actions[remap[i]];
                a.Output = edges[i].Outputs[0];
                a.Phony = edges[i].Rule == "phony";
                a.Pool = facts.Manifest->Binding(i, "pool");
                a.Memory = a.Phony ? 0 : d.ActionMemory;
                auto duration = facts.DurationMs.find(a.Output);
                if (!a.Phony && duration == facts.DurationMs.end()) {
                    Fail("baseline has no duration for " + a.Output);
                }
                a.Time = a.Phony ? 0 : std::max(0.001, duration->second / 1000.0);
                for (const auto* list : {&edges[i].Inputs, &edges[i].ImplicitInputs, &edges[i].OrderOnly}) {
                    for (const auto& path : *list) {
                        int j = int(facts.Manifest->Producer(path));
                        if (j >= 0 && remap[j] >= 0) {
                            a.Dependencies.push_back(remap[j]);
                            if (list != &edges[i].OrderOnly) { a.DirtyDependencies.push_back(remap[j]); }
                        }
                        if (list != &edges[i].OrderOnly) { touch(a, facts.Paths.Normalize(path)); }
                    }
                }
                Unique(a.Dependencies);
                Unique(a.DirtyDependencies);
                Unique(a.Changes);
            }
            // C and other depfile-using actions also participate in historical
            // invalidation; limiting this to traced C++ would miss relinks.
            if (Exists(JoinPath(facts.BuildDir, ".ninja_deps"))) {
                auto deps = ReadNinjaDeps(JoinPath(facts.BuildDir, ".ninja_deps"));
                std::map<std::string, int> actionOf;
                for (size_t i = 0; i < d.Actions.size(); ++i) { actionOf.emplace(d.Actions[i].Output, int(i)); }
                for (auto& row : actionOf) {
                    auto& a = d.Actions[row.second];
                    auto it = deps.Deps.find(row.first);
                    if (it == deps.Deps.end()) { continue; }
                    for (int id : it->second) {
                        const auto& path = deps.Paths[id];
                        touch(a, facts.Paths.Normalize(path));
                        // GN's generated-input ordering is incomplete in a few
                        // targets (for example ydb/core/base includes a graph
                        // protobuf header without an inputdeps edge). The
                        // baseline hid this because the file already existed.
                        // Recorded depfile headers restore real ordering in a
                        // fresh build directory; keep it out of dirty
                        // propagation so invalidation semantics are unchanged.
                        int j = int(facts.Manifest->Producer(path));
                        if (j >= 0 && remap[j] >= 0 && remap[j] != row.second) {
                            a.Dependencies.push_back(remap[j]);
                        }
                    }
                    Unique(a.Changes);
                }
                for (auto& a : d.Actions) { Unique(a.Dependencies); }
            }
            auto compiles = ReadCompiles(facts.BuildDir);
            std::map<std::vector<std::string>, int> classOf;
            std::set<std::string> fingerprint;
            fingerprint.insert(JoinPath(facts.BuildDir, "args.gn"));
            fingerprint.insert(facts.Compiler);
            fingerprint.insert(s.Gn);
            fingerprint.insert(s.Ninja);
            for (const auto& flag : s.GnSwitches) {
                if (StartsWith(flag, "--script-executable=")) { fingerprint.insert(flag.substr(20)); }
            }
            std::string gnDepfile = JoinPath(facts.BuildDir, "build.ninja.d");
            if (Exists(gnDepfile)) {
                for (const auto& path : ReadDepfile(ReadFile(gnDepfile))) {
                    auto real = NormPath(JoinPath(facts.BuildDir, path));
                    if (Exists(real) && !IsDir(real)) { fingerprint.insert(real); }
                }
            }
            int regeneration = int(facts.Manifest->Producer("build.ninja"));
            if (regeneration >= 0) {
                for (const auto* list : {&edges[regeneration].Inputs, &edges[regeneration].ImplicitInputs, &edges[regeneration].OrderOnly}) {
                    for (const auto& path : *list) {
                        auto real = NormPath(JoinPath(facts.BuildDir, path));
                        if (Exists(real) && !IsDir(real)) { fingerprint.insert(real); }
                    }
                }
            }
            d.Files.reserve(facts.Objs.size());
            std::vector<std::string> owners;
            std::set<std::string> ineligibleOwners;
            std::set<std::string> omittedOwners;
            std::vector<int> fileMap(facts.Objs.size(), -1);
            for (size_t f = 0; f < facts.Objs.size(); ++f) {
                int edge = int(facts.EdgeOf[f]);
                const auto& owner = facts.TargetNames.at(facts.TargetOf.at(f));
                if (remap[edge] < 0) { omittedOwners.insert(owner); continue; }
                auto hook = facts.GlobalPchOwner.find(owner);
                if (hook == facts.GlobalPchOwner.end()) {
                    ineligibleOwners.insert(owner);
                } else if (hook->second != owner) {
                    Fail("GN PCH hook for " + owner + " names a different target: " + hook->second);
                }
                const auto it = compiles.find(facts.Objs[f]);
                if (it == compiles.end()) { Fail("compile command missing: " + facts.Objs[f]); }
                auto flags = Canonical(CompileFlags(it->second.Argv, it->second.Source), facts.BuildDir, facts.Root);
                auto [cls, added] = classOf.emplace(flags, int(classOf.size()));
                if (added) { s.Flags.push_back(flags); }
                fileMap[f] = int(d.Files.size());
                TFile file;
                file.Action = remap[edge];
                file.Class = cls->second;
                file.Source = facts.Paths.Normalize(it->second.Source);
                fingerprint.insert(facts.Paths.Real(file.Source));
                auto& a = d.Actions[file.Action];
                a.Time = std::max(0.001, facts.CompileUs[f] / 1e6);
                // The frontend leaves the backend (Execute - Frontend) to a
                // PCH; what it spends outside the outermost includes is what
                // -fpch-instantiate-templates can still remove.
                file.Frontend = std::max<double>(0, facts.FrontendUs[f]) / 1e6;
                file.Outer = std::max<double>(0, facts.HeaderUs[f]) / 1e6;
                for (int dep : facts.DepsOfFile[f]) {
                    touch(a, facts.DepNames[dep]);
                    fingerprint.insert(facts.Paths.Real(facts.DepNames[dep]));
                }
                Unique(a.Changes);
                owners.push_back(owner);
                d.Files.push_back(std::move(file));
            }
            d.Classes = int(classOf.size());
            GroupTargets(d, owners);
            std::unordered_map<std::string, std::string> objects;
            for (const auto& file : d.Files) {
                const auto& output = d.Actions[file.Action].Output;
                objects.emplace(output, output);
            }
            s.LinkInputs = CollectLinkInputs(*facts.Manifest, objects);
            for (auto& target : d.Targets) {
                target.PchEligible = !ineligibleOwners.count(target.Label);
                if (omittedOwners.count(target.Label)) {
                    Fail("workload contains only part of PCH-owner target " + target.Label + "; select the complete target");
                }
            }
            d.Units.resize(core->Headers.size());
            // A PCH size follows the serialized header source, not parse time:
            // cache the source bytes of every header once and sum per unit.
            std::vector<double> sourceBytes(facts.HeaderNames.size(), -1);
            auto sourceSize = [&](int h) {
                if (sourceBytes[h] < 0) {
                    int64_t bytes = FileSize(facts.Paths.Real(facts.HeaderNames[h]));
                    sourceBytes[h] = bytes > 0 ? double(bytes) : 0;
                }
                return sourceBytes[h];
            };
            for (size_t u = 0; u < d.Units.size(); ++u) {
                auto& unit = d.Units[u];
                unit.Children.assign(core->Children[u].begin(), core->Children[u].end());
                unit.Changes.assign(core->Commits[u].begin(), core->Commits[u].end());
                unit.Parse = core->Build[u] / 1e6;
                unit.Stable = unit.Changes.empty();
                std::set<int> counted;
                for (int h : core->Headers[u]) {
                    if (counted.insert(h).second) { unit.Bytes += sourceSize(h); }
                    const auto& name = facts.HeaderNames[h];
                    unit.Headers.push_back(name);
                    fingerprint.insert(facts.Paths.Real(name));
                    unit.Stable &= !StartsWith(name, "<out>/");
                    unit.Safe &= facts.NeedsOfHeader.empty() || RuntimeOnly(facts.NeedsOfHeader[h]);
                    if (unit.Include.empty() && !facts.SelfContained.empty() && facts.SelfContained[h]) {
                        unit.Include = Spelling(facts, name);
                    }
                    int j = producer(name);
                    if (j >= 0 && remap[j] >= 0) { unit.Producers.push_back(remap[j]); }
                }
                Unique(unit.Producers);
                Unique(unit.Changes);
                const auto& own = core->Files[u];
                for (size_t i = 0; i < own.Files.size(); ++i) {
                    int f = fileMap[own.Files[i]];
                    if (f >= 0) { d.Files[f].Own.emplace_back(int(u), own.Own[i] / 1e6); }
                }
            }
            // Explicit inputs of generators and links are part of the source
            // identity too, including untracked files that git diff cannot see.
            for (size_t i = 0; i < edges.size(); ++i) {
                if (!wanted[i]) { continue; }
                for (const auto* list : {&edges[i].Inputs, &edges[i].ImplicitInputs}) {
                    for (const auto& path : *list) {
                        if (facts.Manifest->Producer(path) < 0) {
                            auto real = NormPath(JoinPath(facts.BuildDir, path));
                            if (Exists(real) && !IsDir(real)) { fingerprint.insert(real); }
                        }
                    }
                }
            }
            Log("fingerprinting analysis inputs...");
            const std::vector<std::string> paths(fingerprint.begin(), fingerprint.end());
            const auto digests = DigestFiles(paths, d.Resources.Workers);
            for (size_t i = 0; i < paths.size(); ++i) { s.Inputs.emplace_back(paths[i], digests[i]); }
        }

        // The include layers of the former GroupInputs in groups.cpp: system
        // (sysroot/libc++), contrib, util/library, project sources, generated.
        // A project header included before a system header it shadows can
        // poison macro state for everything included after it; the selector's
        // mass order ignores that.
        int HeaderLayer(const TData& data, int unit) {
            // Start above every real layer so generated-only units land at 4;
            // an empty header list (units of synthetic tests without
            // extraction) stays project-level.
            int rank = data.Units[unit].Headers.empty() ? 3 : 4;
            for (const auto& name : data.Units[unit].Headers) {
                rank = std::min(rank, StartsWith(name, LIBCXX) || StartsWith(name, "<sysroot>") || IsAbs(name) ? 0
                    : StartsWith(name, "contrib/")                                                  ? 1
                    : StartsWith(name, "util/") || StartsWith(name, "library/")                     ? 2
                    : StartsWith(name, "<out>")                                                     ? 4
                                                                                                   : 3);
            }
            return rank;
        }

        std::vector<int> LayeredRoots(const TData& data, const TPch& pch) {
            auto roots = pch.Roots;
            std::stable_sort(roots.begin(), roots.end(), [&](int a, int b) {
                return HeaderLayer(data, a) < HeaderLayer(data, b);
            });
            return roots;
        }

        // Transitive closure of the given roots, sorted; the units a PCH built
        // from exactly these roots contains beyond its parent's units.
        std::vector<int> UnitClosure(const TData& data, const std::vector<int>& roots) {
            std::vector<char> seen(data.Units.size());
            std::vector<int> stack(roots.begin(), roots.end()), result;
            while (!stack.empty()) {
                int u = stack.back();
                stack.pop_back();
                if (u < 0 || size_t(u) >= seen.size()) { Fail("invalid unit in a PCH closure"); }
                if (seen[u]) { continue; }
                seen[u] = true;
                result.push_back(u);
                stack.insert(stack.end(), data.Units[u].Children.begin(), data.Units[u].Children.end());
            }
            Unique(result);
            return result;
        }

        std::string PchHeader(const TData& data, const TPch& pch) {
            std::string header = "#pragma once\n";
            for (int root : LayeredRoots(data, pch)) { header += "#include " + data.Units[root].Include + "\n"; }
            return header;
        }

        // The flags the deployed PCH of a compiler class is built with beyond
        // the class's own, pch_only_cflags of //build/gn/pch/global, as far
        // as they change the artifact a measurement sees. With
        // -fpch-instantiate-templates the PCH instantiates templates itself:
        // larger, slower to build, and it saves its consumers more than the
        // same headers without it. A PCH built without it measures another
        // artifact than the one GN deploys, and passes headers that only
        // break under it on to the build. Left out: the language (the probe
        // names its own), a relocatable PCH, valid only for consumers that
        // resolve its paths against the same sysroot and a plan passes none,
        // and the home of module files, a path convention of the GN build.
        std::vector<std::string> MeasurementPchFlags(const TSnapshot& s) {
            std::vector<std::string> argv, flags;
            if (!TryPchOnlyArgv(s.Root, &argv)) {
                return flags;
            }
            for (size_t i = 0; i < argv.size(); ++i) {
                const auto& flag = argv[i];
                if (flag == "-x" || flag == "-isysroot") {
                    ++i;
                } else if (flag == "-relocatable-pch") {
                    continue;
                } else if (flag == "-Xclang" && i + 1 < argv.size() && argv[i + 1] == "-fmodule-file-home-is-cwd") {
                    ++i;
                } else {
                    flags.push_back(flag);
                }
            }
            return flags;
        }

        std::string Paths(const std::vector<std::string>& paths) {
            std::string text;
            for (const auto& p : paths) { text += " " + NinjaEscapePath(p); }
            return text;
        }

        std::string RuleBinding(const std::string& name, const std::string& value) {
            return value.empty() ? "" : "  " + name + " = " + NinjaEscapeValue(value) + "\n";
        }

        // The baseline was fully built and frozen before analysis. Its compiler
        // depfiles give compilation prerequisites, unlike GN's order-only target
        // dependencies, which also pull in libraries and their unsampled objects.
        // Compatibility probes add inputs exposed only by the assigned PCH.
        using TCompileInputs = std::vector<std::vector<std::string>>;

        std::vector<std::string> CompilerOrderOnly(
            int sample, const std::string& output, const std::vector<std::string>& original,
            const TNinjaDeps& baselineDeps, const std::vector<std::string>& generatedPaths,
            const std::vector<std::string>& probeInputs) {
            if (sample >= 100) { return original; }
            auto found = baselineDeps.Deps.find(output);
            if (found == baselineDeps.Deps.end()) {
                Fail("sampled compilation needs baseline .ninja_deps for " + output +
                     "; refresh the baseline or use --sample 100");
            }
            auto result = probeInputs;
            for (int id : found->second) {
                if (!generatedPaths.at(id).empty()) { result.push_back(generatedPaths[id]); }
            }
            Unique(result);
            return result;
        }

        std::vector<char> MeasurementClosure(const std::vector<std::vector<int>>& dependencies,
                                             const std::vector<int>& roots) {
            std::vector<char> reached(dependencies.size());
            auto pending = roots;
            while (!pending.empty()) {
                int i = pending.back();
                pending.pop_back();
                if (i < 0 || size_t(i) >= dependencies.size()) { Fail("invalid measurement dependency"); }
                if (reached[i]) { continue; }
                reached[i] = true;
                pending.insert(pending.end(), dependencies[i].begin(), dependencies[i].end());
            }
            return reached;
        }

        // How many PCHs are built at once. Building a PCH is the one compile
        // of the plan that does not behave like the ordinary build a worker
        // count is chosen for: forced template instantiation over a large base
        // or delta, and the serialization of hundreds of megabytes of AST,
        // take several times the RSS of a translation unit. The deltas are
        // released together as soon as their base is built, and a plan holds
        // dozens of them, more with the probe allowance than in the final
        // layout, so the full count would start all of them at once. A
        // quarter builds them in a few waves, the same limit in the
        // measurement graph: a few dozen builds against tens of thousands of
        // objects, no measurable cost to the run.
        size_t PchBuildJobs(int workers) {
            return std::max(1, workers / 4);
        }

        void Emit(TSnapshot& s, const TCompileInputs& probeInputs, const std::vector<char>& sampledFiles) {
            const std::string build = JoinPath(s.Directory, "build");
            MakeDirs(build);
            WriteFile(JoinPath(build, "args.gn"), s.Args);
            TRunOptions run;
            run.Cwd = s.Root;
            run.CaptureStdout = run.CaptureStderr = false;
            std::vector<std::string> gn = {s.Gn, "--root=" + s.Root};
            gn.insert(gn.end(), s.GnSwitches.begin(), s.GnSwitches.end());
            gn.insert(gn.end(), {"gen", build, "--export-compile-commands"});
            RunChecked(gn, run);
            TNinjaManifest manifest(build);
            auto compiles = ReadCompiles(build);
            MakeDirs(JoinPath(build, "pch"));
            MakeDirs(JoinPath(s.Directory, "actions"));
            MakeDirs(JoinPath(s.Directory, "observations"));
            const auto& d = s.Data;
            const auto& l = s.Layout;
            const auto& edges = manifest.Edges();
            std::vector<int> edgeOf(d.Actions.size());
            std::vector<int> fileOf(d.Actions.size(), -1);
            for (size_t f = 0; f < d.Files.size(); ++f) { fileOf[d.Files[f].Action] = int(f); }
            std::map<int, int> idOf;
            for (size_t i = 0; i < d.Actions.size(); ++i) {
                int e = int(manifest.Producer(d.Actions[i].Output));
                if (e < 0) { Fail("fresh GN graph changed output: " + d.Actions[i].Output); }
                edgeOf[i] = e;
                idOf[e] = int(i);
            }
            TNinjaDeps baselineDeps;
            std::vector<std::string> generatedPaths;
            TCompileInputs generatedProbes(s.Prediction.Actions.size());
            auto generated = [&](const std::string& path) {
                const auto relative = RelPath(NormPath(JoinPath(s.Baseline, path)), s.Baseline);
                return manifest.Producer(relative) >= 0 ? relative : std::string();
            };
            if (s.Sample < 100) {
                const auto path = JoinPath(s.Baseline, ".ninja_deps");
                baselineDeps = ReadNinjaDeps(path);
                s.Inputs.emplace_back(path, DigestFile(path));
                generatedPaths.reserve(baselineDeps.Paths.size());
                for (const auto& input : baselineDeps.Paths) { generatedPaths.push_back(generated(input)); }
            }
            // A generated header can be below the trace timing threshold and
            // absent from the model's units. Probe depfiles are authoritative
            // for PCH inputs even when measuring the entire target graph.
            for (size_t i = 0; i < probeInputs.size(); ++i) {
                for (const auto& input : probeInputs[i]) {
                    auto path = generated(input);
                    if (!path.empty()) { generatedProbes.at(i).push_back(std::move(path)); }
                }
            }
            std::vector<std::vector<int>> buildDependencies(s.Prediction.Actions.size());
            auto depend = [&](size_t id, const std::vector<std::string>& paths) {
                for (const auto& path : paths) {
                    int producer = int(manifest.Producer(path));
                    if (producer < 0) { continue; }
                    auto found = idOf.find(producer);
                    if (found == idOf.end()) { Fail("unplanned measurement prerequisite: " + path); }
                    buildDependencies[id].push_back(found->second);
                }
            };
            s.Commands.resize(s.Prediction.Actions.size());
            std::string ninja = "# Frozen pch_update plan; use pch_update verify.\nninja_required_version = 1.11\n";
            for (const auto& [name, depth] : manifest.PoolDepths()) {
                if (name != "console") { ninja += "pool " + name + "\n  depth = " + std::to_string(depth) + "\n"; }
                auto pool = d.Resources.Pools.find(name);
                if (pool == d.Resources.Pools.end() || pool->second != depth) { Fail("pool changed since baseline: " + name); }
            }
            // The PCH builds of the graph share one pool of their own, the
            // same limit as the compatibility queue: several times the RSS of
            // an ordinary compile each, and every delta of a base is ready
            // the moment the base is built.
            constexpr const char* PCH_BUILD_POOL = "pch_update_builds";
            ninja += std::string("pool ") + PCH_BUILD_POOL + "\n  depth = " + std::to_string(PchBuildJobs(d.Resources.Workers)) + "\n";
            auto measured = [&](int id, TMeasureConfig config) {
                config.Id = id;
                config.Directory = s.Directory;
                config.Nonce = s.Nonce;
                const auto& prediction = s.Prediction.Actions[id];
                config.Memory = prediction.Memory;
                std::string path = JoinPath(s.Directory, "actions/" + std::to_string(id) + ".bin");
                auto encoded = Encode(config);
                s.Commands[id] = HashHex(encoded);
                WriteFile(path, encoded);
                return ShellJoin({SelfExe(), "measure", path});
            };
            for (size_t i = 0; i < d.Actions.size(); ++i) {
                int e = edgeOf[i];
                const auto& edge = edges[e];
                if (!manifest.Binding(e, "dyndep").empty()) { Fail("dyndep is not supported by frozen plans"); }
                if (edge.Outputs[0] == "build.ninja") { Fail("GN regeneration edge in verification graph"); }
                // GN may choose output-directory-dependent actions. Refuse
                // silently scoring one dependency graph and building another.
                std::vector<int> scheduling, propagating;
                for (const auto* list : {&edge.Inputs, &edge.ImplicitInputs, &edge.OrderOnly}) {
                    for (const auto& path : *list) {
                        int dep = int(manifest.Producer(path));
                        if (dep >= 0 && edges[dep].Outputs[0] != "build.ninja") {
                            auto found = idOf.find(dep);
                            if (found == idOf.end()) { Fail("new dependency in fresh GN graph: " + path); }
                            scheduling.push_back(found->second);
                            if (list != &edge.OrderOnly) { propagating.push_back(found->second); }
                        }
                    }
                }
                Unique(scheduling); Unique(propagating);
                if (propagating != d.Actions[i].DirtyDependencies) {
                    Fail("fresh GN dependency graph differs for " + edge.Outputs[0]);
                }
                for (int dep : scheduling) {
                    if (!std::binary_search(d.Actions[i].Dependencies.begin(), d.Actions[i].Dependencies.end(), dep)) {
                        Fail("GN dependency missing from the planned graph for " + edge.Outputs[0]);
                    }
                }
                // Depfile-recorded generators are scheduled order-only: they must
                // finish first, but they do not propagate historical dirtiness.
                std::vector<std::string> orderOnly = edge.OrderOnly;
                for (int dep : d.Actions[i].Dependencies) {
                    if (std::binary_search(scheduling.begin(), scheduling.end(), dep)) { continue; }
                    const auto& path = s.Prediction.Actions[dep].Output;
                    if (std::find(orderOnly.begin(), orderOnly.end(), path) == orderOnly.end()) {
                        orderOnly.push_back(path);
                    }
                }
                std::string rule = edge.Rule == "phony" ? "phony" : "action_" + std::to_string(i);
                auto implicit = edge.ImplicitInputs;
                if (rule != "phony") {
                    TMeasureConfig config;
                    config.Command = manifest.Command(e);
                    config.Depfile = manifest.Binding(e, "depfile");
                    config.Outputs = edge.Outputs;
                    config.CacheMode = "not-compiler";
                    auto compile = compiles.find(edge.Outputs[0]);
                    if (compile != compiles.end()) {
                        orderOnly = CompilerOrderOnly(s.Sample, edge.Outputs[0], orderOnly,
                                                      baselineDeps, generatedPaths, generatedProbes[i]);
                        auto argv = compile->second.Argv;
                        // The frozen baseline already supplies header traces.
                        // Verification uses runner telemetry; retaining traces
                        // here would waste tens of GB and add serialization I/O.
                        argv.erase(std::remove_if(argv.begin(), argv.end(), [](const auto& arg) {
                            return StartsWith(arg, "-ftime-trace");
                        }), argv.end());
                        auto flags = CompileFlags(argv, compile->second.Source);
                        int f = fileOf[i];
                        if (f >= 0) {
                            if (Canonical(flags, build, s.Root) != s.Flags[d.Files[f].Class]) {
                                Fail("compiler configuration changed: " + edge.Outputs[0]);
                            }
                            int g = l.Assignment[d.Files[f].Target];
                            if (g >= 0) {
                                std::string pch = "pch/" + std::to_string(g) + ".pch";
                                argv.insert(argv.end(), {"-include-pch", pch});
                                implicit.push_back(pch);
                                buildDependencies[i].push_back(int(d.Actions.size()) + g);
                            }
                        }
                        config.Command = ShellJoin(argv);
                        config.CacheMode = "direct-uncached";
                    } else if (edge.Rule == "cxx" || edge.Rule == "cc") {
                        Fail("compiler command absent from fresh database: " + edge.Outputs[0]);
                    }
                    ninja += "rule " + rule + "\n" + RuleBinding("command", measured(int(i), config));
                    for (const char* name : {"depfile", "deps", "pool", "restat", "generator", "rspfile", "rspfile_content"}) {
                        ninja += RuleBinding(name, manifest.Binding(e, name));
                    }
                    ninja += RuleBinding("description", "MEASURE " + edge.Outputs[0]);
                }
                std::vector<std::string> explicitOut(edge.Outputs.begin(), edge.Outputs.begin() + edge.ExplicitOutputs);
                std::vector<std::string> implicitOut(edge.Outputs.begin() + edge.ExplicitOutputs, edge.Outputs.end());
                ninja += "build" + Paths(explicitOut);
                if (!implicitOut.empty()) { ninja += " |" + Paths(implicitOut); }
                ninja += ": " + rule + Paths(edge.Inputs);
                if (!implicit.empty()) { ninja += " |" + Paths(implicit); }
                if (!orderOnly.empty()) { ninja += " ||" + Paths(orderOnly); }
                if (!edge.Validations.empty()) { ninja += " |@" + Paths(edge.Validations); }
                ninja += "\n";
                depend(i, edge.Inputs);
                depend(i, edge.ImplicitInputs);
                depend(i, orderOnly);
                depend(i, edge.Validations);
                Unique(buildDependencies[i]);
            }
            for (size_t g = 0; g < l.Pchs.size(); ++g) {
                const auto& p = l.Pchs[g];
                const auto& file = d.Files[p.Representative];
                const auto& compile = compiles.at(d.Actions[file.Action].Output);
                auto argv = CompileFlags(compile.Argv, compile.Source);
                const auto pchFlags = MeasurementPchFlags(s);
                argv.insert(argv.end(), pchFlags.begin(), pchFlags.end());
                std::string source = "pch/" + std::to_string(g) + ".h";
                std::string output = "pch/" + std::to_string(g) + ".pch";
                std::string header = PchHeader(d, p);
                WriteFile(JoinPath(build, source), header);
                if (p.Parent >= 0) { argv.insert(argv.end(), {"-include-pch", "pch/" + std::to_string(p.Parent) + ".pch"}); }
                argv.insert(argv.end(), {"-x", "c++-header", "-MD", "-MF", output + ".d", source, "-o", output});
                TMeasureConfig config;
                config.Command = ShellJoin(argv);
                config.Depfile = output + ".d";
                config.Outputs = {output};
                config.CacheMode = "direct-uncached";
                int id = int(d.Actions.size() + g);
                std::string rule = "pch_" + std::to_string(g);
                ninja += "rule " + rule + "\n" + RuleBinding("command", measured(id, config)) +
                    RuleBinding("depfile", config.Depfile) + RuleBinding("pool", PCH_BUILD_POOL) + "  deps = gcc\n";
                std::vector<std::string> deps;
                for (int dep : s.Prediction.Actions[id].Dependencies) { deps.push_back(s.Prediction.Actions[dep].Output); }
                buildDependencies[id] = s.Prediction.Actions[id].Dependencies;
                deps.insert(deps.end(), generatedProbes[id].begin(), generatedProbes[id].end());
                depend(id, generatedProbes[id]);
                Unique(deps);
                Unique(buildDependencies[id]);
                ninja += "build " + output + ": " + rule + " " + source;
                if (!deps.empty()) { ninja += " |" + Paths(deps); }
                ninja += "\n";
            }
            // Verification builds what calibration needs. At 100 % that is the
            // complete graph. Below it, every PCH and a budgeted sample of
            // consumer objects. Generators and their tools
            // retain their actual inputs, including any necessary links.
            std::vector<std::string> all;
            std::set<int> selectedTargets;
            for (size_t f = 0; f < sampledFiles.size(); ++f) {
                if (sampledFiles[f]) { selectedTargets.insert(d.Files[f].Target); }
            }
            s.Measured.clear();
            for (size_t i = 0; i < s.Prediction.Actions.size(); ++i) {
                const auto& action = s.Prediction.Actions[i];
                bool take = s.Sample >= 100;
                if (!take) {
                    const bool pch = int(i) >= int(d.Actions.size());
                    take = pch || (fileOf[i] >= 0 && sampledFiles[fileOf[i]]);
                }
                if (!take) { continue; }
                all.push_back(action.Output);
                if (!action.Phony) { s.Measured.push_back(int(i)); }
            }
            ninja += "build _pch_verify: phony" + Paths(all) + "\ndefault _pch_verify\n";
            const auto reached = MeasurementClosure(buildDependencies, s.Measured);
            std::vector<char> selected(reached.size());
            for (int i : s.Measured) { selected[i] = true; }
            size_t commands = 0, full = 0, compilers = 0, selectedCompilers = 0;
            std::ostringstream sampling;
            sampling << "id\toutput\trole\tkind\ttarget\tsample_reason\n";
            for (size_t i = 0; i < reached.size(); ++i) {
                const auto& action = s.Prediction.Actions[i];
                if (action.Phony) { continue; }
                ++full;
                if (!reached[i]) { continue; }
                ++commands;
                const bool compiler = i < d.Actions.size() && compiles.count(action.Output);
                compilers += compiler;
                selectedCompilers += compiler && selected[i];
                sampling << i << '\t' << action.Output << '\t' << (selected[i] ? "sample" : "prerequisite")
                         << '\t' << (i >= d.Actions.size() ? "pch" : compiler ? "compile" : "action")
                         << '\t' << (i >= fileOf.size() || fileOf[i] < 0 ? "" : d.Targets[d.Files[fileOf[i]].Target].Label)
                         << '\t' << (!selected[i] ? "prerequisite" : i >= d.Actions.size() ? "pch-build" :
                            s.Sample == 100 ? "full" : "target-stratum") << '\n';
            }
            sampling << "# sampled_compilations\t" << selectedCompilers
                     << "\n# sampled_targets\t" << selectedTargets.size() << "\n# full_targets\t" << d.Targets.size()
                     << "\n# required_compilations\t" << compilers
                     << "\n# required_commands\t" << commands << "\n# full_commands\t" << full << '\n';
            WriteFile(JoinPath(s.Directory, "sampling.tsv"), sampling.str());
            Report(Format("  Measurement sample: %d%% of C++ objects\n"
                          "    Selected targets: %zu / %zu; selected C++ files: %zu\n"
                          "    PCH builds: %zu; calibration actions: %zu\n"
                          "    C++ incl. prerequisites: %zu; total commands: %zu / %zu\n"
                          "    All PCHs and required tools are built; sample percentage is not a build-time fraction.",
                          s.Sample, selectedTargets.size(), d.Targets.size(), selectedCompilers, l.Pchs.size(), s.Measured.size(),
                          compilers, commands, full));
            WriteFile(JoinPath(build, "measured.ninja"), ninja);
            s.Inputs.emplace_back(JoinPath(build, "measured.ninja"), DigestFile(JoinPath(build, "measured.ninja")));
            s.Inputs.emplace_back(JoinPath(build, "args.gn"), DigestFile(JoinPath(build, "args.gn")));
            for (size_t g = 0; g < l.Pchs.size(); ++g) {
                std::string path = JoinPath(build, "pch/" + std::to_string(g) + ".h");
                s.Inputs.emplace_back(path, DigestFile(path));
            }
        }

        int Measure(const std::string& path) {
            auto bytes = ReadFile(path);
            auto c = Decode<TMeasureConfig>(bytes);
            const char* token = getenv("PCH_UPDATE_RUN");
            if (!token || !*token) { Fail("run this manifest with pch_update verify --plan DIR"); }
            auto run = Decode<TRunRecord>(ReadFile(JoinPath(c.Directory, "run.bin")));
            if (run.Nonce != c.Nonce || run.Token != token || run.End) { Fail("stale verification invocation"); }
            // Keep the measurement lock until this worker exits, including if
            // Ninja dies, but never lend it to the command being measured.
            if (const auto* value = getenv("PCH_UPDATE_VERIFY_LOCK_FD")) {
                const int fd = std::stoi(value);
                struct stat actual{}, expected{};
                const auto name = JoinPath(c.Directory, "verification.lock/cycle.lock");
                if (fstat(fd, &actual) != 0 || stat(name.c_str(), &expected) != 0 ||
                    actual.st_dev != expected.st_dev || actual.st_ino != expected.st_ino ||
                    fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) { Fail("invalid inherited verification lock"); }
                unsetenv("PCH_UPDATE_VERIFY_LOCK_FD");
            }
            std::string recordPath = JoinPath(c.Directory, "observations/" + std::to_string(c.Id));
            TCycleLock actionLock;
            actionLock.Acquire(recordPath + ".lock");
            TRecord record;
            record.Id = c.Id;
            record.Nonce = c.Nonce;
            record.Digest = HashHex(bytes);
            record.Observation.Invocation = token;
            TRunOptions options;
            // Ninja alone limits how many of these run at once: the worker
            // count and the pools of the graph, as in any build of the project.
            options.Cwd = JoinPath(c.Directory, "build");
            options.CaptureStdout = options.CaptureStderr = false;
            options.Env = {"CCACHE_DISABLE=1", "SCCACHE_DISABLE=1", "DC_RECACHE=1"};
            for (const auto& output : c.Outputs) { MakeDirs(DirName(JoinPath(options.Cwd, output))); }
            record.Observation.Start = Now();
            record.Status = Run({"/bin/sh", "-c", c.Command}, options).ExitCode;
            record.Observation.End = Now();
            record.Observation.Time = (record.Observation.End - record.Observation.Start) / 1e9;
            rusage usage{};
            if (getrusage(RUSAGE_CHILDREN, &usage) != 0) { Fail("getrusage failed"); }
            record.Observation.Cpu = usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
                (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
            record.Observation.Memory = usage.ru_maxrss * 1024.0;
            if (!record.Status && !c.Depfile.empty() && c.CacheMode == "direct-uncached") {
                std::string depfile = JoinPath(options.Cwd, c.Depfile);
                auto deps = ReadDepfile(ReadFile(depfile));
                for (auto& dep : deps) {
                    if (IsAbs(dep)) { dep = RelPath(dep, options.Cwd); }
                    dep = NormPath(dep);
                }
                WriteFile(depfile, WriteDepfile(c.Outputs.front(), deps));
            }
            for (const auto& output : c.Outputs) {
                std::string full = JoinPath(options.Cwd, output);
                int64_t size = FileSize(full);
                int64_t mtime = MtimeNs(full);
                record.Outputs.emplace_back(size, mtime);
                if (!record.Status && (size < 0 || mtime < 0)) { record.Status = 1; }
                if (size > 0) { record.Observation.Bytes += size; }
            }
            WriteFile(recordPath + ".bin", Encode(record));
            return record.Status;
        }

        // Compatibility probes use the completed baseline's generated inputs,
        // never the measured graph. Only successful probes are cached: a failed
        // or interrupted compiler may not have produced a complete depfile.
        // Bump when probe results or their keys are incompatible. v1 mixed
        // multiple key formats, so it cannot be migrated entry by entry.
        constexpr int COMPATIBILITY_CACHE_VERSION = 2;

        std::string CompatibilityCacheName(int version = COMPATIBILITY_CACHE_VERSION) {
            return "compatibility-v" + std::to_string(version);
        }

        void PrepareCompatibilityCache(const std::string& workDir, TCycleLock& lock) {
            // Different profiles/baselines may share --work-dir. Keep this
            // lock outside version directories and hold it throughout preflight.
            lock.Acquire(JoinPath(workDir, ".compatibility-lock"));
            for (int version = 1; version < COMPATIBILITY_CACHE_VERSION; ++version) {
                const auto obsolete = JoinPath(workDir, CompatibilityCacheName(version));
                if (Exists(obsolete)) {
                    Log("removing obsolete compatibility cache: " + obsolete);
                    RemoveTree(obsolete);
                }
            }
        }

        class TCompatibilityCache {
        public:
            explicit TCompatibilityCache(std::string root)
                : Root_(std::move(root))
            {
                MakeDirs(Root_);
                Root_ = CacheDirectory(Root_);
            }

            std::string Directory(const std::string& key) const { return JoinPath(Root_, key); }

            std::string Digest(const std::string& path) {
                std::lock_guard<std::mutex> lock(Mutex_);
                auto [it, added] = Digests_.try_emplace(path);
                if (added) {
                    // Every parent PCH is checked before its consumers. Avoid
                    // hashing the same multi-GB artifact for every TU.
                    it->second = Under(path, Root_) && path.ends_with(".pch")
                        ? std::to_string(FileSize(path)) + ":" + std::to_string(MtimeNs(path))
                        : DigestFile(path);
                }
                return it->second;
            }

            bool Probe(const std::string& key, const std::string& cwd,
                       std::vector<std::string> argv, const std::string& header = {},
                       const std::function<TRunResult(const std::vector<std::string>&, const TRunOptions&)>& run = Run,
                       const std::string& output = {}, std::vector<std::string> extraInputs = {}) {
                const auto dir = Directory(key);
                MakeDirs(dir);
                struct TLock {
                    int Fd;
                    ~TLock() { if (Fd >= 0) { close(Fd); } }
                } lock{open(JoinPath(dir, "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644)};
                if (lock.Fd < 0 || flock(lock.Fd, LOCK_EX) != 0) { Fail("cannot lock compatibility cache: " + dir); }
                const auto artifact = header.empty() ? output : JoinPath(dir, "output.pch");
                if (!header.empty()) { WriteFileIfChanged(JoinPath(dir, "input.h"), header); }
                const auto depfile = JoinPath(dir, "deps.d");
                argv.insert(argv.end(), {"-MD", "-MF", depfile, "-ferror-limit=1"});
                TCompatibilityRecord record;
                record.Command = HashHex(cwd + "\n" + ShellJoin(argv));
                const auto result = JoinPath(dir, "result.bin");
                if (Exists(result)) {
                    try {
                        auto saved = Decode<TCompatibilityRecord>(ReadFile(result));
                        bool valid = saved.Command == record.Command && !saved.Inputs.empty();
                        if (!artifact.empty()) {
                            valid &= saved.Size > 0 && saved.Size == FileSize(artifact) && saved.Mtime == MtimeNs(artifact);
                        }
                        for (const auto& [path, digest] : saved.Inputs) {
                            if (!Exists(path) || Digest(path) != digest) { valid = false; break; }
                        }
                        if (valid) {
                            RemoveFile(depfile); // dependencies are already in result.bin
                            ++Hits;
                            return true;
                        }
                    } catch (const TError&) {
                        // An incomplete/corrupt cache entry is not an analysis failure.
                    }
                    RemoveFile(result);
                }
                RemoveFile(depfile);
                if (!artifact.empty()) { RemoveFile(artifact); }
                TRunOptions options;
                options.Cwd = cwd;
                options.Env = {"CCACHE_DISABLE=1", "SCCACHE_DISABLE=1", "DC_RECACHE=1"};
                options.MergeStderr = true;
                options.StdoutFile = JoinPath(dir, "compiler.log");
                auto status = run(argv, options).ExitCode;
                ++Runs;
                if (status) {
                    // Signals, exec failures and compiler crashes are not
                    // evidence that an assignment is incompatible.
                    auto log = Exists(options.StdoutFile) ? ReadFile(options.StdoutFile) : std::string();
                    if (status != 1 || log.find("error:") == std::string::npos ||
                        log.find("unable to execute command") != std::string::npos ||
                        log.find("clang frontend command failed") != std::string::npos ||
                        log.find("unable to open output file") != std::string::npos ||
                        log.find("No space left on device") != std::string::npos ||
                        log.find("Permission denied") != std::string::npos ||
                        log.find("PLEASE submit a bug report") != std::string::npos) {
                        Fail("compatibility compiler failed; see " + options.StdoutFile);
                    }
                    return false;
                }
                if (!Exists(depfile) || (!artifact.empty() && FileSize(artifact) <= 0)) {
                    Fail("compatibility probe produced incomplete outputs: " + dir);
                }
                auto dependencies = ReadDepfile(ReadFile(depfile));
                if (dependencies.empty()) { Fail("empty compatibility depfile: " + depfile); }
                dependencies.push_back(argv.front());
                // Clang need not list the serialized parent itself in -MD.
                // Revalidate it even when its command and cache path are unchanged.
                dependencies.insert(dependencies.end(), extraInputs.begin(), extraInputs.end());
                for (auto& path : dependencies) { path = NormPath(JoinPath(cwd, path)); }
                Unique(dependencies);
                for (const auto& path : dependencies) { record.Inputs.emplace_back(path, Digest(path)); }
                if (!artifact.empty()) {
                    record.Size = FileSize(artifact);
                    record.Mtime = MtimeNs(artifact);
                    // A stale record may have caused this artifact to be rebuilt.
                    std::lock_guard<std::mutex> guard(Mutex_);
                    Digests_.erase(artifact);
                }
                WriteFile(result, Encode(record)); // atomic; never publish partial successes
                RemoveFile(depfile);
                return true;
            }

            std::atomic<size_t> Hits{0}, Runs{0};
        private:
            std::string Root_;
            std::mutex Mutex_;
            std::map<std::string, std::string> Digests_;
        };

        // Only ready PCHs occupy workers. Completion publishes the parent's
        // artifacts before releasing its children; exceptions wake every waiter.
        void ParallelPchs(const std::vector<TPch>& pchs, size_t jobs, const std::function<void(size_t)>& check) {
            std::vector<std::vector<size_t>> children(pchs.size());
            std::deque<size_t> ready;
            for (size_t g = 0; g < pchs.size(); ++g) {
                int parent = pchs[g].Parent;
                if (parent < -1 || parent >= int(g)) { Fail("compatibility: invalid PCH parent"); }
                if (parent < 0) { ready.push_back(g); }
                else { children[parent].push_back(g); }
            }
            std::mutex mutex;
            std::condition_variable changed;
            size_t remaining = pchs.size();
            bool failed = false;
            size_t workers = std::min(pchs.size(), std::max<size_t>(1, jobs));
            ParallelFor(workers, workers, [&](size_t) {
                for (;;) {
                    size_t g;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        changed.wait(lock, [&] { return failed || !remaining || !ready.empty(); });
                        if (failed || !remaining) { return; }
                        g = ready.front();
                        ready.pop_front();
                    }
                    try {
                        check(g);
                    } catch (...) {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            failed = true;
                        }
                        changed.notify_all();
                        throw;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        --remaining;
                        ready.insert(ready.end(), children[g].begin(), children[g].end());
                    }
                    changed.notify_all();
                }
            });
        }

        struct TCompatibilityProgress {
            size_t Targets = 0, FinishedTargets = 0, Files = 0, Checks = 0, Retries = 0;
        };

        void CompatibleTargets(const TData& data, TLayout& layout, const std::vector<char>& built,
                               size_t jobs, const std::function<bool(int, int)>& check,
                               const std::function<void(const TCompatibilityProgress&, bool)>& notify = {}) {
            std::vector<int> pending;
            TCompatibilityProgress progress;
            for (size_t t = 0; t < data.Targets.size(); ++t) {
                if (layout.Assignment[t] >= 0) {
                    pending.push_back(int(t));
                    progress.Files += data.Targets[t].Files.size();
                }
            }
            progress.Targets = pending.size();
            if (notify) { notify(progress, true); }
            std::mutex mutex;
            auto nextUpdate = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            bool retryWave = false;
            // Probe all files in parallel waves; reduce failures only after the
            // wave. A single failing member moves the ENTIRE target to its parent.
            while (!pending.empty()) {
                std::vector<int> files;
                for (int t : pending) {
                    int& g = layout.Assignment[t];
                    while (g >= 0 && !built[g]) { g = layout.Pchs[g].Parent; }
                    const auto& members = data.Targets[t].Files;
                    files.insert(files.end(), members.begin(), members.end());
                }
                std::vector<char> ok(data.Files.size());
                std::vector<size_t> passed(data.Targets.size());
                ParallelFor(files.size(), jobs, [&](size_t i) {
                    int f = files[i];
                    int t = data.Files[f].Target;
                    ok[f] = check(f, layout.Assignment[t]);
                    if (notify) {
                        std::lock_guard<std::mutex> lock(mutex);
                        ++progress.Checks;
                        progress.Retries += retryWave;
                        if (ok[f] && ++passed[t] == data.Targets[t].Files.size()) {
                            ++progress.FinishedTargets;
                        }
                        const auto now = std::chrono::steady_clock::now();
                        if (now >= nextUpdate) {
                            notify(progress, false);
                            nextUpdate = now + std::chrono::seconds(2);
                        }
                    }
                });
                std::vector<int> retry;
                for (int t : pending) {
                    const auto& members = data.Targets[t].Files;
                    if (std::all_of(members.begin(), members.end(), [&](int f) { return ok[f]; })) { continue; }
                    int& g = layout.Assignment[t];
                    if (g < 0) { Fail("compatibility failed without PCH for target " + data.Targets[t].Label); }
                    g = layout.Pchs[g].Parent;
                    retry.push_back(t);
                }
                pending = std::move(retry);
                retryWave = true;
            }
            if (notify && progress.Targets) { notify(progress, true); }
        }



        struct TLeafLinkProbe {
            std::string Key;
            bool Ok = false;
            std::vector<std::string> Needs;
        };

        TLeafLinkProbe ProbeLeafLink(TCompatibilityCache& cache, const std::string& baseline,
                                    std::vector<std::string> flags, const std::string& pch,
                                    const std::string& pchKey) {
            TLeafLinkProbe result;
            result.Key = "link-" + pchKey;
            const auto dir = cache.Directory(result.Key);
            const auto source = JoinPath(dir, "empty.cpp");
            const auto output = JoinPath(dir, "output.so");
            WriteFileIfChanged(source, "// Probe the complete PCH with the consumer's code-generation flags.\n");
            // Keep the actual optimization, target, sysroot and ABI flags. The
            // minimal system link removes ordinary runtime references; remaining
            // strong symbols must be supplied by each consumer's actual links.
            flags.insert(flags.end(), {"-include-pch", pch, "-x", "c++", source, "-shared", "-nodefaultlibs",
                "-fuse-ld=lld", "-Wl,--gc-sections", "-o", output, "-lc", "-lm", "-ldl", "-lpthread", "-lrt"});
            result.Ok = cache.Probe(result.Key, baseline, flags, {}, Run, output, {pch});
            if (result.Ok) { result.Needs = ReadSharedObjectSymbols(output).Unresolved; }
            return result;
        }

        TLinkAvailability TargetLinkAvailability(const TSnapshot& s, int target,
                                                const std::vector<std::string>& needs,
                                                const TSymbolProviders& providers) {
            std::vector<int> links;
            bool complete = true;
            for (int f : s.Data.Targets.at(target).Files) {
                const auto& output = s.Data.Actions[s.Data.Files[f].Action].Output;
                auto it = s.LinkInputs.Targets.find(output);
                if (it == s.LinkInputs.Targets.end() || it->second.empty()) { complete = false; }
                else { links.insert(links.end(), it->second.begin(), it->second.end()); }
            }
            Unique(links);
            auto result = CheckLinkRequirements(needs, providers, s.LinkInputs.Lines, links);
            result.UnknownLinks |= !complete && !needs.empty();
            return result;
        }

        std::string LinkDiagnostic(const TLinkAvailability& result) {
            std::vector<std::string> reasons;
            if (result.UnknownLinks) { reasons.push_back("unknown final links for an owner member"); }
            for (const auto& symbol : result.UnknownSymbols) {
                reasons.push_back("unknown provider for " + symbol);
            }
            for (const auto& [symbol, links] : result.Missing) {
                for (const auto& link : links) { reasons.push_back(link + ": no known provider for " + symbol); }
            }
            return Join(reasons, "; ");
        }

        TCompileInputs Preflight(TSnapshot& s, const std::string& workDir, const TCalibration& calibration = {},
                                 size_t analysisJobs = 1, std::vector<uint64_t>* retainedBytes = nullptr) {
            if (retainedBytes) { retainedBytes->clear(); }
            s.ProbeSizes.clear();
            auto& layout = s.Layout;
            TCompileInputs inputs(s.Data.Actions.size() + layout.Pchs.size());
            const auto cacheRoot = JoinPath(workDir, CompatibilityCacheName());
            if (!StorageAllowed(RealPath(workDir), s.Root, s.Baseline) ||
                (Exists(cacheRoot) && !StorageAllowed(RealPath(cacheRoot), s.Root, s.Baseline))) {
                Fail("compatibility cache must be outside the source tree and baseline, or inside BASELINE/.pch_update/");
            }
            TCycleLock cacheLock;
            PrepareCompatibilityCache(workDir, cacheLock);
            if (layout.Pchs.empty()) {
                WriteFile(JoinPath(s.Directory, "disk-budget.tsv"), "# no selected artifacts; retained_bytes=0\n");
                return inputs;
            }
            std::set<int> baseClasses;
            for (size_t g = 0; g < layout.Pchs.size(); ++g) {
                const auto& p = layout.Pchs[g];
                if (int(g) < layout.Bases ? (p.Parent != -1 || !baseClasses.insert(p.Class).second) :
                    (p.Parent < 0 || p.Parent >= layout.Bases || layout.Pchs[p.Parent].Class != p.Class)) {
                    Fail("invalid two-level PCH hierarchy; run predict again");
                }
            }
            if (layout.Frozen < 0 || layout.Frozen > layout.Bases) {
                Fail("invalid frozen base count; run predict again");
            }
            TCompatibilityCache cache(cacheRoot);
            // Use validated records on both cache hits and fresh probes. These
            // include headers exposed by a PCH that baseline depfiles cannot see.
            auto probeInputs = [&](const std::string& key) {
                std::vector<std::string> result;
                const auto record = Decode<TCompatibilityRecord>(ReadFile(JoinPath(cache.Directory(key), "result.bin")));
                for (const auto& [path, digest] : record.Inputs) {
                    if (Under(path, s.Baseline) && !Under(path, StateDirectory(s.Baseline))) {
                        result.push_back(RelPath(path, s.Baseline));
                    }
                }
                return result;
            };
            auto compiles = ReadCompiles(s.Baseline);
            std::vector<std::vector<std::string>> flags(s.Data.Files.size());
            std::vector<std::string> sources(s.Data.Files.size());
            for (size_t f = 0; f < s.Data.Files.size(); ++f) {
                const auto& file = s.Data.Files[f];
                const auto& compile = compiles.at(s.Data.Actions[file.Action].Output);
                sources[f] = compile.Source;
                flags[f] = CompileFlags(compile.Argv, compile.Source);
                if (Canonical(flags[f], s.Baseline, s.Root) != s.Flags.at(file.Class)) {
                    Fail("compiler class changed before compatibility checking: " + file.Source);
                }
                // clang++ can be a symlink to clang. Preserve argv[0]: the
                // driver name affects language selection for .c inputs.
                flags[f][0] = Program(flags[f][0], s.Baseline, false);
            }
            // The frozen plan validates the full tree/baseline separately.
            // Individual probes depend on their invocation and actual inputs.
            TBinWriter context;
            context.Str(CompatibilityCacheName());
            context.Str(s.Baseline);
            context.Str(s.Root);
            for (const char* name : {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "SDKROOT",
                                     "SOURCE_DATE_EPOCH", "LANG", "LC_ALL", "LC_CTYPE", "PATH",
                                     "LIBRARY_PATH", "COMPILER_PATH", "GCC_EXEC_PREFIX"}) {
                const char* value = getenv(name);
                context.Str(name);
                context.Str(value ? value : "");
            }
            const auto identity = HashHex(context.Data());
            std::vector<std::string> keys(layout.Pchs.size()), artifacts(layout.Pchs.size());
            std::vector<char> built(layout.Pchs.size());
            std::vector<TLeafLinkProbe> linkProbes(layout.Pchs.size());
            std::set<std::string> linkNeeds;
            auto prediction = Predict(s.Data, layout, calibration, analysisJobs);
            double memory = s.Data.ActionMemory;
            for (size_t g = 0; g < layout.Pchs.size(); ++g) {
                memory = std::max(memory, prediction.Actions.at(s.Data.Actions.size() + g).Memory);
            }
            for (size_t f = 0; f < s.Data.Files.size(); ++f) {
                if (layout.Assignment[s.Data.Files[f].Target] >= 0) {
                    memory = std::max(memory, prediction.Actions.at(s.Data.Files[f].Action).Memory);
                }
            }
            // The checks compile one file at a time with its PCH mapped
            // read-only, lighter than the compiles of the ordinary build the
            // worker count was chosen for: they take the full count.
            const size_t jobs = std::max(1, s.Data.Resources.Workers);
            std::ostringstream report;
            report << "kind\ttarget_or_group\told_leaf\tnew_leaf_before_compaction\tdiagnostics\n";
            const auto reportHeader = report.str();
            std::vector<std::string> rows(layout.Pchs.size());
            std::mutex reportMutex;
            size_t completed = 0;
            const auto compatibilityStart = Now();
            const size_t building = PchBuildJobs(s.Data.Resources.Workers);
            Log(Format("compatibility: %zu PCHs, %zu built at once; %zu check workers (of %d); largest predicted action RSS %.2f GB",
                       layout.Pchs.size(), building, jobs, s.Data.Resources.Workers, memory / 1e9));
            Detail("compatibility cache: " + RealPath(cacheRoot));
            // Roots dropped by bisection per PCH, for reporting and for the
            // children of a reduced parent (their stored Units must shrink too).
            std::vector<std::vector<int>> reduced(layout.Pchs.size());
            // The artifact a plan builds and measures is the artifact GN
            // deploys, pch_only_cflags included.
            const auto pchFlags = MeasurementPchFlags(s);
            ParallelPchs(layout.Pchs, building, [&](size_t g) {
                auto& pch = layout.Pchs[g];
                if (pch.Parent >= 0 && !built[pch.Parent]) {
                    std::lock_guard<std::mutex> lock(reportMutex);
                    rows[g] = Format("pch\t%zu\t%zu\t-1\tparent group %d failed; child probe skipped\n", g, g, pch.Parent);
                    Progress(Format("compatibility PCH %zu/%zu: skipped (parent rejected)", ++completed, layout.Pchs.size()));
                    return;
                }
                std::ostringstream row;
                auto argv = flags.at(pch.Representative);
                const auto parentKey = pch.Parent < 0 ? std::string() : keys[pch.Parent];
                auto probePch = [&](const std::vector<int>& roots) {
                    TPch candidate = pch;
                    candidate.Roots = roots;
                    const auto header = PchHeader(s.Data, candidate);
                    TBinWriter key;
                    key.Str(identity);
                    key.Str(cache.Digest(argv.front()));
                    key.Str(ShellJoin(argv));
                    key.Str(ShellJoin(pchFlags));
                    key.Str(header);
                    key.Str(parentKey);
                    keys[g] = "pch-" + HashHex(key.Data());
                    const auto dir = cache.Directory(keys[g]);
                    artifacts[g] = JoinPath(dir, "output.pch");
                    auto command = argv;
                    command.insert(command.end(), pchFlags.begin(), pchFlags.end());
                    if (pch.Parent >= 0) { command.insert(command.end(), {"-include-pch", artifacts[pch.Parent]}); }
                    command.insert(command.end(), {"-x", "c++-header", JoinPath(dir, "input.h"), "-o", artifacts[g]});
                    return cache.Probe(keys[g], s.Baseline, command, header, Run, {},
                        pch.Parent < 0 ? std::vector<std::string>{} : std::vector<std::string>{artifacts[pch.Parent]});
                };
                auto roots = LayeredRoots(s.Data, pch);
                // The key of the last failed probe: the compatibility cache is
                // deleted after correction, so the failing compiler log has to
                // be retained separately from the (possibly successful) final
                // probe of the surviving root set.
                std::string failedKey;
                auto tryProbe = [&](const std::vector<int>& probeRoots) {
                    bool ok = probePch(probeRoots);
                    if (!ok) {
                        failedKey = keys[g];
                    }
                    return ok;
                };
                built[g] = tryProbe(roots);
                if (!built[g] && roots.size() > 1 && int(g) >= layout.Frozen) {
                    // A single incompatible root must not void the whole
                    // family: a rejected base resets its class's every delta
                    // to NONE. Find the smallest failing prefix of the layered
                    // include order, drop that root and retry the remainder,
                    // as the old TBisect search in checks.cpp did. Invariant:
                    // the prefix of length lo - 1 has already compiled, so lo
                    // is never reset — the budget is not spent re-proving it.
                    auto kept = roots;
                    size_t lo = 1;
                    int budget = int(4 + 4 * std::log2(double(roots.size())));
                    while (!built[g] && kept.size() > 1 && budget > 0) {
                        size_t hi = kept.size();
                        while (lo < hi && budget > 0) {
                            size_t mid = lo + (hi - lo) / 2;
                            --budget;
                            if (tryProbe(std::vector<int>(kept.begin(), kept.begin() + mid))) {
                                lo = mid + 1;
                            } else {
                                hi = mid;
                            }
                        }
                        if (lo != hi) { break; } // probe budget exhausted mid-search
                        reduced[g].push_back(kept[lo - 1]);
                        kept.erase(kept.begin() + (lo - 1));
                        --budget;
                        built[g] = tryProbe(kept);
                    }
                    if (!built[g] && lo > 1 && lo - 1 <= kept.size()) {
                        // However the search stopped — out of budget, be that
                        // mid-search or on a failed full retry — the prefix of
                        // lo - 1 roots has compiled: fall back to it instead of
                        // rejecting the PCH (and with a base, its whole class);
                        // an unprofitable survivor is evicted by the regular
                        // disk/payback pricing later.
                        for (size_t k = lo - 1; k < kept.size(); ++k) {
                            reduced[g].push_back(kept[k]);
                        }
                        kept.resize(lo - 1);
                        built[g] = tryProbe(kept);
                    }
                }
                // An initial base measured larger than the room the plan leaves
                // for it would take its whole family to NONE at the measured
                // fit, and a class without a base is lost to the correction
                // too, which builds none. Trim it to that room instead: its
                // roots in the order Pick accepted them, densest first, up to
                // the last whose artifact still fits, the thin tail dropped.
                // The include order is the header's own business. A search on
                // the real bytes finds that root in a few probes from a first
                // guess by the source bytes each root adds.
                //
                // The room is what is left after what the plan must keep, its
                // bases. A plan usually arrives with spare deltas for the probe
                // allowance and is over the budget by estimate; its bases still
                // have to fit, each after the others. A plan whose bases alone
                // are over the budget by estimate is left to the regular eviction.
                std::vector<int> tail;
                double measured = 0, room = 0;
                bool abandoned = false;
                if (built[g] && int(g) < layout.Bases && int(g) >= layout.Frozen && pch.Roots.size() - reduced[g].size() > 1) {
                    const double budget = s.Data.Resources.Disk;
                    double total = 0, mandatory = 0;
                    for (size_t h = 0; h < prediction.PchBytes.size(); ++h) {
                        total += prediction.PchBytes[h];
                        if (int(h) < layout.Bases) { mandatory += prediction.PchBytes[h]; }
                    }
                    const double own = prediction.PchBytes.at(g);
                    // Everything else counts against the room while the whole plan
                    // fits by estimate; after that only the other bases do.
                    const double others = total <= budget ? total - own : mandatory - own;
                    room = budget - others;
                    measured = double(FileSize(artifacts[g]));
                    if (mandatory <= budget && room > 0 && measured > room) {
                        auto kept = pch.Roots;
                        for (int r : reduced[g]) { kept.erase(std::find(kept.begin(), kept.end(), r)); }
                        const std::vector<int> untrimmed = kept;
                        // Source bytes each root adds beyond the ones before it.
                        std::vector<double> added;
                        std::vector<char> seen(s.Data.Units.size());
                        double source = 0;
                        for (int r : kept) {
                            double bytes = 0;
                            for (int u : UnitClosure(s.Data, {r})) {
                                if (!seen[u]) {
                                    seen[u] = 1;
                                    bytes += s.Data.Units[u].Bytes;
                                }
                            }
                            added.push_back(bytes);
                            source += bytes;
                        }
                        double bytes = 0;
                        size_t guess = 0;
                        while (guess + 1 < kept.size() && bytes + added[guess] <= room / measured * source) {
                            bytes += added[guess];
                            ++guess;
                        }
                        // The largest prefix that fits: none is known to fit, the
                        // whole set is known not to.
                        size_t lo = 0, hi = kept.size();
                        size_t k = std::clamp(guess, size_t(1), hi - 1);
                        auto fits = [&](const std::vector<int>& prefix) {
                            return probePch(prefix) && double(FileSize(artifacts[g])) <= room;
                        };
                        // Seven halvings tell the last fitting prefix of up to
                        // 128 roots apart where the guess by source bytes
                        // misses: a small header instantiating most of the
                        // artifact sits anywhere in the order. An exact guess
                        // is done in a probe or two.
                        for (int probes = 0; probes < 7 && hi - lo > 1 && lo < k && k < hi; ++probes) {
                            if (fits(std::vector<int>(kept.begin(), kept.begin() + k))) {
                                lo = k;
                            } else {
                                hi = k;
                            }
                            k = lo + (hi - lo) / 2;
                        }
                        // A base keeps at least its first root: none fitting the
                        // room is for the measured eviction to say.
                        tail.assign(kept.begin() + std::max<size_t>(1, lo), kept.end());
                        kept.resize(std::max<size_t>(1, lo));
                        if (!tail.empty()) {
                            // The probes moved the artifact: back to the kept set (cached).
                            if (probePch(kept)) {
                                reduced[g].insert(reduced[g].end(), tail.begin(), tail.end());
                            } else {
                                // A kept root compiled only with one of the dropped,
                                // under the flags of its class as the PCH builds:
                                // the fit of a prefix is no proof that it compiles.
                                // The trim is not for this base. Back to what did
                                // compile, cached too and no failed probe recorded:
                                // the measured fit decides, with its own reason.
                                tail.clear();
                                built[g] = probePch(untrimmed);
                                abandoned = true;
                            }
                        }
                    }
                }
                if (built[g]) {
                    const bool selfReduced = !reduced[g].empty();
                    const bool parentReduced = pch.Parent >= 0 && !reduced[pch.Parent].empty();
                    if (selfReduced || parentReduced) {
                        // Store the surviving contents: closure of the kept
                        // roots on top of the parent's current units, so the
                        // model prices exactly what the artifact serializes.
                        if (selfReduced) {
                            pch.Roots.erase(std::remove_if(pch.Roots.begin(), pch.Roots.end(), [&](int r) {
                                return std::find(reduced[g].begin(), reduced[g].end(), r) != reduced[g].end();
                            }), pch.Roots.end());
                        }
                        pch.Units = UnitClosure(s.Data, pch.Roots);
                        if (pch.Parent >= 0) {
                            const auto& inherited = layout.Pchs[pch.Parent].Units;
                            pch.Units.insert(pch.Units.end(), inherited.begin(), inherited.end());
                            Unique(pch.Units);
                        }
                    }
                    inputs[s.Data.Actions.size() + g] = probeInputs(keys[g]);
                }
                {
                    std::string note;
                    // Only what the bisection found: a tail dropped for the size
                    // of the artifact has its own line below.
                    if (reduced[g].size() > tail.size()) {
                        std::vector<std::string> dropped;
                        // The roots dropped for the size of the artifact stand
                        // after the ones the bisection found.
                        for (size_t i = 0; i + tail.size() < reduced[g].size(); ++i) {
                            dropped.push_back(s.Data.Units[reduced[g][i]].Include);
                        }
                        note = Format("bisected: dropped %zu root(s) (%s)", dropped.size(), Join(dropped, " ").c_str());
                        if (built[g] && !failedKey.empty()) {
                            // Why a root was dropped must stay explainable after
                            // the compatibility cache is cleaned up.
                            note += "; last failing probe: " + KeepProbeLog(s, JoinPath(cache.Directory(failedKey), "compiler.log"));
                        }
                    }
                    if (!tail.empty() || abandoned) {
                        std::vector<std::string> dropped;
                        for (int r : tail) { dropped.push_back(s.Data.Units[r].Include); }
                        if (!note.empty()) { note += "; "; }
                        note += Format("measured %.0f bytes where the plan leaves room for %.0f: ", measured, room);
                        note += abandoned ? "not trimmed, a kept root compiles only with a dropped one"
                                          : Format("dropped %zu tail root(s) (%s)", tail.size(), Join(dropped, " ").c_str());
                    }
                    // Report the last failing probe's log, not the key of the
                    // final (possibly successful) prefix probe.
                    const std::string& logKey = !built[g] && !failedKey.empty() ? failedKey : keys[g];
                    row << "pch\t" << g << "\t" << g << "\t" << (built[g] ? int(g) : -1)
                        << "\t" << (built[g] ? note : KeepProbeLog(s, JoinPath(cache.Directory(logKey), "compiler.log"))
                            + (note.empty() ? "" : "; " + note)) << '\n';
                }
                if (built[g] && int(g) >= layout.Bases) {
                    auto& link = linkProbes[g];
                    link = ProbeLeafLink(cache, s.Baseline, flags.at(pch.Representative), artifacts[g], keys[g]);
                    if (link.Ok) {
                        auto deps = probeInputs(link.Key);
                        auto& all = inputs[s.Data.Actions.size() + g];
                        all.insert(all.end(), deps.begin(), deps.end());
                        Unique(all);
                    }
                    row << "pch-link\t" << g << '\t' << g << '\t' << (link.Ok ? int(g) : -1)
                           << '\t' << (link.Ok ? "" : KeepProbeLog(s, JoinPath(cache.Directory(link.Key), "compiler.log"))) << '\n';
                }
                {
                    std::lock_guard<std::mutex> lock(reportMutex);
                    rows[g] = row.str();
                    report.str("");
                    report << reportHeader;
                    for (const auto& text : rows) { report << text; }
                    WriteFile(JoinPath(s.Directory, "compatibility.tsv"), report.str());
                    Progress(Format("compatibility PCH %zu/%zu: %s (group %zu), elapsed %s",
                                    ++completed, layout.Pchs.size(), built[g] ? "OK" : "rejected", g,
                                    Seconds((Now() - compatibilityStart) / 1e9).c_str()));
                }
                if (!built[g] && int(g) < layout.Frozen) {
                    Fail("frozen base failed compatibility checking; see " + JoinPath(cache.Directory(keys[g]), "compiler.log"));
                }
            });
            report.str("");
            report << reportHeader;
            for (const auto& text : rows) { report << text; }
            for (const auto& link : linkProbes) {
                if (link.Ok) { linkNeeds.insert(link.Needs.begin(), link.Needs.end()); }
            }
            TSymbolProviders providers;
            if (!linkNeeds.empty()) {
                std::vector<std::string> libraries;
                for (const auto& path : s.LinkInputs.Libraries) {
                    // A partial baseline can have final consumers not built yet.
                    // Missing artifacts cannot establish a provider; their links
                    // remain unproven and cannot silently opt in to a leaf.
                    if (Exists(JoinPath(s.Baseline, path))) { libraries.push_back(path); }
                }
                TWorkDir symbols(JoinPath(workDir, "link-symbols"), s.Ninja, std::max(1, s.Data.Resources.Workers));
                LoadLinkProviders(symbols, s.Baseline, libraries, linkNeeds, providers);
            }
            // Link availability is decided once per owner and leaf — for any
            // leaf the owner may end on: its own chain, but also the sibling
            // deltas a later disk eviction can hand it over to. Cached per
            // (owner, leaf) under a mutex: the members of one owner are
            // checked in parallel.
            std::vector<std::map<int, std::string>> linkErrors(s.Data.Targets.size());
            std::mutex linkMutex;
            auto linkError = [&](int t, int g) -> const std::string& {
                static const std::string none;
                if (g < layout.Bases || !built[g]) { return none; }
                std::lock_guard<std::mutex> lock(linkMutex);
                auto [it, added] = linkErrors[t].try_emplace(g);
                if (added) {
                    const auto& link = linkProbes[g];
                    it->second = link.Ok ? LinkDiagnostic(TargetLinkAvailability(s, t, link.Needs, providers))
                        : "PCH link probe failed: " + KeepProbeLog(s, JoinPath(cache.Directory(link.Key), "compiler.log"));
                }
                return it->second;
            };
            const auto original = layout.Assignment;
            std::vector<std::string> diagnostics(s.Data.Files.size());
            const auto initialHits = cache.Hits.load();
            auto checkFile = [&](int f, int g, bool install) {
                const std::string& error = linkError(s.Data.Files[f].Target, g);
                if (!error.empty()) {
                    diagnostics[f] += (diagnostics[f].empty() ? "" : ";") + error;
                    return false;
                }
                auto argv = flags[f];
                if (g >= 0) { argv.insert(argv.end(), {"-include-pch", artifacts[g]}); }
                argv.insert(argv.end(), {"-fsyntax-only", "-Wno-unused-command-line-argument", sources[f]});
                const auto key = "tu-" + HashHex(identity + cache.Digest(argv.front()) + ShellJoin(argv));
                const auto log = JoinPath(cache.Directory(key), "compiler.log");
                std::vector<std::string> inputsToCheck = {sources[f]};
                if (g >= 0) { inputsToCheck.push_back(artifacts[g]); }
                bool ok = cache.Probe(key, s.Baseline, argv, {}, Run, {}, std::move(inputsToCheck));
                if (ok && install) { inputs[s.Data.Files[f].Action] = probeInputs(key); }
                if (!ok) { diagnostics[f] += (diagnostics[f].empty() ? "" : ";") + KeepProbeLog(s, log); }
                if (g < 0 && !ok) { Fail("compatibility failed without PCH: " + s.Data.Files[f].Source + "; see " + log); }
                return ok;
            };
            CompatibleTargets(s.Data, layout, built, jobs, [&](int f, int g) {
                return checkFile(f, g, true);
            }, [&](const TCompatibilityProgress& p, bool boundary) {
                const auto line = Format("compatibility: targets %zu/%zu, files %zu/%zu, checks %zu (retries %zu), cache hits %zu, %zu workers, elapsed %s",
                    p.FinishedTargets, p.Targets, p.Checks - p.Retries, p.Files, p.Checks, p.Retries,
                    cache.Hits.load() - initialHits, jobs, Seconds((Now() - compatibilityStart) / 1e9).c_str());
                if (boundary) { Log(line); } else { Progress(line); }
            });
            const auto beforeBudget = layout.Assignment;
            std::vector<uint64_t> bytes(built.size());
            for (size_t g = 0; g < built.size(); ++g) {
                if (!built[g]) { continue; }
                auto n = FileSize(artifacts[g]);
                if (n <= 0) { Fail("missing compatibility artifact: " + artifacts[g]); }
                bytes[g] = n;
            }
            auto diskReport = FitMeasuredDisk(s.Data, layout, calibration, bytes, layout.Frozen, [&](int t, int parent) {
                const auto& files = s.Data.Targets[t].Files;
                std::vector<char> ok(files.size());
                ParallelFor(files.size(), jobs, [&](size_t i) { ok[i] = checkFile(files[i], parent, false); });
                return std::all_of(ok.begin(), ok.end(), [](char pass) { return pass; });
            });
            // Payback of what the checks and the fit left: an owner the fit
            // moved to a base that saves it nothing, a family whose paying
            // owners have left, is priced again on its final leaf. It steps
            // down to its parent or to NONE, never up to a leaf nobody
            // checked it with. A parent is a fallback only if it compiles the
            // owner: one that does not is no fallback at all, and the owner
            // keeps a delta that pays against NONE alone instead of losing it
            // on the way to a base it cannot use.
            const auto afterFit = layout.Assignment;
            const auto payback = FinalPayback(s.Data, layout, calibration, [&](int t, int parent) {
                const auto& files = s.Data.Targets[t].Files;
                std::vector<char> ok(files.size());
                ParallelFor(files.size(), jobs, [&](size_t i) { ok[i] = checkFile(files[i], parent, false); });
                return std::all_of(ok.begin(), ok.end(), [](char pass) { return pass; });
            });
            size_t stepped = 0;
            for (size_t t = 0; t < s.Data.Targets.size(); ++t) { stepped += layout.Assignment[t] != afterFit[t]; }
            if (stepped) {
                Log(Format("final payback: %zu owners step down to a parent or to NONE after the compatibility checks and the measured fit", stepped));
                WriteFile(JoinPath(s.Directory, "final-payback.tsv"),
                          "source\tpass\tkind\tcandidate_leaf\tassigned_targets\tparse_saved_s\tload_s\textra_compile_s\tbuild_s\tmandatory_build_s\tnet_s\tdecision\n" + payback);
            }
            // Replace the original leaf's generated-input record with that of the
            // final parent (or NONE). Never install a speculative fallback probe.
            ParallelFor(s.Data.Files.size(), jobs, [&](size_t f) {
                int t = s.Data.Files[f].Target, g = layout.Assignment[t];
                if (beforeBudget[t] != g && !checkFile(f, g, true)) {
                    Fail("final disk fallback failed: " + s.Data.Files[f].Source);
                }
            });
            size_t changed = 0;
            for (size_t t = 0; t < s.Data.Targets.size(); ++t) {
                if (original[t] < 0) { continue; }
                changed += original[t] != layout.Assignment[t];
                report << "target\t" << s.Data.Targets[t].Label << '\t' << original[t] << '\t' << layout.Assignment[t] << '\t';
                for (int f : s.Data.Targets[t].Files) {
                    if (!diagnostics[f].empty()) { report << s.Data.Files[f].Source << ':' << diagnostics[f] << ';'; }
                }
                report << '\n';
            }
            WriteFile(JoinPath(s.Directory, "compatibility.tsv"), report.str());
            const auto compatibleAssignments = layout.Assignment;
            // The probes the plan leaves were built and weighed like the
            // rest: the bases and deltas of exactly the classes it drops.
            // Their sizes are the artifacts' own and cost nothing more; the
            // next selection estimates those classes from them instead of the
            // prior. Nothing else of theirs was measured, and they are gone
            // with the unused chains below.
            {
                std::vector<char> kept(layout.Pchs.size(), 0);
                for (int g : layout.Assignment) {
                    for (; g >= 0 && !kept[g]; g = layout.Pchs[g].Parent) { kept[g] = 1; }
                }
                for (size_t g = 0; g < layout.Pchs.size(); ++g) {
                    if (!built[g] || kept[g]) { continue; }
                    TMeasuredPch probe;
                    probe.Pch = layout.Pchs[g];
                    probe.Parent = layout.Pchs[g].Parent >= 0 ? layout.Pchs[layout.Pchs[g].Parent] : TPch{};
                    probe.Bytes = double(bytes[g]);
                    s.ProbeSizes.push_back(std::move(probe));
                }
            }
            // Assignments are final: a frozen base without a single assigned
            // target or surviving delta leaves the plan here, releasing its
            // bytes and its slot in the exported groups.
            const auto remap = CompactLayout(layout);
            uint64_t resident = 0;
            for (size_t g = 0; g < remap.size(); ++g) {
                if (remap[g] < 0) { continue; }
                resident += bytes[g];
                if (retainedBytes) { retainedBytes->push_back(bytes[g]); }
            }
            if (resident > s.Data.Resources.Disk) { Fail("checked PCH plan exceeds disk budget"); }
            diskReport += "# retained_bytes=" + std::to_string(resident) + "\n";
            WriteFile(JoinPath(s.Directory, "disk-budget.tsv"), diskReport);
            Log(Format("checked PCH artifacts: %.3f / %.3f GB retained", resident / 1e9, s.Data.Resources.Disk / 1e9));
            Diagnostic([&] {
                std::ostringstream targets;
                targets << "target\treason\tselected_leaf\tcompatible_leaf_before_compaction\tfinal_leaf\tdiagnostics\n";
                for (size_t t = 0; t < s.Data.Targets.size(); ++t) {
                    // Reasons are read in the space of IDs before the compaction;
                    // the final leaf alone is in the space after it.
                    const std::string reason = compatibleAssignments[t] != afterFit[t] ?
                        (compatibleAssignments[t] < 0 ? "final-payback-none" : "final-payback-parent") : beforeBudget[t] != afterFit[t] ?
                        (afterFit[t] < 0 ? "disk-budget" : "disk-fallback") : original[t] == afterFit[t] ? "unchanged" :
                        afterFit[t] < 0 ? "compatibility-rejected" : "compatibility-fallback";
                    targets << s.Data.Targets[t].Label << '\t' << reason << '\t' << original[t] << '\t'
                            << compatibleAssignments[t] << '\t' << layout.Assignment[t] << '\t';
                    if (original[t] >= 0 && !built[original[t]]) { targets << "selected PCH or its parent did not compile; "; }
                    for (int f : s.Data.Targets[t].Files) {
                        if (!diagnostics[f].empty()) { targets << s.Data.Files[f].Source << ':' << diagnostics[f] << ';'; }
                    }
                    targets << '\n';
                }
                WriteFile(JoinPath(s.Directory, "compatibility-targets.tsv"), targets.str());
            });
            for (size_t g = 0; g < remap.size(); ++g) {
                if (remap[g] >= 0 && remap[g] != int(g)) {
                    inputs[s.Data.Actions.size() + remap[g]] = std::move(inputs[s.Data.Actions.size() + g]);
                }
            }
            inputs.resize(s.Data.Actions.size() + layout.Pchs.size());
            std::set<std::string> frozenInputs;
            for (const auto& [path, hash] : s.Inputs) { frozenInputs.insert(path); }
            for (const auto& actionInputs : inputs) {
                for (const auto& path : actionInputs) {
                    const auto absolute = NormPath(JoinPath(s.Baseline, path));
                    if (frozenInputs.insert(absolute).second) { s.Inputs.emplace_back(absolute, DigestFile(absolute)); }
                }
            }
            const auto hits = cache.Hits.load(), runs = cache.Runs.load();
            Log(Format("compatibility complete: %zu target assignments changed; %zu PCHs retained; %zu/%zu probe cache hits, "
                       "%zu compiler invocations; %s", changed, layout.Pchs.size(), hits, hits + runs, runs,
                       Seconds((Now() - compatibilityStart) / 1e9).c_str()));
            PublishReports(s);
            return inputs;
        }

        void PublishDiskBudgetReport(const TSnapshot& s) {
            // The normal successful workflow deletes .pch_update/current. Keep
            // the latest completed phase's diagnostics outside that scratch tree.
            WriteFile(JoinPath(s.Baseline, "pch-disk-budget.tsv"), ReadFile(JoinPath(s.Directory, "disk-budget.tsv")));
        }

        void Summary(TSnapshot& s) {
            PublishDiskBudgetReport(s);
            std::ostringstream out;
            out << std::setprecision(10)
                << "phase\t" << (s.Corrected ? "corrected-prediction" : "initial-prediction")
                << "\nmode\t" << s.Mode << "\nq\t" << s.Data.Q
                << "\nnonce\t" << s.Nonce << "\ncold_seconds\t" << s.Prediction.Cold
                << "\nincremental_seconds\t" << s.Prediction.Incremental << "\nobjective_seconds\t" << s.Prediction.Objective
                << "\nbases\t" << s.Layout.Bases << "\npchs\t" << s.Layout.Pchs.size()
                << "\ntargets\t" << s.Data.Targets.size()
                << "\nassigned_targets\t" << std::count_if(s.Layout.Assignment.begin(), s.Layout.Assignment.end(), [](int g) { return g >= 0; })
                << "\ncompilations\t" << s.Data.Files.size()
                << "\nworkers\t" << s.Data.Resources.Workers << "\nram_bytes\t" << s.Data.Resources.Ram
                << "\npch_disk_bytes_budget\t" << s.Data.Resources.Disk
                << "\ndisk_budget_report\t" << JoinPath(s.Baseline, "pch-disk-budget.tsv")
                << "\nsample_percent\t" << (s.Commands.empty() ? "pending" : std::to_string(s.Sample)) << "\n";
            if (!s.Commands.empty() && s.Sample < 100) {
                out << "# Verification samples C++ objects: " << s.Measured.size()
                    << " calibration actions; prerequisite commands are reported separately in sampling.tsv. "
                       "cold_seconds is a prediction, not a measured build.\n";
            }
            out << "# Initial size/RSS/load costs are priors. restat outputs are conservatively assumed changed.\n"
                   "# History is a sampled workload, not an estimate of arbitrary future commits.\n"
                   "# The corrected layout remains a prediction until separately verified.\n";
            WriteFile(JoinPath(s.Directory, "summary.tsv"), out.str());
            std::ostringstream ledger;
            ledger << "id\toutput\tseconds\tRSS_bytes\tIO_bytes\tcommand_digest\n" << std::setprecision(10);
            for (size_t i = 0; i < s.Prediction.Actions.size(); ++i) {
                const auto& a = s.Prediction.Actions[i];
                ledger << i << '\t' << a.Output << '\t' << a.Time << '\t' << a.Memory << '\t' << a.Io << '\t'
                       << (s.Commands.empty() ? "" : s.Commands[i]) << '\n';
            }
            WriteFile(JoinPath(s.Directory, "predictions.tsv"), ledger.str());
            std::ostringstream assignments;
            assignments << "target\tclass\tleaf\tcompilations\tpredicted_compile_seconds\tconstraint\tselection_reason\tcompatibility\n";
            std::ostringstream members;
            members << "target\tsource\toutput\tclass\n";
            const auto selectionReasons = TargetReasons(JoinPath(s.Directory, "selection-targets.tsv"));
            const auto compatibilityReasons = TargetReasons(JoinPath(s.Directory, "compatibility-targets.tsv"));
            std::map<std::string, size_t> reasons;
            size_t assignedTargets = 0, assignedFiles = 0;
            double coveredWork = 0, allWork = 0;
            for (size_t t = 0; t < s.Data.Targets.size(); ++t) {
                const auto& target = s.Data.Targets[t];
                double work = 0;
                for (int f : target.Files) {
                    const auto& file = s.Data.Files[f];
                    work += s.Prediction.Actions[file.Action].Time;
                    members << target.Label << '\t' << file.Source << '\t' << s.Data.Actions[file.Action].Output << '\t' << file.Class << '\n';
                    allWork += s.Data.Actions[file.Action].Time;
                    if (s.Layout.Assignment[t] >= 0) { coveredWork += s.Data.Actions[file.Action].Time; }
                }
                const auto selection = selectionReasons.find(target.Label), compatibility = compatibilityReasons.find(target.Label);
                const std::string selectedReason = selection == selectionReasons.end() ? "unavailable-in-checkpoint" : selection->second;
                const std::string checkedReason = compatibility == compatibilityReasons.end() ? "unavailable-in-checkpoint" : compatibility->second;
                const bool assigned = s.Layout.Assignment[t] >= 0;
                assignedTargets += assigned;
                if (assigned) { assignedFiles += target.Files.size(); }
                ++reasons[checkedReason != "unchanged" && checkedReason != "unavailable-in-checkpoint" ? checkedReason :
                    assigned ? "assigned" : !target.PchEligible ? "no-global-pch-hook" : target.Class < 0 ? "mixed-compiler-classes" : selectedReason];
                assignments << target.Label << '\t' << target.Class << '\t' << s.Layout.Assignment[t] << '\t'
                            << target.Files.size() << '\t' << work << '\t'
                            << (!target.PchEligible ? "no-global-pch-hook" : target.Class < 0 ? "mixed-compiler-classes" : "")
                            << '\t' << selectedReason << '\t' << checkedReason << '\n';
            }
            WriteFile(JoinPath(s.Directory, "assignments.tsv"), assignments.str());
            WriteFile(JoinPath(s.Directory, "target-members.tsv"), members.str());
            double pchBytes = 0;
            for (double bytes : s.Prediction.PchBytes) { pchBytes += bytes; }
            std::ostringstream human;
            human << (s.Corrected ? "Corrected" : "Initial") << " plan — prediction\n"
                  << Format("  PCHs: %zu (%d bases + %zu deltas); predicted disk %.3f / %.3f GB\n",
                      s.Layout.Pchs.size(), s.Layout.Bases, s.Layout.Pchs.size() - s.Layout.Bases, pchBytes / 1e9, s.Data.Resources.Disk / 1e9)
                  << Format("  Coverage: %zu/%zu GN compile targets (%.1f%%), %zu/%zu C++ files (%.1f%%)\n",
                      assignedTargets, s.Data.Targets.size(), 100.0 * assignedTargets / std::max(size_t(1), s.Data.Targets.size()),
                      assignedFiles, s.Data.Files.size(), 100.0 * assignedFiles / std::max(size_t(1), s.Data.Files.size()))
                  << Format("  Covered baseline compile work: %.1f%% (sum of action times, not wall time)\n", 100 * coveredWork / std::max(1e-9, allWork))
                  << "  Predicted cold: " << Seconds(s.Prediction.Cold) << "; incremental: " << Seconds(s.Prediction.Incremental) << '\n'
                  << "  Objective: " << Seconds(s.Prediction.Objective)
                  << Format(" (%.1f%% cold + %.1f%% incremental)\n", 100 * s.Data.Q, 100 * (1 - s.Data.Q))
                  << Format("  Build model: -j %d; the PCHs themselves build at most %zu at once\n",
                            s.Data.Resources.Workers, PchBuildJobs(s.Data.Resources.Workers))
                  << "  Target decisions:";
            for (const auto& [reason, count] : reasons) { human << ' ' << reason << '=' << count; }
            human << "\n  Each assignment covers one GN compile target; generated helper targets are evaluated separately.\n"
                  << "  Disk budget covers resident PCHs, not the whole build directory. GB = 1,000,000,000 bytes.\n"
                  << "  Predictions model the sampled history; a full-build speedup has not been measured.\n";
            if (s.RetainedInitial) {
                human << "  Retained initial plan: no corrected candidate scored better after the compatibility checks and the measured fit.\n";
            }
            human << (s.Corrected ? "  Calibrated where identifiable; other parameters retain priors (see calibration.tsv).\n" :
                "  Initial PCH size, RSS and load costs are priors; measurements follow.\n");
            Diagnostic([&] {
                WriteFile(JoinPath(s.Directory, "summary.txt"), human.str());
                std::ostringstream json;
                json << std::setprecision(17) << "{\"schema\":1,\"kind\":\"prediction\",\"phase\":" << JsonText(PhaseName(s))
                     << ",\"nonce\":" << JsonText(s.Nonce) << ",\"source_tree\":" << JsonText(s.Root)
                     << ",\"tree_identity\":" << JsonText(s.Tree) << ",\"tool_identity\":" << JsonText(s.Tool)
                     << ",\"cold_seconds\":" << s.Prediction.Cold << ",\"incremental_seconds\":" << s.Prediction.Incremental
                     << ",\"objective_seconds\":" << s.Prediction.Objective << ",\"q\":" << s.Data.Q
                     << ",\"pchs\":" << s.Layout.Pchs.size() << ",\"bases\":" << s.Layout.Bases
                     << ",\"assigned_targets\":" << assignedTargets << ",\"targets\":" << s.Data.Targets.size()
                     << ",\"assigned_cpp_files\":" << assignedFiles << ",\"cpp_files\":" << s.Data.Files.size()
                     << ",\"predicted_pch_bytes\":" << pchBytes << ",\"pch_budget_bytes\":" << s.Data.Resources.Disk
                     << ",\"sample_percent\":" << (s.Commands.empty() ? "null" : std::to_string(s.Sample)) << ",\"measured_speedup\":null}\n";
                WriteFile(JoinPath(s.Directory, "summary.json"), json.str());
            });
            Report(human.str());
            Detail(out.str());
            PublishReports(s);
        }

        TSnapshot LoadPlan(const std::string& path) {
            auto s = Decode<TSnapshot>(ReadFile(JoinPath(path, "snapshot.bin")));
            ValidateTargets(s.Data);
            if (s.Layout.Assignment.size() != s.Data.Targets.size()) { Fail("plan target assignment size mismatch"); }
            (void)ModeQ(s.Mode);
            if (RealPath(s.Directory) != RealPath(path)) { Fail("a plan cannot be relocated without preserving its frozen absolute paths"); }
            return s;
        }

        TLayout SelectCheckpoint(TSnapshot& s, const std::string& output,
                                 TCalibration calibration = {}, const TLayout* frozen = nullptr,
                                 const std::function<TLayout()>& compute = {}, size_t analysisJobs = 1) {
            // Nonces and report/build paths are not selector inputs. In
            // particular LoadAnalysis generates a new nonce on resume.
            bool hasFrozen = frozen != nullptr;
            TLayout seed = frozen ? *frozen : TLayout{};
            TSave inputs;
            inputs(s.Tool, s.Data, calibration, hasFrozen, seed);
            const auto key = HashHex(inputs.Out.Data());
            const auto path = JoinPath(output, "selection.bin");
            const auto report = JoinPath(output, "disk-budget.tsv");
            if (Exists(path)) {
                std::optional<TSelectionCheckpoint> saved;
                try { saved = Decode<TSelectionCheckpoint>(ReadFile(path)); }
                catch (const TError&) { Log("selection checkpoint has no usable diagnostics; recomputing layout"); }
                if (saved && saved->Key == key) {
                    Log("selection: reusing saved layout from " + path);
                    WriteFileIfChanged(report, saved->DiskReport);
                    std::string diagnosticKey;
                    TryReadFile(JoinPath(output, "selection-diagnostics.key"), &diagnosticKey);
                    if (diagnosticKey != key) {
                        Log("selection reasons unavailable in this checkpoint; keeping the saved layout");
                        Diagnostic([&] {
                            for (const char* name : {"selection-targets.tsv", "selection-choices.tsv", "selection-widths.tsv", "selection-candidates.tsv", "selection-payback.tsv"}) {
                                WriteFile(JoinPath(output, name), "# unavailable for this checkpoint\n");
                            }
                        });
                    }
                    return std::move(saved->Layout);
                }
                if (saved) { Log("selection inputs changed; recomputing layout"); }
            }
            TSelectionDiagnostics diagnostics;
            auto layout = compute ? compute() : Select(s.Data, calibration, frozen, analysisJobs, &diagnostics);
            TSelectionCheckpoint saved{key, layout, Format("# phase=%s pending compatibility; budget_bytes=%.0f\n", frozen ? "corrected" : "initial", s.Data.Resources.Disk)};
            // Publish before compatibility/graph emission, either of which can
            // be interrupted long after the expensive selector has finished.
            WriteFile(path, Encode(saved));
            WriteFileIfChanged(report, saved.DiskReport);
            Diagnostic([&] {
                WriteFile(JoinPath(output, "selection-targets.tsv"), diagnostics.Targets);
                WriteFile(JoinPath(output, "selection-choices.tsv"), diagnostics.Choices);
                WriteFile(JoinPath(output, "selection-widths.tsv"), diagnostics.Widths);
                WriteFile(JoinPath(output, "selection-candidates.tsv"), diagnostics.Candidates);
                WriteFile(JoinPath(output, "selection-payback.tsv"), diagnostics.Payback);
                WriteFile(JoinPath(output, "selection-diagnostics.key"), key);
            });
            return layout;
        }

        int CreatePlan(TSnapshot& s, const std::string& output, const std::string& workDir, size_t analysisJobs = 1) {
            ValidateIdentity(s);
            CheckProfileOutput(s.Baseline, s.Mode, output);
            if (!Exists(output)) { NewDirectory(output, s.Root, s.Baseline); }
            s.Directory = RealPath(output);
            // Durable boundary before selection and GN/Ninja emission: even a
            // failed plan can be replanned without repeating expensive analysis.
            SaveAnalysis(s);
            // Resolved defaults are now durable, including on a retry after a
            // settings change. Never re-resolve free RAM after selection starts.
            RemoveFile(JoinPath(output, "settings.changed"));
            // Only a plan inside the baseline state directory becomes the
            // default analysis checkpoint. A diagnostic --output elsewhere may
            // be temporary, and pointing the baseline at it loses the analysis.
            if (Under(s.Directory, StateDirectory(s.Baseline))) {
                WriteFile(JoinPath(StateDirectory(s.Baseline), "analysis.path"), s.Directory + "\n");
            }
            s.Layout = SelectCheckpoint(s, output, {}, nullptr, {}, analysisJobs);
            std::vector<uint64_t> bytes;
            auto probeInputs = Preflight(s, workDir, {}, analysisJobs, &bytes);
            s.Prediction = Predict(s.Data, s.Layout, {}, analysisJobs, bytes);
            const auto probes = JoinPath(output, "measurement-inputs.bin");
            WriteFile(probes, Encode(probeInputs));
            s.Inputs.emplace_back(probes, DigestFile(probes));
            ValidateIdentity(s);
            Summary(s);
            WriteFile(JoinPath(output, "snapshot.bin"), Encode(s));
            Detail("prediction snapshot: " + output);
            return 0;
        }

        void PrepareMeasurement(TSnapshot& s, int sample) {
            if (!s.Commands.empty()) {
                if (s.Sample != sample) { Fail("measurement sample is frozen; resume with --sample " + std::to_string(s.Sample)); }
                return;
            }
            ValidateIdentity(s);
            s.Sample = sample;
            Emit(s, Decode<TCompileInputs>(ReadFile(JoinPath(s.Directory, "measurement-inputs.bin"))),
                 SampleFiles(s.Data, s.Nonce, sample, s.Layout.Controls));
            ValidateIdentity(s);
            Summary(s);
            WriteFile(JoinPath(s.Directory, "snapshot.bin"), Encode(s));
        }

        int Verify(TSnapshot& s) {
            TCycleLock verificationLock;
            // Uncached measurement children retain the lock if their controller
            // is killed, so a retry cannot delete outputs still being written.
            verificationLock.Acquire(JoinPath(s.Directory, "verification.lock"));
            if (BuildRunning(JoinPath(s.Directory, "build"))) { Fail("measurement Ninja is still running"); }
            if (s.Commands.empty()) { PrepareMeasurement(s, s.Sample); }
            ValidateIdentity(s);
            for (size_t i = 0; i < s.Commands.size(); ++i) {
                if (!s.Prediction.Actions[i].Phony &&
                    HashHex(ReadFile(JoinPath(s.Directory, "actions/" + std::to_string(i) + ".bin"))) != s.Commands[i]) {
                    Fail("action configuration changed since prediction");
                }
            }
            TRunRecord run;
            const auto recordPath = JoinPath(s.Directory, "run.bin");
            const auto snapshot = DigestFile(JoinPath(s.Directory, "snapshot.bin"));
            if (Exists(recordPath)) {
                run = Decode<TRunRecord>(ReadFile(recordPath));
                if (run.Nonce != s.Nonce || run.Snapshot != snapshot) { Fail("verification belongs to another plan"); }
            }
            const bool retry = Exists(recordPath);
            bool complete = retry && run.Status == 0 && run.End > run.Start;
            const auto measured = MeasuredActions(s.Prediction.Actions.size(), s.Measured);
            const auto now = Now(); // Ninja and orphaned measure processes no longer hold the lock.
            size_t reusable = 0;
            for (size_t i = 0; i < s.Commands.size(); ++i) {
                if (s.Prediction.Actions[i].Phony) { continue; }
                const auto observation = JoinPath(s.Directory, "observations/" + std::to_string(i) + ".bin");
                if (!measured[i] && !Exists(observation)) { continue; }
                const auto config = Decode<TMeasureConfig>(ReadFile(JoinPath(s.Directory, "actions/" + std::to_string(i) + ".bin")));
                bool valid = false;
                if (retry && Exists(observation)) { try {
                    auto r = Decode<TRecord>(ReadFile(observation));
                    valid = r.Nonce == s.Nonce && r.Id == int(i) && r.Status == 0 &&
                        r.Digest == s.Commands[i] && run.Contains(r.Observation, now) &&
                        r.Outputs.size() == config.Outputs.size();
                    for (size_t o = 0; valid && o < config.Outputs.size(); ++o) {
                        auto path = JoinPath(JoinPath(s.Directory, "build"), config.Outputs[o]);
                        valid = r.Outputs[o].first >= 0 && FileSize(path) == r.Outputs[o].first && MtimeNs(path) == r.Outputs[o].second;
                    }
                } catch (const TError&) { valid = false; } }
                if (valid) {
                    reusable += measured[i];
                    continue;
                }
                complete = false;
                RemoveFile(observation);
                // Let Ninja propagate dirtiness to dependents of this edge.
                for (const auto& output : config.Outputs) {
                    RemoveFile(JoinPath(JoinPath(s.Directory, "build"), output));
                }
            }
            if (complete) {
                Log(PhaseName(s) + " measurement: reusing completed, validated observations");
                return 0;
            }
            Log(PhaseName(s) + " measurement: " + (retry ?
                Format("resuming; %zu/%zu sampled observations intact before Ninja dependency checks", reusable, s.Measured.size()) :
                "starting the first attempt; compiler actions run uncached"));
            if (retry) { run.Earlier.emplace(run.Token, std::make_pair(run.Start, run.End ? run.End : now)); }
            run.Nonce = s.Nonce;
            run.Token = HashHex(s.Nonce + std::to_string(Now()) + std::to_string(getpid()));
            run.Snapshot = snapshot;
            run.Start = Now();
            run.Status = -1;
            run.End = 0;
            WriteFile(recordPath, Encode(run));
            MakeDirs(JoinPath(s.Directory, "observations"));
            TRunOptions options;
            options.CaptureStdout = options.CaptureStderr = false;
            options.OutputLabel = PhaseName(s) + "-measurement";
            {
                // Only measurement Ninja and its workers may outlive this
                // controller while still using the frozen verification plan.
                TCycleLock inherited;
                inherited.Fd = dup(verificationLock.Fd); // dup clears FD_CLOEXEC
                if (inherited.Fd < 0) { Fail("cannot pass verification lock to Ninja"); }
                options.Env = {"PCH_UPDATE_RUN=" + run.Token, "PCH_UPDATE_VERIFY_LOCK_FD=" + std::to_string(inherited.Fd)};
                run.Status = Run({s.Ninja, "-C", JoinPath(s.Directory, "build"), "-f", "measured.ninja", "-j",
                                  std::to_string(s.Data.Resources.Workers), "_pch_verify"}, options).ExitCode;
            }
            run.End = Now();
            WriteFile(recordPath, Encode(run));
            ValidateIdentity(s);
            Report("  " + PhaseName(s) + " measurement " + (run.Status ? "failed" : "completed") + ": " +
                   Seconds((run.End - run.Start) / 1e9) + " wall time (this attempt)");
            if (run.Status) { Log("retry the same command to resume the measurement"); }
            return run.Status;
        }

        std::vector<TObservation> ReadObservations(const TSnapshot& s, std::string& report, std::string& actionReport) {
            auto run = Decode<TRunRecord>(ReadFile(JoinPath(s.Directory, "run.bin")));
            if (run.Nonce != s.Nonce || run.Status != 0 || run.End <= run.Start ||
                run.Snapshot != DigestFile(JoinPath(s.Directory, "snapshot.bin"))) {
                Fail("verification was not completed successfully against this snapshot");
            }
            std::vector<TObservation> result(s.Prediction.Actions.size());
            std::vector<char> compiler(result.size());
            for (const auto& file : s.Data.Files) { compiler[file.Action] = true; }
            for (size_t i = s.Data.Actions.size(); i < result.size(); ++i) { compiler[i] = true; }
            // The explicit list also disambiguates a small sample selecting no
            // targets/PCHs from a full verification. An empty list means none.
            const auto measured = MeasuredActions(result.size(), s.Measured);
            size_t observed = 0;
            std::ostringstream errors;
            errors << "id\toutput\tpredicted_seconds\tactual_seconds\terror_seconds\tCPU_seconds\tRSS_bytes\tartifact_bytes\n";
            double absolute = 0, total = 0;
            for (size_t i = 0; i < result.size(); ++i) {
                const auto& a = s.Prediction.Actions[i];
                if (a.Phony || !measured[i]) { continue; }
                ++observed;
                std::string configBytes = ReadFile(JoinPath(s.Directory, "actions/" + std::to_string(i) + ".bin"));
                auto config = Decode<TMeasureConfig>(configBytes);
                auto r = Decode<TRecord>(ReadFile(JoinPath(s.Directory, "observations/" + std::to_string(i) + ".bin")));
                if (r.Nonce != s.Nonce || r.Id != int(i) || r.Status || r.Digest != s.Commands[i] ||
                    HashHex(configBytes) != s.Commands[i] || !run.Contains(r.Observation, run.End) ||
                    r.Outputs.size() != config.Outputs.size()) {
                    Fail("stale, failed or mixed-run observation: " + a.Output);
                }
                if (compiler[i] && config.CacheMode != "direct-uncached") { Fail("cached observation cannot calibrate a compiler action"); }
                for (size_t o = 0; o < r.Outputs.size(); ++o) {
                    auto path = JoinPath(JoinPath(s.Directory, "build"), config.Outputs[o]);
                    if (FileSize(path) != r.Outputs[o].first || MtimeNs(path) != r.Outputs[o].second) {
                        Fail("verification output changed: " + path);
                    }
                }
                result[i] = r.Observation;
                double error = r.Observation.Time - a.Time;
                absolute += std::abs(error);
                total += r.Observation.Time;
                errors << i << '\t' << a.Output << '\t' << a.Time << '\t' << r.Observation.Time << '\t' << error
                       << '\t' << r.Observation.Cpu << '\t' << r.Observation.Memory << '\t' << r.Observation.Bytes << '\n';
            }
            errors << "# initial_action_WAPE\t" << absolute / std::max(1e-9, total) << '\n';
            actionReport = errors.str();
            errors.str("");
            errors.clear();
            errors << "target\tcompilations\tpredicted_compile_seconds\tactual_compile_seconds\terror_seconds\n";
            absolute = total = 0;
            size_t measuredTargets = 0;
            for (const auto& target : s.Data.Targets) {
                double predicted = 0, actual = 0;
                bool complete = true;
                for (int f : target.Files) {
                    int a = s.Data.Files[f].Action;
                    predicted += s.Prediction.Actions[a].Time;
                    actual += result[a].Time;
                    complete &= result[a].Time > 0;
                }
                if (!complete) { continue; }
                ++measuredTargets;
                errors << target.Label << '\t' << target.Files.size() << '\t' << predicted << '\t' << actual << '\t' << actual - predicted << '\n';
                absolute += std::abs(actual - predicted);
                total += actual;
            }
            errors << "# initial_target_WAPE\t" << absolute / std::max(1e-9, total)
                   << "\n# measured_targets\t" << measuredTargets
                   << "\n# predicted_cold_seconds\t" << s.Prediction.Cold
                   << "\n# measured_build_wall_seconds\t" << (run.Earlier.empty() ? Format("%.9g", (run.End - run.Start) / 1e9) : "unavailable")
                   << "\n# sampled_percent\t" << s.Sample
                   << "\n# measured_actions\t" << observed
                   << "\n# unmeasured_actions\t" << (s.Prediction.Actions.size() - observed)
                   << "\n# Wall includes Ninja, process startup, admission and telemetry overhead.\n";
            if (s.Sample < 100) {
                errors << "# A sampled run measures action times, not the cold build: predicted_cold_seconds is\n"
                          "# a model estimate and the wall above covers only the sampled targets.\n";
            }
            report = errors.str();
            return result;
        }

        void MeasurementSummary(const TSnapshot& s, const std::vector<TObservation>& observations,
                                const std::string& targetErrors, const std::string& actionErrors) {
            Diagnostic([&] {
                const auto run = Decode<TRunRecord>(ReadFile(JoinPath(s.Directory, "run.bin")));
                size_t actions = 0, targets = 0, files = 0;
                double cpu = 0, work = 0, predicted = 0, pchBytes = 0, maxRss = 0, error = 0, targetWork = 0;
                for (size_t i = 0; i < observations.size(); ++i) {
                    const auto& o = observations[i];
                    if (o.Time <= 0) { continue; }
                    ++actions;
                    work += o.Time; cpu += o.Cpu; predicted += s.Prediction.Actions[i].Time;
                    maxRss = std::max(maxRss, o.Memory);
                    if (i >= s.Data.Actions.size()) { pchBytes += o.Bytes; }
                }
                for (const auto& target : s.Data.Targets) {
                    bool complete = true;
                    double actual = 0, estimate = 0;
                    for (int f : target.Files) {
                        const int a = s.Data.Files[f].Action;
                        files += observations[a].Time > 0;
                        complete &= observations[a].Time > 0;
                        actual += observations[a].Time;
                        estimate += s.Prediction.Actions[a].Time;
                    }
                    if (!complete) { continue; }
                    ++targets;
                    error += std::abs(estimate - actual); targetWork += actual;
                }
                const double wall = (run.End - run.Start) / 1e9;
                size_t reused = 0;
                for (const auto& o : observations) { reused += o.Time > 0 && o.Invocation != run.Token; }
                const auto wallValue = Format("%.17g", wall);
                const std::string wape = targetWork > 0 ? Format("%.1f%%", 100 * error / targetWork) : "unavailable";
                std::ostringstream human;
                human << (s.Corrected ? "Corrected" : "Initial") << " sample — validated measurements\n"
                      << "  Wall (sample + prerequisites): " << (run.Earlier.empty() ? Seconds(wall) : "unavailable (resumed)") << '\n'
                      << Format("  Calibration observations: %zu reused, %zu new\n", reused, actions - reused)
                      << Format("  Observed calibration actions: %zu; complete targets: %zu; C++ files: %zu\n", actions, targets, files)
                      << "  Calibration action work: " << Seconds(work) << " measured / " << Seconds(predicted) << " predicted\n"
                      << "  Calibration CPU sum: " << Seconds(cpu) << "; target compile-work WAPE: " << wape << '\n'
                      << Format("  PCH artifacts: %.3f / %.3f GB; largest observed action RSS: %.2f GB\n", pchBytes / 1e9,
                          s.Data.Resources.Disk / 1e9, maxRss / 1e9)
                      << "  Work/CPU sums are not build wall time; largest action RSS is not peak total RAM.\n";
                if (s.Sample < 100) { human << "  Full cold-build time remains a prediction. Initial/corrected samples differ.\n"; }
                WriteFile(JoinPath(s.Directory, "measurement.txt"), human.str());
                auto normalizedTargets = targetErrors, normalizedActions = actionErrors;
                Replace(normalizedTargets, "# initial_target_WAPE", "# target_WAPE");
                Replace(normalizedActions, "# initial_action_WAPE", "# action_WAPE");
                WriteFile(JoinPath(s.Directory, "target-errors.tsv"), normalizedTargets);
                WriteFile(JoinPath(s.Directory, "action-errors.tsv"), normalizedActions);
                std::ostringstream values;
                values << std::setprecision(17) << "wall_seconds\t" << (run.Earlier.empty() ? wallValue : "unavailable")
                       << "\nreused_actions\t" << reused << "\nnew_actions\t" << actions - reused << "\nactions\t" << actions
                       << "\ncomplete_targets\t" << targets << "\ncpp_files\t" << files << "\naction_work_seconds\t" << work
                       << "\npredicted_action_work_seconds\t" << predicted << "\ncpu_seconds\t" << cpu
                       << "\npch_bytes\t" << pchBytes << "\nmax_action_rss_bytes\t" << maxRss << "\ntarget_WAPE\t"
                       << (targetWork > 0 ? Format("%.17g", error / targetWork) : "unavailable") << '\n';
                WriteFile(JoinPath(s.Directory, "measurement.tsv"), values.str());
                std::ostringstream json;
                json << std::setprecision(17) << "{\"schema\":1,\"phase\":" << JsonText(PhaseName(s))
                     << ",\"kind\":\"validated-sample\",\"sample_percent\":" << s.Sample
                     << ",\"wall_seconds\":" << (run.Earlier.empty() ? wallValue : "null")
                     << ",\"reused_actions\":" << reused << ",\"new_actions\":" << actions - reused
                     << ",\"actions\":" << actions << ",\"complete_targets\":" << targets
                     << ",\"cpp_files\":" << files << ",\"action_work_seconds\":" << work << ",\"cpu_seconds\":" << cpu
                     << ",\"pch_bytes\":" << pchBytes << ",\"max_action_rss_bytes\":" << maxRss
                     << ",\"target_wape\":" << (targetWork > 0 ? Format("%.17g", error / targetWork) : "null") << "}\n";
                WriteFile(JoinPath(s.Directory, "measurement.json"), json.str());
                Report(human.str());
                PublishReports(s);
            });
        }

        void CalibrationSummary(const TSnapshot& s, const TCalibration& calibration) {
            Diagnostic([&] {
                size_t identified = 0, combined = 0, priors = 0, training = 0, held = 0;
                double error = 0;
                for (auto line : Split(calibration.Report, '\n')) {
                    const auto row = Split(line, '\t');
                    if (row.size() < 5 || row[0] == "class" || StartsWith(row[0], "#")) { continue; }
                    identified += row[1] == "identified"; combined += row[1] == "combined-only"; priors += row[1] == "prior-only";
                    training += std::stoull(std::string(row[2]));
                    const auto n = std::stoull(std::string(row[3]));
                    held += n;
                    if (n && row[4] != "unavailable") { error += n * std::stod(std::string(row[4])); }
                }
                const auto text = Format("Calibration: %zu compiler classes identified, %zu combined-only, %zu prior-only\n"
                    "  Training targets: %zu (all measured); cross-validated targets: %zu; leave-one-group-out target-work MAE: %s\n"
                    "  Combined-only fits net savings; prior-only cannot identify consumer costs. Details: calibration.tsv\n",
                    identified, combined, priors, training, held, held ? Seconds(error / held).c_str() : "unavailable");
                std::string summary = text;
                if (!calibration.Probes.empty()) {
                    double bytes = 0;
                    for (const auto& probe : calibration.Probes) { bytes += probe.Bytes; }
                    summary += Format("  Plus %zu PCHs the plan built, weighed and left (%.2f GB): their sizes, without build times, are in the class, unit and global scales.\n",
                        calibration.Probes.size(), bytes / 1e9);
                }
                WriteFile(JoinPath(s.Directory, "calibration.txt"), summary);
                std::ostringstream parameters;
                parameters << "class\tidentified\tbaseline_scale\tsaved_scale\tload_scale\tresidual_seconds\tinstantiate_scale\tbase_build_scale\tdelta_build_scale"
                           << "\tsize_scale\tpch_memory_scale\tconsumer_memory_scale\n" << std::setprecision(17);
                for (size_t cls = 0; cls < calibration.Classes.size(); ++cls) {
                    const auto& c = calibration.Classes[cls];
                    parameters << cls << '\t' << c.Identified << '\t' << c.Baseline << '\t' << c.Saved << '\t' << c.Load << '\t'
                               << c.Residual << '\t' << c.Instantiate << '\t' << c.BaseBuild << '\t' << c.DeltaBuild << '\t' << c.Size << '\t'
                               << c.Memory << '\t' << c.ConsumerMemory << '\n';
                }
                WriteFile(JoinPath(s.Directory, "calibration-parameters.tsv"), parameters.str());
                Report(summary);
            });
        }

        // The sizes a prediction recorded for the artifacts of a layout, when
        // the prediction is that layout's own: none otherwise.
        std::vector<uint64_t> RecordedSizes(const TPrediction& prediction, const TLayout& layout) {
            std::vector<uint64_t> bytes;
            if (prediction.PchBytes.size() != layout.Pchs.size()) { return bytes; }
            for (double size : prediction.PchBytes) { bytes.push_back(uint64_t(size)); }
            return bytes;
        }

        int CorrectPlan(TSnapshot& s, const std::string& plan, const std::string& output, const std::string& workDir,
                        size_t analysisJobs = 1) {
            if (s.Corrected) { Fail("correction is one step; start a new predict cycle to change the base"); }
            CheckProfileOutput(s.Baseline, s.Mode, output);
            ValidateIdentity(s);
            std::string errors, actionErrors;
            auto observations = ReadObservations(s, errors, actionErrors);
            MeasurementSummary(s, observations, errors, actionErrors);
            auto calibration = Calibrate(s.Data, s.Layout, s.Prediction, observations, s.ProbeSizes);
            const TLayout measuredInitial = s.Layout;
            // Taken now: the prediction at hand is the initial plan's own, with
            // the measured sizes of its artifacts. A moment later it is the
            // corrected plan's.
            const std::vector<uint64_t> initialBytes = RecordedSizes(s.Prediction, measuredInitial);
            if (!Exists(output)) { NewDirectory(output, s.Root, s.Baseline); }
            auto layout = SelectCheckpoint(s, output, calibration, &s.Layout, {}, analysisJobs);
            s.Directory = RealPath(output);
            s.Nonce = HashHex(s.Nonce + "corrected");
            s.Corrected = true;
            s.Layout = std::move(layout);
            SaveAnalysis(s);
            std::vector<uint64_t> bytes;
            auto probeInputs = Preflight(s, workDir, calibration, analysisJobs, &bytes);
            s.Prediction = Predict(s.Data, s.Layout, calibration, analysisJobs, bytes);
            // The correction earns its second measurement only by beating the
            // plan it started from on the model the first one taught. A
            // candidate won its objective before the compatibility checks and
            // the measured fit took their share of it; priced after them, on
            // the same calibrated model as the initial plan, it may not.
            double initialObjective = -1;
            // The artifacts of the initial plan were measured: the fit and the
            // score of it take their sizes, as the corrected plan's do.
            auto initialOnModel = ScoredLayout(s.Data, measuredInitial, calibration, analysisJobs, &initialObjective,
                                               initialBytes.empty() ? nullptr : &initialBytes);
            s.RetainedInitial = initialObjective > 0 && s.Prediction.Objective > initialObjective;
            if (s.RetainedInitial) {
                Report(Format("  Corrected plan scores %s after the compatibility checks and the measured fit; the initial plan scores %s on the same model: retaining the initial plan.",
                    Seconds(s.Prediction.Objective).c_str(), Seconds(initialObjective).c_str()));
                s.Layout = std::move(initialOnModel);
                probeInputs = Preflight(s, workDir, calibration, analysisJobs, &bytes);
                s.Prediction = Predict(s.Data, s.Layout, calibration, analysisJobs, bytes);
            }
            Emit(s, probeInputs, FrozenSampleFiles(s.Data, s.Sample, s.Measured));
            ValidateIdentity(s);
            WriteFile(JoinPath(output, "initial-errors.tsv"), errors);
            WriteFile(JoinPath(output, "initial-action-errors.tsv"), actionErrors);
            WriteFile(JoinPath(output, "calibration.tsv"), calibration.Report);
            WriteFile(JoinPath(output, "calibration-targets.tsv"), calibration.TargetReport);
            WriteFile(JoinPath(output, "calibration-units.tsv"), calibration.UnitReport);
            CalibrationSummary(s, calibration);
            Summary(s);
            WriteFile(JoinPath(output, "snapshot.bin"), Encode(s));
            Detail("corrected prediction snapshot: " + output);
            Diagnostic([&] {
                const auto initial = LoadPlan(plan);
                size_t gained = 0, lost = 0, changed = 0;
                std::ostringstream changes;
                changes << "target\tinitial_leaf\tcorrected_leaf\tchange\n";
                auto samePch = [&](auto&& self, int a, int b) -> bool {
                    if (a < 0 || b < 0) { return a == b; }
                    const auto& x = initial.Layout.Pchs[a];
                    const auto& y = s.Layout.Pchs[b];
                    return x.Class == y.Class && x.Roots == y.Roots && x.Units == y.Units && self(self, x.Parent, y.Parent);
                };
                for (size_t t = 0; t < s.Data.Targets.size(); ++t) {
                    const int a = initial.Layout.Assignment[t], b = s.Layout.Assignment[t];
                    gained += a < 0 && b >= 0; lost += a >= 0 && b < 0;
                    const bool different = !samePch(samePch, a, b);
                    changed += a >= 0 && b >= 0 && different;
                    changes << s.Data.Targets[t].Label << '\t' << a << '\t' << b << '\t'
                            << (a < 0 && b >= 0 ? "gained" : a >= 0 && b < 0 ? "lost" : different ? "changed-pch" : "unchanged") << '\n';
                }
                WriteFile(JoinPath(output, "assignment-changes.tsv"), changes.str());
                Report(Format("  Initial → corrected: %zu → %zu PCHs; targets gained PCH: %zu, lost PCH: %zu, changed PCH contents: %zu",
                    initial.Layout.Pchs.size(), s.Layout.Pchs.size(), gained, lost, changed));
                if (s.RetainedInitial) {
                    Report("  The initial plan is retained: no corrected candidate scored better than it after the checks, on the calibrated model.");
                } else if (initial.Prediction.Objective > 0) {
                    Report(Format("  Objective forecast change: %+.2f%%; model and layout both changed — not a measured speedup.",
                        100 * (s.Prediction.Objective / initial.Prediction.Objective - 1)));
                }
                PublishReports(s);
            });
            return 0;
        }

        bool Verified(const TSnapshot& s) {
            auto path = JoinPath(s.Directory, "run.bin");
            if (!Exists(path)) { return false; }
            auto run = Decode<TRunRecord>(ReadFile(path));
            return run.Status == 0 && run.End > run.Start && run.Nonce == s.Nonce &&
                run.Snapshot == DigestFile(JoinPath(s.Directory, "snapshot.bin"));
        }

        int FinishCycle(TSnapshot& s, const std::string& cycle, const std::string& workDir, size_t analysisJobs = 1) {
            // The plan and its analysis checkpoint are written; older cycles
            // are not needed any more, and verification needs the disk.
            const auto state = ProfileDirectory(s.Baseline, s.Mode);
            PruneRuns(state, cycle);
            auto output = ReadPointer(JoinPath(cycle, "result.path"));
            if (output.empty()) { output = JoinPath(cycle, "corrected"); }
            if (Exists(JoinPath(output, "snapshot.bin"))) {
                auto corrected = LoadPlan(output);
                ValidateIdentity(corrected);
                if (!corrected.Corrected || corrected.Nonce != HashHex(s.Nonce + "corrected")) {
                    Fail("corrected result belongs to a different cycle");
                }
                Report("reusing completed result: " + output);
            } else {
                if (!Verified(s)) {
                    Log("cycle: measured verification");
                    int status = Verify(s);
                    if (status) { return status; }
                } else { Log("cycle: reusing completed verification"); }
                if (Exists(output)) { output += "-" + std::to_string(Now()); }
                Log("cycle: correction");
                const auto initial = s.Directory;
                int status = CorrectPlan(s, initial, output, workDir, analysisJobs);
                if (status) { return status; }
            }
            WriteFile(JoinPath(cycle, "result.path"), output + "\n");
            WriteFile(JoinPath(state, "result.path"), output + "\n");
            // Age counts from the last successful cycle, not from creation.
            TGeneration generation;
            if (TryReadGeneration(JoinPath(state, "generation"), &generation)) {
                generation.Created = WallNow();
                WriteGeneration(state, generation);
            }
            Report("result: " + output);
            return 0;
        }
    } // namespace

    void IoSelfTest() {
        auto check = [](bool ok, const std::string& message) { if (!ok) { Fail("I/O self-test: " + message); } };
        // A plan builds and measures the PCH GN deploys: with the flags of
        // pch_flags.gni that change the artifact, without those that only
        // name its language or where it resolves its paths.
        {
            const char* temp = std::getenv("TMPDIR");
            auto dir = MakeTempDir(temp ? temp : ".", "pch-flags-");
            try {
                TSnapshot s;
                s.Root = dir;
                check(MeasurementPchFlags(s).empty(), "a tree without pch_flags.gni measures a PCH with the flags of its class alone");
                MakeDirs(JoinPath(dir, "build/gn/pch/global"));
                WriteFile(JoinPath(dir, "build/gn/pch/global/pch_flags.gni"),
                          "pch_only_cflags = [ \"-x\", \"c++-header\", \"-fpch-instantiate-templates\", \"-relocatable-pch\","
                          " \"-Xclang\", \"-fno-pch-timestamp\", \"-Xclang\", \"-fmodule-file-home-is-cwd\" ]");
                check(MeasurementPchFlags(s) == std::vector<std::string>({"-fpch-instantiate-templates", "-Xclang", "-fno-pch-timestamp"}),
                      "a measured PCH carries the flags of pch_flags.gni that change the artifact");
            } catch (...) {
                RemoveTree(dir);
                throw;
            }
            RemoveTree(dir);
        }
        // Random bytes in random chunks digest as the whole output.
        {
            uint64_t state = 7;
            auto next = [&] { state = state * 6364136223846793005ULL + 1442695040888963407ULL; return state >> 33; };
            for (int trial = 0; trial < 300; ++trial) {
                std::string text(size_t(next() % 400), '\0');
                for (char& c : text) { c = char(next()); }
                TStreamHash stream;
                size_t at = 0;
                while (at < text.size()) {
                    const size_t n = std::min(size_t(next() % 70), text.size() - at);
                    stream.Feed(std::string_view(text).substr(at, n));
                    at += n;
                }
                check(stream.Hex() == HashHex(text), "a stream hashed in chunks digests as a whole");
            }
        }
        Progress("self-test progress 1/2");
        Progress("self-test progress 2/2, replaced in place on a terminal");
        Log("progress updates are flushed before the next message");
        TSnapshot snapshot;
        snapshot.Nonce = "frozen";
        snapshot.Sample = 40;
        snapshot.Mode = "global";
        snapshot.Measured = {1, 2, 3};
        snapshot.Data.Resources.Pools["compile"] = 2;
        snapshot.Data.Files.push_back({3, 7, "file.cpp", {{1, 2.5}}});
        snapshot.Data.Classes = 8;
        snapshot.Data.Actions.resize(4);
        GroupTargets(snapshot.Data, {"//test:owner"});
        snapshot.Layout.Assignment = {-1};
        snapshot.Data.Targets[0].PchEligible = false;
        snapshot.LinkInputs.Lines = {{"app", {"file.o", "library.a"}}};
        snapshot.LinkInputs.Targets = {{"file.o", {0}}};
        snapshot.LinkInputs.Libraries = {"file.o", "library.a"};
        auto encoded = Encode(snapshot);
        auto loaded = Decode<TSnapshot>(encoded);
        check(loaded.Data.Files[0].Own[0].second == 2.5 && loaded.Data.Resources.Pools.at("compile") == 2,
              "sparse costs and pool depths survive a snapshot");
        check(loaded.Sample == 40 && loaded.Measured == std::vector<int>({1, 2, 3}),
              "measurement sample survives a snapshot");
        check(loaded.Mode == "global", "profile survives a frozen snapshot");
        check(loaded.LinkInputs.Targets.at("file.o") == std::vector<int>{0} &&
              loaded.LinkInputs.Lines[0].Inputs == std::set<std::string>({"file.o", "library.a"}) &&
              loaded.LinkInputs.Libraries == snapshot.LinkInputs.Libraries,
              "exact object membership and deduplicated link inputs survive analysis/snapshot serialization");
        check(loaded.Data.Targets[0].Label == "//test:owner" && loaded.Data.Targets[0].Files == std::vector<int>{0} &&
              loaded.Data.Files[0].Target == 0 && !loaded.Data.Targets[0].PchEligible && loaded.Layout.Assignment == std::vector<int>{-1},
              "target membership and atomic assignments survive a snapshot");
        auto legacy = [](auto value) {
            TSave body;
            body.HasProfile = false;
            body(value);
            TBinWriter envelope;
            envelope.Str(LEGACY_VERSION);
            envelope.Str(HashHex(body.Out.Data()));
            envelope.Str(body.Out.Data());
            return envelope.Data();
        };
        bool oldRejected = false;
        try { Decode<TSnapshot>(legacy(snapshot)); } catch (const TError&) { oldRejected = true; }
        check(oldRejected, "file-wise snapshots cannot be interpreted as target-wise assignments");
        TAnalysis oldAnalysis;
        oldAnalysis.Snapshot = snapshot;
        oldRejected = false;
        try { Decode<TAnalysis>(legacy(oldAnalysis)); } catch (const TError&) { oldRejected = true; }
        check(oldRejected, "old analysis without GN owner membership requires fresh extraction");
        TCompatibilityRecord oldProbe;
        oldProbe.Command = "cached probe";
        check(Decode<TCompatibilityRecord>(legacy(oldProbe)).Command == oldProbe.Command,
              "profile serialization preserves old compatibility cache records");
        {
            TData sampling;
            sampling.Targets.resize(4);
            sampling.Files.resize(10);
            sampling.Actions.resize(10);
            for (int i = 0; i < 10; ++i) {
                int t = i < 5 ? 0 : i < 8 ? 1 : i - 6;
                sampling.Targets[t].Label = "//:" + std::to_string(t);
                sampling.Targets[t].Files.push_back(i);
                sampling.Files[i].Target = t;
                sampling.Files[i].Action = i;
                sampling.Actions[i].Output = "file" + std::to_string(i) + ".o";
                sampling.Actions[i].Time = i + 1; // total work: 15, 21, 9, 10
            }
            auto small = SampleFiles(sampling, "nonce", 11);
            check(std::count(small.begin(), small.begin()+5, true) == 1 &&
                  std::count(small.begin()+5, small.begin()+8, true) == 1 && !small[8] && !small[9],
                  "round up, cover the two highest-work targets when not all fit");
            auto panel = SampleFiles(sampling, "nonce", 70);
            check(panel[0]+panel[1] == 1 && panel[2]+panel[3]+panel[4] == 1 &&
                  panel[5] && panel[6] && panel[7] && panel[8] && panel[9],
                  "cover all targets; extra slots follow baseline work (2/3/1/1 strata), not file count");
            std::reverse(sampling.Targets[0].Files.begin(), sampling.Targets[0].Files.end());
            check(SampleFiles(sampling, "nonce", 70) == panel, "draw stable by object output, not target traversal order");
            auto forced = SampleFiles(sampling, "nonce", 11, {0, 0, 1, 0});
            check(forced[8] && !forced[9] && std::count(forced.begin()+5, forced.begin()+8, true) == 1 &&
                  std::count(forced.begin(), forced.begin()+5, true) == 0,
                  "a calibration control is measured even when low-work targets do not fit");
            auto privileged = SampleFiles(sampling, "nonce", 50, {1, 0, 0, 0});
            check(std::count(privileged.begin(), privileged.begin()+5, true) == 2 && privileged[8] && privileged[9] &&
                  std::count(privileged.begin()+5, privileged.begin()+8, true) == 1,
                  "a control takes the first extra member ahead of a heavier target");
            auto baseControl = SampleFiles(sampling, "nonce", 50, {2, 0, 0, 0});
            check(std::count(baseControl.begin(), baseControl.begin()+5, true) == 1 && baseControl[8] && baseControl[9] &&
                  std::count(baseControl.begin()+5, baseControl.begin()+8, true) == 2,
                  "a control kept on the base takes its turn in the queue like any other owner");
            check(SampleFiles(sampling, "nonce", 50, {1}) == SampleFiles(sampling, "nonce", 50),
                  "a control table of another size is not a control table");
            std::vector<int> ledger;
            for (int f = 0; f < 10; ++f) { if (panel[f]) { ledger.push_back(f); } }
            ledger.push_back(10); // initial PCH ID, not part of the corrected object panel
            check(FrozenSampleFiles(sampling, 70, ledger) == panel, "restore the exact initial panel from its snapshot");
            check(SampleFiles(sampling, "nonce", 100) == std::vector<char>(10, true), "full sample contains every object");
            check(MeasuredActions(3, {}) == std::vector<char>({0, 0, 0}) &&
                  MeasuredActions(3, {0, 2}) == std::vector<char>({1, 0, 1}),
                  "an empty target sample is not interpreted as a full verification");
            // What a header alone still needs of the C++ runtime is no need at
            // all: every final link supplies it. A project symbol is.
            check(RuntimeOnly({}) &&
                      RuntimeOnly({"_Znwm", "_ZnwmSt11align_val_t", "_Znam", "_ZdlPv", "_ZdlPvm", "_ZdaPv", "__cxa_atexit",
                                    "__cxa_guard_acquire", "__gxx_personality_v0", "_Unwind_Resume", "__clang_call_terminate",
                                    "_ZTVN10__cxxabiv117__class_type_infoE", "_ZNSt3__18ios_base4InitC1Ev", "_ZSt4cout",
                                    "_ZNKSt3__19basic_iosIcNS_11char_traitsIcEEE4clearEj",
                                    "_ZZNSt3__110this_thread6get_idEvE15__thread_local_id", "_ZThn16_NSt3__16localeC1Ev",
                                    "_ZGVZNKSt3__16locale5globalEvE4none",
                                    "_ZTv0_24_NSt3__19basic_ostreamIcNS_11char_traitsIcEEE5flushEv"}),
                  "the C++ runtime symbols a header alone needs do not make it unsafe");
            // The entity itself must be the runtime's, not its signature: a
            // project function taking a std::string is a symbol of the project,
            // and so is a thunk, a local static or a vtable of its own.
            check(!RuntimeOnly({"_ZN7example8RegisterERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE"}) &&
                      !RuntimeOnly({"_ZneRK7example3KeyS2_"}) && !RuntimeOnly({"Provided"}) &&
                      !RuntimeOnly({"_ZZN7example3fooEvE5count"}) && !RuntimeOnly({"_ZTV7example3Base"}) &&
                      !RuntimeOnly({"_ZThn16_NK7example3Base3runEv"}) && !RuntimeOnly({"_Z1fISt6vectorIiEEvv"}) &&
                      !RuntimeOnly({"_ZL9StaticFuncv"}) && !RuntimeOnly({"_ZN3foo3barEv"}),
                  "a symbol of the project needed by a header alone keeps it out of a base");
        }
        {
            TSnapshot target;
            TProfileOptions profile;
            profile.Apply(target);
            check(target.Mode == "incremental" && target.Data.Q == 0.1, "default mode preserves the old objective");
            profile.SetMode("global");
            profile.Apply(target);
            check(target.Mode == "global" && target.Data.Q == 1, "global defaults to cold-only scoring");
            profile.SetQ(0.25);
            profile.SetMode("incremental");
            profile.Apply(target);
            check(target.Data.Q == 0.25, "explicit q wins even when mode comes after it");
            profile.SetMode("global");
            profile.SetQ(0);
            profile.Apply(target);
            check(target.Mode == "global" && target.Data.Q == 0, "explicit zero q is not replaced by the profile default");
            TProfileOptions inherited;
            inherited.Apply(target, true);
            check(target.Mode == "global" && target.Data.Q == 0, "replan without profile options inherits mode and q");
            inherited.SetMode("incremental");
            inherited.Apply(target, true);
            check(target.Mode == "incremental" && target.Data.Q == 0.1, "switching replan mode applies its default q");
            size_t i = 0;
            check(ReadOption({"--mode=global"}, i) == std::make_pair(std::string("--mode"), std::string("global")) && i == 0,
                  "equals-form profile option is parsed as one argument");
            i = 0;
            check(ReadOption({"--mode", "incremental"}, i).second == "incremental" && i == 1,
                  "separate profile option consumes its value");
            bool invalid = false;
            try { profile.SetMode("../global"); } catch (const TError&) { invalid = true; }
            check(invalid, "invalid profile names cannot escape their state directory");
            invalid = false;
            try { profile.SetQ(1.1); } catch (const TError&) { invalid = true; }
            check(invalid, "q validation applies to replan as well as predict");
        }
        check(loaded.Sample == 40 && loaded.Measured == std::vector<int>({1, 2, 3}),
              "measurement sample survives a snapshot");
        encoded.back() ^= 1;
        bool rejected = false;
        try { Decode<TSnapshot>(encoded); } catch (const TError&) { rejected = true; }
        check(rejected, "modified snapshot rejected before use");
        auto flags = CompileFlags({"/tool/clang++", "-DNAME=1", "-Igen", "-MD", "-MF", "x.d", "-c", "x.cpp", "-o", "x.o"}, "x.cpp");
        check(flags == std::vector<std::string>({"/tool/clang++", "-DNAME=1", "-Igen"}), "PCH flags preserve macro/include order");
        TCompile generated{{"/tool/clang++", "-Igen", "-c", "/build/gen/helper.cpp", "-o", "obj/helper.o"}, "gen/helper.cpp"};
        NormalizeCompileInput(generated, "/build");
        check(CompileFlags(generated.Argv, generated.Source) == std::vector<std::string>({"/tool/clang++", "-Igen"}),
              "absolute generated source operand is not a compiler class flag");
        const char* out = getenv("OUT");
        std::string parent = out && *out ? out : "/place/vartmp/opencode";
        MakeDirs(parent);
        std::string dir = MakeTempDir(parent, "pch-self-test-");
        struct TCleanup {
            std::string Dir;
            bool HadToken = false;
            std::string Token;
            ~TCleanup() {
                if (HadToken) { setenv("PCH_UPDATE_RUN", Token.c_str(), 1); }
                else { unsetenv("PCH_UPDATE_RUN"); }
                try { RemoveTree(Dir); } catch (...) {}
            }
        } cleanup{dir, getenv("PCH_UPDATE_RUN") != nullptr, getenv("PCH_UPDATE_RUN") ? getenv("PCH_UPDATE_RUN") : ""};
        MakeDirs(JoinPath(dir, "build"));
        MakeDirs(JoinPath(dir, "observations"));
        {
            // A real Ninja invocation catches accidental traversal into the
            // unsampled library, and missing headers from either dependency
            // source. The commands stand in for compilation; no compiler needed.
            const auto fixture = JoinPath(dir, "sampling");
            MakeDirs(fixture);
            WriteFile(JoinPath(fixture, "main.cpp"), "consumer\n");
            WriteFile(JoinPath(fixture, "tool.cpp"), "generator\n");
            TNinjaDeps headers;
            headers.Paths = {"main.cpp", "tool.cpp", "gen/private.h"};
            headers.Deps["sample.o"] = {0, 2};
            headers.Deps["tool.o"] = {1};
            const std::vector<std::string> generated = {"", "", "gen/private.h"};
            const std::vector<std::string> original = {"unrelated"};
            const auto sampleOrder = CompilerOrderOnly(20, "sample.o", original, headers, generated, {"gen/probe.h"});
            const auto toolOrder = CompilerOrderOnly(20, "tool.o", original, headers, generated, {});
            check(CompilerOrderOnly(100, "sample.o", original, {}, {}, {}) == original,
                  "full verification preserves GN ordering without requiring sampled dependency data");
            rejected = false;
            try { CompilerOrderOnly(20, "missing.o", original, headers, generated, {}); }
            catch (const TError&) { rejected = true; }
            check(rejected, "missing baseline dependency records cannot silently lose generated headers");
            std::string manifest =
                "rule compile_tool\n  command = cat $in > $out\n"
                "rule link\n  command = cat $in > $out\n"
                "rule generate\n  command = test -f protoc && mkdir -p gen && echo $out > $out\n"
                "rule pch\n  command = cat gen/pch.h > $out\n"
                "rule consume\n  command = cat main.cpp gen/private.h gen/probe.h base.pch > $out\n"
                "rule forbidden\n  command = false\n"
                "build tool.o: compile_tool tool.cpp" + (toolOrder.empty() ? "" : " ||" + Paths(toolOrder)) + "\n"
                "build protoc: link tool.o\n"
                "build gen/pch.h: generate | protoc\n"
                "build gen/private.h: generate | protoc\n"
                "build gen/probe.h: generate | protoc\n"
                "build base.pch: pch | gen/pch.h\n"
                "build sample.o: consume main.cpp | base.pch ||" + Paths(sampleOrder) + "\n"
                "build unused.o: forbidden\n"
                "build unused.a: link unused.o\n"
                "build unrelated: phony unused.a gen/private.h\n";
            WriteFile(JoinPath(fixture, "build.ninja"), manifest);
            TNinjaManifest graph(fixture);
            std::vector<std::vector<int>> dependencies(graph.Edges().size());
            for (size_t i = 0; i < graph.Edges().size(); ++i) {
                const auto& edge = graph.Edges()[i];
                for (const auto* list : {&edge.Inputs, &edge.ImplicitInputs, &edge.OrderOnly, &edge.Validations}) {
                    for (const auto& path : *list) {
                        int j = int(graph.Producer(path));
                        if (j >= 0) { dependencies[i].push_back(j); }
                    }
                }
            }
            const auto reached = MeasurementClosure(dependencies, {int(graph.Producer("sample.o")), int(graph.Producer("base.pch"))});
            check(std::count(reached.begin(), reached.end(), true) == 7 && !reached[graph.Producer("unused.o")],
                  "reported closure counts generator compilation, linking, headers and PCH but not the unrelated library");
            TRunOptions run;
            run.Cwd = fixture;
            RunChecked({"ninja", "sample.o", "base.pch"}, run);
            check(ReadFile(JoinPath(fixture, "sample.o")) == "consumer\ngen/private.h\ngen/probe.h\ngen/pch.h\n" &&
                  Exists(JoinPath(fixture, "protoc")) && !Exists(JoinPath(fixture, "unused.o")),
                  "sampled Ninja builds both baseline and PCH-only headers through their real tool chain");
        }
        auto managed = StateDirectory(JoinPath(dir, "build"));
        {
            const auto marker = JoinPath(managed, "generation");
            TGeneration generation;
            generation.Schema = ANALYSIS_VERSION;
            generation.Root = dir;
            generation.Tree = "tree-a";
            generation.Baseline = "baseline-a";
            generation.Created = WallNow() - 3600;
            WriteGeneration(managed, generation);
            TGeneration read;
            check(TryReadGeneration(marker, &read) && read.Tree == "tree-a" && read.Created == generation.Created,
                  "state generation survives a round trip");
            TGeneration other = generation;
            other.Tree = "tree-b";
            check(GenerationMismatch(generation, generation, 7 * 86400).empty(), "the same generation is reused");
            check(GenerationMismatch(generation, other, 7 * 86400) == "source tree changed",
                  "a changed tree invalidates the generation");
            check(!GenerationMismatch(generation, generation, 60).empty(), "an old generation expires");
            check(GenerationMismatch(generation, generation, 0).empty(), "zero age never expires");
            const auto runs = JoinPath(managed, "runs");
            for (const char* name : {"cycle-a/initial", "cycle-b/initial", "imported-0/initial"}) {
                MakeDirs(JoinPath(runs, name));
            }
            WriteFile(JoinPath(runs, "cycle-b/initial/observations/1.bin"), "observation");
            WriteFile(JoinPath(runs, "cycle-a/initial/observations/1.bin"), "observation");
            WriteFile(JoinPath(runs, "cycle-a/initial/snapshot.bin"), "snapshot");
            MakeDirs(JoinPath(runs, "cycle-a/initial/build/obj"));
            MakeDirs(JoinPath(runs, "cycle-a/initial/actions"));
            WriteFile(JoinPath(managed, "analysis.path"), JoinPath(runs, "imported-0/initial") + "\n");
            PruneRuns(managed, JoinPath(runs, "cycle-b"));
            check(IsDir(JoinPath(runs, "cycle-b/initial")) && IsDir(JoinPath(runs, "imported-0")),
                  "pruning keeps the current cycle and a reused checkpoint");
            check(!IsDir(JoinPath(runs, "cycle-a/initial/build")) && !Exists(JoinPath(runs, "cycle-a/initial/snapshot.bin")),
                  "a previous cycle loses its build products");
            check(Exists(JoinPath(runs, "cycle-a/initial/observations/1.bin")),
                  "a previous cycle keeps its measurements");
            const int64_t now = WallNow();
            for (const auto& [name, age] : std::vector<std::pair<const char*, int>>{{"cycle-a", 300}, {"imported-0", 200}, {"cycle-b", 100}}) {
                timespec times[2] = {{time_t(now - age), 0}, {time_t(now - age), 0}};
                utimensat(AT_FDCWD, JoinPath(runs, name).c_str(), times, 0);
            }
            ClearState(managed, "self-test");
            check(!Exists(JoinPath(managed, "analysis.path")), "a new profile generation removes its pointers");
            check(ListDir(runs) == std::vector<std::string>{"cycle-b"} &&
                  Exists(JoinPath(runs, "cycle-b/initial/observations/1.bin")),
                  "a new generation keeps the measurements of the last cycle and drops the rest");
        }
        {
            const auto baseline = JoinPath(dir, "profile-baseline");
            MakeDirs(baseline);
            const auto shared = StateDirectory(baseline);
            const auto global = ProfileDirectory(baseline, "global");
            const auto incremental = ProfileDirectory(baseline, "incremental");
            const auto cache = JoinPath(shared, "cache");
            TGeneration generation;
            generation.Schema = ANALYSIS_VERSION;
            generation.Root = dir;
            generation.Tree = "unchanged";
            generation.Baseline = "same baseline";
            generation.Created = WallNow();
            WriteGeneration(shared, generation);
            WriteGeneration(incremental, generation);
            WriteFile(JoinPath(cache, "compatibility-v1/entry"), "shared probe");
            WriteFile(JoinPath(shared, "runs/legacy/initial/snapshot.bin"), "legacy result");
            const auto otherPlan = JoinPath(incremental, "runs/keep/initial");
            WriteFile(JoinPath(otherPlan, "snapshot.bin"), "other result");
            WriteFile(JoinPath(otherPlan, "build/keep.o"), "other object");
            for (const char* name : {"analysis.path", "current.path", "result.path"}) {
                WriteFile(JoinPath(incremental, name), otherPlan + "\n");
            }
            WriteFile(JoinPath(shared, "analysis.path"), otherPlan + "\n");
            PrepareProfileState(shared, global, generation, 60);
            check(Exists(JoinPath(cache, "compatibility-v1/entry")) &&
                  ReadPointer(JoinPath(shared, "analysis.path")) == otherPlan,
                  "starting another profile preserves shared analysis and compatibility work");
            const auto old = JoinPath(global, "runs/old/initial");
            WriteFile(JoinPath(old, "build/old.o"), "old object");
            WriteFile(JoinPath(old, "observations/0.bin"), "old measurement");
            const auto current = JoinPath(global, "runs/new");
            MakeDirs(JoinPath(current, "initial"));
            PruneRuns(global, current);
            check(!Exists(JoinPath(old, "build/old.o")) && Exists(JoinPath(otherPlan, "build/keep.o")),
                  "pruning one profile cannot shrink the other profile's build");
            auto expired = generation;
            expired.Created -= 120;
            WriteGeneration(global, expired);
            PrepareProfileState(shared, global, generation, 60);
            check(Exists(JoinPath(cache, "compatibility-v1/entry")) &&
                  ReadPointer(JoinPath(incremental, "result.path")) == otherPlan &&
                  Exists(JoinPath(otherPlan, "snapshot.bin")),
                  "age expiry is profile-local and preserves the other saved variant");
            generation.Tree = "changed";
            PrepareProfileState(shared, global, generation, 60);
            check(Exists(JoinPath(cache, "compatibility-v1/entry")) &&
                  !Exists(JoinPath(shared, "analysis.path")) && Exists(JoinPath(otherPlan, "snapshot.bin")) &&
                  Exists(JoinPath(shared, "runs/legacy/initial/snapshot.bin")),
                  "new shared inputs retire the analysis but leave dependency-validated probes available");
            rejected = false;
            try { CheckProfileOutput(baseline, "global", JoinPath(incremental, "runs/new")); }
            catch (const TError&) { rejected = true; }
            check(rejected, "explicit output cannot overwrite another profile's state");
        }
        check(StorageAllowed(JoinPath(managed, "runs/one/initial"), RealPath("."), JoinPath(dir, "build")),
              "managed plans may live inside the baseline state directory");
        check(!StorageAllowed(JoinPath(dir, "build/obj"), RealPath("."), JoinPath(dir, "build")) &&
              !StorageAllowed(JoinPath(dir, "build"), RealPath("."), JoinPath(dir, "build")) &&
              !StorageAllowed(managed, RealPath("."), JoinPath(dir, "build")),
              "ordinary baseline outputs and the state root cannot become plan directories");
        auto oldCache = JoinPath(dir, "old-cache"), newCache = JoinPath(managed, "cache");
        MakeDirs(newCache);
        check(symlink(newCache.c_str(), oldCache.c_str()) == 0, "create migration alias");
        WriteFile(JoinPath(newCache, ".path-alias"), oldCache + "\n");
        check(CacheDirectory(newCache) == oldCache, "migrated cache preserves compiler command paths");
        TSnapshot moved;
        moved.Directory = oldCache;
        WriteFile(JoinPath(newCache, "snapshot.bin"), Encode(moved));
        check(LoadPlan(newCache).Directory == oldCache, "migrated plans retain frozen paths through a valid alias");
        WriteFile(JoinPath(newCache, ".path-alias"), JoinPath(dir, "unrelated") + "\n");
        rejected = false;
        try { CacheDirectory(newCache); } catch (const TError&) { rejected = true; }
        check(rejected, "migration alias must resolve to the same cache directory");
        auto compilerLink = JoinPath(dir, "clang++");
        check(symlink("/bin/sh", compilerLink.c_str()) == 0 && Program(compilerLink, dir, false) == compilerLink,
              "compatibility compiler resolution preserves the clang++ driver name");
        TSnapshot cached;
        cached.Root = RealPath(".");
        cached.Baseline = JoinPath(dir, "build");
        cached.Directory = dir;
        cached.Tool = "previous-tool";
        auto input = JoinPath(dir, "input.h");
        WriteFile(input, "original");
        cached.Inputs = {{input, DigestFile(input)}};
        SaveAnalysis(cached);
        auto reused = LoadAnalysis(dir);
        check(reused.Tool == DigestFile(SelfExe()), "analysis reuse allows a new selector binary");
        WriteFile(input, "changed");
        rejected = false;
        try { LoadAnalysis(dir); } catch (const TError&) { rejected = true; }
        check(rejected, "analysis reuse rejects changed source inputs");
        WriteFile(input, "original");
        WriteFile(JoinPath(cached.Baseline, ".ninja_log"), "changed build measurements");
        rejected = false;
        try { LoadAnalysis(dir); } catch (const TError&) { rejected = true; }
        check(rejected, "analysis reuse rejects changed baseline measurements");

        // A leaf introducing an ambiguous name must fall back to a checked
        // parent, or to a checked no-PCH compile, without changing frozen bases.
        TLayout compatibility;
        compatibility.Bases = 2;
        compatibility.Pchs.resize(4);
        compatibility.Pchs[0].Roots = {10};
        compatibility.Pchs[1].Roots = {11};
        compatibility.Pchs[2].Parent = compatibility.Pchs[3].Parent = 1;
        TData consumers;
        consumers.Classes = 1;
        consumers.Actions.resize(3);
        consumers.Files = {{0, 0, "a.cpp", {}}, {1, 0, "b.cpp", {}}, {2, 0, "c.cpp", {}}};
        GroupTargets(consumers, {"//a:lib", "//a:lib", "//b:lib"});
        compatibility.Assignment = {2, 3};
        std::vector<std::vector<int>> attempted(3);
        std::vector<TCompatibilityProgress> boundaries;
        CompatibleTargets(consumers, compatibility, {true, true, true, false}, 2, [&](int f, int g) {
            attempted[f].push_back(g);
            return f != 1 || g == 1;
        }, [&](const TCompatibilityProgress& p, bool boundary) {
            if (boundary) { boundaries.push_back(p); }
        });
        check(boundaries.size() == 2 && boundaries.front().FinishedTargets == 0 &&
              boundaries.back().Targets == 2 && boundaries.back().FinishedTargets == 2 &&
              boundaries.back().Files == 3 && boundaries.back().Checks == 5 && boundaries.back().Retries == 2,
              "fallback counts repeated file checks without inflating file or target coverage");
        check(compatibility.Assignment == std::vector<int>({1, 1}) && attempted[0] == std::vector<int>({2, 1}) &&
              attempted[1] == std::vector<int>({2, 1}) && attempted[2] == std::vector<int>{1},
              "one failing member moves its whole target to a rechecked parent; failed PCHs are skipped");
        compatibility.Assignment = {2, -1};
        attempted.assign(3, {});
        CompatibleTargets(consumers, compatibility, {true, true, true, false}, 2, [&](int f, int g) {
            attempted[f].push_back(g);
            return f == 0 ? g != 1 : g < 0;
        });
        check(compatibility.Assignment == std::vector<int>({-1, -1}) && attempted[0] == std::vector<int>({2, 1, -1}) &&
              attempted[1] == std::vector<int>({2, 1, -1}) && attempted[2].empty(),
              "different member failures cannot produce mixed leaf/parent/none assignments");
        rejected = false;
        compatibility.Assignment = {2, -1};
        try { CompatibleTargets(consumers, compatibility, {true, true, true, false}, 2, [](int, int) { return false; }); }
        catch (const TError&) { rejected = true; }
        check(rejected, "a no-PCH failure cannot be disguised as an incompatible assignment");
        compatibility.Assignment = {1, -1};
        CompactLayout(compatibility);
        check(compatibility.Bases == 1 && compatibility.Pchs.size() == 1 &&
              compatibility.Pchs[0].Roots == std::vector<int>{11} && compatibility.Assignment == std::vector<int>({0, -1}),
              "compatibility pruning removes unused chains and remaps consumers");

        std::vector<TPch> concurrentPchs(4);
        concurrentPchs[2].Parent = 0;
        concurrentPchs[3].Parent = 1;
        std::mutex concurrentMutex;
        std::condition_variable concurrentChanged;
        std::vector<char> concurrentDone(4);
        int arrivals[2] = {}, active = 0, peak = 0;
        ParallelPchs(concurrentPchs, 2, [&](size_t g) {
            std::unique_lock<std::mutex> lock(concurrentMutex);
            int parent = concurrentPchs[g].Parent;
            check(parent < 0 || concurrentDone[parent], "PCH parent completes before its delta starts");
            peak = std::max(peak, ++active);
            ++arrivals[g / 2];
            concurrentChanged.notify_all();
            check(concurrentChanged.wait_for(lock, std::chrono::seconds(5), [&] { return arrivals[g / 2] == 2; }),
                  "independent bases and deltas really run concurrently");
            concurrentDone[g] = true;
            --active;
        });
        check(peak == 2 && std::all_of(concurrentDone.begin(), concurrentDone.end(), [](char v) { return v; }),
              "PCH queue completes all work within its worker ceiling");
        std::fill(concurrentDone.begin(), concurrentDone.end(), false);
        ParallelPchs(concurrentPchs, 1, [&](size_t g) {
            int parent = concurrentPchs[g].Parent;
            check(parent < 0 || concurrentDone[parent], "serial PCH queue preserves dependencies");
            concurrentDone[g] = true;
        });
        rejected = false;
        try {
            ParallelPchs(concurrentPchs, 2, [&](size_t g) { if (!g) { Fail("PCH worker failure"); } });
        } catch (const TError&) { rejected = true; }
        check(rejected, "PCH worker failure wakes waiters and propagates");
        concurrentPchs[0].Parent = 1;
        rejected = false;
        try { ParallelPchs(concurrentPchs, 2, [](size_t) {}); } catch (const TError&) { rejected = true; }
        check(rejected, "invalid PCH dependency fails before launching workers");
        // The PCH builds take a quarter of the workers, at least one: several
        // times the RSS of an ordinary compile each, and every delta of a
        // base is ready the moment the base is built. The rest of the run
        // takes the count as it is.
        check(PchBuildJobs(1) == 1 && PchBuildJobs(3) == 1 && PchBuildJobs(4) == 1 && PchBuildJobs(7) == 1 &&
                  PchBuildJobs(8) == 2 && PchBuildJobs(64) == 16,
              "PCH builds take a quarter of the workers, at least one");
        // A nested loop shares the threads of its outer loop: four items on a
        // budget of 64 leave their four workers 16 threads each, and a loop of
        // one item runs inline and hands its nested loop the whole budget.
        {
            std::atomic<int> active{0}, peak{0};
            auto work = [&](size_t) {
                const int now = ++active;
                for (int seen = peak.load(); now > seen && !peak.compare_exchange_weak(seen, now);) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                --active;
            };
            ParallelFor(4, 64, [&](size_t) { ParallelFor(1000, 64, work); });
            check(peak > 16 && peak <= 64, "a nested loop takes its worker's share of the budget, not one thread");
            peak = 0;
            ParallelFor(1, 64, [&](size_t) { ParallelFor(1000, 64, work); });
            check(peak > 16 && peak <= 64, "a loop of one item runs inline and hands its nested loop the budget");
            std::atomic<int> ran{0};
            ParallelFor(0, 64, [&](size_t) { ++ran; });
            check(ran == 0, "an empty loop starts nothing");
        }
        // A plan of another build of the tool is refused by its identity before
        // any of its files is read: the action configurations are files of the
        // build that wrote them, in whatever layout it had.
        {
            const char* temp = std::getenv("TMPDIR");
            auto dir = MakeTempDir(temp ? temp : ".", "pch-foreign-");
            try {
                TSnapshot foreign;
                foreign.Directory = dir;
                foreign.Tool = "another build";
                MakeDirs(JoinPath(dir, "actions"));
                WriteFile(JoinPath(dir, "actions/0.bin"), "a configuration in a layout of that build");
                WriteFile(JoinPath(dir, "run.bin"), "a run record of that build");
                bool refused = false;
                try {
                    Verify(foreign);
                } catch (const TError& error) {
                    refused = std::string(error.what()) == "pch_update executable changed since prediction";
                }
                check(refused, "a plan of another build is refused by the identity, before its files are read");
            } catch (...) {
                RemoveTree(dir);
                throw;
            }
            RemoveTree(dir);
        }

        auto probeRoot = JoinPath(dir, "compatibility-cache");
        {
            const auto old = JoinPath(probeRoot, "compatibility-v1/entry");
            const auto current = JoinPath(probeRoot, CompatibilityCacheName() + "/entry");
            const auto other = JoinPath(probeRoot, "other-cache/entry");
            for (const auto& file : {old, current, other}) { WriteFile(file, "probe"); }
            TCycleLock lock;
            PrepareCompatibilityCache(probeRoot, lock);
            check(!Exists(old) && Exists(current) && Exists(other),
                  "only obsolete compatibility versions are removed before preflight");
        }
        TCompatibilityCache probeCache(probeRoot);
        auto artifact = JoinPath(probeCache.Directory("pch"), "output.pch");
        auto probeInput = JoinPath(dir, "compatibility-input.h");
        WriteFile(probeInput, "first");
        int executions = 0;
        auto compiler = [&](const std::vector<std::string>& argv, const TRunOptions& options) {
            ++executions;
            WriteFile(options.StdoutFile, "");
            for (size_t i = 1; i + 1 < argv.size(); ++i) {
                if (argv[i] == "-MF") { WriteFile(argv[i + 1], WriteDepfile("probe", {probeInput})); }
                if (argv[i] == "-o") { WriteFile(argv[i + 1], "artifact"); }
            }
            return TRunResult{};
        };
        const std::vector<std::string> probeCommand = {"/bin/sh", "-o", artifact};
        check(probeCache.Probe("pch", dir, probeCommand, "#pragma once\n", compiler) && executions == 1,
              "successful compatibility compile is recorded");
        check(probeCache.Probe("pch", dir, probeCommand, "#pragma once\n", compiler) && executions == 1,
              "unchanged compatibility probe reuses both result and artifact");
        check(!Exists(JoinPath(probeCache.Directory("pch"), "deps.d")),
              "successful probes retain dependencies only in their durable record");
        {
            const auto parent = JoinPath(dir, "parent-for-tu.pch");
            WriteFile(parent, "parent-v1");
            TCompatibilityCache first(probeRoot);
            int start = executions;
            check(first.Probe("tu", dir, {"/bin/sh"}, {}, compiler, {}, {parent}), "record consumer probe");
            WriteFile(JoinPath(dir, "unrelated.cpp"), "// unrelated edit");
            TCompatibilityCache next(probeRoot);
            check(next.Probe("tu", dir, {"/bin/sh"}, {}, compiler, {}, {parent}) && executions == start + 1,
                  "unrelated edit reuses consumer probe across runs");
            WriteFile(parent, "parent-v2");
            TCompatibilityCache changed(probeRoot);
            check(changed.Probe("tu", dir, {"/bin/sh"}, {}, compiler, {}, {parent}) && executions == start + 2,
                  "parent omitted from depfile still invalidates consumer");
            executions = start;
        }
        RemoveFile(artifact);
        check(probeCache.Probe("pch", dir, probeCommand, "#pragma once\n", compiler) && executions == 2,
              "evicted PCH is rebuilt instead of trusting a cached success");
        WriteFile(probeInput, "changed");
        TCompatibilityCache changedCache(probeRoot);
        check(changedCache.Probe("pch", dir, probeCommand, "#pragma once\n", compiler) && executions == 3,
              "changed transitive input invalidates cached compatibility");
        WriteFile(JoinPath(probeCache.Directory("pch"), "result.bin"), "interrupted write");
        check(changedCache.Probe("pch", dir, probeCommand, "#pragma once\n", compiler) && executions == 4,
              "damaged compatibility records are recomputed");
        auto incompatible = [](const std::vector<std::string>&, const TRunOptions& options) {
            WriteFile(options.StdoutFile, "error: reference to Module is ambiguous\n");
            TRunResult result;
            result.ExitCode = 1;
            return result;
        };
        check(!probeCache.Probe("bad", dir, {"/bin/sh"}, {}, incompatible) &&
              !Exists(JoinPath(probeCache.Directory("bad"), "result.bin")),
              "a compiler diagnostic is a rejection, never a cached success");
        auto interrupted = [](const std::vector<std::string>&, const TRunOptions&) {
            TRunResult result;
            result.ExitCode = 137;
            return result;
        };
        rejected = false;
        try { probeCache.Probe("killed", dir, {"/bin/sh"}, {}, interrupted); }
        catch (const TError&) { rejected = true; }
        check(rejected && !Exists(JoinPath(probeCache.Directory("killed"), "result.bin")),
              "interrupted probes cannot silently remove consumers or populate the cache");
        WriteFile(JoinPath(dir, "build/build.ninja"),
            "pool compile\n  depth = 2\nrule c\n  command = clang++ $in -o $out\n  pool = compile\n"
            "  rspfile = a.rsp\n  rspfile_content = $in\nbuild a | hidden: c source | implicit || stamp |@ check\n"
            "build stamp: phony\nbuild check: phony\n");
        TNinjaManifest manifest(JoinPath(dir, "build"));
        check(manifest.PoolDepths().at("compile") == 2 && manifest.PoolDepths().at("console") == 1, "Ninja pool depths retained");
        const auto& edge = manifest.Edges()[0];
        check(edge.ExplicitOutputs == 1 && edge.Outputs.size() == 2 && edge.Validations == std::vector<std::string>{"check"} &&
              edge.OrderOnly == std::vector<std::string>{"stamp"}, "implicit outputs, validation and scheduling dependencies distinguished");
        TRunRecord run;
        run.Nonce = "nonce";
        run.Token = "test-run";
        run.Start = Now();
        WriteFile(JoinPath(dir, "run.bin"), Encode(run));
        TMeasureConfig config;
        config.Directory = dir;
        config.Nonce = run.Nonce;
        config.Id = 0;
        config.Command = "printf first > a && printf second > b";
        config.Outputs = {"a", "b"};
        config.CacheMode = "not-compiler";
        config.Memory = 1;
        auto path = JoinPath(dir, "action.bin");
        WriteFile(path, Encode(config));
        setenv("PCH_UPDATE_RUN", run.Token.c_str(), 1);
        check(Measure(path) == 0, "measurement executes the entire compound shell command");
        auto record = Decode<TRecord>(ReadFile(JoinPath(dir, "observations/0.bin")));
        check(record.Outputs[0].first == 5 && record.Outputs[1].first == 6 && record.Observation.Invocation == run.Token &&
              record.Observation.Time > 0, "measurement records outputs, invocation and elapsed time");
        check(Measure(path) == 0, "Ninja may reschedule an action under the same locked invocation");
    }

    // Internal single-step entry point. Public invocation below owns all paths,
    // persisted options and stage ordering.
    static int StepCommand(const std::vector<std::string>& args, bool refreshSettings = false) {
        if (args.empty() || args[0] == "--help" || args[0] == "-h") { Report(HELP); return 0; }
        if (args[0] == "self-test" && args.size() == 1) { return SelfTest(); }
        if (args[0] == "measure" && args.size() == 2) { return Measure(args[1]); }
        const bool fullCycle = args[0] == "run" || StartsWith(args[0], "-");
        const std::string command = fullCycle ? "predict" : args[0];
        if (command != "predict" && command != "correct" && command != "verify" && command != "replan") { Fail("unknown command: " + command + "\n" + HELP); }
        TSnapshot s;
        TOptions options;
        TTools tools;
        TProfileOptions profile;
        s.Data.Q = 0.1;
        s.Data.Resources.Workers = int(CpuCount());
        s.Data.Resources.Ram = DefaultRam();
        s.Data.Resources.Disk = 0;
        s.Data.Resources.Bandwidth = 1e9; // the snapshot format keeps the field; nothing schedules by it
        std::string output, plan;
        bool targets = false, extractionOverride = false, diskGiven = false, analysisGiven = false, workersGiven = false;
        for (size_t i = fullCycle && args[0] != "run" ? 0 : 1; i < args.size(); ++i) {
            if (args[i] == "--help" || args[i] == "-h") { Report(HELP); return 0; }
            const auto [key, value] = ReadOption(args, i);
            extractionOverride |= key == "--root" || key == "--main-ref" || key == "--target" || key == "--gn" || key == "--ninja";
            auto number = [&]() {
                char* end = nullptr;
                double n = strtod(value.c_str(), &end);
                if (value.empty() || *end || !std::isfinite(n) || n < 0) { Fail("invalid number for " + key); }
                return n;
            };
            auto integer = [&]() {
                double n = number();
                if (n < 1 || n > 1000000 || std::floor(n) != n) { Fail("positive integer required for " + key); }
                return int(n);
            };
            // Byte budgets accept K/M/G/T suffixes (decimal) and a bare number
            // meaning gigabytes, so both 16 and 16G work.
            auto quantity = [&]() {
                const char* text = value.c_str();
                char* end = nullptr;
                double n = strtod(text, &end);
                if (end == text || !std::isfinite(n) || n <= 0) { Fail("positive size required for " + key); }
                std::string suffix;
                for (const char* p = end; *p; ++p) {
                    suffix.push_back(char(std::tolower(static_cast<unsigned char>(*p))));
                }
                double scale = 0;
                if (suffix.empty()) { scale = 1e9; }
                else if (suffix == "k" || suffix == "kb") { scale = 1e3; }
                else if (suffix == "m" || suffix == "mb") { scale = 1e6; }
                else if (suffix == "g" || suffix == "gb") { scale = 1e9; }
                else if (suffix == "t" || suffix == "tb") { scale = 1e12; }
                else { Fail("unknown size suffix for " + key + ": " + value + " (use K, M, G or T)"); }
                return n * scale;
            };
            if (key == "--output") { output = AbsPath(value); }
            else if (key == "--plan") { plan = RealPath(value); }
            else if (key == "--work-dir" && command != "verify") { options.WorkDir = AbsPath(value); }
            else if (key == "--disk" && (command == "predict" || command == "replan")) {
                s.Data.Resources.Disk = quantity();
                diskGiven = true;
            }
            else if (key == "--mode" && (command == "predict" || command == "replan")) { profile.SetMode(value); }
            else if (key == "--q" && (command == "predict" || command == "replan")) { profile.SetQ(number()); }
            else if (command != "predict") { Fail("unexpected option for " + command + ": " + key); }
            else if (key == "--baseline") { options.Baseline = RealPath(value); }
            else if (key == "--root") { s.Root = RealPath(value); }
            else if (key == "--main-ref") { options.MainRef = value; }
            else if (key == "--gn") { tools.Gn = value; }
            else if (key == "--ninja") { tools.Ninja = value; }
            else if (key == "--target") { if (!targets) { options.Targets.clear(); targets = true; } options.Targets.push_back(value); }
            else if (key == "--workers") { s.Data.Resources.Workers = integer(); workersGiven = true; }
            else if (key == "--analysis-jobs") { options.Jobs = integer(); analysisGiven = true; }
            // Ages accept s/m/h/d/w; a bare number means days, 0 never expires.
            else if (key == "--state-age") {
                char* end = nullptr;
                double n = strtod(value.c_str(), &end);
                if (value.empty() || !std::isfinite(n) || n < 0) { Fail("invalid age for " + key + ": " + value); }
                std::string suffix;
                for (const char* p = end; *p; ++p) { suffix.push_back(char(std::tolower(static_cast<unsigned char>(*p)))); }
                double scale = 86400;
                if (suffix.empty() || suffix == "d") { scale = 86400; }
                else if (suffix == "w") { scale = 604800; }
                else if (suffix == "h") { scale = 3600; }
                else if (suffix == "m") { scale = 60; }
                else if (suffix == "s") { scale = 1; }
                else { Fail("unknown age suffix for " + key + ": " + value + " (use s, m, h, d or w)"); }
                options.StateAge = int64_t(n * scale);
            }
            else if (key == "--ram-gb") { s.Data.Resources.Ram = number() * 1e9; }
            else if (key == "--action-memory-gb") { s.Data.ActionMemory = number() * 1e9; }
            else if (key == "--sample") {
                s.Sample = integer();
                if (s.Sample < 10 || s.Sample > 100) { Fail("--sample PERCENT must be between 10 and 100"); }
            }
            else { Fail("unknown option " + key); }
        }
        // One worker count for the whole run: the analysis takes it too
        // unless --analysis-jobs says otherwise. Nothing caps it from above.
        // Where it is not given it is every CPU, at most what the RAM behind
        // the run admits at the action memory prior, the ordinary compiles of
        // the project being what the count is chosen for.
        if (!workersGiven && s.Data.Resources.Ram > 0 && s.Data.ActionMemory > 0) {
            const double affordable = std::floor(s.Data.Resources.Ram / s.Data.ActionMemory);
            s.Data.Resources.Workers = std::max(1, std::min(int(CpuCount()), int(std::max(1.0, affordable))));
        }
        if (!analysisGiven) { options.Jobs = std::max(1, s.Data.Resources.Workers); }
        TCycleLock cycleLock;
        std::string cycle;
        if (command == "predict") {
            profile.Apply(s);
            if (options.Baseline.empty() || (!fullCycle && output.empty()) || s.Data.Resources.Ram <= 0 || !diskGiven ||
                s.Data.ActionMemory <= 0 || s.Data.Q > 1) {
                Fail("--baseline, --disk SIZE, positive resource limits and 0 <= q <= 1 are required; predict also needs --output\n" + std::string(HELP));
            }
            const auto state = StateDirectory(options.Baseline);
            const auto modeState = ProfileDirectory(options.Baseline, s.Mode);
            if (!IsDir(options.Baseline) || !Exists(JoinPath(options.Baseline, "build.ninja"))) {
                Fail("baseline must be an existing GN/Ninja build directory: " + options.Baseline);
            }
            if (options.WorkDir.empty()) { options.WorkDir = JoinPath(state, "cache"); }
            options.WorkDir = CacheDirectory(options.WorkDir);
            if (fullCycle) {
                cycleLock.Acquire(state);
                // One shared identity for caches, independent generations and
                // age limits for saved profiles. The root lock protects both.
                TGeneration current;
                current.Schema = ANALYSIS_VERSION;
                current.Root = BuildRoot(options.Baseline);
                if (!current.Root.empty()) {
                    try { current.Tree = TreeIdentity(current.Root, true); }
                    catch (const TError& error) { Log("no tree identity: " + std::string(error.what())); }
                }
                current.Baseline = BaselineStamp(options.Baseline);
                current.Created = WallNow();
                PrepareProfileState(state, modeState, current, options.StateAge);
                const auto request = HashHex("pch-cycle-1\n" + ShellJoin(args) + DigestFile(SelfExe()));
                auto previous = ReadPointer(JoinPath(modeState, "current.path"));
                if (!previous.empty() && ReadPointer(JoinPath(previous, "request")) == request &&
                    Exists(JoinPath(previous, "initial/snapshot.bin"))) {
                    bool resumable = false;
                    TSnapshot saved;
                    try {
                        saved = LoadPlan(JoinPath(previous, "initial"));
                        ValidateIdentity(saved);
                        (void)LoadAnalysis(JoinPath(previous, "initial"));
                        const bool unstarted = !Exists(JoinPath(previous, "initial/verification.claim")) &&
                            !Exists(JoinPath(previous, "initial/build/.ninja_log"));
                        auto result = ReadPointer(JoinPath(previous, "result.path"));
                        if (result.empty()) { result = JoinPath(previous, "corrected"); }
                        const bool correctionRetry = !Exists(JoinPath(previous, "initial/correction.claim")) ||
                            Exists(JoinPath(result, "snapshot.bin"));
                        resumable = saved.Mode == s.Mode && (unstarted || (Verified(saved) && correctionRetry));
                    } catch (const TError& error) { Log("cycle cannot resume: " + std::string(error.what())); }
                    if (resumable) { return FinishCycle(saved, previous, options.WorkDir, options.Jobs); }
                }
                if (output.empty()) {
                    auto runs = JoinPath(modeState, "runs");
                    MakeDirs(runs);
                    // A run that failed before producing a plan left only its
                    // request file; reuse that directory instead of piling up
                    // empty cycles.
                    if (!previous.empty() && IsDir(previous) && !Exists(JoinPath(previous, "initial"))) {
                        cycle = previous;
                    } else {
                        cycle = MakeTempDir(runs, "cycle-");
                    }
                } else {
                    CheckProfileOutput(options.Baseline, s.Mode, output);
                    if (Exists(output)) { Fail("a new cycle --output directory is required: " + output); }
                    MakeDirs(output);
                    cycle = RealPath(output);
                }
                output = JoinPath(cycle, "initial");
                WriteFile(JoinPath(cycle, "request"), request + "\n");
                WriteFile(JoinPath(modeState, "current.path"), cycle + "\n");
                Report("cycle (" + s.Mode + "): " + cycle);
                auto checkpoint = ReadPointer(JoinPath(state, "analysis.path"));
                if (!checkpoint.empty() && !extractionOverride) {
                    TSnapshot cached;
                    bool reusable = false;
                    try {
                        cached = LoadAnalysis(checkpoint);
                        reusable = cached.Baseline == options.Baseline && cached.Targets == options.Targets;
                    } catch (const TError& error) { Log("analysis cache cannot be reused: " + std::string(error.what())); }
                    if (reusable) {
                        cached.Data.Resources.Workers = s.Data.Resources.Workers;
                        cached.Data.Resources.Ram = s.Data.Resources.Ram;
                        cached.Data.Resources.Disk = s.Data.Resources.Disk;
                        cached.Data.Resources.Bandwidth = s.Data.Resources.Bandwidth; // the format of the plan, nothing more
                        cached.Data.Q = s.Data.Q;
                        cached.Mode = s.Mode;
                        cached.Data.ActionMemory = s.Data.ActionMemory;
                        cached.Sample = s.Sample;
                        for (auto& action : cached.Data.Actions) {
                            action.Memory = action.Phony ? 0 : cached.Data.ActionMemory;
                        }
                        if (!cached.Data.Scenarios && cached.Data.Q != 1) { Fail("no history scenarios; use --q 1"); }
                        int status = CreatePlan(cached, output, options.WorkDir, options.Jobs);
                        return status ? status : FinishCycle(cached, cycle, options.WorkDir, options.Jobs);
                    }
                }
            }
            options.Q = s.Data.Q;
            options.Ninja = tools.Ninja;
            auto checkpoint = ReadPointer(JoinPath(StateDirectory(options.Baseline), "analysis.path"));
            if (Exists(JoinPath(output, "analysis.bin"))) { checkpoint = output; }
            if (!checkpoint.empty() && (checkpoint == output || !extractionOverride)) {
                TSnapshot cached;
                bool reusable = false;
                try {
                    cached = LoadAnalysis(checkpoint);
                    reusable = cached.Baseline == options.Baseline && cached.Targets == options.Targets;
                } catch (const TError& error) { Log("analysis checkpoint not reusable: " + std::string(error.what())); }
                if (reusable) {
                    if (checkpoint != output || refreshSettings) {
                        // New request using a shared analysis checkpoint. A
                        // retry of this output instead retains resolved limits
                        // (especially available-RAM defaults) from that plan.
                        auto pools = cached.Data.Resources.Pools;
                        cached.Data.Resources = s.Data.Resources;
                        cached.Data.Resources.Pools = std::move(pools);
                        cached.Data.ActionMemory = s.Data.ActionMemory;
                        for (auto& action : cached.Data.Actions) {
                            action.Memory = action.Phony ? 0 : cached.Data.ActionMemory;
                        }
                        cached.Data.Q = s.Data.Q;
                        cached.Mode = s.Mode;
                        cached.Sample = s.Sample;
                    }
                    return CreatePlan(cached, output, options.WorkDir, options.Jobs);
                }
            }
            TWorkDir work(options.WorkDir, tools.Ninja, options.Jobs);
            TFacts facts;
            facts.Root = s.Root;
            if (!CheckBaseline(options, tools, work, facts)) { return 1; }
            s.Tree = TreeIdentity(facts.Root);
            if (!ReadTraces(options, work, facts) || !ReadHistory(options, facts)) {
                return 1;
            }
            // A GN owner whose files span several flag classes is legal here:
            // GroupTargets keeps the whole target without a PCH (class -1)
            // instead of failing the run. FlagClasses still fills the class
            // tables and reports the split; its return value only matters to
            // pipelines that cannot represent mixed owners.
            FlagClasses(options, facts);
            if (!HeaderFacts(options, work, facts)) {
                return 1;
            }
            s.Root = facts.Root;
            s.Baseline = facts.BuildDir;
            s.Directory = output;
            s.Args = ReadFile(JoinPath(s.Baseline, "args.gn"));
            s.Gn = Program(tools.Gn);
            s.Ninja = Program(tools.Ninja);
            s.GnSwitches = GnGenerationSwitches(*facts.Manifest);
            for (auto& flag : s.GnSwitches) {
                if (StartsWith(flag, "--script-executable=")) {
                    flag = "--script-executable=" + Program(flag.substr(20));
                }
            }
            s.Targets = options.Targets;
            s.Tool = DigestFile(SelfExe());
            s.Nonce = HashHex(s.Tree + std::to_string(Now()) + output);
            Extract(facts, s);
            int status = CreatePlan(s, output, options.WorkDir, options.Jobs);
            return status || !fullCycle ? status : FinishCycle(s, cycle, options.WorkDir, options.Jobs);
        }
        if (plan.empty()) { Fail("--plan is required"); }
        if (command == "replan") {
            if (output.empty()) { Fail("replan requires a new --output directory"); }
            if (!diskGiven) { Fail("replan requires --disk SIZE (e.g. --disk 16G)"); }
            const double budget = s.Data.Resources.Disk;
            s = LoadAnalysis(plan);
            s.Data.Resources.Disk = budget;
            profile.Apply(s, true);
            if (!s.Data.Scenarios && s.Data.Q != 1) { Fail("no history scenarios; use --mode global or --q 1"); }
            if (options.WorkDir.empty()) { options.WorkDir = JoinPath(StateDirectory(s.Baseline), "cache"); }
            options.WorkDir = CacheDirectory(options.WorkDir);
            return CreatePlan(s, output, options.WorkDir, options.Jobs);
        }
        s = LoadPlan(plan);
        if (command == "verify") { return Verify(s); }
        if (output.empty()) { Fail("correct requires a new --output directory"); }
        if (options.WorkDir.empty()) { options.WorkDir = JoinPath(StateDirectory(s.Baseline), "cache"); }
        return CorrectPlan(s, plan, output, CacheDirectory(options.WorkDir), options.Jobs);
    }
    namespace {
        struct TWorkflow {
            std::string Root, Cycle;
            std::map<std::string, std::vector<std::string>> Options;
        };
        template <class A> void Fields(A& a, TWorkflow& v) { a(v.Root, v.Cycle, v.Options); }

        std::string Trim(std::string text) {
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            text.erase(0, text.find_first_not_of(" \t\r\n"));
            return text;
        }

        std::string WorktreeRoot() {
            return RealPath(Trim(RunChecked({"git", "rev-parse", "--show-toplevel"})));
        }

        TRunOptions LiveRun(const std::string& root) {
            TRunOptions run;
            run.Cwd = root;
            run.CaptureStdout = run.CaptureStderr = false;
            run.OutputLabel = "gn-build";
            return run;
        }

        std::string Setting(const TWorkflow& state, const std::string& key, const std::string& fallback = {}) {
            auto it = state.Options.find(key);
            return it == state.Options.end() ? fallback : it->second.back();
        }

        // A saved setting, not an option: the worker count beside it was
        // derived from the RAM, not given, and follows the RAM.
        constexpr const char* DERIVED_WORKERS = "--workers-from-ram";

        void ResolveResourceDefaults(TWorkflow& state) {
            const std::vector<std::string> keys = {"--workers", "--ram-gb", "--action-memory-gb"};
            if (std::all_of(keys.begin(), keys.end(), [&](const auto& key) { return state.Options.count(key); })) { return; }
            TSnapshot previous;
            bool inherited = false;
            if (!state.Cycle.empty()) {
                const auto initial = JoinPath(state.Cycle, "initial");
                try {
                    if (Exists(JoinPath(initial, "snapshot.bin"))) {
                        previous = LoadPlan(initial);
                        inherited = true;
                    } else if (Exists(JoinPath(initial, "analysis.bin"))) {
                        previous = Decode<TAnalysis>(ReadFile(JoinPath(initial, "analysis.bin"))).Snapshot;
                        inherited = true;
                    }
                } catch (const TError& error) {
                    Log("cannot inherit old resource defaults: " + std::string(error.what()));
                }
            }
            auto set = [&](const std::string& key, double value) {
                if (!state.Options.count(key)) { state.Options[key] = {Format("%.17g", value)}; }
            };
            auto number = [&](const char* key) { return strtod(Setting(state, key).c_str(), nullptr); };
            // The RAM behind the run and the action memory prior come first:
            // where no worker count is given, they shape its default.
            if (!state.Options.count("--ram-gb")) {
                set("--ram-gb", (inherited ? previous.Data.Resources.Ram : DefaultRam()) / 1e9);
            }
            set("--action-memory-gb", inherited ? previous.Data.ActionMemory / 1e9 : 1);
            if (!state.Options.count("--workers")) {
                if (inherited) {
                    // The count the unfinished plan was made with. Where it came
                    // from is not recorded, so it is kept as it was.
                    set("--workers", previous.Data.Resources.Workers);
                    state.Options.erase(DERIVED_WORKERS);
                } else {
                    // The worker count is the one limit of a run, and a given
                    // one is never reduced. Left unset, it is what the machine
                    // sustains for the ordinary compiles of the project: every
                    // CPU, and no more than the RAM behind the run admits at
                    // the action memory prior. The mark tells that count from a
                    // given one: it follows the RAM where the RAM is changed.
                    const double affordable = std::floor(number("--ram-gb") / std::max(number("--action-memory-gb"), 1e-9));
                    set("--workers", std::max(1.0, double(std::min(int(CpuCount()), int(std::max(1.0, affordable))))));
                    state.Options[DERIVED_WORKERS] = {"1"};
                }
            }
        }

        // The request on top of the saved settings; what is returned is the
        // saved state with its defaults, to tell a change by. A worker count
        // derived from the RAM follows the RAM: where the RAM or the action
        // memory is supplied without a count, a count that was derived is
        // resolved again under them. A count that was given is kept, as any
        // given setting is.
        std::map<std::string, std::vector<std::string>> ApplySupplied(
            TWorkflow& state, const std::map<std::string, std::vector<std::string>>& supplied) {
            ResolveResourceDefaults(state);
            auto before = state.Options;
            for (const auto& [key, values] : supplied) { state.Options[key] = values; }
            if (supplied.count("--workers")) {
                state.Options.erase(DERIVED_WORKERS);
            } else if (state.Options.count(DERIVED_WORKERS) && (supplied.count("--ram-gb") || supplied.count("--action-memory-gb"))) {
                state.Options.erase("--workers");
                ResolveResourceDefaults(state);
            }
            return before;
        }

        struct TWorkflowResources {
            int Workers = 0, AnalysisJobs = 0; // analysis jobs default to the workers
            double Ram = 0, ActionMemory = 0;
        };

        TWorkflowResources WorkflowResources(const TWorkflow& state) {
            auto number = [&](const std::string& key, double scale = 1) {
                const auto value = Setting(state, key);
                char* end = nullptr;
                const auto n = strtod(value.c_str(), &end) * scale;
                if (end == value.c_str() || *end || !std::isfinite(n) || n <= 0) {
                    Fail(key + " requires a positive finite number");
                }
                return n;
            };
            const auto workers = number("--workers");
            if (workers > 1000000 || workers != std::floor(workers)) { Fail("--workers requires an integer between 1 and 1000000"); }
            TWorkflowResources resources;
            // One worker count for everything the run builds and checks: the
            // baseline, the measurements, the compatibility checks, the
            // model. Only the builds of the PCHs themselves are narrower.
            resources.Workers = int(workers);
            resources.AnalysisJobs = resources.Workers;
            if (state.Options.count("--analysis-jobs")) {
                const auto jobs = number("--analysis-jobs");
                if (jobs > 1000000 || jobs != std::floor(jobs)) { Fail("--analysis-jobs requires an integer between 1 and 1000000"); }
                resources.AnalysisJobs = int(jobs);
            }
            resources.Ram = number("--ram-gb", 1e9);
            resources.ActionMemory = number("--action-memory-gb", 1e9);
            return resources;
        }

        std::vector<std::string> WorkflowTargets(const TWorkflow& state) {
            auto it = state.Options.find("--target");
            return it == state.Options.end() ? std::vector<std::string>{"all", "tests"} : it->second;
        }

        void RequireIdle(const std::string& build) {
            if (!IsDir(build)) { return; }
            if (BuildRunning(build)) { Fail("Ninja is still running in " + build + "; wait before resuming"); }
        }

        void PrepareBaseline(const TWorkflow& state, const std::string& baseline) {
            RequireIdle(baseline);
            const auto oldRoot = BuildRoot(baseline);
            if (!oldRoot.empty() && RealPath(oldRoot) != state.Root) {
                Fail("baseline belongs to another worktree: " + oldRoot);
            }
            MakeDirs(baseline);
            const auto path = JoinPath(baseline, "args.gn");
            std::string args;
            TryReadFile(path, &args);
            auto values = ParseGnFile(args);
            auto mode = FindGnValue(values, "pch_mode"), trace = FindGnValue(values, "time_trace");
            const bool traceEnabled = trace && trace->Kind == TGnValue::String &&
                (trace->Text == "json" || trace->Text == "lz4" || trace->Text == "zstd");
            bool changed = !mode || mode->Kind != TGnValue::String || mode->Text != "none" ||
                !traceEnabled;
            if (changed) {
                args += "\n# pch_update baseline\npch_mode = \"none\"\n";
                if (!traceEnabled) { args += "time_trace = \"json\"\n"; }
                WriteFile(path, args);
            }
            auto gn = Program(Setting(state, "--gn", "gn"));
            auto ninja = Program(Setting(state, "--ninja", "ninja"));
            if (changed || !Exists(JoinPath(baseline, "build.ninja"))) {
                std::vector<std::string> switches;
                if (Exists(JoinPath(baseline, "build.ninja"))) {
                    TNinjaManifest manifest(baseline);
                    switches = GnGenerationSwitches(manifest);
                } else { switches = {"--script-executable=" + Program("python3")}; }
                std::vector<std::string> command = {gn, "gen", baseline, "--root=" + state.Root, "--export-compile-commands"};
                command.insert(command.end(), switches.begin(), switches.end());
                RunChecked(command, LiveRun(state.Root));
            }
            const auto resources = WorkflowResources(state);
            Report(Format("  Baseline: -j %d", resources.Workers));
            std::vector<std::string> command = {ninja, "-C", baseline, "-j", std::to_string(resources.Workers)};
            const auto targets = WorkflowTargets(state);
            command.insert(command.end(), targets.begin(), targets.end());
            const auto tool = DigestFile(SelfExe());
            RunChecked(command, LiveRun(state.Root));
            if (tool != DigestFile(SelfExe())) { Fail("baseline rebuilt this executable; repeat the same command with the new tool"); }
        }

        std::string PchName(size_t id) { return "group_" + std::to_string(id); }
        std::string PchOutput(size_t id) { return "gen/build/gn/pch/global/pch_" + PchName(id) + ".pch"; }

        // Canonical classes preserve ordered macro/include options. GN expands
        // only the two explicitly introduced path variables, never user '$'.
        std::string ExportFlag(const TSnapshot& s, std::string flag, bool path) {
            if (path && !IsAbs(flag) && !StartsWith(flag, "<out>") && !StartsWith(flag, "<root>")) {
                flag = "<root>/" + flag;
            }
            Replace(flag, s.Baseline, "<out>");
            Replace(flag, s.Root, "<root>");
            auto quoted = GnQuote(flag);
            Replace(quoted, "<root>", "${_pch_export_root}");
            Replace(quoted, "<out>", "${_pch_export_out}");
            return quoted;
        }

        std::string ExportFlags(const TSnapshot& s, int compilerClass) {
            const auto& flags = s.Flags.at(compilerClass);
            std::string result = "    compiler_flags = [\n";
            bool path = false;
            for (size_t i = 1; i < flags.size(); ++i) {
                auto flag = flags[i];
                if (flag == "-Xclang") {
                    result += "      \"-Xclang\",\n";
                    continue; // preserve a pending path operand across its wrapper
                }
                // The Linux GN toolchain already supplies this leading option.
                // Keep any later per-target sysroot override in its original order.
                if (i == 1 && StartsWith(flag, "--sysroot=")) { continue; }
                if (i == 1 && flag == "--sysroot" && i + 1 < flags.size()) { ++i; continue; }
                if (!path && StartsWith(flag, "-I") && flag.size() > 2) {
                    result += "      \"-I\" + " + ExportFlag(s, flag.substr(2), true) + ",\n";
                    continue;
                }
                result += "      " + ExportFlag(s, flag, path) + ",\n";
                path = !path && (flag == "-I" || flag == "-isystem" || flag == "-iquote" ||
                    flag == "-include" || flag == "-isysroot");
            }
            if (path) { Fail("incomplete path option in compiler class"); }
            return result + "    ]\n";
        }

        std::string ExportPaths() {
            // rebase_path("//") ends in '/', which would double the separator
            // already present in compiler options such as -ffile-prefix-map.
            return "_pch_export_root = get_path_info(rebase_path(\"//BUILD.gn\"), \"dir\")\n"
                "_pch_export_out = rebase_path(root_build_dir)\n";
        }

        std::map<std::string, std::string> DirectoryFiles(const std::string& directory) {
            std::map<std::string, std::string> files;
            if (!IsDir(directory)) { return files; }
            for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
                if (entry.is_regular_file()) {
                    auto path = entry.path().string();
                    files[RelPath(path, directory)] = DigestFile(path);
                }
            }
            return files;
        }

        void CopyDirectory(const std::string& from, const std::string& to) {
            MakeDirs(DirName(to));
            RunChecked({"cp", "-a", "--", from, to});
        }

        void DiscardWorkflow(const std::string& state) {
            // An atomic rename separates committed work from an unfinished
            // run. A killed recursive deletion is resumed without reading any
            // partially deleted snapshots or reinstalling the groups.
            const auto garbage = state + ".cleanup";
            if (Exists(state)) { std::filesystem::rename(state, garbage); }
            RemoveTree(garbage);
            Report("PCH workflow complete; temporary build state removed.");
            if (!OutputDirectory().empty()) { Report("Report and logs: " + OutputDirectory()); }
        }

        void RequireWorkflowIdle(const std::string& cycle) {
            RequireIdle(JoinPath(cycle, "global"));
            for (const char* name : {"initial", "corrected"}) {
                const auto plan = JoinPath(cycle, name);
                if (!IsDir(plan)) { continue; }
                RequireIdle(JoinPath(plan, "build"));
                TCycleLock lock;
                lock.Acquire(JoinPath(plan, "verification.lock"));
            }
        }

        struct TRestartWorkflow {
            TWorkflow Request;
            bool Settings = false, Analysis = false;
        };
        template <class A> void Fields(A& a, TRestartWorkflow& v) { a(v.Request, v.Settings, v.Analysis); }

        void RestartWorkflow(const std::string& state, TRestartWorkflow restart) {
            auto& workflow = restart.Request;
            RequireWorkflowIdle(workflow.Cycle);
            // The request is journaled before removing stale results. Repeating
            // an interrupted reset leaves the same one working directory.
            WriteFile(JoinPath(state, "restart.bin"), Encode(restart));
            const auto initial = JoinPath(workflow.Cycle, "initial");
            if (IsDir(initial)) {
                for (const auto& name : ListDir(initial)) {
                    if (name == "analysis.bin" && !restart.Analysis) { continue; }
                    if (name == "selection.bin") { continue; } // keyed by all selector inputs
                    RemoveTree(JoinPath(initial, name));
                }
            }
            if (restart.Settings) { WriteFile(JoinPath(initial, "settings.changed"), "changed\n"); }
            for (const char* name : {"corrected", "export", "export.ready", "global", "original-global", "original.ready"}) {
                RemoveTree(JoinPath(workflow.Cycle, name));
            }
            WriteFile(JoinPath(state, "analysis.path"), initial + "\n");
            WriteFile(JoinPath(state, "state.bin"), Encode(workflow));
            RemoveFile(JoinPath(state, "restart.bin"));
        }

        void ReleaseInitialArtifacts(const TSnapshot& initial, const TSnapshot& corrected) {
            const auto marker = JoinPath(initial.Directory, "artifacts.released");
            if (Exists(marker)) { return; }
            RequireWorkflowIdle(DirName(initial.Directory));
            // The corrected snapshot contains the calibrated model. Initial
            // objects are no longer needed, but its graph/header/config inputs
            // still fingerprint that snapshot and must remain intact.
            std::set<std::string> inputs;
            for (const auto* plan : {&initial, &corrected}) {
                for (const auto& [path, digest] : plan->Inputs) { inputs.insert(path); }
            }
            const auto build = JoinPath(initial.Directory, "build");
            if (IsDir(build)) {
                for (const auto& entry : std::filesystem::recursive_directory_iterator(build)) {
                    if (!entry.is_regular_file()) { continue; }
                    const auto path = entry.path().string(), extension = entry.path().extension().string();
                    if ((extension == ".o" || extension == ".pch" || extension == ".dwo") && !inputs.count(path)) {
                        RemoveFile(path);
                    }
                }
            }
            WriteFile(marker, "released\n");
        }

        std::string InstallationDirectory(const std::string& root, const std::string& cycle) {
            const auto directory = ReadPointer(JoinPath(cycle, "installing"));
            const auto parent = DirName(JoinPath(root, GLOBAL_DIR));
            if (DirName(directory) == parent && StartsWith(BaseName(directory), ".pch-install-")) { return directory; }
            return {};
        }

        void FinishInstallation(const std::string& root, const std::string& cycle) {
            const auto directory = InstallationDirectory(root, cycle);
            if (!directory.empty()) { RemoveTree(directory); }
            RemoveFile(JoinPath(cycle, "installing"));
        }

        void RecoverApply(const std::string& root, const std::string& cycle) {
            if (!Exists(JoinPath(cycle, "installing"))) { return; }
            RequireIdle(JoinPath(cycle, "global"));
            auto old = JoinPath(cycle, "original-global"), bundle = JoinPath(cycle, "export");
            auto current = JoinPath(root, GLOBAL_DIR);
            if (!IsDir(old)) { Fail("apply backup is missing: " + old); }
            const auto before = DirectoryFiles(old), after = DirectoryFiles(bundle);
            // Do not overwrite edits made while a transaction was suspended.
            if (IsDir(current)) {
                const auto live = DirectoryFiles(current);
                if (live != before && live != after) { Fail("PCH files changed during suspended apply"); }
            }
            auto transaction = InstallationDirectory(root, cycle);
            if (transaction.empty()) {
                transaction = MakeTempDir(DirName(current), ".pch-install-");
                WriteFile(JoinPath(cycle, "installing"), transaction + "\n");
            }
            // Recovery itself can be interrupted. Neither installation nor
            // rollback copies bytes into the live directory: only complete
            // directories reach it via same-filesystem rename.
            const auto restored = JoinPath(transaction, "restored");
            const auto discarded = JoinPath(transaction, "discarded");
            RemoveTree(restored);
            CopyDirectory(old, restored);
            RemoveTree(discarded);
            if (Exists(current)) { std::filesystem::rename(current, discarded); }
            std::filesystem::rename(restored, current);
            FinishInstallation(root, cycle);
        }

        void ExportGroups(const TSnapshot& s, const std::string& cycle) {
            const auto output = JoinPath(cycle, "export");
            const auto ready = JoinPath(cycle, "export.ready");
            if (Exists(ready)) {
                auto expected = Decode<std::map<std::string, std::string>>(ReadFile(ready));
                if (expected != DirectoryFiles(output)) { Fail("exported PCH files changed"); }
                return;
            }
            // Resolve actual producer labels from the unchanged baseline graph;
            // reuse the same public *_codegen indirection as the legacy writer.
            TOptions options;
            options.Baseline = s.Baseline;
            options.Targets = s.Targets;
            options.Ninja = s.Ninja;
            TTools tools;
            tools.Gn = s.Gn;
            tools.Ninja = s.Ninja;
            TWorkDir work(CacheDirectory(JoinPath(StateDirectory(s.Baseline), "cache")), s.Ninja, std::max(1, s.Data.Resources.Workers));
            TFacts facts;
            facts.Root = s.Root;
            if (!CheckBaseline(options, tools, work, facts)) { Fail("baseline changed before export"); }
            std::map<std::string, std::string> codegen;
            for (const auto& [label, deps] : facts.DepsOf) {
                if (!EndsWith(label, "_codegen")) { continue; }
                for (const auto& dep : deps) {
                    if (label.substr(0, label.find(':')) == dep.substr(0, dep.find(':'))) {
                        auto [it, added] = codegen.emplace(dep, label);
                        if (!added && label < it->second) { it->second = label; }
                    }
                }
            }
            RemoveTree(output);
            MakeDirs(output);
            for (const auto* name : {"BUILD.gn", "pch_flags.gni", "enum_serialization.gni"}) {
                WriteFile(JoinPath(output, name), ReadFile(JoinPath(JoinPath(s.Root, GLOBAL_DIR), name)));
            }
            std::string groups = "# Generated by pch_update; exact GN-owner assignments.\n" +
                ExportPaths() + "pch_groups = [\n";
            const auto measuredBuild = JoinPath(s.Directory, "build");
            TNinjaDeps measuredDeps;
            if (!s.Layout.Pchs.empty()) { measuredDeps = ReadNinjaDeps(JoinPath(measuredBuild, ".ninja_deps")); }
            for (size_t g = 0; g < s.Layout.Pchs.size(); ++g) {
                const auto& pch = s.Layout.Pchs[g];
                std::vector<std::string> targets;
                for (size_t t = 0; t < s.Data.Targets.size(); ++t) {
                    if (s.Layout.Assignment[t] == int(g)) {
                        if (!s.Data.Targets[t].PchEligible) { Fail("cannot export an owner without a GN PCH hook"); }
                        targets.push_back(s.Data.Targets[t].Label);
                    }
                }
                std::set<std::string> deps;
                auto addProducer = [&](const std::string& output) {
                    auto producer = facts.LabelOfOutput.find(output);
                    if (producer == facts.LabelOfOutput.end()) { Fail("no GN producer for " + output); }
                    auto publicLabel = codegen.find(producer->second);
                    deps.insert(publicLabel == codegen.end() ? producer->second : publicLabel->second);
                };
                for (int u : pch.Units) {
                    for (const auto& header : s.Data.Units.at(u).Headers) {
                        if (!StartsWith(header, "<out>/")) { continue; }
                        addProducer(header.substr(6));
                    }
                }
                // Export all actual generated inputs, including cheap headers
                // omitted by Clang's time trace and headers of ancestor PCHs.
                auto actual = measuredDeps.Deps.find("pch/" + std::to_string(g) + ".pch");
                if (actual == measuredDeps.Deps.end()) { Fail("missing measured PCH dependency record; repeat --correct"); }
                for (int dep : actual->second) {
                    auto relative = RelPath(NormPath(JoinPath(measuredBuild, measuredDeps.Paths.at(dep))), measuredBuild);
                    if (facts.Manifest->Producer(relative) >= 0) { addProducer(relative); }
                }
                groups += "  {\n    name = " + GnQuote(PchName(g)) + "\n";
                if (pch.Parent >= 0) { groups += "    parent = " + GnQuote(PchName(pch.Parent)) + "\n"; }
                groups += GnList("targets", targets, 4) + GnList("deps", {deps.begin(), deps.end()}, 4);
                groups += "    defines = []\n    include_dirs = []\n    cflags = []\n" + ExportFlags(s, pch.Class) + "  },\n";
                WriteFile(JoinPath(JoinPath(output, PchName(g)), "pch.cc"), PchHeader(s.Data, pch));
            }
            groups += "]\n";
            // An empty layout still imports cleanly without unused variables.
            groups += "not_needed([ \"_pch_export_root\", \"_pch_export_out\" ])\n";
            WriteFile(JoinPath(output, "groups.gni"), groups);
            auto files = DirectoryFiles(output);
            WriteFile(ready, Encode(files));
        }

        std::vector<std::string> DeploymentFlags(const TCompile& compile, const std::string& build,
                                                 const std::string& root, bool producer,
                                                 std::vector<std::string>& pchs) {
            std::vector<std::string> argv;
            for (size_t i = 0; i < compile.Argv.size(); ++i) {
                const auto& flag = compile.Argv[i];
                if (flag == "-include-pch" && i + 1 < compile.Argv.size()) {
                    pchs.push_back(RelPath(NormPath(JoinPath(build, compile.Argv[++i])), build));
                } else if (flag == "-isysroot" && i + 1 < compile.Argv.size() &&
                           NormPath(JoinPath(build, compile.Argv[i + 1])) == root) {
                    ++i;
                } else if (flag == "-Xclang" && i + 1 < compile.Argv.size() &&
                           (compile.Argv[i + 1] == "-module-file-deps" ||
                            (producer && (compile.Argv[i + 1] == "-fno-pch-timestamp" ||
                                          compile.Argv[i + 1] == "-fmodule-file-home-is-cwd")))) {
                    ++i;
                } else if (producer && (flag == "-fpch-instantiate-templates" || flag == "-relocatable-pch")) {
                    continue;
                } else { argv.push_back(flag); }
            }
            return Canonical(CompileFlags(argv, compile.Source), build, root);
        }

        void CheckDeployment(const TSnapshot& s, const std::string& build) {
            auto compiles = ReadCompiles(build);
            std::set<std::string> checked;
            for (const auto& file : s.Data.Files) {
                const auto leaf = s.Layout.Assignment.at(file.Target);
                const auto output = s.Data.Actions.at(file.Action).Output;
                auto it = compiles.find(output);
                if (it == compiles.end()) {
                    if (leaf >= 0) { Fail("assigned compilation missing from GN global graph: " + output); }
                    continue; // bootstrap tools can move to clang_no_global_pch
                }
                std::vector<std::string> pchs;
                auto flags = DeploymentFlags(it->second, build, s.Root, false, pchs);
                const auto expected = leaf < 0 ? std::vector<std::string>{} : std::vector<std::string>{PchOutput(leaf)};
                if (pchs != expected) { Fail("GN PCH assignment differs for " + output); }
                if (leaf >= 0 && flags != s.Flags.at(file.Class)) { Fail("GN consumer compiler class differs for " + output); }
                checked.insert(output);
            }
            for (size_t g = 0; g < s.Layout.Pchs.size(); ++g) {
                const auto& pch = s.Layout.Pchs[g];
                const auto output = "obj/build/gn/pch/global/" + PchName(g) + "/pch_" + PchName(g) + "_compile.pch.cc.o";
                auto it = compiles.find(output);
                if (it == compiles.end()) { Fail("GN PCH producer missing: " + output); }
                std::vector<std::string> parents;
                auto flags = DeploymentFlags(it->second, build, s.Root, true, parents);
                const auto expected = pch.Parent < 0 ? std::vector<std::string>{} : std::vector<std::string>{PchOutput(pch.Parent)};
                if (parents != expected || flags != s.Flags.at(pch.Class)) { Fail("GN PCH producer flags/parent differ: " + output); }
                checked.insert(output);
            }
            for (const auto& [output, compile] : compiles) {
                if (checked.count(output)) { continue; }
                if (std::find(compile.Argv.begin(), compile.Argv.end(), "-include-pch") != compile.Argv.end()) {
                    Fail("unexpected GN PCH consumer outside the plan: " + output);
                }
            }
        }

        int ApplyGroups(TSnapshot& s, const std::string& cycle) {
            ValidateIdentity(s);
            if (!s.Corrected || !Verified(s)) { Fail("run --correct before --apply"); }
            std::string errors, actionErrors;
            const auto observations = ReadObservations(s, errors, actionErrors);
            MeasurementSummary(s, observations, errors, actionErrors);
            double bytes = 0;
            for (size_t g = 0; g < s.Layout.Pchs.size(); ++g) { bytes += observations.at(s.Data.Actions.size() + g).Bytes; }
            if (bytes > s.Data.Resources.Disk) { Fail("measured PCH size exceeds the disk budget"); }
            RequireIdle(s.Baseline);
            RequireIdle(JoinPath(cycle, "global"));
            ExportGroups(s, cycle);
            const auto current = JoinPath(s.Root, GLOBAL_DIR), original = JoinPath(cycle, "original-global");
            if (!Exists(JoinPath(cycle, "original.ready"))) {
                RemoveTree(original);
                CopyDirectory(current, original);
                WriteFile(JoinPath(cycle, "original.ready"), "ready\n");
            }
            const auto transaction = MakeTempDir(DirName(current), ".pch-install-");
            WriteFile(JoinPath(cycle, "installing"), transaction + "\n");
            try {
                const auto staged = JoinPath(transaction, "new");
                CopyDirectory(JoinPath(cycle, "export"), staged);
                std::filesystem::rename(current, JoinPath(transaction, "previous"));
                std::filesystem::rename(staged, current);
                const auto build = JoinPath(cycle, "global");
                MakeDirs(build);
                const auto args = s.Args + "\npch_mode = \"global\"\ntime_trace = \"none\"\n";
                std::string previous;
                if (!TryReadFile(JoinPath(build, "args.gn"), &previous) || previous != args) {
                    WriteFile(JoinPath(build, "args.gn"), args);
                }
                std::vector<std::string> gn = {s.Gn, "gen", build, "--root=" + s.Root, "--export-compile-commands"};
                gn.insert(gn.end(), s.GnSwitches.begin(), s.GnSwitches.end());
                RunChecked(gn, LiveRun(s.Root));
                CheckDeployment(s, build);
                // The parallelism every forecast and measurement of this run used.
                std::vector<std::string> command = {s.Ninja, "-C", build, "-j", std::to_string(s.Data.Resources.Workers)};
                command.insert(command.end(), s.Targets.begin(), s.Targets.end());
                RunChecked(command, LiveRun(s.Root));
                // Commit marker precedes clearing the transaction journal. A
                // killed process at either boundary is resolved on the next run.
                WriteFile(JoinPath(cycle, "applied.tree"), TreeIdentity(s.Root) + "\n");
                FinishInstallation(s.Root, cycle);
                Report("applied and built GN global PCH groups: " + current);
                Diagnostic([&] {
                    PublishReports(s);
                    const auto text = Format("Installed %zu PCHs; measured PCH artifacts %.3f / %.3f GB.\n",
                        s.Layout.Pchs.size(), bytes / 1e9, s.Data.Resources.Disk / 1e9) +
                        "Full GN build and linking: passed. Tests were built, not executed.\n"
                        "A speedup over the baseline or previous PCH layout has not been measured.\nInstalled groups: " + current + "\n";
                    Report(text);
                    WriteFile(JoinPath(ReportRoot(s), "summary.txt"), ReadFile(JoinPath(ReportRoot(s), "summary.txt")) + "\n" + text);
                    WriteFile(JoinPath(ReportRoot(s), "final-result.json"), "{\"schema\":1,\"status\":\"applied-and-built\",\"pchs\":" +
                        std::to_string(s.Layout.Pchs.size()) + ",\"measured_pch_bytes\":" + Format("%.17g", bytes) +
                        ",\"tests_executed\":false,\"measured_speedup\":null,\"installed_groups\":" + JsonText(current) + "}\n");
                });
                return 0;
            } catch (...) {
                RecoverApply(s.Root, cycle);
                throw;
            }
        }

        // All five include layers, in the order the emitted PCH header uses.
        void LayerOrderSelfTest() {
            auto check = [](bool ok, const std::string& text) { if (!ok) { Fail("layer self-test: " + text); } };
            TData data;
            data.Units.resize(7);
            data.Units[0].Include = "<vector>";
            data.Units[0].Headers = {"contrib/libs/cxxsupp/libcxx/include/vector"};
            data.Units[1].Include = "<sysheader.h>";
            data.Units[1].Headers = {"<sysroot>/usr/include/sysheader.h"};
            data.Units[2].Include = "<contrib/lib.h>";
            data.Units[2].Headers = {"contrib/libs/some/lib.h"};
            data.Units[3].Include = "\"util/directory/directory.h\"";
            data.Units[3].Headers = {"util/directory/directory.h"};
            data.Units[4].Include = "\"library/cpp/string_utils/generic.h\"";
            data.Units[4].Headers = {"library/cpp/string_utils/generic.h"};
            data.Units[5].Include = "\"ydb/core/base/row.h\"";
            data.Units[5].Headers = {"ydb/core/base/row.h"};
            data.Units[6].Include = "\"gen/protos/foo.pb.h\"";
            data.Units[6].Headers = {"<out>/gen/protos/foo.pb.h"};
            TPch pch;
            pch.Roots = {6, 5, 4, 3, 2, 1, 0}; // deliberately shuffled, generated first
            check(LayeredRoots(data, pch) == std::vector<int>({1, 0, 2, 4, 3, 5, 6}),
                  "sysroot/libc++, contrib, util/library, project and generated roots are included in layer order");
            const auto header = PchHeader(data, pch);
            check(header.find("#include <vector>") != std::string::npos &&
                  header.find("#include <vector>") < header.find("\"ydb/core/base/row.h\"") &&
                  header.find("\"ydb/core/base/row.h\"") < header.find("\"gen/protos/foo.pb.h\""),
                  "the generated root is included last, after project headers");
        }

        void LinkPreflightSelfTest(const std::string& parent, const std::string& compiler) {
            auto check = [](bool ok, const std::string& message) { if (!ok) { Fail("link self-test: " + message); } };
            TSnapshot s;
            s.Root = RealPath(".");
            s.Baseline = JoinPath(parent, "link-baseline");
            s.Directory = JoinPath(parent, "link-plan");
            s.Ninja = Program("ninja");
            s.Data.Classes = s.Data.Scenarios = 1;
            s.Data.Q = 1;
            s.Data.ActionMemory = 1e6;
            s.Data.Resources.Workers = 2;
            s.Data.Resources.Ram = s.Data.Resources.Disk = s.Data.Resources.Bandwidth = 1e9;
            const auto base = JoinPath(s.Baseline, "base.h");
            const auto leaf = JoinPath(s.Baseline, "leaf.h");
            const auto broken = JoinPath(s.Baseline, "broken.h");
            const auto safeLeaf = JoinPath(s.Baseline, "safe_leaf.h");
            WriteFile(base, "#pragma once\nstruct Base {};\n");
            WriteFile(leaf, "#pragma once\nextern int Provided();\nstatic const int Registration = Provided();\n");
            WriteFile(broken, "#pragma once\n#error broken root\n");
            WriteFile(safeLeaf, "#pragma once\nstruct SafeLeaf {};\n");
            s.Data.Units.resize(4);
            for (int u = 0; u < 4; ++u) {
                s.Data.Units[u].Include = "\"" + (u == 0 ? base : u == 1 ? leaf : u == 2 ? broken : safeLeaf) + "\"";
                s.Data.Units[u].Parse = .01;
                s.Data.Units[u].Bytes = 100;
                s.Data.Units[u].Stable = true;
            }
            s.Data.Units[1].Safe = false;
            const std::vector<std::string> flags = {compiler, "-std=c++20", "-O2", "-fPIC"};
            s.Flags = {Canonical(flags, s.Baseline, s.Root)};
            TRunOptions run;
            run.Cwd = s.Baseline;
            std::ostringstream commands;
            commands << "[";
            for (int f = 0; f < 3; ++f) {
                const auto source = JoinPath(s.Baseline, "file" + std::to_string(f) + ".cpp");
                const auto output = "file" + std::to_string(f) + ".o";
                WriteFile(source, "int Function" + std::to_string(f) + "() { return 1; }\n");
                auto argv = flags;
                argv.insert(argv.end(), {"-c", source, "-o", output});
                RunChecked(argv, run);
                commands << (f ? "," : "");
                commands << "{\"directory\":" << std::quoted(s.Baseline) << ",\"file\":" << std::quoted(source)
                         << ",\"command\":" << std::quoted(ShellJoin(argv)) << '}';
                TAction action;
                action.Output = output;
                action.Time = 1;
                action.Memory = 1e6;
                s.Data.Actions.push_back(action);
                s.Data.Files.push_back({f, 0, source, {{0, .01}, {1, .01}, {2, .01}}});
            }
            // A base-only owner keeps the whole family expensive to evict, so
            // under a tight budget the real leaf is the cheapest group to drop
            // regardless of the toolchain's base-to-delta artifact size ratio.
            {
                const auto source = JoinPath(s.Baseline, "file3.cpp");
                const auto output = "file3.o";
                WriteFile(source, "int Function3() { return 1; }\n");
                auto argv = flags;
                argv.insert(argv.end(), {"-c", source, "-o", output});
                RunChecked(argv, run);
                commands << ",{\"directory\":" << std::quoted(s.Baseline) << ",\"file\":" << std::quoted(source)
                         << ",\"command\":" << std::quoted(ShellJoin(argv)) << '}';
                TAction action;
                action.Output = output;
                action.Time = 1;
                action.Memory = 1e6;
                s.Data.Actions.push_back(action);
                s.Data.Files.push_back({3, 0, source, {{0, .5}}});
            }
            commands << "]";
            WriteFile(JoinPath(s.Baseline, "compile_commands.json"), commands.str());
            GroupTargets(s.Data, {"//allowed:owner", "//allowed:owner", "//missing:owner", "//baseonly:owner"});
            const auto provider = JoinPath(s.Baseline, "provider.cpp");
            auto buildProvider = [&](const std::string& text) {
                WriteFile(provider, text);
                auto argv = flags;
                argv.insert(argv.end(), {"-shared", "-nostdlib", "-fuse-ld=lld", provider, "-o", "provider.so"});
                RunChecked(argv, run);
            };
            buildProvider("int Provided() { return 7; }\n");
            WriteFile(JoinPath(s.Baseline, "provider.so.TOC"), "_Z8Providedv T\n");
            WriteFile(JoinPath(s.Baseline, "build.ninja"),
                "rule cxx\n  command = unused\nrule link\n  command = unused\nrule solink\n  command = unused\n"
                "build file0.o: cxx file0.cpp\nbuild file1.o: cxx file1.cpp\nbuild file2.o: cxx file2.cpp\n"
                "build file3.o: cxx file3.cpp\n"
                "build provider.so provider.so.TOC: solink provider.cpp\n"
                "build allowed: link file0.o | provider.so.TOC\nbuild helper: link file1.o | provider.so.TOC\n"
                "build missing: link file2.o\n");
            TNinjaManifest manifest(s.Baseline);
            s.LinkInputs = CollectLinkInputs(manifest,
                {{"file0.o", "file0.o"}, {"file1.o", "file1.o"}, {"file2.o", "file2.o"}, {"file3.o", "file3.o"}});
            TLayout candidate;
            candidate.Bases = 1;
            candidate.Pchs.resize(3);
            for (auto& pch : candidate.Pchs) { pch.Class = pch.Representative = 0; }
            candidate.Pchs[0].Roots = candidate.Pchs[0].Units = {0, 2}; // the base carries a broken root
            candidate.Pchs[1].Roots = {1}; // needs the provider on every final link
            candidate.Pchs[1].Units = {0, 1, 2};
            candidate.Pchs[1].Parent = 0;
            candidate.Pchs[2].Roots = {3}; // a sibling leaf without link requirements
            candidate.Pchs[2].Units = {0, 2, 3};
            candidate.Pchs[2].Parent = 0;
            candidate.Assignment = {1, 0, 1}; // allowed, baseonly, missing
            s.Layout = candidate;
            const auto cache = JoinPath(parent, "link-cache");
            std::vector<uint64_t> measured;
            Preflight(s, cache, {}, 1, &measured);
            check(s.Layout.Assignment == std::vector<int>({1, 0, 0}),
                  "real leaf is kept only by the owner whose every final link has the provider");
            check(s.Layout.Pchs.size() == 2 && s.Layout.Pchs[0].Roots == std::vector<int>({0}),
                  "bisecting a failing base drops the broken root and keeps the base alive");
            check(std::find(s.Layout.Pchs[0].Units.begin(), s.Layout.Pchs[0].Units.end(), 2) == s.Layout.Pchs[0].Units.end() &&
                  std::find(s.Layout.Pchs[1].Units.begin(), s.Layout.Pchs[1].Units.end(), 2) == s.Layout.Pchs[1].Units.end(),
                  "the broken unit disappears from the base and from the delta built on it");
            check(ReadFile(JoinPath(s.Directory, "compatibility.tsv")).find("bisected: dropped 1 root") != std::string::npos,
                  "bisecting a failing base is reported with the dropped root");
            check(ReadFile(JoinPath(s.Directory, "compatibility.tsv")).find("missing: no known provider") != std::string::npos,
                  "target fallback explains the failing final link");
            check(measured.size() == 2 && measured[0] && measured[1], "both real artifact sizes are available");
            // A disk eviction may hand an owner over to a sibling delta: that
            // leaf's link requirements must be checked for its new owner. The
            // owner without the provider on its link line may not inherit the
            // leaf that needs one, and falls back to the base instead.
            auto fullBudget = s.Data.Resources.Disk;
            s.Data.Resources.Disk = measured[0] + measured[1]; // the base and the provider leaf fit; the safe leaf does not
            s.Layout = candidate;
            s.Layout.Assignment[2] = 2; // missing: on the safe leaf
            Preflight(s, cache);
            check(s.Layout.Assignment == std::vector<int>({1, 0, 0}) && s.Layout.Pchs.size() == 2,
                  "a sibling fallback is link-checked for its new owner");
            check(ReadFile(JoinPath(s.Directory, "compatibility-targets.tsv")).find("no known provider") != std::string::npos,
                  "the rejected sibling explains the failing final link of its prospective owner");
            s.Data.Resources.Disk = fullBudget;
            s.Data.Resources.Disk = measured[0];
            s.Layout = candidate;
            s.ProbeSizes.clear();
            Preflight(s, cache);
            check(s.Layout.Assignment == std::vector<int>({0, 0, 0}) && s.Layout.Pchs.size() == 1 &&
                  ReadFile(JoinPath(s.Directory, "disk-budget.tsv")).find("retained_bytes=" + std::to_string(measured[0])) != std::string::npos,
                  "one real measured base fits exactly; evict the real leaf and verify the owner's parent");
            // Both leaves were built and weighed before the fit evicted them:
            // their sizes stay with the plan for the calibration, with the
            // content they were built with and of the parent they were built
            // on, and they survive the snapshot.
            check(s.ProbeSizes.size() == 2 && s.ProbeSizes[0].Bytes > 0 && s.ProbeSizes[1].Bytes > 0 &&
                      s.ProbeSizes[0].Pch.Class == 0 && s.ProbeSizes[0].Pch.Parent == 0 && s.ProbeSizes[1].Pch.Parent == 0 &&
                      s.ProbeSizes[0].Parent.Units == s.Layout.Pchs[0].Units && s.ProbeSizes[1].Parent.Units == s.Layout.Pchs[0].Units &&
                      s.ProbeSizes[0].Pch.Roots == std::vector<int>({1}) && s.ProbeSizes[1].Pch.Roots == std::vector<int>({3}) &&
                      std::find(s.ProbeSizes[0].Pch.Units.begin(), s.ProbeSizes[0].Pch.Units.end(), 2) == s.ProbeSizes[0].Pch.Units.end(),
                  "the probes the measured fit evicted keep their sizes and the content they were built with");
            const auto reloaded = Decode<TSnapshot>(Encode(s));
            check(reloaded.ProbeSizes.size() == 2 && reloaded.ProbeSizes[0].Bytes == s.ProbeSizes[0].Bytes &&
                      reloaded.ProbeSizes[1].Pch.Units == s.ProbeSizes[1].Pch.Units && reloaded.ProbeSizes[1].Parent.Units == s.ProbeSizes[1].Parent.Units,
                  "the probe sizes survive the snapshot");
            s.Data.Resources.Disk = fullBudget;
            s.Layout = candidate;
            auto helper = s.LinkInputs.Targets.at("file1.o").front();
            s.LinkInputs.Lines[helper].Inputs.erase("provider.so");
            Preflight(s, cache);
            check(s.Layout.Assignment == std::vector<int>({0, 0, 0}),
                  "one helper's missing provider moves the whole owner to its parent");
            s.LinkInputs.Lines[helper].Inputs.insert("provider.so");
            s.LinkInputs.Targets.erase("file1.o");
            s.Layout = candidate;
            Preflight(s, cache);
            check(s.Layout.Assignment == std::vector<int>({0, 0, 0}) &&
                  ReadFile(JoinPath(s.Directory, "compatibility.tsv")).find("unknown final links") != std::string::npos,
                  "missing helper link metadata is not accepted as success");
            s.LinkInputs.Targets["file1.o"] = {helper};
            const auto identity = BaselineIdentity(s);
            buildProvider("int DifferentProvider() { return 12345; }\n");
            check(identity != BaselineIdentity(s), "changed library invalidates baseline identity without a Ninja log update");
            s.Layout = candidate;
            Preflight(s, cache);
            check(s.Layout.Assignment == std::vector<int>({0, 0, 0}) &&
                  ReadFile(JoinPath(s.Directory, "compatibility.tsv")).find("unknown provider") != std::string::npos,
                  "provider cache notices replaced artifacts and rejects unknown symbols");
            // An initial base measured larger than the room the plan leaves for
            // it is trimmed to that room instead of taking its whole family to
            // NONE at the measured fit: the thin tail of its roots goes, in the
            // order Pick accepted them, whatever layer of the include order
            // its headers belong to. A plan over the budget by estimate because
            // of spare deltas still trims, only the bases being mandatory. A
            // corrected base is frozen: over the budget it is an error, as before.
            {
                const auto big = JoinPath(s.Baseline, "big.h");
                std::string text = "#pragma once\n";
                for (int i = 0; i < 50000; ++i) { text += "struct Big" + std::to_string(i) + " { int a; double b; };\n"; }
                WriteFile(big, text);
                s.Data.Units.resize(5);
                s.Data.Units[0].Headers = {"util/directory/base.h"}; // layer 2
                s.Data.Units[4].Include = "\"" + big + "\"";
                s.Data.Units[4].Headers = {"contrib/libs/big/big.h"}; // layer 1: first in the include order
                s.Data.Units[4].Parse = .5;
                s.Data.Units[4].Bytes = 100; // the estimate knows nothing of the content
                s.Data.Units[4].Stable = true;
                TLayout topHeavy;
                topHeavy.Bases = 1;
                TPch pch;
                pch.Class = pch.Representative = 0;
                pch.Roots = pch.Units = {0, 4}; // the dense root first, as Pick accepts them
                // A spare delta the estimate makes large: the plan is over the
                // budget by estimate, and its base still has to fit.
                TPch spare;
                spare.Class = spare.Representative = 0;
                spare.Parent = 0;
                spare.Roots = {3}; // the safe leaf, which the base-only owner saves with below
                spare.Units = {0, 3};
                topHeavy.Pchs = {pch, spare};
                topHeavy.Assignment = {0, 1, 0};
                const auto spareBytes = s.Data.Units[3].Bytes;
                s.Data.Units[3].Bytes = 1e6; // far over the budget below, within the RAM model
                const auto ownerOwn = s.Data.Files[3].Own;
                s.Data.Files[3].Own = {{0, .5}, {3, .5}};
                const auto budget = s.Data.Resources.Disk;
                s.Data.Resources.Disk = 1e6; // far above the base's estimate, far below its artifact
                s.Layout = topHeavy;
                Preflight(s, cache);
                const auto report = ReadFile(JoinPath(s.Directory, "compatibility.tsv"));
                check(s.Layout.Bases == 1 && s.Layout.Pchs.size() == 2 && s.Layout.Pchs[0].Roots == std::vector<int>({0}) &&
                          s.Layout.Pchs[0].Units == std::vector<int>({0}) && s.Layout.Pchs[1].Parent == 0 &&
                          s.Layout.Pchs[1].Roots == std::vector<int>({3}) && s.Layout.Assignment == std::vector<int>({0, 1, 0}),
                      "an initial base measured over the room the plan leaves it loses its thin tail, not its family");
                check(report.find("dropped 1 tail root") != std::string::npos &&
                          report.find("bisected: dropped 0 root") == std::string::npos,
                      "the tail is reported on its own, not as a bisection that dropped nothing");
                s.Corrected = true;
                s.Layout = topHeavy;
                s.Layout.Frozen = s.Layout.Bases; // the bases of a corrected plan are the initial plan's
                bool overBudget = false;
                try { Preflight(s, cache); } catch (const TError&) { overBudget = true; }
                check(overBudget, "a corrected base is frozen: over the budget is an error, not a trim");
                s.Corrected = false;
                s.Data.Units[3].Bytes = spareBytes;
                s.Data.Files[3].Own = ownerOwn;
                s.Data.Resources.Disk = budget;
            }
            // The trim is abandoned where a kept root compiles only with a
            // dropped one: that the PCH of a prefix fits is no proof that it
            // builds under the flags of its class. The base is back whole and
            // the measured fit decides: it keeps the base and drops the spare
            // delta for it, or evicts the family with a disk reason. The trim
            // used to turn a base that built into a rejected one.
            {
                const auto budget = s.Data.Resources.Disk;
                WriteFile(base, "#pragma once\nstruct Derived : Big0 { int x; };\n"); // needs the root the trim drops
                const auto ownerOwn = s.Data.Files[3].Own;
                s.Data.Files[3].Own = {{0, .5}, {3, .5}};
                TLayout dependent;
                dependent.Bases = 1;
                TPch pch;
                pch.Class = pch.Representative = 0;
                pch.Roots = pch.Units = {0, 4};
                TPch spare;
                spare.Class = spare.Representative = 0;
                spare.Parent = 0;
                spare.Roots = {3};
                spare.Units = {0, 3};
                dependent.Pchs = {pch, spare};
                dependent.Assignment = {0, 1, 0};
                const auto spareBytes = s.Data.Units[3].Bytes;
                s.Data.Units[3].Bytes = 3e6; // the spare delta alone leaves the base less room than it takes
                s.Data.Resources.Disk = 30e6; // the whole base and the delta fit
                s.Layout = dependent;
                Preflight(s, cache);
                check(s.Layout.Bases == 1 && s.Layout.Pchs.size() == 2 && s.Layout.Pchs[0].Roots == std::vector<int>({0, 4}) &&
                          s.Layout.Assignment == std::vector<int>({0, 1, 0}) &&
                          ReadFile(JoinPath(s.Directory, "compatibility.tsv")).find("not trimmed, a kept root compiles only with a dropped one") != std::string::npos,
                      "a base whose kept root needs a dropped one is back whole, and the measured fit keeps it with its delta");
                s.Layout = dependent;
                s.Data.Resources.Disk = 20e6; // the base alone does not fit
                Preflight(s, cache);
                check(s.Layout.Pchs.empty() && s.Layout.Assignment == std::vector<int>(3, -1) &&
                          ReadFile(JoinPath(s.Directory, "disk-budget.tsv")).find("family") != std::string::npos,
                      "the same base over the budget is evicted for its bytes, not rejected for a compile that never failed");
                s.Data.Units[3].Bytes = spareBytes;
                s.Data.Files[3].Own = ownerOwn;
                s.Data.Resources.Disk = budget;
                WriteFile(base, "#pragma once\nstruct Base {};\n");
            }
            // A small header instantiating most of the artifact can sit
            // anywhere in the order of the roots. The guess by source bytes
            // then lands far past the boundary, and only enough halvings find
            // the last prefix that fits: here ten of twenty thin roots before
            // the eleventh, a large one.
            {
                const auto budget = s.Data.Resources.Disk;
                s.Data.Units.resize(23);
                const int thin[] = {0, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
                for (int u = 5; u < 23; ++u) {
                    const auto path = JoinPath(s.Baseline, "thin" + std::to_string(u) + ".h");
                    WriteFile(path, "#pragma once\nstruct Thin" + std::to_string(u) + " {};\n");
                    s.Data.Units[u].Include = "\"" + path + "\"";
                    s.Data.Units[u].Parse = .01;
                    s.Data.Units[u].Bytes = 100;
                    s.Data.Units[u].Stable = true;
                }
                TLayout wide;
                wide.Bases = 1;
                TPch pch;
                pch.Class = pch.Representative = 0;
                for (int i = 0; i < 10; ++i) { pch.Roots.push_back(thin[i]); }
                pch.Roots.push_back(4); // the large one, small to the estimate
                for (int i = 10; i < 20; ++i) { pch.Roots.push_back(thin[i]); }
                pch.Units = pch.Roots;
                std::sort(pch.Units.begin(), pch.Units.end());
                wide.Pchs = {pch};
                wide.Assignment.assign(s.Data.Targets.size(), 0);
                s.Data.Resources.Disk = 1e6; // ten thin roots fit, the large one does not
                s.Layout = wide;
                Preflight(s, cache);
                check(s.Layout.Pchs.size() == 1 && s.Layout.Pchs[0].Roots.size() == 10 &&
                          std::find(s.Layout.Pchs[0].Roots.begin(), s.Layout.Pchs[0].Roots.end(), 4) == s.Layout.Pchs[0].Roots.end() &&
                          ReadFile(JoinPath(s.Directory, "compatibility.tsv")).find("dropped 11 tail root(s)") != std::string::npos,
                      "the search for the last prefix that fits does not stop three halvings short of it");
                s.Data.Resources.Disk = budget;
            }
            candidate.Bases = 0;
            candidate.Pchs.erase(candidate.Pchs.begin());
            candidate.Pchs[0].Parent = -1;
            candidate.Pchs[0].Units = candidate.Pchs[0].Roots = {1};
            candidate.Assignment = {0, 0, 0};
            s.Layout = candidate;
            bool invalidHierarchy = false;
            try { Preflight(s, cache); } catch (const TError&) { invalidHierarchy = true; }
            check(invalidHierarchy, "a standalone delta is not a two-level layout");
            Report("real Clang PCH link self-test: OK");
        }

        void WorkflowSelfTest() {
            LayerOrderSelfTest();
            auto check = [](bool ok, const std::string& text) { if (!ok) { Fail("workflow self-test: " + text); } };
            TWorkflow request;
            request.Root = "/source";
            request.Cycle = "/baseline/state/cycle";
            request.Options = {{"--disk", {"6G"}}, {"--target", {"all", "tests"}}};
            auto decoded = Decode<TWorkflow>(Encode(request));
            check(decoded.Root == request.Root && decoded.Options == request.Options, "persist parameters and target lists");
            auto dir = MakeTempDir(getenv("TMPDIR") ? getenv("TMPDIR") : "/place/vartmp/opencode", "pch-resume-");
            {
                const auto reports = JoinPath(dir, "output-test");
                ConfigureOutput(reports, false);
                TRunOptions run;
                run.OutputLabel = "stream-test";
                const auto failed = Run({"/bin/sh", "-c", "printf 'ordinary output\\nwarning: fixture\\n' ; printf 'FAILED: fixture\\nerror: fixture' >&2; exit 7"}, run);
                check(failed.ExitCode == 7 && failed.Stderr.find("error: fixture") != std::string::npos,
                      "logged child preserves exit status and unterminated stderr diagnostics");
                const auto silent = Run({"/bin/sh", "-c", "printf 'ordinary failure without a severity prefix'; exit 9"}, run);
                check(silent.ExitCode == 9 && silent.Stderr.find("ordinary failure") != std::string::npos,
                      "nonstandard failures retain a diagnostic tail");
                size_t childLogs = 0;
                for (const auto& entry : std::filesystem::directory_iterator(JoinPath(reports, "logs"))) {
                    if (entry.path().filename() == "run.log") { continue; }
                    ++childLogs;
                    check(!ReadFile(entry.path().string()).empty(), "child log preserves full output");
                }
                check(childLogs == 2, "child logs are distinct within one attempt");
                CloseOutput();
                ConfigureOutput(reports, true);
                check(Run({"/bin/sh", "-c", "printf 'verbose output\\n'"}, run).ExitCode == 0,
                      "verbose changes presentation only");
                CloseOutput();
            }
            if (const char* compiler = getenv("PCH_UPDATE_TEST_CXX"); compiler && *compiler) {
                LinkPreflightSelfTest(dir, Program(compiler, {}, false));
            }
            // Exercise export through GN itself, including the toolchain-owned
            // sysroot and the trailing slash in GN's source-root path.
            TSnapshot exported;
            exported.Root = JoinPath(dir, "export-source");
            const auto exportBuild = JoinPath(dir, "export-build");
            exported.Flags = {Canonical({"/tool/clang++", "--sysroot=/sysroot",
                "-ffile-prefix-map=" + exported.Root + "/=", "-I" + exported.Root,
                "-I" + exportBuild + "/gen", "--sysroot=/target-override", "-include", "gen/driver.h",
                "-Xclang", "-include", "-Xclang", "gen/forwarded.h"}, exportBuild, exported.Root)};
            auto containsFlags = [](const auto& flags, const std::vector<std::string>& expected) {
                return std::search(flags.begin(), flags.end(), expected.begin(), expected.end()) != flags.end();
            };
            check(containsFlags(exported.Flags[0], {"-include", "<out>/gen/driver.h"}) &&
                  containsFlags(exported.Flags[0], {"-Xclang", "-include", "-Xclang", "<out>/gen/forwarded.h"}),
                  "canonical paths preserve cc1 wrappers and normalize the actual header operand");
            exported.Layout.Pchs.resize(1);
            exported.Layout.Pchs[0].Class = 0;
            WriteFile(JoinPath(exported.Root, ".gn"), "buildconfig = \"//config.gn\"\n");
            WriteFile(JoinPath(exported.Root, "config.gn"), "set_default_toolchain(\"//toolchain:clang\")\n");
            WriteFile(JoinPath(exported.Root, "BUILD.gn"), "group(\"all\") { deps = [ \"//build/gn/pch/global:pch_group_0_compile\" ] }\n");
            WriteFile(JoinPath(exported.Root, "toolchain/BUILD.gn"), R"(toolchain("clang") {
  tool("cxx") {
    command = "/tool/clang++ --sysroot=/sysroot -MD -MF {{output}}.d {{cflags}} -c {{source}} -o {{output}}"
    depfile = "{{output}}.d"
    depsformat = "gcc"
    outputs = [ "{{source_out_dir}}/{{target_output_name}}.{{source_file_part}}.o" ]
  }
  tool("stamp") { command = "touch {{output}}" }
}
)");
            WriteFile(JoinPath(exported.Root, "build/gn/pch/global/BUILD.gn"), ExportPaths() + ExportFlags(exported, 0) + R"(
source_set("pch_group_0_compile") {
  sources = [ "group_0/pch.cc" ]
  cflags = compiler_flags
}
)");
            WriteFile(JoinPath(exported.Root, "build/gn/pch/global/group_0/pch.cc"), "struct Exported {};\n");
            RunChecked({Program("gn"), "gen", exportBuild, "--root=" + exported.Root, "--export-compile-commands"});
            const auto exportedCompiles = ReadCompiles(exportBuild);
            const auto& exportedArgv = exportedCompiles.at("obj/build/gn/pch/global/group_0/pch_group_0_compile.pch.cc.o").Argv;
            check(containsFlags(exportedArgv, {"-include", exportBuild + "/gen/driver.h"}) &&
                  containsFlags(exportedArgv, {"-Xclang", "-include", "-Xclang", exportBuild + "/gen/forwarded.h"}),
                  "GN export preserves cc1 wrappers and rebases both kinds of forced include");
            CheckDeployment(exported, exportBuild);
            TWorkflow resourceRequest;
            resourceRequest.Options = {{"--workers", {"4"}}, {"--ram-gb", {"2"}}, {"--action-memory-gb", {"1"}}};
            ResolveResourceDefaults(resourceRequest);
            const auto resourceOptions = resourceRequest.Options;
            ResolveResourceDefaults(resourceRequest);
            const auto limits = WorkflowResources(resourceRequest);
            check(resourceOptions == resourceRequest.Options && limits.Workers == 4,
                  "a given worker count is kept whole, and resolved defaults persist");
            check(limits.AnalysisJobs == 4, "analysis concurrency defaults to the worker count");
            auto analysisRequest = resourceRequest;
            analysisRequest.Options["--analysis-jobs"] = {"3"};
            auto resumedAnalysis = Decode<TWorkflow>(Encode(analysisRequest));
            check(WorkflowResources(resumedAnalysis).AnalysisJobs == 3 &&
                  WorkflowResources(resumedAnalysis).Workers == limits.Workers,
                  "resumed correction inherits analysis concurrency without changing build workers");
            for (const auto* invalid : {"0", "-1", "1.5", "1000001", "invalid"}) {
                analysisRequest.Options["--analysis-jobs"] = {invalid};
                bool rejected = false;
                try { WorkflowResources(analysisRequest); } catch (const TError&) { rejected = true; }
                check(rejected, "invalid analysis concurrency fails before workflow execution");
            }
            // Four no-PCH compiles of the action memory prior, twice what the
            // RAM admits at once: the model schedules them the way Ninja will,
            // all four at once on the four workers.
            std::vector<TAction> noPch(4);
            for (auto& action : noPch) { action.Time = 1; action.Memory = limits.ActionMemory; }
            TResources modelResources;
            modelResources.Workers = limits.Workers;
            modelResources.Ram = limits.Ram;
            check(Makespan(noPch, modelResources) == 1, "the model schedules on the worker count alone, as the build does");
            // Left unset, the count is what the machine sustains for ordinary
            // compiles: every CPU, and no more than the RAM behind the run
            // admits at the action memory prior.
            TWorkflow unset;
            unset.Options = {{"--ram-gb", {"2"}}, {"--action-memory-gb", {"1"}}};
            ResolveResourceDefaults(unset);
            check(WorkflowResources(unset).Workers == std::max(1, std::min(int(CpuCount()), 2)) &&
                      WorkflowResources(unset).AnalysisJobs == WorkflowResources(unset).Workers,
                  "an unset worker count takes every CPU, at most what the RAM admits at the action memory");
            TWorkflow starved;
            starved.Options = {{"--ram-gb", {"0.5"}}, {"--action-memory-gb", {"1"}}}; // less than one action
            ResolveResourceDefaults(starved);
            check(WorkflowResources(starved).Workers == 1, "RAM below the action memory prior leaves one worker, not an error");
            // The request comes on top of the defaults. A count derived from
            // the RAM follows a RAM budget supplied with the request; a count
            // given in the request is kept whole, whatever else it supplies.
            TWorkflow supplied;
            const auto before = ApplySupplied(supplied, {{"--ram-gb", {"16"}}});
            check(before.count(DERIVED_WORKERS) && before.count("--workers") && before.count("--ram-gb") &&
                      WorkflowResources(supplied).Workers == std::max(1, std::min(int(CpuCount()), 16)) &&
                      WorkflowResources(supplied).Ram == 16e9,
                  "a worker count derived from the RAM follows a RAM budget supplied with the request");
            TWorkflow given;
            const auto fresh = ApplySupplied(given, {{"--workers", {"3"}}, {"--ram-gb", {"1"}}});
            const auto saved = given.Options;
            check(fresh.count("--workers") && WorkflowResources(given).Workers == 3 && !given.Options.count(DERIVED_WORKERS),
                  "a worker count given with the request is kept whole");
            const auto earlier = ApplySupplied(given, {{"--ram-gb", {"64"}}});
            check(earlier == saved && WorkflowResources(given).Workers == 3 && WorkflowResources(given).Ram == 64e9,
                  "and it stays whole when the RAM budget is supplied on a retry");
            resourceRequest.Cycle = JoinPath(dir, "inherited-resources");
            TSnapshot oldResources;
            oldResources.Directory = JoinPath(resourceRequest.Cycle, "initial");
            oldResources.Data.Resources = modelResources;
            oldResources.Data.ActionMemory = limits.ActionMemory;
            WriteFile(JoinPath(resourceRequest.Cycle, "initial/snapshot.bin"), Encode(oldResources));
            resourceRequest.Options.clear();
            ResolveResourceDefaults(resourceRequest);
            check(WorkflowResources(resourceRequest).Ram == limits.Ram &&
                  WorkflowResources(resourceRequest).Workers == limits.Workers,
                  "unfinished runs inherit frozen resources rather than current free RAM");
            TSnapshot selecting;
            selecting.Tool = "selector-version";
            selecting.Data.Resources.Disk = 1e9;
            TCalibration calibration;
            calibration.Classes.resize(1);
            int selections = 0;
            auto select = [&]() { ++selections; return TLayout{}; };
            const auto selectionDir = JoinPath(dir, "selection");
            const auto diskReport = JoinPath(selectionDir, "disk-budget.tsv");
            MakeDirs(diskReport); // publication fails after the checkpoint is durable
            bool reportFailed = false;
            try { SelectCheckpoint(selecting, selectionDir, calibration, nullptr, select); }
            catch (const TError&) { reportFailed = true; }
            check(reportFailed && selections == 1, "report failure occurs after completing selection");
            check(Exists(JoinPath(selectionDir, "selection.bin")) &&
                  !Exists(JoinPath(selectionDir, "snapshot.bin")), "selection is durable before compatibility completes");
            RemoveTree(diskReport);
            SelectCheckpoint(selecting, selectionDir, calibration, nullptr, select);
            const auto firstReport = ReadFile(diskReport);
            check(selections == 1 && firstReport.find("budget_bytes=1000000000") != std::string::npos,
                  "a failed diagnostic write is retried from the checkpoint without rerunning selection");
            auto resumed = Decode<TSnapshot>(Encode(selecting));
            resumed.Nonce = "new-analysis-resume-nonce";
            RemoveFile(diskReport);
            SelectCheckpoint(resumed, selectionDir, calibration, nullptr, select);
            check(selections == 1 && ReadFile(diskReport) == firstReport,
                  "resume restores missing diagnostics and skips the expensive selector");
            calibration.Classes[0].Load *= 2;
            SelectCheckpoint(resumed, selectionDir, calibration, nullptr, select);
            check(selections == 2, "changed calibration invalidates selection");
            resumed.Data.Resources.Disk *= 2;
            SelectCheckpoint(resumed, selectionDir, calibration, nullptr, select);
            check(selections == 3, "changed model/resources invalidate selection");
            TLayout frozen;
            SelectCheckpoint(resumed, selectionDir, calibration, &frozen, select);
            SelectCheckpoint(resumed, selectionDir, calibration, &frozen, select);
            check(selections == 4, "frozen-base correction has its own reusable selection");
            check(ReadFile(diskReport).find("phase=corrected") != std::string::npos &&
                  ReadFile(diskReport).find("budget_bytes=2000000000") != std::string::npos,
                  "corrected diagnostics use the corrected phase and current budget");
            std::pair<std::string, TLayout> oldSelection{"old-format", {}};
            WriteFile(JoinPath(selectionDir, "selection.bin"), Encode(oldSelection));
            SelectCheckpoint(resumed, selectionDir, calibration, &frozen, select);
            check(selections == 5, "old checkpoints without diagnostics are recomputed once");
            resumed.Directory = selectionDir;
            resumed.Baseline = JoinPath(dir, "diagnostic-baseline");
            PublishDiskBudgetReport(resumed);
            const auto published = ReadFile(JoinPath(resumed.Baseline, "pch-disk-budget.tsv"));
            RemoveTree(selectionDir);
            check(ReadFile(JoinPath(resumed.Baseline, "pch-disk-budget.tsv")) == published && !published.empty(),
                  "published disk diagnostics survive deletion of the disposable phase");
            TSnapshot s;
            s.Root = WorktreeRoot();
            s.Tree = TreeIdentity(s.Root);
            // The streamed identity is the digest of HEAD and the whole diff, as
            // saved plans recorded it.
            {
                TRunOptions git;
                git.Cwd = s.Root;
                check(s.Tree == HashHex(RunChecked({"git", "rev-parse", "HEAD"}, git) +
                                        RunChecked({"git", "diff", "--binary", "HEAD", "--"}, git)),
                      "the streamed tree identity equals the digest of the whole diff");
            }
            s.Tool = DigestFile(SelfExe());
            s.Directory = dir;
            s.Nonce = "resume-test";
            s.Ninja = Program("ninja");
            s.Data.Resources.Workers = 1;
            s.Prediction.Actions.resize(2);
            s.Data.Actions.resize(2); // two ordinary commands, neither a PCH nor a compiler
            s.Measured = {0, 1};
            MakeDirs(JoinPath(dir, "build"));
            std::string ninja;
            for (int i = 0; i < 2; ++i) {
                TMeasureConfig c;
                c.Directory = dir;
                c.Nonce = s.Nonce;
                c.Id = i;
                c.Memory = 1;
                c.Outputs = {"output" + std::to_string(i)};
                c.CacheMode = "not-compiler";
                c.Command = i == 0 ? "printf good > output0" :
                    "if test -f retry; then printf good > output1; else touch retry; exit 1; fi";
                auto path = JoinPath(dir, "actions/" + std::to_string(i) + ".bin");
                auto bytes = Encode(c);
                WriteFile(path, bytes);
                s.Commands.push_back(HashHex(bytes));
                ninja += "rule action" + std::to_string(i) + "\n  command = " +
                    ShellJoin({SelfExe(), "measure", path}) + "\nbuild " + c.Outputs[0] + ": action" + std::to_string(i) +
                    (i ? " output0\n" : "\n");
            }
            ninja += "build _pch_verify: phony output1\n";
            WriteFile(JoinPath(dir, "build/measured.ninja"), ninja);
            WriteFile(JoinPath(dir, "snapshot.bin"), Encode(s));
            const auto frozenPlan = ReadFile(JoinPath(dir, "snapshot.bin"));
            check(Verify(s) != 0, "fixture fails after the first completed action");
            auto first = ReadFile(JoinPath(dir, "observations/0.bin"));
            auto mtime = MtimeNs(JoinPath(dir, "build/output0"));
            check(Verify(s) == 0, "retry completes the failed phase");
            check(first == ReadFile(JoinPath(dir, "observations/0.bin")) && mtime == MtimeNs(JoinPath(dir, "build/output0")) &&
                  ReadFile(JoinPath(dir, "snapshot.bin")) == frozenPlan,
                  "retry preserves successful measurement, output and plan");
            {
                std::string targets, actions;
                const auto observations = ReadObservations(s, targets, actions);
                check(observations[0].Invocation != observations[1].Invocation &&
                      targets.find("measured_build_wall_seconds\tunavailable") != std::string::npos,
                      "calibration accepts validated prior attempt without claiming a full build wall");
            }
            first = ReadFile(JoinPath(dir, "observations/0.bin"));
            RemoveFile(JoinPath(dir, "observations/1.bin"));
            check(Verify(s) == 0 && Exists(JoinPath(dir, "observations/1.bin")) &&
                  first == ReadFile(JoinPath(dir, "observations/0.bin")), "missing telemetry only invalidates its action");
            first = ReadFile(JoinPath(dir, "observations/0.bin"));
            const auto completedRun = ReadFile(JoinPath(dir, "run.bin"));
            check(Verify(s) == 0 && first == ReadFile(JoinPath(dir, "observations/0.bin")) &&
                  completedRun == ReadFile(JoinPath(dir, "run.bin")), "completed verification is idempotent");
            // Model Ctrl+C to the foreground group, then a controller-only
            // SIGKILL. Both retries must preserve validated successes.
            for (int interrupt : {SIGINT, SIGKILL}) {
                s.Directory = JoinPath(dir, interrupt == SIGINT ? "interrupt" : "crash");
                s.Nonce = "interrupt-test-" + std::to_string(interrupt);
                s.Commands.clear();
                MakeDirs(JoinPath(s.Directory, "build"));
                for (int i = 0; i < 2; ++i) {
                    auto c = Decode<TMeasureConfig>(ReadFile(JoinPath(dir, "actions/" + std::to_string(i) + ".bin")));
                    c.Directory = s.Directory;
                    c.Nonce = s.Nonce;
                    if (i == 1) {
                        c.Command = "if test -f released; then printf resumed > output1; else "
                                     "printf ready > ready.pipe; cat release.pipe > output1; touch released; printf 'child survived controller\\n'; fi";
                    }
                    auto bytes = Encode(c);
                    WriteFile(JoinPath(s.Directory, "actions/" + std::to_string(i) + ".bin"), bytes);
                    s.Commands.push_back(HashHex(bytes));
                }
                auto interruptedNinja = ninja;
                Replace(interruptedNinja, dir + "/actions/", s.Directory + "/actions/");
                WriteFile(JoinPath(s.Directory, "build/measured.ninja"), interruptedNinja);
                const auto interruptedPlan = Encode(s);
                WriteFile(JoinPath(s.Directory, "snapshot.bin"), interruptedPlan);
                const auto readyPipe = JoinPath(s.Directory, "build/ready.pipe");
                const auto releasePipe = JoinPath(s.Directory, "build/release.pipe");
                check(mkfifo(readyPipe.c_str(), 0600) == 0 && mkfifo(releasePipe.c_str(), 0600) == 0, "create interruption rendezvous");
                pid_t controller = fork();
                check(controller >= 0, "fork verification controller");
                if (controller == 0) {
                    // Only the test creates a foreground group; Run must keep
                    // Ninja in it so the same SIGINT reaches both processes.
                    if (interrupt == SIGINT && setpgid(0, 0) != 0) { _exit(98); }
                    signal(SIGINT, SIG_DFL);
                    try {
                        ConfigureOutput(JoinPath(s.Directory, "interruption-report"), false);
                        _exit(Verify(s));
                    } catch (...) { _exit(99); }
                }
                int readyFd = open(readyPipe.c_str(), O_RDONLY);
                char message[5];
                check(readyFd >= 0 && read(readyFd, message, sizeof(message)) == ssize_t(sizeof(message)), "measurement reached rendezvous");
                close(readyFd);
                first = ReadFile(JoinPath(s.Directory, "observations/0.bin"));
                const auto interruptedRun = Decode<TRunRecord>(ReadFile(JoinPath(s.Directory, "run.bin")));
                check(kill(interrupt == SIGINT ? -controller : controller, interrupt) == 0, "interrupt measurement");
                int status = 0;
                check(waitpid(controller, &status, 0) == controller && WIFSIGNALED(status) &&
                      WTERMSIG(status) == interrupt, "controller was interrupted");
                const auto lockPath = JoinPath(s.Directory, "verification.lock/cycle.lock");
                int lockFd = open(lockPath.c_str(), O_RDWR);
                check(lockFd >= 0, "open verification lock");
                if (interrupt == SIGKILL) {
                    check(flock(lockFd, LOCK_EX | LOCK_NB) != 0, "orphaned child retains verification lock");
                    int releaseFd = open(releasePipe.c_str(), O_WRONLY);
                    check(releaseFd >= 0 && write(releaseFd, "good", 4) == 4, "release orphaned child");
                    close(releaseFd);
                }
                check(flock(lockFd, LOCK_EX) == 0, "wait for Ninja and measurement children to exit");
                close(lockFd);
                if (interrupt == SIGKILL) {
                    bool retained = false;
                    for (const auto& entry : std::filesystem::directory_iterator(JoinPath(s.Directory, "interruption-report/logs"))) {
                        if (entry.path().filename() == "run.log") { continue; }
                        retained |= ReadFile(entry.path().string()).find("child survived controller") != std::string::npos;
                    }
                    check(retained, "orphaned Ninja writes its log after controller SIGKILL without SIGPIPE");
                }
                check(!BuildRunning(JoinPath(s.Directory, "build")), "interrupted measurement leaves no running Ninja");
                WriteFile(JoinPath(s.Directory, "build/released"), "ready\n");
                check(Verify(s) == 0 && first == ReadFile(JoinPath(s.Directory, "observations/0.bin")) &&
                      Decode<TRunRecord>(ReadFile(JoinPath(s.Directory, "run.bin"))).Token != interruptedRun.Token &&
                      ReadFile(JoinPath(s.Directory, "snapshot.bin")) == interruptedPlan &&
                      ReadFile(JoinPath(s.Directory, "build/output1")) == (interrupt == SIGINT ? "resumed" : "good"),
                      "resume preserves successes, including orphaned actions that completed under the lock");
            }
            // Recover an interrupted replacement without discarding novel edits.
            auto root = JoinPath(dir, "source"), cycle = JoinPath(dir, "cycle");
            auto live = JoinPath(root, GLOBAL_DIR);
            WriteFile(JoinPath(cycle, "original-global/groups.gni"), "old");
            WriteFile(JoinPath(cycle, "export/groups.gni"), "new");
            WriteFile(JoinPath(live, "groups.gni"), "user edit");
            WriteFile(JoinPath(cycle, "installing"), "installing");
            bool rejected = false;
            try { RecoverApply(root, cycle); } catch (const TError&) { rejected = true; }
            check(rejected, "recovery protects edits made during interruption");
            RemoveFile(JoinPath(live, "groups.gni"));
            rejected = false;
            try { RecoverApply(root, cycle); } catch (const TError&) { rejected = true; }
            check(rejected, "recovery also protects user deletions");
            WriteFile(JoinPath(live, "groups.gni"), "new");
            RecoverApply(root, cycle);
            check(ReadFile(JoinPath(live, "groups.gni")) == "old" && !Exists(JoinPath(cycle, "installing")), "interrupted apply restores backup");
            auto transaction = MakeTempDir(DirName(live), ".pch-install-");
            WriteFile(JoinPath(cycle, "installing"), transaction + "\n");
            WriteFile(JoinPath(transaction, "new/groups.gni"), "partial bytes");
            RecoverApply(root, cycle);
            check(ReadFile(JoinPath(live, "groups.gni")) == "old" && !Exists(transaction), "interrupted staging copy leaves live files intact");
            transaction = MakeTempDir(DirName(live), ".pch-install-");
            WriteFile(JoinPath(cycle, "installing"), transaction + "\n");
            std::filesystem::rename(live, JoinPath(transaction, "previous"));
            RecoverApply(root, cycle);
            check(ReadFile(JoinPath(live, "groups.gni")) == "old", "interruption between directory renames recovers");

            auto state = JoinPath(dir, "baseline/.pch_update");
            TWorkflow active;
            active.Root = root;
            active.Cycle = JoinPath(state, "current");
            active.Options["--disk"] = {"1G"};
            WriteFile(JoinPath(active.Cycle, "initial/analysis.bin"), "analysis");
            WriteFile(JoinPath(active.Cycle, "initial/selection.bin"), "selection");
            WriteFile(JoinPath(active.Cycle, "initial/build/object.o"), "stale object");
            WriteFile(JoinPath(active.Cycle, "corrected/snapshot.bin"), "stale correction");
            WriteFile(JoinPath(state, "cache/probe"), "reusable probe");
            TRestartWorkflow restart{active, true, false};
            WriteFile(JoinPath(state, "restart.bin"), Encode(restart));
            RestartWorkflow(state, Decode<TRestartWorkflow>(ReadFile(JoinPath(state, "restart.bin"))));
            RestartWorkflow(state, restart);
            check(Exists(JoinPath(active.Cycle, "initial/analysis.bin")) &&
                  Exists(JoinPath(active.Cycle, "initial/settings.changed")) &&
                  Exists(JoinPath(state, "cache/probe")) &&
                  !Exists(JoinPath(active.Cycle, "initial/build")) &&
                  !Exists(JoinPath(active.Cycle, "corrected")), "restart is idempotent and preserves reusable work");
            TSnapshot firstPlan, secondPlan;
            firstPlan.Directory = JoinPath(active.Cycle, "initial");
            secondPlan.Directory = JoinPath(active.Cycle, "corrected");
            const auto kept = JoinPath(firstPlan.Directory, "build/keep.pch");
            WriteFile(kept, "fingerprinted");
            WriteFile(JoinPath(firstPlan.Directory, "build/drop.o"), "obsolete");
            secondPlan.Inputs.emplace_back(kept, DigestFile(kept));
            ReleaseInitialArtifacts(firstPlan, secondPlan);
            ReleaseInitialArtifacts(firstPlan, secondPlan);
            check(Exists(kept) && !Exists(JoinPath(firstPlan.Directory, "build/drop.o")),
                  "calibrated initial artifacts are released without breaking snapshot inputs");
            DiscardWorkflow(state);
            check(!Exists(state) && !Exists(state + ".cleanup") && ReadFile(JoinPath(live, "groups.gni")) == "old",
                  "successful cleanup leaves installed files and no workflow state");
            WriteFile(JoinPath(state, "cache/large-file"), "remaining bytes");
            std::filesystem::rename(state, state + ".cleanup");
            DiscardWorkflow(state);
            check(!Exists(state + ".cleanup"), "interrupted cleanup completes without snapshots");
            RemoveTree(dir);
        }
    }

    int Command(const std::vector<std::string>& args) {
        if (args.empty() || std::find(args.begin(), args.end(), "--help") != args.end() || args[0] == "-h") { Report(HELP); return 0; }
        if (args[0] == "measure" && args.size() == 2) { return Measure(args[1]); }
        if (args[0] == "self-test" && args.size() == 1) {
            const int result = SelfTest();
            if (!result) { WorkflowSelfTest(); }
            return result;
        }
        std::string stage, baseline;
        bool verbose = false;
        std::map<std::string, std::vector<std::string>> supplied;
        const std::set<std::string> allowed = {"--disk", "--mode", "--q", "--sample", "--workers", "--ram-gb",
            "--action-memory-gb", "--analysis-jobs", "--main-ref", "--target", "--gn", "--ninja"};
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--verbose") { verbose = true; continue; }
            if (args[i] == "--predict" || args[i] == "--correct" || args[i] == "--apply") {
                if (!stage.empty()) { Fail("choose one of --predict, --correct, --apply, or none for the full cycle"); }
                stage = args[i];
                continue;
            }
            auto [key, value] = ReadOption(args, i);
            if (key == "--baseline") { baseline = AbsPath(value); }
            else if (allowed.count(key)) { supplied[key].push_back(value); }
            else { Fail("unknown option " + key + "\n" + HELP); }
        }
        if (baseline.empty()) { Fail("--baseline DIR is required"); }
        if (supplied.count("--sample") && (stage == "--predict" || stage == "--apply")) {
            Fail("--sample is a measurement option; specify it with --correct");
        }
        if (supplied.count("--sample")) {
            const auto& v = supplied.at("--sample").back();
            if (v.empty() || v.size() > 3 || v.find_first_not_of("0123456789") != std::string::npos ||
                std::stoi(v) < 10 || std::stoi(v) > 100) { Fail("--sample must be an integer from 10 to 100"); }
        }
        MakeDirs(baseline);
        baseline = RealPath(baseline);
        const auto root = WorktreeRoot(), state = StateDirectory(baseline);
        const auto baselineRoot = BuildRoot(baseline);
        if (!baselineRoot.empty() && RealPath(baselineRoot) != root) {
            Fail("baseline belongs to another worktree: " + baselineRoot);
        }
        TCycleLock rootLock, stateLock;
        // Controller locks must not escape via baseline builds into dist-clangd.
        rootLock.Acquire(JoinPath(Trim(RunChecked({"git", "rev-parse", "--absolute-git-dir"})), "pch_update"));
        if (Exists(state + ".cleanup")) {
            RemoveTree(state + ".cleanup");
            Report("PCH workflow cleanup completed");
            return 0;
        }
        if (!Exists(state)) {
            if (stage == "--correct" || stage == "--apply") { Fail("no unfinished run; run --predict first"); }
            if (!supplied.count("--disk")) { Fail("--disk SIZE is required to start a new run"); }
        }
        stateLock.Acquire(state);
        const auto path = JoinPath(state, "state.bin");
        TWorkflow workflow;
        if (Exists(path)) { workflow = Decode<TWorkflow>(ReadFile(path)); }
        else {
            // Adopt the last unfinished legacy workflow at its existing path:
            // PCHs and Ninja commands embed it. All legacy scratch is removed
            // together with this run after a successful installation.
            auto mode = ReadPointer(JoinPath(state, "workflow.mode"));
            if (mode.empty()) { mode = "incremental"; }
            (void)ModeQ(mode);
            const auto legacy = JoinPath(ProfileDirectory(baseline, mode), "workflow/state.bin");
            if (Exists(legacy)) { workflow = Decode<TWorkflow>(ReadFile(legacy)); }
        }
        if (!workflow.Root.empty() && workflow.Root != root) { Fail("saved workflow belongs to " + workflow.Root); }
        workflow.Root = root;
        struct TOutputScope { ~TOutputScope() { CloseOutput(); } } outputScope;
        auto configureReport = [&](bool fresh) { Diagnostic([&] {
            const auto reportPointer = JoinPath(state, "report.path");
            auto directory = ReadPointer(reportPointer);
            const auto reports = JoinPath(baseline, "pch-report");
            if (fresh || directory.empty() || !Under(directory, reports)) {
                directory = JoinPath(reports, "run-" + std::to_string(Now()) + "-" + std::to_string(getpid()));
            }
            ConfigureOutput(directory, verbose);
            WriteFile(reportPointer, directory + "\n");
            WriteFile(JoinPath(reports, "latest.txt"), directory + "\n");
            Detail("command: " + ShellJoin(args));
            Report("PCH update: " + (stage.empty() ? std::string("full workflow") : stage.substr(2)) + "\nReport: " + directory);
        }); };
        configureReport(false);
        if (!workflow.Cycle.empty()) {
            if (!Under(workflow.Cycle, state) || workflow.Cycle == state) { Fail("workflow directory is outside its scratch state"); }
            RequireWorkflowIdle(workflow.Cycle);
            const auto applied = ReadPointer(JoinPath(workflow.Cycle, "applied.tree"));
            if (!applied.empty()) {
                // The marker commits a successfully validated installation.
                // Changes made by the user since then do not undo that success.
                FinishInstallation(root, workflow.Cycle);
                DiscardWorkflow(state);
                return 0;
            }
            RecoverApply(root, workflow.Cycle);
        }
        if (Exists(JoinPath(state, "restart.bin"))) {
            auto restart = Decode<TRestartWorkflow>(ReadFile(JoinPath(state, "restart.bin")));
            if (restart.Request.Root != root || restart.Request.Cycle != workflow.Cycle) { Fail("invalid restart journal"); }
            RestartWorkflow(state, restart);
            workflow = std::move(restart.Request);
        }
        // Freeze defaults before baseline work consumes RAM. Materializing old
        // defaults does not count as a user settings change on resume.
        workflow.Options.erase("--measure-max-external-cpu"); // obsolete persisted setting
        workflow.Options.erase("--bandwidth-gbps"); // ditto: nothing schedules by bandwidth any more
        const auto before = ApplySupplied(workflow, supplied);
        if (!workflow.Options.count("--mode")) { workflow.Options["--mode"] = {"incremental"}; }
        (void)ModeQ(Setting(workflow, "--mode"));
        (void)WorkflowResources(workflow); // reject invalid limits before starting a build
        auto predictionOptions = [](auto options) { options.erase("--sample"); return options; };
        const bool changed = !workflow.Cycle.empty() && predictionOptions(before) != predictionOptions(workflow.Options);
        const auto requestedSample = std::stoi(Setting(workflow, "--sample", "100"));
        if (!workflow.Cycle.empty() && supplied.count("--sample") && !changed) {
            const auto path = JoinPath(workflow.Cycle, "initial/snapshot.bin");
            if (Exists(path)) {
                auto plan = LoadPlan(DirName(path));
                if (!plan.Commands.empty() && plan.Sample != requestedSample) {
                    Fail("measurement sample is frozen; resume with --sample " + std::to_string(plan.Sample));
                }
            }
        }
        if (changed && (stage == "--correct" || stage == "--apply")) { Fail("parameters changed; run --predict first"); }
        if (workflow.Cycle.empty()) {
            if (stage == "--correct" || stage == "--apply") { Fail("run --predict first"); }
            if (!workflow.Options.count("--disk")) { Fail("--disk SIZE is required on the first --predict"); }
            workflow.Cycle = JoinPath(state, "current");
        }
        if (changed) {
            configureReport(true);
            bool extraction = false;
            for (const char* key : {"--main-ref", "--target", "--gn", "--ninja"}) {
                auto a = before.find(key);
                auto b = workflow.Options.find(key);
                extraction |= (a == before.end()) != (b == workflow.Options.end()) ||
                    (a != before.end() && b != workflow.Options.end() && a->second != b->second);
            }
            RestartWorkflow(state, {workflow, true, extraction});
        }
        WriteFile(path, Encode(workflow));
        const auto initial = JoinPath(workflow.Cycle, "initial"), corrected = JoinPath(workflow.Cycle, "corrected");
        bool ready = false;
        if (Exists(JoinPath(initial, "snapshot.bin"))) {
            try { auto s = LoadPlan(initial); ValidateIdentity(s); ready = true; }
            catch (const TError& error) {
                if (stage == "--correct" || stage == "--apply") { Fail(std::string(error.what()) + "; run --predict first"); }
                Log("prediction inputs changed: " + std::string(error.what()));
                configureReport(true);
                RestartWorkflow(state, {workflow, false, false});
            }
        }
        if (!ready) {
            if (stage == "--correct" || stage == "--apply") { Fail("run --predict first"); }
            {
                TPhase phase("[1/7] Prepare baseline");
                PrepareBaseline(workflow, baseline);
            }
            std::vector<std::string> command = {"predict", "--baseline", baseline, "--output", initial};
            for (const auto& [key, values] : workflow.Options) {
                // The sample is a setting of the correction; the mark of a
                // derived count is no setting at all.
                if (key == "--sample" || key == DERIVED_WORKERS) { continue; }
                for (const auto& value : values) { command.insert(command.end(), {key, value}); }
            }
            TPhase phase("[2/7] Analyze, select and check initial PCHs");
            int status = StepCommand(command, Exists(JoinPath(initial, "settings.changed")));
            phase.Status(status);
            if (status) { return status; }
        } else {
            Log("[1–2/7] Reusing validated initial plan; baseline preparation and selection skipped");
        }
        if (stage == "--predict") {
            Diagnostic([&] { PublishReports(LoadPlan(initial)); });
            Report("Prediction ready. Next: " + ShellJoin({SelfExe(), "--baseline", baseline, "--correct"}));
            return 0;
        }
        if (stage != "--apply") {
            auto s = LoadPlan(initial);
            PrepareMeasurement(s, requestedSample);
            int status = 0;
            if (!Exists(JoinPath(corrected, "snapshot.bin"))) {
                {
                    TPhase phase("[3/7] Measure initial plan");
                    status = Verify(s);
                    phase.Status(status);
                    if (status) { return status; }
                }
                {
                    TPhase phase("[4/7] Calibrate, select and check corrected PCHs");
                    status = CorrectPlan(s, initial, corrected, CacheDirectory(JoinPath(state, "cache")),
                                         WorkflowResources(workflow).AnalysisJobs);
                    phase.Status(status);
                    if (status) { return status; }
                }
                s = LoadPlan(initial);
            } else {
                Log("[3–4/7] Reusing completed correction");
            }
            auto c = LoadPlan(corrected);
            if (c.Nonce != HashHex(s.Nonce + "corrected")) { Fail("corrected plan belongs to another cycle"); }
            ValidateIdentity(c);
            ReleaseInitialArtifacts(s, c);
            // Compatibility is complete for both plans. No later stage needs
            // these duplicate PCHs or per-translation-unit dependency records.
            {
                const auto cache = JoinPath(state, "cache");
                TCycleLock cacheLock;
                PrepareCompatibilityCache(cache, cacheLock);
                RemoveTree(JoinPath(cache, CompatibilityCacheName()));
            }
            {
                TPhase phase("[5/7] Measure corrected plan");
                status = Verify(c);
                phase.Status(status);
                if (status) { return status; }
            }
            if (stage == "--correct") {
                Diagnostic([&] {
                    std::string errors, actions;
                    const auto observations = ReadObservations(c, errors, actions);
                    MeasurementSummary(c, observations, errors, actions);
                });
                Report("Correction measured. Next: " + ShellJoin({SelfExe(), "--baseline", baseline, "--apply"}));
                return 0;
            }
        }
        if (!Exists(JoinPath(corrected, "snapshot.bin"))) { Fail("run --correct first"); }
        auto s = LoadPlan(corrected);
        int status;
        {
            TPhase phase("[6/7] Install PCHs and validate with a full GN build");
            status = ApplyGroups(s, workflow.Cycle);
            phase.Status(status);
        }
        if (!status) {
            TPhase phase("[7/7] Final report and cleanup");
            DiscardWorkflow(state);
        }
        return status;
    }
} // namespace NPchUpdate::NTwoStep
