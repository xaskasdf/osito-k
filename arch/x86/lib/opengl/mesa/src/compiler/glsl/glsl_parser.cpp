/* A Bison parser, made by GNU Bison 2.3.  */

/* Skeleton implementation for Bison's Yacc-like parsers in C

   Copyright (C) 1984, 1989, 1990, 2000, 2001, 2002, 2003, 2004, 2005, 2006
   Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Bison version.  */
#define YYBISON_VERSION "2.3"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 1

/* Using locations.  */
#define YYLSP_NEEDED 1

/* Substitute the variable and function names.  */
#define yyparse _mesa_glsl_parse
#define yylex   _mesa_glsl_lex
#define yyerror _mesa_glsl_error
#define yylval  _mesa_glsl_lval
#define yychar  _mesa_glsl_char
#define yydebug _mesa_glsl_debug
#define yynerrs _mesa_glsl_nerrs
#define yylloc _mesa_glsl_lloc

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
     ATTRIBUTE = 258,
     CONST_TOK = 259,
     BASIC_TYPE_TOK = 260,
     BREAK = 261,
     BUFFER = 262,
     CONTINUE = 263,
     DO = 264,
     ELSE = 265,
     FOR = 266,
     IF = 267,
     DEMOTE = 268,
     DISCARD = 269,
     RETURN = 270,
     SWITCH = 271,
     CASE = 272,
     DEFAULT = 273,
     CENTROID = 274,
     IN_TOK = 275,
     OUT_TOK = 276,
     INOUT_TOK = 277,
     UNIFORM = 278,
     VARYING = 279,
     SAMPLE = 280,
     NOPERSPECTIVE = 281,
     FLAT = 282,
     SMOOTH = 283,
     IMAGE1DSHADOW = 284,
     IMAGE2DSHADOW = 285,
     IMAGE1DARRAYSHADOW = 286,
     IMAGE2DARRAYSHADOW = 287,
     COHERENT = 288,
     VOLATILE = 289,
     RESTRICT = 290,
     READONLY = 291,
     WRITEONLY = 292,
     SHARED = 293,
     STRUCT = 294,
     VOID_TOK = 295,
     WHILE = 296,
     IDENTIFIER = 297,
     TYPE_IDENTIFIER = 298,
     NEW_IDENTIFIER = 299,
     FLOATCONSTANT = 300,
     FLOAT16CONSTANT = 301,
     DOUBLECONSTANT = 302,
     INTCONSTANT = 303,
     UINTCONSTANT = 304,
     BOOLCONSTANT = 305,
     INT64CONSTANT = 306,
     UINT64CONSTANT = 307,
     FIELD_SELECTION = 308,
     LEFT_OP = 309,
     RIGHT_OP = 310,
     INC_OP = 311,
     DEC_OP = 312,
     LE_OP = 313,
     GE_OP = 314,
     EQ_OP = 315,
     NE_OP = 316,
     AND_OP = 317,
     OR_OP = 318,
     XOR_OP = 319,
     MUL_ASSIGN = 320,
     DIV_ASSIGN = 321,
     ADD_ASSIGN = 322,
     MOD_ASSIGN = 323,
     LEFT_ASSIGN = 324,
     RIGHT_ASSIGN = 325,
     AND_ASSIGN = 326,
     XOR_ASSIGN = 327,
     OR_ASSIGN = 328,
     SUB_ASSIGN = 329,
     INVARIANT = 330,
     PRECISE = 331,
     LOWP = 332,
     MEDIUMP = 333,
     HIGHP = 334,
     SUPERP = 335,
     PRECISION = 336,
     VERSION_TOK = 337,
     EXTENSION = 338,
     LINE = 339,
     COLON = 340,
     EOL = 341,
     INTERFACE_TOK = 342,
     OUTPUT = 343,
     PRAGMA_DEBUG_ON = 344,
     PRAGMA_DEBUG_OFF = 345,
     PRAGMA_OPTIMIZE_ON = 346,
     PRAGMA_OPTIMIZE_OFF = 347,
     PRAGMA_WARNING_ON = 348,
     PRAGMA_WARNING_OFF = 349,
     PRAGMA_INVARIANT_ALL = 350,
     LAYOUT_TOK = 351,
     DOT_TOK = 352,
     ASM = 353,
     CLASS = 354,
     UNION = 355,
     ENUM = 356,
     TYPEDEF = 357,
     TEMPLATE = 358,
     THIS = 359,
     PACKED_TOK = 360,
     GOTO = 361,
     INLINE_TOK = 362,
     NOINLINE = 363,
     PUBLIC_TOK = 364,
     STATIC = 365,
     EXTERN = 366,
     EXTERNAL = 367,
     LONG_TOK = 368,
     SHORT_TOK = 369,
     HALF = 370,
     FIXED_TOK = 371,
     UNSIGNED = 372,
     INPUT_TOK = 373,
     HVEC2 = 374,
     HVEC3 = 375,
     HVEC4 = 376,
     FVEC2 = 377,
     FVEC3 = 378,
     FVEC4 = 379,
     SAMPLER3DRECT = 380,
     SIZEOF = 381,
     CAST = 382,
     NAMESPACE = 383,
     USING = 384,
     RESOURCE = 385,
     PATCH = 386,
     SUBROUTINE = 387,
     ERROR_TOK = 388,
     COMMON = 389,
     PARTITION = 390,
     ACTIVE = 391,
     FILTER = 392,
     ROW_MAJOR = 393,
     THEN = 394
   };
#endif
/* Tokens.  */
#define ATTRIBUTE 258
#define CONST_TOK 259
#define BASIC_TYPE_TOK 260
#define BREAK 261
#define BUFFER 262
#define CONTINUE 263
#define DO 264
#define ELSE 265
#define FOR 266
#define IF 267
#define DEMOTE 268
#define DISCARD 269
#define RETURN 270
#define SWITCH 271
#define CASE 272
#define DEFAULT 273
#define CENTROID 274
#define IN_TOK 275
#define OUT_TOK 276
#define INOUT_TOK 277
#define UNIFORM 278
#define VARYING 279
#define SAMPLE 280
#define NOPERSPECTIVE 281
#define FLAT 282
#define SMOOTH 283
#define IMAGE1DSHADOW 284
#define IMAGE2DSHADOW 285
#define IMAGE1DARRAYSHADOW 286
#define IMAGE2DARRAYSHADOW 287
#define COHERENT 288
#define VOLATILE 289
#define RESTRICT 290
#define READONLY 291
#define WRITEONLY 292
#define SHARED 293
#define STRUCT 294
#define VOID_TOK 295
#define WHILE 296
#define IDENTIFIER 297
#define TYPE_IDENTIFIER 298
#define NEW_IDENTIFIER 299
#define FLOATCONSTANT 300
#define FLOAT16CONSTANT 301
#define DOUBLECONSTANT 302
#define INTCONSTANT 303
#define UINTCONSTANT 304
#define BOOLCONSTANT 305
#define INT64CONSTANT 306
#define UINT64CONSTANT 307
#define FIELD_SELECTION 308
#define LEFT_OP 309
#define RIGHT_OP 310
#define INC_OP 311
#define DEC_OP 312
#define LE_OP 313
#define GE_OP 314
#define EQ_OP 315
#define NE_OP 316
#define AND_OP 317
#define OR_OP 318
#define XOR_OP 319
#define MUL_ASSIGN 320
#define DIV_ASSIGN 321
#define ADD_ASSIGN 322
#define MOD_ASSIGN 323
#define LEFT_ASSIGN 324
#define RIGHT_ASSIGN 325
#define AND_ASSIGN 326
#define XOR_ASSIGN 327
#define OR_ASSIGN 328
#define SUB_ASSIGN 329
#define INVARIANT 330
#define PRECISE 331
#define LOWP 332
#define MEDIUMP 333
#define HIGHP 334
#define SUPERP 335
#define PRECISION 336
#define VERSION_TOK 337
#define EXTENSION 338
#define LINE 339
#define COLON 340
#define EOL 341
#define INTERFACE_TOK 342
#define OUTPUT 343
#define PRAGMA_DEBUG_ON 344
#define PRAGMA_DEBUG_OFF 345
#define PRAGMA_OPTIMIZE_ON 346
#define PRAGMA_OPTIMIZE_OFF 347
#define PRAGMA_WARNING_ON 348
#define PRAGMA_WARNING_OFF 349
#define PRAGMA_INVARIANT_ALL 350
#define LAYOUT_TOK 351
#define DOT_TOK 352
#define ASM 353
#define CLASS 354
#define UNION 355
#define ENUM 356
#define TYPEDEF 357
#define TEMPLATE 358
#define THIS 359
#define PACKED_TOK 360
#define GOTO 361
#define INLINE_TOK 362
#define NOINLINE 363
#define PUBLIC_TOK 364
#define STATIC 365
#define EXTERN 366
#define EXTERNAL 367
#define LONG_TOK 368
#define SHORT_TOK 369
#define HALF 370
#define FIXED_TOK 371
#define UNSIGNED 372
#define INPUT_TOK 373
#define HVEC2 374
#define HVEC3 375
#define HVEC4 376
#define FVEC2 377
#define FVEC3 378
#define FVEC4 379
#define SAMPLER3DRECT 380
#define SIZEOF 381
#define CAST 382
#define NAMESPACE 383
#define USING 384
#define RESOURCE 385
#define PATCH 386
#define SUBROUTINE 387
#define ERROR_TOK 388
#define COMMON 389
#define PARTITION 390
#define ACTIVE 391
#define FILTER 392
#define ROW_MAJOR 393
#define THEN 394




/* Copy the first part of user declarations.  */
#line 1 "glsl_parser.yy"

/*
 * Copyright © 2008, 2009 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#endif
#include <assert.h>

#include "ast.h"
#include "glsl_parser_extras.h"
#include "compiler/glsl_types.h"
#include "util/u_string.h"
#include "util/format/u_format.h"
#include "main/consts_exts.h"

#ifdef _MSC_VER
#pragma warning( disable : 4065 ) // switch statement contains 'default' but no 'case' labels
#endif

#undef yyerror

static void yyerror(YYLTYPE *loc, _mesa_glsl_parse_state *st, const char *msg)
{
   _mesa_glsl_error(loc, st, "%s", msg);
}

static int
_mesa_glsl_lex(YYSTYPE *val, YYLTYPE *loc, _mesa_glsl_parse_state *state)
{
   return _mesa_glsl_lexer_lex(val, loc, state->scanner);
}

static bool match_layout_qualifier(const char *s1, const char *s2,
                                   _mesa_glsl_parse_state *state)
{
   /* From the GLSL 1.50 spec, section 4.3.8 (Layout Qualifiers):
    *
    *     "The tokens in any layout-qualifier-id-list ... are not case
    *     sensitive, unless explicitly noted otherwise."
    *
    * The text "unless explicitly noted otherwise" appears to be
    * vacuous--no desktop GLSL spec (up through GLSL 4.40) notes
    * otherwise.
    *
    * However, the GLSL ES 3.00 spec says, in section 4.3.8 (Layout
    * Qualifiers):
    *
    *     "As for other identifiers, they are case sensitive."
    *
    * So we need to do a case-sensitive or a case-insensitive match,
    * depending on whether we are compiling for GLSL ES.
    */
   if (state->es_shader)
      return strcmp(s1, s2);
   else
      return strcasecmp(s1, s2);
}


/* Enabling traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif

/* Enabling verbose error messages.  */
#ifdef YYERROR_VERBOSE
# undef YYERROR_VERBOSE
# define YYERROR_VERBOSE 1
#else
# define YYERROR_VERBOSE 1
#endif

/* Enabling the token table.  */
#ifndef YYTOKEN_TABLE
# define YYTOKEN_TABLE 0
#endif

#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef union YYSTYPE
#line 101 "glsl_parser.yy"
{
   int n;
   int64_t n64;
   float real;
   double dreal;
   const char *identifier;

   struct ast_type_qualifier type_qualifier;

   ast_node *node;
   ast_type_specifier *type_specifier;
   ast_array_specifier *array_specifier;
   ast_fully_specified_type *fully_specified_type;
   ast_function *function;
   ast_parameter_declarator *parameter_declarator;
   ast_function_definition *function_definition;
   ast_compound_statement *compound_statement;
   ast_expression *expression;
   ast_declarator_list *declarator_list;
   ast_struct_specifier *struct_specifier;
   ast_declaration *declaration;
   ast_switch_body *switch_body;
   ast_case_label *case_label;
   ast_case_label_list *case_label_list;
   ast_case_statement *case_statement;
   ast_case_statement_list *case_statement_list;
   ast_interface_block *interface_block;
   ast_subroutine_list *subroutine_list;
   struct {
      ast_node *cond;
      ast_expression *rest;
   } for_rest_statement;

   struct {
      ast_node *then_statement;
      ast_node *else_statement;
   } selection_rest_statement;

   const glsl_type *type;
}
/* Line 193 of yacc.c.  */
#line 505 "/tmp/glsl_parser.cpp"
	YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif

#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
} YYLTYPE;
# define yyltype YYLTYPE /* obsolescent; will be withdrawn */
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif


/* Copy the second part of user declarations.  */


/* Line 216 of yacc.c.  */
#line 530 "/tmp/glsl_parser.cpp"

#ifdef short
# undef short
#endif

#ifdef YYTYPE_UINT8
typedef YYTYPE_UINT8 yytype_uint8;
#else
typedef unsigned char yytype_uint8;
#endif

#ifdef YYTYPE_INT8
typedef YYTYPE_INT8 yytype_int8;
#elif (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
typedef signed char yytype_int8;
#else
typedef short int yytype_int8;
#endif

#ifdef YYTYPE_UINT16
typedef YYTYPE_UINT16 yytype_uint16;
#else
typedef unsigned short int yytype_uint16;
#endif

