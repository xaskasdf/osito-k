#ifndef OSITO_USER32_CONTROLS_H
#define OSITO_USER32_CONTROLS_H

#include "user32_shim.h"

typedef struct U32_CONTROL U32_CONTROL;
U32_CONTROL *user32_control_create(UINT atom);
void user32_control_release(U32_CONTROL *control);
BOOL user32_control_message(U32_CONTROL *control, HWND window, UINT message,
                            WPARAM wparam, LPARAM lparam, LRESULT *result);
PWSTR user32_copy_window_text(HWND window);

#endif
