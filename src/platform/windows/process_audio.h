#ifndef LIFESCREEN_PROCESS_AUDIO_H
#define LIFESCREEN_PROCESS_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void* process_audio_open(uint32_t target_pid, bool exclude_process);
uint32_t process_audio_read(void* handle, float* out_samples, uint32_t max_floats, float* out_peak);
void process_audio_close(void* handle);

#ifdef __cplusplus
}
#endif

#endif
