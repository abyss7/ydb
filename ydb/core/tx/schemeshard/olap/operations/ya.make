LIBRARY()

SRCS(
    create_table.cpp  # gn: into ydb/core/tx/schemeshard
    drop_table.cpp  # gn: into ydb/core/tx/schemeshard
    alter_table.cpp  # gn: into ydb/core/tx/schemeshard
    create_store.cpp  # gn: into ydb/core/tx/schemeshard
    drop_store.cpp  # gn: into ydb/core/tx/schemeshard
    alter_store.cpp  # gn: into ydb/core/tx/schemeshard
)

PEERDIR(
    ydb/core/mind/hive
    ydb/services/bg_tasks
    ydb/core/tx/schemeshard/olap/operations/alter
)

YQL_LAST_ABI_VERSION()

END()
