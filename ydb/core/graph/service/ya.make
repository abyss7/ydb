LIBRARY()

SRCS(
    log.h
    service_impl.cpp
)

PEERDIR(
    ydb/core/base
    ydb/core/graph/api
    ydb/core/tablet
    ydb/public/sdk/cpp/src/client/params
)

END()
