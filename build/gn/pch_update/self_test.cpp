#include "pch_model.h"
#include "workflow.h"

#include "common.h"
#include "facts.h"
#include "gni.h"
#include "ninja.h"
#include "steps.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>

namespace NPchUpdate {

    namespace {

        void Check(bool ok, const std::string& msg) {
            if (!ok) {
                Fail("self-test: " + msg);
            }
        }

        void Near(double actual, double expected, double tol, const std::string& msg) {
            if (std::abs(actual - expected) > tol) {
                Fail(Format("self-test: %s (got %.6f, expected %.6f)", msg.c_str(), actual, expected));
            }
        }

        std::string TempBaseDir() {
            const char* tmp = std::getenv("TMPDIR");
            if (tmp && *tmp) { return tmp; }
            const char* out = std::getenv("OUT");
            if (out && *out) { return out; }
            return "/tmp";
        }

        struct TTempDirGuard {
            std::string Path;
            explicit TTempDirGuard(const std::string& prefix)
                : Path(MakeTempDir(TempBaseDir(), prefix))
            {
            }
            ~TTempDirGuard() {
                try { RemoveTree(Path); } catch (...) {}
            }
        };

        void TestGnParserAndPaths() {
            const auto gnArgs = ParseGnFile(
                "pch_mode = \"global\"\ntime_trace = \"none\"\n"
                "# pch_update baseline\npch_mode = \"none\"\ntime_trace = \"json\"\n"
                "flags = [ \"-a\", \"-b\", \"-c\" ]\nflags += [ \"-d\" ]\nflags -= [ \"-b\" ]\n"
                "reset = [ \"-x\" ]\nreset += [ \"-y\" ]\nreset = [ \"-z\" ]\n"
                "scope = { k = 1 k = 2 }\n");
            Check(FindGnValue(gnArgs, "pch_mode")->Text == "none" &&
                  FindGnValue(gnArgs, "time_trace")->Text == "json",
                  "last assignment in args.gn overrides earlier values");
            Check(FindGnValue(gnArgs, "flags")->Strings() == std::vector<std::string>({"-a", "-c", "-d"}) &&
                  FindGnValue(gnArgs, "reset")->Strings() == std::vector<std::string>({"-z"}) &&
                  FindGnValue(gnArgs, "scope")->Member("k")->IntValue == 2,
                  "GN += and -= fold onto the active assignment while '=' replaces it");

            TTempDirGuard tmp("pch-paths-test-");
            const std::string root = JoinPath(tmp.Path, "src");
            const std::string buildInTree = JoinPath(root, "out/gn");
            MakeDirs(buildInTree);

            TFacts facts;
            facts.Paths = TPaths(buildInTree, root, "/");
            Check(facts.Paths.Normalize(".") == "<out>" &&
                  facts.Paths.Real("<out>") == facts.Paths.BuildDir &&
                  facts.Paths.Normalize("gen/a.h") == "<out>/gen/a.h" &&
                  facts.Paths.Normalize("../..") == "." &&
                  facts.Paths.Real(".") == facts.Paths.SourceRoot &&
                  facts.Paths.Normalize("../../util/x.h") == "util/x.h" &&
                  GnDir(facts, ".") == "$root_build_dir" &&
                  GnDir(facts, "../..") == "//",
                  "TPaths normalizes root_build_dir and sourceRoot cleanly even when build lives under source");

            const auto shallow = CanonicalizeFlags(
                {"/tool/clang++", "-ffile-prefix-map=../=", "-DVER=\"1..2\"", "--sysroot=../../sysroot"},
                JoinPath(root, "out"), root);
            Check(shallow == std::vector<std::string>({
                      "/tool/clang++", "-ffile-prefix-map=<root>/=", "-DVER=\"1..2\"", "--sysroot=../../sysroot"}),
                  "shallow relative root replacement does not corrupt '..' inside unrelated flags");

            const std::string baselineBuild = JoinPath(tmp.Path, "baseline");
            const std::string globalBuild = JoinPath(tmp.Path, "global");
            MakeDirs(baselineBuild);
            MakeDirs(globalBuild);

            const auto canonBase = CanonicalizeFlags({
                "/tool/clang++", "--sysroot=/sysroot",
                "-ffile-prefix-map=" + root + "/=",
                "-I" + root, "-I.", "-I", ".", "-I" + baselineBuild + "/gen",
                "-idirafter", "gen/after", "--sysroot=/target-override",
                "-include", "gen/driver.h", "-Xclang", "-include", "-Xclang", "gen/forwarded.h"
            }, baselineBuild, root);

            const auto canonNested = CanonicalizeFlags({
                "/tool/clang++", "--sysroot=/sysroot",
                "-ffile-prefix-map=" + RelPath(root, globalBuild) + "/=",
                "-I" + RelPath(root, globalBuild), "-I.", "-I", ".", "-Igen",
                "-idirafter", "gen/after", "--sysroot=/target-override",
                "-include", "gen/driver.h", "-Xclang", "-include", "-Xclang", "gen/forwarded.h"
            }, globalBuild, root);

            Check(canonBase == canonNested, "canonical flags match across baseline and separate global build dirs");

            TAnalysisModel exportModel;
            exportModel.Root = root;
            exportModel.Baseline = baselineBuild;
            exportModel.ClassFlags = {canonBase};
            const std::string exportedGni = FormatExportPaths() + FormatExportFlags(exportModel, 0);
            Check(exportedGni.find("rebase_path(\"//BUILD.gn\", root_build_dir)") != std::string::npos &&
                  exportedGni.find("\"-Xclang\"") != std::string::npos &&
                  exportedGni.find("\"-idirafter\"") != std::string::npos,
                  "FormatExportFlags emits portable root_build_dir-relative paths and preserves cc1/idirafter flags");
        }

