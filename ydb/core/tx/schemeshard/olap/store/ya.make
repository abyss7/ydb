LIBRARY()

# gn: into ydb/core/tx/schemeshard

SRCS(
    store.cpp
)

PEERDIR(
    ydb/core/tx/schemeshard/olap/layout
    ydb/core/protos
)

END()
