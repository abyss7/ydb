LIBRARY()

SRCS(
    read_metadata.cpp
    read_metadata_owner.cpp  # gn: into ydb/core/tx/columnshard
    resolver.cpp
)

PEERDIR(
    ydb/core/kqp/common/events
    ydb/core/tx/columnshard/engines/reader/abstract
    ydb/core/tx/columnshard/engines/reader/common
)

END()
