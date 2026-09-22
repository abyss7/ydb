#pragma once

#include <yql/essentials/core/cbo/cbo_optimizer_new.h>

#include <functional>

// gn: weak references to the yql_pg_runtime link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif

namespace NYql {

struct TExprContext;

struct IOptimizer {
    using TVarId = std::tuple<int, int>;

    struct TVar {
        char Name = 0; // debug name: 'a', 'b', 'c', ...
    };

    struct TRel {
        double Rows = 0;
        double TotalCost = 0;

        std::vector<TVar> TargetVars;
    };

    struct TEq {
        std::vector<TVarId> Vars;
    };

    struct TInput {
        std::vector<TRel> Rels;
        std::vector<TEq> EqClasses;
        std::vector<TEq> Left;
        std::vector<TEq> Right;

        TString ToString() const;
        void Normalize();
    };

    enum class EJoinType {
        Unknown,
        Inner,
        Left,
        Right,
    };

    enum class EJoinStrategy {
        Unknown,
        Hash,
        Loop
    };

    struct TJoinNode {
        EJoinType Mode = EJoinType::Unknown;
        EJoinStrategy Strategy = EJoinStrategy::Unknown;
        // only a = b && c = d ...  supported yet
        std::vector<TVarId> LeftVars = {};
        std::vector<TVarId> RightVars = {};
        std::vector<int> Rels = {};
        int Outer = -1; // index in Nodes
        int Inner = -1; // index in Nodes
    };

    struct TOutput {
        std::vector<TJoinNode> Nodes;
        TInput* Input = nullptr;
        double Rows = 0;
        double TotalCost = 0;

        TString ToString(bool printCost = true) const;
    };

    virtual ~IOptimizer() = default;
    virtual TOutput JoinSearch() = 0;
};

IOptimizer* MakePgOptimizerInternal(const IOptimizer::TInput& input, const std::function<void(const TString&)>& log = {});

IOptimizerNew* MakePgOptimizerNew(IProviderContext& pctx, TExprContext& ctx, const std::function<void(const TString&)>& log = {});

} // namespace NYql

#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute pop
#endif
