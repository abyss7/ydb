LIBRARY()

# gn: into ydb/core/tx/columnshard

SRCS(
    ext_tx_base.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/hooks/abstract
)

END()