#ifdef YYTYPE_INT16
typedef YYTYPE_INT16 yytype_int16;
#else
typedef short int yytype_int16;
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif ! defined YYSIZE_T && (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned int
# endif
#endif

#define YYSIZE_MAXIMUM ((YYSIZE_T) -1)

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(msgid) dgettext ("bison-runtime", msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(msgid) msgid
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YYUSE(e) ((void) (e))
#else
# define YYUSE(e) /* empty */
#endif

/* Identity function, used to suppress warnings about constant conditions.  */
#ifndef lint
# define YYID(n) (n)
#else
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static int
YYID (int i)
#else
static int
YYID (i)
    int i;
#endif
{
  return i;
}
#endif

#if ! defined yyoverflow || YYERROR_VERBOSE

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined _STDLIB_H && (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#     ifndef _STDLIB_H
#      define _STDLIB_H 1
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's `empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (YYID (0))
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined _STDLIB_H \
       && ! ((defined YYMALLOC || defined malloc) \
	     && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef _STDLIB_H
#    define _STDLIB_H 1
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined _STDLIB_H && (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined _STDLIB_H && (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* ! defined yyoverflow || YYERROR_VERBOSE */


#if (! defined yyoverflow \
     && (! defined __cplusplus \
	 || (defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL \
	     && defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yytype_int16 yyss;
  YYSTYPE yyvs;
    YYLTYPE yyls;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (sizeof (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (sizeof (yytype_int16) + sizeof (YYSTYPE) + sizeof (YYLTYPE)) \
      + 2 * YYSTACK_GAP_MAXIMUM)

/* Copy COUNT objects from FROM to TO.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(To, From, Count) \
      __builtin_memcpy (To, From, (Count) * sizeof (*(From)))
#  else
#   define YYCOPY(To, From, Count)		\
      do					\
	{					\
	  YYSIZE_T yyi;				\
	  for (yyi = 0; yyi < (Count); yyi++)	\
	    (To)[yyi] = (From)[yyi];		\
	}					\
      while (YYID (0))
#  endif
# endif

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack)					\
    do									\
      {									\
	YYSIZE_T yynewbytes;						\
	YYCOPY (&yyptr->Stack, Stack, yysize);				\
	Stack = &yyptr->Stack;						\
	yynewbytes = yystacksize * sizeof (*Stack) + YYSTACK_GAP_MAXIMUM; \
	yyptr += yynewbytes / sizeof (*yyptr);				\
      }									\
    while (YYID (0))

#endif

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  5
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   2623

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  163
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  111
/* YYNRULES -- Number of rules.  */
#define YYNRULES  313
/* YYNRULES -- Number of states.  */
#define YYNSTATES  476

/* YYTRANSLATE(YYLEX) -- Bison symbol number corresponding to YYLEX.  */
#define YYUNDEFTOK  2
#define YYMAXUTOK   394

#define YYTRANSLATE(YYX)						\
  ((unsigned int) (YYX) <= YYMAXUTOK ? yytranslate[YYX] : YYUNDEFTOK)

/* YYTRANSLATE[YYLEX] -- Bison symbol number corresponding to YYLEX.  */
static const yytype_uint8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,   147,     2,     2,     2,   151,   154,     2,
     140,   141,   149,   145,   144,   146,     2,   150,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,   158,   160,
     152,   159,   153,   157,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,   142,     2,   143,   155,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,   161,   156,   162,   148,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73,    74,
      75,    76,    77,    78,    79,    80,    81,    82,    83,    84,
      85,    86,    87,    88,    89,    90,    91,    92,    93,    94,
      95,    96,    97,    98,    99,   100,   101,   102,   103,   104,
     105,   106,   107,   108,   109,   110,   111,   112,   113,   114,
     115,   116,   117,   118,   119,   120,   121,   122,   123,   124,
     125,   126,   127,   128,   129,   130,   131,   132,   133,   134,
     135,   136,   137,   138,   139
};

#if YYDEBUG
/* YYPRHS[YYN] -- Index of the first RHS symbol of rule number YYN in
   YYRHS.  */
static const yytype_uint16 yyprhs[] =
{
       0,     0,     3,     4,     9,    10,    14,    19,    22,    25,
      28,    31,    34,    37,    40,    41,    44,    46,    48,    50,
      56,    58,    61,    64,    66,    68,    70,    72,    74,    76,
      78,    80,    82,    84,    86,    90,    92,    97,    99,   103,
     106,   109,   111,   113,   115,   118,   121,   124,   126,   129,
     133,   136,   138,   140,   142,   145,   148,   151,   153,   155,
     157,   159,   161,   165,   169,   173,   175,   179,   183,   185,
     189,   193,   195,   199,   203,   207,   211,   213,   217,   221,
     223,   227,   229,   233,   235,   239,   241,   245,   247,   251,
     253,   257,   259,   265,   267,   271,   273,   275,   277,   279,
     281,   283,   285,   287,   289,   291,   293,   295,   299,   301,
     304,   307,   312,   314,   317,   319,   321,   324,   328,   332,
     335,   339,   343,   346,   349,   350,   353,   356,   359,   362,
     365,   367,   369,   371,   373,   375,   379,   384,   391,   397,
     399,   402,   406,   412,   417,   420,   423,   425,   428,   433,
     435,   439,   441,   445,   447,   449,   451,   453,   455,   460,
     462,   466,   468,   470,   472,   474,   476,   478,   480,   482,
     484,   486,   488,   490,   493,   496,   499,   502,   505,   508,
     511,   514,   517,   519,   521,   523,   525,   527,   529,   531,
     533,   535,   537,   539,   541,   543,   545,   547,   549,   551,
     554,   558,   562,   567,   569,   572,   574,   576,   578,   580,
     582,   585,   587,   589,   591,   597,   602,   604,   607,   611,
     613,   617,   619,   622,   624,   628,   633,   635,   639,   641,
     643,   645,   647,   649,   651,   653,   655,   657,   659,   662,
     663,   668,   670,   672,   675,   679,   681,   684,   687,   689,
     692,   698,   702,   704,   706,   711,   717,   720,   724,   728,
     731,   733,   736,   739,   742,   744,   747,   753,   761,   768,
     770,   772,   774,   775,   778,   782,   785,   788,   791,   795,
     798,   801,   803,   805,   807,   809,   811,   814,   816,   819,
     822,   830,   832,   834,   836,   838,   841,   842,   844,   847,
     849,   852,   856,   859,   863,   866,   870,   873,   877,   880,
     884,   886,   888,   890
};

/* YYRHS -- A `-1'-separated list of the rules' RHS.  */
static const yytype_int16 yyrhs[] =
{
     164,     0,    -1,    -1,   166,   168,   165,   171,    -1,    -1,
      82,    48,    86,    -1,    82,    48,   169,    86,    -1,    89,
      86,    -1,    90,    86,    -1,    91,    86,    -1,    92,    86,
      -1,    95,    86,    -1,    93,    86,    -1,    94,    86,    -1,
      -1,   168,   170,    -1,    42,    -1,    43,    -1,    44,    -1,
      83,   169,    85,   169,    86,    -1,   261,    -1,   171,   261,
      -1,   171,   170,    -1,    42,    -1,    44,    -1,   172,    -1,
      48,    -1,    49,    -1,    51,    -1,    52,    -1,    46,    -1,
      45,    -1,    47,    -1,    50,    -1,   140,   199,   141,    -1,
     173,    -1,   174,   142,   175,   143,    -1,   176,    -1,   174,
      97,    53,    -1,   174,    56,    -1,   174,    57,    -1,   199,
      -1,   177,    -1,   178,    -1,   180,   141,    -1,   179,   141,
      -1,   181,    40,    -1,   181,    -1,   181,   197,    -1,   180,
     144,   197,    -1,   182,   140,    -1,   226,    -1,   174,    -1,
     174,    -1,    56,   183,    -1,    57,   183,    -1,   184,   183,
      -1,   145,    -1,   146,    -1,   147,    -1,   148,    -1,   183,
      -1,   185,   149,   183,    -1,   185,   150,   183,    -1,   185,
     151,   183,    -1,   185,    -1,   186,   145,   185,    -1,   186,
     146,   185,    -1,   186,    -1,   187,    54,   186,    -1,   187,
      55,   186,    -1,   187,    -1,   188,   152,   187,    -1,   188,
     153,   187,    -1,   188,    58,   187,    -1,   188,    59,   187,
      -1,   188,    -1,   189,    60,   188,    -1,   189,    61,   188,
      -1,   189,    -1,   190,   154,   189,    -1,   190,    -1,   191,
     155,   190,    -1,   191,    -1,   192,   156,   191,    -1,   192,
      -1,   193,    62,   192,    -1,   193,    -1,   194,    64,   193,
      -1,   194,    -1,   195,    63,   194,    -1,   195,    -1,   195,
     157,   199,   158,   197,    -1,   196,    -1,   183,   198,   197,
      -1,   159,    -1,    65,    -1,    66,    -1,    68,    -1,    67,
      -1,    74,    -1,    69,    -1,    70,    -1,    71,    -1,    72,
      -1,    73,    -1,   197,    -1,   199,   144,   197,    -1,   196,
      -1,   202,   160,    -1,   211,   160,    -1,    81,   229,   226,
     160,    -1,   263,    -1,   203,   141,    -1,   205,    -1,   204,
      -1,   205,   207,    -1,   204,   144,   207,    -1,   213,   172,
     140,    -1,   226,   169,    -1,   214,   226,   169,    -1,   226,
     169,   225,    -1,   208,   206,    -1,   208,   210,    -1,    -1,
       4,   208,    -1,    76,   208,    -1,   209,   208,    -1,   229,
     208,    -1,   224,   208,    -1,    20,    -1,    21,    -1,    22,
      -1,   226,    -1,   212,    -1,   211,   144,   169,    -1,   211,
     144,   169,   225,    -1,   211,   144,   169,   225,   159,   235,
      -1,   211,   144,   169,   159,   235,    -1,   213,    -1,   213,
     169,    -1,   213,   169,   225,    -1,   213,   169,   225,   159,
     235,    -1,   213,   169,   159,   235,    -1,    75,   172,    -1,
      76,   172,    -1,   226,    -1,   221,   226,    -1,    96,   140,
     215,   141,    -1,   216,    -1,   215,   144,   216,    -1,   169,
      -1,   169,   159,   200,    -1,   217,    -1,   138,    -1,   105,
      -1,    38,    -1,   132,    -1,   132,   140,   219,   141,    -1,
     169,    -1,   219,   144,   169,    -1,    28,    -1,    27,    -1,
      26,    -1,    75,    -1,    76,    -1,   222,    -1,   223,    -1,
     220,    -1,   214,    -1,   224,    -1,   218,    -1,   229,    -1,
      76,   221,    -1,    75,   221,    -1,   220,   221,    -1,   214,
     221,    -1,   218,   221,    -1,   222,   221,    -1,   223,   221,
      -1,   229,   221,    -1,   224,   221,    -1,    19,    -1,    25,
      -1,   131,    -1,     4,    -1,     3,    -1,    24,    -1,    20,
      -1,    21,    -1,    22,    -1,    23,    -1,     7,    -1,    38,
      -1,    33,    -1,    34,    -1,    35,    -1,    36,    -1,    37,
      -1,   142,   143,    -1,   142,   200,   143,    -1,   225,   142,
     143,    -1,   225,   142,   200,   143,    -1,   227,    -1,   227,
     225,    -1,   228,    -1,   230,    -1,    43,    -1,    40,    -1,
       5,    -1,   117,     5,    -1,    79,    -1,    78,    -1,    77,
      -1,    39,   169,   161,   231,   162,    -1,    39,   161,   231,
     162,    -1,   232,    -1,   231,   232,    -1,   213,   233,   160,
      -1,   234,    -1,   233,   144,   234,    -1,   169,    -1,   169,
     225,    -1,   197,    -1,   161,   236,   162,    -1,   161,   236,
     144,   162,    -1,   235,    -1,   236,   144,   235,    -1,   201,
      -1,   240,    -1,   239,    -1,   237,    -1,   245,    -1,   246,
      -1,   249,    -1,   255,    -1,   259,    -1,   260,    -1,   161,
     162,    -1,    -1,   161,   241,   244,   162,    -1,   243,    -1,
     239,    -1,   161,   162,    -1,   161,   244,   162,    -1,   238,
      -1,   244,   238,    -1,   244,   170,    -1,   160,    -1,   199,
     160,    -1,    12,   140,   199,   141,   247,    -1,   238,    10,
     238,    -1,   238,    -1,   199,    -1,   213,   169,   159,   235,
      -1,    16,   140,   199,   141,   250,    -1,   161,   162,    -1,
     161,   254,   162,    -1,    17,   199,   158,    -1,    18,   158,
      -1,   251,    -1,   252,   251,    -1,   252,   238,    -1,   253,
     238,    -1,   253,    -1,   254,   253,    -1,    41,   140,   248,
     141,   242,    -1,     9,   242,    41,   140,   199,   141,   160,
      -1,    11,   140,   256,   258,   141,   242,    -1,   245,    -1,
     237,    -1,   248,    -1,    -1,   257,   160,    -1,   257,   160,
     199,    -1,     8,   160,    -1,     6,   160,    -1,    15,   160,
      -1,    15,   199,   160,    -1,    14,   160,    -1,    13,   160,
      -1,   262,    -1,   201,    -1,   167,    -1,   273,    -1,   160,
      -1,   202,   243,    -1,   264,    -1,   214,   263,    -1,   224,
     263,    -1,   265,    44,   161,   267,   162,   266,   160,    -1,
      20,    -1,    21,    -1,    23,    -1,     7,    -1,   222,   265,
      -1,    -1,    44,    -1,    44,   225,    -1,   268,    -1,   268,
     267,    -1,   213,   233,   160,    -1,   214,   269,    -1,   214,
      23,   160,    -1,   214,   270,    -1,   214,     7,   160,    -1,
     214,   271,    -1,   214,    20,   160,    -1,   214,   272,    -1,
     214,    21,   160,    -1,   269,    -1,   270,    -1,   271,    -1,
     272,    -1
};

/* YYRLINE[YYN] -- source line where rule number YYN was defined.  */
static const yytype_uint16 yyrline[] =
{
       0,   295,   295,   294,   318,   320,   327,   337,   338,   339,
     340,   341,   365,   370,   377,   379,   383,   384,   385,   389,
     398,   406,   414,   425,   426,   430,   437,   444,   451,   458,
     465,   472,   479,   486,   493,   500,   501,   507,   511,   518,
     524,   533,   537,   541,   545,   546,   550,   551,   555,   561,
     573,   577,   583,   597,   598,   604,   610,   620,   621,   622,
     623,   627,   628,   634,   640,   649,   650,   656,   665,   666,
     672,   681,   682,   688,   694,   700,   709,   710,   716,   725,
     726,   735,   736,   745,   746,   755,   756,   765,   766,   775,
     776,   785,   786,   795,   796,   805,   806,   807,   808,   809,
     810,   811,   812,   813,   814,   815,   819,   823,   839,   843,
     848,   852,   857,   874,   878,   879,   883,   888,   896,   914,
     925,   930,   945,   953,   970,   973,   981,   989,  1001,  1013,
    1020,  1025,  1030,  1039,  1043,  1044,  1054,  1064,  1074,  1088,
    1095,  1106,  1117,  1128,  1139,  1151,  1166,  1173,  1191,  1198,
    1199,  1209,  1738,  1908,  1934,  1939,  1944,  1952,  1957,  1966,
    1975,  1987,  1992,  1997,  2006,  2011,  2016,  2017,  2018,  2019,
    2020,  2021,  2022,  2040,  2048,  2073,  2097,  2111,  2116,  2132,
    2157,  2169,  2177,  2182,  2187,  2194,  2199,  2204,  2209,  2214,
    2239,  2251,  2256,  2261,  2269,  2274,  2279,  2285,  2290,  2298,
    2306,  2312,  2322,  2333,  2334,  2342,  2348,  2354,  2363,  2364,
    2365,  2377,  2382,  2387,  2395,  2402,  2419,  2424,  2432,  2470,
    2475,  2483,  2489,  2498,  2499,  2503,  2510,  2517,  2524,  2530,
    2531,  2535,  2536,  2537,  2538,  2539,  2540,  2541,  2545,  2552,
    2551,  2565,  2566,  2570,  2576,  2585,  2595,  2604,  2616,  2622,
    2631,  2640,  2645,  2653,  2657,  2675,  2683,  2688,  2696,  2701,
    2709,  2717,  2725,  2733,  2741,  2749,  2757,  2764,  2771,  2781,
    2782,  2786,  2788,  2794,  2799,  2808,  2814,  2820,  2826,  2832,
    2841,  2850,  2851,  2852,  2853,  2854,  2858,  2872,  2876,  2889,
    2907,  2926,  2931,  2936,  2941,  2946,  2961,  2964,  2969,  2977,
    2982,  2990,  3014,  3021,  3025,  3032,  3036,  3046,  3055,  3065,
    3074,  3086,  3108,  3118
};
#endif

#if YYDEBUG || YYERROR_VERBOSE || YYTOKEN_TABLE
/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "$end", "error", "$undefined", "ATTRIBUTE", "CONST_TOK",
  "BASIC_TYPE_TOK", "BREAK", "BUFFER", "CONTINUE", "DO", "ELSE", "FOR",
  "IF", "DEMOTE", "DISCARD", "RETURN", "SWITCH", "CASE", "DEFAULT",
  "CENTROID", "IN_TOK", "OUT_TOK", "INOUT_TOK", "UNIFORM", "VARYING",
  "SAMPLE", "NOPERSPECTIVE", "FLAT", "SMOOTH", "IMAGE1DSHADOW",
  "IMAGE2DSHADOW", "IMAGE1DARRAYSHADOW", "IMAGE2DARRAYSHADOW", "COHERENT",
  "VOLATILE", "RESTRICT", "READONLY", "WRITEONLY", "SHARED", "STRUCT",
  "VOID_TOK", "WHILE", "IDENTIFIER", "TYPE_IDENTIFIER", "NEW_IDENTIFIER",
  "FLOATCONSTANT", "FLOAT16CONSTANT", "DOUBLECONSTANT", "INTCONSTANT",
  "UINTCONSTANT", "BOOLCONSTANT", "INT64CONSTANT", "UINT64CONSTANT",
  "FIELD_SELECTION", "LEFT_OP", "RIGHT_OP", "INC_OP", "DEC_OP", "LE_OP",
  "GE_OP", "EQ_OP", "NE_OP", "AND_OP", "OR_OP", "XOR_OP", "MUL_ASSIGN",
  "DIV_ASSIGN", "ADD_ASSIGN", "MOD_ASSIGN", "LEFT_ASSIGN", "RIGHT_ASSIGN",
  "AND_ASSIGN", "XOR_ASSIGN", "OR_ASSIGN", "SUB_ASSIGN", "INVARIANT",
  "PRECISE", "LOWP", "MEDIUMP", "HIGHP", "SUPERP", "PRECISION",
  "VERSION_TOK", "EXTENSION", "LINE", "COLON", "EOL", "INTERFACE_TOK",
  "OUTPUT", "PRAGMA_DEBUG_ON", "PRAGMA_DEBUG_OFF", "PRAGMA_OPTIMIZE_ON",
  "PRAGMA_OPTIMIZE_OFF", "PRAGMA_WARNING_ON", "PRAGMA_WARNING_OFF",
  "PRAGMA_INVARIANT_ALL", "LAYOUT_TOK", "DOT_TOK", "ASM", "CLASS", "UNION",
  "ENUM", "TYPEDEF", "TEMPLATE", "THIS", "PACKED_TOK", "GOTO",
  "INLINE_TOK", "NOINLINE", "PUBLIC_TOK", "STATIC", "EXTERN", "EXTERNAL",
  "LONG_TOK", "SHORT_TOK", "HALF", "FIXED_TOK", "UNSIGNED", "INPUT_TOK",
  "HVEC2", "HVEC3", "HVEC4", "FVEC2", "FVEC3", "FVEC4", "SAMPLER3DRECT",
  "SIZEOF", "CAST", "NAMESPACE", "USING", "RESOURCE", "PATCH",
  "SUBROUTINE", "ERROR_TOK", "COMMON", "PARTITION", "ACTIVE", "FILTER",
  "ROW_MAJOR", "THEN", "'('", "')'", "'['", "']'", "','", "'+'", "'-'",
  "'!'", "'~'", "'*'", "'/'", "'%'", "'<'", "'>'", "'&'", "'^'", "'|'",
  "'?'", "':'", "'='", "';'", "'{'", "'}'", "$accept", "translation_unit",
  "@1", "version_statement", "pragma_statement",
  "extension_statement_list", "any_identifier", "extension_statement",
  "external_declaration_list", "variable_identifier", "primary_expression",
  "postfix_expression", "integer_expression", "function_call",
  "function_call_or_method", "function_call_generic",
  "function_call_header_no_parameters",
  "function_call_header_with_parameters", "function_call_header",
  "function_identifier", "unary_expression", "unary_operator",
  "multiplicative_expression", "additive_expression", "shift_expression",
  "relational_expression", "equality_expression", "and_expression",
  "exclusive_or_expression", "inclusive_or_expression",
  "logical_and_expression", "logical_xor_expression",
  "logical_or_expression", "conditional_expression",
  "assignment_expression", "assignment_operator", "expression",
  "constant_expression", "declaration", "function_prototype",
  "function_declarator", "function_header_with_parameters",
  "function_header", "parameter_declarator", "parameter_declaration",
  "parameter_qualifier", "parameter_direction_qualifier",
  "parameter_type_specifier", "init_declarator_list", "single_declaration",
  "fully_specified_type", "layout_qualifier", "layout_qualifier_id_list",
  "layout_qualifier_id", "interface_block_layout_qualifier",
  "subroutine_qualifier", "subroutine_type_list",
  "interpolation_qualifier", "type_qualifier",
  "auxiliary_storage_qualifier", "storage_qualifier", "memory_qualifier",
  "array_specifier", "type_specifier", "type_specifier_nonarray",
  "basic_type_specifier_nonarray", "precision_qualifier",
  "struct_specifier", "struct_declaration_list", "struct_declaration",
  "struct_declarator_list", "struct_declarator", "initializer",
  "initializer_list", "declaration_statement", "statement",
  "simple_statement", "compound_statement", "@2", "statement_no_new_scope",
  "compound_statement_no_new_scope", "statement_list",
  "expression_statement", "selection_statement",
  "selection_rest_statement", "condition", "switch_statement",
  "switch_body", "case_label", "case_label_list", "case_statement",
  "case_statement_list", "iteration_statement", "for_init_statement",
  "conditionopt", "for_rest_statement", "jump_statement",
  "demote_statement", "external_declaration", "function_definition",
  "interface_block", "basic_interface_block", "interface_qualifier",
  "instance_name_opt", "member_list", "member_declaration",
  "layout_uniform_defaults", "layout_buffer_defaults",
  "layout_in_defaults", "layout_out_defaults", "layout_defaults", 0
};
#endif

# ifdef YYPRINT
/* YYTOKNUM[YYLEX-NUM] -- Internal token number corresponding to
   token YYLEX-NUM.  */
static const yytype_uint16 yytoknum[] =
{
       0,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   267,   268,   269,   270,   271,   272,   273,   274,
     275,   276,   277,   278,   279,   280,   281,   282,   283,   284,
     285,   286,   287,   288,   289,   290,   291,   292,   293,   294,
     295,   296,   297,   298,   299,   300,   301,   302,   303,   304,
     305,   306,   307,   308,   309,   310,   311,   312,   313,   314,
     315,   316,   317,   318,   319,   320,   321,   322,   323,   324,
     325,   326,   327,   328,   329,   330,   331,   332,   333,   334,
     335,   336,   337,   338,   339,   340,   341,   342,   343,   344,
     345,   346,   347,   348,   349,   350,   351,   352,   353,   354,
     355,   356,   357,   358,   359,   360,   361,   362,   363,   364,
     365,   366,   367,   368,   369,   370,   371,   372,   373,   374,
     375,   376,   377,   378,   379,   380,   381,   382,   383,   384,
     385,   386,   387,   388,   389,   390,   391,   392,   393,   394,
      40,    41,    91,    93,    44,    43,    45,    33,   126,    42,
      47,    37,    60,    62,    38,    94,   124,    63,    58,    61,
      59,   123,   125
};
# endif

/* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_uint16 yyr1[] =
{
       0,   163,   165,   164,   166,   166,   166,   167,   167,   167,
     167,   167,   167,   167,   168,   168,   169,   169,   169,   170,
     171,   171,   171,   172,   172,   173,   173,   173,   173,   173,
     173,   173,   173,   173,   173,   174,   174,   174,   174,   174,
     174,   175,   176,   177,   178,   178,   179,   179,   180,   180,
     181,   182,   182,   183,   183,   183,   183,   184,   184,   184,
     184,   185,   185,   185,   185,   186,   186,   186,   187,   187,
     187,   188,   188,   188,   188,   188,   189,   189,   189,   190,
     190,   191,   191,   192,   192,   193,   193,   194,   194,   195,
     195,   196,   196,   197,   197,   198,   198,   198,   198,   198,
     198,   198,   198,   198,   198,   198,   199,   199,   200,   201,
     201,   201,   201,   202,   203,   203,   204,   204,   205,   206,
     206,   206,   207,   207,   208,   208,   208,   208,   208,   208,
     209,   209,   209,   210,   211,   211,   211,   211,   211,   212,
     212,   212,   212,   212,   212,   212,   213,   213,   214,   215,
     215,   216,   216,   216,   217,   217,   217,   218,   218,   219,
     219,   220,   220,   220,   221,   221,   221,   221,   221,   221,
     221,   221,   221,   221,   221,   221,   221,   221,   221,   221,
     221,   221,   222,   222,   222,   223,   223,   223,   223,   223,
     223,   223,   223,   223,   224,   224,   224,   224,   224,   225,
     225,   225,   225,   226,   226,   227,   227,   227,   228,   228,
     228,   229,   229,   229,   230,   230,   231,   231,   232,   233,
     233,   234,   234,   235,   235,   235,   236,   236,   237,   238,
     238,   239,   239,   239,   239,   239,   239,   239,   240,   241,
     240,   242,   242,   243,   243,   244,   244,   244,   245,   245,
     246,   247,   247,   248,   248,   249,   250,   250,   251,   251,
     252,   252,   253,   253,   254,   254,   255,   255,   255,   256,
     256,   257,   257,   258,   258,   259,   259,   259,   259,   259,
     260,   261,   261,   261,   261,   261,   262,   263,   263,   263,
     264,   265,   265,   265,   265,   265,   266,   266,   266,   267,
     267,   268,   269,   269,   270,   270,   271,   271,   272,   272,
     273,   273,   273,   273
};

/* YYR2[YYN] -- Number of symbols composing right hand side of rule YYN.  */
static const yytype_uint8 yyr2[] =
{
       0,     2,     0,     4,     0,     3,     4,     2,     2,     2,
       2,     2,     2,     2,     0,     2,     1,     1,     1,     5,
       1,     2,     2,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     3,     1,     4,     1,     3,     2,
       2,     1,     1,     1,     2,     2,     2,     1,     2,     3,
       2,     1,     1,     1,     2,     2,     2,     1,     1,     1,
       1,     1,     3,     3,     3,     1,     3,     3,     1,     3,
       3,     1,     3,     3,     3,     3,     1,     3,     3,     1,
       3,     1,     3,     1,     3,     1,     3,     1,     3,     1,
       3,     1,     5,     1,     3,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     3,     1,     2,
       2,     4,     1,     2,     1,     1,     2,     3,     3,     2,
       3,     3,     2,     2,     0,     2,     2,     2,     2,     2,
       1,     1,     1,     1,     1,     3,     4,     6,     5,     1,
       2,     3,     5,     4,     2,     2,     1,     2,     4,     1,
       3,     1,     3,     1,     1,     1,     1,     1,     4,     1,
       3,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     2,
       3,     3,     4,     1,     2,     1,     1,     1,     1,     1,
       2,     1,     1,     1,     5,     4,     1,     2,     3,     1,
       3,     1,     2,     1,     3,     4,     1,     3,     1,     1,
       1,     1,     1,     1,     1,     1,     1,     1,     2,     0,
       4,     1,     1,     2,     3,     1,     2,     2,     1,     2,
       5,     3,     1,     1,     4,     5,     2,     3,     3,     2,
       1,     2,     2,     2,     1,     2,     5,     7,     6,     1,
       1,     1,     0,     2,     3,     2,     2,     2,     3,     2,
       2,     1,     1,     1,     1,     1,     2,     1,     2,     2,
       7,     1,     1,     1,     1,     2,     0,     1,     2,     1,
       2,     3,     2,     3,     2,     3,     2,     3,     2,     3,
       1,     1,     1,     1
};

/* YYDEFACT[STATE-NAME] -- Default rule to reduce with in state
   STATE-NUM when YYTABLE doesn't specify something else to do.  Zero
   means the default is an error.  */
static const yytype_uint16 yydefact[] =
{
       4,     0,     0,    14,     0,     1,     2,    16,    17,    18,
       5,     0,     0,     0,    15,     6,     0,   186,   185,   209,
     192,   182,   188,   189,   190,   191,   187,   183,   163,   162,
     161,   194,   195,   196,   197,   198,   193,     0,   208,   207,
     164,   165,   213,   212,   211,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,   184,   157,   285,   283,     3,
     282,     0,     0,   115,   124,     0,   134,   139,   169,   171,
     168,     0,   166,   167,   170,   146,   203,   205,   172,   206,
      20,   281,   112,   287,     0,   310,   311,   312,   313,   284,
       0,     0,     0,   192,   188,   189,   191,    23,    24,   164,
     165,   144,   169,   174,   166,   170,   145,   173,     0,     7,
       8,     9,    10,    12,    13,    11,     0,   210,     0,    22,
      21,   109,     0,   286,   113,   124,   124,   130,   131,   132,
     124,   116,     0,   124,   124,   124,     0,   110,    16,    18,
     140,     0,   192,   188,   189,   191,   176,   288,   302,   304,
     306,   308,   177,   175,   147,   178,   295,   179,   169,   181,
     289,     0,   204,   180,     0,     0,     0,     0,   216,     0,
       0,   156,   155,   154,   151,     0,   149,   153,   159,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      31,    30,    32,    26,    27,    33,    28,    29,     0,     0,
       0,    57,    58,    59,    60,   248,   239,   243,    25,    35,
      53,    37,    42,    43,     0,     0,    47,     0,    61,     0,
      65,    68,    71,    76,    79,    81,    83,    85,    87,    89,
      91,    93,   106,     0,   228,     0,   146,   231,   245,   230,
     229,     0,   232,   233,   234,   235,   236,   237,   117,   125,
     126,   122,   123,     0,   133,   127,   129,   128,   135,     0,
     141,   118,   305,   307,   309,   303,   199,    61,   108,     0,
      51,     0,     0,    19,   221,     0,   219,   215,   217,     0,
     111,     0,   148,     0,   158,     0,   276,   275,   242,     0,
     241,     0,     0,   280,   279,   277,     0,     0,     0,    54,
      55,     0,   238,     0,    39,    40,     0,     0,    45,    44,
       0,   208,    48,    50,    96,    97,    99,    98,   101,   102,
     103,   104,   105,   100,    95,     0,    56,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,   249,   244,
     247,   246,     0,   119,     0,   136,     0,   223,   143,     0,
     200,   201,     0,     0,     0,   299,   222,     0,   218,   214,
     152,   150,   160,     0,   270,   269,   272,     0,   278,     0,
     253,     0,     0,    34,     0,    38,     0,    41,    49,    94,
      62,    63,    64,    66,    67,    69,    70,    74,    75,    72,
      73,    77,    78,    80,    82,    84,    86,    88,    90,     0,
     107,   120,   121,   138,     0,   226,     0,   142,   202,     0,
     296,   300,   220,     0,   271,     0,     0,     0,     0,     0,
       0,   240,    36,     0,   137,     0,   224,   301,   297,     0,
       0,   273,     0,   252,   250,     0,   255,     0,   266,    92,
     225,   227,   298,   290,     0,   274,   268,     0,     0,     0,
     256,   260,     0,   264,     0,   254,   267,   251,     0,   259,
     262,   261,   263,   257,   265,   258
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
      -1,     2,    13,     3,    58,     6,   274,   350,    59,   208,
     209,   210,   386,   211,   212,   213,   214,   215,   216,   217,
     218,   219,   220,   221,   222,   223,   224,   225,   226,   227,
     228,   229,   230,   231,   232,   325,   233,   269,   234,   235,
      62,    63,    64,   251,   131,   132,   133,   252,    65,    66,
      67,   102,   175,   176,   177,    69,   179,    70,    71,    72,
      73,   105,   162,   270,    76,    77,    78,    79,   167,   168,
     275,   276,   358,   416,   237,   238,   239,   240,   303,   289,
     290,   241,   242,   243,   444,   382,   244,   446,   461,   462,
     463,   464,   245,   376,   425,   426,   246,   247,    80,    81,
      82,    83,    84,   439,   364,   365,    85,    86,    87,    88,
      89
};

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
#define YYPACT_NINF -329
static const yytype_int16 yypact[] =
{
     -44,    -6,    44,  -329,   143,  -329,    39,  -329,  -329,  -329,
    -329,    21,   181,  1770,  -329,  -329,    52,  -329,  -329,  -329,
     109,  -329,   123,   128,  -329,   140,  -329,  -329,  -329,  -329,
    -329,  -329,  -329,  -329,  -329,  -329,  -329,   -23,  -329,  -329,
    2297,  2297,  -329,  -329,  -329,   167,   104,   108,   112,   119,
     134,   147,   161,    95,   247,  -329,   121,  -329,  -329,  1670,
    -329,    57,   126,   125,   321,  -111,  -329,   207,  2361,  2427,
    2427,    31,  2491,  2427,  2491,  -329,   137,  -329,  2427,  -329,
    -329,  -329,  -329,  -329,   232,  -329,  -329,  -329,  -329,  -329,
     181,  2219,   120,  -329,  -329,  -329,  -329,  -329,  -329,  2427,
    2427,  -329,  2427,  -329,  2427,  2427,  -329,  -329,    31,  -329,
    -329,  -329,  -329,  -329,  -329,  -329,    55,  -329,   181,  -329,
    -329,  -329,   812,  -329,  -329,   390,   390,  -329,  -329,  -329,
     390,  -329,    51,   390,   390,   390,   181,  -329,   144,   148,
     -54,   153,   -21,   -20,   -19,   -16,  -329,  -329,  -329,  -329,
    -329,  -329,  -329,  -329,  -329,  -329,  -329,  -329,  2491,  -329,
    -329,  2013,   138,  -329,   122,   201,   181,   942,  -329,  2219,
     136,  -329,  -329,  -329,   139,   -33,  -329,  -329,  -329,    30,
     141,   142,  1295,   160,   165,   146,   151,  1828,   168,   169,
    -329,  -329,  -329,  -329,  -329,  -329,  -329,  -329,  2089,  2089,
    2089,  -329,  -329,  -329,  -329,  -329,   164,  -329,  -329,  -329,
      79,  -329,  -329,  -329,   188,    58,  2144,   190,   248,  2089,
     105,    81,   184,   -52,   197,   180,   182,   179,   274,   275,
     -47,  -329,  -329,   -92,  -329,   185,   200,  -329,  -329,  -329,
    -329,   492,  -329,  -329,  -329,  -329,  -329,  -329,  -329,  -329,
    -329,  -329,  -329,    31,   181,  -329,  -329,  -329,   -53,  1775,
     -39,  -329,  -329,  -329,  -329,  -329,  -329,  -329,  -329,   203,
    -329,  2068,  2219,  -329,   137,   -87,  -329,  -329,  -329,  1006,
    -329,  2089,  -329,    55,  -329,   181,  -329,  -329,  -329,   303,
    -329,  1584,  2089,  -329,  -329,  -329,   -68,  2089,  1958,  -329,
    -329,    63,  -329,  1454,  -329,  -329,   296,  2089,  -329,  -329,
    2089,   209,  -329,  -329,  -329,  -329,  -329,  -329,  -329,  -329,
    -329,  -329,  -329,  -329,  -329,  2089,  -329,  2089,  2089,  2089,
    2089,  2089,  2089,  2089,  2089,  2089,  2089,  2089,  2089,  2089,
    2089,  2089,  2089,  2089,  2089,  2089,  2089,  2089,  -329,  -329,
    -329,  -329,   181,   137,  1775,     3,  1775,  -329,  -329,  1775,
    -329,  -329,   208,   181,   191,  2219,   138,   181,  -329,  -329,
    -329,  -329,  -329,   219,  -329,  -329,  1958,    65,  -329,    70,
     216,   181,   220,  -329,   652,  -329,   221,   216,  -329,  -329,
    -329,  -329,  -329,   105,   105,    81,    81,   184,   184,   184,
     184,   -52,   -52,   197,   180,   182,   179,   274,   275,  -115,
    -329,  -329,   138,  -329,  1775,  -329,  -117,  -329,  -329,   -48,
     318,  -329,  -329,  2089,  -329,   205,   226,  1454,   211,   210,
    1295,  -329,  -329,  2089,  -329,   948,  -329,  -329,   137,   213,
      72,  2089,  1295,   360,  -329,    -7,  -329,  1775,  -329,  -329,
    -329,  -329,   138,  -329,   214,   216,  -329,  1454,  2089,   218,
    -329,  -329,  1136,  1454,    -5,  -329,  -329,  -329,  -110,  -329,
    -329,  -329,  -329,  -329,  1454,  -329
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -329,  -329,  -329,  -329,  -329,  -329,    14,    24,  -329,   124,
    -329,  -329,  -329,  -329,  -329,  -329,  -329,  -329,  -329,  -329,
     149,  -329,   -67,   -61,  -157,   -65,    37,    38,    36,    40,
      42,    35,  -329,  -153,  -144,  -329,  -146,  -234,     4,    26,
    -329,  -329,  -329,  -329,   256,    62,  -329,  -329,  -329,  -329,
     -90,     1,  -329,    99,  -329,  -329,  -329,  -329,   511,   -38,
    -329,    -9,  -131,   -13,  -329,  -329,   198,  -329,   215,  -145,
      25,    20,  -272,  -329,    98,  -226,  -177,  -329,  -329,  -328,
     329,    88,   101,  -329,  -329,    17,  -329,  -329,   -66,  -329,
     -63,  -329,  -329,  -329,  -329,  -329,  -329,  -329,   343,  -329,
      12,  -329,   331,  -329,    41,  -329,   336,   337,   340,   341,
    -329
};

/* YYTABLE[YYPACT[STATE-NUM]].  What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule which
   number is the opposite.  If zero, do what YYDEFACT says.
   If YYTABLE_NINF, syntax error.  */
#define YYTABLE_NINF -295
static const yytype_int16 yytable[] =
{
      75,   166,   104,   104,    74,   288,   334,   335,   268,   260,
     458,   459,   458,   459,    68,   351,   345,    60,    11,     7,
       8,     9,   278,  -294,  -291,  -292,    16,   435,  -293,   347,
      14,   104,   104,   136,   347,   104,    19,   362,     1,    61,
     104,   296,     4,   433,     5,   436,    75,   370,   475,   137,
      74,    92,   347,   104,   301,   134,    19,   367,   154,    74,
      68,   104,   104,    60,   104,    74,   104,   104,   348,    68,
      37,    38,   312,   368,    39,   158,   347,   166,    75,   166,
     147,   140,   413,   119,   415,    61,   160,   417,   161,   161,
      37,    38,   378,   171,    39,   170,   367,     7,     8,     9,
     336,   337,   448,   271,   165,   259,   354,    15,   282,   236,
     346,   283,   437,    74,   456,   357,   134,   134,   268,   254,
     359,   134,    12,   158,   134,   134,   134,   355,   268,   104,
     174,   104,   178,   253,   278,   304,   305,    90,    91,   262,
     263,   264,   434,   366,   265,   271,   377,    53,    54,    74,
     258,   379,   380,  -294,    75,   460,    75,   473,   351,   158,
     172,   387,   414,   451,   101,   106,   388,  -291,    54,   236,
     147,   284,  -292,    74,   285,   465,   306,   397,   398,   399,
     400,   389,   363,   158,  -293,     7,     8,     9,   249,   166,
     109,   141,   250,   173,   110,   255,   256,   257,   111,   309,
     409,   443,   310,   410,   383,   112,   427,   347,   381,   347,
     357,   428,   357,   454,   347,   357,   347,   121,   122,   -52,
     113,   307,   412,     7,     8,     9,   330,   331,   236,    10,
     380,   467,    74,   114,   104,   116,   470,   472,   332,   333,
     352,   104,   158,   108,    42,    43,    44,   115,   472,   138,
       8,   139,   117,   288,   327,   328,   329,   338,   339,    75,
     104,   118,   135,   393,   394,   288,    75,   124,   353,   125,
     357,   395,   396,   401,   402,   363,   164,   440,   236,   161,
     271,   169,    74,   272,   -23,   236,   381,   273,   -24,   449,
     236,   357,   158,   261,    74,   455,   280,   174,   281,   372,
     291,   286,   287,   357,   158,   292,   293,   452,   297,   298,
     267,   294,   468,   314,   315,   316,   317,   318,   319,   320,
     321,   322,   323,   135,   135,   126,   302,   104,   135,   308,
     313,   135,   135,   135,   340,   342,   343,   341,   104,   344,
     -51,   127,   128,   129,   373,   121,   360,   299,   300,   385,
     -46,   418,    75,   420,    31,    32,    33,    34,    35,   423,
     347,   430,   438,   236,   432,   441,   411,   442,   326,   447,
     457,   236,   445,   453,   466,    74,   469,   403,   405,   404,
     408,   248,   371,   406,   279,   158,   407,   422,   419,   374,
     123,   384,   375,   424,   126,   429,   471,   130,    42,    43,
      44,   474,   120,   156,   148,   149,   421,   324,   150,   151,
     127,   128,   129,     0,   236,     0,     0,   236,    74,     0,
     267,    74,     0,    31,    32,    33,    34,    35,   158,   236,
     267,   158,     0,    74,     0,     0,     0,     0,     0,     0,
       0,     0,     0,   158,   236,     0,     0,     0,    74,   236,
     236,     0,     0,    74,    74,     0,     0,     0,   158,     0,
       0,   236,  -114,   158,   158,    74,   130,    42,    43,    44,
       0,     0,     0,     0,     0,   158,   390,   391,   392,   267,
     267,   267,   267,   267,   267,   267,   267,   267,   267,   267,
     267,   267,   267,   267,   267,    17,    18,    19,   180,    20,
     181,   182,     0,   183,   184,   185,   186,   187,   188,     0,
       0,    21,    22,    23,    24,    25,    26,    27,    28,    29,
      30,     0,     0,     0,     0,    31,    32,    33,    34,    35,
      36,    37,    38,   189,    97,    39,    98,   190,   191,   192,
     193,   194,   195,   196,   197,     0,     0,     0,   198,   199,
       0,   103,   107,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    40,    41,    42,
      43,    44,     0,    45,     0,    12,     0,     0,     0,   146,
     152,   153,     0,   155,   157,   159,     0,     0,    53,   163,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    54,
     103,   107,     0,   146,     0,   155,   159,     0,     0,     0,
       0,     0,     0,    55,    56,     0,     0,     0,     0,     0,
       0,     0,   200,     0,     0,     0,     0,   201,   202,   203,
     204,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,   205,   206,   349,    17,    18,    19,   180,    20,
     181,   182,     0,   183,   184,   185,   186,   187,   188,   146,
       0,    21,    22,    23,    24,    25,    26,    27,    28,    29,
      30,     0,     0,     0,     0,    31,    32,    33,    34,    35,
      36,    37,    38,   189,    97,    39,    98,   190,   191,   192,
     193,   194,   195,   196,   197,     0,     0,     0,   198,   199,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    40,    41,    42,
      43,    44,     0,    45,     0,    12,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    53,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    54,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    55,    56,     0,     0,     0,     0,     0,
       0,     0,   200,     0,     0,     0,     0,   201,   202,   203,
     204,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,   205,   206,   431,    17,    18,    19,   180,    20,
     181,   182,     0,   183,   184,   185,   186,   187,   188,     0,
       0,    21,    22,    23,    24,    25,    26,    27,    28,    29,
      30,     0,     0,     0,     0,    31,    32,    33,    34,    35,
      36,    37,    38,   189,    97,    39,    98,   190,   191,   192,
     193,   194,   195,   196,   197,     0,     0,     0,   198,   199,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    40,    41,    42,
      43,    44,     0,    45,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    53,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    54,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    55,    56,    17,    18,    19,     0,    93,
       0,     0,   200,    19,     0,     0,     0,   201,   202,   203,
     204,    21,    94,    95,    24,    96,    26,    27,    28,    29,
      30,     0,   205,   206,   207,    31,    32,    33,    34,    35,
      36,    37,    38,     0,     0,    39,     0,    37,    38,     0,
      97,    39,    98,   190,   191,   192,   193,   194,   195,   196,
     197,     0,     0,     0,   198,   199,     0,     0,     0,    17,
      18,    19,     0,    93,     0,     0,     0,    99,   100,    42,
      43,    44,     0,     0,     0,    21,    94,    95,    24,    96,
      26,    27,    28,    29,    30,     0,     0,     0,    53,    31,
      32,    33,    34,    35,    36,    37,    38,     0,     0,    39,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    54,
       0,     0,     0,     0,     0,    54,     0,     0,     0,     0,
       0,     0,     0,    55,    56,     0,     0,     0,     0,     0,
       0,    99,   100,    42,    43,    44,     0,     0,   200,     0,
       0,     0,     0,   201,   202,   203,   204,     0,     0,     0,
       0,     0,    53,     0,   277,     0,     0,     0,     0,   356,
     450,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    54,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    55,    56,    17,
      18,    19,   180,    20,   181,   182,     0,   183,   184,   185,
     186,   187,   188,   458,   459,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,     0,     0,     0,   369,    31,
      32,    33,    34,    35,    36,    37,    38,   189,    97,    39,
      98,   190,   191,   192,   193,   194,   195,   196,   197,     0,
       0,     0,   198,   199,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,    40,    41,    42,    43,    44,     0,    45,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    53,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    54,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    55,    56,     0,
       0,     0,     0,     0,     0,     0,   200,     0,     0,     0,
       0,   201,   202,   203,   204,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,   205,   206,    17,    18,
      19,   180,    20,   181,   182,     0,   183,   184,   185,   186,
     187,   188,     0,     0,    21,    22,    23,    24,    25,    26,
      27,    28,    29,    30,     0,     0,     0,     0,    31,    32,
      33,    34,    35,    36,    37,    38,   189,    97,    39,    98,
     190,   191,   192,   193,   194,   195,   196,   197,     0,     0,
       0,   198,   199,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      40,    41,    42,    43,    44,     0,    45,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,    53,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    54,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,    55,    56,     0,     0,
       0,     0,     0,     0,     0,   200,     0,     0,     0,     0,
     201,   202,   203,   204,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,   205,   122,    17,    18,    19,
     180,    20,   181,   182,     0,   183,   184,   185,   186,   187,
     188,     0,     0,    21,    22,    23,    24,    25,    26,    27,
      28,    29,    30,     0,     0,     0,     0,    31,    32,    33,
      34,    35,    36,    37,    38,   189,    97,    39,    98,   190,
     191,   192,   193,   194,   195,   196,   197,     0,     0,     0,
     198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    40,
      41,    42,    43,    44,     0,    45,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
      53,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,    54,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,    55,    56,    17,    18,    19,
       0,    20,     0,     0,   200,     0,     0,     0,     0,   201,
     202,   203,   204,    21,    22,    23,    24,    25,    26,    27,
      28,    29,    30,     0,   205,   206,     0,    31,    32,    33,
      34,    35,    36,    37,    38,     0,    97,    39,    98,   190,
     191,   192,   193,   194,   195,   196,   197,     0,     0,     0,
     198,   199,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    40,
      41,    42,    43,    44,     0,    45,     0,     0,     0,     0,
       0,     0,     0,    17,    18,    19,     0,    20,     0,     0,
      53,     0,     0,     0,     0,     0,     0,     0,     0,    21,
      22,    23,    24,    25,    26,    27,    28,    29,    30,     0,
       0,    54,     0,    31,    32,    33,    34,    35,    36,    37,
      38,     0,     0,    39,     0,    55,    56,     0,     0,     0,
       0,     0,     0,     0,   200,     0,     0,     0,     0,   201,
     202,   203,   204,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   205,    40,    41,    42,    43,    44,
       0,    45,     0,    12,     0,     0,     0,     0,     0,    46,
      47,    48,    49,    50,    51,    52,    53,     0,     0,     0,
       0,     0,     0,    17,    18,    19,     0,    20,     0,     0,
      19,     0,     0,     0,     0,     0,     0,    54,     0,    21,
      22,    23,    24,    25,    26,    27,    28,    29,    30,     0,
       0,    55,    56,    31,    32,    33,    34,    35,    36,    37,
      38,     0,     0,    39,    37,    38,     0,    97,    39,    98,
     190,   191,   192,   193,   194,   195,   196,   197,     0,     0,
      57,   198,   199,    19,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,    40,    41,    42,    43,    44,
       0,    45,     0,     0,     0,     0,     0,     0,     0,    46,
      47,    48,    49,    50,    51,    52,    53,    37,    38,     0,
      97,    39,    98,   190,   191,   192,   193,   194,   195,   196,
     197,     0,     0,     0,   198,   199,     0,    54,     0,     0,
       0,     0,    54,     0,     0,     0,     0,     0,     0,     0,
       0,    55,    56,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,   200,     0,     0,     0,     0,
     201,   202,   203,   204,     0,     0,     0,     0,     0,     0,
      57,     0,     0,     0,     0,     0,   356,     0,     0,     0,
       0,     0,     0,     0,     0,    54,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,    17,    18,    19,     0,    93,     0,     0,   200,     0,
       0,     0,     0,   201,   202,   203,   204,    21,    94,    95,
      24,    96,    26,    27,    28,    29,    30,     0,   295,     0,
       0,    31,    32,    33,    34,    35,    36,    37,    38,     0,
      97,    39,    98,   190,   191,   192,   193,   194,   195,   196,
     197,     0,     0,     0,   198,   199,     0,     0,    19,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    99,   100,    42,    43,    44,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    37,    38,    53,    97,    39,    98,   190,   191,
     192,   193,   194,   195,   196,   197,     0,     0,     0,   198,
     199,     0,     0,    19,     0,    54,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    55,
      56,     0,     0,     0,    19,     0,     0,     0,   200,     0,
       0,     0,     0,   201,   202,   203,   204,    37,    38,     0,
      97,    39,    98,   190,   191,   192,   193,   194,   195,   196,
     197,     0,     0,     0,   198,   199,     0,     0,    37,    38,
      54,    97,    39,    98,   190,   191,   192,   193,   194,   195,
     196,   197,     0,     0,     0,   198,   199,     0,     0,    19,
       0,     0,     0,   200,     0,     0,   266,     0,   201,   202,
     203,   204,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,    37,   311,    54,    97,    39,    98,   190,
     191,   192,   193,   194,   195,   196,   197,     0,     0,     0,
     198,   199,     0,     0,     0,     0,    54,     0,   200,     0,
       0,   361,     0,   201,   202,   203,   204,     0,     0,     0,
       0,     0,    17,    18,    19,     0,    93,     0,     0,   200,
       0,     0,     0,     0,   201,   202,   203,   204,    21,    94,
      95,    24,    96,    26,    27,    28,    29,    30,     0,     0,
       0,     0,    31,    32,    33,    34,    35,    36,    37,    38,
       0,    54,    39,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   200,     0,     0,     0,     0,   201,
     202,   203,   204,     0,    99,   100,    42,    43,    44,     0,
      17,    18,     0,     0,    93,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,    53,    21,    94,    95,    24,
      96,    26,    27,    28,    29,    30,     0,     0,     0,     0,
      31,    32,    33,    34,    35,    36,    54,     0,     0,    97,
       0,    98,     0,     0,     0,     0,     0,     0,     0,     0,
      55,    56,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,    17,    18,     0,     0,   142,     0,
       0,     0,    99,   100,    42,    43,    44,     0,     0,     0,
      21,   143,   144,    24,   145,    26,    27,    28,    29,    30,
       0,     0,     0,    53,    31,    32,    33,    34,    35,    36,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    55,    56,
      17,    18,     0,     0,    93,     0,    99,   100,    42,    43,
      44,     0,     0,     0,     0,     0,    21,    94,    95,    24,
      96,    26,    27,    28,    29,    30,     0,    53,     0,     0,
      31,    32,    33,    34,    35,    36,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    55,    56,    17,    18,     0,     0,    20,     0,
       0,     0,    99,   100,    42,    43,    44,     0,     0,     0,
      21,    22,    23,    24,    25,    26,    27,    28,    29,    30,
       0,     0,     0,    53,    31,    32,    33,    34,    35,    36,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,    55,    56,
       0,     0,     0,     0,     0,     0,    99,   100,    42,    43,
      44,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    53,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    55,    56
};

static const yytype_int16 yycheck[] =
{
      13,    91,    40,    41,    13,   182,    58,    59,   161,   140,
      17,    18,    17,    18,    13,   241,    63,    13,     4,    42,
      43,    44,   167,    44,    44,    44,    12,   144,    44,   144,
       6,    69,    70,   144,   144,    73,     5,   271,    82,    13,
      78,   187,    48,   158,     0,   162,    59,   281,   158,   160,
      59,    37,   144,    91,   200,    64,     5,   144,    71,    68,
      59,    99,   100,    59,   102,    74,   104,   105,   160,    68,
      39,    40,   216,   160,    43,    74,   144,   167,    91,   169,
      68,    67,   354,    59,   356,    59,    74,   359,   142,   142,
      39,    40,   160,    38,    43,   108,   144,    42,    43,    44,
     152,   153,   430,   142,    90,   159,   159,    86,   141,   122,
     157,   144,   160,   122,   442,   259,   125,   126,   271,   132,
     159,   130,    83,   122,   133,   134,   135,   258,   281,   167,
     116,   169,   118,   132,   279,    56,    57,    85,   161,   160,
     160,   160,   414,   274,   160,   142,   292,    96,   117,   158,
     136,   297,   298,    44,   167,   162,   169,   162,   384,   158,
     105,   307,   159,   435,    40,    41,   310,    44,   117,   182,
     158,   141,    44,   182,   144,   447,    97,   334,   335,   336,
     337,   325,   272,   182,    44,    42,    43,    44,   126,   279,
      86,    67,   130,   138,    86,   133,   134,   135,    86,   141,
     346,   427,   144,   347,   141,    86,   141,   144,   298,   144,
     354,   141,   356,   141,   144,   359,   144,   160,   161,   140,
      86,   142,   353,    42,    43,    44,   145,   146,   241,    86,
     376,   457,   241,    86,   272,   140,   462,   463,    54,    55,
     253,   279,   241,    45,    77,    78,    79,    86,   474,    42,
      43,    44,     5,   430,   149,   150,   151,    60,    61,   272,
     298,   140,    64,   330,   331,   442,   279,   141,   254,   144,
     414,   332,   333,   338,   339,   365,    44,   423,   291,   142,
     142,   161,   291,   161,   140,   298,   376,    86,   140,   433,
     303,   435,   291,   140,   303,   441,   160,   283,   159,   285,
     140,   160,   160,   447,   303,   140,   160,   438,   140,   140,
     161,   160,   458,    65,    66,    67,    68,    69,    70,    71,
      72,    73,    74,   125,   126,     4,   162,   365,   130,   141,
     140,   133,   134,   135,   154,   156,    62,   155,   376,    64,
     140,    20,    21,    22,    41,   160,   143,   198,   199,    53,
     141,   143,   365,   162,    33,    34,    35,    36,    37,   140,
     144,   141,    44,   376,   143,   160,   352,   141,   219,   159,
      10,   384,   161,   160,   160,   384,   158,   340,   342,   341,
     345,   125,   283,   343,   169,   384,   344,   367,   363,   291,
      61,   303,   291,   376,     4,   381,   462,    76,    77,    78,
      79,   464,    59,    72,    68,    68,   365,   159,    68,    68,
      20,    21,    22,    -1,   427,    -1,    -1,   430,   427,    -1,
     271,   430,    -1,    33,    34,    35,    36,    37,   427,   442,
     281,   430,    -1,   442,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   442,   457,    -1,    -1,    -1,   457,   462,
     463,    -1,    -1,   462,   463,    -1,    -1,    -1,   457,    -1,
      -1,   474,   141,   462,   463,   474,    76,    77,    78,    79,
      -1,    -1,    -1,    -1,    -1,   474,   327,   328,   329,   330,
     331,   332,   333,   334,   335,   336,   337,   338,   339,   340,
     341,   342,   343,   344,   345,     3,     4,     5,     6,     7,
       8,     9,    -1,    11,    12,    13,    14,    15,    16,    -1,
      -1,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      28,    -1,    -1,    -1,    -1,    33,    34,    35,    36,    37,
      38,    39,    40,    41,    42,    43,    44,    45,    46,    47,
      48,    49,    50,    51,    52,    -1,    -1,    -1,    56,    57,
      -1,    40,    41,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,    77,
      78,    79,    -1,    81,    -1,    83,    -1,    -1,    -1,    68,
      69,    70,    -1,    72,    73,    74,    -1,    -1,    96,    78,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   117,
      99,   100,    -1,   102,    -1,   104,   105,    -1,    -1,    -1,
      -1,    -1,    -1,   131,   132,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   140,    -1,    -1,    -1,    -1,   145,   146,   147,
     148,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   160,   161,   162,     3,     4,     5,     6,     7,
       8,     9,    -1,    11,    12,    13,    14,    15,    16,   158,
      -1,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      28,    -1,    -1,    -1,    -1,    33,    34,    35,    36,    37,
      38,    39,    40,    41,    42,    43,    44,    45,    46,    47,
      48,    49,    50,    51,    52,    -1,    -1,    -1,    56,    57,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,    77,
      78,    79,    -1,    81,    -1,    83,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    96,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   117,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   131,   132,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   140,    -1,    -1,    -1,    -1,   145,   146,   147,
     148,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   160,   161,   162,     3,     4,     5,     6,     7,
       8,     9,    -1,    11,    12,    13,    14,    15,    16,    -1,
      -1,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      28,    -1,    -1,    -1,    -1,    33,    34,    35,    36,    37,
      38,    39,    40,    41,    42,    43,    44,    45,    46,    47,
      48,    49,    50,    51,    52,    -1,    -1,    -1,    56,    57,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,    76,    77,
      78,    79,    -1,    81,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    96,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   117,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   131,   132,     3,     4,     5,    -1,     7,
      -1,    -1,   140,     5,    -1,    -1,    -1,   145,   146,   147,
     148,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      28,    -1,   160,   161,   162,    33,    34,    35,    36,    37,
      38,    39,    40,    -1,    -1,    43,    -1,    39,    40,    -1,
      42,    43,    44,    45,    46,    47,    48,    49,    50,    51,
      52,    -1,    -1,    -1,    56,    57,    -1,    -1,    -1,     3,
       4,     5,    -1,     7,    -1,    -1,    -1,    75,    76,    77,
      78,    79,    -1,    -1,    -1,    19,    20,    21,    22,    23,
      24,    25,    26,    27,    28,    -1,    -1,    -1,    96,    33,
      34,    35,    36,    37,    38,    39,    40,    -1,    -1,    43,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   117,
      -1,    -1,    -1,    -1,    -1,   117,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   131,   132,    -1,    -1,    -1,    -1,    -1,
      -1,    75,    76,    77,    78,    79,    -1,    -1,   140,    -1,
      -1,    -1,    -1,   145,   146,   147,   148,    -1,    -1,    -1,
      -1,    -1,    96,    -1,   162,    -1,    -1,    -1,    -1,   161,
     162,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   117,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,   131,   132,     3,
       4,     5,     6,     7,     8,     9,    -1,    11,    12,    13,
      14,    15,    16,    17,    18,    19,    20,    21,    22,    23,
      24,    25,    26,    27,    28,    -1,    -1,    -1,   162,    33,
      34,    35,    36,    37,    38,    39,    40,    41,    42,    43,
      44,    45,    46,    47,    48,    49,    50,    51,    52,    -1,
      -1,    -1,    56,    57,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    75,    76,    77,    78,    79,    -1,    81,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    96,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,   117,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,   131,   132,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,   140,    -1,    -1,    -1,
      -1,   145,   146,   147,   148,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,   160,   161,     3,     4,
       5,     6,     7,     8,     9,    -1,    11,    12,    13,    14,
      15,    16,    -1,    -1,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    -1,    -1,    -1,    -1,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    -1,    -1,
      -1,    56,    57,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      75,    76,    77,    78,    79,    -1,    81,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    96,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   117,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,   131,   132,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   140,    -1,    -1,    -1,    -1,
     145,   146,   147,   148,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   160,   161,     3,     4,     5,
       6,     7,     8,     9,    -1,    11,    12,    13,    14,    15,
      16,    -1,    -1,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    28,    -1,    -1,    -1,    -1,    33,    34,    35,
      36,    37,    38,    39,    40,    41,    42,    43,    44,    45,
      46,    47,    48,    49,    50,    51,    52,    -1,    -1,    -1,
      56,    57,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,
      76,    77,    78,    79,    -1,    81,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      96,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,   117,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   131,   132,     3,     4,     5,
      -1,     7,    -1,    -1,   140,    -1,    -1,    -1,    -1,   145,
     146,   147,   148,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    28,    -1,   160,   161,    -1,    33,    34,    35,
      36,    37,    38,    39,    40,    -1,    42,    43,    44,    45,
      46,    47,    48,    49,    50,    51,    52,    -1,    -1,    -1,
      56,    57,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    75,
      76,    77,    78,    79,    -1,    81,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,     3,     4,     5,    -1,     7,    -1,    -1,
      96,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    19,
      20,    21,    22,    23,    24,    25,    26,    27,    28,    -1,
      -1,   117,    -1,    33,    34,    35,    36,    37,    38,    39,
      40,    -1,    -1,    43,    -1,   131,   132,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   140,    -1,    -1,    -1,    -1,   145,
     146,   147,   148,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   160,    75,    76,    77,    78,    79,
      -1,    81,    -1,    83,    -1,    -1,    -1,    -1,    -1,    89,
      90,    91,    92,    93,    94,    95,    96,    -1,    -1,    -1,
      -1,    -1,    -1,     3,     4,     5,    -1,     7,    -1,    -1,
       5,    -1,    -1,    -1,    -1,    -1,    -1,   117,    -1,    19,
      20,    21,    22,    23,    24,    25,    26,    27,    28,    -1,
      -1,   131,   132,    33,    34,    35,    36,    37,    38,    39,
      40,    -1,    -1,    43,    39,    40,    -1,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    -1,    -1,
     160,    56,    57,     5,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    75,    76,    77,    78,    79,
      -1,    81,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    89,
      90,    91,    92,    93,    94,    95,    96,    39,    40,    -1,
      42,    43,    44,    45,    46,    47,    48,    49,    50,    51,
      52,    -1,    -1,    -1,    56,    57,    -1,   117,    -1,    -1,
      -1,    -1,   117,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,   131,   132,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   140,    -1,    -1,    -1,    -1,
     145,   146,   147,   148,    -1,    -1,    -1,    -1,    -1,    -1,
     160,    -1,    -1,    -1,    -1,    -1,   161,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   117,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,     3,     4,     5,    -1,     7,    -1,    -1,   140,    -1,
      -1,    -1,    -1,   145,   146,   147,   148,    19,    20,    21,
      22,    23,    24,    25,    26,    27,    28,    -1,   160,    -1,
      -1,    33,    34,    35,    36,    37,    38,    39,    40,    -1,
      42,    43,    44,    45,    46,    47,    48,    49,    50,    51,
      52,    -1,    -1,    -1,    56,    57,    -1,    -1,     5,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    75,    76,    77,    78,    79,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    39,    40,    96,    42,    43,    44,    45,    46,
      47,    48,    49,    50,    51,    52,    -1,    -1,    -1,    56,
      57,    -1,    -1,     5,    -1,   117,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   131,
     132,    -1,    -1,    -1,     5,    -1,    -1,    -1,   140,    -1,
      -1,    -1,    -1,   145,   146,   147,   148,    39,    40,    -1,
      42,    43,    44,    45,    46,    47,    48,    49,    50,    51,
      52,    -1,    -1,    -1,    56,    57,    -1,    -1,    39,    40,
     117,    42,    43,    44,    45,    46,    47,    48,    49,    50,
      51,    52,    -1,    -1,    -1,    56,    57,    -1,    -1,     5,
      -1,    -1,    -1,   140,    -1,    -1,   143,    -1,   145,   146,
     147,   148,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    39,    40,   117,    42,    43,    44,    45,
      46,    47,    48,    49,    50,    51,    52,    -1,    -1,    -1,
      56,    57,    -1,    -1,    -1,    -1,   117,    -1,   140,    -1,
      -1,   143,    -1,   145,   146,   147,   148,    -1,    -1,    -1,
      -1,    -1,     3,     4,     5,    -1,     7,    -1,    -1,   140,
      -1,    -1,    -1,    -1,   145,   146,   147,   148,    19,    20,
      21,    22,    23,    24,    25,    26,    27,    28,    -1,    -1,
      -1,    -1,    33,    34,    35,    36,    37,    38,    39,    40,
      -1,   117,    43,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   140,    -1,    -1,    -1,    -1,   145,
     146,   147,   148,    -1,    75,    76,    77,    78,    79,    -1,
       3,     4,    -1,    -1,     7,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    96,    19,    20,    21,    22,
      23,    24,    25,    26,    27,    28,    -1,    -1,    -1,    -1,
      33,    34,    35,    36,    37,    38,   117,    -1,    -1,    42,
      -1,    44,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
     131,   132,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,     3,     4,    -1,    -1,     7,    -1,
      -1,    -1,    75,    76,    77,    78,    79,    -1,    -1,    -1,
      19,    20,    21,    22,    23,    24,    25,    26,    27,    28,
      -1,    -1,    -1,    96,    33,    34,    35,    36,    37,    38,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   131,   132,
       3,     4,    -1,    -1,     7,    -1,    75,    76,    77,    78,
      79,    -1,    -1,    -1,    -1,    -1,    19,    20,    21,    22,
      23,    24,    25,    26,    27,    28,    -1,    96,    -1,    -1,
      33,    34,    35,    36,    37,    38,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   131,   132,     3,     4,    -1,    -1,     7,    -1,
      -1,    -1,    75,    76,    77,    78,    79,    -1,    -1,    -1,
      19,    20,    21,    22,    23,    24,    25,    26,    27,    28,
      -1,    -1,    -1,    96,    33,    34,    35,    36,    37,    38,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   131,   132,
      -1,    -1,    -1,    -1,    -1,    -1,    75,    76,    77,    78,
      79,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    96,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   131,   132
};

/* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
   symbol of state STATE-NUM.  */
static const yytype_uint16 yystos[] =
{
       0,    82,   164,   166,    48,     0,   168,    42,    43,    44,
      86,   169,    83,   165,   170,    86,   169,     3,     4,     5,
       7,    19,    20,    21,    22,    23,    24,    25,    26,    27,
      28,    33,    34,    35,    36,    37,    38,    39,    40,    43,
      75,    76,    77,    78,    79,    81,    89,    90,    91,    92,
      93,    94,    95,    96,   117,   131,   132,   160,   167,   171,
     201,   202,   203,   204,   205,   211,   212,   213,   214,   218,
     220,   221,   222,   223,   224,   226,   227,   228,   229,   230,
     261,   262,   263,   264,   265,   269,   270,   271,   272,   273,
      85,   161,   169,     7,    20,    21,    23,    42,    44,    75,
      76,   172,   214,   221,   222,   224,   172,   221,   229,    86,
      86,    86,    86,    86,    86,    86,   140,     5,   140,   170,
     261,   160,   161,   243,   141,   144,     4,    20,    21,    22,
      76,   207,   208,   209,   224,   229,   144,   160,    42,    44,
     169,   172,     7,    20,    21,    23,   221,   263,   269,   270,
     271,   272,   221,   221,   226,   221,   265,   221,   214,   221,
     263,   142,   225,   221,    44,   169,   213,   231,   232,   161,
     226,    38,   105,   138,   169,   215,   216,   217,   169,   219,
       6,     8,     9,    11,    12,    13,    14,    15,    16,    41,
      45,    46,    47,    48,    49,    50,    51,    52,    56,    57,
     140,   145,   146,   147,   148,   160,   161,   162,   172,   173,
     174,   176,   177,   178,   179,   180,   181,   182,   183,   184,
     185,   186,   187,   188,   189,   190,   191,   192,   193,   194,
     195,   196,   197,   199,   201,   202,   226,   237,   238,   239,
     240,   244,   245,   246,   249,   255,   259,   260,   207,   208,
     208,   206,   210,   214,   226,   208,   208,   208,   169,   159,
     225,   140,   160,   160,   160,   160,   143,   183,   196,   200,
     226,   142,   161,    86,   169,   233,   234,   162,   232,   231,
     160,   159,   141,   144,   141,   144,   160,   160,   239,   242,
     243,   140,   140,   160,   160,   160,   199,   140,   140,   183,
     183,   199,   162,   241,    56,    57,    97,   142,   141,   141,
     144,    40,   197,   140,    65,    66,    67,    68,    69,    70,
      71,    72,    73,    74,   159,   198,   183,   149,   150,   151,
     145,   146,    54,    55,    58,    59,   152,   153,    60,    61,
     154,   155,   156,    62,    64,    63,   157,   144,   160,   162,
     170,   238,   226,   169,   159,   225,   161,   197,   235,   159,
     143,   143,   200,   213,   267,   268,   225,   144,   160,   162,
     200,   216,   169,    41,   237,   245,   256,   199,   160,   199,
     199,   213,   248,   141,   244,    53,   175,   199,   197,   197,
     183,   183,   183,   185,   185,   186,   186,   187,   187,   187,
     187,   188,   188,   189,   190,   191,   192,   193,   194,   199,
     197,   169,   225,   235,   159,   235,   236,   235,   143,   233,
     162,   267,   234,   140,   248,   257,   258,   141,   141,   169,
     141,   162,   143,   158,   235,   144,   162,   160,    44,   266,
     199,   160,   141,   238,   247,   161,   250,   159,   242,   197,
     162,   235,   225,   160,   141,   199,   242,    10,    17,    18,
     162,   251,   252,   253,   254,   235,   160,   238,   199,   158,
     238,   251,   238,   162,   253,   158
};

#define yyerrok		(yyerrstatus = 0)
#define yyclearin	(yychar = YYEMPTY)
#define YYEMPTY		(-2)
#define YYEOF		0

#define YYACCEPT	goto yyacceptlab
#define YYABORT		goto yyabortlab
#define YYERROR		goto yyerrorlab


/* Like YYERROR except do call yyerror.  This remains here temporarily
   to ease the transition to the new meaning of YYERROR, for GCC.
   Once GCC version 2 has supplanted version 1, this can go.  */

#define YYFAIL		goto yyerrlab

#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)					\
do								\
  if (yychar == YYEMPTY && yylen == 1)				\
    {								\
      yychar = (Token);						\
      yylval = (Value);						\
      yytoken = YYTRANSLATE (yychar);				\
      YYPOPSTACK (1);						\
      goto yybackup;						\
    }								\
  else								\
    {								\
      yyerror (&yylloc, state, YY_("syntax error: cannot back up")); \
      YYERROR;							\
    }								\
while (YYID (0))


#define YYTERROR	1
#define YYERRCODE	256


/* YYLLOC_DEFAULT -- Set CURRENT to span from RHS[1] to RHS[N].
   If N is 0, then set CURRENT to the empty location which ends
   the previous symbol: RHS[0] (always defined).  */

#define YYRHSLOC(Rhs, K) ((Rhs)[K])
#ifndef YYLLOC_DEFAULT
# define YYLLOC_DEFAULT(Current, Rhs, N)				\
    do									\
      if (YYID (N))                                                    \
	{								\
	  (Current).first_line   = YYRHSLOC (Rhs, 1).first_line;	\
	  (Current).first_column = YYRHSLOC (Rhs, 1).first_column;	\
	  (Current).last_line    = YYRHSLOC (Rhs, N).last_line;		\
	  (Current).last_column  = YYRHSLOC (Rhs, N).last_column;	\
	}								\
      else								\
	{								\
	  (Current).first_line   = (Current).last_line   =		\
	    YYRHSLOC (Rhs, 0).last_line;				\
	  (Current).first_column = (Current).last_column =		\
	    YYRHSLOC (Rhs, 0).last_column;				\
	}								\
    while (YYID (0))
#endif


/* YY_LOCATION_PRINT -- Print the location on the stream.
   This macro was not mandated originally: define only if we know
   we won't break user code: when these are the locations we know.  */

#ifndef YY_LOCATION_PRINT
# if defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL
#  define YY_LOCATION_PRINT(File, Loc)			\
     fprintf (File, "%d.%d-%d.%d",			\
	      (Loc).first_line, (Loc).first_column,	\
	      (Loc).last_line,  (Loc).last_column)
# else
#  define YY_LOCATION_PRINT(File, Loc) ((void) 0)
# endif
#endif


/* YYLEX -- calling `yylex' with the right arguments.  */

#ifdef YYLEX_PARAM
# define YYLEX yylex (&yylval, &yylloc, YYLEX_PARAM)
#else
# define YYLEX yylex (&yylval, &yylloc, state)
#endif

/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)			\
do {						\
  if (yydebug)					\
    YYFPRINTF Args;				\
} while (YYID (0))

# define YY_SYMBOL_PRINT(Title, Type, Value, Location)			  \
do {									  \
  if (yydebug)								  \
    {									  \
      YYFPRINTF (stderr, "%s ", Title);					  \
      yy_symbol_print (stderr,						  \
		  Type, Value, Location, state); \
      YYFPRINTF (stderr, "\n");						  \
    }									  \
} while (YYID (0))


/*--------------------------------.
| Print this symbol on YYOUTPUT.  |
`--------------------------------*/

/*ARGSUSED*/
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static void
yy_symbol_value_print (FILE *yyoutput, int yytype, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, struct _mesa_glsl_parse_state *state)
#else
static void
yy_symbol_value_print (yyoutput, yytype, yyvaluep, yylocationp, state)
    FILE *yyoutput;
    int yytype;
    YYSTYPE const * const yyvaluep;
    YYLTYPE const * const yylocationp;
    struct _mesa_glsl_parse_state *state;
#endif
{
  if (!yyvaluep)
    return;
  YYUSE (yylocationp);
  YYUSE (state);
# ifdef YYPRINT
  if (yytype < YYNTOKENS)
    YYPRINT (yyoutput, yytoknum[yytype], *yyvaluep);
# else
  YYUSE (yyoutput);
# endif
  switch (yytype)
    {
      default:
	break;
    }
}


/*--------------------------------.
| Print this symbol on YYOUTPUT.  |
`--------------------------------*/

#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static void
yy_symbol_print (FILE *yyoutput, int yytype, YYSTYPE const * const yyvaluep, YYLTYPE const * const yylocationp, struct _mesa_glsl_parse_state *state)
#else
static void
yy_symbol_print (yyoutput, yytype, yyvaluep, yylocationp, state)
    FILE *yyoutput;
    int yytype;
    YYSTYPE const * const yyvaluep;
    YYLTYPE const * const yylocationp;
    struct _mesa_glsl_parse_state *state;
#endif
{
  if (yytype < YYNTOKENS)
    YYFPRINTF (yyoutput, "token %s (", yytname[yytype]);
  else
    YYFPRINTF (yyoutput, "nterm %s (", yytname[yytype]);

  YY_LOCATION_PRINT (yyoutput, *yylocationp);
  YYFPRINTF (yyoutput, ": ");
  yy_symbol_value_print (yyoutput, yytype, yyvaluep, yylocationp, state);
  YYFPRINTF (yyoutput, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static void
yy_stack_print (yytype_int16 *bottom, yytype_int16 *top)
#else
static void
yy_stack_print (bottom, top)
    yytype_int16 *bottom;
    yytype_int16 *top;
#endif
{
  YYFPRINTF (stderr, "Stack now");
  for (; bottom <= top; ++bottom)
    YYFPRINTF (stderr, " %d", *bottom);
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)				\
do {								\
  if (yydebug)							\
    yy_stack_print ((Bottom), (Top));				\
} while (YYID (0))


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static void
yy_reduce_print (YYSTYPE *yyvsp, YYLTYPE *yylsp, int yyrule, struct _mesa_glsl_parse_state *state)
#else
static void
yy_reduce_print (yyvsp, yylsp, yyrule, state)
    YYSTYPE *yyvsp;
    YYLTYPE *yylsp;
    int yyrule;
    struct _mesa_glsl_parse_state *state;
#endif
{
  int yynrhs = yyr2[yyrule];
  int yyi;
  unsigned long int yylno = yyrline[yyrule];
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %lu):\n",
	     yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      fprintf (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr, yyrhs[yyprhs[yyrule] + yyi],
		       &(yyvsp[(yyi + 1) - (yynrhs)])
		       , &(yylsp[(yyi + 1) - (yynrhs)])		       , state);
      fprintf (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)		\
do {					\
  if (yydebug)				\
    yy_reduce_print (yyvsp, yylsp, Rule, state); \
} while (YYID (0))

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args)
# define YY_SYMBOL_PRINT(Title, Type, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef	YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif



#if YYERROR_VERBOSE

# ifndef yystrlen
#  if defined __GLIBC__ && defined _STRING_H
#   define yystrlen strlen
#  else
/* Return the length of YYSTR.  */
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static YYSIZE_T
yystrlen (const char *yystr)
#else
static YYSIZE_T
yystrlen (yystr)
    const char *yystr;
#endif
{
  YYSIZE_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
#  endif
# endif

# ifndef yystpcpy
#  if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#   define yystpcpy stpcpy
#  else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static char *
yystpcpy (char *yydest, const char *yysrc)
#else
static char *
yystpcpy (yydest, yysrc)
    char *yydest;
    const char *yysrc;
#endif
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
#  endif
# endif

# ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYSIZE_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYSIZE_T yyn = 0;
      char const *yyp = yystr;

      for (;;)
	switch (*++yyp)
	  {
	  case '\'':
	  case ',':
	    goto do_not_strip_quotes;

	  case '\\':
	    if (*++yyp != '\\')
	      goto do_not_strip_quotes;
	    /* Fall through.  */
	  default:
	    if (yyres)
	      yyres[yyn] = *yyp;
	    yyn++;
	    break;

	  case '"':
	    if (yyres)
	      yyres[yyn] = '\0';
	    return yyn;
	  }
    do_not_strip_quotes: ;
    }

  if (! yyres)
    return yystrlen (yystr);

  return yystpcpy (yyres, yystr) - yyres;
}
# endif

/* Copy into YYRESULT an error message about the unexpected token
   YYCHAR while in state YYSTATE.  Return the number of bytes copied,
   including the terminating null byte.  If YYRESULT is null, do not
   copy anything; just return the number of bytes that would be
   copied.  As a special case, return 0 if an ordinary "syntax error"
   message will do.  Return YYSIZE_MAXIMUM if overflow occurs during
   size calculation.  */
static YYSIZE_T
yysyntax_error (char *yyresult, int yystate, int yychar)
{
  int yyn = yypact[yystate];

  if (! (YYPACT_NINF < yyn && yyn <= YYLAST))
    return 0;
  else
    {
      int yytype = YYTRANSLATE (yychar);
      YYSIZE_T yysize0 = yytnamerr (0, yytname[yytype]);
      YYSIZE_T yysize = yysize0;
      YYSIZE_T yysize1;
      int yysize_overflow = 0;
      enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
      char const *yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];
      int yyx;

# if 0
      /* This is so xgettext sees the translatable formats that are
	 constructed on the fly.  */
      YY_("syntax error, unexpected %s");
      YY_("syntax error, unexpected %s, expecting %s");
      YY_("syntax error, unexpected %s, expecting %s or %s");
      YY_("syntax error, unexpected %s, expecting %s or %s or %s");
      YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s");
# endif
      char *yyfmt;
      char const *yyf;
      static char const yyunexpected[] = "syntax error, unexpected %s";
      static char const yyexpecting[] = ", expecting %s";
      static char const yyor[] = " or %s";
      char yyformat[sizeof yyunexpected
		    + sizeof yyexpecting - 1
		    + ((YYERROR_VERBOSE_ARGS_MAXIMUM - 2)
		       * (sizeof yyor - 1))];
      char const *yyprefix = yyexpecting;

      /* Start YYX at -YYN if negative to avoid negative indexes in
	 YYCHECK.  */
      int yyxbegin = yyn < 0 ? -yyn : 0;

      /* Stay within bounds of both yycheck and yytname.  */
      int yychecklim = YYLAST - yyn + 1;
      int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
      int yycount = 1;

      yyarg[0] = yytname[yytype];
      yyfmt = yystpcpy (yyformat, yyunexpected);

      for (yyx = yyxbegin; yyx < yyxend; ++yyx)
	if (yycheck[yyx + yyn] == yyx && yyx != YYTERROR)
	  {
	    if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM)
	      {
		yycount = 1;
		yysize = yysize0;
		yyformat[sizeof yyunexpected - 1] = '\0';
		break;
	      }
	    yyarg[yycount++] = yytname[yyx];
	    yysize1 = yysize + yytnamerr (0, yytname[yyx]);
	    yysize_overflow |= (yysize1 < yysize);
	    yysize = yysize1;
	    yyfmt = yystpcpy (yyfmt, yyprefix);
	    yyprefix = yyor;
	  }

      yyf = YY_(yyformat);
      yysize1 = yysize + yystrlen (yyf);
      yysize_overflow |= (yysize1 < yysize);
      yysize = yysize1;

      if (yysize_overflow)
	return YYSIZE_MAXIMUM;

      if (yyresult)
	{
	  /* Avoid sprintf, as that infringes on the user's name space.
	     Don't have undefined behavior even if the translation
	     produced a string with the wrong number of "%s"s.  */
	  char *yyp = yyresult;
	  int yyi = 0;
	  while ((*yyp = *yyf) != '\0')
	    {
	      if (*yyp == '%' && yyf[1] == 's' && yyi < yycount)
		{
		  yyp += yytnamerr (yyp, yyarg[yyi++]);
		  yyf += 2;
		}
	      else
		{
		  yyp++;
		  yyf++;
		}
	    }
	}
      return yysize;
    }
}
#endif /* YYERROR_VERBOSE */


/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

/*ARGSUSED*/
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
static void
yydestruct (const char *yymsg, int yytype, YYSTYPE *yyvaluep, YYLTYPE *yylocationp, struct _mesa_glsl_parse_state *state)
#else
static void
yydestruct (yymsg, yytype, yyvaluep, yylocationp, state)
    const char *yymsg;
    int yytype;
    YYSTYPE *yyvaluep;
    YYLTYPE *yylocationp;
    struct _mesa_glsl_parse_state *state;
#endif
{
  YYUSE (yyvaluep);
  YYUSE (yylocationp);
  YYUSE (state);

  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yytype, yyvaluep, yylocationp);

  switch (yytype)
    {

      default:
	break;
    }
}


/* Prevent warnings from -Wmissing-prototypes.  */

#ifdef YYPARSE_PARAM
#if defined __STDC__ || defined __cplusplus
int yyparse (void *YYPARSE_PARAM);
#else
int yyparse ();
#endif
#else /* ! YYPARSE_PARAM */
#if defined __STDC__ || defined __cplusplus
int yyparse (struct _mesa_glsl_parse_state *state);
#else
int yyparse ();
#endif
#endif /* ! YYPARSE_PARAM */






/*----------.
| yyparse.  |
`----------*/

#ifdef YYPARSE_PARAM
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
int
yyparse (void *YYPARSE_PARAM)
#else
int
yyparse (YYPARSE_PARAM)
    void *YYPARSE_PARAM;
#endif
#else /* ! YYPARSE_PARAM */
#if (defined __STDC__ || defined __C99__FUNC__ \
     || defined __cplusplus || defined _MSC_VER)
int
yyparse (struct _mesa_glsl_parse_state *state)
#else
int
yyparse (state)
    struct _mesa_glsl_parse_state *state;
#endif
#endif
{
  /* The look-ahead symbol.  */
int yychar;

/* The semantic value of the look-ahead symbol.  */
YYSTYPE yylval;

/* Number of syntax errors so far.  */
int yynerrs;
/* Location data for the look-ahead symbol.  */
YYLTYPE yylloc;

  int yystate;
  int yyn;
  int yyresult;
  /* Number of tokens to shift before error messages enabled.  */
  int yyerrstatus;
  /* Look-ahead token as an internal (translated) token number.  */
  int yytoken = 0;
#if YYERROR_VERBOSE
  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYSIZE_T yymsg_alloc = sizeof yymsgbuf;
#endif

  /* Three stacks and their tools:
     `yyss': related to states,
     `yyvs': related to semantic values,
     `yyls': related to locations.

     Refer to the stacks thru separate pointers, to allow yyoverflow
     to reallocate them elsewhere.  */

  /* The state stack.  */
  yytype_int16 yyssa[YYINITDEPTH];
  yytype_int16 *yyss = yyssa;
  yytype_int16 *yyssp;

  /* The semantic value stack.  */
  YYSTYPE yyvsa[YYINITDEPTH];
  YYSTYPE *yyvs = yyvsa;
  YYSTYPE *yyvsp;

  /* The location stack.  */
  YYLTYPE yylsa[YYINITDEPTH];
  YYLTYPE *yyls = yylsa;
  YYLTYPE *yylsp;
  /* The locations where the error started and ended.  */
  YYLTYPE yyerror_range[2];

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N), yylsp -= (N))

  YYSIZE_T yystacksize = YYINITDEPTH;

  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;
  YYLTYPE yyloc;

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yystate = 0;
  yyerrstatus = 0;
  yynerrs = 0;
  yychar = YYEMPTY;		/* Cause a token to be read.  */

  /* Initialize stack pointers.
     Waste one element of value and location stack
     so that they stay on the same level as the state stack.
     The wasted elements are never initialized.  */

  yyssp = yyss;
  yyvsp = yyvs;
  yylsp = yyls;
#if defined YYLTYPE_IS_TRIVIAL && YYLTYPE_IS_TRIVIAL
  /* Initialize the default location before parsing starts.  */
  yylloc.first_line   = yylloc.last_line   = 1;
  yylloc.first_column = yylloc.last_column = 0;
#endif


  /* User initialization code.  */
#line 89 "glsl_parser.yy"
{
   yylloc.first_line = 1;
   yylloc.first_column = 1;
   yylloc.last_line = 1;
   yylloc.last_column = 1;
   yylloc.source = 0;
   yylloc.path = NULL;
}
/* Line 1078 of yacc.c.  */
#line 2564 "/tmp/glsl_parser.cpp"
  yylsp[0] = yylloc;
  goto yysetstate;

/*------------------------------------------------------------.
| yynewstate -- Push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
 yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;

 yysetstate:
  *yyssp = yystate;

  if (yyss + yystacksize - 1 <= yyssp)
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYSIZE_T yysize = yyssp - yyss + 1;

#ifdef yyoverflow
      {
	/* Give user a chance to reallocate the stack.  Use copies of
	   these so that the &'s don't force the real ones into
	   memory.  */
	YYSTYPE *yyvs1 = yyvs;
	yytype_int16 *yyss1 = yyss;
	YYLTYPE *yyls1 = yyls;

	/* Each stack pointer address is followed by the size of the
	   data in use in that stack, in bytes.  This used to be a
	   conditional around just the two extra args, but that might
	   be undefined if yyoverflow is a macro.  */
	yyoverflow (YY_("memory exhausted"),
		    &yyss1, yysize * sizeof (*yyssp),
		    &yyvs1, yysize * sizeof (*yyvsp),
		    &yyls1, yysize * sizeof (*yylsp),
		    &yystacksize);
	yyls = yyls1;
	yyss = yyss1;
	yyvs = yyvs1;
      }
