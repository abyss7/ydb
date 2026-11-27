#include "pch_model.h"

#include "common.h"
#include "elf.h"
#include "facts.h"
#include "jobs.h"
#include "ninja.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

extern char** environ;

namespace NPchUpdate {

    namespace {

        constexpr const char* PROBE_CACHE_VERSION = "probes-v1";
        constexpr const char* PROBE_RECORD_TAG = "pch-probe-rec-1";
        constexpr int MAX_SELECT_ROUNDS = 4;

        template <class T>
        void SortUnique(std::vector<T>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        struct TTimedRun {
            int ExitCode = 0;
            double CpuSec = 0.0;
            double WallSec = 0.0;
        };

        // Runs a direct compiler/linker command without shell, recording exact per-PID
        // rusage CPU time via wait4() so concurrent threads never mix child telemetry.
        TTimedRun RunCompilerTimed(const std::vector<std::string>& argv,
                                   const std::string& cwd,
                                   const std::string& logFile) {
            if (argv.empty()) {
                Fail("RunCompilerTimed: empty command");
            }
            MakeDirs(DirName(logFile));

            std::string program = argv[0];
            if (program.find('/') == std::string::npos) {
                program = ResolveProgram(program, cwd, false);
            }

            std::vector<char*> cArgs;
            cArgs.reserve(argv.size() + 1);
            for (const std::string& a : argv) {
                cArgs.push_back(const_cast<char*>(a.c_str()));
            }
            cArgs.push_back(nullptr);

            const std::vector<std::string> extraEnv = {
                "CCACHE_DISABLE=1",
                "SCCACHE_DISABLE=1",
                "DC_RECACHE=1",
            };
            std::vector<std::string> envStore;
            for (char** e = environ; e && *e; ++e) {
                std::string_view entry(*e);
                const size_t eq = entry.find('=');
                const std::string_view key = eq == std::string_view::npos ? entry : entry.substr(0, eq + 1);
                bool overridden = false;
                for (const std::string& ov : extraEnv) {
                    if (StartsWith(ov, key)) {
                        overridden = true;
                        break;
                    }
                }
                if (!overridden) {
                    envStore.emplace_back(entry);
                }
            }
            envStore.insert(envStore.end(), extraEnv.begin(), extraEnv.end());
            std::vector<char*> cEnv;
            cEnv.reserve(envStore.size() + 1);
            for (std::string& s : envStore) {
                cEnv.push_back(s.data());
            }
            cEnv.push_back(nullptr);

            int failPipe[2] = {-1, -1};
            if (pipe2(failPipe, O_CLOEXEC) != 0) {
                Fail(std::string("pipe2: ") + strerror(errno));
            }

            const auto wallStart = std::chrono::steady_clock::now();
            const pid_t pid = fork();
            if (pid < 0) {
                const int saved = errno;
                close(failPipe[0]);
                close(failPipe[1]);
                Fail(std::string("fork: ") + strerror(saved));
            }
            if (pid == 0) {
                const int devNull = open("/dev/null", O_RDONLY);
                if (devNull >= 0) {
                    dup2(devNull, STDIN_FILENO);
                    if (devNull > STDERR_FILENO) { close(devNull); }
                }
                const int outFd = open(logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (outFd < 0) {
                    int err = errno;
                    ssize_t ign = write(failPipe[1], &err, sizeof(err));
                    (void)ign;
                    _exit(127);
                }
                dup2(outFd, STDOUT_FILENO);
                dup2(outFd, STDERR_FILENO);
                if (outFd > STDERR_FILENO) { close(outFd); }

                if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
                    int err = errno;
                    ssize_t ign = write(failPipe[1], &err, sizeof(err));
                    (void)ign;
                    _exit(127);
                }
                execve(program.c_str(), cArgs.data(), cEnv.data());
                int err = errno;
                ssize_t ign = write(failPipe[1], &err, sizeof(err));
                (void)ign;
                _exit(127);
            }

            close(failPipe[1]);
            int childErr = 0;
            ssize_t rd = 0;
            while ((rd = read(failPipe[0], &childErr, sizeof(childErr))) < 0 && errno == EINTR) {}
            close(failPipe[0]);

            int status = 0;
            rusage usage{};
            while (wait4(pid, &status, 0, &usage) < 0) {
                if (errno != EINTR) {
                    Fail(std::string("wait4: ") + strerror(errno));
                }
            }
            const auto wallEnd = std::chrono::steady_clock::now();
            if (rd == sizeof(childErr) && childErr != 0) {
                Fail("cannot execute " + argv[0] + ": " + strerror(childErr));
            }

            TTimedRun out;
            out.ExitCode = WIFEXITED(status) ? WEXITSTATUS(status) : (128 + WTERMSIG(status));
            out.WallSec = std::chrono::duration<double>(wallEnd - wallStart).count();
            out.CpuSec = double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
            if (out.CpuSec <= 0.0) {
                out.CpuSec = out.WallSec;
            }
            return out;
        }

        struct TProbeRecord {
            std::string CommandHash;
            std::vector<std::pair<std::string, std::string>> Inputs;
            int64_t ArtifactSize = -1;
            int64_t ArtifactMtime = -1;
            double CpuSec = 0.0;
        };

        void SaveProbeRecord(const std::string& path, const TProbeRecord& rec) {
            TBinWriter body;
            body.Str(rec.CommandHash);
            body.U64(rec.Inputs.size());
            for (const auto& [p, d] : rec.Inputs) {
                body.Str(p);
                body.Str(d);
            }
            body.I64(rec.ArtifactSize);
            body.I64(rec.ArtifactMtime);
            uint64_t bits = 0;
            std::memcpy(&bits, &rec.CpuSec, sizeof(bits));
            body.U64(bits);

            TBinWriter env;
            env.Str(PROBE_RECORD_TAG);
            env.Str(HashHex(body.Data()));
            env.Str(body.Data());
            WriteFile(path, env.Data());
        }

        bool TryLoadProbeRecord(const std::string& path, TProbeRecord* out) {
            std::string raw;
            if (!TryReadFile(path, &raw)) { return false; }
            try {
                TBinReader env(raw);
                if (env.Str() != PROBE_RECORD_TAG) { return false; }
                const std::string expected(env.Str());
                const std::string_view payload = env.Str();
                if (!env.AtEnd() || HashHex(payload) != expected) { return false; }
                TBinReader in(payload);
                out->CommandHash = std::string(in.Str());
                const uint64_t n = in.U64();
                if (n > 10'000'000) { return false; }
                out->Inputs.resize(size_t(n));
                for (size_t i = 0; i < size_t(n); ++i) {
                    out->Inputs[i].first = std::string(in.Str());
                    out->Inputs[i].second = std::string(in.Str());
                }
                out->ArtifactSize = in.I64();
                out->ArtifactMtime = in.I64();
                const uint64_t bits = in.U64();
                std::memcpy(&out->CpuSec, &bits, sizeof(out->CpuSec));
                return in.AtEnd() && std::isfinite(out->CpuSec) && out->CpuSec >= 0.0;
            } catch (const TError&) {
                return false;
            }
        }

        class TProbeCache {
        public:
            explicit TProbeCache(const std::string& root)
                : Root_(CacheDir((MakeDirs(root), root)))
            {
            }

            std::string EntryDir(const std::string& key) const {
                return JoinPath(Root_, key);
            }

            std::string Digest(const std::string& path) {
                std::lock_guard<std::mutex> guard(Mutex_);
                auto [it, added] = Digests_.try_emplace(path);
                if (added) {
                    it->second = (IsUnderDir(path, Root_) && EndsWith(path, ".pch"))
                        ? (std::to_string(FileSize(path)) + ":" + std::to_string(MtimeNs(path)))
                        : DigestFile(path);
                }
                return it->second;
            }

            bool Probe(const std::string& key,
                       const std::string& cwd,
                       std::vector<std::string> argv,
                       const std::string& headerText,
                       const std::string& explicitOutput,
                       std::vector<std::string> extraInputs,
                       TProbeRecord* outRecord = nullptr) {
                const std::string dir = EntryDir(key);
                MakeDirs(dir);
                int fd = open(JoinPath(dir, "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
                if (fd < 0 || flock(fd, LOCK_EX) != 0) {
                    if (fd >= 0) { close(fd); }
                    Fail("cannot lock probe cache entry: " + dir);
                }
                struct TFdGuard {
                    int Fd;
                    ~TFdGuard() { if (Fd >= 0) { close(Fd); } }
                } fdGuard{fd};

                const std::string artifact = headerText.empty() ? explicitOutput : JoinPath(dir, "output.pch");
                if (!headerText.empty()) {
                    WriteFileIfChanged(JoinPath(dir, "input.h"), headerText);
                }
                const std::string depfile = JoinPath(dir, "deps.d");
                argv.insert(argv.end(), {"-MD", "-MF", depfile, "-ferror-limit=1"});
                const std::string cmdHash = HashHex(cwd + "\n" + ShellJoin(argv));
                const std::string recPath = JoinPath(dir, "record.bin");

                TProbeRecord saved;
                if (TryLoadProbeRecord(recPath, &saved) && saved.CommandHash == cmdHash && !saved.Inputs.empty()) {
                    bool valid = true;
                    if (!artifact.empty()) {
                        valid = saved.ArtifactSize > 0 &&
                                FileSize(artifact) == saved.ArtifactSize &&
                                MtimeNs(artifact) == saved.ArtifactMtime;
                    }
                    for (const auto& [inPath, inDigest] : saved.Inputs) {
                        if (!Exists(inPath) || Digest(inPath) != inDigest) {
                            valid = false;
                            break;
                        }
                    }
                    if (valid) {
                        RemoveFile(depfile);
                        if (outRecord) { *outRecord = std::move(saved); }
                        ++Hits;
                        return true;
                    }
                }

                RemoveFile(recPath);
                RemoveFile(depfile);
                if (!artifact.empty()) {
                    RemoveFile(artifact);
                }

                const std::string logPath = JoinPath(dir, "compiler.log");
                const TTimedRun run = RunCompilerTimed(argv, cwd, logPath);
                ++Runs;

                if (run.ExitCode != 0) {
                    std::string logText;
                    TryReadFile(logPath, &logText);
                    if (run.ExitCode != 1 ||
                        logText.find("error:") == std::string::npos ||
                        logText.find("unable to execute command") != std::string::npos ||
                        logText.find("clang frontend command failed") != std::string::npos ||
                        logText.find("unable to open output file") != std::string::npos ||
                        logText.find("No space left on device") != std::string::npos ||
                        logText.find("Permission denied") != std::string::npos ||
                        logText.find("PLEASE submit a bug report") != std::string::npos) {
                        Fail("compiler probe crashed or failed fatally; see " + logPath);
                    }
                    return false;
                }

                if (!Exists(depfile) || (!artifact.empty() && FileSize(artifact) <= 0)) {
                    Fail("compiler probe produced incomplete output in " + dir);
                }
                std::vector<std::string> deps = ReadDepfile(ReadFile(depfile));
                if (deps.empty()) {
                    Fail("empty compiler depfile: " + depfile);
                }
                deps.push_back(argv.front());
                deps.insert(deps.end(), extraInputs.begin(), extraInputs.end());
                for (std::string& p : deps) {
                    p = NormPath(JoinPath(cwd, p));
                }
                SortUnique(deps);

                TProbeRecord rec;
                rec.CommandHash = cmdHash;
                rec.CpuSec = std::max(0.001, run.CpuSec);
                rec.Inputs.reserve(deps.size());
                for (const std::string& p : deps) {
                    rec.Inputs.emplace_back(p, Digest(p));
                }
                if (!artifact.empty()) {
                    rec.ArtifactSize = FileSize(artifact);
                    rec.ArtifactMtime = MtimeNs(artifact);
                    std::lock_guard<std::mutex> guard(Mutex_);
                    Digests_.erase(artifact);
                }
                SaveProbeRecord(recPath, rec);
                RemoveFile(depfile);
                if (outRecord) { *outRecord = std::move(rec); }
                return true;
            }

            std::atomic<size_t> Hits{0};
            std::atomic<size_t> Runs{0};

        private:
            std::string Root_;
            std::mutex Mutex_;
            std::map<std::string, std::string> Digests_;
        };

        std::string EnvironmentContextHash(const TAnalysisModel& model) {
            TBinWriter w;
            w.Str(PROBE_CACHE_VERSION);
            w.Str(model.Baseline);
            w.Str(model.Root);
            for (const char* name : {"CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "SDKROOT",
                                     "SOURCE_DATE_EPOCH", "LANG", "LC_ALL", "LC_CTYPE", "PATH",
                                     "LIBRARY_PATH", "COMPILER_PATH", "GCC_EXEC_PREFIX"}) {
                const char* val = std::getenv(name);
                w.Str(name);
                w.Str(val ? val : "");
            }
            return HashHex(w.Data());
        }

        std::string RetainProbeLog(const std::string& baseline, const std::string& logPath) {
            std::string retained = logPath;
            try {
                if (!Exists(logPath)) { return retained; }
                const std::string reportRoot = OutputDirectory().empty()
                    ? JoinPath(baseline, "pch-report")
                    : OutputDirectory();
                const std::string dst = JoinPath(reportRoot, "logs/compatibility/" + HashHex(logPath) + ".log");
                WriteFile(dst, ReadFile(logPath));
                retained = dst;
            } catch (const std::exception&) {
            }
            return retained;
        }

        // Executes `fn(g)` across PCH groups ensuring every parent completes before its children.
        void ParallelPchDag(const std::vector<TPchGroup>& groups, size_t maxJobs, const std::function<void(size_t)>& fn) {
            if (groups.empty()) { return; }
            std::vector<std::vector<size_t>> children(groups.size());
            std::deque<size_t> ready;
            for (size_t g = 0; g < groups.size(); ++g) {
                const int p = groups[g].Parent;
                if (p < -1 || p >= int(g)) {
                    Fail("ParallelPchDag: invalid parent ordering");
                }
                if (p < 0) {
                    ready.push_back(g);
                } else {
                    children[size_t(p)].push_back(g);
                }
            }
            std::mutex mutex;
            std::condition_variable cv;
            size_t remaining = groups.size();
            bool failed = false;
            const size_t poolSize = std::min(groups.size(), std::max<size_t>(1, maxJobs));

            ParallelFor(poolSize, poolSize, [&](size_t) {
                for (;;) {
                    size_t g = 0;
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [&] { return failed || remaining == 0 || !ready.empty(); });
                        if (failed || remaining == 0) { return; }
                        g = ready.front();
                        ready.pop_front();
                    }
                    try {
                        fn(g);
                    } catch (...) {
                        {
                            std::lock_guard<std::mutex> lock(mutex);
                            failed = true;
                        }
                        cv.notify_all();
                        throw;
                    }
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        --remaining;
                        for (size_t ch : children[g]) {
                            ready.push_back(ch);
                        }
                    }
                    cv.notify_all();
                }
            });
        }