        void TestRuntimeSymbolFilter() {
            Check(IsCxxRuntimeOnlySymbols({}) &&
                  IsCxxRuntimeOnlySymbols({
                      "_Znwm", "_ZnwmSt11align_val_t", "_Znam", "_ZdlPv", "_ZdlPvm", "_ZdaPv",
                      "_ZnwmRKSt9nothrow_t", "_ZdlPvSt11align_val_t",
                      "__cxa_atexit", "__cxa_guard_acquire", "__gxx_personality_v0",
                      "_Unwind_Resume", "__clang_call_terminate",
                      "_ZTVN10__cxxabiv117__class_type_infoE",
                      "_ZNSt3__18ios_base4InitC1Ev", "_ZSt4cout",
                      "_ZNKSt3__19basic_iosIcNS_11char_traitsIcEEE4clearEj",
                      "_ZZNSt3__111this_thread6get_idEvE17__thread_local_id",
                      "_ZThn16_NSt3__16localeC1Ev",
                      "_ZGVZNKSt3__16locale5globalEvE4none",
                      "_ZTv0_24_NSt3__19basic_ostreamIcNS_11char_traitsIcEEE5flushEv",
                      "_ZNSt3__15arrayIiLm16EE4dataEv",
                      "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEC1EPKc",
                  }),
                  "standard C++ runtime symbols are accepted for base PCH eligibility");

            for (const char* nonRuntime : {
                     "_ZnwmR7MyArena",
                     "_ZdlPvR7MyArena",
                     "_ZNKSt3__14hashI13MyProjectTypeEclERKS1_",
                     "_ZNKSt3__14hashIN7example3KeyEEclERKS2_",
                     "_ZNSt3__19formatterI9MyLogItemcE6formatERKS1_",
                     "_ZNSt3__1lsIcNS_11char_traitsIcEEEERNS_13basic_ostreamIT_T0_EES7_RK9MyLogItem",
                     "_ZN7example8RegisterERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE",
                     "_ZneRK7example3KeyS2_",
                     "Provided",
                     "_ZZN7example3fooEvE5count",
                     "_ZTV7example3Base",
                     "_ZThn16_NK7example3Base3runEv",
                     "_Z1fISt6vectorIiEEvv",
                     "_ZL9StaticFuncv",
                     "_ZN3foo3barEv",
                 }) {
                Check(!IsStrictRuntimeSymbol(nonRuntime),
                      std::string("project symbol rejected from base PCH: ") + nonRuntime);
            }
        }

