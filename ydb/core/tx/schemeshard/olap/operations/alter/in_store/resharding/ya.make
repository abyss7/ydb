YQL_LIBRARY()

# gn: into ydb/core/tx/schemeshard

SRCS(
    update.cpp
)

PEERDIR(
    ydb/core/tx/schemeshard/olap/operations/alter/abstract
    ydb/core/tx/schemeshard/olap/bg_tasks/tx_chain
)

END()
