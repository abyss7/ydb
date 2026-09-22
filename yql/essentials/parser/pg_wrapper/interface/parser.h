#pragma once

#include <yql/essentials/ast/yql_ast.h>
#include <yql/essentials/parser/pg_catalog/catalog.h>
#include <yql/essentials/parser/pg_wrapper/interface/raw_parser.h>

// gn: weak references to the yql_pg_runtime link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif

namespace NSQLTranslation {

struct TTranslationSettings;
class ITranslator;
using TTranslatorPtr = TIntrusivePtr<ITranslator>;

} // namespace NSQLTranslation

namespace NSQLTranslationPG {

NYql::TAstParseResult PGToYql(const TString& query, const NSQLTranslation::TTranslationSettings& settings, NYql::TStmtParseInfo* stmtParseInfo = nullptr);
NYql::TAstParseResult PGToYql(const NYql::TPGParseResult& parseResult, const TString& query, const NSQLTranslation::TTranslationSettings& settings, NYql::TStmtParseInfo* stmtParseInfo = nullptr);
TVector<NYql::TAstParseResult> PGToYqlStatements(const TString& query, const NSQLTranslation::TTranslationSettings& settings, TVector<NYql::TStmtParseInfo>* stmtParseInfo = nullptr);
std::unique_ptr<NYql::NPg::IExtensionSqlParser> CreateExtensionSqlParser();
std::unique_ptr<NYql::NPg::ISystemFunctionsParser> CreateSystemFunctionsParser();
std::unique_ptr<NYql::NPg::ISqlLanguageParser> CreateSqlLanguageParser();
NSQLTranslation::TTranslatorPtr MakeTranslator();

} // namespace NSQLTranslationPG

#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute pop
#endif
