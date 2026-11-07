#include "two_step.h"
#include "common.h"
#include "ninja.h"
#include "steps.h"

#include <rapidjson/document.h>

#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>

namespace NPchUpdate::NTwoStep {
    namespace {
        constexpr const char* VERSION = "pch-two-step-4";
        constexpr const char* LEGACY_VERSION = "pch-two-step-3";
        // Bump when extraction or the meaning of cached model features changes.
        constexpr const char* ANALYSIS_VERSION = "pch-analysis-3";

        const char* HELP =
            "pch_update --baseline DIR --disk SIZE [options]\n"
            "  Runs analysis, compatibility checks, prediction, verification and correction.\n"
            "  State, cache and results default to BASELINE/.pch_update/.\n"
            "  --ram-gb N           compiler RAM budget (default: 75% available RAM)\n"
            "  --disk SIZE          resident PCH budget (required; 16G, 4.5G, 400K, 800M, bare = GB)\n"
            "  --output DIR         optional new cycle directory (default: managed automatically)\n"
            "pch_update predict --baseline DIR --output DIR --ram-gb N --disk SIZE [options]\n"
            "  --workers N          verification workers (default: CPU count)\n"
            "  --bandwidth-gbps N   PCH disk bandwidth budget (default: 1)\n"
            "  --action-memory-gb N unmeasured action RSS prior (default: 1)\n"
            "  --mode MODE         global or incremental (default: incremental)\n"
            "  --q Q               cold fraction; overrides mode default (global: 1, incremental: 0.1)\n"
            "  --target T          repeat for each target (default: all, tests)\n"
            "  --root DIR --work-dir DIR --main-ref REF --gn PATH --ninja PATH\n"
            "  --analysis-jobs N    parallel analysis jobs (default: min(24, CPUs))\n"
            "  --state-age AGE      expire the selected mode without a successful cycle for AGE\n"
            "                       (12h, 7d, 2w; bare = days, 0 = never; default 7d)\n"
            "  --sample PERCENT     share of compile actions measured by verify (10..100,\n"
            "                       default 100); PCHs and required generator tools always built\n"
            "pch_update verify --plan DIR\n"
            "pch_update replan --plan DIR --output DIR --disk SIZE\n"
            "pch_update correct --plan DIR --output DIR\n"
            "pch_update self-test\n"
            "predict/replan/correct check PCH and consumer syntax before freezing predictions.\n"
            "  --work-dir DIR       cache override (default: BASELINE/.pch_update/cache)\n"
            "predict performs GN generation and cached header-analysis compiles.\n"
            "replan reuses a checked analysis checkpoint with the current selector/tool.\n"
            "replan accepts --mode and --q; omitted values inherit the plan's profile.\n"
            "--mode=MODE is also accepted. Results: BASELINE/.pch_update/modes/MODE/result.path.\n"
            "verify performs one fresh, locally compiled, measured build.\n"
            "correct freezes the base and emits a calibrated prediction, not a convergence guarantee.\n";

        int64_t Now() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        uint64_t Mix(uint64_t x) {
            x += 0x9e3779b97f4a7c15ull;
            x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
            x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
            return x ^ (x >> 31);
        }

        // Deterministic and unbiased: one plan always samples the same actions,
        // and different plans do not sample in the same places.
        bool SampleHit(const std::string& nonce, int id, int percent) {
            const auto seed = std::stoull(HashHex(nonce).substr(0, 16), nullptr, 16);
            return int(Mix(seed ^ (uint64_t(id) * 0x9e3779b97f4a7c15ull)) % 100) < percent;
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
            TData Data;
            TLayout Layout;
            TPrediction Prediction;
            std::vector<std::string> Commands; // digests of small action configs
            bool Corrected = false;
            // Percentage of compile actions measured by verify; PCH actions are
            // always measured. 100 measures every action, links included.
            int Sample = 100;
            // Calibration sample, not its prerequisite closure. Extra tool
            // compilations must not bias the randomly selected observations.
            // Empty means every action participates in calibration.
            std::vector<int> Measured;
        };

