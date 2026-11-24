/* Differential packing check against Intel's independent Mesa decoder. */
#include "../drivers/intel_gen9_shader.c"
#include "compiler/brw_eu.h"
#include <stdio.h>
int main(void)
{
    struct intel_device_info dev;
    if (!intel_get_device_info_from_pci_id(0x3e9b,&dev)) return 2;
    struct brw_isa_info isa;brw_init_isa_info(&isa,&dev);
    uint64_t seed=0x3245abdc9876ef01ULL;
    unsigned checks=0;
    const unsigned ops[]={1,64,65};
    for (unsigned i=0;i<300000;i++) {
        seed=seed*6364136223846793005ULL+1442695040888963407ULL;
        uint64_t raw=(seed&~((1ULL<<7)|(1ULL<<28)|127ULL))|(1ULL<<29)|ops[i%3];
        uint32_t words[4];
        if (!gen9_expand_mov(raw,words)) return 3;
        brw_eu_compact_inst compact;memcpy(&compact,&raw,8);
        brw_eu_inst expected;brw_uncompact_instruction(&isa,&expected,&compact);
        if (memcmp(words,&expected,16)) {
            fprintf(stderr,"Mismatch raw=%016llx native=%08x/%08x/%08x/%08x expected=%016llx/%016llx\n",
                (unsigned long long)raw,words[0],words[1],words[2],words[3],
                (unsigned long long)expected.data[0],(unsigned long long)expected.data[1]);return 1;
        }
        checks++;
    }
    printf("Gen9 compact expansion: %u Intel decoder comparisons passed\n",checks);return 0;
}
