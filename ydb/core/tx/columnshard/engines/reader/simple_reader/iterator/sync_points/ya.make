LIBRARY()

SRCS(
    abstract.cpp  # gn: into ydb/core/tx/columnshard/engines/reader/simple_reader/iterator
    result.cpp  # gn: into ydb/core/tx/columnshard/engines/reader/simple_reader/iterator
    limit.cpp  # gn: into ydb/core/tx/columnshard/engines/reader/simple_reader/iterator
    aggr.cpp  # gn: into ydb/core/tx/columnshard/engines/reader/simple_reader/iterator
)

PEERDIR(
    ydb/core/formats/arrow
)

END()