        std::vector<std::vector<std::string>> PrepareRawFileFlags(const TAnalysisModel& model) {
            const auto compileDb = ReadCompileCommands(model.Baseline);
            std::vector<std::vector<std::string>> rawFlags(model.Files.size());
            for (size_t f = 0; f < model.Files.size(); ++f) {
                const TTuFile& tu = model.Files[f];
                const auto it = compileDb.find(tu.Object);
                if (it == compileDb.end()) {
                    Fail("compile_commands.json missing object before preflight: " + tu.Object);
                }
                rawFlags[f] = StripCompileFlags(it->second.Argv, it->second.Source);
                if (CanonicalizeFlags(rawFlags[f], model.Baseline, model.Root) != model.ClassFlags.at(size_t(tu.Class))) {
                    Fail("compiler flags drifted since analysis for " + tu.Object);
                }
                rawFlags[f][0] = ResolveProgram(rawFlags[f][0], model.Baseline, false);
            }
            return rawFlags;
        }

        std::string MakePchCacheKey(TProbeCache& cache,
                                    const std::string& envId,
                                    const std::vector<std::string>& repFlags,
                                    const std::vector<std::string>& pchOnlyFlags,
                                    const std::string& headerText,
                                    const std::string& parentKey) {
            TBinWriter k;
            k.Str(envId);
            k.Str(cache.Digest(repFlags.front()));
            k.Str(ShellJoin(repFlags));
            k.Str(ShellJoin(pchOnlyFlags));
            k.Str(headerText);
            k.Str(parentKey);
            return "pch-" + HashHex(k.Data());
        }

    } // namespace

