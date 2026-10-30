#ifndef OSITO_WIN32_OSVERSION_H
#define OSITO_WIN32_OSVERSION_H

/* Embedded application manifest compatibility: 62, 63 or 100. The caller
 * obtains the resource from the main executable, never the calling DLL. */
unsigned osversion_manifest_level(const void *data, unsigned size);

#endif
