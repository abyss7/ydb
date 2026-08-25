#pragma once

#include <yql/essentials/core/dqs_expr_nodes/dqs_expr_nodes.gen.h>

#include <yql/essentials/core/expr_nodes/yql_expr_nodes.h>

namespace NYql::NNodes {
#include <yql/essentials/core/dqs_expr_nodes/dqs_expr_nodes.decl.inl.h>

#include <yql/essentials/core/dqs_expr_nodes/dqs_expr_nodes.defs.inl.h>

// Compatibility namespace: these expr-node classes live directly in NYql::NNodes
// (the code generator emits TMaybeNode<> specializations into NNodes). Some
// consumers — notably the YT provider — write `using namespace NNodes::NDq;`, so
// expose NNodes here as well.
namespace NDq {
    using namespace NYql::NNodes;
}
}
