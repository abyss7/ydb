#pragma once

// No guard list yet: the slot's functions are made weak by a clang attribute
// region in yql/essentials/parser/pg_wrapper/interface/*.h; a guard would list
// the ones an executable must get from its provider.
