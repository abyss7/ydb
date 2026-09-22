#pragma once

#include <string_view>
#include <yql/essentials/core/pg_settings/guc_settings.h>
#include <yql/essentials/parser/pg_catalog/catalog.h>

// gn: weak references to the yql_pg_runtime link slot, see build/gn/link_slots.gni
#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute push (__attribute__((weak)), apply_to = function)
#endif

namespace NKikimr {
namespace NMiniKQL {

void* PgInitializeMainContext();
void PgDestroyMainContext(void* ctx);

void PgAcquireThreadContext(void* ctx);
void PgReleaseThreadContext(void* ctx);

std::unique_ptr<NYql::NPg::IExtensionLoader> CreateExtensionLoader();

void* PgInitializeContext(const std::string_view& contextType);
void PgDestroyContext(const std::string_view& contextType, void* ctx);

void PgSetGUCSettings(void* ctx, const TGUCSettings::TPtr& GUCSettings);
std::optional<std::string> PGGetGUCSetting(const std::string& key);

void PgCreateSysCacheEntries(void* ctx);
} // namespace NMiniKQL
} // namespace NKikimr

#if defined(YQL_GN_LINK_SLOTS) && !defined(GN_SLOT_PROVIDER_yql_pg_runtime)
#pragma clang attribute pop
#endif
