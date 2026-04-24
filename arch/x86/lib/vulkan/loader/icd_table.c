#include "loader.h"

/* Each ICD declares its own PFN_vk_icdGetInstanceProcAddr symbol.
 * Declarations live here so the table below can reference them. */

extern PFN_vkVoidFunction VKAPI_PTR
nvk_stub_icdGetInstanceProcAddr(VkInstance instance, const char *name);

/* Wave 3 will add venus entry here. For now, only nvk-stub registers. */

const struct osito_icd_entry osito_icd_table[] = {
    { "nvk-stub", nvk_stub_icdGetInstanceProcAddr },
};
const unsigned osito_icd_count =
    sizeof(osito_icd_table) / sizeof(osito_icd_table[0]);
