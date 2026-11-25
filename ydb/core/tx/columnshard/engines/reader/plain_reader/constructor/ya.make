LIBRARY()

SRCS(
    GLOBAL constructor.cpp  # gn: into ydb/core/tx/columnshard
    read_metadata.cpp  # gn: into ydb/core/tx/columnshard/engines/reader/plain_reader/iterator
)

PEERDIR(
    ydb/core/kqp/common/events
    ydb/core/tx/columnshard/engines/reader/abstract
    ydb/core/tx/columnshard/engines/reader/common_reader/constructor
)

END()
