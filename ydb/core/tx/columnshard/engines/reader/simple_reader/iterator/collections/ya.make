LIBRARY()

SRCS(
    abstract.cpp  # gn: into ydb/core/tx/columnshard/engines/reader/simple_reader/iterator
    constructors.cpp  # gn: into ydb/core/tx/columnshard/engines/reader/simple_reader/iterator
    not_sorted.cpp  # gn: into ydb/core/tx/columnshard/engines/reader/simple_reader/iterator
    full_scan_sorted.cpp  # gn: into ydb/core/tx/columnshard/engines/reader/simple_reader/iterator
    limit_sorted.cpp  # gn: into ydb/core/tx/columnshard/engines/reader/simple_reader/iterator
)

PEERDIR(
    ydb/core/formats/arrow
)

END()
