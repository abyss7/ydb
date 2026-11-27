#include "pch_model.h"

#include "common.h"
#include "facts.h"
#include "gni.h"
#include "jobs.h"
#include "ninja.h"
#include "steps.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace NPchUpdate {

    namespace {

        constexpr const char* EXPORT_READY_TAG = "pch-export-ready-1";

        std::string PchGroupName(size_t id) {
            return "group_" + std::to_string(id);
        }

        std::string PchArtifactRelPath(size_t id) {
            return "gen/build/gn/pch/global/pch_" + PchGroupName(id) + ".pch";
        }

        void ReplaceAll(std::string& text, const std::string& from, const std::string& to) {
            if (from.empty()) { return; }
            size_t pos = 0;
            while ((pos = text.find(from, pos)) != std::string::npos) {
                text.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        std::string JsonStringLiteral(const std::string& text) {
            rapidjson::StringBuffer buffer;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            writer.String(text.data(), text.size());
            return buffer.GetString();
        }

        std::string ReportRootDir(const std::string& baseline) {
            return OutputDirectory().empty() ? JoinPath(baseline, "pch-report") : OutputDirectory();
        }

        std::string ExportSingleFlag(const TAnalysisModel& model, std::string flag, bool isPath) {
            if (!model.Baseline.empty()) {
                ReplaceAll(flag, model.Baseline, "<out>");
            }
            if (!model.Root.empty()) {
                ReplaceAll(flag, model.Root, "<root>");
            }
            if (isPath) {
                if (flag == ".") {
                    flag = "<root>";
                } else if (StartsWith(flag, "<out>/")) {
                    flag = flag.substr(6);
                } else if (!IsAbs(flag) && flag != "<out>" && !StartsWith(flag, "<root>")) {
                    flag = "<root>/" + flag;
                }
            }
            std::string quoted = GnQuote(flag);
            ReplaceAll(quoted, "<root>", "${_pch_export_root}");
            ReplaceAll(quoted, "<out>", isPath ? "${_pch_export_out}" : "${_pch_export_abs_out}");
            return quoted;
        }

        std::vector<std::string> ExtractDeployedFlags(const TCompileCommand& compile,
                                                      const std::string& buildDir,
                                                      const std::string& root,
                                                      bool isProducer,
                                                      std::vector<std::string>& outPchs) {
            std::vector<std::string> argv;
            argv.reserve(compile.Argv.size());
            for (size_t i = 0; i < compile.Argv.size(); ++i) {
                const std::string& flag = compile.Argv[i];
                if (flag == "-include-pch" && i + 1 < compile.Argv.size()) {
                    outPchs.push_back(RelPath(NormPath(JoinPath(buildDir, compile.Argv[++i])), buildDir));
                } else if (flag == "-isysroot" && i + 1 < compile.Argv.size() &&
                           NormPath(JoinPath(buildDir, compile.Argv[i + 1])) == root) {
                    ++i;
                } else if (flag == "-Xclang" && i + 1 < compile.Argv.size() &&
                           (compile.Argv[i + 1] == "-module-file-deps" ||
                            (isProducer && (compile.Argv[i + 1] == "-fno-pch-timestamp" ||
                                            compile.Argv[i + 1] == "-fmodule-file-home-is-cwd")))) {
                    ++i;
                } else if (isProducer && (flag == "-fpch-instantiate-templates" || flag == "-relocatable-pch")) {
                    continue;
                } else {
                    argv.push_back(flag);
                }
            }
            return CanonicalizeFlags(StripCompileFlags(argv, compile.Source), buildDir, root);
        }

        void SaveExportManifest(const std::string& path, const std::map<std::string, std::string>& files) {
            TBinWriter body;
            body.U64(files.size());
            for (const auto& [rel, digest] : files) {
                body.Str(rel);
                body.Str(digest);
            }
            TBinWriter env;
            env.Str(EXPORT_READY_TAG);
            env.Str(HashHex(body.Data()));
            env.Str(body.Data());
            WriteFile(path, env.Data());
        }

        bool CheckExportManifest(const std::string& readyPath, const std::string& exportDir) {
            std::string raw;
            if (!TryReadFile(readyPath, &raw)) { return false; }
            try {
                TBinReader env(raw);
                if (env.Str() != EXPORT_READY_TAG) { return false; }
                const std::string expectedHash(env.Str());
                const std::string_view payload = env.Str();
                if (!env.AtEnd() || HashHex(payload) != expectedHash) { return false; }
                TBinReader in(payload);
                const uint64_t n = in.U64();
                if (n > 1'000'000) { return false; }
                std::map<std::string, std::string> expected;
                for (uint64_t i = 0; i < n; ++i) {
                    std::string k(in.Str());
                    std::string v(in.Str());
                    expected.emplace(std::move(k), std::move(v));
                }
                if (!in.AtEnd()) { return false; }
                return expected == DirectoryFiles(exportDir);
            } catch (const TError&) {
                return false;
            }
        }

        void ExportGroupsTree(const TPlanSnapshot& plan, const std::string& planDir) {
            const std::string output = JoinPath(planDir, "export");
            const std::string ready = JoinPath(planDir, "export.ready");
            if (Exists(ready)) {
                if (!CheckExportManifest(ready, output)) {
                    Fail("staged export tree in " + output + " was modified unexpectedly");
                }
                return;
            }

            const TAnalysisModel& m = plan.Model;
            TOptions options;
            options.Baseline = m.Baseline;
            options.Targets = m.WorkloadTargets;
            options.Ninja = m.NinjaPath;
            TTools tools{m.GnPath, m.NinjaPath};

            TWorkDir work(CacheDir(JoinPath(StateDir(m.Baseline), "cache")),
                          m.NinjaPath, std::max(1, plan.Config.Workers));
            TFacts facts;
            facts.Root = m.Root;
            if (!CheckBaseline(options, tools, work, facts)) {
                Fail("baseline changed before GN PCH export");
            }

            std::map<std::string, std::string> publicCodegen;
            for (const auto& [label, deps] : facts.DepsOf) {
                if (!EndsWith(label, "_codegen")) { continue; }
                const std::string_view prefix = std::string_view(label).substr(0, label.find(':'));
                for (const std::string& dep : deps) {
                    if (std::string_view(dep).substr(0, dep.find(':')) == prefix) {
                        auto [it, added] = publicCodegen.emplace(dep, label);
                        if (!added && label < it->second) { it->second = label; }
                    }
                }
            }

            RemoveTree(output);
            MakeDirs(output);
            for (const char* name : {"BUILD.gn", "pch_flags.gni", "enum_serialization.gni"}) {
                WriteFile(JoinPath(output, name), ReadFile(JoinPath(JoinPath(m.Root, GLOBAL_DIR), name)));
            }

            std::string gni = "# Generated by pch_update; exact GN-owner assignments.\n" +
                FormatExportPaths() + "pch_groups = [\n";

            for (size_t g = 0; g < plan.Layout.Groups.size(); ++g) {
                const TPchGroup& grp = plan.Layout.Groups[g];
                std::vector<std::string> targets;
                for (size_t t = 0; t < m.Targets.size(); ++t) {
                    if (plan.Layout.Assignment[t] == int(g)) {
                        if (!m.Targets[t].PchEligible) {
                            Fail("cannot export ineligible GN target: " + m.Targets[t].Label);
                        }
                        targets.push_back(m.Targets[t].Label);
                    }
                }

                std::set<std::string> deps;
                auto addProducer = [&](const std::string& relOutput) {
                    const auto prod = facts.LabelOfOutput.find(relOutput);
                    if (prod == facts.LabelOfOutput.end()) {
                        Fail("no GN producer target owns generated PCH dependency: " + relOutput);
                    }
                    const auto pub = publicCodegen.find(prod->second);
                    deps.insert(pub == publicCodegen.end() ? prod->second : pub->second);
                };

                for (int u : grp.Units) {
                    for (const std::string& hdr : m.Units.at(size_t(u)).Headers) {
                        if (StartsWith(hdr, "<out>/")) {
                            addProducer(hdr.substr(6));
                        }
                    }
                }
                for (const std::string& genDep : grp.GeneratedDeps) {
                    if (facts.Manifest->Producer(genDep) >= 0) {
                        addProducer(genDep);
                    }
                }

                gni += "  {\n    name = " + GnQuote(PchGroupName(g)) + "\n";
                if (grp.Parent >= 0) {
                    gni += "    parent = " + GnQuote(PchGroupName(size_t(grp.Parent))) + "\n";
                }
                gni += GnList("targets", targets, 4);
                gni += GnList("deps", {deps.begin(), deps.end()}, 4);
                gni += "    defines = []\n    include_dirs = []\n    cflags = []\n";
                gni += FormatExportFlags(m, grp.Class) + "  },\n";

                WriteFile(JoinPath(JoinPath(output, PchGroupName(g)), "pch.cc"),
                          RenderPchHeader(m, grp.Roots));
            }

            gni += "]\n";
            gni += "not_needed([ \"_pch_export_root\", \"_pch_export_out\", \"_pch_export_abs_out\" ])\n";
            WriteFile(JoinPath(output, "groups.gni"), gni);
            SaveExportManifest(ready, DirectoryFiles(output));
        }

        std::map<std::string, std::string> ReadTargetReasonsMap(const std::string& path) {
            std::map<std::string, std::string> out;
            std::string text;
            if (!TryReadFile(path, &text)) { return out; }
            for (std::string_view line : Split(text, '\n')) {
                if (line.empty() || line.front() == '#') { continue; }
                const auto cols = Split(line, '\t');
                if (cols.size() >= 4 && cols[0] != "target") {
                    out.emplace(std::string(cols[0]), std::string(cols[3]));
                } else if (cols.size() >= 2 && cols[0] != "target") {
                    out.emplace(std::string(cols[0]), std::string(cols[1]));
                }
            }
            return out;
        }

    } // namespace

    std::string FormatExportPaths() {
        return "_pch_export_root = get_path_info(rebase_path(\"//BUILD.gn\", root_build_dir), \"dir\")\n"
               "_pch_export_out = \".\"\n"
               "_pch_export_abs_out = rebase_path(root_build_dir)\n";
    }

    std::string FormatExportFlags(const TAnalysisModel& model, int compilerClass) {
        const auto& flags = model.ClassFlags.at(size_t(compilerClass));
        std::string result = "    compiler_flags = [\n";
        bool path = false;
        for (size_t i = 1; i < flags.size(); ++i) {
            const std::string& flag = flags[i];
            if (flag == "-Xclang") {
                result += "      \"-Xclang\",\n";
                continue;
            }
            if (i == 1 && StartsWith(flag, "--sysroot=")) {
                continue;
            }
            if (i == 1 && flag == "--sysroot" && i + 1 < flags.size()) {
                ++i;
                continue;
            }
            if (!path && StartsWith(flag, "-I") && flag.size() > 2) {
                result += "      \"-I\" + " + ExportSingleFlag(model, flag.substr(2), true) + ",\n";
                continue;
            }
            result += "      " + ExportSingleFlag(model, flag, path) + ",\n";
            path = !path && (flag == "-I" || flag == "-isystem" || flag == "-iquote" || flag == "-idirafter" ||
                             flag == "-include" || flag == "-isysroot");
        }
        if (path) {
            Fail("incomplete path option at end of compiler class flags");
        }
        return result + "    ]\n";
    }

    void CheckGnDeployment(const TPlanSnapshot& plan, const std::string& buildDir) {
        const TAnalysisModel& m = plan.Model;
        const auto compiles = ReadCompileCommands(buildDir);
        std::set<std::string> checked;

        for (const TTuFile& file : m.Files) {
            const int leaf = plan.Layout.Assignment.at(size_t(file.Target));
            const auto it = compiles.find(file.Object);
            if (it == compiles.end()) {
                if (leaf >= 0) {
                    Fail("assigned C++ compilation missing from GN global build: " + file.Object);
                }
                continue;
            }
            std::vector<std::string> pchs;
            const auto flags = ExtractDeployedFlags(it->second, buildDir, m.Root, /*isProducer=*/false, pchs);
            const std::vector<std::string> expected = leaf < 0
                ? std::vector<std::string>{}
                : std::vector<std::string>{PchArtifactRelPath(size_t(leaf))};
            if (pchs != expected) {
                Fail("GN PCH assignment mismatch for " + file.Object);
            }
            if (leaf >= 0 && flags != m.ClassFlags.at(size_t(file.Class))) {
                Fail("GN consumer compiler flags mismatch for " + file.Object);
            }
            checked.insert(file.Object);
        }

        for (size_t g = 0; g < plan.Layout.Groups.size(); ++g) {
            const TPchGroup& grp = plan.Layout.Groups[g];
            const std::string prodObj = "obj/build/gn/pch/global/" + PchGroupName(g) + "/pch_" +
                                        PchGroupName(g) + "_compile.pch.cc.o";
            const auto it = compiles.find(prodObj);
            if (it == compiles.end()) {
                Fail("GN PCH producer missing from compile_commands.json: " + prodObj);
            }
            std::vector<std::string> parents;
            const auto flags = ExtractDeployedFlags(it->second, buildDir, m.Root, /*isProducer=*/true, parents);
            const std::vector<std::string> expectedParent = grp.Parent < 0
                ? std::vector<std::string>{}
                : std::vector<std::string>{PchArtifactRelPath(size_t(grp.Parent))};
            if (parents != expectedParent || flags != m.ClassFlags.at(size_t(grp.Class))) {
                Fail("GN PCH producer flags or parent mismatch for " + prodObj);
            }
            checked.insert(prodObj);
        }

        for (const auto& [output, compile] : compiles) {
            if (checked.count(output)) { continue; }
            if (std::find(compile.Argv.begin(), compile.Argv.end(), "-include-pch") != compile.Argv.end()) {
                Fail("unexpected GN PCH consumer outside the plan: " + output);
            }
        }
    }

    void PublishPlanReports(const TPlanSnapshot& plan, const std::string& planDir) {
        try {
            const TAnalysisModel& m = plan.Model;
            size_t rootGroups = 0;
            for (const TPchGroup& g : plan.Layout.Groups) {
                rootGroups += (g.Parent < 0) ? 1 : 0;
            }
            const size_t deltaGroups = plan.Layout.Groups.size() - rootGroups;

            size_t assignedTargets = 0;
            size_t assignedFiles = 0;
            std::map<std::string, size_t> reasonCounts;

            const auto selReasons = ReadTargetReasonsMap(JoinPath(planDir, "selection-targets.tsv"));
            std::ostringstream assignments;
            assignments << "target\tclass\tleaf\tcompilations\tbaseline_compile_seconds\tconstraint\tselection_reason\tfinal_reason\n";
            std::ostringstream members;
            members << "target\tsource\toutput\tclass\ttraced\n";

            for (size_t t = 0; t < m.Targets.size(); ++t) {
                const TTargetOwner& owner = m.Targets[t];
                const int leaf = plan.Layout.Assignment[t];
                const bool assigned = leaf >= 0;
                assignedTargets += assigned ? 1 : 0;
                if (assigned) {
                    assignedFiles += owner.Files.size();
                }
                double baselineWork = 0.0;
                for (int fIdx : owner.Files) {
                    const TTuFile& f = m.Files[size_t(fIdx)];
                    baselineWork += f.CompileSec;
                    members << owner.Label << '\t' << f.Source << '\t' << f.Object << '\t'
                            << f.Class << '\t' << int(f.Traced) << '\n';
                }
                const auto sIt = selReasons.find(owner.Label);
                const std::string selReason = sIt == selReasons.end() ? "unavailable" : sIt->second;
                const std::string finalReason = t < plan.Layout.Reasons.size() ? plan.Layout.Reasons[t] : "unknown";
                ++reasonCounts[assigned ? "assigned" : finalReason];

                assignments << owner.Label << '\t' << owner.Class << '\t' << leaf << '\t'
                            << owner.Files.size() << '\t' << baselineWork << '\t'
                            << owner.IneligibleReason << '\t' << selReason << '\t' << finalReason << '\n';
            }
            WriteFile(JoinPath(planDir, "assignments.tsv"), assignments.str());
            WriteFile(JoinPath(planDir, "target-members.tsv"), members.str());

            const double compressiblePct = plan.TotalCompressibleCpuSec > 0.0
                ? (100.0 * plan.CoveredCompressibleCpuSec / plan.TotalCompressibleCpuSec)
                : 0.0;
            const double achievedRatio = plan.TotalPchBytes > 0
                ? (plan.NetSavedCpuSec / (double(plan.TotalPchBytes) / 1e6))
                : 0.0;

            std::ostringstream tsv;
            tsv << std::setprecision(10)
                << "mode\t" << plan.Config.Mode
                << "\nq\t" << plan.Config.Q
                << "\nmin_ratio_s_per_mb\t" << plan.Config.MinRatio
                << "\nachieved_ratio_s_per_mb\t" << achievedRatio
                << "\npchs\t" << plan.Layout.Groups.size()
                << "\nbases\t" << rootGroups
                << "\ndeltas\t" << deltaGroups
                << "\nmeasured_pch_bytes\t" << plan.TotalPchBytes
                << "\npch_disk_bytes_budget\t" << uint64_t(plan.Config.DiskBudgetBytes)
                << "\ntargets\t" << m.Targets.size()
                << "\nassigned_targets\t" << assignedTargets
                << "\ncompilations\t" << m.Files.size()
                << "\nassigned_compilations\t" << assignedFiles
                << "\ntotal_compressible_cpu_seconds\t" << plan.TotalCompressibleCpuSec
                << "\ncovered_compressible_cpu_seconds\t" << plan.CoveredCompressibleCpuSec
                << "\ncovered_compressible_percent\t" << compressiblePct
                << "\nnet_saved_cpu_seconds\t" << plan.NetSavedCpuSec
                << "\nworkers\t" << plan.Config.Workers << '\n';
            WriteFile(JoinPath(planDir, "summary.tsv"), tsv.str());

            std::ostringstream human;
            human << "PCH plan summary (real compiled PCH sizes, pure CPU-time model)\n"
                  << Format("  PCH artifacts: %zu (%zu root/base + %zu deltas); actual size %.2f MB",
                            plan.Layout.Groups.size(), rootGroups, deltaGroups, double(plan.TotalPchBytes) / 1e6);
            if (plan.Config.DiskBudgetBytes > 0.0) {
                human << Format(" / %.2f MB cap", plan.Config.DiskBudgetBytes / 1e6);
            }
            human << '\n'
                  << Format("  Compressible header CPU work absorbed: %.1f%% (%s / %s)\n",
                            compressiblePct, Seconds(plan.CoveredCompressibleCpuSec).c_str(),
                            Seconds(plan.TotalCompressibleCpuSec).c_str())
                  << Format("  Estimated net CPU savings per build: %s (avg %.2f CPU-s / MB; --min-ratio %.2f)\n",
                            Seconds(plan.NetSavedCpuSec).c_str(), achievedRatio, plan.Config.MinRatio)
                  << Format("  Coverage: %zu/%zu GN targets (%.1f%%), %zu/%zu C++ files (%.1f%%)\n",
                            assignedTargets, m.Targets.size(),
                            100.0 * double(assignedTargets) / double(std::max<size_t>(1, m.Targets.size())),
                            assignedFiles, m.Files.size(),
                            100.0 * double(assignedFiles) / double(std::max<size_t>(1, m.Files.size())))
                  << "  Target decisions:";
            for (const auto& [reason, count] : reasonCounts) {
                human << ' ' << reason << '=' << count;
            }
            human << '\n';
            WriteFile(JoinPath(planDir, "summary.txt"), human.str());

            std::ostringstream json;
            json << std::setprecision(15)
                 << "{\"schema\":2,\"mode\":" << JsonStringLiteral(plan.Config.Mode)
                 << ",\"q\":" << plan.Config.Q
                 << ",\"min_ratio\":" << plan.Config.MinRatio
                 << ",\"achieved_ratio\":" << achievedRatio
                 << ",\"pchs\":" << plan.Layout.Groups.size()
                 << ",\"bases\":" << rootGroups
                 << ",\"deltas\":" << deltaGroups
                 << ",\"measured_pch_bytes\":" << plan.TotalPchBytes
                 << ",\"pch_budget_bytes\":" << uint64_t(plan.Config.DiskBudgetBytes)
                 << ",\"targets\":" << m.Targets.size()
                 << ",\"assigned_targets\":" << assignedTargets
                 << ",\"cpp_files\":" << m.Files.size()
                 << ",\"assigned_cpp_files\":" << assignedFiles
                 << ",\"total_compressible_cpu_seconds\":" << plan.TotalCompressibleCpuSec
                 << ",\"covered_compressible_cpu_seconds\":" << plan.CoveredCompressibleCpuSec
                 << ",\"covered_compressible_percent\":" << compressiblePct
                 << ",\"net_saved_cpu_seconds\":" << plan.NetSavedCpuSec
                 << "}\n";
            WriteFile(JoinPath(planDir, "summary.json"), json.str());

            Report(human.str());

            const std::string reportRoot = ReportRootDir(m.Baseline);
            MakeDirs(reportRoot);
            for (const char* name : {"summary.tsv", "summary.txt", "summary.json", "assignments.tsv",
                                     "target-members.tsv", "selection-targets.tsv", "selection-candidates.tsv",
                                     "selection-payback.tsv", "compatibility.tsv", "compatibility-targets.tsv",
                                     "disk-budget.tsv", "final-payback.tsv"}) {
                std::string data;
                if (TryReadFile(JoinPath(planDir, name), &data)) {
                    WriteFile(JoinPath(reportRoot, name), data);
                }
            }
        } catch (const std::exception& e) {
            Log("warning: report publication failed: " + std::string(e.what()));
        }
    }

    void ApplyAndValidatePlan(const TWorkflowConfig& cfg, const TPlanSnapshot& plan, const std::string& planDir) {
        const TAnalysisModel& m = plan.Model;
        ValidateAnalysisInputs(m, cfg.Workers);
        if (cfg.DiskBudgetBytes > 0.0 && double(plan.TotalPchBytes) > cfg.DiskBudgetBytes + 0.5) {
            Fail(Format("measured PCH total (%llu bytes) exceeds --disk cap (%.0f bytes)",
                        static_cast<unsigned long long>(plan.TotalPchBytes), cfg.DiskBudgetBytes));
        }
        RequireBuildIdle(m.Baseline);
        RequireBuildIdle(JoinPath(planDir, "global"));

        ExportGroupsTree(plan, planDir);

        const std::string liveDir = JoinPath(m.Root, GLOBAL_DIR);
        const std::string backupDir = JoinPath(planDir, "original-global");
        if (!Exists(JoinPath(planDir, "original.ready"))) {
            RemoveTree(backupDir);
            CopyDirectory(liveDir, backupDir);
            WriteFile(JoinPath(planDir, "original.ready"), "ready\n");
        }

        const std::string tx = MakeTempDir(DirName(liveDir), ".pch-install-");
        WriteFile(JoinPath(planDir, "installing"), tx + "\n");
        try {
            const std::string staged = JoinPath(tx, "new");
            CopyDirectory(JoinPath(planDir, "export"), staged);
            std::filesystem::rename(liveDir, JoinPath(tx, "previous"));
            std::filesystem::rename(staged, liveDir);

            const std::string globalBuild = JoinPath(planDir, "global");
            MakeDirs(globalBuild);
            const std::string globalArgs = m.ArgsGn + "\npch_mode = \"global\"\ntime_trace = \"none\"\n";
            WriteFileIfChanged(JoinPath(globalBuild, "args.gn"), globalArgs);

            std::vector<std::string> gnCmd = {
                m.GnPath, "gen", globalBuild, "--root=" + m.Root, "--export-compile-commands"
            };
            gnCmd.insert(gnCmd.end(), m.GnSwitches.begin(), m.GnSwitches.end());
            TRunOptions gnRun;
            gnRun.Cwd = m.Root;
            gnRun.CaptureStdout = gnRun.CaptureStderr = false;
            gnRun.OutputLabel = "gn-gen-global";
            RunChecked(gnCmd, gnRun);

            CheckGnDeployment(plan, globalBuild);

            if (cfg.VerifyBuild != EVerifyBuild::None) {
                std::vector<std::string> ninjaTargets;
                if (cfg.VerifyBuild == EVerifyBuild::Full) {
                    ninjaTargets = m.WorkloadTargets;
                } else {
                    for (size_t g = 0; g < plan.Layout.Groups.size(); ++g) {
                        ninjaTargets.push_back(PchArtifactRelPath(g));
                    }
                }
                if (!ninjaTargets.empty()) {
                    std::vector<std::string> ninjaCmd = {
                        m.NinjaPath, "-C", globalBuild, "-j", std::to_string(cfg.Workers)
                    };
                    ninjaCmd.insert(ninjaCmd.end(), ninjaTargets.begin(), ninjaTargets.end());
                    TRunOptions ninjaRun;
                    ninjaRun.Cwd = m.Root;
                    ninjaRun.CaptureStdout = ninjaRun.CaptureStderr = false;
                    ninjaRun.OutputLabel = "gn-verify-build";
                    RunChecked(ninjaCmd, ninjaRun);
                }
            }

            WriteFile(JoinPath(planDir, "applied.tree"), TreeIdentity(m.Root, false) + "\n");
            FinishInstallation(m.Root, planDir);
            Report("Installed and validated GN global PCH groups: " + liveDir);

            try {
                PublishPlanReports(plan, planDir);
                const std::string reportRoot = ReportRootDir(m.Baseline);
                const std::string finalJson = Format(
                    "{\"schema\":2,\"status\":\"applied-and-validated\",\"verify_build\":%s,"
                    "\"pchs\":%zu,\"measured_pch_bytes\":%llu,\"net_saved_cpu_seconds\":%.6f,"
                    "\"tests_executed\":false,\"installed_groups\":%s}\n",
                    JsonStringLiteral(VerifyBuildName(cfg.VerifyBuild)).c_str(),
                    plan.Layout.Groups.size(),
                    static_cast<unsigned long long>(plan.TotalPchBytes),
                    plan.NetSavedCpuSec,
                    JsonStringLiteral(liveDir).c_str());
                WriteFile(JoinPath(reportRoot, "final-result.json"), finalJson);
            } catch (const std::exception& e) {
                Log("warning: final report write failed: " + std::string(e.what()));
            }
        } catch (...) {
            RecoverInstallation(m.Root, planDir);
            throw;
        }
    }

} // namespace NPchUpdate
