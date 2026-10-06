LIBRARY()

# gn: move into parent

SRCS(
    GLOBAL gc_counters.cpp
    GLOBAL broken_txs.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/normalizer/abstract
)

END()
