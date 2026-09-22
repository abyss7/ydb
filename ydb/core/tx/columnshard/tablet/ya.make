LIBRARY()

SRCS(
    ext_tx_base.cpp  # gn: into ydb/core/tx/columnshard
    write_queue.cpp  # gn: into ydb/core/tx/columnshard
)

PEERDIR(
    ydb/core/tx/columnshard/hooks/abstract
)

END()
