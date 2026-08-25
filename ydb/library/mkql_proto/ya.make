LIBRARY()

PEERDIR(
    ydb/core/scheme_types
    ydb/library/mkql_proto/protos
    yql/essentials/minikql/computation
    yql/essentials/parser/pg_catalog
    yql/essentials/parser/pg_wrapper/interface
    yql/essentials/providers/common/codec
    ydb/public/api/protos
)

SRCS(
    mkql_proto.cpp
    mkql_type_ops.cpp
)

YQL_LAST_ABI_VERSION()

END()

IF (NOT OPENSOURCE OR OPENSOURCE_PROJECT == "ydb")
    RECURSE_FOR_TESTS(
        ut
    )
ENDIF()
