#pragma once
#include "ppc.h"

typedef struct FuncEntry {
    uint32_t addr;
    GuestFn fn;
    const char *name;
} FuncEntry;

extern const FuncEntry g_func_table[];
extern const unsigned g_func_count;

const FuncEntry *func_lookup(uint32_t addr);
const FuncEntry *func_containing(uint32_t addr);