#else /* no yyoverflow */
# ifndef YYSTACK_RELOCATE
      goto yyexhaustedlab;
# else
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
	goto yyexhaustedlab;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
	yystacksize = YYMAXDEPTH;

      {
	yytype_int16 *yyss1 = yyss;
	union yyalloc *yyptr =
	  (union yyalloc *) YYSTACK_ALLOC (YYSTACK_BYTES (yystacksize));
	if (! yyptr)
	  goto yyexhaustedlab;
	YYSTACK_RELOCATE (yyss);
	YYSTACK_RELOCATE (yyvs);
	YYSTACK_RELOCATE (yyls);
#  undef YYSTACK_RELOCATE
	if (yyss1 != yyssa)
	  YYSTACK_FREE (yyss1);
      }
# endif
#endif /* no yyoverflow */

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;
      yylsp = yyls + yysize - 1;

      YYDPRINTF ((stderr, "Stack size increased to %lu\n",
		  (unsigned long int) yystacksize));

      if (yyss + yystacksize - 1 <= yyssp)
	YYABORT;
    }

  YYDPRINTF ((stderr, "Entering state %d\n", yystate));

  goto yybackup;

/*-----------.
| yybackup.  |
`-----------*/
yybackup:

  /* Do appropriate processing given the current state.  Read a
     look-ahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to look-ahead token.  */
  yyn = yypact[yystate];
  if (yyn == YYPACT_NINF)
    goto yydefault;

  /* Not known => get a look-ahead token if don't already have one.  */

  /* YYCHAR is either YYEMPTY or YYEOF or a valid look-ahead symbol.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token: "));
      yychar = YYLEX;
    }

  if (yychar <= YYEOF)
    {
      yychar = yytoken = YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yyn == 0 || yyn == YYTABLE_NINF)
	goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  if (yyn == YYFINAL)
    YYACCEPT;

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the look-ahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);

  /* Discard the shifted token unless it is eof.  */
  if (yychar != YYEOF)
    yychar = YYEMPTY;

  yystate = yyn;
  *++yyvsp = yylval;
  *++yylsp = yylloc;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- Do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     `$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];

  /* Default location.  */
  YYLLOC_DEFAULT (yyloc, (yylsp - yylen), yylen);
  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
        case 2:
#line 295 "glsl_parser.yy"
    {
      _mesa_glsl_initialize_types(state);
   ;}
    break;

  case 3:
#line 299 "glsl_parser.yy"
    {
      delete state->symbols;
      state->symbols = new(ralloc_parent(state)) glsl_symbol_table;
      if (state->es_shader) {
         if (state->stage == MESA_SHADER_FRAGMENT) {
            state->symbols->add_default_precision_qualifier("int", ast_precision_medium);
         } else {
            state->symbols->add_default_precision_qualifier("float", ast_precision_high);
            state->symbols->add_default_precision_qualifier("int", ast_precision_high);
         }
         state->symbols->add_default_precision_qualifier("sampler2D", ast_precision_low);
         state->symbols->add_default_precision_qualifier("samplerExternalOES", ast_precision_low);
         state->symbols->add_default_precision_qualifier("samplerCube", ast_precision_low);
         state->symbols->add_default_precision_qualifier("atomic_uint", ast_precision_high);
      }
      _mesa_glsl_initialize_types(state);
   ;}
    break;

  case 5:
#line 321 "glsl_parser.yy"
    {
      state->process_version_directive(&(yylsp[(2) - (3)]), (yyvsp[(2) - (3)].n), NULL);
      if (state->error) {
         YYERROR;
      }
   ;}
    break;

  case 6:
#line 328 "glsl_parser.yy"
    {
      state->process_version_directive(&(yylsp[(2) - (4)]), (yyvsp[(2) - (4)].n), (yyvsp[(3) - (4)].identifier));
      if (state->error) {
         YYERROR;
      }
   ;}
    break;

  case 7:
#line 337 "glsl_parser.yy"
    { (yyval.node) = NULL; ;}
    break;

  case 8:
#line 338 "glsl_parser.yy"
    { (yyval.node) = NULL; ;}
    break;

  case 9:
#line 339 "glsl_parser.yy"
    { (yyval.node) = NULL; ;}
    break;

  case 10:
#line 340 "glsl_parser.yy"
    { (yyval.node) = NULL; ;}
    break;

  case 11:
#line 342 "glsl_parser.yy"
    {
      /* Pragma invariant(all) cannot be used in a fragment shader.
       *
       * Page 27 of the GLSL 1.20 spec, Page 53 of the GLSL ES 3.00 spec:
       *
       *     "It is an error to use this pragma in a fragment shader."
       */
      if (state->is_version(120, 300) &&
          state->stage == MESA_SHADER_FRAGMENT) {
         _mesa_glsl_error(& (yylsp[(1) - (2)]), state,
                          "pragma `invariant(all)' cannot be used "
                          "in a fragment shader.");
      } else if (!state->is_version(120, 100)) {
         _mesa_glsl_warning(& (yylsp[(1) - (2)]), state,
                            "pragma `invariant(all)' not supported in %s "
                            "(GLSL ES 1.00 or GLSL 1.20 required)",
                            state->get_version_string());
      } else {
         state->all_invariant = true;
      }

      (yyval.node) = NULL;
   ;}
    break;

  case 12:
