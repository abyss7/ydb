#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace NPchUpdate {
    class TNinjaManifest;
    class TWorkDir;

    struct TLinkLine {
        std::string Output;
        // Actual object/library paths, not target labels: two outputs of the
        // same target need not provide the same symbols or share a link line.
        std::set<std::string> Inputs;
    };

    struct TLinkInputs {
        std::unordered_map<std::string, std::vector<TLinkLine>> Targets;
        std::vector<std::string> Libraries;
    };

    using TSymbolProviders = std::map<std::string, std::set<std::string>>;

    struct TLinkAvailability {
        // Symbol -> final link outputs with no known provider on their line.
        std::map<std::string, std::vector<std::string>> Missing;
        std::vector<std::string> UnknownSymbols;
        bool UnknownLinks = false;

        // A necessary link-input check, not a substitute for a real link or
        // a check of initialization side effects. Unknown is never success.
        bool Available() const {
            return !UnknownLinks && UnknownSymbols.empty() && Missing.empty();
        }
    };

    TLinkInputs CollectLinkInputs(const TNinjaManifest& manifest,
                                 const std::unordered_map<std::string, std::string>& labels);

    // Extend the provider index for a fixed, built baseline. Artifact paths
    // have the same spelling as CollectLinkInputs, relative to buildDir.
    void LoadLinkProviders(TWorkDir& work, const std::string& buildDir,
                           const std::vector<std::string>& libraries,
                           const std::set<std::string>& symbols, TSymbolProviders& providers);

    TLinkAvailability CheckLinkRequirements(const std::vector<std::string>& symbols,
                                           const TSymbolProviders& providers,
                                           const std::vector<TLinkLine>& links);
} // namespace NPchUpdate