        void TestLinkGraphAndLayers() {
            TSymbolProviders providers = {{"needed", {"provider.a", "alt.a"}}, {"unknown", {}}};
            std::vector<TLinkLine> links = {{"app", {"consumer.o", "provider.a"}},
                                            {"test", {"consumer.o", "alt.a"}}};
            Check(CheckLinkRequirements({"needed"}, providers, links).Available(),
                  "different providers may satisfy the same symbol across final links");
            links[1].Inputs.erase("alt.a");
            const auto missing = CheckLinkRequirements({"needed", "needed"}, providers, links);
            Check(!missing.Available() && missing.Missing.at("needed") == std::vector<std::string>{"test"},
                  "missing provider on one final link is detected without duplicate entries");
            Check(!CheckLinkRequirements({"needed"}, providers, {}).Available(),
                  "absent final links are treated as unknown, never available");

            TTempDirGuard tmp("pch-link-graph-");
            WriteFile(JoinPath(tmp.Path, "build.ninja"),
                "rule cxx\n  command = c++ -c $in -o $out\n"
                "rule alink\n  command = ar rcs $out $in\n"
                "rule link\n  command = c++ $in -o $out\n"
                "rule solink\n  command = c++ -shared $in -o $out\n"
                "build first.o: cxx first.cpp\n"
                "build member.o: cxx member.cpp\n"
                "build shared.o: cxx shared.cpp\n"
                "build order.o: cxx order.cpp\n"
                "build archive.a: alink member.o\n"
                "build shared.so shared.so.TOC: solink shared.o\n"
                "build app: link first.o archive.a | shared.so.TOC || order.o\n");
            TNinjaManifest manifest(tmp.Path);
            const auto inputs = CollectLinkInputs(manifest, {
                {"first.o", "first.o"}, {"member.o", "member.o"},
                {"shared.o", "shared.o"}, {"order.o", "order.o"}, {"app", "app"}
            });
            Check(inputs.Targets.at("first.o").size() == 1 &&
                  inputs.Lines.at(inputs.Targets.at("first.o")[0]).Inputs ==
                      std::set<std::string>({"first.o", "archive.a", "shared.so"}) &&
                  inputs.Targets.at("shared.o").size() == 1 &&
                  inputs.Lines.at(inputs.Targets.at("shared.o")[0]).Output == "shared.so" &&
                  inputs.Targets.at("member.o").size() == 1 &&
                  !inputs.Targets.count("order.o"),
                  "CollectLinkInputs resolves solink .TOC to .so, propagates archives, and ignores order-only deps");

            TAnalysisModel layered;
            layered.Units.resize(7);
            layered.Units[0] = {"<vector>", {"contrib/libs/cxxsupp/libcxx/include/vector"}, {}, {}, {}, 1, 1, true, false};
            layered.Units[1] = {"<sys.h>", {"<sysroot>/usr/include/sys.h"}, {}, {}, {}, 1, 1, true, false};
            layered.Units[2] = {"<contrib/lib.h>", {"contrib/libs/some/lib.h"}, {}, {}, {}, 1, 1, true, false};
            layered.Units[3] = {"\"util/dir.h\"", {"util/dir.h"}, {}, {}, {}, 1, 1, true, false};
            layered.Units[4] = {"\"library/cpp/str.h\"", {"library/cpp/str.h"}, {}, {}, {}, 1, 1, true, false};
            layered.Units[5] = {"\"ydb/core/row.h\"", {"ydb/core/row.h"}, {}, {}, {}, 1, 1, true, false};
            layered.Units[6] = {"\"gen/foo.pb.h\"", {"<out>/gen/foo.pb.h"}, {}, {}, {}, 1, 1, true, true};
            Check(LayeredRootOrder(layered, {6, 5, 4, 3, 2, 1, 0}) == std::vector<int>({1, 0, 2, 4, 3, 5, 6}),
                  "LayeredRootOrder sorts sysroot/libcxx, contrib, util/library, project and generated headers");
        }

