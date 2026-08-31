LIBRARY()

SRCS(
    read_metadata.cpp
    read_metadata_shard.cpp
    resolver.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/engines/reader/abstract
    ydb/core/kqp/compute_actor
)

END()
