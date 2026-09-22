LIBRARY()

SRCS(
    tx_scan.cpp  # gn: into ydb/core/tx/columnshard
    tx_internal_scan.cpp  # gn: into ydb/core/tx/columnshard
)

PEERDIR(
    ydb/core/tx/columnshard/engines/reader/abstract
    ydb/core/tablet_flat
    ydb/core/tx/columnshard/engines/reader/actor
)

END()