        void TestGreedyTwoLayerSelector() {
            // 1. Unprofitable-alone shared header hoisted into Base when shared by 2 profitable deltas.
            {
                TAnalysisModel m;
                m.ClassFlags = {{"clang++", "-O2"}};
                m.Scenarios = 10;
                m.Units.resize(3);
                m.Units[0] = {"\"util/heavy_shared.h\"", {"util/heavy_shared.h"}, {}, {}, {}, 2.0, 8e6 / 6.2, true, false};
                m.Units[1] = {"\"sub1/api.h\"", {"sub1/api.h"}, {0}, {}, {}, 3.0, 1e6 / 6.2, false, false};
                m.Units[2] = {"\"sub2/api.h\"", {"sub2/api.h"}, {0}, {}, {}, 3.0, 1e6 / 6.2, false, false};
                for (int t = 0; t < 20; ++t) {
                    const int sub = (t < 10) ? 1 : 2;
                    m.Files.push_back({"t" + std::to_string(t) + ".o", "t.cpp", "t.cpp", 0, t, true, 30.0, 25.0, 16.0, {}, {{0, 0.2}, {sub, 15.0}}});
                    m.Targets.push_back({"//sub:t" + std::to_string(100 + t), 0, true, "", {t}});
                }
                TModelParams p;
                p.Q = 1.0;
                p.MinRatio = 0.5;
                const TPchLayout l1 = SelectGreedyLayout(m, p);
                p.AnalysisJobs = 4;
                const TPchLayout l4 = SelectGreedyLayout(m, p);
                Check(l1.Groups.size() == 3 && l1.Groups[0].Parent == -1 &&
                      l1.Groups[0].Roots == std::vector<int>({0}) &&
                      l1.Groups[1].Parent == 0 && l1.Groups[2].Parent == 0,
                      "shared safe unit unprofitable alone is hoisted into Base beneath two profitable deltas");
                Check(l1.Assignment == l4.Assignment && l1.Groups.size() == l4.Groups.size(),
                      "parallel class selection is deterministic");
                const auto acc = EvaluateLayoutAccounting(m, p, l1);
                Near(double(acc.TotalPchBytes), 10e6, 1e4, "shared Base + 2 Deltas avoids duplicating the 8 MB base");
            }

            // 2. Twin small targets (40 small targets sharing a 20 MB subsystem header) are bundled together.
            {
                TAnalysisModel m;
                m.ClassFlags = {{"clang++", "-O2"}};
                m.Scenarios = 10;
                m.Units.push_back({"\"sub/twin.h\"", {"sub/twin.h"}, {}, {}, {}, 3.0, 20e6 / 6.2, false, false});
                for (int t = 0; t < 40; ++t) {
                    m.Files.push_back({"tw" + std::to_string(t) + ".o", "tw.cpp", "tw.cpp", 0, t, true, 10.0, 8.0, 2.0, {}, {{0, 2.0}}});
                    m.Targets.push_back({"//twin:t" + std::to_string(t), 0, true, "", {t}});
                }
                TModelParams p;
                p.Q = 1.0;
                p.MinRatio = 0.5;
                const TPchLayout l = SelectGreedyLayout(m, p);
                Check(l.Groups.size() == 1 &&
                      std::all_of(l.Assignment.begin(), l.Assignment.end(), [](int g) { return g == 0; }),
                      "40 small twin targets bundle together to clear --min-ratio");
            }

            // 3. Supermodularity wakeup on co-changed roots + real-PCH size feedback + --disk cap.
            {
                TAnalysisModel m;
                m.ClassFlags = {{"clang++", "-O2"}};
                m.Scenarios = 2;
                m.Units.push_back({"<root0>", {"r0.h"}, {}, {0}, {}, 1.0, 1e6 / 6.2, true, false});
                m.Units.push_back({"<root1>", {"r1.h"}, {}, {0}, {}, 1.0, 1e6 / 6.2, true, false});
                for (int t = 0; t < 4; ++t) {
                    m.Files.push_back({"c" + std::to_string(t) + ".o", "c.cpp", "c.cpp", 0, t, true, 10.0, 9.5, 9.5, {1}, {{0, 7.5}, {1, 2.0}}});
                    m.Targets.push_back({"//co:t" + std::to_string(t), 0, true, "", {t}});
                }
                TModelParams p;
                p.Q = 0.0;
                p.MinRatio = 0.5;
                const TPchLayout both = SelectGreedyLayout(m, p);
                Check(both.Groups.size() == 1 && both.Groups[0].Units == std::vector<int>({0, 1}),
                      "co-changed deferred root wakes up once the shared commit penalty is paid");

                // Simulate measured PCH feedback showing root1 bloated to 100 MB -> rejected by MinRatio!
                TSelectorFeedback fb;
                fb.UnitBytesScale = {{6.2, 620.0}};
                const TPchLayout trimmed = SelectGreedyLayout(m, p, fb);
                Check(trimmed.Groups.size() == 1 && trimmed.Groups[0].Units == std::vector<int>({0}),
                      "measured byte scale feedback trims bloated root while keeping dense prefix");
            }

            // 4. Cycle prevention via ForbiddenTargets & optional --disk cap eviction.
            {
                TAnalysisModel m;
                m.ClassFlags = {{"clang++", "-O2"}};
                m.Scenarios = 1;
                m.Units.push_back({"<base.h>", {"base.h"}, {}, {}, {}, 2.0, 1e6 / 6.2, true, false});
                m.Units.push_back({"\"gen/proto.pb.h\"", {"<out>/gen/proto.pb.h"}, {}, {}, {0}, 3.0, 4e6 / 6.2, true, true});
                for (int t = 0; t < 5; ++t) {
                    m.Files.push_back({"g" + std::to_string(t) + ".o", "g.cpp", "g.cpp", 0, t, true, 20.0, 18.0, 13.0, {}, {{0, 5.0}, {1, 8.0}}});
                    m.Targets.push_back({"//cyc:t" + std::to_string(t), 0, true, "", {t}});
                }
                TModelParams p;
                p.Q = 1.0;
                p.MinRatio = 0.5;
                const TPchLayout cyc = SelectGreedyLayout(m, p);
                Check(cyc.Groups.size() == 2 && cyc.Assignment[0] == 0 && cyc.Assignment[1] == 1,
                      "upstream generator target stays on acyclic Base while consumers use generated Delta");

                p.DiskBudgetBytes = 1.5e6; // Fits 1 MB Base, evicts 4 MB Delta!
                const TPchLayout capped = SelectGreedyLayout(m, p);
                Check(capped.Groups.size() == 1 &&
                      std::all_of(capped.Assignment.begin(), capped.Assignment.end(), [](int g) { return g == 0; }),
                      "optional --disk cap evicts Delta and falls consumers back to Base");
            }
        }

