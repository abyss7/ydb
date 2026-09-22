LIBRARY()

SRCS(
    client_session.cpp  # gn: into ydb/public/sdk/cpp/src/client/table
    data_query.cpp  # gn: into ydb/public/sdk/cpp/src/client/table
    readers.cpp  # gn: into ydb/public/sdk/cpp/src/client/table
    request_migrator.cpp  # gn: into ydb/public/sdk/cpp/src/client/table
    table_client.cpp  # gn: into ydb/public/sdk/cpp/src/client/table
    transaction.cpp  # gn: into ydb/public/sdk/cpp/src/client/table
)

PEERDIR(
    library/cpp/threading/future
    ydb/public/api/protos
    ydb/public/sdk/cpp/src/client/impl/endpoints
    ydb/public/sdk/cpp/src/client/impl/session
    ydb/public/sdk/cpp/src/client/table/query_stats
    ydb/public/sdk/cpp/src/library/string_utils/helpers
)

END()
