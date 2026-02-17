/*
 * OsitoK - Tiny BASIC interpreter
 *
 * Interactive BASIC interpreter running inside the shell task.
 * Enter with 'basic' command, exit with BYE or Ctrl+C.
 *
 * Language: PRINT, INPUT, LET, IF/THEN, GOTO, GOSUB/RETURN,
 *           FOR/NEXT/STEP, REM, END, NEW, LIST, RUN, SAVE, LOAD, BYE
 * Graphics: CLS, PSET, LINE, DRAW
 * Variables: A-Z (26 int32), expressions with +,-,*,/,MOD,AND,OR,NOT
 */
#ifndef OSITO_BASIC_H
#define OSITO_BASIC_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enter the BASIC interpreter (blocks until BYE/Ctrl+C) */
void basic_enter(void);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_BASIC_H */
