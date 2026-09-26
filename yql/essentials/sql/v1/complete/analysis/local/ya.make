LIBRARY()

SRCS(
    cursor_token_context.cpp
    local.cpp
    parser_call_stack.cpp
)

ADDINCL(
    yql/essentials/sql/v1/complete
)

PEERDIR(
    yql/essentials/parser/antlr_ast/gen/v1_ansi_antlr4
    yql/essentials/parser/antlr_ast/gen/v1_antlr4
    yql/essentials/sql/v1/complete/syntax
)

END()

RECURSE_FOR_TESTS(
    ut
)