        void TestPersistenceAndTransactionalRecovery() {
            TTempDirGuard tmp("pch-persist-");

            // 1. Nested ParallelFor thread budget sharing.
            {
                std::atomic<int> active{0}, peak{0};
                ParallelFor(4, 32, [&](size_t) {
                    ParallelFor(200, 32, [&](size_t) {
                        const int cur = ++active;
                        for (int old = peak.load(); cur > old && !peak.compare_exchange_weak(old, cur);) {}
                        std::this_thread::sleep_for(std::chrono::microseconds(200));
                        --active;
                    });
                });
                Check(peak.load() >= 1 && peak.load() <= 32, "nested ParallelFor stays within total thread budget");
            }

            // 2. Workflow state & plan snapshot serialization + tamper detection.
            const std::string statePath = JoinPath(tmp.Path, "state.bin");
            TWorkflowConfig cfg;
            cfg.Root = "/src";
            cfg.Baseline = "/out";
            cfg.Mode = "global";
            cfg.Q = 1.0;
            cfg.MinRatio = 0.75;
            cfg.DiskBudgetBytes = 8e9;
            cfg.Workers = 16;
            cfg.AnalysisJobs = 8;
            cfg.RamBytes = 32e9;
            cfg.ActionMemoryBytes = 1e9;
            cfg.VerifyBuild = EVerifyBuild::PchOnly;
            SaveWorkflowState(statePath, cfg);
            Check(LoadWorkflowState(statePath) == cfg, "TWorkflowConfig survives binary round-trip");

            std::string rawState = ReadFile(statePath);
            rawState.back() ^= 0x01;
            WriteFile(statePath, rawState);
            bool rejected = false;
            try { (void)LoadWorkflowState(statePath); } catch (const TError&) { rejected = true; }
            Check(rejected, "corrupted state.bin is rejected by checksum");

            TPlanSnapshot snap;
            snap.ToolDigest = "tool-1";
            snap.TreeHash = "tree-1";
            snap.Config = cfg;
            snap.Model.Root = "/src";
            snap.Model.Baseline = "/out";
            snap.Model.ClassFlags = {{"clang++", "-O2"}};
            snap.Model.Units.push_back({"<v>", {"v"}, {}, {0}, {}, 1.5, 100.0, true, false});
            snap.Model.Files.push_back({"a.o", "a.cpp", "a.cpp", 0, 0, true, 5.0, 4.0, 2.0, {0}, {{0, 1.5}}});
            snap.Model.Targets.push_back({"//:a", 0, true, "", {0}});
            snap.Layout.Groups.push_back({0, -1, 0, {0}, {0}, 12345, 0.4, {"gen/x.h"}});
            snap.Layout.Assignment = {0};
            snap.Layout.Reasons = {"selected"};
            snap.TotalCompressibleCpuSec = 10.0;
            snap.CoveredCompressibleCpuSec = 9.0;
            snap.NetSavedCpuSec = 7.5;
            snap.TotalPchBytes = 12345;

            const std::string planPath = JoinPath(tmp.Path, "plan.bin");
            SavePlanSnapshot(planPath, snap);
            const TPlanSnapshot loaded = LoadPlanSnapshot(planPath);
            Check(loaded.Config == cfg && loaded.TotalPchBytes == 12345 &&
                  loaded.Layout.Groups.size() == 1 && loaded.Layout.Groups[0].GeneratedDeps == std::vector<std::string>{"gen/x.h"},
                  "TPlanSnapshot survives binary round-trip");

            // 3. Transactional --apply recovery and user-edit protection.
            const std::string root = JoinPath(tmp.Path, "worktree");
            const std::string planDir = JoinPath(tmp.Path, "plan");
            const std::string live = JoinPath(root, GLOBAL_DIR);
            WriteFile(JoinPath(planDir, "original-global/groups.gni"), "old");
            WriteFile(JoinPath(planDir, "export/groups.gni"), "new");
            WriteFile(JoinPath(live, "groups.gni"), "external user edit");
            WriteFile(JoinPath(planDir, "installing"), "");

            rejected = false;
            try { RecoverInstallation(root, planDir); } catch (const TError&) { rejected = true; }
            Check(rejected, "RecoverInstallation refuses to clobber external user edits");

            WriteFile(JoinPath(live, "groups.gni"), "new");
            RecoverInstallation(root, planDir);
            Check(ReadFile(JoinPath(live, "groups.gni")) == "old" && !Exists(JoinPath(planDir, "installing")),
                  "interrupted apply cleanly rolls back to original-global");

            const std::string stateDir = JoinPath(tmp.Path, "scratch/.pch_update");
            WriteFile(JoinPath(stateDir, "cache/item"), "data");
            DiscardWorkflowState(stateDir);
            Check(!Exists(stateDir) && !Exists(stateDir + ".cleanup"),
                  "DiscardWorkflowState atomically renames and removes scratch directory");
        }

