/* OsitoK shim — fenv.h */
#ifndef _FENV_H
#define _FENV_H
#define FE_DOWNWARD   1
#define FE_TONEAREST  0
#define FE_TOWARDZERO 3
#define FE_UPWARD     2
typedef unsigned short fenv_t;
typedef unsigned short fexcept_t;
int fesetround(int round);
int fegetround(void);
#endif