    TPlanSnapshot SelectAndMaterializePchs(const TWorkflowConfig& cfg,
                                           const TAnalysisModel& model,
                                           const std::string& cacheDir,
                                           const std::string& planDir) {
        ValidateAnalysisInputs(model, cfg.Workers);
        const std::vector<std::vector<std::string>> rawFlags = PrepareRawFileFlags(model);
        const TNinjaManifest baselineManifest(model.Baseline);
        const std::string stateRoot = StateDir(model.Baseline);
        const std::string envId = EnvironmentContextHash(model);

        TDirLock probeLock;
        probeLock.Acquire(JoinPath(cacheDir, ".probes-lock"));
        TProbeCache cache(JoinPath(cacheDir, PROBE_CACHE_VERSION));

        const TModelParams params = MakeModelParams(cfg);
        const size_t nClasses = model.ClassFlags.size();
        const size_t nUnits = model.Units.size();

        TSelectorFeedback feedback;
        feedback.BannedUnits.assign(nClasses, std::vector<char>(nUnits, 0));
        feedback.UnitBytesScale.assign(nClasses, std::vector<double>(nUnits, 0.0));
        feedback.UnitBuildScale.assign(nClasses, std::vector<double>(nUnits, 0.0));

        TPchLayout layout;
        TSelectionDiagnostics selDiag;
        std::string paybackReport;
        std::vector<std::string> pchReportRows;
        const size_t pchBuildPool = PchBuildJobs(cfg.Workers);

        for (int round = 0; round < MAX_SELECT_ROUNDS; ++round) {
            selDiag = {};
            layout = SelectGreedyLayout(model, params, feedback, &selDiag);
            if (layout.Groups.empty()) {
                paybackReport.clear();
                pchReportRows.clear();
                break;
            }

            const size_t nGroups = layout.Groups.size();
            std::vector<char> built(nGroups, 0);
            std::vector<std::string> keys(nGroups);
            std::vector<std::string> artifacts(nGroups);
            std::vector<std::vector<int>> droppedByBisect(nGroups);
            pchReportRows.assign(nGroups, {});

            Log(Format("preflight round %d: building/checking %zu PCH groups (%zu concurrent PCH builds)...",
                       round + 1, nGroups, pchBuildPool));

            ParallelPchDag(layout.Groups, pchBuildPool, [&](size_t g) {
                TPchGroup& grp = layout.Groups[g];
                if (grp.Parent >= 0 && !built[size_t(grp.Parent)]) {
                    pchReportRows[g] = Format("pch\t%zu\t%zu\t-1\tparent group %d failed; skipped\n", g, g, grp.Parent);
                    return;
                }

                if (grp.Parent >= 0 && !droppedByBisect[size_t(grp.Parent)].empty()) {
                    const auto& parentDropped = droppedByBisect[size_t(grp.Parent)];
                    grp.Roots.erase(std::remove_if(grp.Roots.begin(), grp.Roots.end(), [&](int r) {
                        return std::find(parentDropped.begin(), parentDropped.end(), r) != parentDropped.end();
                    }), grp.Roots.end());
                    if (grp.Roots.empty()) {
                        pchReportRows[g] = Format("pch\t%zu\t%zu\t-1\tall roots dropped with parent\n", g, g);
                        return;
                    }
                }

                const auto& repFlags = rawFlags.at(size_t(grp.RepresentativeFile));
                const std::string parentKey = grp.Parent >= 0 ? keys[size_t(grp.Parent)] : std::string();
                const std::string parentArt = grp.Parent >= 0 ? artifacts[size_t(grp.Parent)] : std::string();

                std::string lastFailedKey;
                TProbeRecord rec;

                auto tryBuildRoots = [&](const std::vector<int>& candidateRoots) -> bool {
                    const std::string hdr = RenderPchHeader(model, candidateRoots);
                    const std::string key = MakePchCacheKey(cache, envId, repFlags, model.PchOnlyFlags, hdr, parentKey);
                    const std::string dir = cache.EntryDir(key);
                    const std::string outPch = JoinPath(dir, "output.pch");

                    std::vector<std::string> cmd = repFlags;
                    cmd.insert(cmd.end(), model.PchOnlyFlags.begin(), model.PchOnlyFlags.end());
                    if (!parentArt.empty()) {
                        cmd.insert(cmd.end(), {"-include-pch", parentArt});
                    }
                    cmd.insert(cmd.end(), {"-x", "c++-header", JoinPath(dir, "input.h"), "-o", outPch});
                    std::vector<std::string> extra;
                    if (!parentArt.empty()) { extra.push_back(parentArt); }

                    TProbeRecord localRec;
                    if (cache.Probe(key, model.Baseline, std::move(cmd), hdr, {}, std::move(extra), &localRec)) {
                        keys[g] = key;
                        artifacts[g] = outPch;
                        rec = std::move(localRec);
                        return true;
                    }
                    lastFailedKey = key;
                    return false;
                };

                std::vector<int> ordered = LayeredRootOrder(model, grp.Roots);
                bool ok = tryBuildRoots(ordered);
                if (!ok) {
                    if (ordered.size() == 1) {
                        droppedByBisect[g].push_back(ordered[0]);
                    } else {
                        std::vector<int> kept = ordered;
                        size_t lo = 1;
                        int budget = int(4 + 4 * std::log2(double(ordered.size())));
                        while (!ok && kept.size() > 1 && budget > 0) {
                            size_t hi = kept.size();
                            while (lo < hi && budget > 0) {
                                const size_t mid = lo + (hi - lo) / 2;
                                --budget;
                                if (tryBuildRoots(std::vector<int>(kept.begin(), kept.begin() + ptrdiff_t(mid)))) {
                                    lo = mid + 1;
                                } else {
                                    hi = mid;
                                }
                            }
                            if (lo != hi) { break; }
                            droppedByBisect[g].push_back(kept[lo - 1]);
                            kept.erase(kept.begin() + ptrdiff_t(lo - 1));
                            --budget;
                            ok = tryBuildRoots(kept);
                        }
                        if (!ok && lo > 1 && lo - 1 <= kept.size()) {
                            for (size_t k = lo - 1; k < kept.size(); ++k) {
                                droppedByBisect[g].push_back(kept[k]);
                            }
                            kept.resize(lo - 1);
                            ok = tryBuildRoots(kept);
                        }
                        if (!ok && lo == 1 && !kept.empty()) {
                            droppedByBisect[g].push_back(kept[0]);
                        }
                        if (ok) {
                            ordered = std::move(kept);
                        }
                    }
                }

                if (ok) {
                    built[g] = 1;
                    grp.Roots = ordered;
                    grp.Units = ComputeUnitClosure(model, grp.Roots);
                    if (grp.Parent >= 0) {
                        const auto& pUnits = layout.Groups[size_t(grp.Parent)].Units;
                        grp.Units.insert(grp.Units.end(), pUnits.begin(), pUnits.end());
                        SortUnique(grp.Units);
                    }
                    grp.ActualBytes = uint64_t(std::max<int64_t>(1, rec.ArtifactSize));
                    grp.ActualBuildSec = std::max(0.001, rec.CpuSec);

                    grp.GeneratedDeps.clear();
                    if (grp.Parent >= 0) {
                        const auto& pDeps = layout.Groups[size_t(grp.Parent)].GeneratedDeps;
                        grp.GeneratedDeps.insert(grp.GeneratedDeps.end(), pDeps.begin(), pDeps.end());
                    }
                    for (const auto& [inPath, inDigest] : rec.Inputs) {
                        (void)inDigest;
                        if (IsUnderDir(inPath, model.Baseline) && !IsUnderDir(inPath, stateRoot)) {
                            const std::string rel = RelPath(inPath, model.Baseline);
                            if (baselineManifest.Producer(rel) >= 0) {
                                grp.GeneratedDeps.push_back(rel);
                            }
                        }
                    }
                    SortUnique(grp.GeneratedDeps);
                }

                std::string diag;
                if (!droppedByBisect[g].empty()) {
                    std::vector<std::string> names;
                    for (int r : droppedByBisect[g]) { names.push_back(model.Units[size_t(r)].Include); }
                    diag = Format("bisected: dropped %zu root(s) (%s)", names.size(), Join(names, " ").c_str());
                    if (!lastFailedKey.empty()) {
                        diag += "; log: " + RetainProbeLog(model.Baseline, JoinPath(cache.EntryDir(lastFailedKey), "compiler.log"));
                    }
                } else if (!ok && !lastFailedKey.empty()) {
                    diag = RetainProbeLog(model.Baseline, JoinPath(cache.EntryDir(lastFailedKey), "compiler.log"));
                }
                pchReportRows[g] = Format("pch\t%zu\t%zu\t%d\t%s\n", g, g, ok ? int(g) : -1, diag.c_str());
            });

            // Deterministic serial feedback reduction across all groups in order 0..nGroups-1.
            bool anyBannedThisRound = false;
            std::vector<std::vector<double>> numerBytes(nClasses, std::vector<double>(nUnits, 0.0));
            std::vector<std::vector<double>> denomBytes(nClasses, std::vector<double>(nUnits, 0.0));
            std::vector<std::vector<double>> numerBuild(nClasses, std::vector<double>(nUnits, 0.0));
            std::vector<std::vector<double>> denomBuild(nClasses, std::vector<double>(nUnits, 0.0));

            for (size_t g = 0; g < nGroups; ++g) {
                const TPchGroup& grp = layout.Groups[g];
                for (int badRoot : droppedByBisect[g]) {
                    feedback.BannedUnits[size_t(grp.Class)][size_t(badRoot)] = 1;
                    anyBannedThisRound = true;
                }
                if (!built[g]) { continue; }

                std::vector<int> pRoots = grp.Parent >= 0 ? layout.Groups[size_t(grp.Parent)].Roots : std::vector<int>{};
                std::vector<int> ownRoots = grp.Roots;
                SortUnique(pRoots);
                SortUnique(ownRoots);
                feedback.KnownArtifacts[TArtifactKey{grp.Class, std::move(pRoots), std::move(ownRoots)}] =
                    TArtifactMetrics{grp.ActualBytes, grp.ActualBuildSec};

                const std::vector<int>* pUnits = grp.Parent >= 0 ? &layout.Groups[size_t(grp.Parent)].Units : nullptr;
                double ownSrcBytes = 0.0;
                double ownParseSec = 0.0;
                double parentParseSec = 0.0;
                if (pUnits) {
                    for (int pu : *pUnits) { parentParseSec += model.Units[size_t(pu)].ParseSec; }
                }
                std::vector<int> ownUnits;
                size_t j = 0;
                for (int u : grp.Units) {
                    while (pUnits && j < pUnits->size() && (*pUnits)[j] < u) { ++j; }
                    if (pUnits && j < pUnits->size() && (*pUnits)[j] == u) { continue; }
                    ownUnits.push_back(u);
                    ownSrcBytes += model.Units[size_t(u)].SourceBytes;
                    ownParseSec += model.Units[size_t(u)].ParseSec;
                }
                if (ownSrcBytes > 0.0) {
                    const double scale = std::clamp(double(grp.ActualBytes) / ownSrcBytes, 0.1, 200.0);
                    for (int u : ownUnits) {
                        const double w = std::max(1.0, model.Units[size_t(u)].SourceBytes);
                        numerBytes[size_t(grp.Class)][size_t(u)] += w * scale;
                        denomBytes[size_t(grp.Class)][size_t(u)] += w;
                    }
                }
                if (ownParseSec > 0.0) {
                    const double netBuild = std::max(0.001, grp.ActualBuildSec -
                        (pUnits ? params.LoadShare * parentParseSec : 0.0));
                    const double scale = std::clamp(netBuild / ownParseSec, 0.05, 100.0);
                    for (int u : ownUnits) {
                        const double w = std::max(1e-4, model.Units[size_t(u)].ParseSec);
                        numerBuild[size_t(grp.Class)][size_t(u)] += w * scale;
                        denomBuild[size_t(grp.Class)][size_t(u)] += w;
                    }
                }
            }
            for (size_t cls = 0; cls < nClasses; ++cls) {
                for (size_t u = 0; u < nUnits; ++u) {
                    if (denomBytes[cls][u] > 0.0) {
                        feedback.UnitBytesScale[cls][u] = numerBytes[cls][u] / denomBytes[cls][u];
                    }
                    if (denomBuild[cls][u] > 0.0) {
                        feedback.UnitBuildScale[cls][u] = numerBuild[cls][u] / denomBuild[cls][u];
                    }
                }
            }

            bool allBuilt = true;
            for (size_t g = 0; g < nGroups; ++g) {
                if (!built[g]) { allBuilt = false; }
            }
            if (!allBuilt) {
                for (size_t t = 0; t < layout.Assignment.size(); ++t) {
                    int g = layout.Assignment[t];
                    while (g >= 0 && !built[size_t(g)]) {
                        g = layout.Groups[size_t(g)].Parent;
                    }
                    if (g != layout.Assignment[t]) {
                        layout.Assignment[t] = g;
                        layout.Reasons[t] = g >= 0 ? "pch-build-fallback" : "pch-build-rejected";
                    }
                }
            }

            paybackReport.clear();
            const bool allSurvived = EnforcePaybackAndBudget(model, params, layout, /*allowSiblings=*/true, &paybackReport);
            CompactPchLayout(layout);

            if (allBuilt && !anyBannedThisRound && allSurvived) {
                break;
            }
            if (round + 1 == MAX_SELECT_ROUNDS) {
                Log("preflight reached max refinement rounds; retaining measured-fit layout");
                break;
            }
            Log("preflight measured sizes/bans updated; re-running fast greedy selection...");
        }

        MakeDirs(planDir);
        WriteFile(JoinPath(planDir, "selection-targets.tsv"), selDiag.TargetsTsv);
        WriteFile(JoinPath(planDir, "selection-candidates.tsv"), selDiag.GroupsTsv);
        WriteFile(JoinPath(planDir, "selection-payback.tsv"), paybackReport.empty() ? selDiag.PaybackTsv : paybackReport);

        std::string preCompatReport = "kind\ttarget_or_group\told_leaf\tnew_leaf_before_compaction\tdiagnostics\n";
        for (const std::string& row : pchReportRows) {
            preCompatReport += row;
        }
        WriteFile(JoinPath(planDir, "compatibility-pch.tsv"), preCompatReport);

        const TLayoutAccounting acc = EvaluateLayoutAccounting(model, params, layout);
        Log(Format("materialized %zu real PCH groups (%.2f MB total; %zu cache hits, %zu compiler runs)",
                   layout.Groups.size(), double(acc.TotalPchBytes) / 1e6, cache.Hits.load(), cache.Runs.load()));

        TPlanSnapshot snap;
        snap.ToolDigest = DigestFile(SelfExe());
        snap.TreeHash = TreeIdentity(cfg.Root, false);
        snap.Config = cfg;
        snap.Model = model;
        snap.Layout = std::move(layout);
        snap.TotalCompressibleCpuSec = acc.TotalCompressibleCpuSec;
        snap.CoveredCompressibleCpuSec = acc.CoveredCompressibleCpuSec;
        snap.NetSavedCpuSec = acc.NetSavedCpuSec;
        snap.TotalPchBytes = acc.TotalPchBytes;
        return snap;
    }

