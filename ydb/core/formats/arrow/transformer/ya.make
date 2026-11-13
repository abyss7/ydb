LIBRARY()

# gn: into ydb/core/formats/arrow

PEERDIR(
    contrib/libs/apache/arrow
    ydb/core/formats/arrow/dictionary
    ydb/library/formats/arrow/transformer
)

SRCS(
    dictionary.cpp
)

END()
