#pragma once

// The MiniKQL type-conversion helpers (namespace NKikimr::NScheme) moved to their
// proper low-level home in ydb/library/mkql_proto to avoid gn dependency cycles
// (they are pure scheme/minikql utilities, not KQP-specific). This forwarding
// header keeps existing includers of ydb/core/kqp/common/kqp_types.h working.
#include <ydb/library/mkql_proto/mkql_type_ops.h>
