#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace NPchUpdate::NTwoStep {

    // All times are seconds, storage and memory are bytes. IDs are indices in
    // their respective tables and remain stable throughout a predict/correct cycle.
    struct TResources {
        int Workers = 1; // -j: what schedules the build, with the pools below
        double Ram = 0; // RAM behind --workers where it is not given; never a schedule input
        double Disk = 0; // resident PCH budget
        double Bandwidth = 0; // kept for the snapshot format; schedules nothing
        std::map<std::string, int> Pools; // the named pools of the build, as Ninja limits them
    };

    struct TAction {
        std::string Output;
        std::string Pool;
        double Time = 0;
        double Memory = 0;
        double Io = 0;
        bool Phony = false;
        std::vector<int> Dependencies; // includes order-only dependencies
        std::vector<int> DirtyDependencies; // excludes order-only dependencies
        std::vector<int> Changes; // historical scenarios touching an input
    };

    struct TUnit {
        std::string Include; // empty if no safe standalone root exists
        std::vector<std::string> Headers;
        std::vector<int> Children;
        std::vector<int> Changes;
        std::vector<int> Producers;
        double Parse = 0;
        double Bytes = 0; // header source bytes, serialized once by a PCH
        // Unchanged in the history and not generated. A header that did
        // change may still be base material when the change is cheap for the
        // objective: Pick decides.
        bool Stable = false;
        bool Safe = true;
    };

    struct TFile {
        int Action = -1;
        int Class = -1;
        std::string Source;
        std::vector<std::pair<int, double>> Own;
        int Target = -1;
        double Frontend = 0; // baseline trace: frontend seconds, zero if unknown
        double Outer = 0; // baseline trace: seconds in the outermost includes
    };

    struct TTarget {
        std::string Label; // GN PCH owner, including helpers inheriting its config
        int Class = -1; // mixed compiler classes: keep the entire target without PCH
        std::vector<int> Files;
        bool PchEligible = true; // all members receive this owner's PCH in GN
    };

    struct TData {
        std::vector<TAction> Actions;
        std::vector<TUnit> Units;
        std::vector<TFile> Files;
        std::vector<TTarget> Targets;
        int Classes = 0;
        int Scenarios = 0;
        double Q = 0;
        TResources Resources;
        double ActionMemory = 1e9; // explicit prior for unmeasured actions
    };

    struct TPch {
        int Class = -1;
        int Parent = -1;
        int Representative = -1; // a file with exactly matching flags
        std::vector<int> Roots;
        std::vector<int> Units; // closure including the parent
    };

    struct TLayout {
        std::vector<TPch> Pchs; // bases first, then deltas
        int Bases = 0;
        // The first Frozen bases come from the initial plan. Their content is
        // fixed and neither fit evicts them. A base at or after Frozen is one
        // the correction built for a class the initial plan left without, and
        // it competes for the disk like an initial base. Zero in the initial plan.
        int Frozen = 0;
        std::vector<int> Assignment; // target -> leaf, -1 means no PCH
        // Initial plan only: 1 = calibration control without PCH, 2 = control
        // kept on the base to measure the deltas' marginal value. Empty otherwise.
        // A class of fewer than four owners has none.
        std::vector<char> Controls;
    };

    struct TCoefficients {
        double Baseline = 1;
        double Saved = 1;
        double Load = 0.02;
        double Residual = 0;
        // Priors of an unmeasured class: the medians of the classes a full
        // run did measure, the size the bytes-weighted ratio of its artifacts
        // (template instantiation makes a PCH half again the bytes the old
        // 3.9 estimated, and a budget fitted to it was over by that much).
        double BaseBuild = 1.1;
        double DeltaBuild = 1.75;
        double Size = 6.2; // PCH bytes per header source byte, calibrated from artifacts
        double Memory = 1;
        double ConsumerMemory = 1;
        // Extra saved fraction per unit of (Frontend - Outer) / Outer: what
        // -fpch-instantiate-templates removes from the end of the unit.
        double Instantiate = 0;
        bool Identified = false;
    };

    struct TPrediction {
        std::vector<TAction> Actions;
        std::vector<double> Saved;
        std::vector<double> Load;
        std::vector<double> PchBytes;
        double Cold = 0;
        double Incremental = 0;
        double Objective = 0;
        bool Feasible = false;
    };

    struct TObservation {
        double Time = 0;
        double Cpu = 0;
        double Memory = 0;
        double Bytes = 0;
        int64_t Start = 0;
        int64_t End = 0;
        std::string Invocation;
    };

    // A PCH built and measured in S0. Identity is content, never an ID: a
    // later layout reuses the observation only for the same own units, roots
    // and parent chain. Time and memory are zero for a probe: a PCH the plan
    // built, weighed and then left, whose build nothing timed.
    struct TMeasuredPch {
        TPch Pch;
        TPch Parent; // default for a base
        double Bytes = 0;
        double Time = 0;
        double Memory = 0;
    };

    struct TCalibration {
        std::vector<TCoefficients> Classes;
        std::vector<TMeasuredPch> Measured;
        // The probes a plan built and left that the measurement did not weigh
        // in its own right: sizes without build times.
        std::vector<TMeasuredPch> Probes;
        // PCH bytes per header source byte of every measured and probed
        // artifact of every class together, moved towards the prior by the
        // bytes behind it (zero: nothing was weighed). A class's own scale
        // is moved towards it by the bytes the class weighed; a class that
        // weighed nothing takes it as it is.
        double GlobalSize = 0;
        // Per action. Compilations of measured targets carry the anchored
        // baseline (the trace time that reproduces the observed S0 compile
        // under the initial leaf); other actions the observation or the model.
        std::vector<double> ActionTime;
        std::vector<double> ActionMemory;
        std::vector<double> ActionFloor; // irreducible backend seconds
        // Saved-time multiplier per target, shrunk toward 1 by the noise of
        // the class controls and never below zero; 1 for unmeasured targets.
        std::vector<double> TargetGain;
        // Per compiler class, per unit: PCH bytes per source byte, build
        // seconds per parse second (zero: class prior) and saved-time
        // multiplier (negative: none, zero: a leaf that helped nobody). Empty
        // tables mean uncalibrated.
        std::vector<std::vector<double>> UnitSize;
        std::vector<std::vector<double>> UnitBuild;
        std::vector<std::vector<double>> UnitGain;
        // Every measured artifact of every class, decomposed together: a
        // unit's PCH bytes per source byte and its build-time multiplier
        // (zero: no artifact measured it). A class that never measured a unit
        // takes these before the scale of its own small artifacts.
        std::vector<double> GlobalUnitSize;
        std::vector<double> GlobalUnitBuild;
        // Build seconds per parse second over the classes that measured each;
        // the fallback of a class that measured neither (zero: the prior stands).
        double GlobalBaseBuild = 0;
        double GlobalDeltaBuild = 0;
        std::string Report;
        std::string TargetReport; // per measured PCH target: anchor and multiplier
        std::string UnitReport; // per calibrated unit: size, build and gain weights
    };

    // Use verified artifact sizes. Keep original IDs until the caller remaps
    // both the layout and the generated prerequisites of each surviving action.
    // The first `frozenBases` bases are never evicted.
    std::string FitMeasuredDisk(const TData& data, TLayout& layout, const TCalibration& calibration,
                                const std::vector<uint64_t>& bytes, int frozenBases,
                                const std::function<bool(int target, int parent)>& compatible);
    std::vector<int> CompactLayout(TLayout& layout);
    // The assignments as the compatibility checks and the measured fit left
    // them, priced once more: a family that no longer pays for its base
    // leaves, an owner steps down to its parent or to NONE, never to a leaf
    // nobody checked it with. `compatible(target, parent)` says whether the
    // parent compiles the owner; a parent that does not is no fallback, and
    // the owner keeps a delta that pays against NONE alone. Returns the payback rows.
    std::string FinalPayback(const TData& data, TLayout& layout, const TCalibration& calibration,
                             const std::function<bool(int, int)>& compatible = {});
    // What a layout is worth under a calibration: payback, the fit at the
    // final budget with every base of it frozen, the score. The fit and the
    // score take the `measured` sizes of the artifacts when they are given
    // for every PCH of the layout, estimates otherwise.
    TLayout ScoredLayout(const TData& data, const TLayout& layout, const TCalibration& calibration,
                         size_t analysisJobs, double* objective, const std::vector<uint64_t>* measured = nullptr);

    // Scoring returns infinity only when its schedule cannot proceed, which a
    // graph without cycles never does; malformed graphs are errors.
    // Optional observer: no model/checkpoint fields and no additional scoring.
    struct TSelectionDiagnostics {
        std::string Targets; // winning width, one decision per owner
        std::string Choices; // shortlist costs, IDs local to each width before pruning
        std::string Candidates; // roots/parents for those local IDs
        std::string Payback; // marginal delta and whole-family work on assigned targets
        std::string Widths;
    };

    void GroupTargets(TData& data, const std::vector<std::string>& owners);
    void ValidateTargets(const TData& data);
    // Cold wall time of the dirty actions under the worker count and the
    // named pools: the schedule Ninja runs, and nothing else. Returns
    // infinity only when nothing can run before everything is done, which a
    // graph without cycles never does; malformed graphs are errors.
    double Makespan(const std::vector<TAction>& actions, const TResources& resources,
                    const std::vector<char>& dirty = {});
    std::vector<char> Invalidate(const std::vector<TAction>& actions, int scenario);
    // Analysis concurrency is execution-only: it does not change modeled build
    // resources, snapshot identity or deterministic reduction order.
    TPrediction Predict(const TData& data, const TLayout& layout, const TCalibration& calibration,
                        size_t analysisJobs = 1, const std::vector<uint64_t>& measuredBytes = {});
    // Score a fixed set of PCHs, repairing cyclic GN target assignments in place.
    // With `measured`: the sizes of the artifacts the plan built, not estimates.
    TPrediction ScoreCandidate(const TData& data, TLayout& layout, const TCalibration& calibration,
                               size_t analysisJobs, int width, const std::vector<uint64_t>* measured = nullptr);
    // Without `frozen`: bases, deltas and assignments from scratch, with the
    // calibration controls of TLayout::Controls. With `frozen`: the first
    // TLayout::Frozen bases stay as they are, a base is built for each class
    // the plan left without one, and a frozen base leaves a candidate as soon
    // as that candidate is scored without an assigned target or a surviving
    // delta.
    TLayout Select(const TData& data, const TCalibration& calibration, const TLayout* frozen = nullptr,
                   size_t analysisJobs = 1,
                   TSelectionDiagnostics* diagnostics = nullptr);
    // What a measurement of `layout` teaches the next selection: class
    // coefficients, every built artifact by content, per-target anchors and
    // multipliers, per-unit weights. Observations cover every action of
    // `prediction` (zero time: not measured). `probes` are the PCHs a plan
    // built, weighed and left: their sizes count, their builds were not timed.
    TCalibration Calibrate(const TData& data, const TLayout& layout, const TPrediction& prediction,
                           const std::vector<TObservation>& observations, const std::vector<TMeasuredPch>& probes = {});

    int Command(const std::vector<std::string>& args);
    int SelfTest();
    void IoSelfTest();

} // namespace NPchUpdate::NTwoStep
