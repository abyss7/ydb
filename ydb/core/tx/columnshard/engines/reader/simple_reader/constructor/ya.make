LIBRARY()

SRCS(
    GLOBAL constructor.cpp
    read_metadata.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/engines/reader/abstract
    ydb/core/tx/columnshard/engines/reader/common_reader/constructor  # gn: :shard
    ydb/core/kqp/compute_actor
)

END()
