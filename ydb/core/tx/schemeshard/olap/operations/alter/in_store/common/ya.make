LIBRARY()

SRCS(
    update.cpp  # gn: into ydb/core/tx/schemeshard
)

PEERDIR(
    ydb/core/tx/schemeshard/olap/operations/alter/abstract
)

YQL_LAST_ABI_VERSION()

END()
