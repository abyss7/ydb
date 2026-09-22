LIBRARY()

SRCS(
    read_metadata.cpp
    read_metadata_shard.cpp  # gn: shard
    resolver.cpp
)
# gn: shard headers read_metadata_shard.h

PEERDIR(
    ydb/core/tx/columnshard/engines/reader/abstract
    ydb/core/kqp/compute_actor
)

END()
