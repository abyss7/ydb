#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace NPchUpdate::NTwoStep {

    // All times are seconds, storage and memory are bytes. IDs are indices in
    // their respective tables and remain stable throughout a predict/correct cycle.
    struct TResources {
        int Workers = 1;
        double Ram = 0;
        double Disk = 0; // resident PCH budget
        double Bandwidth = 0; // concurrent PCH read/write bytes per second
        std::map<std::string, int> Pools;
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
        bool Stable = false;
        bool Safe = true;
    };

    struct TFile {
        int Action = -1;
        int Class = -1;
        std::string Source;
        std::vector<std::pair<int, double>> Own;
    };

    struct TData {
        std::vector<TAction> Actions;
        std::vector<TUnit> Units;
        std::vector<TFile> Files;
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
        std::vector<int> Assignment; // file -> leaf, -1 means no PCH
    };

    struct TCoefficients {
        double Baseline = 1;
        double Saved = 1;
        double Load = 0.02;
        double Residual = 0;
        double BaseBuild = 1;
        double DeltaBuild = 1;
        double Size = 3.9; // PCH bytes per header source byte, calibrated from artifacts
        double Memory = 1;
        double ConsumerMemory = 1;
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

    struct TCalibration {
        std::vector<TCoefficients> Classes;
        // Non-PCH actions measured in S0, reusable when scoring S1.
        std::vector<double> ActionTime;
        std::vector<double> ActionMemory;
        std::string Report;
    };

    // Returns infinity on resource infeasibility; malformed graphs are errors.
    double Makespan(const std::vector<TAction>& actions, const TResources& resources,
                    const std::vector<char>& dirty = {});
    std::vector<char> Invalidate(const std::vector<TAction>& actions, int scenario);
    TPrediction Predict(const TData& data, const TLayout& layout, const TCalibration& calibration);
    TLayout Select(const TData& data, const TCalibration& calibration, const TLayout* frozen = nullptr);
    TCalibration Calibrate(const TData& data, const TLayout& layout, const TPrediction& prediction,
                           const std::vector<TObservation>& observations);

    int Command(const std::vector<std::string>& args);
    int SelfTest();
    void IoSelfTest();

} // namespace NPchUpdate::NTwoStep