#line 366 "glsl_parser.yy"
    {
      linear_ctx *mem_ctx = state->linalloc;
      (yyval.node) = new(mem_ctx) ast_warnings_toggle(true);
   ;}
    break;

  case 13:
#line 371 "glsl_parser.yy"
    {
      linear_ctx *mem_ctx = state->linalloc;
      (yyval.node) = new(mem_ctx) ast_warnings_toggle(false);
   ;}
    break;

  case 19:
#line 390 "glsl_parser.yy"
    {
      if (!_mesa_glsl_process_extension((yyvsp[(2) - (5)].identifier), & (yylsp[(2) - (5)]), (yyvsp[(4) - (5)].identifier), & (yylsp[(4) - (5)]), state)) {
         YYERROR;
      }
   ;}
    break;

  case 20:
#line 399 "glsl_parser.yy"
    {
      /* FINISHME: The NULL test is required because pragmas are set to
       * FINISHME: NULL. (See production rule for external_declaration.)
       */
      if ((yyvsp[(1) - (1)].node) != NULL)
         state->translation_unit.push_tail(& (yyvsp[(1) - (1)].node)->link);
   ;}
    break;

  case 21:
#line 407 "glsl_parser.yy"
    {
      /* FINISHME: The NULL test is required because pragmas are set to
       * FINISHME: NULL. (See production rule for external_declaration.)
       */
      if ((yyvsp[(2) - (2)].node) != NULL)
         state->translation_unit.push_tail(& (yyvsp[(2) - (2)].node)->link);
   ;}
    break;

  case 22:
#line 414 "glsl_parser.yy"
    {
      if (!state->allow_extension_directive_midshader) {
         _mesa_glsl_error(& (yylsp[(2) - (2)]), state,
                          "#extension directive is not allowed "
                          "in the middle of a shader");
         YYERROR;
      }
   ;}
    break;

  case 25:
#line 431 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_identifier, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      (yyval.expression)->primary_expression.identifier = (yyvsp[(1) - (1)].identifier);
   ;}
    break;

  case 26:
#line 438 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_int_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      (yyval.expression)->primary_expression.int_constant = (yyvsp[(1) - (1)].n);
   ;}
    break;

  case 27:
#line 445 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_uint_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      (yyval.expression)->primary_expression.uint_constant = (yyvsp[(1) - (1)].n);
   ;}
    break;

  case 28:
#line 452 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_int64_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      (yyval.expression)->primary_expression.int64_constant = (yyvsp[(1) - (1)].n64);
   ;}
    break;

  case 29:
#line 459 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_uint64_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      (yyval.expression)->primary_expression.uint64_constant = (yyvsp[(1) - (1)].n64);
   ;}
    break;

  case 30:
#line 466 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_float16_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      (yyval.expression)->primary_expression.float16_constant = (yyvsp[(1) - (1)].real);
   ;}
    break;

  case 31:
#line 473 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_float_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      (yyval.expression)->primary_expression.float_constant = (yyvsp[(1) - (1)].real);
   ;}
    break;

  case 32:
#line 480 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_double_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      (yyval.expression)->primary_expression.double_constant = (yyvsp[(1) - (1)].dreal);
   ;}
    break;

  case 33:
#line 487 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_bool_constant, NULL, NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      (yyval.expression)->primary_expression.bool_constant = (yyvsp[(1) - (1)].n);
   ;}
    break;

  case 34:
#line 494 "glsl_parser.yy"
    {
      (yyval.expression) = (yyvsp[(2) - (3)].expression);
   ;}
    break;

  case 36:
#line 502 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_array_index, (yyvsp[(1) - (4)].expression), (yyvsp[(3) - (4)].expression), NULL);
      (yyval.expression)->set_location_range((yylsp[(1) - (4)]), (yylsp[(4) - (4)]));
   ;}
    break;

  case 37:
#line 508 "glsl_parser.yy"
    {
      (yyval.expression) = (yyvsp[(1) - (1)].expression);
   ;}
    break;

  case 38:
#line 512 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_field_selection, (yyvsp[(1) - (3)].expression), NULL, NULL);
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
      (yyval.expression)->primary_expression.identifier = (yyvsp[(3) - (3)].identifier);
   ;}
    break;

  case 39:
#line 519 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_post_inc, (yyvsp[(1) - (2)].expression), NULL, NULL);
      (yyval.expression)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
   ;}
    break;

  case 40:
#line 525 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_post_dec, (yyvsp[(1) - (2)].expression), NULL, NULL);
      (yyval.expression)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
   ;}
    break;

  case 48:
#line 556 "glsl_parser.yy"
    {
      (yyval.expression) = (yyvsp[(1) - (2)].expression);
      (yyval.expression)->set_location((yylsp[(1) - (2)]));
      (yyval.expression)->expressions.push_tail(& (yyvsp[(2) - (2)].expression)->link);
   ;}
    break;

  case 49:
#line 562 "glsl_parser.yy"
    {
      (yyval.expression) = (yyvsp[(1) - (3)].expression);
      (yyval.expression)->set_location((yylsp[(1) - (3)]));
      (yyval.expression)->expressions.push_tail(& (yyvsp[(3) - (3)].expression)->link);
   ;}
    break;

  case 51:
#line 578 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_function_expression((yyvsp[(1) - (1)].type_specifier));
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      ;}
    break;

  case 52:
#line 584 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_function_expression((yyvsp[(1) - (1)].expression));
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      ;}
    break;

  case 54:
#line 599 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_pre_inc, (yyvsp[(2) - (2)].expression), NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (2)]));
   ;}
    break;

  case 55:
#line 605 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_pre_dec, (yyvsp[(2) - (2)].expression), NULL, NULL);
      (yyval.expression)->set_location((yylsp[(1) - (2)]));
   ;}
    break;

  case 56:
#line 611 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression((yyvsp[(1) - (2)].n), (yyvsp[(2) - (2)].expression), NULL, NULL);
      (yyval.expression)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
   ;}
    break;

  case 57:
#line 620 "glsl_parser.yy"
    { (yyval.n) = ast_plus; ;}
    break;

  case 58:
#line 621 "glsl_parser.yy"
    { (yyval.n) = ast_neg; ;}
    break;

  case 59:
#line 622 "glsl_parser.yy"
    { (yyval.n) = ast_logic_not; ;}
    break;

  case 60:
#line 623 "glsl_parser.yy"
    { (yyval.n) = ast_bit_not; ;}
    break;

  case 62:
#line 629 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_mul, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 63:
#line 635 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_div, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 64:
#line 641 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_mod, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 66:
#line 651 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_add, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 67:
#line 657 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_sub, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 69:
#line 667 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_lshift, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 70:
#line 673 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_rshift, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 72:
#line 683 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_less, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 73:
#line 689 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_greater, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 74:
#line 695 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_lequal, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 75:
#line 701 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_gequal, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 77:
#line 711 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_equal, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 78:
#line 717 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_nequal, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 80:
#line 727 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_bit_and, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 82:
#line 737 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_bit_xor, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 84:
#line 747 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_bit_or, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 86:
#line 757 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_logic_and, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 88:
#line 767 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_logic_xor, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 90:
#line 777 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression_bin(ast_logic_or, (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 92:
#line 787 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression(ast_conditional, (yyvsp[(1) - (5)].expression), (yyvsp[(3) - (5)].expression), (yyvsp[(5) - (5)].expression));
      (yyval.expression)->set_location_range((yylsp[(1) - (5)]), (yylsp[(5) - (5)]));
   ;}
    break;

  case 94:
#line 797 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_expression((yyvsp[(2) - (3)].n), (yyvsp[(1) - (3)].expression), (yyvsp[(3) - (3)].expression), NULL);
      (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 95:
#line 805 "glsl_parser.yy"
    { (yyval.n) = ast_assign; ;}
    break;

  case 96:
#line 806 "glsl_parser.yy"
    { (yyval.n) = ast_mul_assign; ;}
    break;

  case 97:
#line 807 "glsl_parser.yy"
    { (yyval.n) = ast_div_assign; ;}
    break;

  case 98:
#line 808 "glsl_parser.yy"
    { (yyval.n) = ast_mod_assign; ;}
    break;

  case 99:
#line 809 "glsl_parser.yy"
    { (yyval.n) = ast_add_assign; ;}
    break;

  case 100:
#line 810 "glsl_parser.yy"
    { (yyval.n) = ast_sub_assign; ;}
    break;

  case 101:
#line 811 "glsl_parser.yy"
    { (yyval.n) = ast_ls_assign; ;}
    break;

  case 102:
#line 812 "glsl_parser.yy"
    { (yyval.n) = ast_rs_assign; ;}
    break;

  case 103:
#line 813 "glsl_parser.yy"
    { (yyval.n) = ast_and_assign; ;}
    break;

  case 104:
#line 814 "glsl_parser.yy"
    { (yyval.n) = ast_xor_assign; ;}
    break;

  case 105:
#line 815 "glsl_parser.yy"
    { (yyval.n) = ast_or_assign; ;}
    break;

  case 106:
#line 820 "glsl_parser.yy"
    {
      (yyval.expression) = (yyvsp[(1) - (1)].expression);
   ;}
    break;

  case 107:
#line 824 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      if ((yyvsp[(1) - (3)].expression)->oper != ast_sequence) {
         (yyval.expression) = new(ctx) ast_expression(ast_sequence, NULL, NULL, NULL);
         (yyval.expression)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
         (yyval.expression)->expressions.push_tail(& (yyvsp[(1) - (3)].expression)->link);
      } else {
         (yyval.expression) = (yyvsp[(1) - (3)].expression);
      }

      (yyval.expression)->expressions.push_tail(& (yyvsp[(3) - (3)].expression)->link);
   ;}
    break;

  case 109:
#line 844 "glsl_parser.yy"
    {
      state->symbols->pop_scope();
      (yyval.node) = (yyvsp[(1) - (2)].function);
   ;}
    break;

  case 110:
#line 849 "glsl_parser.yy"
    {
      (yyval.node) = (yyvsp[(1) - (2)].declarator_list);
   ;}
    break;

  case 111:
#line 853 "glsl_parser.yy"
    {
      (yyvsp[(3) - (4)].type_specifier)->default_precision = (yyvsp[(2) - (4)].n);
      (yyval.node) = (yyvsp[(3) - (4)].type_specifier);
   ;}
    break;

  case 112:
#line 858 "glsl_parser.yy"
    {
      ast_interface_block *block = (ast_interface_block *) (yyvsp[(1) - (1)].node);
      if (block->layout.has_layout() || block->layout.has_memory()) {
         if (!block->default_layout.merge_qualifier(& (yylsp[(1) - (1)]), state, block->layout, false)) {
            YYERROR;
         }
      }
      block->layout = block->default_layout;
      if (!block->layout.push_to_global(& (yylsp[(1) - (1)]), state)) {
         YYERROR;
      }
      (yyval.node) = (yyvsp[(1) - (1)].node);
   ;}
    break;

  case 116:
#line 884 "glsl_parser.yy"
    {
      (yyval.function) = (yyvsp[(1) - (2)].function);
      (yyval.function)->parameters.push_tail(& (yyvsp[(2) - (2)].parameter_declarator)->link);
   ;}
    break;

  case 117:
#line 889 "glsl_parser.yy"
    {
      (yyval.function) = (yyvsp[(1) - (3)].function);
      (yyval.function)->parameters.push_tail(& (yyvsp[(3) - (3)].parameter_declarator)->link);
   ;}
    break;

  case 118:
#line 897 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.function) = new(ctx) ast_function();
      (yyval.function)->set_location((yylsp[(2) - (3)]));
      (yyval.function)->return_type = (yyvsp[(1) - (3)].fully_specified_type);
      (yyval.function)->identifier = (yyvsp[(2) - (3)].identifier);

      if ((yyvsp[(1) - (3)].fully_specified_type)->qualifier.is_subroutine_decl()) {
         /* add type for IDENTIFIER search */
         state->symbols->add_type((yyvsp[(2) - (3)].identifier), glsl_subroutine_type((yyvsp[(2) - (3)].identifier)));
      } else
         state->symbols->add_function(new(state) ir_function((yyvsp[(2) - (3)].identifier)));
      state->symbols->push_scope();
   ;}
    break;

  case 119:
#line 915 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.parameter_declarator) = new(ctx) ast_parameter_declarator();
      (yyval.parameter_declarator)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
      (yyval.parameter_declarator)->type = new(ctx) ast_fully_specified_type();
      (yyval.parameter_declarator)->type->set_location((yylsp[(1) - (2)]));
      (yyval.parameter_declarator)->type->specifier = (yyvsp[(1) - (2)].type_specifier);
      (yyval.parameter_declarator)->identifier = (yyvsp[(2) - (2)].identifier);
      state->symbols->add_variable(new(state) ir_variable(NULL, (yyvsp[(2) - (2)].identifier), ir_var_auto));
   ;}
    break;

  case 120:
#line 926 "glsl_parser.yy"
    {
      _mesa_glsl_error(&(yylsp[(1) - (3)]), state, "is is not allowed on function parameter");
      YYERROR;
   ;}
    break;

  case 121:
#line 931 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.parameter_declarator) = new(ctx) ast_parameter_declarator();
      (yyval.parameter_declarator)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
      (yyval.parameter_declarator)->type = new(ctx) ast_fully_specified_type();
      (yyval.parameter_declarator)->type->set_location((yylsp[(1) - (3)]));
      (yyval.parameter_declarator)->type->specifier = (yyvsp[(1) - (3)].type_specifier);
      (yyval.parameter_declarator)->identifier = (yyvsp[(2) - (3)].identifier);
      (yyval.parameter_declarator)->array_specifier = (yyvsp[(3) - (3)].array_specifier);
      state->symbols->add_variable(new(state) ir_variable(NULL, (yyvsp[(2) - (3)].identifier), ir_var_auto));
   ;}
    break;

  case 122:
#line 946 "glsl_parser.yy"
    {
      (yyval.parameter_declarator) = (yyvsp[(2) - (2)].parameter_declarator);
      (yyval.parameter_declarator)->type->qualifier = (yyvsp[(1) - (2)].type_qualifier);
      if (!(yyval.parameter_declarator)->type->qualifier.push_to_global(& (yylsp[(1) - (2)]), state)) {
         YYERROR;
      }
   ;}
    break;

  case 123:
#line 954 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.parameter_declarator) = new(ctx) ast_parameter_declarator();
      (yyval.parameter_declarator)->set_location((yylsp[(2) - (2)]));
      (yyval.parameter_declarator)->type = new(ctx) ast_fully_specified_type();
      (yyval.parameter_declarator)->type->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
      (yyval.parameter_declarator)->type->qualifier = (yyvsp[(1) - (2)].type_qualifier);
      if (!(yyval.parameter_declarator)->type->qualifier.push_to_global(& (yylsp[(1) - (2)]), state)) {
         YYERROR;
      }
      (yyval.parameter_declarator)->type->specifier = (yyvsp[(2) - (2)].type_specifier);
   ;}
    break;

  case 124:
#line 970 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
   ;}
    break;

  case 125:
#line 974 "glsl_parser.yy"
    {
      if ((yyvsp[(2) - (2)].type_qualifier).flags.q.constant)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "duplicate const qualifier");

      (yyval.type_qualifier) = (yyvsp[(2) - (2)].type_qualifier);
      (yyval.type_qualifier).flags.q.constant = 1;
   ;}
    break;

  case 126:
#line 982 "glsl_parser.yy"
    {
      if ((yyvsp[(2) - (2)].type_qualifier).flags.q.precise)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "duplicate precise qualifier");

      (yyval.type_qualifier) = (yyvsp[(2) - (2)].type_qualifier);
      (yyval.type_qualifier).flags.q.precise = 1;
   ;}
    break;

  case 127:
#line 990 "glsl_parser.yy"
    {
      if (((yyvsp[(1) - (2)].type_qualifier).flags.q.in || (yyvsp[(1) - (2)].type_qualifier).flags.q.out) && ((yyvsp[(2) - (2)].type_qualifier).flags.q.in || (yyvsp[(2) - (2)].type_qualifier).flags.q.out))
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "duplicate in/out/inout qualifier");

      if (!state->has_420pack_or_es31() && (yyvsp[(2) - (2)].type_qualifier).flags.q.constant)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "in/out/inout must come after const "
                                      "or precise");

      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false);
   ;}
    break;

  case 128:
#line 1002 "glsl_parser.yy"
    {
      if ((yyvsp[(2) - (2)].type_qualifier).precision != ast_precision_none)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "duplicate precision qualifier");

      if (!state->has_420pack_or_es31() &&
          (yyvsp[(2) - (2)].type_qualifier).flags.i != 0)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "precision qualifiers must come last");

      (yyval.type_qualifier) = (yyvsp[(2) - (2)].type_qualifier);
      (yyval.type_qualifier).precision = (yyvsp[(1) - (2)].n);
   ;}
    break;

  case 129:
#line 1014 "glsl_parser.yy"
    {
      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false);
   ;}
    break;

  case 130:
#line 1021 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.in = 1;
   ;}
    break;

  case 131:
#line 1026 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.out = 1;
   ;}
    break;

  case 132:
#line 1031 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.in = 1;
      (yyval.type_qualifier).flags.q.out = 1;
   ;}
    break;

  case 135:
#line 1045 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(3) - (3)].identifier), NULL, NULL);
      decl->set_location((yylsp[(3) - (3)]));

      (yyval.declarator_list) = (yyvsp[(1) - (3)].declarator_list);
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(state) ir_variable(NULL, (yyvsp[(3) - (3)].identifier), ir_var_auto));
   ;}
    break;

  case 136:
#line 1055 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(3) - (4)].identifier), (yyvsp[(4) - (4)].array_specifier), NULL);
      decl->set_location_range((yylsp[(3) - (4)]), (yylsp[(4) - (4)]));

      (yyval.declarator_list) = (yyvsp[(1) - (4)].declarator_list);
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(state) ir_variable(NULL, (yyvsp[(3) - (4)].identifier), ir_var_auto));
   ;}
    break;

  case 137:
#line 1065 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(3) - (6)].identifier), (yyvsp[(4) - (6)].array_specifier), (yyvsp[(6) - (6)].expression));
      decl->set_location_range((yylsp[(3) - (6)]), (yylsp[(4) - (6)]));

      (yyval.declarator_list) = (yyvsp[(1) - (6)].declarator_list);
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(state) ir_variable(NULL, (yyvsp[(3) - (6)].identifier), ir_var_auto));
   ;}
    break;

  case 138:
#line 1075 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(3) - (5)].identifier), NULL, (yyvsp[(5) - (5)].expression));
      decl->set_location((yylsp[(3) - (5)]));

      (yyval.declarator_list) = (yyvsp[(1) - (5)].declarator_list);
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(state) ir_variable(NULL, (yyvsp[(3) - (5)].identifier), ir_var_auto));
   ;}
    break;

  case 139:
#line 1089 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      /* Empty declaration list is valid. */
      (yyval.declarator_list) = new(ctx) ast_declarator_list((yyvsp[(1) - (1)].fully_specified_type));
      (yyval.declarator_list)->set_location((yylsp[(1) - (1)]));
   ;}
    break;

  case 140:
#line 1096 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(2) - (2)].identifier), NULL, NULL);
      decl->set_location((yylsp[(2) - (2)]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list((yyvsp[(1) - (2)].fully_specified_type));
      (yyval.declarator_list)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(state) ir_variable(NULL, (yyvsp[(2) - (2)].identifier), ir_var_auto));
   ;}
    break;

  case 141:
#line 1107 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(2) - (3)].identifier), (yyvsp[(3) - (3)].array_specifier), NULL);
      decl->set_location_range((yylsp[(2) - (3)]), (yylsp[(3) - (3)]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list((yyvsp[(1) - (3)].fully_specified_type));
      (yyval.declarator_list)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(state) ir_variable(NULL, (yyvsp[(2) - (3)].identifier), ir_var_auto));
   ;}
    break;

  case 142:
#line 1118 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(2) - (5)].identifier), (yyvsp[(3) - (5)].array_specifier), (yyvsp[(5) - (5)].expression));
      decl->set_location_range((yylsp[(2) - (5)]), (yylsp[(3) - (5)]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list((yyvsp[(1) - (5)].fully_specified_type));
      (yyval.declarator_list)->set_location_range((yylsp[(1) - (5)]), (yylsp[(3) - (5)]));
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(state) ir_variable(NULL, (yyvsp[(2) - (5)].identifier), ir_var_auto));
   ;}
    break;

  case 143:
#line 1129 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(2) - (4)].identifier), NULL, (yyvsp[(4) - (4)].expression));
      decl->set_location((yylsp[(2) - (4)]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list((yyvsp[(1) - (4)].fully_specified_type));
      (yyval.declarator_list)->set_location_range((yylsp[(1) - (4)]), (yylsp[(2) - (4)]));
      (yyval.declarator_list)->declarations.push_tail(&decl->link);
      state->symbols->add_variable(new(state) ir_variable(NULL, (yyvsp[(2) - (4)].identifier), ir_var_auto));
   ;}
    break;

  case 144:
#line 1140 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(2) - (2)].identifier), NULL, NULL);
      decl->set_location((yylsp[(2) - (2)]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list(NULL);
      (yyval.declarator_list)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
      (yyval.declarator_list)->invariant = true;

      (yyval.declarator_list)->declarations.push_tail(&decl->link);
   ;}
    break;

  case 145:
#line 1152 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(2) - (2)].identifier), NULL, NULL);
      decl->set_location((yylsp[(2) - (2)]));

      (yyval.declarator_list) = new(ctx) ast_declarator_list(NULL);
      (yyval.declarator_list)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
      (yyval.declarator_list)->precise = true;

      (yyval.declarator_list)->declarations.push_tail(&decl->link);
   ;}
    break;

  case 146:
#line 1167 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.fully_specified_type) = new(ctx) ast_fully_specified_type();
      (yyval.fully_specified_type)->set_location((yylsp[(1) - (1)]));
      (yyval.fully_specified_type)->specifier = (yyvsp[(1) - (1)].type_specifier);
   ;}
    break;

  case 147:
#line 1174 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.fully_specified_type) = new(ctx) ast_fully_specified_type();
      (yyval.fully_specified_type)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
      (yyval.fully_specified_type)->qualifier = (yyvsp[(1) - (2)].type_qualifier);
      if (!(yyval.fully_specified_type)->qualifier.push_to_global(& (yylsp[(1) - (2)]), state)) {
         YYERROR;
      }
      (yyval.fully_specified_type)->specifier = (yyvsp[(2) - (2)].type_specifier);
      if ((yyval.fully_specified_type)->specifier->structure != NULL &&
          (yyval.fully_specified_type)->specifier->structure->is_declaration) {
            (yyval.fully_specified_type)->specifier->structure->layout = &(yyval.fully_specified_type)->qualifier;
      }
   ;}
    break;

  case 148:
#line 1192 "glsl_parser.yy"
    {
      (yyval.type_qualifier) = (yyvsp[(3) - (4)].type_qualifier);
   ;}
    break;

  case 150:
#line 1200 "glsl_parser.yy"
    {
      (yyval.type_qualifier) = (yyvsp[(1) - (3)].type_qualifier);
      if (!(yyval.type_qualifier).merge_qualifier(& (yylsp[(3) - (3)]), state, (yyvsp[(3) - (3)].type_qualifier), true)) {
         YYERROR;
      }
   ;}
    break;

  case 151:
#line 1210 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));

      /* Layout qualifiers for ARB_fragment_coord_conventions. */
      if (!(yyval.type_qualifier).flags.i && (state->ARB_fragment_coord_conventions_enable ||
                          state->is_version(150, 0))) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "origin_upper_left", state) == 0) {
            (yyval.type_qualifier).flags.q.origin_upper_left = 1;
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "pixel_center_integer",
                                           state) == 0) {
            (yyval.type_qualifier).flags.q.pixel_center_integer = 1;
         }

         if ((yyval.type_qualifier).flags.i && state->ARB_fragment_coord_conventions_warn) {
            _mesa_glsl_warning(& (yylsp[(1) - (1)]), state,
                               "GL_ARB_fragment_coord_conventions layout "
                               "identifier `%s' used", (yyvsp[(1) - (1)].identifier));
         }
      }

      /* Layout qualifiers for AMD/ARB_conservative_depth. */
      if (!(yyval.type_qualifier).flags.i &&
          (state->AMD_conservative_depth_enable ||
           state->ARB_conservative_depth_enable ||
           state->EXT_conservative_depth_enable ||
           state->is_version(420, 0))) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "depth_any", state) == 0) {
            (yyval.type_qualifier).flags.q.depth_type = 1;
            (yyval.type_qualifier).depth_type = ast_depth_any;
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "depth_greater", state) == 0) {
            (yyval.type_qualifier).flags.q.depth_type = 1;
            (yyval.type_qualifier).depth_type = ast_depth_greater;
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "depth_less", state) == 0) {
            (yyval.type_qualifier).flags.q.depth_type = 1;
            (yyval.type_qualifier).depth_type = ast_depth_less;
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "depth_unchanged",
                                           state) == 0) {
            (yyval.type_qualifier).flags.q.depth_type = 1;
            (yyval.type_qualifier).depth_type = ast_depth_unchanged;
         }

         if ((yyval.type_qualifier).flags.i && state->AMD_conservative_depth_warn) {
            _mesa_glsl_warning(& (yylsp[(1) - (1)]), state,
                               "GL_AMD_conservative_depth "
                               "layout qualifier `%s' is used", (yyvsp[(1) - (1)].identifier));
         }
         if ((yyval.type_qualifier).flags.i && state->ARB_conservative_depth_warn) {
            _mesa_glsl_warning(& (yylsp[(1) - (1)]), state,
                               "GL_ARB_conservative_depth "
                               "layout qualifier `%s' is used", (yyvsp[(1) - (1)].identifier));
         }
         if ((yyval.type_qualifier).flags.i && state->EXT_conservative_depth_warn) {
            _mesa_glsl_warning(& (yylsp[(1) - (1)]), state,
                               "GL_EXT_conservative_depth "
                               "layout qualifier `%s' is used", (yyvsp[(1) - (1)].identifier));
         }
      }

      /* See also interface_block_layout_qualifier. */
      if (!(yyval.type_qualifier).flags.i && state->has_uniform_buffer_objects()) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "std140", state) == 0) {
            (yyval.type_qualifier).flags.q.std140 = 1;
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "shared", state) == 0) {
            (yyval.type_qualifier).flags.q.shared = 1;
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "std430", state) == 0) {
            (yyval.type_qualifier).flags.q.std430 = 1;
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "column_major", state) == 0) {
            (yyval.type_qualifier).flags.q.column_major = 1;
         /* "row_major" is a reserved word in GLSL 1.30+. Its token is parsed
          * below in the interface_block_layout_qualifier rule.
          *
          * It is not a reserved word in GLSL ES 3.00, so it's handled here as
          * an identifier.
          *
          * Also, this takes care of alternate capitalizations of
          * "row_major" (which is necessary because layout qualifiers
          * are case-insensitive in desktop GLSL).
          */
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "row_major", state) == 0) {
            (yyval.type_qualifier).flags.q.row_major = 1;
         /* "packed" is a reserved word in GLSL, and its token is
          * parsed below in the interface_block_layout_qualifier rule.
          * However, we must take care of alternate capitalizations of
          * "packed", because layout qualifiers are case-insensitive
          * in desktop GLSL.
          */
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "packed", state) == 0) {
           (yyval.type_qualifier).flags.q.packed = 1;
         }

         if ((yyval.type_qualifier).flags.i && state->ARB_uniform_buffer_object_warn) {
            _mesa_glsl_warning(& (yylsp[(1) - (1)]), state,
                               "#version 140 / GL_ARB_uniform_buffer_object "
                               "layout qualifier `%s' is used", (yyvsp[(1) - (1)].identifier));
         }
      }

      /* Layout qualifiers for GLSL 1.50 geometry shaders. */
      if (!(yyval.type_qualifier).flags.i) {
         static const struct {
            const char *s;
            GLenum e;
         } map[] = {
                 { "points", GL_POINTS },
                 { "lines", GL_LINES },
                 { "lines_adjacency", GL_LINES_ADJACENCY },
                 { "line_strip", GL_LINE_STRIP },
                 { "triangles", GL_TRIANGLES },
                 { "triangles_adjacency", GL_TRIANGLES_ADJACENCY },
                 { "triangle_strip", GL_TRIANGLE_STRIP },
         };
         for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
            if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), map[i].s, state) == 0) {
               (yyval.type_qualifier).flags.q.prim_type = 1;
               (yyval.type_qualifier).prim_type = map[i].e;
               break;
            }
         }

         if ((yyval.type_qualifier).flags.i && !state->has_geometry_shader() &&
             !state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state, "#version 150 layout "
                             "qualifier `%s' used", (yyvsp[(1) - (1)].identifier));
         }
      }

      /* Layout qualifiers for ARB_shader_image_load_store. */
      if (state->has_shader_image_load_store()) {
         if (!(yyval.type_qualifier).flags.i) {
            static const struct {
               const char *name;
               enum pipe_format format;
               glsl_base_type base_type;
               /** Minimum desktop GLSL version required for the image
                * format.  Use 130 if already present in the original
                * ARB extension.
                */
               unsigned required_glsl;
               /** Minimum GLSL ES version required for the image format. */
               unsigned required_essl;
               /* NV_image_formats */
               bool nv_image_formats;
               bool ext_qualifiers;
            } map[] = {
               { "rgba32f", PIPE_FORMAT_R32G32B32A32_FLOAT, GLSL_TYPE_FLOAT, 130, 310, false, false },
               { "rgba16f", PIPE_FORMAT_R16G16B16A16_FLOAT, GLSL_TYPE_FLOAT, 130, 310, false, false },
               { "rg32f", PIPE_FORMAT_R32G32_FLOAT, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "rg16f", PIPE_FORMAT_R16G16_FLOAT, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "r11f_g11f_b10f", PIPE_FORMAT_R11G11B10_FLOAT, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "r32f", PIPE_FORMAT_R32_FLOAT, GLSL_TYPE_FLOAT, 130, 310, false, false },
               { "r16f", PIPE_FORMAT_R16_FLOAT, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "rgba32ui", PIPE_FORMAT_R32G32B32A32_UINT, GLSL_TYPE_UINT, 130, 310, false, false },
               { "rgba16ui", PIPE_FORMAT_R16G16B16A16_UINT, GLSL_TYPE_UINT, 130, 310, false, false },
               { "rgb10_a2ui", PIPE_FORMAT_R10G10B10A2_UINT, GLSL_TYPE_UINT, 130, 0, true, false },
               { "rgba8ui", PIPE_FORMAT_R8G8B8A8_UINT, GLSL_TYPE_UINT, 130, 310, false, false },
               { "rg32ui", PIPE_FORMAT_R32G32_UINT, GLSL_TYPE_UINT, 130, 0, true, false },
               { "rg16ui", PIPE_FORMAT_R16G16_UINT, GLSL_TYPE_UINT, 130, 0, true, false },
               { "rg8ui", PIPE_FORMAT_R8G8_UINT, GLSL_TYPE_UINT, 130, 0, true, false },
               { "r32ui", PIPE_FORMAT_R32_UINT, GLSL_TYPE_UINT, 130, 310, false, false },
               { "r16ui", PIPE_FORMAT_R16_UINT, GLSL_TYPE_UINT, 130, 0, true, false },
               { "r8ui", PIPE_FORMAT_R8_UINT, GLSL_TYPE_UINT, 130, 0, true, false },
               { "rgba32i", PIPE_FORMAT_R32G32B32A32_SINT, GLSL_TYPE_INT, 130, 310, false, false },
               { "rgba16i", PIPE_FORMAT_R16G16B16A16_SINT, GLSL_TYPE_INT, 130, 310, false, false },
               { "rgba8i", PIPE_FORMAT_R8G8B8A8_SINT, GLSL_TYPE_INT, 130, 310, false, false },
               { "rg32i", PIPE_FORMAT_R32G32_SINT, GLSL_TYPE_INT, 130, 0, true, false },
               { "rg16i", PIPE_FORMAT_R16G16_SINT, GLSL_TYPE_INT, 130, 0, true, false },
               { "rg8i", PIPE_FORMAT_R8G8_SINT, GLSL_TYPE_INT, 130, 0, true, false },
               { "r32i", PIPE_FORMAT_R32_SINT, GLSL_TYPE_INT, 130, 310, false, false },
               { "r16i", PIPE_FORMAT_R16_SINT, GLSL_TYPE_INT, 130, 0, true, false },
               { "r8i", PIPE_FORMAT_R8_SINT, GLSL_TYPE_INT, 130, 0, true, false },
               { "rgba16", PIPE_FORMAT_R16G16B16A16_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "rgb10_a2", PIPE_FORMAT_R10G10B10A2_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "rgba8", PIPE_FORMAT_R8G8B8A8_UNORM, GLSL_TYPE_FLOAT, 130, 310, false, false },
               { "rg16", PIPE_FORMAT_R16G16_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "rg8", PIPE_FORMAT_R8G8_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "r16", PIPE_FORMAT_R16_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "r8", PIPE_FORMAT_R8_UNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "rgba16_snorm", PIPE_FORMAT_R16G16B16A16_SNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "rgba8_snorm", PIPE_FORMAT_R8G8B8A8_SNORM, GLSL_TYPE_FLOAT, 130, 310, false, false },
               { "rg16_snorm", PIPE_FORMAT_R16G16_SNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "rg8_snorm", PIPE_FORMAT_R8G8_SNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "r16_snorm", PIPE_FORMAT_R16_SNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },
               { "r8_snorm", PIPE_FORMAT_R8_SNORM, GLSL_TYPE_FLOAT, 130, 0, true, false },

               /* From GL_EXT_shader_image_load_store: */
               /* base_type is incorrect but it'll be patched later when we know
                * the variable type. See ast_to_hir.cpp */
               { "size1x8", PIPE_FORMAT_R8_SINT, GLSL_TYPE_VOID, 130, 0, false, true },
               { "size1x16", PIPE_FORMAT_R16_SINT, GLSL_TYPE_VOID, 130, 0, false, true },
               { "size1x32", PIPE_FORMAT_R32_SINT, GLSL_TYPE_VOID, 130, 0, false, true },
               { "size2x32", PIPE_FORMAT_R32G32_SINT, GLSL_TYPE_VOID, 130, 0, false, true },
               { "size4x32", PIPE_FORMAT_R32G32B32A32_SINT, GLSL_TYPE_VOID, 130, 0, false, true },
            };

            for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
               if ((state->is_version(map[i].required_glsl,
                                      map[i].required_essl) ||
                    (state->NV_image_formats_enable &&
                     map[i].nv_image_formats)) &&
                   match_layout_qualifier((yyvsp[(1) - (1)].identifier), map[i].name, state) == 0) {
                  /* Skip ARB_shader_image_load_store qualifiers if not enabled */
                  if (!map[i].ext_qualifiers && !(state->ARB_shader_image_load_store_enable ||
                                                  state->is_version(420, 310))) {
                     continue;
                  }
                  /* Skip EXT_shader_image_load_store qualifiers if not enabled */
                  if (map[i].ext_qualifiers && !state->EXT_shader_image_load_store_enable) {
                     continue;
                  }
                  (yyval.type_qualifier).flags.q.explicit_image_format = 1;
                  (yyval.type_qualifier).image_format = map[i].format;
                  (yyval.type_qualifier).image_base_type = map[i].base_type;
                  break;
               }
            }
         }
      }

      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "early_fragment_tests", state) == 0) {
            /* From section 4.4.1.3 of the GLSL 4.50 specification
             * (Fragment Shader Inputs):
             *
             *  "Fragment shaders also allow the following layout
             *   qualifier on in only (not with variable declarations)
             *     layout-qualifier-id
             *        early_fragment_tests
             *   [...]"
             */
            if (state->stage != MESA_SHADER_FRAGMENT) {
               _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                                "early_fragment_tests layout qualifier only "
                                "valid in fragment shaders");
            }

            (yyval.type_qualifier).flags.q.early_fragment_tests = 1;
         }

         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "inner_coverage", state) == 0) {
            if (state->stage != MESA_SHADER_FRAGMENT) {
               _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                                "inner_coverage layout qualifier only "
                                "valid in fragment shaders");
            }

            if (state->INTEL_conservative_rasterization_enable) {
               (yyval.type_qualifier).flags.q.inner_coverage = 1;
            } else {
               _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                                "inner_coverage layout qualifier present, "
                                "but the INTEL_conservative_rasterization extension "
                                "is not enabled.");
            }
         }

         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "post_depth_coverage", state) == 0) {
            if (state->stage != MESA_SHADER_FRAGMENT) {
               _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                                "post_depth_coverage layout qualifier only "
                                "valid in fragment shaders");
            }

            if (state->ARB_post_depth_coverage_enable ||
                state->INTEL_conservative_rasterization_enable) {
               (yyval.type_qualifier).flags.q.post_depth_coverage = 1;
            } else {
               _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                                "post_depth_coverage layout qualifier present, "
                                "but the GL_ARB_post_depth_coverage extension "
                                "is not enabled.");
            }
         }

         if ((yyval.type_qualifier).flags.q.post_depth_coverage && (yyval.type_qualifier).flags.q.inner_coverage) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                             "post_depth_coverage & inner_coverage layout qualifiers "
                             "are mutually exclusive");
         }
      }

      const bool pixel_interlock_ordered = match_layout_qualifier((yyvsp[(1) - (1)].identifier),
         "pixel_interlock_ordered", state) == 0;
      const bool pixel_interlock_unordered = match_layout_qualifier((yyvsp[(1) - (1)].identifier),
         "pixel_interlock_unordered", state) == 0;
      const bool sample_interlock_ordered = match_layout_qualifier((yyvsp[(1) - (1)].identifier),
         "sample_interlock_ordered", state) == 0;
      const bool sample_interlock_unordered = match_layout_qualifier((yyvsp[(1) - (1)].identifier),
         "sample_interlock_unordered", state) == 0;

      if (pixel_interlock_ordered + pixel_interlock_unordered +
          sample_interlock_ordered + sample_interlock_unordered > 0 &&
          state->stage != MESA_SHADER_FRAGMENT) {
         _mesa_glsl_error(& (yylsp[(1) - (1)]), state, "interlock layout qualifiers: "
                          "pixel_interlock_ordered, pixel_interlock_unordered, "
                          "sample_interlock_ordered and sample_interlock_unordered, "
                          "only valid in fragment shader input layout declaration.");
      } else if (pixel_interlock_ordered + pixel_interlock_unordered +
                 sample_interlock_ordered + sample_interlock_unordered > 0 &&
                 !state->ARB_fragment_shader_interlock_enable &&
                 !state->NV_fragment_shader_interlock_enable) {
         _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                          "interlock layout qualifier present, but the "
                          "GL_ARB_fragment_shader_interlock or "
                          "GL_NV_fragment_shader_interlock extension is not "
                          "enabled.");
      } else {
         (yyval.type_qualifier).flags.q.pixel_interlock_ordered = pixel_interlock_ordered;
         (yyval.type_qualifier).flags.q.pixel_interlock_unordered = pixel_interlock_unordered;
         (yyval.type_qualifier).flags.q.sample_interlock_ordered = sample_interlock_ordered;
         (yyval.type_qualifier).flags.q.sample_interlock_unordered = sample_interlock_unordered;
      }

      /* Layout qualifiers for tessellation evaluation shaders. */
      if (!(yyval.type_qualifier).flags.i) {
         static const struct {
            const char *s;
            GLenum e;
         } map[] = {
                 /* triangles already parsed by gs-specific code */
                 { "quads", GL_QUADS },
                 { "isolines", GL_ISOLINES },
         };
         for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
            if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), map[i].s, state) == 0) {
               (yyval.type_qualifier).flags.q.prim_type = 1;
               (yyval.type_qualifier).prim_type = map[i].e;
               break;
            }
         }

         if ((yyval.type_qualifier).flags.i && !state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                             "primitive mode qualifier `%s' requires "
                             "GLSL 4.00 or ARB_tessellation_shader", (yyvsp[(1) - (1)].identifier));
         }
      }
      if (!(yyval.type_qualifier).flags.i) {
         static const struct {
            const char *s;
            enum gl_tess_spacing e;
         } map[] = {
                 { "equal_spacing", TESS_SPACING_EQUAL },
                 { "fractional_odd_spacing", TESS_SPACING_FRACTIONAL_ODD },
                 { "fractional_even_spacing", TESS_SPACING_FRACTIONAL_EVEN },
         };
         for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
            if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), map[i].s, state) == 0) {
               (yyval.type_qualifier).flags.q.vertex_spacing = 1;
               (yyval.type_qualifier).vertex_spacing = map[i].e;
               break;
            }
         }

         if ((yyval.type_qualifier).flags.i && !state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                             "vertex spacing qualifier `%s' requires "
                             "GLSL 4.00 or ARB_tessellation_shader", (yyvsp[(1) - (1)].identifier));
         }
      }
      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "cw", state) == 0) {
            (yyval.type_qualifier).flags.q.ordering = 1;
            (yyval.type_qualifier).ordering = GL_CW;
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "ccw", state) == 0) {
            (yyval.type_qualifier).flags.q.ordering = 1;
            (yyval.type_qualifier).ordering = GL_CCW;
         }

         if ((yyval.type_qualifier).flags.i && !state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                             "ordering qualifier `%s' requires "
                             "GLSL 4.00 or ARB_tessellation_shader", (yyvsp[(1) - (1)].identifier));
         }
      }
      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "point_mode", state) == 0) {
            (yyval.type_qualifier).flags.q.point_mode = 1;
            (yyval.type_qualifier).point_mode = true;
         }

         if ((yyval.type_qualifier).flags.i && !state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                             "qualifier `point_mode' requires "
                             "GLSL 4.00 or ARB_tessellation_shader");
         }
      }

      if (!(yyval.type_qualifier).flags.i) {
         static const struct {
            const char *s;
            uint32_t mask;
         } map[] = {
                 { "blend_support_multiply",       BITFIELD_BIT(BLEND_MULTIPLY) },
                 { "blend_support_screen",         BITFIELD_BIT(BLEND_SCREEN) },
                 { "blend_support_overlay",        BITFIELD_BIT(BLEND_OVERLAY) },
                 { "blend_support_darken",         BITFIELD_BIT(BLEND_DARKEN) },
                 { "blend_support_lighten",        BITFIELD_BIT(BLEND_LIGHTEN) },
                 { "blend_support_colordodge",     BITFIELD_BIT(BLEND_COLORDODGE) },
                 { "blend_support_colorburn",      BITFIELD_BIT(BLEND_COLORBURN) },
                 { "blend_support_hardlight",      BITFIELD_BIT(BLEND_HARDLIGHT) },
                 { "blend_support_softlight",      BITFIELD_BIT(BLEND_SOFTLIGHT) },
                 { "blend_support_difference",     BITFIELD_BIT(BLEND_DIFFERENCE) },
                 { "blend_support_exclusion",      BITFIELD_BIT(BLEND_EXCLUSION) },
                 { "blend_support_hsl_hue",        BITFIELD_BIT(BLEND_HSL_HUE) },
                 { "blend_support_hsl_saturation", BITFIELD_BIT(BLEND_HSL_SATURATION) },
                 { "blend_support_hsl_color",      BITFIELD_BIT(BLEND_HSL_COLOR) },
                 { "blend_support_hsl_luminosity", BITFIELD_BIT(BLEND_HSL_LUMINOSITY) },
                 { "blend_support_all_equations",  (1u << (BLEND_HSL_LUMINOSITY + 1)) - 2 },
         };
         for (unsigned i = 0; i < ARRAY_SIZE(map); i++) {
            if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), map[i].s, state) == 0) {
               (yyval.type_qualifier).flags.q.blend_support = 1;
               state->fs_blend_support |= map[i].mask;
               break;
            }
         }

         if ((yyval.type_qualifier).flags.i &&
             !state->KHR_blend_equation_advanced_enable &&
             !state->is_version(0, 320)) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                             "advanced blending layout qualifiers require "
                             "ESSL 3.20 or KHR_blend_equation_advanced");
         }

         if ((yyval.type_qualifier).flags.i && state->stage != MESA_SHADER_FRAGMENT) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                             "advanced blending layout qualifiers only "
                             "valid in fragment shaders");
         }
      }

      /* Layout qualifiers for ARB_compute_variable_group_size. */
      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "local_size_variable", state) == 0) {
            (yyval.type_qualifier).flags.q.local_size_variable = 1;
         }

         if ((yyval.type_qualifier).flags.i && !state->ARB_compute_variable_group_size_enable) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                             "qualifier `local_size_variable` requires "
                             "ARB_compute_variable_group_size");
         }
      }

      /* Layout qualifiers for ARB_bindless_texture. */
      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "bindless_sampler", state) == 0)
            (yyval.type_qualifier).flags.q.bindless_sampler = 1;
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "bound_sampler", state) == 0)
            (yyval.type_qualifier).flags.q.bound_sampler = 1;

         if (state->has_shader_image_load_store()) {
            if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "bindless_image", state) == 0)
               (yyval.type_qualifier).flags.q.bindless_image = 1;
            if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "bound_image", state) == 0)
               (yyval.type_qualifier).flags.q.bound_image = 1;
         }

         if ((yyval.type_qualifier).flags.i && !state->has_bindless()) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                             "qualifier `%s` requires "
                             "ARB_bindless_texture", (yyvsp[(1) - (1)].identifier));
         }
      }

      if (!(yyval.type_qualifier).flags.i &&
          state->EXT_shader_framebuffer_fetch_non_coherent_enable) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "noncoherent", state) == 0)
            (yyval.type_qualifier).flags.q.non_coherent = 1;
      }

      // Layout qualifiers for NV_compute_shader_derivatives.
      if (!(yyval.type_qualifier).flags.i) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "derivative_group_quadsNV", state) == 0) {
            (yyval.type_qualifier).flags.q.derivative_group = 1;
            (yyval.type_qualifier).derivative_group = DERIVATIVE_GROUP_QUADS;
         } else if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "derivative_group_linearNV", state) == 0) {
            (yyval.type_qualifier).flags.q.derivative_group = 1;
            (yyval.type_qualifier).derivative_group = DERIVATIVE_GROUP_LINEAR;
         }

         if ((yyval.type_qualifier).flags.i) {
            if (!state->has_compute_shader()) {
               _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                                "qualifier `%s' requires "
                                "a compute shader", (yyvsp[(1) - (1)].identifier));
            }

            if (!state->NV_compute_shader_derivatives_enable) {
               _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                                "qualifier `%s' requires "
                                "NV_compute_shader_derivatives", (yyvsp[(1) - (1)].identifier));
            }

            if (state->NV_compute_shader_derivatives_warn) {
               _mesa_glsl_warning(& (yylsp[(1) - (1)]), state,
                                  "NV_compute_shader_derivatives layout "
                                  "qualifier `%s' used", (yyvsp[(1) - (1)].identifier));
            }
         }
      }

      /* Layout qualifier for NV_viewport_array2. */
      if (!(yyval.type_qualifier).flags.i && state->stage != MESA_SHADER_FRAGMENT) {
         if (match_layout_qualifier((yyvsp[(1) - (1)].identifier), "viewport_relative", state) == 0) {
            (yyval.type_qualifier).flags.q.viewport_relative = 1;
         }

         if ((yyval.type_qualifier).flags.i && !state->NV_viewport_array2_enable) {
            _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                             "qualifier `%s' requires "
                             "GL_NV_viewport_array2", (yyvsp[(1) - (1)].identifier));
         }

         if ((yyval.type_qualifier).flags.i && state->NV_viewport_array2_warn) {
            _mesa_glsl_warning(& (yylsp[(1) - (1)]), state,
                               "GL_NV_viewport_array2 layout "
                               "identifier `%s' used", (yyvsp[(1) - (1)].identifier));
         }
      }

      if (!(yyval.type_qualifier).flags.i) {
         _mesa_glsl_error(& (yylsp[(1) - (1)]), state, "unrecognized layout identifier "
                          "`%s'", (yyvsp[(1) - (1)].identifier));
         YYERROR;
      }
   ;}
    break;

  case 152:
#line 1739 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      linear_ctx *ctx = state->linalloc;

      if ((yyvsp[(3) - (3)].expression)->oper != ast_int_constant &&
          (yyvsp[(3) - (3)].expression)->oper != ast_uint_constant &&
          !state->has_enhanced_layouts()) {
         _mesa_glsl_error(& (yylsp[(1) - (3)]), state,
                          "compile-time constant expressions require "
                          "GLSL 4.40 or ARB_enhanced_layouts");
      }

      if (match_layout_qualifier("align", (yyvsp[(1) - (3)].identifier), state) == 0) {
         if (!state->has_enhanced_layouts()) {
            _mesa_glsl_error(& (yylsp[(1) - (3)]), state,
                             "align qualifier requires "
                             "GLSL 4.40 or ARB_enhanced_layouts");
         } else {
            (yyval.type_qualifier).flags.q.explicit_align = 1;
            (yyval.type_qualifier).align = (yyvsp[(3) - (3)].expression);
         }
      }

      if (match_layout_qualifier("location", (yyvsp[(1) - (3)].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.explicit_location = 1;

         if ((yyval.type_qualifier).flags.q.attribute == 1 &&
             state->ARB_explicit_attrib_location_warn) {
            _mesa_glsl_warning(& (yylsp[(1) - (3)]), state,
                               "GL_ARB_explicit_attrib_location layout "
                               "identifier `%s' used", (yyvsp[(1) - (3)].identifier));
         }
         (yyval.type_qualifier).location = (yyvsp[(3) - (3)].expression);
      }

      if (match_layout_qualifier("num_views", (yyvsp[(1) - (3)].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.explicit_numviews = 1;
         (yyval.type_qualifier).num_views = (yyvsp[(3) - (3)].expression);
      }

      if (match_layout_qualifier("component", (yyvsp[(1) - (3)].identifier), state) == 0) {
         if (!state->has_enhanced_layouts()) {
            _mesa_glsl_error(& (yylsp[(1) - (3)]), state,
                             "component qualifier requires "
                             "GLSL 4.40 or ARB_enhanced_layouts");
         } else {
            (yyval.type_qualifier).flags.q.explicit_component = 1;
            (yyval.type_qualifier).component = (yyvsp[(3) - (3)].expression);
         }
      }

      if (match_layout_qualifier("index", (yyvsp[(1) - (3)].identifier), state) == 0) {
         if (state->es_shader && !state->EXT_blend_func_extended_enable) {
            _mesa_glsl_error(& (yylsp[(3) - (3)]), state, "index layout qualifier requires EXT_blend_func_extended");
            YYERROR;
         }

         (yyval.type_qualifier).flags.q.explicit_index = 1;
         (yyval.type_qualifier).index = (yyvsp[(3) - (3)].expression);
      }

      if ((state->has_420pack_or_es31() ||
           state->has_atomic_counters() ||
           state->has_shader_storage_buffer_objects()) &&
          match_layout_qualifier("binding", (yyvsp[(1) - (3)].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.explicit_binding = 1;
         (yyval.type_qualifier).binding = (yyvsp[(3) - (3)].expression);
      }

      if ((state->has_atomic_counters() ||
           state->has_enhanced_layouts()) &&
          match_layout_qualifier("offset", (yyvsp[(1) - (3)].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.explicit_offset = 1;
         (yyval.type_qualifier).offset = (yyvsp[(3) - (3)].expression);
      }

      if (match_layout_qualifier("max_vertices", (yyvsp[(1) - (3)].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.max_vertices = 1;
         (yyval.type_qualifier).max_vertices = new(ctx) ast_layout_expression((yylsp[(1) - (3)]), (yyvsp[(3) - (3)].expression));
         if (!state->has_geometry_shader()) {
            _mesa_glsl_error(& (yylsp[(3) - (3)]), state,
                             "#version 150 max_vertices qualifier "
                             "specified");
         }
      }

      if (state->stage == MESA_SHADER_GEOMETRY) {
         if (match_layout_qualifier("stream", (yyvsp[(1) - (3)].identifier), state) == 0 &&
             state->check_explicit_attrib_stream_allowed(& (yylsp[(3) - (3)]))) {
            (yyval.type_qualifier).flags.q.stream = 1;
            (yyval.type_qualifier).flags.q.explicit_stream = 1;
            (yyval.type_qualifier).stream = (yyvsp[(3) - (3)].expression);
         }
      }

      if (state->has_enhanced_layouts()) {
         if (match_layout_qualifier("xfb_buffer", (yyvsp[(1) - (3)].identifier), state) == 0) {
            (yyval.type_qualifier).flags.q.xfb_buffer = 1;
            (yyval.type_qualifier).flags.q.explicit_xfb_buffer = 1;
            (yyval.type_qualifier).xfb_buffer = (yyvsp[(3) - (3)].expression);
         }

         if (match_layout_qualifier("xfb_offset", (yyvsp[(1) - (3)].identifier), state) == 0) {
            (yyval.type_qualifier).flags.q.explicit_xfb_offset = 1;
            (yyval.type_qualifier).offset = (yyvsp[(3) - (3)].expression);
         }

         if (match_layout_qualifier("xfb_stride", (yyvsp[(1) - (3)].identifier), state) == 0) {
            (yyval.type_qualifier).flags.q.xfb_stride = 1;
            (yyval.type_qualifier).flags.q.explicit_xfb_stride = 1;
            (yyval.type_qualifier).xfb_stride = (yyvsp[(3) - (3)].expression);
         }
      }

      static const char * const local_size_qualifiers[3] = {
         "local_size_x",
         "local_size_y",
         "local_size_z",
      };
      for (int i = 0; i < 3; i++) {
         if (match_layout_qualifier(local_size_qualifiers[i], (yyvsp[(1) - (3)].identifier),
                                    state) == 0) {
            if (!state->has_compute_shader()) {
               _mesa_glsl_error(& (yylsp[(3) - (3)]), state,
                                "%s qualifier requires GLSL 4.30 or "
                                "GLSL ES 3.10 or ARB_compute_shader",
                                local_size_qualifiers[i]);
               YYERROR;
            } else {
               (yyval.type_qualifier).flags.q.local_size |= (1 << i);
               (yyval.type_qualifier).local_size[i] = new(ctx) ast_layout_expression((yylsp[(1) - (3)]), (yyvsp[(3) - (3)].expression));
            }
            break;
         }
      }

      if (match_layout_qualifier("invocations", (yyvsp[(1) - (3)].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.invocations = 1;
         (yyval.type_qualifier).invocations = new(ctx) ast_layout_expression((yylsp[(1) - (3)]), (yyvsp[(3) - (3)].expression));
         if (!state->is_version(400, 320) &&
             !state->ARB_gpu_shader5_enable &&
             !state->OES_geometry_shader_enable &&
             !state->EXT_geometry_shader_enable) {
            _mesa_glsl_error(& (yylsp[(3) - (3)]), state,
                             "GL_ARB_gpu_shader5 invocations "
                             "qualifier specified");
         }
      }

      /* Layout qualifiers for tessellation control shaders. */
      if (match_layout_qualifier("vertices", (yyvsp[(1) - (3)].identifier), state) == 0) {
         (yyval.type_qualifier).flags.q.vertices = 1;
         (yyval.type_qualifier).vertices = new(ctx) ast_layout_expression((yylsp[(1) - (3)]), (yyvsp[(3) - (3)].expression));
         if (!state->has_tessellation_shader()) {
            _mesa_glsl_error(& (yylsp[(1) - (3)]), state,
                             "vertices qualifier requires GLSL 4.00 or "
                             "ARB_tessellation_shader");
         }
      }

      /* If the identifier didn't match any known layout identifiers,
       * emit an error.
       */
      if (!(yyval.type_qualifier).flags.i) {
         _mesa_glsl_error(& (yylsp[(1) - (3)]), state, "unrecognized layout identifier "
                          "`%s'", (yyvsp[(1) - (3)].identifier));
         YYERROR;
      }
   ;}
    break;

  case 153:
#line 1909 "glsl_parser.yy"
    {
      (yyval.type_qualifier) = (yyvsp[(1) - (1)].type_qualifier);
      /* Layout qualifiers for ARB_uniform_buffer_object. */
      if ((yyval.type_qualifier).flags.q.uniform && !state->has_uniform_buffer_objects()) {
         _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                          "#version 140 / GL_ARB_uniform_buffer_object "
                          "layout qualifier `uniform' is used");
      } else if ((yyval.type_qualifier).flags.q.uniform && state->ARB_uniform_buffer_object_warn) {
         _mesa_glsl_warning(& (yylsp[(1) - (1)]), state,
                            "#version 140 / GL_ARB_uniform_buffer_object "
                            "layout qualifier `uniform' is used");
      }
   ;}
    break;

  case 154:
#line 1935 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.row_major = 1;
   ;}
    break;

  case 155:
#line 1940 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.packed = 1;
   ;}
    break;

  case 156:
#line 1945 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.shared = 1;
   ;}
    break;

  case 157:
#line 1953 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.subroutine = 1;
   ;}
    break;

  case 158:
#line 1958 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.subroutine = 1;
      (yyval.type_qualifier).subroutine_list = (yyvsp[(3) - (4)].subroutine_list);
   ;}
    break;

  case 159:
#line 1967 "glsl_parser.yy"
    {
        linear_ctx *ctx = state->linalloc;
        ast_declaration *decl = new(ctx)  ast_declaration((yyvsp[(1) - (1)].identifier), NULL, NULL);
        decl->set_location((yylsp[(1) - (1)]));

        (yyval.subroutine_list) = new(ctx) ast_subroutine_list();
        (yyval.subroutine_list)->declarations.push_tail(&decl->link);
   ;}
    break;

  case 160:
#line 1976 "glsl_parser.yy"
    {
        linear_ctx *ctx = state->linalloc;
        ast_declaration *decl = new(ctx)  ast_declaration((yyvsp[(3) - (3)].identifier), NULL, NULL);
        decl->set_location((yylsp[(3) - (3)]));

        (yyval.subroutine_list) = (yyvsp[(1) - (3)].subroutine_list);
        (yyval.subroutine_list)->declarations.push_tail(&decl->link);
   ;}
    break;

  case 161:
#line 1988 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.smooth = 1;
   ;}
    break;

  case 162:
#line 1993 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.flat = 1;
   ;}
    break;

  case 163:
#line 1998 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.noperspective = 1;
   ;}
    break;

  case 164:
#line 2007 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.invariant = 1;
   ;}
    break;

  case 165:
#line 2012 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.precise = 1;
   ;}
    break;

  case 172:
#line 2023 "glsl_parser.yy"
    {
      memset(&(yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).precision = (yyvsp[(1) - (1)].n);
   ;}
    break;

  case 173:
#line 2041 "glsl_parser.yy"
    {
      if ((yyvsp[(2) - (2)].type_qualifier).flags.q.precise)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "duplicate \"precise\" qualifier");

      (yyval.type_qualifier) = (yyvsp[(2) - (2)].type_qualifier);
      (yyval.type_qualifier).flags.q.precise = 1;
   ;}
    break;

  case 174:
#line 2049 "glsl_parser.yy"
    {
      if ((yyvsp[(2) - (2)].type_qualifier).flags.q.invariant)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "duplicate \"invariant\" qualifier");

      if (!state->has_420pack_or_es31() && (yyvsp[(2) - (2)].type_qualifier).flags.q.precise)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state,
                          "\"invariant\" must come after \"precise\"");

      (yyval.type_qualifier) = (yyvsp[(2) - (2)].type_qualifier);
      (yyval.type_qualifier).flags.q.invariant = 1;

      /* GLSL ES 3.00 spec, section 4.6.1 "The Invariant Qualifier":
       *
       * "Only variables output from a shader can be candidates for invariance.
       * This includes user-defined output variables and the built-in output
       * variables. As only outputs can be declared as invariant, an invariant
       * output from one shader stage will still match an input of a subsequent
       * stage without the input being declared as invariant."
       *
       * On the desktop side, this text first appears in GLSL 4.20.
       */
      if (state->is_version(420, 300) && (yyval.type_qualifier).flags.q.in)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "invariant qualifiers cannot be used with shader inputs");
   ;}
    break;

  case 175:
#line 2074 "glsl_parser.yy"
    {
      /* Section 4.3 of the GLSL 1.40 specification states:
       * "...qualified with one of these interpolation qualifiers"
       *
       * GLSL 1.30 claims to allow "one or more", but insists that:
       * "These interpolation qualifiers may only precede the qualifiers in,
       *  centroid in, out, or centroid out in a declaration."
       *
       * ...which means that e.g. smooth can't precede smooth, so there can be
       * only one after all, and the 1.40 text is a clarification, not a change.
       */
      if ((yyvsp[(2) - (2)].type_qualifier).has_interpolation())
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "duplicate interpolation qualifier");

      if (!state->has_420pack_or_es31() &&
          ((yyvsp[(2) - (2)].type_qualifier).flags.q.precise || (yyvsp[(2) - (2)].type_qualifier).flags.q.invariant)) {
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "interpolation qualifiers must come "
                          "after \"precise\" or \"invariant\"");
      }

      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false);
   ;}
    break;

  case 176:
#line 2098 "glsl_parser.yy"
    {
      /* In the absence of ARB_shading_language_420pack, layout qualifiers may
       * appear no later than auxiliary storage qualifiers. There is no
       * particularly clear spec language mandating this, but in all examples
       * the layout qualifier precedes the storage qualifier.
       *
       * We allow combinations of layout with interpolation, invariant or
       * precise qualifiers since these are useful in ARB_separate_shader_objects.
       * There is no clear spec guidance on this either.
       */
      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(& (yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false, (yyvsp[(2) - (2)].type_qualifier).has_layout());
   ;}
    break;

  case 177:
#line 2112 "glsl_parser.yy"
    {
      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false);
   ;}
    break;

  case 178:
#line 2117 "glsl_parser.yy"
    {
      if ((yyvsp[(2) - (2)].type_qualifier).has_auxiliary_storage()) {
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state,
                          "duplicate auxiliary storage qualifier (centroid or sample)");
      }

      if ((!state->has_420pack_or_es31() && !state->EXT_gpu_shader4_enable) &&
          ((yyvsp[(2) - (2)].type_qualifier).flags.q.precise || (yyvsp[(2) - (2)].type_qualifier).flags.q.invariant ||
           (yyvsp[(2) - (2)].type_qualifier).has_interpolation() || (yyvsp[(2) - (2)].type_qualifier).has_layout())) {
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "auxiliary storage qualifiers must come "
                          "just before storage qualifiers");
      }
      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false);
   ;}
    break;

  case 179:
#line 2133 "glsl_parser.yy"
    {
      /* Section 4.3 of the GLSL 1.20 specification states:
       * "Variable declarations may have a storage qualifier specified..."
       *  1.30 clarifies this to "may have one storage qualifier".
       *
       * GL_EXT_gpu_shader4 allows "varying out" in fragment shaders.
       */
      if ((yyvsp[(2) - (2)].type_qualifier).has_storage() &&
          (!state->EXT_gpu_shader4_enable ||
           state->stage != MESA_SHADER_FRAGMENT ||
           !(yyvsp[(1) - (2)].type_qualifier).flags.q.varying || !(yyvsp[(2) - (2)].type_qualifier).flags.q.out))
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "duplicate storage qualifier");

      if (!state->has_420pack_or_es31() &&
          ((yyvsp[(2) - (2)].type_qualifier).flags.q.precise || (yyvsp[(2) - (2)].type_qualifier).flags.q.invariant || (yyvsp[(2) - (2)].type_qualifier).has_interpolation() ||
           (yyvsp[(2) - (2)].type_qualifier).has_layout() || (yyvsp[(2) - (2)].type_qualifier).has_auxiliary_storage())) {
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "storage qualifiers must come after "
                          "precise, invariant, interpolation, layout and auxiliary "
                          "storage qualifiers");
      }

      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false);
   ;}
    break;

  case 180:
#line 2158 "glsl_parser.yy"
    {
      if ((yyvsp[(2) - (2)].type_qualifier).precision != ast_precision_none)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "duplicate precision qualifier");

      if (!(state->has_420pack_or_es31()) &&
          (yyvsp[(2) - (2)].type_qualifier).flags.i != 0)
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "precision qualifiers must come last");

      (yyval.type_qualifier) = (yyvsp[(2) - (2)].type_qualifier);
      (yyval.type_qualifier).precision = (yyvsp[(1) - (2)].n);
   ;}
    break;

  case 181:
#line 2170 "glsl_parser.yy"
    {
      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      (yyval.type_qualifier).merge_qualifier(&(yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false);
   ;}
    break;

  case 182:
#line 2178 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.centroid = 1;
   ;}
    break;

  case 183:
#line 2183 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.sample = 1;
   ;}
    break;

  case 184:
#line 2188 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.patch = 1;
   ;}
    break;

  case 185:
#line 2195 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.constant = 1;
   ;}
    break;

  case 186:
#line 2200 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.attribute = 1;
   ;}
    break;

  case 187:
#line 2205 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.varying = 1;
   ;}
    break;

  case 188:
#line 2210 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.in = 1;
   ;}
    break;

  case 189:
#line 2215 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.out = 1;

      if (state->stage == MESA_SHADER_GEOMETRY &&
          state->has_explicit_attrib_stream()) {
         /* Section 4.3.8.2 (Output Layout Qualifiers) of the GLSL 4.00
          * spec says:
          *
          *     "If the block or variable is declared with the stream
          *     identifier, it is associated with the specified stream;
          *     otherwise, it is associated with the current default stream."
          */
          (yyval.type_qualifier).flags.q.stream = 1;
          (yyval.type_qualifier).flags.q.explicit_stream = 0;
          (yyval.type_qualifier).stream = state->out_qualifier->stream;
      }

      if (state->has_enhanced_layouts() && state->exts->ARB_transform_feedback3) {
          (yyval.type_qualifier).flags.q.xfb_buffer = 1;
          (yyval.type_qualifier).flags.q.explicit_xfb_buffer = 0;
          (yyval.type_qualifier).xfb_buffer = state->out_qualifier->xfb_buffer;
      }
   ;}
    break;

  case 190:
#line 2240 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.in = 1;
      (yyval.type_qualifier).flags.q.out = 1;

      if (!state->has_framebuffer_fetch() ||
          !state->is_version(130, 300) ||
          state->stage != MESA_SHADER_FRAGMENT)
         _mesa_glsl_error(&(yylsp[(1) - (1)]), state, "A single interface variable cannot be "
                          "declared as both input and output");
   ;}
    break;

  case 191:
#line 2252 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.uniform = 1;
   ;}
    break;

  case 192:
#line 2257 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.buffer = 1;
   ;}
    break;

  case 193:
#line 2262 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.shared_storage = 1;
   ;}
    break;

  case 194:
#line 2270 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.coherent = 1;
   ;}
    break;

  case 195:
#line 2275 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q._volatile = 1;
   ;}
    break;

  case 196:
#line 2280 "glsl_parser.yy"
    {
      STATIC_ASSERT(sizeof((yyval.type_qualifier).flags.q) <= sizeof((yyval.type_qualifier).flags.i));
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.restrict_flag = 1;
   ;}
    break;

  case 197:
#line 2286 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.read_only = 1;
   ;}
    break;

  case 198:
#line 2291 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.write_only = 1;
   ;}
    break;

  case 199:
#line 2299 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.array_specifier) = new(ctx) ast_array_specifier((yylsp[(1) - (2)]), new(ctx) ast_expression(
                                                  ast_unsized_array_dim, NULL,
                                                  NULL, NULL));
      (yyval.array_specifier)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
   ;}
    break;

  case 200:
#line 2307 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.array_specifier) = new(ctx) ast_array_specifier((yylsp[(1) - (3)]), (yyvsp[(2) - (3)].expression));
      (yyval.array_specifier)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 201:
#line 2313 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.array_specifier) = (yyvsp[(1) - (3)].array_specifier);

      if (state->check_arrays_of_arrays_allowed(& (yylsp[(1) - (3)]))) {
         (yyval.array_specifier)->add_dimension(new(ctx) ast_expression(ast_unsized_array_dim, NULL,
                                                   NULL, NULL));
      }
   ;}
    break;

  case 202:
#line 2323 "glsl_parser.yy"
    {
      (yyval.array_specifier) = (yyvsp[(1) - (4)].array_specifier);

      if (state->check_arrays_of_arrays_allowed(& (yylsp[(1) - (4)]))) {
         (yyval.array_specifier)->add_dimension((yyvsp[(3) - (4)].expression));
      }
   ;}
    break;

  case 204:
#line 2335 "glsl_parser.yy"
    {
      (yyval.type_specifier) = (yyvsp[(1) - (2)].type_specifier);
      (yyval.type_specifier)->array_specifier = (yyvsp[(2) - (2)].array_specifier);
   ;}
    break;

  case 205:
#line 2343 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.type_specifier) = new(ctx) ast_type_specifier((yyvsp[(1) - (1)].type));
      (yyval.type_specifier)->set_location((yylsp[(1) - (1)]));
   ;}
    break;

  case 206:
#line 2349 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.type_specifier) = new(ctx) ast_type_specifier((yyvsp[(1) - (1)].struct_specifier));
      (yyval.type_specifier)->set_location((yylsp[(1) - (1)]));
   ;}
    break;

  case 207:
#line 2355 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.type_specifier) = new(ctx) ast_type_specifier((yyvsp[(1) - (1)].identifier));
      (yyval.type_specifier)->set_location((yylsp[(1) - (1)]));
   ;}
    break;

  case 208:
#line 2363 "glsl_parser.yy"
    { (yyval.type) = &glsl_type_builtin_void; ;}
    break;

  case 209:
#line 2364 "glsl_parser.yy"
    { (yyval.type) = (yyvsp[(1) - (1)].type); ;}
    break;

  case 210:
#line 2366 "glsl_parser.yy"
    {
      if ((yyvsp[(2) - (2)].type) == &glsl_type_builtin_int) {
         (yyval.type) = &glsl_type_builtin_uint;
      } else {
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state,
                          "\"unsigned\" is only allowed before \"int\"");
      }
   ;}
    break;

  case 211:
#line 2378 "glsl_parser.yy"
    {
      state->check_precision_qualifiers_allowed(&(yylsp[(1) - (1)]));
      (yyval.n) = ast_precision_high;
   ;}
    break;

  case 212:
#line 2383 "glsl_parser.yy"
    {
      state->check_precision_qualifiers_allowed(&(yylsp[(1) - (1)]));
      (yyval.n) = ast_precision_medium;
   ;}
    break;

  case 213:
#line 2388 "glsl_parser.yy"
    {
      state->check_precision_qualifiers_allowed(&(yylsp[(1) - (1)]));
      (yyval.n) = ast_precision_low;
   ;}
    break;

  case 214:
#line 2396 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.struct_specifier) = new(ctx) ast_struct_specifier((yyvsp[(2) - (5)].identifier), (yyvsp[(4) - (5)].declarator_list));
      (yyval.struct_specifier)->set_location_range((yylsp[(2) - (5)]), (yylsp[(5) - (5)]));
      state->symbols->add_type((yyvsp[(2) - (5)].identifier), &glsl_type_builtin_void);
   ;}
    break;

  case 215:
#line 2403 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;

      /* All anonymous structs have the same name. This simplifies matching of
       * globals whose type is an unnamed struct.
       *
       * It also avoids a memory leak when the same shader is compiled over and
       * over again.
       */
      (yyval.struct_specifier) = new(ctx) ast_struct_specifier("#anon_struct", (yyvsp[(3) - (4)].declarator_list));

      (yyval.struct_specifier)->set_location_range((yylsp[(2) - (4)]), (yylsp[(4) - (4)]));
   ;}
    break;

  case 216:
#line 2420 "glsl_parser.yy"
    {
      (yyval.declarator_list) = (yyvsp[(1) - (1)].declarator_list);
      (yyvsp[(1) - (1)].declarator_list)->link.self_link();
   ;}
    break;

  case 217:
#line 2425 "glsl_parser.yy"
    {
      (yyval.declarator_list) = (yyvsp[(1) - (2)].declarator_list);
      (yyval.declarator_list)->link.insert_before(& (yyvsp[(2) - (2)].declarator_list)->link);
   ;}
    break;

  case 218:
#line 2433 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_fully_specified_type *const type = (yyvsp[(1) - (3)].fully_specified_type);
      type->set_location((yylsp[(1) - (3)]));

      if (state->has_bindless()) {
         ast_type_qualifier input_layout_mask;

         /* Allow to declare qualifiers for images. */
         input_layout_mask.flags.i = 0;
         input_layout_mask.flags.q.coherent = 1;
         input_layout_mask.flags.q._volatile = 1;
         input_layout_mask.flags.q.restrict_flag = 1;
         input_layout_mask.flags.q.read_only = 1;
         input_layout_mask.flags.q.write_only = 1;
         input_layout_mask.flags.q.explicit_image_format = 1;

         if ((type->qualifier.flags.i & ~input_layout_mask.flags.i) != 0) {
            _mesa_glsl_error(&(yylsp[(1) - (3)]), state,
                             "only precision and image qualifiers may be "
                             "applied to structure members");
         }
      } else {
         if (type->qualifier.flags.i != 0)
            _mesa_glsl_error(&(yylsp[(1) - (3)]), state,
                             "only precision qualifiers may be applied to "
                             "structure members");
      }

      (yyval.declarator_list) = new(ctx) ast_declarator_list(type);
      (yyval.declarator_list)->set_location((yylsp[(2) - (3)]));

      (yyval.declarator_list)->declarations.push_degenerate_list_at_head(& (yyvsp[(2) - (3)].declaration)->link);
   ;}
    break;

  case 219:
#line 2471 "glsl_parser.yy"
    {
      (yyval.declaration) = (yyvsp[(1) - (1)].declaration);
      (yyvsp[(1) - (1)].declaration)->link.self_link();
   ;}
    break;

  case 220:
#line 2476 "glsl_parser.yy"
    {
      (yyval.declaration) = (yyvsp[(1) - (3)].declaration);
      (yyval.declaration)->link.insert_before(& (yyvsp[(3) - (3)].declaration)->link);
   ;}
    break;

  case 221:
#line 2484 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.declaration) = new(ctx) ast_declaration((yyvsp[(1) - (1)].identifier), NULL, NULL);
      (yyval.declaration)->set_location((yylsp[(1) - (1)]));
   ;}
    break;

  case 222:
#line 2490 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.declaration) = new(ctx) ast_declaration((yyvsp[(1) - (2)].identifier), (yyvsp[(2) - (2)].array_specifier), NULL);
      (yyval.declaration)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
   ;}
    break;

  case 224:
#line 2500 "glsl_parser.yy"
    {
      (yyval.expression) = (yyvsp[(2) - (3)].expression);
   ;}
    break;

  case 225:
#line 2504 "glsl_parser.yy"
    {
      (yyval.expression) = (yyvsp[(2) - (4)].expression);
   ;}
    break;

  case 226:
#line 2511 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.expression) = new(ctx) ast_aggregate_initializer();
      (yyval.expression)->set_location((yylsp[(1) - (1)]));
      (yyval.expression)->expressions.push_tail(& (yyvsp[(1) - (1)].expression)->link);
   ;}
    break;

  case 227:
#line 2518 "glsl_parser.yy"
    {
      (yyvsp[(1) - (3)].expression)->expressions.push_tail(& (yyvsp[(3) - (3)].expression)->link);
   ;}
    break;

  case 229:
#line 2530 "glsl_parser.yy"
    { (yyval.node) = (ast_node *) (yyvsp[(1) - (1)].compound_statement); ;}
    break;

  case 238:
#line 2546 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.compound_statement) = new(ctx) ast_compound_statement(true, NULL);
      (yyval.compound_statement)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
   ;}
    break;

  case 239:
#line 2552 "glsl_parser.yy"
    {
      state->symbols->push_scope();
   ;}
    break;

  case 240:
#line 2556 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.compound_statement) = new(ctx) ast_compound_statement(true, (yyvsp[(3) - (4)].node));
      (yyval.compound_statement)->set_location_range((yylsp[(1) - (4)]), (yylsp[(4) - (4)]));
      state->symbols->pop_scope();
   ;}
    break;

  case 241:
#line 2565 "glsl_parser.yy"
    { (yyval.node) = (ast_node *) (yyvsp[(1) - (1)].compound_statement); ;}
    break;

  case 243:
#line 2571 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.compound_statement) = new(ctx) ast_compound_statement(false, NULL);
      (yyval.compound_statement)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
   ;}
    break;

  case 244:
#line 2577 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.compound_statement) = new(ctx) ast_compound_statement(false, (yyvsp[(2) - (3)].node));
      (yyval.compound_statement)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 245:
#line 2586 "glsl_parser.yy"
    {
      if ((yyvsp[(1) - (1)].node) == NULL) {
         _mesa_glsl_error(& (yylsp[(1) - (1)]), state, "<nil> statement");
         assert((yyvsp[(1) - (1)].node) != NULL);
      }

      (yyval.node) = (yyvsp[(1) - (1)].node);
      (yyval.node)->link.self_link();
   ;}
    break;

  case 246:
#line 2596 "glsl_parser.yy"
    {
      if ((yyvsp[(2) - (2)].node) == NULL) {
         _mesa_glsl_error(& (yylsp[(2) - (2)]), state, "<nil> statement");
         assert((yyvsp[(2) - (2)].node) != NULL);
      }
      (yyval.node) = (yyvsp[(1) - (2)].node);
      (yyval.node)->link.insert_before(& (yyvsp[(2) - (2)].node)->link);
   ;}
    break;

  case 247:
#line 2605 "glsl_parser.yy"
    {
      if (!state->allow_extension_directive_midshader) {
         _mesa_glsl_error(& (yylsp[(1) - (2)]), state,
                          "#extension directive is not allowed "
                          "in the middle of a shader");
         YYERROR;
      }
   ;}
    break;

  case 248:
#line 2617 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_expression_statement(NULL);
      (yyval.node)->set_location((yylsp[(1) - (1)]));
   ;}
    break;

  case 249:
#line 2623 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_expression_statement((yyvsp[(1) - (2)].expression));
      (yyval.node)->set_location((yylsp[(1) - (2)]));
   ;}
    break;

  case 250:
#line 2632 "glsl_parser.yy"
    {
      (yyval.node) = new(state->linalloc) ast_selection_statement((yyvsp[(3) - (5)].expression), (yyvsp[(5) - (5)].selection_rest_statement).then_statement,
                                                        (yyvsp[(5) - (5)].selection_rest_statement).else_statement);
      (yyval.node)->set_location_range((yylsp[(1) - (5)]), (yylsp[(5) - (5)]));
   ;}
    break;

  case 251:
#line 2641 "glsl_parser.yy"
    {
      (yyval.selection_rest_statement).then_statement = (yyvsp[(1) - (3)].node);
      (yyval.selection_rest_statement).else_statement = (yyvsp[(3) - (3)].node);
   ;}
    break;

  case 252:
#line 2646 "glsl_parser.yy"
    {
      (yyval.selection_rest_statement).then_statement = (yyvsp[(1) - (1)].node);
      (yyval.selection_rest_statement).else_statement = NULL;
   ;}
    break;

  case 253:
#line 2654 "glsl_parser.yy"
    {
      (yyval.node) = (ast_node *) (yyvsp[(1) - (1)].expression);
   ;}
    break;

  case 254:
#line 2658 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_declaration *decl = new(ctx) ast_declaration((yyvsp[(2) - (4)].identifier), NULL, (yyvsp[(4) - (4)].expression));
      ast_declarator_list *declarator = new(ctx) ast_declarator_list((yyvsp[(1) - (4)].fully_specified_type));
      decl->set_location_range((yylsp[(2) - (4)]), (yylsp[(4) - (4)]));
      declarator->set_location((yylsp[(1) - (4)]));

      declarator->declarations.push_tail(&decl->link);
      (yyval.node) = declarator;
   ;}
    break;

  case 255:
#line 2676 "glsl_parser.yy"
    {
      (yyval.node) = new(state->linalloc) ast_switch_statement((yyvsp[(3) - (5)].expression), (yyvsp[(5) - (5)].switch_body));
      (yyval.node)->set_location_range((yylsp[(1) - (5)]), (yylsp[(5) - (5)]));
   ;}
    break;

  case 256:
#line 2684 "glsl_parser.yy"
    {
      (yyval.switch_body) = new(state->linalloc) ast_switch_body(NULL);
      (yyval.switch_body)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
   ;}
    break;

  case 257:
#line 2689 "glsl_parser.yy"
    {
      (yyval.switch_body) = new(state->linalloc) ast_switch_body((yyvsp[(2) - (3)].case_statement_list));
      (yyval.switch_body)->set_location_range((yylsp[(1) - (3)]), (yylsp[(3) - (3)]));
   ;}
    break;

  case 258:
#line 2697 "glsl_parser.yy"
    {
      (yyval.case_label) = new(state->linalloc) ast_case_label((yyvsp[(2) - (3)].expression));
      (yyval.case_label)->set_location((yylsp[(2) - (3)]));
   ;}
    break;

  case 259:
#line 2702 "glsl_parser.yy"
    {
      (yyval.case_label) = new(state->linalloc) ast_case_label(NULL);
      (yyval.case_label)->set_location((yylsp[(2) - (2)]));
   ;}
    break;

  case 260:
#line 2710 "glsl_parser.yy"
    {
      ast_case_label_list *labels = new(state->linalloc) ast_case_label_list();

      labels->labels.push_tail(& (yyvsp[(1) - (1)].case_label)->link);
      (yyval.case_label_list) = labels;
      (yyval.case_label_list)->set_location((yylsp[(1) - (1)]));
   ;}
    break;

  case 261:
#line 2718 "glsl_parser.yy"
    {
      (yyval.case_label_list) = (yyvsp[(1) - (2)].case_label_list);
      (yyval.case_label_list)->labels.push_tail(& (yyvsp[(2) - (2)].case_label)->link);
   ;}
    break;

  case 262:
#line 2726 "glsl_parser.yy"
    {
      ast_case_statement *stmts = new(state->linalloc) ast_case_statement((yyvsp[(1) - (2)].case_label_list));
      stmts->set_location((yylsp[(2) - (2)]));

      stmts->stmts.push_tail(& (yyvsp[(2) - (2)].node)->link);
      (yyval.case_statement) = stmts;
   ;}
    break;

  case 263:
#line 2734 "glsl_parser.yy"
    {
      (yyval.case_statement) = (yyvsp[(1) - (2)].case_statement);
      (yyval.case_statement)->stmts.push_tail(& (yyvsp[(2) - (2)].node)->link);
   ;}
    break;

  case 264:
#line 2742 "glsl_parser.yy"
    {
      ast_case_statement_list *cases= new(state->linalloc) ast_case_statement_list();
      cases->set_location((yylsp[(1) - (1)]));

      cases->cases.push_tail(& (yyvsp[(1) - (1)].case_statement)->link);
      (yyval.case_statement_list) = cases;
   ;}
    break;

  case 265:
#line 2750 "glsl_parser.yy"
    {
      (yyval.case_statement_list) = (yyvsp[(1) - (2)].case_statement_list);
      (yyval.case_statement_list)->cases.push_tail(& (yyvsp[(2) - (2)].case_statement)->link);
   ;}
    break;

  case 266:
#line 2758 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_iteration_statement(ast_iteration_statement::ast_while,
                                            NULL, (yyvsp[(3) - (5)].node), NULL, (yyvsp[(5) - (5)].node));
      (yyval.node)->set_location_range((yylsp[(1) - (5)]), (yylsp[(4) - (5)]));
   ;}
    break;

  case 267:
#line 2765 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_iteration_statement(ast_iteration_statement::ast_do_while,
                                            NULL, (yyvsp[(5) - (7)].expression), NULL, (yyvsp[(2) - (7)].node));
      (yyval.node)->set_location_range((yylsp[(1) - (7)]), (yylsp[(6) - (7)]));
   ;}
    break;

  case 268:
#line 2772 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_iteration_statement(ast_iteration_statement::ast_for,
                                            (yyvsp[(3) - (6)].node), (yyvsp[(4) - (6)].for_rest_statement).cond, (yyvsp[(4) - (6)].for_rest_statement).rest, (yyvsp[(6) - (6)].node));
      (yyval.node)->set_location_range((yylsp[(1) - (6)]), (yylsp[(6) - (6)]));
   ;}
    break;

  case 272:
#line 2788 "glsl_parser.yy"
    {
      (yyval.node) = NULL;
   ;}
    break;

  case 273:
#line 2795 "glsl_parser.yy"
    {
      (yyval.for_rest_statement).cond = (yyvsp[(1) - (2)].node);
      (yyval.for_rest_statement).rest = NULL;
   ;}
    break;

  case 274:
#line 2800 "glsl_parser.yy"
    {
      (yyval.for_rest_statement).cond = (yyvsp[(1) - (3)].node);
      (yyval.for_rest_statement).rest = (yyvsp[(3) - (3)].expression);
   ;}
    break;

  case 275:
#line 2809 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_jump_statement(ast_jump_statement::ast_continue, NULL);
      (yyval.node)->set_location((yylsp[(1) - (2)]));
   ;}
    break;

  case 276:
#line 2815 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_jump_statement(ast_jump_statement::ast_break, NULL);
      (yyval.node)->set_location((yylsp[(1) - (2)]));
   ;}
    break;

  case 277:
#line 2821 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_jump_statement(ast_jump_statement::ast_return, NULL);
      (yyval.node)->set_location((yylsp[(1) - (2)]));
   ;}
    break;

  case 278:
#line 2827 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_jump_statement(ast_jump_statement::ast_return, (yyvsp[(2) - (3)].expression));
      (yyval.node)->set_location_range((yylsp[(1) - (3)]), (yylsp[(2) - (3)]));
   ;}
    break;

  case 279:
#line 2833 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_jump_statement(ast_jump_statement::ast_discard, NULL);
      (yyval.node)->set_location((yylsp[(1) - (2)]));
   ;}
    break;

  case 280:
#line 2842 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.node) = new(ctx) ast_demote_statement();
      (yyval.node)->set_location((yylsp[(1) - (2)]));
   ;}
    break;

  case 281:
#line 2850 "glsl_parser.yy"
    { (yyval.node) = (yyvsp[(1) - (1)].function_definition); ;}
    break;

  case 282:
#line 2851 "glsl_parser.yy"
    { (yyval.node) = (yyvsp[(1) - (1)].node); ;}
    break;

  case 283:
#line 2852 "glsl_parser.yy"
    { (yyval.node) = (yyvsp[(1) - (1)].node); ;}
    break;

  case 284:
#line 2853 "glsl_parser.yy"
    { (yyval.node) = (yyvsp[(1) - (1)].node); ;}
    break;

  case 285:
#line 2854 "glsl_parser.yy"
    { (yyval.node) = NULL; ;}
    break;

  case 286:
#line 2859 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      (yyval.function_definition) = new(ctx) ast_function_definition();
      (yyval.function_definition)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
      (yyval.function_definition)->prototype = (yyvsp[(1) - (2)].function);
      (yyval.function_definition)->body = (yyvsp[(2) - (2)].compound_statement);

      state->symbols->pop_scope();
   ;}
    break;

  case 287:
#line 2873 "glsl_parser.yy"
    {
      (yyval.node) = (yyvsp[(1) - (1)].interface_block);
   ;}
    break;

  case 288:
#line 2877 "glsl_parser.yy"
    {
      ast_interface_block *block = (ast_interface_block *) (yyvsp[(2) - (2)].node);

      if (!(yyvsp[(1) - (2)].type_qualifier).merge_qualifier(& (yylsp[(1) - (2)]), state, block->layout, false,
                              block->layout.has_layout())) {
         YYERROR;
      }

      block->layout = (yyvsp[(1) - (2)].type_qualifier);

      (yyval.node) = block;
   ;}
    break;

  case 289:
#line 2890 "glsl_parser.yy"
    {
      ast_interface_block *block = (ast_interface_block *)(yyvsp[(2) - (2)].node);

      if (!block->default_layout.flags.q.buffer) {
            _mesa_glsl_error(& (yylsp[(1) - (2)]), state,
                             "memory qualifiers can only be used in the "
                             "declaration of shader storage blocks");
      }
      if (!(yyvsp[(1) - (2)].type_qualifier).merge_qualifier(& (yylsp[(1) - (2)]), state, block->layout, false)) {
         YYERROR;
      }
      block->layout = (yyvsp[(1) - (2)].type_qualifier);
      (yyval.node) = block;
   ;}
    break;

  case 290:
#line 2908 "glsl_parser.yy"
    {
      ast_interface_block *const block = (yyvsp[(6) - (7)].interface_block);

      if ((yyvsp[(1) - (7)].type_qualifier).flags.q.uniform) {
         block->default_layout = *state->default_uniform_qualifier;
      } else if ((yyvsp[(1) - (7)].type_qualifier).flags.q.buffer) {
         block->default_layout = *state->default_shader_storage_qualifier;
      }
      block->block_name = (yyvsp[(2) - (7)].identifier);
      block->declarations.push_degenerate_list_at_head(& (yyvsp[(4) - (7)].declarator_list)->link);

      _mesa_ast_process_interface_block(& (yylsp[(1) - (7)]), state, block, (yyvsp[(1) - (7)].type_qualifier));

      (yyval.interface_block) = block;
   ;}
    break;

  case 291:
#line 2927 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.in = 1;
   ;}
    break;

  case 292:
#line 2932 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.out = 1;
   ;}
    break;

  case 293:
#line 2937 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.uniform = 1;
   ;}
    break;

  case 294:
#line 2942 "glsl_parser.yy"
    {
      memset(& (yyval.type_qualifier), 0, sizeof((yyval.type_qualifier)));
      (yyval.type_qualifier).flags.q.buffer = 1;
   ;}
    break;

  case 295:
#line 2947 "glsl_parser.yy"
    {
      if (!(yyvsp[(1) - (2)].type_qualifier).flags.q.patch) {
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "invalid interface qualifier");
      }
      if ((yyvsp[(2) - (2)].type_qualifier).has_auxiliary_storage()) {
         _mesa_glsl_error(&(yylsp[(1) - (2)]), state, "duplicate patch qualifier");
      }
      (yyval.type_qualifier) = (yyvsp[(2) - (2)].type_qualifier);
      (yyval.type_qualifier).flags.q.patch = 1;
   ;}
    break;

  case 296:
#line 2961 "glsl_parser.yy"
    {
      (yyval.interface_block) = new(state->linalloc) ast_interface_block(NULL, NULL);
   ;}
    break;

  case 297:
#line 2965 "glsl_parser.yy"
    {
      (yyval.interface_block) = new(state->linalloc) ast_interface_block((yyvsp[(1) - (1)].identifier), NULL);
      (yyval.interface_block)->set_location((yylsp[(1) - (1)]));
   ;}
    break;

  case 298:
#line 2970 "glsl_parser.yy"
    {
      (yyval.interface_block) = new(state->linalloc) ast_interface_block((yyvsp[(1) - (2)].identifier), (yyvsp[(2) - (2)].array_specifier));
      (yyval.interface_block)->set_location_range((yylsp[(1) - (2)]), (yylsp[(2) - (2)]));
   ;}
    break;

  case 299:
#line 2978 "glsl_parser.yy"
    {
      (yyval.declarator_list) = (yyvsp[(1) - (1)].declarator_list);
      (yyvsp[(1) - (1)].declarator_list)->link.self_link();
   ;}
    break;

  case 300:
#line 2983 "glsl_parser.yy"
    {
      (yyval.declarator_list) = (yyvsp[(1) - (2)].declarator_list);
      (yyvsp[(2) - (2)].declarator_list)->link.insert_before(& (yyval.declarator_list)->link);
   ;}
    break;

  case 301:
#line 2991 "glsl_parser.yy"
    {
      linear_ctx *ctx = state->linalloc;
      ast_fully_specified_type *type = (yyvsp[(1) - (3)].fully_specified_type);
      type->set_location((yylsp[(1) - (3)]));

      if (type->qualifier.flags.q.attribute) {
         _mesa_glsl_error(& (yylsp[(1) - (3)]), state,
                          "keyword 'attribute' cannot be used with "
                          "interface block member");
      } else if (type->qualifier.flags.q.varying) {
         _mesa_glsl_error(& (yylsp[(1) - (3)]), state,
                          "keyword 'varying' cannot be used with "
                          "interface block member");
      }

      (yyval.declarator_list) = new(ctx) ast_declarator_list(type);
      (yyval.declarator_list)->set_location((yylsp[(2) - (3)]));

      (yyval.declarator_list)->declarations.push_degenerate_list_at_head(& (yyvsp[(2) - (3)].declaration)->link);
   ;}
    break;

  case 302:
#line 3015 "glsl_parser.yy"
    {
      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      if (!(yyval.type_qualifier).merge_qualifier(& (yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false, true)) {
         YYERROR;
      }
   ;}
    break;

  case 304:
#line 3026 "glsl_parser.yy"
    {
      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      if (!(yyval.type_qualifier).merge_qualifier(& (yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false, true)) {
         YYERROR;
      }
   ;}
    break;

  case 306:
#line 3037 "glsl_parser.yy"
    {
      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      if (!(yyval.type_qualifier).merge_qualifier(& (yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false, true)) {
         YYERROR;
      }
      if (!(yyval.type_qualifier).validate_in_qualifier(& (yylsp[(1) - (2)]), state)) {
         YYERROR;
      }
   ;}
    break;

  case 307:
#line 3047 "glsl_parser.yy"
    {
      if (!(yyvsp[(1) - (3)].type_qualifier).validate_in_qualifier(& (yylsp[(1) - (3)]), state)) {
         YYERROR;
      }
   ;}
    break;

  case 308:
#line 3056 "glsl_parser.yy"
    {
      (yyval.type_qualifier) = (yyvsp[(1) - (2)].type_qualifier);
      if (!(yyval.type_qualifier).merge_qualifier(& (yylsp[(1) - (2)]), state, (yyvsp[(2) - (2)].type_qualifier), false, true)) {
         YYERROR;
      }
      if (!(yyval.type_qualifier).validate_out_qualifier(& (yylsp[(1) - (2)]), state)) {
         YYERROR;
      }
   ;}
    break;

  case 309:
#line 3066 "glsl_parser.yy"
    {
      if (!(yyvsp[(1) - (3)].type_qualifier).validate_out_qualifier(& (yylsp[(1) - (3)]), state)) {
         YYERROR;
      }
   ;}
    break;

  case 310:
#line 3075 "glsl_parser.yy"
    {
      (yyval.node) = NULL;
      if (!state->default_uniform_qualifier->
             merge_qualifier(& (yylsp[(1) - (1)]), state, (yyvsp[(1) - (1)].type_qualifier), false)) {
         YYERROR;
      }
      if (!state->default_uniform_qualifier->
             push_to_global(& (yylsp[(1) - (1)]), state)) {
         YYERROR;
      }
   ;}
    break;

  case 311:
#line 3087 "glsl_parser.yy"
    {
      (yyval.node) = NULL;
      if (!state->default_shader_storage_qualifier->
             merge_qualifier(& (yylsp[(1) - (1)]), state, (yyvsp[(1) - (1)].type_qualifier), false)) {
         YYERROR;
      }
      if (!state->default_shader_storage_qualifier->
             push_to_global(& (yylsp[(1) - (1)]), state)) {
         YYERROR;
      }

      /* From the GLSL 4.50 spec, section 4.4.5:
       *
       *     "It is a compile-time error to specify the binding identifier for
       *     the global scope or for block member declarations."
       */
      if (state->default_shader_storage_qualifier->flags.q.explicit_binding) {
         _mesa_glsl_error(& (yylsp[(1) - (1)]), state,
                          "binding qualifier cannot be set for default layout");
      }
   ;}
    break;

  case 312:
#line 3109 "glsl_parser.yy"
    {
      (yyval.node) = NULL;
      if (!(yyvsp[(1) - (1)].type_qualifier).merge_into_in_qualifier(& (yylsp[(1) - (1)]), state, (yyval.node))) {
         YYERROR;
      }
      if (!state->in_qualifier->push_to_global(& (yylsp[(1) - (1)]), state)) {
         YYERROR;
      }
   ;}
    break;

  case 313:
#line 3119 "glsl_parser.yy"
    {
      (yyval.node) = NULL;
      if (!(yyvsp[(1) - (1)].type_qualifier).merge_into_out_qualifier(& (yylsp[(1) - (1)]), state, (yyval.node))) {
         YYERROR;
      }
      if (!state->out_qualifier->push_to_global(& (yylsp[(1) - (1)]), state)) {
         YYERROR;
      }

      (void)yynerrs;
   ;}
    break;


/* Line 1267 of yacc.c.  */
#line 5947 "/tmp/glsl_parser.cpp"
      default: break;
    }
  YY_SYMBOL_PRINT ("-> $$ =", yyr1[yyn], &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);

  *++yyvsp = yyval;
  *++yylsp = yyloc;

  /* Now `shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */

  yyn = yyr1[yyn];

  yystate = yypgoto[yyn - YYNTOKENS] + *yyssp;
  if (0 <= yystate && yystate <= YYLAST && yycheck[yystate] == *yyssp)
    yystate = yytable[yystate];
  else
    yystate = yydefgoto[yyn - YYNTOKENS];

  goto yynewstate;


/*------------------------------------.
| yyerrlab -- here on detecting error |
`------------------------------------*/
yyerrlab:
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
#if ! YYERROR_VERBOSE
      yyerror (&yylloc, state, YY_("syntax error"));
#else
      {
	YYSIZE_T yysize = yysyntax_error (0, yystate, yychar);
	if (yymsg_alloc < yysize && yymsg_alloc < YYSTACK_ALLOC_MAXIMUM)
	  {
	    YYSIZE_T yyalloc = 2 * yysize;
	    if (! (yysize <= yyalloc && yyalloc <= YYSTACK_ALLOC_MAXIMUM))
	      yyalloc = YYSTACK_ALLOC_MAXIMUM;
	    if (yymsg != yymsgbuf)
	      YYSTACK_FREE (yymsg);
	    yymsg = (char *) YYSTACK_ALLOC (yyalloc);
	    if (yymsg)
	      yymsg_alloc = yyalloc;
	    else
	      {
		yymsg = yymsgbuf;
		yymsg_alloc = sizeof yymsgbuf;
	      }
	  }

	if (0 < yysize && yysize <= yymsg_alloc)
	  {
	    (void) yysyntax_error (yymsg, yystate, yychar);
	    yyerror (&yylloc, state, yymsg);
	  }
	else
	  {
	    yyerror (&yylloc, state, YY_("syntax error"));
	    if (yysize != 0)
	      goto yyexhaustedlab;
	  }
      }
#endif
    }

  yyerror_range[0] = yylloc;

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse look-ahead token after an
	 error, discard it.  */

      if (yychar <= YYEOF)
	{
	  /* Return failure if at end of input.  */
	  if (yychar == YYEOF)
	    YYABORT;
	}
      else
	{
	  yydestruct ("Error: discarding",
		      yytoken, &yylval, &yylloc, state);
	  yychar = YYEMPTY;
	}
    }

  /* Else will try to reuse look-ahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:

  /* Pacify compilers like GCC when the user code never invokes
     YYERROR and the label yyerrorlab therefore never appears in user
     code.  */
  if (/*CONSTCOND*/ 0)
     goto yyerrorlab;

  yyerror_range[0] = yylsp[1-yylen];
  /* Do not reclaim the symbols of the rule which action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;	/* Each real token shifted decrements this.  */

  for (;;)
    {
      yyn = yypact[yystate];
      if (yyn != YYPACT_NINF)
	{
	  yyn += YYTERROR;
	  if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYTERROR)
	    {
	      yyn = yytable[yyn];
	      if (0 < yyn)
		break;
	    }
	}

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
	YYABORT;

      yyerror_range[0] = *yylsp;
      yydestruct ("Error: popping",
		  yystos[yystate], yyvsp, yylsp, state);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  if (yyn == YYFINAL)
    YYACCEPT;

  *++yyvsp = yylval;

  yyerror_range[1] = yylloc;
  /* Using YYLLOC is tempting, but would change the location of
     the look-ahead.  YYLOC is available though.  */
  YYLLOC_DEFAULT (yyloc, (yyerror_range - 1), 2);
  *++yylsp = yyloc;

  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", yystos[yyn], yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturn;

/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturn;

#ifndef yyoverflow
/*-------------------------------------------------.
| yyexhaustedlab -- memory exhaustion comes here.  |
`-------------------------------------------------*/
yyexhaustedlab:
  yyerror (&yylloc, state, YY_("memory exhausted"));
  yyresult = 2;
  /* Fall through.  */
#endif

yyreturn:
  if (yychar != YYEOF && yychar != YYEMPTY)
     yydestruct ("Cleanup: discarding lookahead",
		 yytoken, &yylval, &yylloc, state);
  /* Do not reclaim the symbols of the rule which action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
		  yystos[*yyssp], yyvsp, yylsp, state);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
#if YYERROR_VERBOSE
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
#endif
  /* Make sure YYID is used.  */
  return YYID (yyresult);
}



