#include "workflow.h"

#include "common.h"
#include "gni.h"
#include "ninja.h"
#include "pch_model.h"
#include "steps.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace NPchUpdate {

    namespace {

        constexpr const char* WORKFLOW_STATE_TAG = "pch-workflow-state-1";

        const char* HELP =
            "pch_update --baseline DIR [--predict | --apply] [options]\n"
            "  Default: predict and apply a two-level (base/delta) global PCH layout.\n"
            "  --predict               prepare/analyze baseline, greedily select & build real PCHs, check TUs\n"
            "  --apply                 export groups.gni, replace global PCH tree, validate and clean scratch\n"
            "  --min-ratio R           min net saved CPU-seconds per 1 MB of PCH (default: 0.5)\n"
            "  --disk SIZE             optional resident PCH cap (e.g. 16G, 800M; 0 = unbounded, default: 0)\n"
            "  --mode MODE             incremental or global (default: incremental)\n"
            "  --q Q                   cold-build weight in [0, 1]; overrides mode default (global: 1, incremental: 0.1)\n"
            "  --workers N             -j for baseline, TU checks and validation build; PCH builds use max(1, N/4)\n"
            "  --analysis-jobs N       in-process analysis threads (default: --workers)\n"
            "  --ram-gb N              RAM budget shaping default --workers (default: 75% available RAM)\n"
            "  --action-memory-gb N    per-action RSS prior for default --workers (default: 1)\n"
            "  --verify-build MODE     full, pch or none on --apply (default: full)\n"
            "  --target T              repeatable build target (default: all, tests)\n"
            "  --main-ref REF          git history base ref (default: origin/main)\n"
            "  --gn PATH --ninja PATH  tool executables\n"
            "  --verbose               show detailed progress and full child output (presentation only)\n"
            "pch_update self-test      run internal unit and workflow self-tests\n";

        int64_t NowNs() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        }

        std::string TrimText(std::string text) {
            const auto last = text.find_last_not_of(" \t\r\n");
            if (last == std::string::npos) {
                return {};
            }
            text.erase(last + 1);
            text.erase(0, text.find_first_not_of(" \t\r\n"));
            return text;
        }

        std::string ReadPointerFile(const std::string& path) {
            std::string text;
            if (!TryReadFile(path, &text)) {
                return {};
            }
            return TrimText(std::move(text));
        }

        std::string WorktreeRoot() {
            return RealPath(TrimText(RunChecked({"git", "rev-parse", "--show-toplevel"})));
        }

        std::pair<std::string, std::string> ReadCliOption(const std::vector<std::string>& args, size_t& i) {
            const std::string& key = args[i];
            const size_t eq = key.find('=');
            if (eq != std::string::npos) {
                return {key.substr(0, eq), key.substr(eq + 1)};
            }
            if (++i >= args.size()) {
                Fail("missing value for " + key);
            }
            return {key, args[i]};
        }

        double ParseDoubleOption(const std::string& key, const std::string& value, double minVal) {
            char* end = nullptr;
            const double n = std::strtod(value.c_str(), &end);
            if (value.empty() || *end != '\0' || !std::isfinite(n) || n < minVal) {
                Fail("invalid number for " + key + ": " + value);
            }
            return n;
        }

        int ParseIntOption(const std::string& key, const std::string& value, int minVal, int maxVal) {
            const double n = ParseDoubleOption(key, value, double(minVal));
            if (n > double(maxVal) || std::floor(n) != n) {
                Fail(Format("%s requires an integer in [%d, %d]: %s", key.c_str(), minVal, maxVal, value.c_str()));
            }
            return int(n);
        }

        // Streamed chunk-invariant hashing for `git diff --binary HEAD`.
        struct TStreamHasher {
            THasher Hash;
            std::string Pending;

            void Feed(std::string_view chunk) {
                Pending.append(chunk);
                const size_t whole = Pending.size() & ~size_t(7);
                if (whole > 0) {
                    Hash.Update(std::string_view(Pending).substr(0, whole));
                    Pending.erase(0, whole);
                }
            }

            std::string Hex() {
                Hash.Update(Pending);
                Pending.clear();
                return Hash.Hex();
            }
        };

        std::string ResolveCgroupV2Dir() {
            std::string content;
            if (TryReadFile("/proc/self/cgroup", &content)) {
                for (std::string_view line : Split(content, '\n')) {
                    if (StartsWith(line, "0::")) {
                        std::string sub(line.substr(3));
                        if (!sub.empty() && sub != "/") {
                            std::string dir = JoinPath("/sys/fs/cgroup", sub.front() == '/' ? sub.substr(1) : sub);
                            if (IsDir(dir)) {
                                return dir;
                            }
                        }
                    }
                }
            }
            return "/sys/fs/cgroup";
        }

        double ReadReclaimableFileCache(const std::string& cgroupDir) {
            std::string statText;
            if (!TryReadFile(JoinPath(cgroupDir, "memory.stat"), &statText)) {
                return 0.0;
            }
            for (std::string_view line : Split(statText, '\n')) {
                if (StartsWith(line, "inactive_file ")) {
                    double bytes = 0.0;
                    std::istringstream in{std::string(line.substr(14))};
                    if (in >> bytes && bytes > 0.0) {
                        return bytes;
                    }
                }
            }
            return 0.0;
        }

        std::string InstallationDir(const std::string& root, const std::string& planDir) {
            const std::string directory = ReadPointerFile(JoinPath(planDir, "installing"));
            const std::string parent = DirName(JoinPath(root, GLOBAL_DIR));
            if (!directory.empty() && DirName(directory) == parent && StartsWith(BaseName(directory), ".pch-install-")) {
                return directory;
            }
            return {};
        }

        void ClearPlanOutputs(const std::string& planDir, bool dropAnalysis) {
            if (!IsDir(planDir)) {
                return;
            }
            for (const std::string& entry : ListDir(planDir)) {
                if (entry == "analysis.bin" && !dropAnalysis) {
                    continue;
                }
                RemoveTree(JoinPath(planDir, entry));
            }
        }

        bool PredictionInputsEqual(TWorkflowConfig a, TWorkflowConfig b) {
            a.VerifyBuild = EVerifyBuild::Full;
            b.VerifyBuild = EVerifyBuild::Full;
            return a == b;
        }

        bool ExtractionInputsEqual(const TWorkflowConfig& a, const TWorkflowConfig& b) {
            return a.MainRef == b.MainRef && a.Targets == b.Targets && a.Gn == b.Gn && a.Ninja == b.Ninja;
        }

    } // namespace

    TDirLock::~TDirLock() {
        if (Fd_ >= 0) {
            close(Fd_);
        }
    }

    void TDirLock::Acquire(const std::string& directory, const std::string& fileName) {
        MakeDirs(directory);
        const std::string lockPath = JoinPath(directory, fileName);
        Fd_ = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
        if (Fd_ < 0) {
            Fail("cannot open lock " + lockPath + ": " + strerror(errno));
        }
        if (flock(Fd_, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK) {
                Fail("another pch_update run is active in " + directory);
            }
            Fail("cannot acquire lock " + lockPath + ": " + strerror(errno));
        }
    }

    std::string StateDir(const std::string& baseline) {
        return JoinPath(RealPath(baseline), ".pch_update");
    }

    std::string PlanDir(const std::string& baseline) {
        return JoinPath(StateDir(baseline), "plan");
    }

    std::string CacheDir(const std::string& path) {
        const std::string real = RealPath(path);
        const std::string alias = ReadPointerFile(JoinPath(real, ".path-alias"));
        if (alias.empty()) {
            return real;
        }
        if (!IsAbs(alias) || !Exists(alias) || RealPath(alias) != real) {
            Fail("invalid migrated-cache alias: " + alias);
        }
        return NormPath(alias);
    }

    bool IsUnderDir(const std::string& path, const std::string& dir) {
        return path == dir || StartsWith(path, dir + "/");
    }

    std::string DigestFile(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            Fail("cannot fingerprint " + path);
        }
        THasher hash;
        char buffer[65536];
        while (in) {
            in.read(buffer, sizeof(buffer));
            hash.Update(std::string_view(buffer, size_t(in.gcount())));
        }
        if (!in.eof()) {
            Fail("cannot read " + path);
        }
        return hash.Hex();
    }

    std::vector<std::string> DigestFiles(const std::vector<std::string>& paths, int workers) {
        std::vector<std::string> digests(paths.size());
        ParallelFor(paths.size(), AnalysisThreads(workers), [&](size_t i) {
            digests[i] = DigestFile(paths[i]);
        });
        return digests;
    }

    std::string TreeIdentity(const std::string& root, bool excludeSelf) {
        TRunOptions run;
        run.Cwd = root;
        std::vector<std::string> diff = {"git", "diff", "--binary", "HEAD", "--"};
        if (excludeSelf) {
            diff.insert(diff.end(), {".", ":(exclude)build/gn/pch_update/**"});
        }
        TStreamHasher hash;
        hash.Feed(RunChecked({"git", "rev-parse", "HEAD"}, run));
        run.OnStdout = [&](std::string_view chunk) { hash.Feed(chunk); };
        RunChecked(diff, run);
        return hash.Hex();
    }

    std::string ResolveProgram(const std::string& name, const std::string& cwd, bool resolveLinks) {
        auto resolve = [&](const std::string& path) {
            const std::string absolute = cwd.empty() ? AbsPath(path) : NormPath(JoinPath(cwd, path));
            return resolveLinks ? RealPath(absolute) : absolute;
        };
        if (name.find('/') != std::string::npos) {
            const std::string candidate = resolve(name);
            if (access(candidate.c_str(), X_OK) != 0) {
                Fail("executable not found: " + name);
            }
            return candidate;
        }
        const char* envPath = std::getenv("PATH");
        for (std::string_view dir : Split(envPath ? envPath : "", ':')) {
            const std::string candidate = resolve(JoinPath(dir.empty() ? "." : dir, name));
            if (access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
        }
        Fail("executable not found: " + name);
    }

    std::string BuildRootFromNinja(const std::string& buildDir) {
        std::ifstream in(JoinPath(buildDir, "build.ninja"));
        std::string line;
        while (std::getline(in, line)) {
            if (line.find("--root=") == std::string::npos) {
                continue;
            }
            try {
                for (const std::string& token : ShellSplit(line)) {
                    if (StartsWith(token, "--root=") && token.size() > 7) {
                        return NormPath(JoinPath(buildDir, token.substr(7)));
                    }
                }
            } catch (const TError&) {
                const size_t pos = line.find("--root=");
                size_t end = pos + 7;
                while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) {
                    ++end;
                }
                if (end > pos + 7) {
                    return NormPath(JoinPath(buildDir, line.substr(pos + 7, end - (pos + 7))));
                }
            }
        }
        return {};
    }

    void RequireBuildIdle(const std::string& buildDir) {
        if (IsDir(buildDir) && BuildRunning(buildDir)) {
            Fail("Ninja is still running in " + buildDir + "; wait before continuing");
        }
    }

    double ModeDefaultQ(const std::string& mode) {
        if (mode == "global") {
            return 1.0;
        }
        if (mode == "incremental") {
            return 0.1;
        }
        Fail("--mode must be global or incremental: " + mode);
    }

    double DefaultAvailableRam() {
        double available = 0.0;
        {
            std::ifstream meminfo("/proc/meminfo");
            std::string line;
            while (std::getline(meminfo, line)) {
                std::istringstream fields(line);
                std::string key;
                double kb = 0.0;
                if (fields >> key >> kb && key == "MemAvailable:") {
                    available = kb * 1024.0;
                    break;
                }
            }
        }
        if (available <= 0.0) {
            const long pages = sysconf(_SC_AVPHYS_PAGES);
            const long pageSize = sysconf(_SC_PAGESIZE);
            if (pages > 0 && pageSize > 0) {
                available = double(pages) * double(pageSize);
            }
        }
        const std::string cgroupDir = ResolveCgroupV2Dir();
        std::string limitText, currentText;
        if (TryReadFile(JoinPath(cgroupDir, "memory.max"), &limitText) &&
            TryReadFile(JoinPath(cgroupDir, "memory.current"), &currentText)) {
            double maximum = 0.0;
            double current = 0.0;
            if ((std::istringstream(limitText) >> maximum) && (std::istringstream(currentText) >> current) && maximum > 0.0) {
                const double reclaimable = ReadReclaimableFileCache(cgroupDir);
                const double used = std::max(0.0, current - reclaimable);
                available = available > 0.0 ? std::min(available, std::max(0.0, maximum - used))
                                            : std::max(0.0, maximum - used);
            }
        }
        return available > 0.0 ? available * 0.75 : 4e9;
    }

    size_t PchBuildJobs(int workers) {
        return size_t(std::max(1, workers / 4));
    }

    double ParseByteSize(const std::string& key, const std::string& value) {
        const char* text = value.c_str();
        char* end = nullptr;
        const double n = std::strtod(text, &end);
        if (end == text || !std::isfinite(n) || n < 0.0) {
            Fail("non-negative size required for " + key + ": " + value);
        }
        std::string suffix;
        for (const char* p = end; *p != '\0'; ++p) {
            suffix.push_back(char(std::tolower(static_cast<unsigned char>(*p))));
        }
        double scale = 1e9;
        if (suffix.empty() || suffix == "g" || suffix == "gb") {
            scale = 1e9;
        } else if (suffix == "k" || suffix == "kb") {
            scale = 1e3;
        } else if (suffix == "m" || suffix == "mb") {
            scale = 1e6;
        } else if (suffix == "t" || suffix == "tb") {
            scale = 1e12;
        } else if (suffix == "b") {
            scale = 1.0;
        } else {
            Fail("unknown size suffix for " + key + ": " + value + " (use B, K, M, G or T)");
        }
        return n * scale;
    }

    EVerifyBuild ParseVerifyBuild(const std::string& value) {
        if (value == "full") {
            return EVerifyBuild::Full;
        }
        if (value == "pch") {
            return EVerifyBuild::PchOnly;
        }
        if (value == "none") {
            return EVerifyBuild::None;
        }
        Fail("--verify-build must be full, pch or none: " + value);
    }

    std::string VerifyBuildName(EVerifyBuild mode) {
        switch (mode) {
            case EVerifyBuild::Full:
                return "full";
            case EVerifyBuild::PchOnly:
                return "pch";
            case EVerifyBuild::None:
                return "none";
        }
        return "full";
    }

    void SaveWorkflowState(const std::string& path, const TWorkflowConfig& cfg) {
        auto packDouble = [](TBinWriter& w, double v) {
            uint64_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            w.U64(bits);
        };
        TBinWriter body;
        body.Str(cfg.Root);
        body.Str(cfg.Baseline);
        body.Str(cfg.Mode);
        packDouble(body, cfg.Q);
        packDouble(body, cfg.MinRatio);
        packDouble(body, cfg.DiskBudgetBytes);
        body.I64(cfg.Workers);
        body.I64(cfg.AnalysisJobs);
        packDouble(body, cfg.RamBytes);
        packDouble(body, cfg.ActionMemoryBytes);
        body.U64(cfg.WorkersDerivedFromRam ? 1 : 0);
        body.Str(cfg.MainRef);
        body.StrList(cfg.Targets);
        body.Str(cfg.Gn);
        body.Str(cfg.Ninja);
        body.Str(VerifyBuildName(cfg.VerifyBuild));

        TBinWriter env;
        env.Str(WORKFLOW_STATE_TAG);
        env.Str(HashHex(body.Data()));
        env.Str(body.Data());
        WriteFile(path, env.Data());
    }

    TWorkflowConfig LoadWorkflowState(const std::string& path) {
        const std::string raw = ReadFile(path);
        TBinReader env(raw);
        if (env.Str() != WORKFLOW_STATE_TAG) {
            Fail("unsupported workflow state version in " + path);
        }
        const std::string expectedHash(env.Str());
        const std::string_view payload = env.Str();
        if (!env.AtEnd() || HashHex(payload) != expectedHash) {
            Fail("corrupt workflow state checksum in " + path);
        }
        TBinReader in(payload);
        auto unpackDouble = [&](double minVal) {
            const uint64_t bits = in.U64();
            double v = 0.0;
            std::memcpy(&v, &bits, sizeof(v));
            if (!std::isfinite(v) || v < minVal) {
                Fail("invalid numeric value in workflow state");
            }
            return v;
        };
        TWorkflowConfig cfg;
        cfg.Root = std::string(in.Str());
        cfg.Baseline = std::string(in.Str());
        cfg.Mode = std::string(in.Str());
        (void)ModeDefaultQ(cfg.Mode);
        cfg.Q = unpackDouble(0.0);
        if (cfg.Q > 1.0) {
            Fail("invalid q in workflow state");
        }
        cfg.MinRatio = unpackDouble(0.0);
        cfg.DiskBudgetBytes = unpackDouble(0.0);
        cfg.Workers = int(in.I64());
        cfg.AnalysisJobs = int(in.I64());
        if (cfg.Workers < 1 || cfg.Workers > 1000000 || cfg.AnalysisJobs < 1 || cfg.AnalysisJobs > 1000000) {
            Fail("invalid worker count in workflow state");
        }
        cfg.RamBytes = unpackDouble(1.0);
        cfg.ActionMemoryBytes = unpackDouble(1.0);
        cfg.WorkersDerivedFromRam = in.U64() != 0;
        cfg.MainRef = std::string(in.Str());
        cfg.Targets = in.StrList();
        cfg.Gn = std::string(in.Str());
        cfg.Ninja = std::string(in.Str());
        cfg.VerifyBuild = ParseVerifyBuild(std::string(in.Str()));
        if (!in.AtEnd()) {
            Fail("trailing bytes in workflow state");
        }
        return cfg;
    }

    void PrepareBaselineBuild(const TWorkflowConfig& cfg) {
        RequireBuildIdle(cfg.Baseline);
        const std::string existingRoot = BuildRootFromNinja(cfg.Baseline);
        if (!existingRoot.empty() && RealPath(existingRoot) != cfg.Root) {
            Fail("baseline belongs to another worktree: " + existingRoot);
        }
        MakeDirs(cfg.Baseline);
        const std::string argsPath = JoinPath(cfg.Baseline, "args.gn");
        std::string argsText;
        TryReadFile(argsPath, &argsText);
        const auto parsedArgs = ParseGnFile(argsText);
        const TGnValue* pchMode = FindGnValue(parsedArgs, "pch_mode");
        const TGnValue* timeTrace = FindGnValue(parsedArgs, "time_trace");
        const bool traceOk = timeTrace && timeTrace->Kind == TGnValue::String &&
            (timeTrace->Text == "json" || timeTrace->Text == "lz4" || timeTrace->Text == "zstd");
        const bool needArgsUpdate = !pchMode || pchMode->Kind != TGnValue::String || pchMode->Text != "none" || !traceOk;
        if (needArgsUpdate) {
            if (!argsText.empty() && argsText.back() != '\n') {
                argsText += '\n';
            }
            argsText += "\n# pch_update baseline\npch_mode = \"none\"\n";
            if (!traceOk) {
                argsText += "time_trace = \"json\"\n";
            }
            WriteFile(argsPath, argsText);
        }

        const std::string gnBin = ResolveProgram(cfg.Gn);
        const std::string ninjaBin = ResolveProgram(cfg.Ninja);
        if (needArgsUpdate || !Exists(JoinPath(cfg.Baseline, "build.ninja"))) {
            std::vector<std::string> switches;
            if (Exists(JoinPath(cfg.Baseline, "build.ninja"))) {
                TNinjaManifest manifest(cfg.Baseline);
                switches = GnGenerationSwitches(manifest);
            } else {
                switches = {"--script-executable=" + ResolveProgram("python3")};
            }
            std::vector<std::string> genCmd = {gnBin, "gen", cfg.Baseline, "--root=" + cfg.Root, "--export-compile-commands"};
            genCmd.insert(genCmd.end(), switches.begin(), switches.end());
            TRunOptions genRun;
            genRun.Cwd = cfg.Root;
            genRun.CaptureStdout = genRun.CaptureStderr = false;
            genRun.OutputLabel = "gn-gen-baseline";
            RunChecked(genCmd, genRun);
        }

        Report(Format("  Baseline build: -j %d", cfg.Workers));
        std::vector<std::string> buildCmd = {ninjaBin, "-C", cfg.Baseline, "-j", std::to_string(cfg.Workers)};
        buildCmd.insert(buildCmd.end(), cfg.Targets.begin(), cfg.Targets.end());
        const std::string toolBefore = DigestFile(SelfExe());
        TRunOptions buildRun;
        buildRun.Cwd = cfg.Root;
        buildRun.CaptureStdout = buildRun.CaptureStderr = false;
        buildRun.OutputLabel = "baseline-build";
        RunChecked(buildCmd, buildRun);
        if (toolBefore != DigestFile(SelfExe())) {
            Fail("baseline rebuilt pch_update; repeat the same command to run the updated binary");
        }
    }

    std::map<std::string, std::string> DirectoryFiles(const std::string& directory) {
        std::map<std::string, std::string> files;
        if (!IsDir(directory)) {
            return files;
        }
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                const std::string path = entry.path().string();
                files[RelPath(path, directory)] = DigestFile(path);
            }
        }
        return files;
    }

    void CopyDirectory(const std::string& from, const std::string& to) {
        MakeDirs(DirName(to));
        RunChecked({"cp", "-a", "--", from, to});
    }

    void FinishInstallation(const std::string& root, const std::string& planDir) {
        const std::string tx = InstallationDir(root, planDir);
        if (!tx.empty()) {
            RemoveTree(tx);
        }
        RemoveFile(JoinPath(planDir, "installing"));
    }

    void RecoverInstallation(const std::string& root, const std::string& planDir) {
        if (!Exists(JoinPath(planDir, "installing"))) {
            return;
        }
        RequireBuildIdle(JoinPath(planDir, "global"));
        const std::string backup = JoinPath(planDir, "original-global");
        const std::string exported = JoinPath(planDir, "export");
        const std::string live = JoinPath(root, GLOBAL_DIR);
        if (!IsDir(backup)) {
            Fail("apply rollback backup is missing: " + backup);
        }
        const auto before = DirectoryFiles(backup);
        const auto after = DirectoryFiles(exported);
        if (IsDir(live)) {
            const auto current = DirectoryFiles(live);
            if (current != before && current != after) {
                Fail("PCH files in " + live + " were modified externally during an interrupted apply");
            }
        }
        std::string tx = InstallationDir(root, planDir);
        if (tx.empty()) {
            tx = MakeTempDir(DirName(live), ".pch-install-");
            WriteFile(JoinPath(planDir, "installing"), tx + "\n");
        }
        const std::string restored = JoinPath(tx, "restored");
        const std::string discarded = JoinPath(tx, "discarded");
        RemoveTree(restored);
        CopyDirectory(backup, restored);
        RemoveTree(discarded);
        if (Exists(live)) {
            std::filesystem::rename(live, discarded);
        }
        std::filesystem::rename(restored, live);
        FinishInstallation(root, planDir);
    }

    void DiscardWorkflowState(const std::string& stateDir) {
        const std::string cleanup = stateDir + ".cleanup";
        if (Exists(stateDir)) {
            std::filesystem::rename(stateDir, cleanup);
        }
        RemoveTree(cleanup);
        Report("PCH workflow complete; temporary scratch state removed.");
        if (!OutputDirectory().empty()) {
            Report("Report and logs: " + OutputDirectory());
        }
    }

    int RunWorkflow(const std::vector<std::string>& args) {
        if (args.empty() || std::find(args.begin(), args.end(), "--help") != args.end() ||
            std::find(args.begin(), args.end(), "-h") != args.end()) {
            Report(HELP);
            return 0;
        }
        if (args.size() == 1 && args[0] == "self-test") {
            return RunSelfTests();
        }

        std::string stage;
        std::string baselineArg;
        bool verbose = false;
        bool minRatioGiven = false, diskGiven = false, modeGiven = false, qGiven = false;
        bool workersGiven = false, analysisJobsGiven = false, ramGiven = false, actionMemGiven = false;
        bool mainRefGiven = false, targetsGiven = false, gnGiven = false, ninjaGiven = false, verifyBuildGiven = false;

        double reqMinRatio = 0.5;
        double reqDiskBytes = 0.0;
        std::string reqMode = "incremental";
        double reqQ = 0.1;
        int reqWorkers = 1;
        int reqAnalysisJobs = 1;
        double reqRamBytes = 0.0;
        double reqActionMemBytes = 1e9;
        std::string reqMainRef;
        std::vector<std::string> reqTargets;
        std::string reqGn;
        std::string reqNinja;
        EVerifyBuild reqVerifyBuild = EVerifyBuild::Full;

        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--verbose") {
                verbose = true;
                continue;
            }
            if (args[i] == "--predict" || args[i] == "--apply") {
                if (!stage.empty()) {
                    Fail("choose at most one of --predict or --apply (or neither for the full workflow)");
                }
                stage = args[i];
                continue;
            }
            const auto [key, value] = ReadCliOption(args, i);
            if (key == "--baseline") {
                baselineArg = AbsPath(value);
            } else if (key == "--min-ratio") {
                reqMinRatio = ParseDoubleOption(key, value, 0.0);
                minRatioGiven = true;
            } else if (key == "--disk") {
                reqDiskBytes = ParseByteSize(key, value);
                diskGiven = true;
            } else if (key == "--mode") {
                (void)ModeDefaultQ(value);
                reqMode = value;
                modeGiven = true;
            } else if (key == "--q") {
                reqQ = ParseDoubleOption(key, value, 0.0);
                if (reqQ > 1.0) {
                    Fail("--q must be between 0 and 1");
                }
                qGiven = true;
            } else if (key == "--workers") {
                reqWorkers = ParseIntOption(key, value, 1, 1000000);
                workersGiven = true;
            } else if (key == "--analysis-jobs") {
                reqAnalysisJobs = ParseIntOption(key, value, 1, 1000000);
                analysisJobsGiven = true;
            } else if (key == "--ram-gb") {
                reqRamBytes = ParseDoubleOption(key, value, 1e-6) * 1e9;
                if (reqRamBytes <= 1.0) {
                    Fail("--ram-gb must be positive");
                }
                ramGiven = true;
            } else if (key == "--action-memory-gb") {
                reqActionMemBytes = ParseDoubleOption(key, value, 1e-6) * 1e9;
                if (reqActionMemBytes <= 1.0) {
                    Fail("--action-memory-gb must be positive");
                }
                actionMemGiven = true;
            } else if (key == "--main-ref") {
                reqMainRef = value;
                mainRefGiven = true;
            } else if (key == "--target") {
                reqTargets.push_back(value);
                targetsGiven = true;
            } else if (key == "--gn") {
                reqGn = value;
                gnGiven = true;
            } else if (key == "--ninja") {
                reqNinja = value;
                ninjaGiven = true;
            } else if (key == "--verify-build") {
                reqVerifyBuild = ParseVerifyBuild(value);
                verifyBuildGiven = true;
            } else {
                Fail("unknown option " + key + "\n" + HELP);
            }
        }

        if (baselineArg.empty()) {
            Fail("--baseline DIR is required\n" + std::string(HELP));
        }
        MakeDirs(baselineArg);
        const std::string baseline = RealPath(baselineArg);
        const std::string root = WorktreeRoot();
        const std::string baselineRoot = BuildRootFromNinja(baseline);
        if (!baselineRoot.empty() && RealPath(baselineRoot) != root) {
            Fail("baseline belongs to another worktree: " + baselineRoot);
        }

        const std::string gitDir = TrimText(RunChecked({"git", "-C", root, "rev-parse", "--absolute-git-dir"}));
        TDirLock worktreeLock;
        worktreeLock.Acquire(JoinPath(gitDir, "pch_update"));

        const std::string stateDir = StateDir(baseline);
        if (Exists(stateDir + ".cleanup")) {
            RemoveTree(stateDir + ".cleanup");
            Report("PCH workflow cleanup completed");
            return 0;
        }
        if (!Exists(stateDir) && stage == "--apply") {
            Fail("no unfinished run; run --predict first");
        }

        TDirLock stateLock;
        stateLock.Acquire(stateDir);

        const std::string stateFile = JoinPath(stateDir, "state.bin");
        const std::string planDir = PlanDir(baseline);
        TWorkflowConfig cfg;
        const bool hadSavedState = Exists(stateFile);
        if (hadSavedState) {
            cfg = LoadWorkflowState(stateFile);
            if (!cfg.Root.empty() && cfg.Root != root) {
                Fail("saved workflow belongs to another worktree: " + cfg.Root);
            }
        } else {
            cfg.Root = root;
            cfg.Baseline = baseline;
            cfg.RamBytes = DefaultAvailableRam();
            cfg.ActionMemoryBytes = 1e9;
            cfg.WorkersDerivedFromRam = true;
        }
        cfg.Root = root;
        cfg.Baseline = baseline;

        struct TOutputGuard {
            ~TOutputGuard() { CloseOutput(); }
        } outputGuard;

        auto configureReport = [&](bool fresh) {
            try {
                const std::string reportPtr = JoinPath(stateDir, "report.path");
                std::string dir = ReadPointerFile(reportPtr);
                const std::string reportsRoot = JoinPath(baseline, "pch-report");
                if (fresh || dir.empty() || !IsUnderDir(dir, reportsRoot)) {
                    dir = JoinPath(reportsRoot, "run-" + std::to_string(NowNs()) + "-" + std::to_string(getpid()));
                }
                ConfigureOutput(dir, verbose);
                WriteFile(reportPtr, dir + "\n");
                WriteFile(JoinPath(reportsRoot, "latest.txt"), dir + "\n");
                Detail("command: " + ShellJoin(args));
                Report("PCH update: " + (stage.empty() ? std::string("full workflow") : stage.substr(2)) +
                       "\nReport: " + dir);
            } catch (const std::exception& e) {
                Log("warning: report output setup failed: " + std::string(e.what()));
            }
        };
        configureReport(!hadSavedState);

        if (IsDir(planDir)) {
            RequireBuildIdle(JoinPath(planDir, "global"));
            if (!ReadPointerFile(JoinPath(planDir, "applied.tree")).empty()) {
                FinishInstallation(root, planDir);
                DiscardWorkflowState(stateDir);
                return 0;
            }
            RecoverInstallation(root, planDir);
        }

        const TWorkflowConfig previousCfg = cfg;
        if (minRatioGiven) {
            cfg.MinRatio = reqMinRatio;
        }
        if (diskGiven) {
            cfg.DiskBudgetBytes = reqDiskBytes;
        }
        if (modeGiven) {
            cfg.Mode = reqMode;
            if (!qGiven) {
                cfg.Q = ModeDefaultQ(cfg.Mode);
            }
        }
        if (qGiven) {
            cfg.Q = reqQ;
        }
        if (ramGiven) {
            cfg.RamBytes = reqRamBytes;
        }
        if (actionMemGiven) {
            cfg.ActionMemoryBytes = reqActionMemBytes;
        }
        const int prevWorkers = cfg.Workers;
        if (workersGiven) {
            cfg.Workers = reqWorkers;
            cfg.WorkersDerivedFromRam = false;
        } else if (!hadSavedState || (cfg.WorkersDerivedFromRam && (ramGiven || actionMemGiven))) {
            const double affordable = std::floor(cfg.RamBytes / std::max(cfg.ActionMemoryBytes, 1.0));
            cfg.Workers = std::max(1, std::min(int(CpuCount()), int(std::max(1.0, affordable))));
            cfg.WorkersDerivedFromRam = true;
        }
        if (analysisJobsGiven) {
            cfg.AnalysisJobs = reqAnalysisJobs;
        } else if (!hadSavedState || cfg.AnalysisJobs == prevWorkers) {
            cfg.AnalysisJobs = cfg.Workers;
        }
        if (mainRefGiven) {
            cfg.MainRef = reqMainRef;
        }
        if (targetsGiven) {
            cfg.Targets = reqTargets;
        }
        if (gnGiven) {
            cfg.Gn = reqGn;
        }
        if (ninjaGiven) {
            cfg.Ninja = reqNinja;
        }
        if (verifyBuildGiven) {
            cfg.VerifyBuild = reqVerifyBuild;
        }

        const bool predictionChanged = hadSavedState && !PredictionInputsEqual(previousCfg, cfg);
        if (predictionChanged && stage == "--apply") {
            Fail("prediction parameters changed; run --predict first");
        }
        if (predictionChanged) {
            configureReport(true);
            const bool dropAnalysis = !ExtractionInputsEqual(previousCfg, cfg);
            ClearPlanOutputs(planDir, dropAnalysis);
        }
        MakeDirs(planDir);
        SaveWorkflowState(stateFile, cfg);

        const std::string cacheDir = CacheDir(JoinPath(stateDir, "cache"));
        MakeDirs(cacheDir);

        if (stage != "--apply") {
            bool planReady = false;
            if (Exists(JoinPath(planDir, "plan.bin"))) {
                try {
                    const TPlanSnapshot existing = LoadAndValidatePlan(planDir, cfg);
                    PublishPlanReports(existing, planDir);
                    planReady = true;
                } catch (const TError& err) {
                    Log("cached plan invalidated (" + std::string(err.what()) + "); recomputing");
                    configureReport(true);
                    ClearPlanOutputs(planDir, false);
                }
            }
            if (!planReady) {
                TAnalysisModel model;
                {
                    TPhase phase("[1/4] Prepare and analyze baseline");
                    if (!TryLoadValidAnalysis(planDir, cfg, &model)) {
                        PrepareBaselineBuild(cfg);
                        model = ExtractAnalysisModel(cfg, cacheDir, planDir);
                    }
                }
                TPlanSnapshot plan;
                {
                    TPhase phase("[2/4] Select and build real base/delta PCHs");
                    plan = SelectAndMaterializePchs(cfg, model, cacheDir, planDir);
                }
                {
                    TPhase phase("[3/4] Check target compatibility and finalize plan");
                    CheckTargetsAndFinalizePlan(cfg, model, plan, cacheDir, planDir);
                }
            } else {
                Log("[1–3/4] Reusing validated PCH plan; baseline and selection skipped");
            }
            if (stage == "--predict") {
                Report("Prediction ready. Next: " + ShellJoin({SelfExe(), "--baseline", baseline, "--apply"}));
                return 0;
            }
        }

        if (!Exists(JoinPath(planDir, "plan.bin"))) {
            Fail("no completed prediction; run --predict first");
        }
        const TPlanSnapshot finalPlan = LoadAndValidatePlan(planDir, cfg);
        {
            TPhase phase("[4/4] Export, install and validate GN global PCHs");
            ApplyAndValidatePlan(cfg, finalPlan, planDir);
        }
        DiscardWorkflowState(stateDir);
        return 0;
    }

} // namespace NPchUpdate
