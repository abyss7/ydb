#pragma once

// Conversion helpers between NKikimr::NScheme::TTypeInfo, the MiniKQL proto type
// (NKikimrMiniKQL::TType) and the in-memory NMiniKQL::TType. These are pure
// scheme/minikql utilities; they used to live in ydb/core/kqp/common (as
// kqp_types.h) which made low-level modules such as ydb/core/engine depend on
// kqp/common and produced gn dependency cycles. mkql_proto is their natural home.

#include <ydb/core/scheme_types/scheme_type_info.h>
#include <ydb/library/mkql_proto/protos/minikql.pb.h>
#include <yql/essentials/minikql/mkql_node.h>

namespace NKikimr::NScheme {

void ProtoMiniKQLTypeFromTypeInfo(NKikimrMiniKQL::TType* type, const TTypeInfo typeInfo);
TTypeInfo TypeInfoFromProtoMiniKQLType(const NKikimrMiniKQL::TType& type);

TTypeInfo TypeInfoFromMiniKQLType(const NMiniKQL::TType* type);

} // namespace NKikimr::NScheme