    void CheckTargetsAndFinalizePlan(const TWorkflowConfig& cfg,
                                     const TAnalysisModel& model,
                                     TPlanSnapshot& plan,
                                     const std::string& cacheDir,
                                     const std::string& planDir) {
        ValidateAnalysisInputs(model, cfg.Workers);
        TPchLayout& layout = plan.Layout;
        const size_t nGroups = layout.Groups.size();
        const std::vector<int> selectedBeforeCompat = layout.Assignment;

        std::string pchRows;
        TryReadFile(JoinPath(planDir, "compatibility-pch.tsv"), &pchRows);
        if (pchRows.empty()) {
            pchRows = "kind\ttarget_or_group\told_leaf\tnew_leaf_before_compaction\tdiagnostics\n";
        }
        std::ostringstream compatReport;
        compatReport << pchRows;

        std::vector<std::string> fileDiagnostics(model.Files.size());

        if (nGroups > 0) {
            const std::vector<std::vector<std::string>> rawFlags = PrepareRawFileFlags(model);
            const std::string envId = EnvironmentContextHash(model);

            TDirLock probeLock;
            probeLock.Acquire(JoinPath(cacheDir, ".probes-lock"));
            TProbeCache cache(JoinPath(cacheDir, PROBE_CACHE_VERSION));

            // 1. Resolve cached PCH artifact paths (built in Phase 2).
            std::vector<std::string> keys(nGroups);
            std::vector<std::string> artifacts(nGroups);
            ParallelPchDag(layout.Groups, PchBuildJobs(cfg.Workers), [&](size_t g) {
                const TPchGroup& grp = layout.Groups[g];
                const auto& repFlags = rawFlags.at(size_t(grp.RepresentativeFile));
                const std::string parentKey = grp.Parent >= 0 ? keys[size_t(grp.Parent)] : std::string();
                const std::string parentArt = grp.Parent >= 0 ? artifacts[size_t(grp.Parent)] : std::string();
                const std::string hdr = RenderPchHeader(model, grp.Roots);
                keys[g] = MakePchCacheKey(cache, envId, repFlags, model.PchOnlyFlags, hdr, parentKey);
                const std::string dir = cache.EntryDir(keys[g]);
                artifacts[g] = JoinPath(dir, "output.pch");

                std::vector<std::string> cmd = repFlags;
                cmd.insert(cmd.end(), model.PchOnlyFlags.begin(), model.PchOnlyFlags.end());
                if (!parentArt.empty()) {
                    cmd.insert(cmd.end(), {"-include-pch", parentArt});
                }
                cmd.insert(cmd.end(), {"-x", "c++-header", JoinPath(dir, "input.h"), "-o", artifacts[g]});
                std::vector<std::string> extra;
                if (!parentArt.empty()) { extra.push_back(parentArt); }
                if (!cache.Probe(keys[g], model.Baseline, std::move(cmd), hdr, {}, std::move(extra))) {
                    Fail("materialized PCH failed validation in Phase 3: group " + std::to_string(g));
                }
            });

            // 2. Link probe for EVERY group (bases and deltas alike) to catch non-runtime symbols.
            struct TLinkProbe {
                bool Ok = false;
                std::string LogPath;
                std::vector<std::string> RequiredSymbols;
            };
            std::vector<TLinkProbe> linkProbes(nGroups);
            ParallelFor(nGroups, PchBuildJobs(cfg.Workers), [&](size_t g) {
                const TPchGroup& grp = layout.Groups[g];
                const std::string linkKey = "link-" + keys[g];
                const std::string dir = cache.EntryDir(linkKey);
                MakeDirs(dir);
                const std::string emptySrc = JoinPath(dir, "empty.cpp");
                const std::string outSo = JoinPath(dir, "output.so");
                WriteFileIfChanged(emptySrc, "// PCH link symbol probe\n");

                std::vector<std::string> cmd = rawFlags.at(size_t(grp.RepresentativeFile));
                cmd.insert(cmd.end(), {
                    "-Wno-unused-command-line-argument",
                    "-include-pch", artifacts[g],
                    "-x", "c++", emptySrc,
                    "-shared", "-nodefaultlibs", "-fuse-ld=lld", "-Wl,--gc-sections",
                    "-o", outSo,
                    "-lc", "-lm", "-ldl", "-lpthread", "-lrt",
                });
                linkProbes[g].Ok = cache.Probe(linkKey, model.Baseline, std::move(cmd), {}, outSo, {artifacts[g]});
                if (linkProbes[g].Ok) {
                    for (const std::string& sym : ReadSharedObjectSymbols(outSo).Unresolved) {
                        if (!IsStrictRuntimeSymbol(sym)) {
                            linkProbes[g].RequiredSymbols.push_back(sym);
                        }
                    }
                } else {
                    linkProbes[g].LogPath = RetainProbeLog(model.Baseline, JoinPath(dir, "compiler.log"));
                }
            });

            std::set<std::string> allNeededSymbols;
            for (size_t g = 0; g < nGroups; ++g) {
                compatReport << "pch-link\t" << g << '\t' << g << '\t' << (linkProbes[g].Ok ? int(g) : -1)
                             << '\t' << linkProbes[g].LogPath << '\n';
                if (linkProbes[g].Ok) {
                    allNeededSymbols.insert(linkProbes[g].RequiredSymbols.begin(),
                                            linkProbes[g].RequiredSymbols.end());
                }
            }

            TSymbolProviders providers;
            if (!allNeededSymbols.empty()) {
                std::vector<std::string> builtLibs;
                for (const std::string& lib : model.LinkInputs.Libraries) {
                    if (Exists(JoinPath(model.Baseline, lib))) {
                        builtLibs.push_back(lib);
                    }
                }
                TWorkDir symWork(JoinPath(cacheDir, "link-symbols"), model.NinjaPath, std::max(1, cfg.Workers));
                LoadLinkProviders(symWork, model.Baseline, builtLibs, allNeededSymbols, providers);
            }

            auto checkTargetLink = [&](int targetId, int groupIdx) -> std::string {
                if (groupIdx < 0) { return {}; }
                const TLinkProbe& lp = linkProbes[size_t(groupIdx)];
                if (!lp.Ok) {
                    return "PCH link probe failed: " + lp.LogPath;
                }
                if (lp.RequiredSymbols.empty()) {
                    return {};
                }
                std::vector<int> linkIndices;
                bool complete = true;
                for (int fIdx : model.Targets[size_t(targetId)].Files) {
                    const std::string& obj = model.Files[size_t(fIdx)].Object;
                    const auto it = model.LinkInputs.Targets.find(obj);
                    if (it == model.LinkInputs.Targets.end() || it->second.empty()) {
                        complete = false;
                    } else {
                        linkIndices.insert(linkIndices.end(), it->second.begin(), it->second.end());
                    }
                }
                SortUnique(linkIndices);
                TLinkAvailability avail = CheckLinkRequirements(
                    lp.RequiredSymbols, providers, model.LinkInputs.Lines, linkIndices);
                avail.UnknownLinks |= !complete;
                return avail.Available() ? std::string() : FormatLinkDiagnostic(avail);
            };

            auto checkTuSyntax = [&](int fIdx, int groupIdx) -> bool {
                const TTuFile& tu = model.Files[size_t(fIdx)];
                std::vector<std::string> cmd = rawFlags[size_t(fIdx)];
                if (groupIdx >= 0) {
                    cmd.insert(cmd.end(), {"-include-pch", artifacts[size_t(groupIdx)]});
                }
                cmd.insert(cmd.end(), {"-fsyntax-only", "-Wno-unused-command-line-argument", tu.RawSource});
                const std::string key = "tu-" + HashHex(envId + cache.Digest(cmd.front()) + ShellJoin(cmd));
                std::vector<std::string> extra = {tu.RawSource};
                if (groupIdx >= 0) {
                    extra.push_back(artifacts[size_t(groupIdx)]);
                }
                if (cache.Probe(key, model.Baseline, std::move(cmd), {}, {}, std::move(extra))) {
                    return true;
                }
                const std::string logPath = RetainProbeLog(model.Baseline, JoinPath(cache.EntryDir(key), "compiler.log"));
                if (!fileDiagnostics[size_t(fIdx)].empty()) {
                    fileDiagnostics[size_t(fIdx)] += ';';
                }
                fileDiagnostics[size_t(fIdx)] += logPath;
                if (groupIdx < 0) {
                    Fail("translation unit failed -fsyntax-only even without PCH: " + tu.Source + "; see " + logPath);
                }
                return false;
            };

            // 3. Wave check (at most 3 waves: delta -> base -> NONE) with per-target short-circuit.
            auto verifyPendingTargets = [&](std::vector<int> pending) {
                int wave = 0;
                while (!pending.empty()) {
                    ++wave;
                    std::vector<std::atomic<uint8_t>> targetFailed(model.Targets.size());
                    for (auto& flag : targetFailed) { flag.store(0, std::memory_order_relaxed); }

                    std::vector<int> filesToCheck;
                    for (int t : pending) {
                        const int g = layout.Assignment[size_t(t)];
                        if (g >= 0) {
                            const std::string linkErr = checkTargetLink(t, g);
                            if (!linkErr.empty()) {
                                targetFailed[size_t(t)].store(1, std::memory_order_relaxed);
                                const int firstFile = model.Targets[size_t(t)].Files.front();
                                if (!fileDiagnostics[size_t(firstFile)].empty()) {
                                    fileDiagnostics[size_t(firstFile)] += ';';
                                }
                                fileDiagnostics[size_t(firstFile)] += linkErr;
                                continue;
                            }
                        }
                        const auto& tFiles = model.Targets[size_t(t)].Files;
                        filesToCheck.insert(filesToCheck.end(), tFiles.begin(), tFiles.end());
                    }

                    Log(Format("compatibility wave %d: checking %zu files across %zu targets (%d workers)...",
                               wave, filesToCheck.size(), pending.size(), cfg.Workers));

                    ParallelFor(filesToCheck.size(), size_t(std::max(1, cfg.Workers)), [&](size_t i) {
                        const int fIdx = filesToCheck[i];
                        const int t = model.Files[size_t(fIdx)].Target;
                        const int g = layout.Assignment[size_t(t)];
                        if (g >= 0 && targetFailed[size_t(t)].load(std::memory_order_relaxed) != 0) {
                            return;
                        }
                        if (!checkTuSyntax(fIdx, g)) {
                            targetFailed[size_t(t)].store(1, std::memory_order_relaxed);
                        }
                    });

                    std::vector<int> retry;
                    for (int t : pending) {
                        if (targetFailed[size_t(t)].load(std::memory_order_relaxed) == 0) {
                            continue;
                        }
                        const int cur = layout.Assignment[size_t(t)];
                        if (cur < 0) {
                            Fail("target failed compatibility without PCH: " + model.Targets[size_t(t)].Label);
                        }
                        const int next = layout.Groups[size_t(cur)].Parent;
                        layout.Assignment[size_t(t)] = next;
                        layout.Reasons[size_t(t)] = next >= 0 ? "compatibility-fallback" : "compatibility-rejected";
                        retry.push_back(t);
                    }
                    pending = std::move(retry);
                }
            };

            std::vector<int> initialPending;
            for (size_t t = 0; t < model.Targets.size(); ++t) {
                if (layout.Assignment[t] >= 0) {
                    initialPending.push_back(int(t));
                }
            }
            verifyPendingTargets(std::move(initialPending));

            // 4. Final step-down-only payback after compatibility fallbacks.
            const std::vector<int> afterCompat = layout.Assignment;
            const TModelParams params = MakeModelParams(cfg);
            std::string finalPayback;
            EnforcePaybackAndBudget(model, params, layout, /*allowSiblings=*/false, &finalPayback);

            std::vector<int> demotedToBase;
            for (size_t t = 0; t < model.Targets.size(); ++t) {
                if (layout.Assignment[t] != afterCompat[t] && layout.Assignment[t] >= 0) {
                    demotedToBase.push_back(int(t));
                }
            }
            if (!demotedToBase.empty()) {
                verifyPendingTargets(std::move(demotedToBase));
                EnforcePaybackAndBudget(model, params, layout, /*allowSiblings=*/false, &finalPayback);
            }
            if (!finalPayback.empty()) {
                WriteFile(JoinPath(planDir, "final-payback.tsv"), finalPayback);
            }
        }

        const std::vector<int> beforeCompact = layout.Assignment;
        for (size_t t = 0; t < model.Targets.size(); ++t) {
            if (selectedBeforeCompat[t] < 0) { continue; }
            compatReport << "target\t" << model.Targets[t].Label << '\t' << selectedBeforeCompat[t] << '\t'
                         << beforeCompact[t] << '\t';
            for (int fIdx : model.Targets[t].Files) {
                if (!fileDiagnostics[size_t(fIdx)].empty()) {
                    compatReport << model.Files[size_t(fIdx)].Source << ':' << fileDiagnostics[size_t(fIdx)] << ';';
                }
            }
            compatReport << '\n';
        }
        WriteFile(JoinPath(planDir, "compatibility.tsv"), compatReport.str());

        CompactPchLayout(layout);

        std::ostringstream compatTargets;
        compatTargets << "target\treason\tselected_leaf\tcompatible_leaf_before_compaction\tfinal_leaf\tdiagnostics\n";
        for (size_t t = 0; t < model.Targets.size(); ++t) {
            compatTargets << model.Targets[t].Label << '\t' << layout.Reasons[t] << '\t'
                          << selectedBeforeCompat[t] << '\t' << beforeCompact[t] << '\t'
                          << layout.Assignment[t] << '\t';
            for (int fIdx : model.Targets[t].Files) {
                if (!fileDiagnostics[size_t(fIdx)].empty()) {
                    compatTargets << model.Files[size_t(fIdx)].Source << ':' << fileDiagnostics[size_t(fIdx)] << ';';
                }
            }
            compatTargets << '\n';
        }
        WriteFile(JoinPath(planDir, "compatibility-targets.tsv"), compatTargets.str());

        const TModelParams params = MakeModelParams(cfg);
        const TLayoutAccounting acc = EvaluateLayoutAccounting(model, params, layout);
        plan.ToolDigest = DigestFile(SelfExe());
        plan.TreeHash = TreeIdentity(cfg.Root, false);
        plan.TotalCompressibleCpuSec = acc.TotalCompressibleCpuSec;
        plan.CoveredCompressibleCpuSec = acc.CoveredCompressibleCpuSec;
        plan.NetSavedCpuSec = acc.NetSavedCpuSec;
        plan.TotalPchBytes = acc.TotalPchBytes;

        std::ostringstream diskBudget;
        diskBudget << "# retained_groups=" << layout.Groups.size()
                   << " retained_bytes=" << plan.TotalPchBytes
                   << " min_ratio=" << cfg.MinRatio
                   << " disk_cap_bytes=" << uint64_t(cfg.DiskBudgetBytes) << "\n"
                   << "group\tparent\tclass\tbytes\tbuild_cpu_sec\n";
        for (size_t g = 0; g < layout.Groups.size(); ++g) {
            diskBudget << g << '\t' << layout.Groups[g].Parent << '\t' << layout.Groups[g].Class << '\t'
                       << layout.Groups[g].ActualBytes << '\t' << layout.Groups[g].ActualBuildSec << '\n';
        }
        WriteFile(JoinPath(planDir, "disk-budget.tsv"), diskBudget.str());
        WriteFile(JoinPath(model.Baseline, "pch-disk-budget.tsv"), diskBudget.str());

        SavePlanSnapshot(JoinPath(planDir, "plan.bin"), plan);
        PublishPlanReports(plan, planDir);
    }

} // namespace NPchUpdate
