#ifndef OSITO_USER32_LISTBOX_H
#define OSITO_USER32_LISTBOX_H

#include "user32_shim.h"

typedef struct U32_LISTBOX U32_LISTBOX;
U32_LISTBOX *user32_listbox_create(DWORD style);
void user32_listbox_release(U32_LISTBOX *list);
BOOL user32_listbox_has_strings(const U32_LISTBOX *list);
BOOL user32_listbox_message(U32_LISTBOX *list, HWND window, DWORD message,
                            WPARAM wp, LPARAM lp, BOOL wide, LRESULT *result);

#endif
