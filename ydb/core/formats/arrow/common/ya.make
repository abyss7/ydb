LIBRARY()

PEERDIR(
    contrib/libs/apache/arrow
    ydb/core/formats/arrow/switch
    ydb/library/actors/core
    ydb/library/conclusion
    ydb/library/formats/arrow
    ydb/core/formats/arrow/splitter
)

SRCS(
    container.cpp  # gn: into ydb/core/formats/arrow
    adapter.cpp  # gn: into ydb/core/formats/arrow
)

END()
