LIBRARY()

SRCS(
    metadata_helpers.cpp
    scheme_helpers.h
)

PEERDIR(
    ydb/core/base
    ydb/core/cms/console
    ydb/core/kqp/gateway/actors
    ydb/core/protos
    ydb/core/protos/schemeshard
)

YQL_LAST_ABI_VERSION()

END()
