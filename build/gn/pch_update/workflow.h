#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace NPchUpdate {

    enum class EVerifyBuild {
        Full,
        PchOnly,
        None,
    };

    // Persisted configuration of an active run in BASELINE/.pch_update/state.bin.
    struct TWorkflowConfig {
        std::string Root;
        std::string Baseline;
        std::string Mode = "incremental";
        double Q = 0.1;
        // Minimum net saved CPU-seconds required per 1 MB (1e6 bytes) of PCH.
        double MinRatio = 0.5;
        // Optional resident PCH cap in bytes; 0 means unbounded (governed by MinRatio).
        double DiskBudgetBytes = 0.0;
        int Workers = 1;
        int AnalysisJobs = 1;
        double RamBytes = 0.0;
        double ActionMemoryBytes = 1e9;
        bool WorkersDerivedFromRam = false;
        std::string MainRef = "origin/main";
        std::vector<std::string> Targets = {"all", "tests"};
        std::string Gn = "gn";
        std::string Ninja = "ninja";
        EVerifyBuild VerifyBuild = EVerifyBuild::Full;

        bool operator==(const TWorkflowConfig& other) const = default;
    };

    // Non-blocking exclusive flock guard on a lock file inside `directory`.
    class TDirLock {
    public:
        TDirLock() = default;
        ~TDirLock();
        TDirLock(const TDirLock&) = delete;
        TDirLock& operator=(const TDirLock&) = delete;

        void Acquire(const std::string& directory, const std::string& fileName = "cycle.lock");
        int Fd() const { return Fd_; }

    private:
        int Fd_ = -1;
    };

    std::string StateDir(const std::string& baseline);
    std::string PlanDir(const std::string& baseline);
    std::string CacheDir(const std::string& path);
    bool IsUnderDir(const std::string& path, const std::string& dir);

    std::string DigestFile(const std::string& path);
    std::vector<std::string> DigestFiles(const std::vector<std::string>& paths, int workers);
    std::string TreeIdentity(const std::string& root, bool excludeSelf = false);
    std::string ResolveProgram(const std::string& name, const std::string& cwd = {}, bool resolveLinks = true);
    std::string BuildRootFromNinja(const std::string& buildDir);
    void RequireBuildIdle(const std::string& buildDir);

    double ModeDefaultQ(const std::string& mode);
    double DefaultAvailableRam();
    size_t PchBuildJobs(int workers);
    double ParseByteSize(const std::string& key, const std::string& value);
    EVerifyBuild ParseVerifyBuild(const std::string& value);
    std::string VerifyBuildName(EVerifyBuild mode);

    void SaveWorkflowState(const std::string& path, const TWorkflowConfig& cfg);
    TWorkflowConfig LoadWorkflowState(const std::string& path);
    void PrepareBaselineBuild(const TWorkflowConfig& cfg);

    // Public CLI entry point (`pch_update ...`) and self-test suite (`pch_update self-test`).
    int RunWorkflow(const std::vector<std::string>& args);
    int RunSelfTests();

} // namespace NPchUpdate
