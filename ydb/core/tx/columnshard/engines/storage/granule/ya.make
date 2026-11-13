LIBRARY()

# gn: into ydb/core/tx/columnshard/engines

SRCS(
    granule.cpp
    loaded_key_ranges.cpp
    storage.cpp
    portions_index.cpp
    portion_interval_tree.cpp
    stages.cpp
)

PEERDIR(
    ydb/core/tx/columnshard/engines/storage/optimizer/abstract
    ydb/core/tx/columnshard/engines/storage/actualizer/index
    ydb/core/tx/columnshard/counters
    ydb/core/tx/columnshard/engines/portions
    ydb/core/tx/columnshard/engines/predicate
    ydb/core/tx/columnshard/hooks/abstract
    ydb/core/base
    ydb/core/formats/arrow/reader
    ydb/core/tx/columnshard/engines/storage/optimizer/lbuckets/planner  # gn: plugin
    ydb/core/tx/columnshard/engines/storage/optimizer/lcbuckets/planner  # gn: plugin
)

GENERATE_ENUM_SERIALIZATION(granule.h)

END()
