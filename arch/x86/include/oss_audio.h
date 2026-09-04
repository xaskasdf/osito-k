#ifndef OSITOK_OSS_AUDIO_H
#define OSITOK_OSS_AUDIO_H

#include "fd.h"

extern const fd_device_ops_t oss_audio_device_ops;

/* Create one OSS open-file description. legacy_audio selects the
 * 8 kHz mono mu-law defaults used by /dev/audio. */
int oss_audio_create(bool legacy_audio, uint32_t access_mode,
                     void **context_out);
int oss_audio_selftest(void);

#endif
