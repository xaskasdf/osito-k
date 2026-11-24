/* A Bison parser, made by GNU Bison 2.3.  */

/* Skeleton interface for Bison's Yacc-like parsers in C

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
/* Line 1529 of yacc.c.  */
#line 368 "/tmp/glsl_parser.h"
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


