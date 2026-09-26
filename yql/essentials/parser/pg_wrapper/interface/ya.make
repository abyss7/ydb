LIBRARY()

SRCS(
    arrow.h  # gn: slot yql_pg_runtime
    codec.h  # gn: slot yql_pg_runtime
    compare.h  # gn: slot yql_pg_runtime
    comp_factory.h  # gn: slot yql_pg_runtime
    config.h  # gn: slot yql_pg_runtime
    context.h  # gn: slot yql_pg_runtime
    interface.h
    interface.cpp
    optimizer.h  # gn: slot yql_pg_runtime
    pack.h  # gn: slot yql_pg_runtime
    parser.h  # gn: slot yql_pg_runtime
    raw_parser.h  # gn: slot yql_pg_runtime
    type_desc.h  # gn: slot yql_pg_runtime
    utils.h  # gn: slot yql_pg_runtime
)

PEERDIR(
    util
    yql/essentials/ast
    yql/essentials/public/udf
    yql/essentials/public/udf/arrow
    yql/essentials/core/cbo
    library/cpp/disjoint_sets
    yql/essentials/providers/common/codec/yt_arrow_converter_interface
)

YQL_LAST_ABI_VERSION()

END()