        double ModeQ(const std::string& mode) {
            if (mode == "global") { return 1; }
            if (mode == "incremental") { return 0.1; }
            Fail("--mode must be global or incremental: " + mode);
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

        struct TMeasureConfig {
            std::string Directory, Nonce, Command, Depfile, CacheMode;
            std::vector<std::string> Outputs;
            int Id = -1;
            double Memory = 0, Bandwidth = 0, RamLimit = 0, BandwidthLimit = 0;
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
        template <class A> void Fields(A& a, TFile& v) { a(v.Action, v.Class, v.Source, v.Own); }
        template <class A> void Fields(A& a, TData& v) { a(v.Actions, v.Units, v.Files, v.Classes, v.Scenarios, v.Q, v.Resources, v.ActionMemory); }
        template <class A> void Fields(A& a, TPch& v) { a(v.Class, v.Parent, v.Representative, v.Roots, v.Units); }
        template <class A> void Fields(A& a, TLayout& v) { a(v.Pchs, v.Bases, v.Assignment); }
        template <class A> void Fields(A& a, TPrediction& v) { a(v.Actions, v.Saved, v.Load, v.PchBytes, v.Cold, v.Incremental, v.Objective, v.Feasible); }
        template <class A> void Fields(A& a, TObservation& v) { a(v.Time, v.Cpu, v.Memory, v.Bytes, v.Start, v.End, v.Invocation); }
        template <class A> void Fields(A& a, TSnapshot& v) {
            a(v.Root, v.Baseline, v.Directory, v.Args, v.Gn, v.Ninja, v.Nonce, v.Tree, v.Tool, v.Targets, v.GnSwitches, v.Inputs, v.Flags, v.Data, v.Layout, v.Prediction, v.Commands, v.Corrected, v.Sample, v.Measured);
            if (a.HasProfile) { a(v.Mode); }
        }
        template <class A> void Fields(A& a, TAnalysis& v) { a(v.Version, v.Tree, v.Baseline, v.Snapshot); }
        template <class A> void Fields(A& a, TMeasureConfig& v) { a(v.Directory, v.Nonce, v.Command, v.Depfile, v.CacheMode, v.Outputs, v.Id, v.Memory, v.Bandwidth, v.RamLimit, v.BandwidthLimit); }
        template <class A> void Fields(A& a, TRecord& v) { a(v.Nonce, v.Digest, v.Id, v.Status, v.Observation, v.Outputs); }
        template <class A> void Fields(A& a, TRunRecord& v) { a(v.Nonce, v.Token, v.Snapshot, v.Status, v.Start, v.End); }
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
            if (version != VERSION && version != LEGACY_VERSION) { Fail("unsupported two-step snapshot version"); }
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

        std::string TreeIdentity(const std::string& root, bool analysis = false) {
            TRunOptions run;
            run.Cwd = root;
            std::vector<std::string> diff = {"git", "diff", "--binary", "HEAD", "--"};
            if (analysis) { diff.insert(diff.end(), {".", ":(exclude)build/gn/pch_update/**"}); }
            return HashHex(RunChecked({"git", "rev-parse", "HEAD"}, run) +
                           RunChecked(diff, run));
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
                                 const std::string& workDir, const TGeneration& current, int64_t maxAge) {
            // Shared caches follow source/configuration identity, not a mode's
            // age. Switching profiles alone must not invalidate cached work.
            TGeneration common;
            if (!TryReadGeneration(JoinPath(shared, "generation"), &common) ||
                !GenerationMismatch(common, current, 0).empty()) {
                RemoveFile(JoinPath(shared, "analysis.path"));
                const auto compatible = JoinPath(workDir, "compatibility-v1");
                if (IsDir(compatible)) { RemoveTree(compatible); }
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
                if (Fd < 0 || flock(Fd, LOCK_EX | LOCK_NB) != 0) { Fail("another cycle is active in " + directory); }
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

        void Claim(const std::string& path) {
            int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
            if (fd < 0) { Fail("already started, or cannot create " + path); }
            close(fd);
        }

        void ValidateInputs(const TSnapshot& s) {
            for (const auto& [path, hash] : s.Inputs) {
                if (DigestFile(path) != hash) { Fail("input changed since prediction: " + path); }
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
            // Avoid rereading tens of GB of traces. A rebuild changes the Ninja
            // logs above; also reject replaced/touched object or trace files.
            for (const auto& file : s.Data.Files) {
                auto object = JoinPath(s.Baseline, s.Data.Actions.at(file.Action).Output);
                auto dot = object.rfind('.');
                auto trace = object.substr(0, dot) + ".json";
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
                // Upgrade a complete v2 plan produced before checkpoints were
                // introduced. Without a separate analysis identity be stricter.
                s = Decode<TSnapshot>(ReadFile(JoinPath(path, "snapshot.bin")));
                if (s.Tree != TreeIdentity(s.Root)) { Fail("legacy analysis tree changed; run predict again"); }
            }
            ValidateInputs(s);
            (void)ModeQ(s.Mode);
            s.Layout = {};
            s.Prediction = {};
            s.Commands.clear();
            s.Corrected = false;
            s.Tree = TreeIdentity(s.Root);
            s.Tool = DigestFile(SelfExe());
            s.Nonce = HashHex(s.Nonce + std::to_string(Now()));
            Log("reusing checked analysis: " + std::to_string(s.Data.Files.size()) + " C++ files; skipping trace/header analysis");
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
            std::vector<int> fileMap(facts.Objs.size(), -1);
            for (size_t f = 0; f < facts.Objs.size(); ++f) {
                int edge = int(facts.EdgeOf[f]);
                if (remap[edge] < 0) { continue; }
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
                for (int dep : facts.DepsOfFile[f]) {
                    touch(a, facts.DepNames[dep]);
                    fingerprint.insert(facts.Paths.Real(facts.DepNames[dep]));
                }
                Unique(a.Changes);
                d.Files.push_back(std::move(file));
            }
            d.Classes = int(classOf.size());
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
                    unit.Safe &= facts.NeedsOfHeader.empty() || facts.NeedsOfHeader[h].empty();
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
            for (const auto& path : fingerprint) { s.Inputs.emplace_back(path, DigestFile(path)); }
        }

        std::string PchHeader(const TData& data, const TPch& pch) {
            std::string header = "#pragma once\n";
            for (int root : pch.Roots) { header += "#include " + data.Units[root].Include + "\n"; }
            return header;
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

        void Emit(TSnapshot& s, const TCompileInputs& probeInputs) {
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
                for (size_t i = 0; i < probeInputs.size(); ++i) {
                    for (const auto& input : probeInputs[i]) {
                        auto path = generated(input);
                        if (!path.empty()) { generatedProbes.at(i).push_back(std::move(path)); }
                    }
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
            auto measured = [&](int id, TMeasureConfig config) {
                config.Id = id;
                config.Directory = s.Directory;
                config.Nonce = s.Nonce;
                const auto& prediction = s.Prediction.Actions[id];
                config.Memory = prediction.Memory;
                config.RamLimit = d.Resources.Ram;
                config.BandwidthLimit = d.Resources.Bandwidth;
                config.Bandwidth = prediction.Io / std::max(0.001, std::max(prediction.Time, prediction.Io / d.Resources.Bandwidth));
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
                            int g = l.Assignment[f];
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
                    RuleBinding("depfile", config.Depfile) + "  deps = gcc\n";
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
            // complete graph. Below it, every PCH (all consumers need them) and
            // a deterministic share of compilations. Generators and their tools
            // retain their actual inputs, including any necessary links.
            std::vector<std::string> all;
            s.Measured.clear();
            for (size_t i = 0; i < s.Prediction.Actions.size(); ++i) {
                const auto& action = s.Prediction.Actions[i];
                bool take = s.Sample >= 100;
                if (!take) {
                    const bool pch = int(i) >= int(d.Actions.size());
                    take = pch || (fileOf[i] >= 0 && SampleHit(s.Nonce, int(i), s.Sample));
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
            sampling << "id\toutput\trole\tkind\n";
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
                         << '\t' << (i >= d.Actions.size() ? "pch" : compiler ? "compile" : "action") << '\n';
            }
            sampling << "# sampled_compilations\t" << selectedCompilers
                     << "\n# required_compilations\t" << compilers
                     << "\n# required_commands\t" << commands << "\n# full_commands\t" << full << '\n';
            WriteFile(JoinPath(s.Directory, "sampling.tsv"), sampling.str());
            Log(Format("measurement sample %d%%: %zu selected compilations, %zu including prerequisites; %zu/%zu commands",
                       s.Sample, selectedCompilers, compilers, commands, full));
            WriteFile(JoinPath(build, "measured.ninja"), ninja);
            s.Inputs.emplace_back(JoinPath(build, "measured.ninja"), DigestFile(JoinPath(build, "measured.ninja")));
            s.Inputs.emplace_back(JoinPath(build, "args.gn"), DigestFile(JoinPath(build, "args.gn")));
            for (size_t g = 0; g < l.Pchs.size(); ++g) {
                std::string path = JoinPath(build, "pch/" + std::to_string(g) + ".h");
                s.Inputs.emplace_back(path, DigestFile(path));
            }
        }

        // Cross-process admission enforces the same memory/bandwidth reservations
        // as the simulator. Ninja itself enforces worker and named-pool limits.
        class TAdmission {
        public:
            explicit TAdmission(const TMeasureConfig& c) {
                Path_ = JoinPath(c.Directory, "admission");
                Fd_ = open(Path_.c_str(), O_CREAT | O_RDWR, 0644);
                if (Fd_ < 0) { Fail("cannot open admission file"); }
                if (c.Memory > c.RamLimit || c.Bandwidth > c.BandwidthLimit * (1 + 1e-9)) {
                    Fail("action cannot fit resource limits");
                }
                while (true) {
                    Lock();
                    auto active = Read();
                    double memory = 0, bandwidth = 0;
                    for (const auto& row : active) { memory += row.Memory; bandwidth += row.Bandwidth; }
                    if (memory + c.Memory <= c.RamLimit && bandwidth + c.Bandwidth <= c.BandwidthLimit * (1 + 1e-9)) {
                        active.push_back({getpid(), c.Memory, c.Bandwidth});
                        Write(active);
                        flock(Fd_, LOCK_UN);
                        return;
                    }
                    flock(Fd_, LOCK_UN);
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }
            ~TAdmission() {
                if (Fd_ < 0) { return; }
                if (flock(Fd_, LOCK_EX) == 0) {
                    try {
                        auto active = Read();
                        active.erase(std::remove_if(active.begin(), active.end(), [](const auto& row) { return row.Pid == getpid(); }), active.end());
                        Write(active);
                    } catch (...) {
                        // The next admission drops dead processes as well.
                    }
                    flock(Fd_, LOCK_UN);
                }
                close(Fd_);
            }
        private:
            struct TRow { int Pid; double Memory, Bandwidth; };
            int Fd_ = -1;
            std::string Path_;
            void Lock() { if (flock(Fd_, LOCK_EX) != 0) { Fail("cannot lock admission file"); } }
            std::vector<TRow> Read() {
                std::istringstream in(ReadFile(Path_));
                std::vector<TRow> result;
                TRow row;
                while (in >> row.Pid >> row.Memory >> row.Bandwidth) {
                    if (kill(row.Pid, 0) == 0 || errno == EPERM) { result.push_back(row); }
                }
                return result;
            }
            void Write(const std::vector<TRow>& rows) {
                std::ostringstream out;
                out << std::setprecision(17);
                for (const auto& row : rows) { out << row.Pid << ' ' << row.Memory << ' ' << row.Bandwidth << '\n'; }
                auto text = out.str();
                if (ftruncate(Fd_, 0) != 0 || pwrite(Fd_, text.data(), text.size(), 0) != ssize_t(text.size())) {
                    Fail("cannot save admission reservations");
                }
            }
        };

        int Measure(const std::string& path) {
            auto bytes = ReadFile(path);
            auto c = Decode<TMeasureConfig>(bytes);
            const char* token = getenv("PCH_UPDATE_RUN");
            if (!token || !*token) { Fail("run this manifest with pch_update verify --plan DIR"); }
            auto run = Decode<TRunRecord>(ReadFile(JoinPath(c.Directory, "run.bin")));
            if (run.Nonce != c.Nonce || run.Token != token || run.End) { Fail("stale verification invocation"); }
            std::string recordPath = JoinPath(c.Directory, "observations/" + std::to_string(c.Id));
            Claim(recordPath + ".claim");
            TAdmission admission(c);
            TRecord record;
            record.Id = c.Id;
            record.Nonce = c.Nonce;
            record.Digest = HashHex(bytes);
            record.Observation.Invocation = token;
            TRunOptions options;
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
                       const std::function<TRunResult(const std::vector<std::string>&, const TRunOptions&)>& run = Run) {
                const auto dir = Directory(key);
                MakeDirs(dir);
                struct TLock {
                    int Fd;
                    ~TLock() { if (Fd >= 0) { close(Fd); } }
                } lock{open(JoinPath(dir, "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644)};
                if (lock.Fd < 0 || flock(lock.Fd, LOCK_EX) != 0) { Fail("cannot lock compatibility cache: " + dir); }
                const auto artifact = header.empty() ? std::string() : JoinPath(dir, "output.pch");
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
                        if (valid) { ++Hits; return true; }
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
                return true;
            }

            std::atomic<size_t> Hits{0}, Runs{0};
        private:
            std::string Root_;
            std::mutex Mutex_;
            std::map<std::string, std::string> Digests_;
        };

        int CompatibleAssignment(const TLayout& layout, int group, const std::vector<char>& built,
                                 const std::function<bool(int)>& check) {
            for (int g = group; g >= 0; g = layout.Pchs[g].Parent) {
                if (built[g] && check(g)) { return g; }
            }
            if (!check(-1)) { Fail("compatibility check also fails without PCH; see compatibility diagnostics"); }
            return -1;
        }

        std::vector<int> PruneCompatible(TLayout& layout, bool frozen) {
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

        TCompileInputs Preflight(TSnapshot& s, const std::string& workDir, const TCalibration& calibration = {}) {
            auto& layout = s.Layout;
            TCompileInputs inputs(s.Data.Actions.size() + layout.Pchs.size());
            if (layout.Pchs.empty()) { return inputs; }
            const auto cacheRoot = JoinPath(workDir, "compatibility-v1");
            MakeDirs(cacheRoot);
            if (!StorageAllowed(RealPath(cacheRoot), s.Root, s.Baseline)) {
                Fail("compatibility cache must be outside the source tree and baseline, or inside BASELINE/.pch_update/");
            }
            TCompatibilityCache cache(cacheRoot);
            // Use validated records on both cache hits and fresh probes. These
            // include headers exposed by a PCH that baseline depfiles cannot see.
            auto probeInputs = [&](const std::string& key) {
                std::vector<std::string> result;
                if (s.Sample >= 100) { return result; }
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
            // Scope all results to the validated analysis. This deliberately
            // invalidates the cache on any analysis-input change, but not on
            // selector/emitter edits. Probe depfiles additionally cover headers
            // introduced by combinations absent from baseline compilations.
            TBinWriter context;
            context.Str("compatibility-v1");
            context.Str(s.Baseline);
            context.Str(TreeIdentity(s.Root, true));
            context.Str(BaselineIdentity(s));
            for (const auto& [path, hash] : s.Inputs) { context.Str(path); context.Str(hash); }
            for (const char* name : {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "SDKROOT",
                                     "SOURCE_DATE_EPOCH", "LANG", "LC_ALL", "LC_CTYPE"}) {
                const char* value = getenv(name);
                context.Str(name);
                context.Str(value ? value : "");
            }
            const auto identity = HashHex(context.Data());
            std::vector<std::string> keys(layout.Pchs.size()), artifacts(layout.Pchs.size());
            std::vector<char> built(layout.Pchs.size());
            auto prediction = Predict(s.Data, layout, calibration);
            double memory = s.Data.ActionMemory;
            for (size_t g = 0; g < layout.Pchs.size(); ++g) {
                memory = std::max(memory, prediction.Actions.at(s.Data.Actions.size() + g).Memory);
            }
            for (size_t f = 0; f < layout.Assignment.size(); ++f) {
                if (layout.Assignment[f] >= 0) {
                    memory = std::max(memory, prediction.Actions.at(s.Data.Files[f].Action).Memory);
                }
            }
            if (memory > s.Data.Resources.Ram) { Fail("compatibility action cannot fit RAM budget"); }
            std::ostringstream report;
            report << "kind\tsource_or_group\told_leaf\tnew_leaf_before_compaction\tdiagnostics\n";
            Log("compatibility: checking " + std::to_string(layout.Pchs.size()) + " PCHs; cache " + RealPath(cacheRoot));
            for (size_t g = 0; g < layout.Pchs.size(); ++g) {
                const auto& pch = layout.Pchs[g];
                if (pch.Parent >= 0 && !built[pch.Parent]) { continue; }
                auto argv = flags.at(pch.Representative);
                const auto header = PchHeader(s.Data, pch);
                TBinWriter key;
                key.Str(identity);
                key.Str(cache.Digest(argv.front()));
                key.Str(ShellJoin(argv));
                key.Str(header);
                key.Str(pch.Parent < 0 ? "" : keys[pch.Parent]);
                key.Str(pch.Parent < 0 ? "" : cache.Digest(artifacts[pch.Parent]));
                keys[g] = "pch-" + HashHex(key.Data());
                const auto dir = cache.Directory(keys[g]);
                artifacts[g] = JoinPath(dir, "output.pch");
                if (pch.Parent >= 0) { argv.insert(argv.end(), {"-include-pch", artifacts[pch.Parent]}); }
                argv.insert(argv.end(), {"-x", "c++-header", JoinPath(dir, "input.h"), "-o", artifacts[g]});
                built[g] = cache.Probe(keys[g], s.Baseline, argv, header);
                if (built[g]) { inputs[s.Data.Actions.size() + g] = probeInputs(keys[g]); }
                report << "pch\t" << g << "\t" << g << "\t" << (built[g] ? int(g) : -1)
                       << "\t" << JoinPath(dir, "compiler.log") << '\n';
                WriteFile(JoinPath(s.Directory, "compatibility.tsv"), report.str());
                Progress(Format("compatibility PCH %zu/%zu: %s", g + 1, layout.Pchs.size(), built[g] ? "OK" : "rejected"));
                if (!built[g] && s.Corrected && int(g) < layout.Bases) {
                    Fail("frozen base failed compatibility checking; see " + JoinPath(dir, "compiler.log"));
                }
            }
            size_t jobs = std::max(1, std::min(s.Data.Resources.Workers, int(std::min(
                double(s.Data.Resources.Workers), std::floor(s.Data.Resources.Ram / std::max(1.0, memory))))));
            std::vector<size_t> files;
            for (size_t f = 0; f < layout.Assignment.size(); ++f) { if (layout.Assignment[f] >= 0) { files.push_back(f); } }
            std::vector<std::string> rows(files.size());
            std::atomic<size_t> done{0}, changed{0};
            Log(Format("compatibility: checking %zu consumers, %zu workers", files.size(), jobs));
            ParallelFor(files.size(), jobs, [&](size_t i) {
                const auto f = files[i];
                const int original = layout.Assignment[f];
                std::string diagnostics;
                auto probe = [&](int g) {
                    auto argv = flags[f];
                    if (g >= 0) { argv.insert(argv.end(), {"-include-pch", artifacts[g]}); }
                    argv.insert(argv.end(), {"-fsyntax-only", "-Wno-unused-command-line-argument", sources[f]});
                    const auto key = "tu-" + HashHex(identity + cache.Digest(argv.front()) + ShellJoin(argv) +
                        (g < 0 ? "" : cache.Digest(artifacts[g])));
                    const auto log = JoinPath(cache.Directory(key), "compiler.log");
                    bool ok = cache.Probe(key, s.Baseline, argv);
                    if (ok) { inputs[s.Data.Files[f].Action] = probeInputs(key); }
                    if (!ok) { diagnostics += (diagnostics.empty() ? "" : ";") + log; }
                    if (g < 0 && !ok) { Fail("compatibility failed without PCH: " + s.Data.Files[f].Source + "; see " + log); }
                    return ok;
                };
                const int selected = CompatibleAssignment(layout, original, built, probe);
                layout.Assignment[f] = selected;
                changed += selected != original;
                rows[i] = "tu\t" + s.Data.Files[f].Source + "\t" + std::to_string(original) + "\t" +
                    std::to_string(selected) + "\t" + diagnostics + "\n";
                auto count = ++done;
                if (count % 100 == 0 || count == files.size()) {
                    Progress(Format("compatibility consumers %zu/%zu, changed %zu, cache hits %zu",
                        count, files.size(), changed.load(), cache.Hits.load()));
                }
            });
            for (const auto& row : rows) { report << row; }
            WriteFile(JoinPath(s.Directory, "compatibility.tsv"), report.str());
            const auto remap = PruneCompatible(layout, s.Corrected);
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
            Log(Format("compatibility complete: %zu assignments changed, %zu PCHs retained, %zu cache hits, %zu compiler runs",
                 changed.load(), layout.Pchs.size(), cache.Hits.load(), cache.Runs.load()));
            return inputs;
        }

        void Summary(TSnapshot& s) {
            std::ostringstream out;
            out << std::setprecision(10)
                << "phase\t" << (s.Corrected ? "corrected-prediction" : "initial-prediction")
                << "\nmode\t" << s.Mode << "\nq\t" << s.Data.Q
                << "\nnonce\t" << s.Nonce << "\ncold_seconds\t" << s.Prediction.Cold
                << "\nincremental_seconds\t" << s.Prediction.Incremental << "\nobjective_seconds\t" << s.Prediction.Objective
                << "\nbases\t" << s.Layout.Bases << "\npchs\t" << s.Layout.Pchs.size()
                << "\nworkers\t" << s.Data.Resources.Workers << "\nram_bytes\t" << s.Data.Resources.Ram
                << "\npch_disk_bytes_budget\t" << s.Data.Resources.Disk
                << "\nsample_percent\t" << s.Sample << "\n";
            if (s.Sample < 100) {
                out << "# Verification measures a deterministic share of compilations: " << s.Measured.size()
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
                ledger << i << '\t' << a.Output << '\t' << a.Time << '\t' << a.Memory << '\t' << a.Io << '\t' << s.Commands[i] << '\n';
            }
            WriteFile(JoinPath(s.Directory, "predictions.tsv"), ledger.str());
            std::ostringstream assignments;
            assignments << "source\tclass\tleaf\n";
            for (size_t f = 0; f < s.Data.Files.size(); ++f) {
                assignments << s.Data.Files[f].Source << '\t' << s.Data.Files[f].Class << '\t' << s.Layout.Assignment[f] << '\n';
            }
            WriteFile(JoinPath(s.Directory, "assignments.tsv"), assignments.str());
            Report(out.str());
        }

        TSnapshot LoadPlan(const std::string& path) {
            auto s = Decode<TSnapshot>(ReadFile(JoinPath(path, "snapshot.bin")));
            (void)ModeQ(s.Mode);
            if (RealPath(s.Directory) != RealPath(path)) { Fail("a plan cannot be relocated without preserving its frozen absolute paths"); }
            return s;
        }

        int CreatePlan(TSnapshot& s, const std::string& output, const std::string& workDir) {
            ValidateIdentity(s);
            CheckProfileOutput(s.Baseline, s.Mode, output);
            NewDirectory(output, s.Root, s.Baseline);
            s.Directory = RealPath(output);
            // Durable boundary before selection and GN/Ninja emission: even a
            // failed plan can be replanned without repeating expensive analysis.
            SaveAnalysis(s);
            // Only a plan inside the baseline state directory becomes the
            // default analysis checkpoint. A diagnostic --output elsewhere may
            // be temporary, and pointing the baseline at it loses the analysis.
            if (Under(s.Directory, StateDirectory(s.Baseline))) {
                WriteFile(JoinPath(StateDirectory(s.Baseline), "analysis.path"), s.Directory + "\n");
                WriteFile(JoinPath(ProfileDirectory(s.Baseline, s.Mode), "analysis.path"), s.Directory + "\n");
            }
            s.Layout = Select(s.Data, {});
            const auto probeInputs = Preflight(s, workDir);
            s.Prediction = Predict(s.Data, s.Layout, {});
            Emit(s, probeInputs);
            ValidateIdentity(s);
            Summary(s);
            WriteFile(JoinPath(output, "snapshot.bin"), Encode(s));
            Report("verification: " + ShellJoin({SelfExe(), "verify", "--plan", output}));
            return 0;
        }

        int Verify(TSnapshot& s) {
            ValidateIdentity(s);
            for (size_t i = 0; i < s.Commands.size(); ++i) {
                if (!s.Prediction.Actions[i].Phony &&
                    HashHex(ReadFile(JoinPath(s.Directory, "actions/" + std::to_string(i) + ".bin"))) != s.Commands[i]) {
                    Fail("action configuration changed since prediction");
                }
            }
            if (Exists(JoinPath(s.Directory, "build/.ninja_log"))) { Fail("verification requires a fresh, unbuilt plan"); }
            Claim(JoinPath(s.Directory, "verification.claim"));
            TRunRecord run;
            run.Nonce = s.Nonce;
            run.Token = HashHex(s.Nonce + std::to_string(Now()) + std::to_string(getpid()));
            run.Snapshot = DigestFile(JoinPath(s.Directory, "snapshot.bin"));
            run.Start = Now();
            WriteFile(JoinPath(s.Directory, "run.bin"), Encode(run));
            TRunOptions options;
            options.CaptureStdout = options.CaptureStderr = false;
            options.Env = {"PCH_UPDATE_RUN=" + run.Token};
            run.Status = Run({s.Ninja, "-C", JoinPath(s.Directory, "build"), "-f", "measured.ninja", "-j",
                              std::to_string(s.Data.Resources.Workers), "_pch_verify"}, options).ExitCode;
            run.End = Now();
            WriteFile(JoinPath(s.Directory, "run.bin"), Encode(run));
            ValidateIdentity(s);
            return run.Status;
        }

        std::vector<TObservation> ReadObservations(const TSnapshot& s, std::string& report) {
            auto run = Decode<TRunRecord>(ReadFile(JoinPath(s.Directory, "run.bin")));
            if (run.Nonce != s.Nonce || run.Status != 0 || run.End <= run.Start ||
                run.Snapshot != DigestFile(JoinPath(s.Directory, "snapshot.bin"))) {
                Fail("verification was not completed successfully against this snapshot");
            }
            std::vector<TObservation> result(s.Prediction.Actions.size());
            std::vector<char> compiler(result.size());
            for (const auto& file : s.Data.Files) { compiler[file.Action] = true; }
            for (size_t i = s.Data.Actions.size(); i < result.size(); ++i) { compiler[i] = true; }
            // A sampled plan builds only part of the graph; the rest keeps no
            // observation and is predicted from the model. An empty list means
            // every action was measured.
            std::vector<char> measured(result.size(), s.Measured.empty() ? 1 : 0);
            for (int i : s.Measured) { measured[i] = 1; }
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
                    HashHex(configBytes) != s.Commands[i] || r.Observation.Invocation != run.Token ||
                    r.Observation.Start < run.Start || r.Observation.End > run.End ||
                    r.Observation.End <= r.Observation.Start || r.Observation.Time <= 0 ||
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
            errors << "# initial_action_WAPE\t" << absolute / std::max(1e-9, total)
                   << "\n# predicted_cold_seconds\t" << s.Prediction.Cold
                   << "\n# measured_build_wall_seconds\t" << (run.End - run.Start) / 1e9
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
        int CorrectPlan(TSnapshot& s, const std::string& plan, const std::string& output, const std::string& workDir) {
            if (s.Corrected) { Fail("correction is one step; start a new predict cycle to change the base"); }
            CheckProfileOutput(s.Baseline, s.Mode, output);
            ValidateIdentity(s);
            std::string errors;
            auto observations = ReadObservations(s, errors);
            auto calibration = Calibrate(s.Data, s.Layout, s.Prediction, observations);
            auto layout = Select(s.Data, calibration, &s.Layout);
            NewDirectory(output, s.Root, s.Baseline);
            s.Directory = RealPath(output);
            s.Nonce = HashHex(s.Nonce + "corrected");
            s.Corrected = true;
            s.Layout = std::move(layout);
            SaveAnalysis(s);
            const auto probeInputs = Preflight(s, workDir, calibration);
            s.Prediction = Predict(s.Data, s.Layout, calibration);
            Emit(s, probeInputs);
            ValidateIdentity(s);
            WriteFile(JoinPath(output, "initial-errors.tsv"), errors);
            WriteFile(JoinPath(output, "calibration.tsv"), calibration.Report);
            Summary(s);
            Claim(JoinPath(plan, "correction.claim"));
            WriteFile(JoinPath(output, "snapshot.bin"), Encode(s));
            Report("corrected prediction: " + output);
            return 0;
        }

        bool Verified(const TSnapshot& s) {
            auto path = JoinPath(s.Directory, "run.bin");
            if (!Exists(path)) { return false; }
            auto run = Decode<TRunRecord>(ReadFile(path));
            return run.Status == 0 && run.End > run.Start && run.Nonce == s.Nonce &&
                run.Snapshot == DigestFile(JoinPath(s.Directory, "snapshot.bin"));
        }

        int FinishCycle(TSnapshot& s, const std::string& cycle, const std::string& workDir) {
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
                int status = CorrectPlan(s, initial, output, workDir);
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
        auto encoded = Encode(snapshot);
        auto loaded = Decode<TSnapshot>(encoded);
        check(loaded.Data.Files[0].Own[0].second == 2.5 && loaded.Data.Resources.Pools.at("compile") == 2,
              "sparse costs and pool depths survive a snapshot");
        check(loaded.Sample == 40 && loaded.Measured == std::vector<int>({1, 2, 3}),
              "measurement sample survives a snapshot");
        check(loaded.Mode == "global", "profile survives a frozen snapshot");
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
        check(Decode<TSnapshot>(legacy(snapshot)).Mode == "incremental",
              "legacy snapshots use the default profile without discarding data");
        TAnalysis oldAnalysis;
        oldAnalysis.Snapshot = snapshot;
        check(Decode<TAnalysis>(legacy(oldAnalysis)).Snapshot.Sample == 40,
              "legacy analysis checkpoints remain reusable across profiles");
        TCompatibilityRecord oldProbe;
        oldProbe.Command = "cached probe";
        check(Decode<TCompatibilityRecord>(legacy(oldProbe)).Command == oldProbe.Command,
              "profile serialization preserves old compatibility cache records");
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
        size_t hits = 0, all = 0;
        for (int i = 0; i < 10000; ++i) { hits += SampleHit("sample-nonce", i, 10); }
        for (int i = 0; i < 100; ++i) { all += SampleHit("sample-nonce", i, 100); }
        check(hits > 700 && hits < 1300, "sampling follows the requested share");
        check(all == 100, "100 % measures every action");
        check(SampleHit("sample-nonce", 42, 10) == SampleHit("sample-nonce", 42, 10),
              "sampling is deterministic for one plan");
        check(loaded.Sample == 40 && loaded.Measured == std::vector<int>({1, 2, 3}),
              "measurement sample survives a snapshot");
        encoded.back() ^= 1;
        bool rejected = false;
        try { Decode<TSnapshot>(encoded); } catch (const TError&) { rejected = true; }
        check(rejected, "modified snapshot rejected before use");
        auto flags = CompileFlags({"/tool/clang++", "-DNAME=1", "-Igen", "-MD", "-MF", "x.d", "-c", "x.cpp", "-o", "x.o"}, "x.cpp");
        check(flags == std::vector<std::string>({"/tool/clang++", "-DNAME=1", "-Igen"}), "PCH flags preserve macro/include order");
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
            PrepareProfileState(shared, global, cache, generation, 60);
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
            PrepareProfileState(shared, global, cache, generation, 60);
            check(Exists(JoinPath(cache, "compatibility-v1/entry")) &&
                  ReadPointer(JoinPath(incremental, "result.path")) == otherPlan &&
                  Exists(JoinPath(otherPlan, "snapshot.bin")),
                  "age expiry is profile-local and preserves the other saved variant");
            generation.Tree = "changed";
            PrepareProfileState(shared, global, cache, generation, 60);
            check(!Exists(JoinPath(cache, "compatibility-v1")) &&
                  !Exists(JoinPath(shared, "analysis.path")) && Exists(JoinPath(otherPlan, "snapshot.bin")) &&
                  Exists(JoinPath(shared, "runs/legacy/initial/snapshot.bin")),
                  "new shared inputs invalidate caches without deleting other or legacy results");
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
        std::vector<int> attempted;
        auto parentOnly = [&](int g) { attempted.push_back(g); return g == 1; };
        check(CompatibleAssignment(compatibility, 2, {true, true, true, false}, parentOnly) == 1 &&
              attempted == std::vector<int>({2, 1}), "incompatible leaf falls back to a validated parent");
        attempted.clear();
        check(CompatibleAssignment(compatibility, 3, {true, true, true, false}, parentOnly) == 1 &&
              attempted == std::vector<int>{1}, "failed PCH is never loaded into a consumer");
        check(CompatibleAssignment(compatibility, 2, {true, true, true, false}, [](int g) { return g < 0; }) == -1,
              "an incompatible chain falls back to a validated no-PCH compile");
        rejected = false;
        try { CompatibleAssignment(compatibility, 2, {true, true, true, false}, [](int) { return false; }); }
        catch (const TError&) { rejected = true; }
        check(rejected, "a no-PCH failure cannot be disguised as an incompatible assignment");
        compatibility.Assignment = {1, -1};
        auto frozenCompatibility = compatibility;
        PruneCompatible(compatibility, false);
        check(compatibility.Bases == 1 && compatibility.Pchs.size() == 1 &&
              compatibility.Pchs[0].Roots == std::vector<int>{11} && compatibility.Assignment == std::vector<int>({0, -1}),
              "compatibility pruning removes unused chains and remaps consumers");
        PruneCompatible(frozenCompatibility, true);
        check(frozenCompatibility.Bases == 2 && frozenCompatibility.Pchs.size() == 2 &&
              frozenCompatibility.Pchs[0].Roots == std::vector<int>{10} && frozenCompatibility.Assignment[0] == 1,
              "correction retains an unused frozen base and its ID");

        auto probeRoot = JoinPath(dir, "compatibility-cache");
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
        config.RamLimit = config.BandwidthLimit = 1e9;
        auto path = JoinPath(dir, "action.bin");
        WriteFile(path, Encode(config));
        setenv("PCH_UPDATE_RUN", run.Token.c_str(), 1);
        check(Measure(path) == 0, "measurement executes the entire compound shell command");
        auto record = Decode<TRecord>(ReadFile(JoinPath(dir, "observations/0.bin")));
        check(record.Outputs[0].first == 5 && record.Outputs[1].first == 6 && record.Observation.Invocation == run.Token &&
              record.Observation.Time > 0, "measurement records outputs, invocation and elapsed time");
        rejected = false;
        try { Measure(path); } catch (const TError&) { rejected = true; }
        check(rejected, "a second observation cannot silently replace the first");
    }

    int Command(const std::vector<std::string>& args) {
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
        s.Data.Resources.Bandwidth = 1e9;
        options.Jobs = int(std::min(size_t(24), CpuCount()));
        std::string output, plan;
        bool targets = false, extractionOverride = false, diskGiven = false;
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
            else if (key == "--workers") { s.Data.Resources.Workers = integer(); }
            else if (key == "--analysis-jobs") { options.Jobs = integer(); }
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
            else if (key == "--bandwidth-gbps") { s.Data.Resources.Bandwidth = number() * 1e9; }
            else if (key == "--action-memory-gb") { s.Data.ActionMemory = number() * 1e9; }
            else if (key == "--sample") {
                s.Sample = integer();
                if (s.Sample < 10 || s.Sample > 100) { Fail("--sample PERCENT must be between 10 and 100"); }
            }
            else { Fail("unknown option " + key); }
        }
        TCycleLock cycleLock;
        std::string cycle;
        if (command == "predict") {
            profile.Apply(s);
            if (options.Baseline.empty() || (!fullCycle && output.empty()) || s.Data.Resources.Ram <= 0 || !diskGiven ||
                s.Data.Resources.Bandwidth <= 0 || s.Data.ActionMemory <= 0 || s.Data.Q > 1) {
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
                PrepareProfileState(state, modeState, options.WorkDir, current, options.StateAge);
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
                    if (resumable) { return FinishCycle(saved, previous, options.WorkDir); }
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
                        cached.Data.Resources.Bandwidth = s.Data.Resources.Bandwidth;
                        cached.Data.Q = s.Data.Q;
                        cached.Mode = s.Mode;
                        cached.Data.ActionMemory = s.Data.ActionMemory;
                        cached.Sample = s.Sample;
                        for (auto& action : cached.Data.Actions) {
                            action.Memory = action.Phony ? 0 : cached.Data.ActionMemory;
                        }
                        if (!cached.Data.Scenarios && cached.Data.Q != 1) { Fail("no history scenarios; use --q 1"); }
                        int status = CreatePlan(cached, output, options.WorkDir);
                        return status ? status : FinishCycle(cached, cycle, options.WorkDir);
                    }
                }
            }
            options.Q = s.Data.Q;
            options.Ninja = tools.Ninja;
            TWorkDir work(options.WorkDir, tools.Ninja, options.Jobs);
            TFacts facts;
            facts.Root = s.Root;
            if (!CheckBaseline(options, tools, work, facts)) { return 1; }
            s.Tree = TreeIdentity(facts.Root);
            if (!ReadTraces(options, work, facts) ||
                !ReadHistory(options, facts) || !FlagClasses(options, facts) || !HeaderFacts(options, work, facts)) {
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
            int status = CreatePlan(s, output, options.WorkDir);
            return status || !fullCycle ? status : FinishCycle(s, cycle, options.WorkDir);
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
            return CreatePlan(s, output, options.WorkDir);
        }
        s = LoadPlan(plan);
        if (command == "verify") { return Verify(s); }
        if (output.empty()) { Fail("correct requires a new --output directory"); }
        if (options.WorkDir.empty()) { options.WorkDir = JoinPath(StateDirectory(s.Baseline), "cache"); }
        return CorrectPlan(s, plan, output, CacheDirectory(options.WorkDir));
    }
} // namespace NPchUpdate::NTwoStep
