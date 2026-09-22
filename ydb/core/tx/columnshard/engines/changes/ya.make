LIBRARY()

SRCS(
    cleanup_portions.cpp  # gn: into ydb/core/tx/columnshard/engines
    cleanup_tables.cpp  # gn: into ydb/core/tx/columnshard/engines
    compaction.cpp  # gn: into ydb/core/tx/columnshard/engines
    general_compaction.cpp  # gn: into ydb/core/tx/columnshard/engines
    merge_subset.cpp  # gn: into ydb/core/tx/columnshard/engines
    move_portions.cpp  # gn: into ydb/core/tx/columnshard/engines
    remove_portions.cpp  # gn: into ydb/core/tx/columnshard/engines
    ttl.cpp  # gn: into ydb/core/tx/columnshard/engines
    with_appended.cpp  # gn: into ydb/core/tx/columnshard/engines
)

PEERDIR(
    ydb/core/formats/arrow
    ydb/core/tx/columnshard/common
    ydb/core/tx/columnshard/engines/changes/abstract
    ydb/core/tx/columnshard/engines/changes/compaction
    ydb/core/tx/columnshard/engines/changes/counters
    ydb/core/tx/columnshard/engines/changes/actualization
    ydb/core/tx/columnshard/splitter
    ydb/core/tablet_flat
    ydb/core/tx/tiering
    ydb/core/protos
)

END()
