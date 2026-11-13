YQL_LIBRARY()

# gn: into ydb/core/tx/schemeshard

SRCS(
    update.cpp
)

PEERDIR(
    ydb/core/tx/schemeshard/olap/operations/alter/abstract
)

END()
