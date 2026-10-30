#ifndef OSITOK_MSI_SUMMARY_H
#define OSITOK_MSI_SUMMARY_H
#include "msi.h"
typedef struct {
    uint32_t codepage, installer_version, word_count;
    uint16_t *platform, *package_code;
} msi_summary_t;
/* Required MSI summary properties from the standard SummaryInformation set.
 * Text is owned UTF-16. Unsupported property types/codepages fail explicitly. */
msi_summary_t *msi_summary_read(msi_db_t *database, uint32_t *error);
msi_summary_t *msi_summary_parse(const uint8_t *stream, uint32_t size, uint32_t *error);
void msi_summary_close(msi_summary_t *summary);
#endif
