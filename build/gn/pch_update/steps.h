#pragma once

#include "facts.h"
#include "jobs.h"

#include <set>
#include <string>
#include <vector>

namespace NPchUpdate {

    // The command lines of the steps.
    struct TTools {
        std::string Gn = "gn";
        std::string Ninja = "ninja";
    };

    bool BuildRunning(const std::string& buildDir);

    // An include dir of a command of the baseline as gn spells it.
    std::string GnDir(const TFacts& facts, const std::string& dir);
    // The targets `start` depend on (them too), not through an executable:
    // with pch_mode = "global" the tools are of tool_toolchain.
    std::set<std::string> Reachable(const TFacts& facts, const std::vector<std::string>& start);
    // pch_only_cflags of pch_flags.gni.
    std::vector<std::string> PchOnlyArgv(const TFacts& facts);
    // The same under a source root: false when the file or the value is absent.
    bool TryPchOnlyArgv(const std::string& root, std::vector<std::string>* argv);

    // The steps: each reports its lines; false stops the run, nothing written.
    // check, gn labels, traces: the baseline as built, the facts of its files.
    bool CheckBaseline(const TOptions& options, const TTools& tools, TWorkDir& work, TFacts& facts);
    std::vector<std::string> GnGenerationSwitches(const TNinjaManifest& manifest);
    bool ReadTraces(const TOptions& options, TWorkDir& work, TFacts& facts);
    bool ReadHistory(const TOptions& options, TFacts& facts);
    bool FlagClasses(const TOptions& options, TFacts& facts);
    bool HeaderFacts(const TOptions& options, TWorkDir& work, TFacts& facts);

} // namespace NPchUpdate
