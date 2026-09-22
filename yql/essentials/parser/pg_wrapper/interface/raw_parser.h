#pragma once

extern "C" {
    struct List;
    struct Node;
}

#include <yql/essentials/public/issue/yql_issue.h>

// gn: weak references to the yql_pg_runtime link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif

namespace NYql {

class IPGParseEvents {
public:
    virtual ~IPGParseEvents() = default;
    virtual void OnResult(const List* raw) = 0;
    virtual void OnError(const TIssue& issue) = 0;
};

TString PrintPGTree(const List* raw);

TString GetCommandName(Node* node);

void PGParse(const TString& input, IPGParseEvents& events);

class TArenaMemoryContext;

class TPGParseResult {
public:
    void Visit(IPGParseEvents& events) const;

    TPGParseResult() = default;
    TPGParseResult(TPGParseResult&& other) = default;
    explicit TPGParseResult(TIssue&& issue);
    TPGParseResult(const List* raw, THolder<TArenaMemoryContext>&& arena);
    TPGParseResult& operator=(TPGParseResult&& other) = default;
    ~TPGParseResult();

private:
    using TAstData = std::pair<const List*, THolder<TArenaMemoryContext>>;
    using TData = std::variant<TAstData, TIssue>;

    TData Data_;
};

void PGParse(const TString& input, TPGParseResult& result);

} // namespace NYql

#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute pop
#endif