        void TestRealClangPreflight(const std::string& cxx) {
            TTempDirGuard tmp("pch-clang-selftest-");
            const std::string root = JoinPath(tmp.Path, "src");
            const std::string baseline = JoinPath(tmp.Path, "baseline");
            const std::string cacheDir = JoinPath(tmp.Path, "cache");
            const std::string planDir = JoinPath(tmp.Path, "plan");
            MakeDirs(root);
            MakeDirs(baseline);

            std::string ninjaPath;
            try { ninjaPath = ResolveProgram("ninja"); } catch (const TError&) { return; }

            // Initialize minimal git repo so TreeIdentity works in CheckTargetsAndFinalizePlan.
            RunChecked({"git", "init", "--quiet", root});
            WriteFile(JoinPath(root, "base.h"), "#pragma once\nstruct BaseType { int v = 1; };\n");
            WriteFile(JoinPath(root, "broken.h"), "#pragma once\n#error broken_pch_root\n");
            WriteFile(JoinPath(root, "leaf.h"),
                      "#pragma once\nextern \"C\" int ProvidedSym();\nstatic const int Reg = ProvidedSym();\n");
            WriteFile(JoinPath(root, "provider.cpp"), "extern \"C\" int ProvidedSym() { return 42; }\n");
            WriteFile(JoinPath(root, "allowed.cpp"), "int AllowedFn() { return 1; }\n");
            WriteFile(JoinPath(root, "denied.cpp"), "int DeniedFn() { return 2; }\n");
            RunChecked({"git", "-C", root, "add", "."});
            RunChecked({"git", "-C", root, "-c", "user.name=test", "-c", "user.email=test@invalid",
                        "-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false",
                        "commit", "--quiet", "-m", "init"});

            TRunOptions buildRun;
            buildRun.Cwd = baseline;
            const std::vector<std::string> baseFlags = {cxx, "-std=c++20", "-O2", "-fPIC", "-I" + root};
            for (const char* name : {"allowed", "denied"}) {
                auto cmd = baseFlags;
                cmd.insert(cmd.end(), {"-c", JoinPath(root, std::string(name) + ".cpp"), "-o", std::string(name) + ".o"});
                RunChecked(cmd, buildRun);
            }
            {
                auto cmd = baseFlags;
                cmd.insert(cmd.end(), {"-shared", "-nostdlib", "-fuse-ld=lld", JoinPath(root, "provider.cpp"), "-o", "libprov.so"});
                RunChecked(cmd, buildRun);
            }
            WriteFile(JoinPath(baseline, "libprov.so.TOC"), "ProvidedSym T\n");
            WriteFile(JoinPath(baseline, "args.gn"), "pch_mode = \"none\"\ntime_trace = \"json\"\n");
            WriteFile(JoinPath(baseline, "build.ninja"),
                "rule cxx\n  command = true\nrule link\n  command = true\nrule solink\n  command = true\n"
                "build allowed.o: cxx\nbuild denied.o: cxx\n"
                "build libprov.so libprov.so.TOC: solink\n"
                "build allowed_app: link allowed.o | libprov.so.TOC\n"
                "build denied_app: link denied.o\n");

            std::ostringstream cdb;
            cdb << "[";
            for (int i = 0; i < 2; ++i) {
                const std::string name = i == 0 ? "allowed" : "denied";
                const std::string src = JoinPath(root, name + ".cpp");
                auto cmd = baseFlags;
                cmd.insert(cmd.end(), {"-c", src, "-o", name + ".o"});
                if (i) { cdb << ","; }
                cdb << "{\"directory\":" << std::quoted(baseline)
                    << ",\"file\":" << std::quoted(src)
                    << ",\"command\":" << std::quoted(ShellJoin(cmd)) << "}";
            }
            cdb << "]";
            WriteFile(JoinPath(baseline, "compile_commands.json"), cdb.str());

            TNinjaManifest manifest(baseline);
            TAnalysisModel m;
            m.Root = root;
            m.Baseline = baseline;
            m.NinjaPath = ninjaPath;
            m.CompilerPath = cxx;
            m.PchOnlyFlags = {"-fpch-instantiate-templates"};
            m.ClassFlags = {CanonicalizeFlags(baseFlags, baseline, root)};
            m.Scenarios = 1;
            m.Units = {
                {"\"base.h\"", {"base.h"}, {}, {}, {}, 2.0, 2000.0, true, false},
                {"\"broken.h\"", {"broken.h"}, {}, {}, {}, 2.0, 2000.0, true, false},
                {"\"leaf.h\"", {"leaf.h"}, {0}, {}, {}, 4.0, 2000.0, false, false},
            };
            m.Files = {
                {"allowed.o", "allowed.cpp", JoinPath(root, "allowed.cpp"), 0, 0, true, 20.0, 18.0, 15.0, {}, {{0, 5.0}, {1, 5.0}, {2, 8.0}}},
                {"denied.o", "denied.cpp", JoinPath(root, "denied.cpp"), 0, 1, true, 20.0, 18.0, 15.0, {}, {{0, 5.0}, {1, 5.0}, {2, 8.0}}},
            };
            m.Targets = {
                {"//:allowed", 0, true, "", {0}},
                {"//:denied", 0, true, "", {1}},
            };
            m.LinkInputs = CollectLinkInputs(manifest, {{"allowed.o", "allowed.o"}, {"denied.o", "denied.o"}});
            for (const char* f : {"base.h", "broken.h", "leaf.h", "allowed.cpp", "denied.cpp"}) {
                const std::string p = JoinPath(root, f);
                m.InputDigests.emplace_back(p, DigestFile(p));
            }

            TWorkflowConfig cfg;
            cfg.Root = root;
            cfg.Baseline = baseline;
            cfg.Mode = "global";
            cfg.Q = 1.0;
            cfg.MinRatio = 0.01;
            cfg.Workers = 2;
            cfg.AnalysisJobs = 2;
            cfg.Ninja = ninjaPath;

            TPlanSnapshot plan = SelectAndMaterializePchs(cfg, m, cacheDir, planDir);
            CheckTargetsAndFinalizePlan(cfg, m, plan, cacheDir, planDir);

            Check(plan.Layout.Groups.size() == 2 &&
                  plan.Layout.Groups[0].Roots == std::vector<int>({0}) &&
                  plan.Layout.Groups[1].Roots == std::vector<int>({2}) &&
                  plan.Layout.Assignment == std::vector<int>({1, 0}),
                  "real Clang preflight bisects broken root, keeps provider-backed delta for allowed, and demotes denied to base");
            Report("real Clang preflight self-test: OK");
        }

    } // namespace

    int RunSelfTests() {
        TestGnParserAndPaths();
        TestRuntimeSymbolFilter();
        TestLinkGraphAndLayers();
        TestGreedyTwoLayerSelector();
        TestPersistenceAndTransactionalRecovery();
        if (const char* cxx = std::getenv("PCH_UPDATE_TEST_CXX"); cxx && *cxx) {
            TestRealClangPreflight(ResolveProgram(cxx, {}, false));
        }
        Report("pch_update self-tests: OK");
        return 0;
    }

} // namespace NPchUpdate
