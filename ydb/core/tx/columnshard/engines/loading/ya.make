LIBRARY()

# gn: into ydb/core/tx/columnshard/engines

SRCS(
    stages.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/common
    ydb/core/tx/columnshard/tx_reader
)

END()
