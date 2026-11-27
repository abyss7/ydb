#include "pch_model.h"

#include "common.h"
#include "elf.h"
#include "facts.h"
#include "jobs.h"
#include "ninja.h"
#include "steps.h"
#include "trace.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <set>
#include <unordered_map>

namespace NPchUpdate {

    namespace {

        constexpr std::string_view PCH_PLATFORM_DEPS[] = {
            "//contrib/libs/linux-headers:linux-headers",
            "//contrib/libs/cxxsupp/libcxx:libcxx",
        };

        template <class T>
        void SortUnique(std::vector<T>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        }

        bool IsCompileRule(const std::string& rule) {
            return rule == "cc" || rule == "cxx" || rule == "asm";
        }

        bool IsLinkRule(const std::string& rule) {
            return rule == "link" || rule == "solink";
        }

        void ReplaceAll(std::string& text, const std::string& from, const std::string& to) {
            if (from.empty()) {
                return;
            }
            size_t pos = 0;
            while ((pos = text.find(from, pos)) != std::string::npos) {
                text.replace(pos, from.size(), to);
                pos += to.size();
            }
        }

        int64_t MedianInt64(std::vector<int64_t> values) {
            if (values.empty()) {
                return 0;
            }
            std::sort(values.begin(), values.end());
            return values[values.size() / 2];
        }

        // Tarjan's strongly connected components; child SCCs receive lower IDs than parents.
        int32_t CondenseScc(const std::vector<std::vector<int32_t>>& children, std::vector<int32_t>* comp) {
            const int32_t n = int32_t(children.size());
            std::vector<int32_t> index(n, -1);
            std::vector<int32_t> low(n, 0);
            comp->assign(n, -1);
            std::vector<char> onStack(n, 0);
            std::vector<int32_t> stack;
            std::vector<std::pair<int32_t, size_t>> work;
            int32_t counter = 0;
            int32_t count = 0;
            for (int32_t root = 0; root < n; ++root) {
                if (index[root] != -1) {
                    continue;
                }
                index[root] = low[root] = counter++;
                stack.push_back(root);
                onStack[root] = 1;
                work.emplace_back(root, 0);
                while (!work.empty()) {
                    auto& [v, i] = work.back();
                    if (i < children[v].size()) {
                        const int32_t w = children[v][i++];
                        if (index[w] == -1) {
                            index[w] = low[w] = counter++;
                            stack.push_back(w);
                            onStack[w] = 1;
                            work.emplace_back(w, 0);
                        } else if (onStack[w]) {
                            low[v] = std::min(low[v], index[w]);
                        }
                        continue;
                    }
                    const int32_t done = v;
                    work.pop_back();
                    if (!work.empty()) {
                        const int32_t u = work.back().first;
                        low[u] = std::min(low[u], low[done]);
                    }
                    if (low[done] == index[done]) {
                        int32_t w = -1;
                        while (w != done) {
                            w = stack.back();
                            stack.pop_back();
                            onStack[w] = 0;
                            (*comp)[w] = count;
                        }
                        ++count;
                    }
                }
            }
            return count;
        }

        std::string ComputeBaselineStamp(const TAnalysisModel& m) {
            TBinWriter stamp;
            for (const char* name : {"args.gn", "build.ninja", "build.ninja.d", "compile_commands.json", ".ninja_log", ".ninja_deps"}) {
                const std::string path = JoinPath(m.Baseline, name);
                stamp.Str(path);
                stamp.Str(Exists(path) ? DigestFile(path) : std::string("missing"));
            }
            for (const std::string& lib : m.LinkInputs.Libraries) {
                const std::string path = NormPath(JoinPath(m.Baseline, lib));
                stamp.Str(path);
                stamp.I64(FileSize(path));
                stamp.I64(MtimeNs(path));
            }
            for (const TTuFile& file : m.Files) {
                const std::string object = JoinPath(m.Baseline, file.Object);
                stamp.Str(object);
                const bool objExists = Exists(object);
                stamp.I64(objExists ? 1 : 0);
                if (objExists) {
                    stamp.I64(FileSize(object));
                    stamp.I64(MtimeNs(object));
                }
                if (file.Traced) {
                    const size_t dot = object.rfind('.');
                    const std::string trace = FindTrace((dot == std::string::npos ? object : object.substr(0, dot)) + ".json");
                    stamp.Str(trace);
                    const bool trExists = Exists(trace);
                    stamp.I64(trExists ? 1 : 0);
                    if (trExists) {
                        stamp.I64(FileSize(trace));
                        stamp.I64(MtimeNs(trace));
                    }
                }
            }
            return HashHex(stamp.Data());
        }

        // =====================================================================
        // Binary serialization helpers
        // =====================================================================

        constexpr size_t MAX_TABLE_COUNT = 100'000'000;

        void WriteDouble(TBinWriter& out, double value) {
            uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            out.U64(bits);
        }

        double ReadDouble(TBinReader& in) {
            const uint64_t bits = in.U64();
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof(value));
            if (!std::isfinite(value)) {
                Fail("snapshot: non-finite floating-point value");
            }
            return value;
        }

        size_t ReadBoundedCount(TBinReader& in) {
            const uint64_t n = in.U64();
            if (n > MAX_TABLE_COUNT) {
                Fail("snapshot: oversized container");
            }
            return size_t(n);
        }

        void WriteIntVec(TBinWriter& out, const std::vector<int>& values) {
            out.U64(values.size());
            for (int v : values) {
                out.I64(v);
            }
        }

