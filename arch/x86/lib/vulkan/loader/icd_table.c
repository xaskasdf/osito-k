#include "loader.h"

/* Each ICD declares its own PFN_vk_icdGetInstanceProcAddr symbol.
 * Declarations live here so the table below can reference them. */

extern PFN_vkVoidFunction VKAPI_PTR
nvk_stub_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern PFN_vkVoidFunction VKAPI_PTR
venus_icdGetInstanceProcAddr(VkInstance instance, const char *name);

const struct osito_icd_entry osito_icd_table[] = {
    { "venus",    venus_icdGetInstanceProcAddr    },  /* preferred when VIRGL is up */
    { "nvk-stub", nvk_stub_icdGetInstanceProcAddr },
};
const unsigned osito_icd_count =
    sizeof(osito_icd_table) / sizeof(osito_icd_table[0]);