        std::vector<int> ReadIntVec(TBinReader& in) {
            const size_t n = ReadBoundedCount(in);
            std::vector<int> out(n);
            for (size_t i = 0; i < n; ++i) {
                const int64_t v = in.I64();
                if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
                    Fail("snapshot: int out of range");
                }
                out[i] = int(v);
            }
            return out;
        }

        void WriteModelBody(TBinWriter& out, const TAnalysisModel& m) {
            out.Str(m.SchemaVersion);
            out.Str(m.Root);
            out.Str(m.Baseline);
            out.Str(m.TreeHash);
            out.Str(m.BaselineHash);
            out.Str(m.ArgsGn);
            out.Str(m.GnPath);
            out.Str(m.NinjaPath);
            out.Str(m.CompilerPath);
            out.StrList(m.GnSwitches);
            out.StrList(m.PchOnlyFlags);
            out.StrList(m.WorkloadTargets);

            out.U64(m.ClassFlags.size());
            for (const auto& flags : m.ClassFlags) {
                out.StrList(flags);
            }

            out.U64(m.Units.size());
            for (const THeaderUnit& u : m.Units) {
                out.Str(u.Include);
                out.StrList(u.Headers);
                WriteIntVec(out, u.Children);
                WriteIntVec(out, u.Changes);
                WriteIntVec(out, u.ForbiddenTargets);
                WriteDouble(out, u.ParseSec);
                WriteDouble(out, u.SourceBytes);
                out.U64(u.SafeLink ? 1 : 0);
                out.U64(u.Generated ? 1 : 0);
            }

            out.U64(m.Files.size());
            for (const TTuFile& f : m.Files) {
                out.Str(f.Object);
                out.Str(f.Source);
                out.Str(f.RawSource);
                out.I64(f.Class);
                out.I64(f.Target);
                out.U64(f.Traced ? 1 : 0);
                WriteDouble(out, f.CompileSec);
                WriteDouble(out, f.FrontendSec);
                WriteDouble(out, f.HeaderSec);
                WriteIntVec(out, f.Changes);
                out.U64(f.Own.size());
                for (const auto& [unitId, sec] : f.Own) {
                    out.I64(unitId);
                    WriteDouble(out, sec);
                }
            }

            out.U64(m.Targets.size());
            for (const TTargetOwner& t : m.Targets) {
                out.Str(t.Label);
                out.I64(t.Class);
                out.U64(t.PchEligible ? 1 : 0);
                out.Str(t.IneligibleReason);
                WriteIntVec(out, t.Files);
            }

            out.U64(m.LinkInputs.Lines.size());
            for (const TLinkLine& line : m.LinkInputs.Lines) {
                out.Str(line.Output);
                out.U64(line.Inputs.size());
                for (const std::string& input : line.Inputs) {
                    out.Str(input);
                }
            }
            out.U64(m.LinkInputs.Targets.size());
            for (const auto& [obj, indices] : m.LinkInputs.Targets) {
                out.Str(obj);
                WriteIntVec(out, indices);
            }
            out.StrList(m.LinkInputs.Libraries);

            out.U64(m.InputDigests.size());
            for (const auto& [path, digest] : m.InputDigests) {
                out.Str(path);
                out.Str(digest);
            }
            out.I64(m.Scenarios);
        }

        TAnalysisModel ReadModelBody(TBinReader& in) {
            TAnalysisModel m;
            m.SchemaVersion = std::string(in.Str());
            m.Root = std::string(in.Str());
            m.Baseline = std::string(in.Str());
            m.TreeHash = std::string(in.Str());
            m.BaselineHash = std::string(in.Str());
            m.ArgsGn = std::string(in.Str());
            m.GnPath = std::string(in.Str());
            m.NinjaPath = std::string(in.Str());
            m.CompilerPath = std::string(in.Str());
            m.GnSwitches = in.StrList();
            m.PchOnlyFlags = in.StrList();
            m.WorkloadTargets = in.StrList();

            m.ClassFlags.resize(ReadBoundedCount(in));
            for (auto& flags : m.ClassFlags) {
                flags = in.StrList();
            }

            m.Units.resize(ReadBoundedCount(in));
            for (THeaderUnit& u : m.Units) {
                u.Include = std::string(in.Str());
                u.Headers = in.StrList();
                u.Children = ReadIntVec(in);
                u.Changes = ReadIntVec(in);
                u.ForbiddenTargets = ReadIntVec(in);
                u.ParseSec = ReadDouble(in);
                u.SourceBytes = ReadDouble(in);
                u.SafeLink = in.U64() != 0;
                u.Generated = in.U64() != 0;
            }

            m.Files.resize(ReadBoundedCount(in));
            for (TTuFile& f : m.Files) {
                f.Object = std::string(in.Str());
                f.Source = std::string(in.Str());
                f.RawSource = std::string(in.Str());
                f.Class = int(in.I64());
                f.Target = int(in.I64());
                f.Traced = in.U64() != 0;
                f.CompileSec = ReadDouble(in);
                f.FrontendSec = ReadDouble(in);
                f.HeaderSec = ReadDouble(in);
                f.Changes = ReadIntVec(in);
                const size_t ownCount = ReadBoundedCount(in);
                f.Own.resize(ownCount);
                for (size_t i = 0; i < ownCount; ++i) {
                    f.Own[i].first = int(in.I64());
                    f.Own[i].second = ReadDouble(in);
                }
            }

            m.Targets.resize(ReadBoundedCount(in));
            for (TTargetOwner& t : m.Targets) {
                t.Label = std::string(in.Str());
                t.Class = int(in.I64());
                t.PchEligible = in.U64() != 0;
                t.IneligibleReason = std::string(in.Str());
                t.Files = ReadIntVec(in);
            }

            m.LinkInputs.Lines.resize(ReadBoundedCount(in));
            for (TLinkLine& line : m.LinkInputs.Lines) {
                line.Output = std::string(in.Str());
                line.Inputs.clear();
                const size_t inputCount = ReadBoundedCount(in);
                for (size_t i = 0; i < inputCount; ++i) {
                    line.Inputs.insert(std::string(in.Str()));
                }
            }
            m.LinkInputs.Targets.clear();
            const size_t linkTargetCount = ReadBoundedCount(in);
            for (size_t i = 0; i < linkTargetCount; ++i) {
                std::string obj(in.Str());
                m.LinkInputs.Targets.emplace(std::move(obj), ReadIntVec(in));
            }
            m.LinkInputs.Libraries = in.StrList();

            m.InputDigests.resize(ReadBoundedCount(in));
            for (auto& [path, digest] : m.InputDigests) {
                path = std::string(in.Str());
                digest = std::string(in.Str());
            }
            m.Scenarios = int(in.I64());
            return m;
        }

        void WriteWorkflowConfigBody(TBinWriter& out, const TWorkflowConfig& cfg) {
            out.Str(cfg.Root);
            out.Str(cfg.Baseline);
            out.Str(cfg.Mode);
            WriteDouble(out, cfg.Q);
            WriteDouble(out, cfg.MinRatio);
            WriteDouble(out, cfg.DiskBudgetBytes);
            out.I64(cfg.Workers);
            out.I64(cfg.AnalysisJobs);
            WriteDouble(out, cfg.RamBytes);
            WriteDouble(out, cfg.ActionMemoryBytes);
            out.U64(cfg.WorkersDerivedFromRam ? 1 : 0);
            out.Str(cfg.MainRef);
            out.StrList(cfg.Targets);
            out.Str(cfg.Gn);
            out.Str(cfg.Ninja);
            out.Str(VerifyBuildName(cfg.VerifyBuild));
        }

        TWorkflowConfig ReadWorkflowConfigBody(TBinReader& in) {
            TWorkflowConfig cfg;
            cfg.Root = std::string(in.Str());
            cfg.Baseline = std::string(in.Str());
            cfg.Mode = std::string(in.Str());
            cfg.Q = ReadDouble(in);
            cfg.MinRatio = ReadDouble(in);
            cfg.DiskBudgetBytes = ReadDouble(in);
            cfg.Workers = int(in.I64());
            cfg.AnalysisJobs = int(in.I64());
            cfg.RamBytes = ReadDouble(in);
            cfg.ActionMemoryBytes = ReadDouble(in);
            cfg.WorkersDerivedFromRam = in.U64() != 0;
            cfg.MainRef = std::string(in.Str());
            cfg.Targets = in.StrList();
            cfg.Gn = std::string(in.Str());
            cfg.Ninja = std::string(in.Str());
            cfg.VerifyBuild = ParseVerifyBuild(std::string(in.Str()));
            return cfg;
        }

    } // namespace

    // =========================================================================
    // Link graph & symbol availability
    // =========================================================================

    TLinkInputs CollectLinkInputs(const TNinjaManifest& manifest,
                                  const std::unordered_map<std::string, std::string>& labels) {
        TLinkInputs result;
        std::set<std::string> libraries;
        std::map<std::string, std::set<std::string>> members;
        std::set<std::string> visiting;

        std::function<const std::set<std::string>&(const std::string&)> owners;
        owners = [&](const std::string& path) -> const std::set<std::string>& {
            if (visiting.count(path)) {
                Fail("cycle in link inputs: " + path);
            }
            auto [it, added] = members.try_emplace(path);
            if (!added) {
                return it->second;
            }
            visiting.insert(path);
            const auto label = labels.find(path);
            if (label != labels.end()) {
                it->second.insert(label->second);
            }
            const int64_t producer = manifest.Producer(path);
            if (producer >= 0 && manifest.Edges()[size_t(producer)].Rule == "alink") {
                const auto& edge = manifest.Edges()[size_t(producer)];
                for (const auto* inputs : {&edge.Inputs, &edge.ImplicitInputs}) {
                    for (const std::string& input : *inputs) {
                        const int64_t source = manifest.Producer(input);
                        if (source < 0) {
                            continue;
                        }
                        const std::string& rule = manifest.Edges()[size_t(source)].Rule;
                        if (IsCompileRule(rule) || rule == "alink") {
                            const auto& nested = owners(input);
                            it->second.insert(nested.begin(), nested.end());
                        }
                    }
                }
            }
            visiting.erase(path);
            return it->second;
        };

        for (const auto& edge : manifest.Edges()) {
            if (!IsLinkRule(edge.Rule)) {
                continue;
            }
            TLinkLine line;
            line.Output = edge.Outputs.at(0);
            std::set<std::string> targets;
            const auto own = labels.find(line.Output);
            if (own != labels.end()) {
                targets.insert(own->second);
            }
            for (const auto* inputs : {&edge.Inputs, &edge.ImplicitInputs}) {
                for (const std::string& input : *inputs) {
                    const int64_t producer = manifest.Producer(input);
                    if (producer < 0) {
                        continue;
                    }
                    const std::string& rule = manifest.Edges()[size_t(producer)].Rule;
                    if (!IsCompileRule(rule) && rule != "alink" && rule != "solink") {
                        continue;
                    }
                    line.Inputs.insert(rule == "solink" ? manifest.Edges()[size_t(producer)].Outputs.at(0) : input);
                    if (rule != "solink") {
                        const auto& nested = owners(input);
                        targets.insert(nested.begin(), nested.end());
                    }
                }
            }
            if (targets.empty()) {
                continue;
            }
            libraries.insert(line.Inputs.begin(), line.Inputs.end());
            for (const std::string& target : targets) {
                result.Targets[target].push_back(int(result.Lines.size()));
            }
            result.Lines.push_back(std::move(line));
        }
        result.Libraries.assign(libraries.begin(), libraries.end());
        return result;
    }

    void LoadLinkProviders(TWorkDir& work, const std::string& buildDir,
                           const std::vector<std::string>& libraries,
                           const std::set<std::string>& symbols, TSymbolProviders& providers) {
        std::set<std::string> fresh;
        for (const std::string& symbol : symbols) {
            if (!providers.count(symbol)) {
                fresh.insert(symbol);
            }
        }
        if (fresh.empty()) {
            return;
        }
        std::vector<std::string> results;
        results.reserve(libraries.size());
        for (const std::string& path : libraries) {
            TWorkDir::TJob job;
            job.Kind = "syms";
            const std::string abs = JoinPath(buildDir, path);
            job.Args = {abs};
            job.Inputs = {abs};
            job.Depfile = false;
            results.push_back(work.Add(job));
        }
        work.Build(results, "symbols of the libraries");
        TSymbolProviders loaded;
        for (const std::string& symbol : fresh) {
            loaded[symbol];
        }
        std::mutex lock;
        ParallelFor(results.size(), AnalysisThreads(work.Jobs()), [&](size_t i) {
            const auto defined = LoadSymbolsResult(results[i]);
            std::vector<const std::string*> hits;
            for (const std::string& symbol : defined) {
                const auto it = fresh.find(symbol);
                if (it != fresh.end()) {
                    hits.push_back(&*it);
                }
            }
            std::lock_guard<std::mutex> guard(lock);
            for (const std::string* symbol : hits) {
                loaded[*symbol].insert(libraries[i]);
            }
        });
        providers.insert(loaded.begin(), loaded.end());
        Log(Format("link providers: %zu libraries indexed for %zu symbols", results.size(), fresh.size()));
    }

    TLinkAvailability CheckLinkRequirements(const std::vector<std::string>& symbols,
                                            const TSymbolProviders& providers,
                                            const std::vector<TLinkLine>& links) {
        std::vector<int> selected(links.size());
        std::iota(selected.begin(), selected.end(), 0);
        return CheckLinkRequirements(symbols, providers, links, selected);
    }

    TLinkAvailability CheckLinkRequirements(const std::vector<std::string>& symbols,
                                            const TSymbolProviders& providers,
                                            const std::vector<TLinkLine>& links,
                                            const std::vector<int>& selected) {
        TLinkAvailability result;
        if (symbols.empty()) {
            return result;
        }
        result.UnknownLinks = selected.empty();
        for (const std::string& symbol : std::set<std::string>(symbols.begin(), symbols.end())) {
            const auto it = providers.find(symbol);
            if (it == providers.end() || it->second.empty()) {
                result.UnknownSymbols.push_back(symbol);
                continue;
            }
            for (int index : selected) {
                const TLinkLine& line = links.at(size_t(index));
                const bool found = std::any_of(it->second.begin(), it->second.end(), [&](const std::string& path) {
                    return line.Inputs.count(path) != 0;
                });
                if (!found) {
                    result.Missing[symbol].push_back(line.Output);
                }
            }
        }
        return result;
    }

    std::string FormatLinkDiagnostic(const TLinkAvailability& availability) {
        std::vector<std::string> reasons;
        if (availability.UnknownLinks) {
            reasons.push_back("unknown final links for an owner member");
        }
        for (const std::string& symbol : availability.UnknownSymbols) {
            reasons.push_back("unknown provider for " + symbol);
        }
        for (const auto& [symbol, links] : availability.Missing) {
            for (const std::string& link : links) {
                reasons.push_back(link + ": no known provider for " + symbol);
            }
        }
        return Join(reasons, "; ");
    }

    bool IsStrictRuntimeSymbol(std::string_view s) {
        static const std::set<std::string_view> STD_NEW_DELETE = {
            "_Znwm", "_Znwj", "_Znam", "_Znaj",
            "_ZnwmSt11align_val_t", "_ZnwjSt11align_val_t",
            "_ZnamSt11align_val_t", "_ZnajSt11align_val_t",
            "_ZnwmRKSt9nothrow_t", "_ZnwjRKSt9nothrow_t",
            "_ZnamRKSt9nothrow_t", "_ZnajRKSt9nothrow_t",
            "_ZnwmSt11align_val_tRKSt9nothrow_t", "_ZnwjSt11align_val_tRKSt9nothrow_t",
            "_ZnamSt11align_val_tRKSt9nothrow_t", "_ZnajSt11align_val_tRKSt9nothrow_t",
            "_ZdlPv", "_ZdaPv",
            "_ZdlPvm", "_ZdlPvj", "_ZdaPvm", "_ZdaPvj",
            "_ZdlPvSt11align_val_t", "_ZdaPvSt11align_val_t",
            "_ZdlPvmSt11align_val_t", "_ZdlPvjSt11align_val_t",
            "_ZdaPvmSt11align_val_t", "_ZdaPvjSt11align_val_t",
            "_ZdlPvRKSt9nothrow_t", "_ZdaPvRKSt9nothrow_t",
            "_ZdlPvSt11align_val_tRKSt9nothrow_t", "_ZdaPvSt11align_val_tRKSt9nothrow_t",
        };
        if (STD_NEW_DELETE.count(s)) {
            return true;
        }
        if (StartsWith(s, "__cxa_") || StartsWith(s, "_Unwind_") ||
            s == "__gxx_personality_v0" || s == "__clang_call_terminate") {
            return true;
        }
        if (!StartsWith(s, "_Z")) {
            return false;
        }
        s.remove_prefix(2);
        for (const char* kind : {"TV", "TT", "TI", "TS", "GV", "GR"}) {
            if (StartsWith(s, kind)) {
                s.remove_prefix(2);
                break;
            }
        }
        if (StartsWith(s, "Th") || StartsWith(s, "Tv") || StartsWith(s, "Tc")) {
            const int offsets = s[1] == 'h' ? 1 : 2;
            s.remove_prefix(2);
            for (int k = 0; k < offsets; ++k) {
                const size_t end = s.find('_');
                if (end == std::string_view::npos) {
                    return false;
                }
                s.remove_prefix(end + 1);
            }
        }
        if (StartsWith(s, "L")) {
            s.remove_prefix(1);
        }

        enum class EScope { RootStd, NestedOther, NestedStd, TemplateArgs, LocalFunc };
        std::vector<EScope> scopes;
        if (StartsWith(s, "Z")) {
            scopes.push_back(EScope::LocalFunc);
            s.remove_prefix(1);
        }
        if (StartsWith(s, "N")) {
            scopes.push_back(EScope::RootStd);
            s.remove_prefix(1);
            while (!s.empty() && std::string_view("KVrRO").find(s.front()) != std::string_view::npos) {
                s.remove_prefix(1);
            }
        }
        if (!(StartsWith(s, "St3__1") || StartsWith(s, "10__cxxabiv1") ||
              (StartsWith(s, "St") && s.size() > 2 && std::isdigit(static_cast<unsigned char>(s[2]))))) {
            return false;
        }

        size_t i = 0;
        const size_t n = s.size();
        bool inStdChain = true;
        while (i < n) {
            const char c = s[i];
            if (c == 'I') {
                scopes.push_back(EScope::TemplateArgs);
                inStdChain = false;
                ++i;
                continue;
            }
            if (c == 'N') {
                scopes.push_back(EScope::NestedOther);
                inStdChain = false;
                ++i;
                while (i < n && std::string_view("KVrRO").find(s[i]) != std::string_view::npos) {
                    ++i;
                }
                continue;
            }
            if (c == 'Z') {
                scopes.push_back(EScope::LocalFunc);
                inStdChain = false;
                ++i;
                continue;
            }
            if (c == 'E') {
                const EScope closed = scopes.empty() ? EScope::NestedOther : scopes.back();
                if (!scopes.empty()) {
                    scopes.pop_back();
                }
                inStdChain = (closed == EScope::LocalFunc) ||
                    (!scopes.empty() && (scopes.back() == EScope::RootStd || scopes.back() == EScope::NestedStd));
                ++i;
                continue;
            }
            if ((c == 'C' || c == 'D') && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '3') {
                i += 2;
                continue;
            }
            if (c == 'T') {
                const size_t end = s.find('_', i + 1);
                if (end != std::string_view::npos) {
                    bool digits = true;
                    for (size_t k = i + 1; k < end; ++k) {
                        digits &= std::isdigit(static_cast<unsigned char>(s[k])) != 0;
                    }
                    if (digits) {
                        i = end + 1;
                        inStdChain = false;
                        continue;
                    }
                }
            }
            const bool insideTemplate = std::find(scopes.begin(), scopes.end(), EScope::TemplateArgs) != scopes.end();
            if (insideTemplate && c == 'L' && i + 1 < n && !std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
                const size_t end = s.find('E', i + 2);
                if (end == std::string_view::npos) {
                    return false;
                }
                i = end + 1;
                inStdChain = false;
                continue;
            }
            if (c == 'A' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
                const size_t end = s.find('_', i + 1);
                if (end != std::string_view::npos) {
                    bool digits = true;
                    for (size_t k = i + 1; k < end; ++k) {
                        digits &= std::isdigit(static_cast<unsigned char>(s[k])) != 0;
                    }
                    if (digits) {
                        i = end + 1;
                        inStdChain = false;
                        continue;
                    }
                }
            }
            if (c == 'S') {
                if (StartsWith(s.substr(i), "St")) {
                    inStdChain = true;
                    if (!scopes.empty() && scopes.back() == EScope::NestedOther) {
                        scopes.back() = EScope::NestedStd;
                    }
                    i += 2;
                    continue;
                }
                const size_t end = s.find('_', i + 1);
                if (end != std::string_view::npos) {
                    bool seq = true;
                    for (size_t k = i + 1; k < end; ++k) {
                        const unsigned char ch = static_cast<unsigned char>(s[k]);
                        seq &= (std::isdigit(ch) != 0) || (ch >= 'A' && ch <= 'Z');
                    }
                    if (seq) {
                        inStdChain = true;
                        if (!scopes.empty() && scopes.back() == EScope::NestedOther) {
                            scopes.back() = EScope::NestedStd;
                        }
                        i = end + 1;
                        continue;
                    }
                }
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                size_t j = i;
                size_t len = 0;
                while (j < n && std::isdigit(static_cast<unsigned char>(s[j]))) {
                    len = len * 10 + size_t(s[j] - '0');
                    if (len > n) {
                        return false;
                    }
                    ++j;
                }
                if (len == 0 || j + len > n) {
                    return false;
                }
                const std::string_view ident = s.substr(j, len);
                if (!inStdChain && ident != "__cxxabiv1") {
                    return false;
                }
                inStdChain = !scopes.empty() &&
                    (scopes.back() == EScope::RootStd || scopes.back() == EScope::NestedStd);
                i = j + len;
                continue;
            }
            inStdChain = false;
            ++i;
        }
        return true;
    }

    bool IsCxxRuntimeOnlySymbols(const std::vector<std::string>& unresolved) {
        for (const std::string& sym : unresolved) {
            if (!IsStrictRuntimeSymbol(sym)) {
                return false;
            }
        }
        return true;
    }

    // =========================================================================
    // Compilation database & flag canonicalization
    // =========================================================================

    void NormalizeCompileCommandInput(TCompileCommand& cmd, const std::string& buildDir) {
        for (size_t i = 1; i < cmd.Argv.size(); ++i) {
            if (cmd.Argv[i - 1] == "-c" && cmd.Argv[i] != cmd.Source &&
                NormPath(JoinPath(buildDir, cmd.Argv[i])) == NormPath(JoinPath(buildDir, cmd.Source))) {
                cmd.Argv[i] = cmd.Source;
            }
        }
    }

    std::map<std::string, TCompileCommand> ReadCompileCommands(const std::string& buildDir) {
        const std::string text = ReadFile(JoinPath(buildDir, "compile_commands.json"));
        rapidjson::Document doc;
        doc.Parse(text.data(), text.size());
        if (doc.HasParseError() || !doc.IsArray()) {
            Fail("invalid compile_commands.json in " + buildDir);
        }
        const std::string realBuild = RealPath(buildDir);
        std::map<std::string, TCompileCommand> result;
        for (const auto& entry : doc.GetArray()) {
            if (!entry.IsObject() || !entry.HasMember("file") || !entry["file"].IsString() ||
                !entry.HasMember("directory") || !entry["directory"].IsString()) {
                Fail("invalid entry in compile_commands.json");
            }
            if (RealPath(entry["directory"].GetString()) != realBuild) {
                Fail("compile_commands.json entry uses unexpected working directory");
            }
            TCompileCommand cmd;
            cmd.Source = std::string(entry["file"].GetString(), entry["file"].GetStringLength());
            if (entry.HasMember("arguments") && entry["arguments"].IsArray()) {
                for (const auto& word : entry["arguments"].GetArray()) {
                    if (!word.IsString()) {
                        Fail("invalid compiler argument in compile_commands.json");
                    }
                    cmd.Argv.emplace_back(word.GetString(), word.GetStringLength());
                }
            } else if (entry.HasMember("command") && entry["command"].IsString()) {
                cmd.Argv = ShellSplit(std::string_view(entry["command"].GetString(), entry["command"].GetStringLength()));
            } else {
                Fail("compile_commands.json entry has no command or arguments");
            }
            NormalizeCompileCommandInput(cmd, buildDir);
            std::string output;
            for (size_t i = 0; i + 1 < cmd.Argv.size(); ++i) {
                if (cmd.Argv[i] == "-o") {
                    output = cmd.Argv[i + 1];
                }
            }
            if (output.empty()) {
                Fail("compiler output (-o) missing in compile_commands.json");
            }
            if (IsAbs(output)) {
                output = RelPath(output, buildDir);
            }
            result.emplace(NormPath(output), std::move(cmd));
        }
        return result;
    }

    std::vector<std::string> StripCompileFlags(const std::vector<std::string>& argv, const std::string& source) {
        std::vector<std::string> flags;
        flags.reserve(argv.size());
        for (size_t i = 0; i < argv.size(); ++i) {
            const std::string& a = argv[i];
            if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ" || a == "-x") {
                if (++i >= argv.size()) {
                    Fail("incomplete compiler option: " + a);
                }
            } else if (a != source && a != "-c" && a != "-MD" && a != "-MMD" && !StartsWith(a, "-ftime-trace")) {
                if (a == "-include-pch" || a == "-emit-pch" || StartsWith(a, "@")) {
                    Fail("baseline compiler command contains a PCH or response file: " + a);
                }
                flags.push_back(a);
            }
        }
        if (flags.empty() || (!StartsWith(BaseName(flags[0]), "clang++") && BaseName(flags[0]) != "clang")) {
            Fail("direct clang/clang++ compile command required");
        }
        return flags;
    }

    std::vector<std::string> CanonicalizeFlags(std::vector<std::string> flags,
                                               const std::string& buildDir,
                                               const std::string& root) {
        const TPaths paths(buildDir, root, "");
        const std::string relRoot = paths.SourceRoot.empty() ? std::string() : RelPath(paths.SourceRoot, paths.BuildDir);
        const std::string rawRel = root.empty() ? std::string() : RelPath(root, buildDir);
        auto replaceRelRoot = [](std::string& text, const std::string& rel) {
            if (rel.empty() || rel == ".") {
                return;
            }
            std::string out;
            size_t pos = 0;
            for (;;) {
                const size_t at = text.find(rel, pos);
                if (at == std::string::npos) {
                    out.append(text, pos, std::string::npos);
                    break;
                }
                const size_t end = at + rel.size();
                const bool okStart = (at == 0) || text[at - 1] == '=' || text[at - 1] == ':';
                const std::string_view rest = std::string_view(text).substr(end);
                const bool dot = rest == "/." || rest == "/.." || StartsWith(rest, "/./") || StartsWith(rest, "/../") ||
                    StartsWith(rest, "/.=") || StartsWith(rest, "/..=") || StartsWith(rest, "/.:") || StartsWith(rest, "/..:");
                const bool okEnd = (end == text.size()) || text[end] == '=' || text[end] == ':' || (text[end] == '/' && !dot);
                if (okStart && okEnd) {
                    out.append(text, pos, at - pos);
                    out += "<root>";
                    pos = end;
                } else {
                    out.append(text, pos, at - pos + 1);
                    pos = at + 1;
                }
            }
            text = std::move(out);
        };

        bool pathArg = false;
        for (std::string& flag : flags) {
            if (flag == "-Xclang") {
                continue;
            }
            if (pathArg) {
                flag = paths.Normalize(flag);
                pathArg = false;
                continue;
            }
            if (flag == "-I" || flag == "-isystem" || flag == "-iquote" || flag == "-idirafter" ||
                flag == "-include" || flag == "-isysroot") {
                pathArg = true;
            } else if (StartsWith(flag, "-I") && flag.size() > 2) {
                flag = "-I" + paths.Normalize(flag.substr(2));
            } else {
                ReplaceAll(flag, paths.BuildDir, "<out>");
                ReplaceAll(flag, buildDir, "<out>");
                replaceRelRoot(flag, relRoot);
                if (rawRel != relRoot) {
                    replaceRelRoot(flag, rawRel);
                }
                ReplaceAll(flag, paths.SourceRoot, "<root>");
                ReplaceAll(flag, root, "<root>");
            }
        }
        return flags;
    }

    std::vector<std::string> ExtractPchOnlyBuildFlags(const std::string& root) {
        std::vector<std::string> argv;
        std::vector<std::string> out;
        if (!TryPchOnlyArgv(root, &argv)) {
            return out;
        }
        for (size_t i = 0; i < argv.size(); ++i) {
            const std::string& flag = argv[i];
            if (flag == "-x" || flag == "-isysroot") {
                ++i;
            } else if (flag == "-relocatable-pch") {
                continue;
            } else if (flag == "-Xclang" && i + 1 < argv.size() && argv[i + 1] == "-fmodule-file-home-is-cwd") {
                ++i;
            } else {
                out.push_back(flag);
            }
        }
        return out;
    }

    // =========================================================================
    // Unit helpers
    // =========================================================================

    int HeaderUnitLayer(const TAnalysisModel& model, int unit) {
        const THeaderUnit& u = model.Units.at(size_t(unit));
        if (u.Headers.empty()) {
            return u.Generated ? 4 : 3;
        }
        int rank = 4;
        for (const std::string& name : u.Headers) {
            const int r = (StartsWith(name, LIBCXX) || StartsWith(name, "<sysroot>") || IsAbs(name)) ? 0
                : StartsWith(name, "contrib/")                                                        ? 1
                : (StartsWith(name, "util/") || StartsWith(name, "library/"))                         ? 2
                : StartsWith(name, "<out>")                                                           ? 4
                                                                                                      : 3;
            rank = std::min(rank, r);
        }
        return rank;
    }

    std::vector<int> LayeredRootOrder(const TAnalysisModel& model, const std::vector<int>& roots) {
        std::vector<int> ordered = roots;
        std::stable_sort(ordered.begin(), ordered.end(), [&](int a, int b) {
            return HeaderUnitLayer(model, a) < HeaderUnitLayer(model, b);
        });
        return ordered;
    }

    std::vector<int> ComputeUnitClosure(const TAnalysisModel& model, const std::vector<int>& roots) {
        std::vector<char> seen(model.Units.size(), 0);
        std::vector<int> stack = roots;
        std::vector<int> out;
        while (!stack.empty()) {
            const int u = stack.back();
            stack.pop_back();
            if (u < 0 || size_t(u) >= model.Units.size()) {
                Fail("invalid header unit ID in closure");
            }
            if (seen[size_t(u)]) {
                continue;
            }
            seen[size_t(u)] = 1;
            out.push_back(u);
            const auto& children = model.Units[size_t(u)].Children;
            stack.insert(stack.end(), children.begin(), children.end());
        }
        SortUnique(out);
        return out;
    }

    std::string RenderPchHeader(const TAnalysisModel& model, const std::vector<int>& roots) {
        std::string header = "#pragma once\n";
        for (int root : LayeredRootOrder(model, roots)) {
            header += "#include " + model.Units.at(size_t(root)).Include + "\n";
        }
        return header;
    }

    // =========================================================================
    // Model Extraction & Persistence
    // =========================================================================

    void SaveAnalysisModel(const std::string& path, const TAnalysisModel& model) {
        TBinWriter body;
        WriteModelBody(body, model);
        TBinWriter env;
        env.Str(ANALYSIS_SCHEMA_VERSION);
        env.Str(HashHex(body.Data()));
        env.Str(body.Data());
        WriteFile(path, env.Data());
    }

    TAnalysisModel LoadAnalysisModel(const std::string& path) {
        const std::string raw = ReadFile(path);
        TBinReader env(raw);
        if (env.Str() != ANALYSIS_SCHEMA_VERSION) {
            Fail("analysis schema changed; recomputing baseline extraction");
        }
        const std::string expectedHash(env.Str());
        const std::string_view payload = env.Str();
        if (!env.AtEnd() || HashHex(payload) != expectedHash) {
            Fail("analysis checkpoint checksum mismatch");
        }
        TBinReader in(payload);
        TAnalysisModel model = ReadModelBody(in);
        if (!in.AtEnd()) {
            Fail("analysis checkpoint has trailing bytes");
        }
        return model;
    }

    void ValidateAnalysisInputs(const TAnalysisModel& model, int workers) {
        std::vector<std::string> paths;
        paths.reserve(model.InputDigests.size());
        for (const auto& [path, digest] : model.InputDigests) {
            if (!Exists(path)) {
                Fail("input removed since analysis: " + path);
            }
            paths.push_back(path);
        }
        const std::vector<std::string> digests = DigestFiles(paths, workers);
        for (size_t i = 0; i < paths.size(); ++i) {
            if (digests[i] != model.InputDigests[i].second) {
                Fail("input modified since analysis: " + paths[i]);
            }
        }
    }

    bool TryLoadValidAnalysis(const std::string& planDir, const TWorkflowConfig& cfg, TAnalysisModel* out) {
        const std::string path = JoinPath(planDir, "analysis.bin");
        if (!Exists(path)) {
            return false;
        }
        try {
            TAnalysisModel loaded = LoadAnalysisModel(path);
            if (loaded.SchemaVersion != ANALYSIS_SCHEMA_VERSION ||
                loaded.Root != cfg.Root ||
                loaded.Baseline != cfg.Baseline ||
                loaded.WorkloadTargets != cfg.Targets) {
                return false;
            }
            if (loaded.Scenarios == 0 && cfg.Q < 1.0) {
                Fail("no git history commits in window; use --mode=global or --q 1");
            }
            if (loaded.TreeHash != TreeIdentity(cfg.Root, true)) {
                Log("analysis cache invalidated: source tree changed");
                return false;
            }
            if (loaded.BaselineHash != ComputeBaselineStamp(loaded)) {
                Log("analysis cache invalidated: baseline build artifacts changed");
                return false;
            }
            ValidateAnalysisInputs(loaded, cfg.Workers);
            Log(Format("reusing validated baseline analysis: %zu targets, %zu C++ files, %zu header units",
                       loaded.Targets.size(), loaded.Files.size(), loaded.Units.size()));
            *out = std::move(loaded);
            return true;
        } catch (const TError& err) {
            if (std::string_view(err.what()).find("no git history commits") != std::string_view::npos) {
                throw;
            }
            Log("recomputing baseline analysis: " + std::string(err.what()));
            return false;
        }
    }

    TAnalysisModel ExtractAnalysisModel(const TWorkflowConfig& cfg, const std::string& cacheDir, const std::string& planDir) {
        TOptions options;
        options.Baseline = cfg.Baseline;
        options.Targets = cfg.Targets;
        options.Q = cfg.Q;
        options.Jobs = cfg.AnalysisJobs;
        options.MainRef = cfg.MainRef;
        options.WorkDir = cacheDir;
        options.Ninja = cfg.Ninja;

        TTools tools;
        tools.Gn = cfg.Gn;
        tools.Ninja = cfg.Ninja;

        TWorkDir work(cacheDir, cfg.Ninja, cfg.AnalysisJobs);
        TFacts facts;
        facts.Root = cfg.Root;

        if (!CheckBaseline(options, tools, work, facts)) {
            Fail("baseline check failed");
        }
        if (!ReadTraces(options, work, facts) || !ReadHistory(options, facts)) {
            Fail("trace/history extraction failed");
        }
        FlagClasses(options, facts);
        if (!HeaderFacts(options, work, facts)) {
            Fail("header standalone probing failed");
        }

        TAnalysisModel m;
        m.SchemaVersion = ANALYSIS_SCHEMA_VERSION;
        m.Root = facts.Root;
        m.Baseline = facts.BuildDir;
        m.ArgsGn = ReadFile(JoinPath(m.Baseline, "args.gn"));
        m.GnPath = ResolveProgram(cfg.Gn);
        m.NinjaPath = ResolveProgram(cfg.Ninja);
        m.CompilerPath = ResolveProgram(facts.Compiler, m.Baseline, false);
        m.GnSwitches = GnGenerationSwitches(*facts.Manifest);
        for (std::string& sw : m.GnSwitches) {
            if (StartsWith(sw, "--script-executable=")) {
                sw = "--script-executable=" + ResolveProgram(sw.substr(20));
            }
        }
        m.PchOnlyFlags = ExtractPchOnlyBuildFlags(m.Root);
        m.WorkloadTargets = cfg.Targets;
        m.Scenarios = int(facts.CommitFiles.size());
        if (m.Scenarios == 0 && cfg.Q < 1.0) {
            Fail("no git history commits found; use --mode=global or --q 1 for a cold-only run");
        }

        // 1. Condense headers into strongly connected components (Tarjan SCC).
        const int32_t headerCount = int32_t(facts.HeaderNames.size());
        std::vector<uint64_t> rawEdges(facts.HeaderEdges.begin(), facts.HeaderEdges.end());
        std::sort(rawEdges.begin(), rawEdges.end());
        std::vector<std::vector<int32_t>> hdrChildren(headerCount);
        for (uint64_t e : rawEdges) {
            hdrChildren[size_t(e >> 32)].push_back(int32_t(e & 0xFFFFFFFFu));
        }
        std::vector<int32_t> compOfHeader;
        const int32_t unitCount = CondenseScc(hdrChildren, &compOfHeader);
        std::vector<std::vector<int32_t>> unitHeaders(unitCount);
        for (int32_t h = 0; h < headerCount; ++h) {
            unitHeaders[size_t(compOfHeader[size_t(h)])].push_back(h);
        }
        std::vector<std::set<int>> unitChildSets(unitCount);
        for (uint64_t e : rawEdges) {
            const int a = compOfHeader[size_t(e >> 32)];
            const int b = compOfHeader[size_t(e & 0xFFFFFFFFu)];
            if (a != b) {
                unitChildSets[size_t(a)].insert(b);
            }
        }

        std::unordered_map<std::string, std::vector<int>> commitsByPath;
        for (int k = 0; k < m.Scenarios; ++k) {
            for (const std::string& p : facts.CommitFiles[size_t(k)]) {
                commitsByPath[p].push_back(k);
            }
        }

        std::set<std::string> fingerprint;
        fingerprint.insert(JoinPath(m.Baseline, "args.gn"));
        fingerprint.insert(ResolveProgram(facts.Compiler, m.Baseline, true));
        fingerprint.insert(m.GnPath);
        fingerprint.insert(m.NinjaPath);
        for (const std::string& sw : m.GnSwitches) {
            if (StartsWith(sw, "--script-executable=")) {
                fingerprint.insert(sw.substr(20));
            }
        }
        const std::string gnDepfile = JoinPath(m.Baseline, "build.ninja.d");
        if (Exists(gnDepfile)) {
            for (const std::string& dep : ReadDepfile(ReadFile(gnDepfile))) {
                const std::string real = NormPath(JoinPath(m.Baseline, dep));
                if (Exists(real) && !IsDir(real)) {
                    fingerprint.insert(real);
                }
            }
        }

        const auto& edges = facts.Manifest->Edges();
        const int regenEdge = int(facts.Manifest->Producer("build.ninja"));
        if (regenEdge >= 0) {
            for (const auto* list : {&edges[size_t(regenEdge)].Inputs,
                                     &edges[size_t(regenEdge)].ImplicitInputs,
                                     &edges[size_t(regenEdge)].OrderOnly}) {
                for (const std::string& p : *list) {
                    const std::string real = NormPath(JoinPath(m.Baseline, p));
                    if (Exists(real) && !IsDir(real)) {
                        fingerprint.insert(real);
                    }
                }
            }
        }

        // 2. Reachable Ninja edges for the requested workload targets.
        auto headerProducerEdge = [&](const std::string& normalized) {
            return int(facts.Manifest->Producer(RelPath(facts.Paths.Real(normalized), m.Baseline)));
        };
        std::vector<char> wantedEdge(edges.size(), 0);
        {
            std::vector<int> stack;
            for (const std::string& target : cfg.Targets) {
                const int64_t prod = facts.Manifest->Producer(target);
                if (prod < 0) {
                    Fail("unknown Ninja target: " + target);
                }
                stack.push_back(int(prod));
            }
            for (const std::string& hdr : facts.HeaderNames) {
                const int prod = headerProducerEdge(hdr);
                if (prod >= 0) {
                    stack.push_back(prod);
                }
            }
            while (!stack.empty()) {
                const int e = stack.back();
                stack.pop_back();
                if (wantedEdge[size_t(e)]) {
                    continue;
                }
                wantedEdge[size_t(e)] = 1;
                for (const auto* list : {&edges[size_t(e)].Inputs,
                                         &edges[size_t(e)].ImplicitInputs,
                                         &edges[size_t(e)].OrderOnly,
                                         &edges[size_t(e)].Validations}) {
                    for (const std::string& p : *list) {
                        const int dep = int(facts.Manifest->Producer(p));
                        if (dep >= 0 && edges[size_t(dep)].Outputs[0] != "build.ninja") {
                            stack.push_back(dep);
                        }
                    }
                }
            }
        }

        for (size_t e = 0; e < edges.size(); ++e) {
            if (!wantedEdge[e]) {
                continue;
            }
            for (const auto* list : {&edges[e].Inputs, &edges[e].ImplicitInputs}) {
                for (const std::string& p : *list) {
                    if (facts.Manifest->Producer(p) < 0) {
                        const std::string real = NormPath(JoinPath(m.Baseline, p));
                        if (Exists(real) && !IsDir(real)) {
                            fingerprint.insert(real);
                        }
                    }
                }
            }
        }

        // 3. Populate header SCC units and reverse per-traced-file Own map.
        m.Units.resize(size_t(unitCount));
        std::vector<std::vector<std::pair<int, double>>> ownByTracedFile(facts.Objs.size());
        for (int32_t u = 0; u < unitCount; ++u) {
            THeaderUnit& unit = m.Units[size_t(u)];
            unit.Children.assign(unitChildSets[size_t(u)].begin(), unitChildSets[size_t(u)].end());
            int64_t medianParseUs = 0;
            std::map<int32_t, int64_t> mergedOwnUs;
            for (int32_t h : unitHeaders[size_t(u)]) {
                const std::string& name = facts.HeaderNames[size_t(h)];
                unit.Headers.push_back(name);
                const std::string realPath = facts.Paths.Real(name);
                fingerprint.insert(realPath);
                const int64_t sz = FileSize(realPath);
                if (sz > 0) {
                    unit.SourceBytes += double(sz);
                }
                medianParseUs += MedianInt64(facts.HeaderOwn[size_t(h)]);
                unit.Generated |= StartsWith(name, "<out>/") || (headerProducerEdge(name) >= 0);
                unit.SafeLink &= facts.NeedsOfHeader.empty() || IsCxxRuntimeOnlySymbols(facts.NeedsOfHeader[size_t(h)]);
                if (unit.Include.empty() && (facts.SelfContained.empty() || facts.SelfContained[size_t(h)] != 0)) {
                    unit.Include = Spelling(facts, name);
                }
                const auto chIt = commitsByPath.find(name);
                if (chIt != commitsByPath.end()) {
                    unit.Changes.insert(unit.Changes.end(), chIt->second.begin(), chIt->second.end());
                }
                for (size_t i = 0; i < facts.HeaderFiles[size_t(h)].size(); ++i) {
                    mergedOwnUs[facts.HeaderFiles[size_t(h)][i]] += facts.HeaderOwn[size_t(h)][i];
                }
            }
            unit.ParseSec = double(medianParseUs) / 1e6;
            SortUnique(unit.Changes);
            for (const auto& [tracedIdx, ownUs] : mergedOwnUs) {
                if (ownUs > 0) {
                    ownByTracedFile[size_t(tracedIdx)].emplace_back(int(u), double(ownUs) / 1e6);
                }
            }
        }

        // 4. Collect ALL C++ TUs (traced and untraced) and group by GN global_pch_owner.
        const auto compileDb = ReadCompileCommands(m.Baseline);
        TNinjaDeps ninjaDeps;
        const std::string ninjaDepsPath = JoinPath(m.Baseline, ".ninja_deps");
        if (Exists(ninjaDepsPath)) {
            ninjaDeps = ReadNinjaDeps(ninjaDepsPath);
        }

        const std::set<std::string> platformPrereqs = Reachable(
            facts, std::vector<std::string>(std::begin(PCH_PLATFORM_DEPS), std::end(PCH_PLATFORM_DEPS)));

        auto resolveOwner = [&](const std::string& compileLabel, bool* hasHook) -> std::string {
            const auto hook = facts.GlobalPchOwner.find(compileLabel);
            if (hook == facts.GlobalPchOwner.end()) {
                *hasHook = false;
                return compileLabel;
            }
            *hasHook = true;
            return hook->second;
        };

        // Identify all owners touched by wanted cxx edges, and detect partial/unbuilt owners.
        std::set<std::string> activeOwners;
        std::set<std::string> noHookOwners;
        std::set<std::string> incompleteOwners;
        std::set<std::string> platformDepOwners;

        for (size_t e = 0; e < edges.size(); ++e) {
            if (edges[e].Rule != "cxx" || edges[e].Outputs.empty() || !wantedEdge[e]) {
                continue;
            }
            const std::string& obj = edges[e].Outputs[0];
            const auto lblIt = facts.LabelOfOutput.find(obj);
            if (lblIt == facts.LabelOfOutput.end()) {
                Fail("no GN target label owns C++ object: " + obj);
            }
            bool hasHook = false;
            const std::string owner = resolveOwner(lblIt->second, &hasHook);
            activeOwners.insert(owner);
            if (!hasHook) {
                noHookOwners.insert(owner);
            }
            if (platformPrereqs.count(lblIt->second) || platformPrereqs.count(owner)) {
                platformDepOwners.insert(owner);
            }
        }

        // Second pass over all cxx edges in the manifest: include every TU of any active owner
        // so that GN global PCH assignment never touches an unchecked TU in compile_commands.json.
        std::map<std::vector<std::string>, int> classIdByFlags;
        std::map<std::string, std::vector<int>> filesByOwner;

        for (size_t e = 0; e < edges.size(); ++e) {
            if (edges[e].Rule != "cxx" || edges[e].Outputs.empty()) {
                continue;
            }
            const std::string& obj = edges[e].Outputs[0];
            const auto lblIt = facts.LabelOfOutput.find(obj);
            if (lblIt == facts.LabelOfOutput.end()) {
                continue;
            }
            bool hasHook = false;
            const std::string owner = resolveOwner(lblIt->second, &hasHook);
            if (!activeOwners.count(owner)) {
                continue;
            }
            if (!hasHook) {
                noHookOwners.insert(owner);
            }
            if (platformPrereqs.count(lblIt->second) || platformPrereqs.count(owner)) {
                platformDepOwners.insert(owner);
            }
            if (!wantedEdge[e] || !Exists(JoinPath(m.Baseline, obj))) {
                incompleteOwners.insert(owner);
            }

            const auto cmdIt = compileDb.find(obj);
            if (cmdIt == compileDb.end()) {
                Fail("compile_commands.json is missing C++ object: " + obj);
            }
            const auto canonFlags = CanonicalizeFlags(
                StripCompileFlags(cmdIt->second.Argv, cmdIt->second.Source), m.Baseline, m.Root);
            auto [clsIt, added] = classIdByFlags.emplace(canonFlags, int(classIdByFlags.size()));
            if (added) {
                m.ClassFlags.push_back(canonFlags);
            }

            TTuFile tu;
            tu.Object = obj;
            tu.RawSource = cmdIt->second.Source;
            tu.Source = facts.Paths.Normalize(cmdIt->second.Source);
            tu.Class = clsIt->second;

            const std::string realSrc = facts.Paths.Real(tu.Source);
            if (Exists(realSrc) && !IsDir(realSrc)) {
                fingerprint.insert(realSrc);
            }

            const auto tracedIt = facts.FileOf.find(obj);
            if (tracedIt != facts.FileOf.end()) {
                const size_t f = size_t(tracedIt->second);
                tu.Traced = true;
                tu.CompileSec = std::max(0.001, double(facts.CompileUs[f]) / 1e6);
                tu.FrontendSec = std::max(0.0, double(facts.FrontendUs[f]) / 1e6);
                tu.HeaderSec = std::max(0.0, double(facts.HeaderUs[f]) / 1e6);
                tu.Own = std::move(ownByTracedFile[f]);
                for (int32_t depId : facts.DepsOfFile[f]) {
                    const std::string& depNorm = facts.DepNames[size_t(depId)];
                    const auto chIt = commitsByPath.find(depNorm);
                    if (chIt != commitsByPath.end()) {
                        tu.Changes.insert(tu.Changes.end(), chIt->second.begin(), chIt->second.end());
                    }
                    const std::string realDep = facts.Paths.Real(depNorm);
                    if (Exists(realDep) && !IsDir(realDep)) {
                        fingerprint.insert(realDep);
                    }
                }
            } else {
                tu.Traced = false;
                const auto durIt = facts.DurationMs.find(obj);
                tu.CompileSec = durIt != facts.DurationMs.end() ? std::max(0.001, double(durIt->second) / 1000.0) : 0.1;
                const auto ndIt = ninjaDeps.Deps.find(obj);
                if (ndIt != ninjaDeps.Deps.end()) {
                    for (int32_t pathId : ndIt->second) {
                        const std::string depNorm = facts.Paths.Normalize(ninjaDeps.Paths[size_t(pathId)]);
                        const auto chIt = commitsByPath.find(depNorm);
                        if (chIt != commitsByPath.end()) {
                            tu.Changes.insert(tu.Changes.end(), chIt->second.begin(), chIt->second.end());
                        }
                        const std::string realDep = facts.Paths.Real(depNorm);
                        if (Exists(realDep) && !IsDir(realDep)) {
                            fingerprint.insert(realDep);
                        }
                    }
                }
            }
            for (const auto* list : {&edges[e].Inputs, &edges[e].ImplicitInputs}) {
                for (const std::string& p : *list) {
                    const auto chIt = commitsByPath.find(facts.Paths.Normalize(p));
                    if (chIt != commitsByPath.end()) {
                        tu.Changes.insert(tu.Changes.end(), chIt->second.begin(), chIt->second.end());
                    }
                }
            }
            SortUnique(tu.Changes);

            const int fileIdx = int(m.Files.size());
            m.Files.push_back(std::move(tu));
            filesByOwner[owner].push_back(fileIdx);
        }

        std::unordered_map<std::string, int> ownerIdByLabel;
        m.Targets.reserve(filesByOwner.size());
        for (auto& [label, fileIndices] : filesByOwner) {
            const int targetId = int(m.Targets.size());
            ownerIdByLabel.emplace(label, targetId);
            int commonClass = m.Files[size_t(fileIndices.front())].Class;
            for (int fIdx : fileIndices) {
                m.Files[size_t(fIdx)].Target = targetId;
                if (m.Files[size_t(fIdx)].Class != commonClass) {
                    commonClass = -1;
                }
            }
            TTargetOwner target;
            target.Label = label;
            target.Class = commonClass;
            target.Files = std::move(fileIndices);
            if (noHookOwners.count(label)) {
                target.PchEligible = false;
                target.IneligibleReason = "no-global-pch-hook";
            } else if (incompleteOwners.count(label)) {
                target.PchEligible = false;
                target.IneligibleReason = "incomplete-target-in-baseline";
            } else if (platformDepOwners.count(label)) {
                target.PchEligible = false;
                target.IneligibleReason = "pch-platform-dep";
            } else if (commonClass < 0) {
                target.PchEligible = false;
                target.IneligibleReason = "mixed-compiler-classes";
            }
            m.Targets.push_back(std::move(target));
        }

        // 5. Link graph for object-level symbol availability checks.
        std::unordered_map<std::string, std::string> objectIdentity;
        objectIdentity.reserve(m.Files.size());
        for (const TTuFile& f : m.Files) {
            objectIdentity.emplace(f.Object, f.Object);
        }
        m.LinkInputs = CollectLinkInputs(*facts.Manifest, objectIdentity);

        // 6. Compute exact ForbiddenTargets per generated unit (both GN deps and Ninja DAG).
        std::map<std::string, std::string> publicCodegen;
        for (const auto& [lbl, deps] : facts.DepsOf) {
            if (!EndsWith(lbl, "_codegen")) {
                continue;
            }
            const std::string_view prefix = std::string_view(lbl).substr(0, lbl.find(':'));
            for (const std::string& dep : deps) {
                if (std::string_view(dep).substr(0, dep.find(':')) == prefix) {
                    auto [it, added] = publicCodegen.emplace(dep, lbl);
                    if (!added && lbl < it->second) {
                        it->second = lbl;
                    }
                }
            }
        }

        // Map each Ninja edge to the TTargetOwner IDs whose outputs it produces.
        std::vector<std::vector<int>> edgeTargetOwners(edges.size());
        for (const TTuFile& f : m.Files) {
            const int64_t prod = facts.Manifest->Producer(f.Object);
            if (prod >= 0) {
                edgeTargetOwners[size_t(prod)].push_back(f.Target);
            }
        }
        for (size_t e = 0; e < edges.size(); ++e) {
            for (const std::string& outPath : edges[e].Outputs) {
                const auto lblIt = facts.LabelOfOutput.find(outPath);
                if (lblIt == facts.LabelOfOutput.end()) {
                    continue;
                }
                const auto ownIt = ownerIdByLabel.find(lblIt->second);
                if (ownIt != ownerIdByLabel.end()) {
                    edgeTargetOwners[e].push_back(ownIt->second);
                }
                const auto hookIt = facts.GlobalPchOwner.find(lblIt->second);
                if (hookIt != facts.GlobalPchOwner.end()) {
                    const auto mappedIt = ownerIdByLabel.find(hookIt->second);
                    if (mappedIt != ownerIdByLabel.end()) {
                        edgeTargetOwners[e].push_back(mappedIt->second);
                    }
                }
            }
            SortUnique(edgeTargetOwners[e]);
        }

        std::unordered_map<int, std::vector<int>> ninjaUpstreamCache;
        auto upstreamTargetsOfEdge = [&](int startEdge) -> const std::vector<int>& {
            const auto cached = ninjaUpstreamCache.find(startEdge);
            if (cached != ninjaUpstreamCache.end()) {
                return cached->second;
            }
            std::vector<char> visited(edges.size(), 0);
            std::vector<int> stack = {startEdge};
            std::vector<int> forbidden;
            while (!stack.empty()) {
                const int cur = stack.back();
                stack.pop_back();
                if (cur < 0 || size_t(cur) >= edges.size() || visited[size_t(cur)]) {
                    continue;
                }
                visited[size_t(cur)] = 1;
                const auto& ownIds = edgeTargetOwners[size_t(cur)];
                forbidden.insert(forbidden.end(), ownIds.begin(), ownIds.end());
                const auto& edge = edges[size_t(cur)];
                for (const auto* list : {&edge.Inputs, &edge.ImplicitInputs, &edge.OrderOnly, &edge.Validations}) {
                    for (const std::string& p : *list) {
                        const int dep = int(facts.Manifest->Producer(p));
                        if (dep >= 0 && edges[size_t(dep)].Outputs[0] != "build.ninja" && !visited[size_t(dep)]) {
                            stack.push_back(dep);
                        }
                    }
                }
                if (!edge.Outputs.empty()) {
                    const auto ndIt = ninjaDeps.Deps.find(edge.Outputs[0]);
                    if (ndIt != ninjaDeps.Deps.end()) {
                        for (int32_t pathId : ndIt->second) {
                            const int dep = int(facts.Manifest->Producer(ninjaDeps.Paths[size_t(pathId)]));
                            if (dep >= 0 && dep != cur && edges[size_t(dep)].Outputs[0] != "build.ninja" && !visited[size_t(dep)]) {
                                stack.push_back(dep);
                            }
                        }
                    }
                }
            }
            SortUnique(forbidden);
            return ninjaUpstreamCache.emplace(startEdge, std::move(forbidden)).first->second;
        };

        for (size_t u = 0; u < m.Units.size(); ++u) {
            THeaderUnit& unit = m.Units[u];
            if (!unit.Generated) {
                continue;
            }
            std::vector<std::string> gnSeeds;
            for (const std::string& hdr : unit.Headers) {
                const int prodEdge = headerProducerEdge(hdr);
                if (prodEdge >= 0) {
                    const auto& ninjaForbidden = upstreamTargetsOfEdge(prodEdge);
                    unit.ForbiddenTargets.insert(unit.ForbiddenTargets.end(),
                                                 ninjaForbidden.begin(), ninjaForbidden.end());
                }
                if (StartsWith(hdr, "<out>/")) {
                    const std::string relOut = hdr.substr(6);
                    const auto prodLbl = facts.LabelOfOutput.find(relOut);
                    if (prodLbl != facts.LabelOfOutput.end()) {
                        gnSeeds.push_back(prodLbl->second);
                        const auto pubIt = publicCodegen.find(prodLbl->second);
                        if (pubIt != publicCodegen.end()) {
                            gnSeeds.push_back(pubIt->second);
                        }
                    }
                }
            }
            if (!gnSeeds.empty()) {
                SortUnique(gnSeeds);
                for (const std::string& reachedLbl : Reachable(facts, gnSeeds)) {
                    const auto ownIt = ownerIdByLabel.find(reachedLbl);
                    if (ownIt != ownerIdByLabel.end()) {
                        unit.ForbiddenTargets.push_back(ownIt->second);
                    }
                    const auto hookIt = facts.GlobalPchOwner.find(reachedLbl);
                    if (hookIt != facts.GlobalPchOwner.end()) {
                        const auto mappedIt = ownerIdByLabel.find(hookIt->second);
                        if (mappedIt != ownerIdByLabel.end()) {
                            unit.ForbiddenTargets.push_back(mappedIt->second);
                        }
                    }
                }
            }
            SortUnique(unit.ForbiddenTargets);
        }

        // CondenseScc numbers child SCCs before parent SCCs: propagate ForbiddenTargets bottom-up
        // so checking any root unit immediately covers its entire transitive header closure.
        for (size_t u = 0; u < m.Units.size(); ++u) {
            auto& forbidden = m.Units[u].ForbiddenTargets;
            for (int child : m.Units[u].Children) {
                const auto& childForbidden = m.Units[size_t(child)].ForbiddenTargets;
                forbidden.insert(forbidden.end(), childForbidden.begin(), childForbidden.end());
            }
            SortUnique(forbidden);
        }

        // 7. Fingerprint all tracked inputs and persist analysis.bin.
        Log("fingerprinting analysis inputs...");
        const std::vector<std::string> fpPaths(fingerprint.begin(), fingerprint.end());
        const std::vector<std::string> fpDigests = DigestFiles(fpPaths, cfg.Workers);
        m.InputDigests.reserve(fpPaths.size());
        for (size_t i = 0; i < fpPaths.size(); ++i) {
            m.InputDigests.emplace_back(fpPaths[i], fpDigests[i]);
        }
        m.TreeHash = TreeIdentity(m.Root, true);
        m.BaselineHash = ComputeBaselineStamp(m);

        MakeDirs(planDir);
        SaveAnalysisModel(JoinPath(planDir, "analysis.bin"), m);
        Report(Format("  Extracted model: %zu GN targets, %zu C++ TUs (%zu classes), %zu header units, %d commits",
                      m.Targets.size(), m.Files.size(), m.ClassFlags.size(), m.Units.size(), m.Scenarios));
        return m;
    }

    // =========================================================================
    // Plan snapshot persistence
    // =========================================================================

    void SavePlanSnapshot(const std::string& path, const TPlanSnapshot& plan) {
        TBinWriter body;
        body.Str(plan.ToolDigest);
        body.Str(plan.TreeHash);
        WriteWorkflowConfigBody(body, plan.Config);
        WriteModelBody(body, plan.Model);

        body.U64(plan.Layout.Groups.size());
        for (const TPchGroup& g : plan.Layout.Groups) {
            body.I64(g.Class);
            body.I64(g.Parent);
            body.I64(g.RepresentativeFile);
            WriteIntVec(body, g.Roots);
            WriteIntVec(body, g.Units);
            body.U64(g.ActualBytes);
            WriteDouble(body, g.ActualBuildSec);
            body.StrList(g.GeneratedDeps);
        }
        WriteIntVec(body, plan.Layout.Assignment);
        body.StrList(plan.Layout.Reasons);

        WriteDouble(body, plan.TotalCompressibleCpuSec);
        WriteDouble(body, plan.CoveredCompressibleCpuSec);
        WriteDouble(body, plan.NetSavedCpuSec);
        body.U64(plan.TotalPchBytes);

        TBinWriter env;
        env.Str(PLAN_SCHEMA_VERSION);
        env.Str(HashHex(body.Data()));
        env.Str(body.Data());
        WriteFile(path, env.Data());
    }

    TPlanSnapshot LoadPlanSnapshot(const std::string& path) {
        const std::string raw = ReadFile(path);
        TBinReader env(raw);
        if (env.Str() != PLAN_SCHEMA_VERSION) {
            Fail("unsupported plan snapshot version; run --predict again");
        }
        const std::string expectedHash(env.Str());
        const std::string_view payload = env.Str();
        if (!env.AtEnd() || HashHex(payload) != expectedHash) {
            Fail("plan snapshot checksum mismatch");
        }
        TBinReader in(payload);
        TPlanSnapshot plan;
        plan.ToolDigest = std::string(in.Str());
        plan.TreeHash = std::string(in.Str());
        plan.Config = ReadWorkflowConfigBody(in);
        plan.Model = ReadModelBody(in);

        plan.Layout.Groups.resize(ReadBoundedCount(in));
        for (TPchGroup& g : plan.Layout.Groups) {
            g.Class = int(in.I64());
            g.Parent = int(in.I64());
            g.RepresentativeFile = int(in.I64());
            g.Roots = ReadIntVec(in);
            g.Units = ReadIntVec(in);
            g.ActualBytes = in.U64();
            g.ActualBuildSec = ReadDouble(in);
            g.GeneratedDeps = in.StrList();
        }
        plan.Layout.Assignment = ReadIntVec(in);
        plan.Layout.Reasons = in.StrList();

        plan.TotalCompressibleCpuSec = ReadDouble(in);
        plan.CoveredCompressibleCpuSec = ReadDouble(in);
        plan.NetSavedCpuSec = ReadDouble(in);
        plan.TotalPchBytes = in.U64();
        if (!in.AtEnd()) {
            Fail("plan snapshot has trailing bytes");
        }
        return plan;
    }

    TPlanSnapshot LoadAndValidatePlan(const std::string& planDir, const TWorkflowConfig& cfg) {
        TPlanSnapshot plan = LoadPlanSnapshot(JoinPath(planDir, "plan.bin"));
        if (plan.ToolDigest != DigestFile(SelfExe())) {
            Fail("pch_update binary changed since prediction");
        }
        if (plan.TreeHash != TreeIdentity(cfg.Root, false)) {
            Fail("source tree changed since prediction");
        }
        TWorkflowConfig lhs = plan.Config;
        TWorkflowConfig rhs = cfg;
        lhs.VerifyBuild = EVerifyBuild::Full;
        rhs.VerifyBuild = EVerifyBuild::Full;
        if (!(lhs == rhs)) {
            Fail("workflow configuration changed since prediction");
        }
        if (plan.Layout.Assignment.size() != plan.Model.Targets.size()) {
            Fail("corrupt plan target assignment table");
        }
        ValidateAnalysisInputs(plan.Model, cfg.Workers);
        return plan;
    }

} // namespace NPchUpdate
