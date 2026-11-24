/*
 * zForth — minimal Forth for embedded systems
 * Adapted for OsitoK: no libc (ctype.h, string.h) — inline replacements.
 * Original: https://github.com/zevv/zForth (MIT license)
 */

#include "setjmp.h"
#include "zforth.h"

/* Inline replacements for libc functions (no ctype.h, no string.h) */

static inline int zf_isspace(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static inline size_t zf_strlen(const char *s)
{
	size_t n = 0;
	while (*s++) n++;
	return n;
}

static inline int zf_memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *pa = (const unsigned char *)a;
	const unsigned char *pb = (const unsigned char *)b;
	while (n--) {
		if (*pa != *pb) return *pa - *pb;
		pa++; pb++;
	}
	return 0;
}

/* Redefine so the rest of the code uses our versions */
#define strlen  zf_strlen
#define memcmp  zf_memcmp
#define isspace zf_isspace


/* Flags and length encoded in words */

#define ZF_FLAG_IMMEDIATE (1<<6)
#define ZF_FLAG_PRIM      (1<<5)
#define ZF_FLAG_LEN(v)    (v & 0x1f)


/* This macro is used to perform boundary checks. If ZF_ENABLE_BOUNDARY_CHECKS
 * is set to 0, the boundary check code will not be compiled in to reduce size */

#if ZF_ENABLE_BOUNDARY_CHECKS
#define CHECK(ctx, exp, abort) if(!(exp)) zf_abort(ctx, abort);
#else
#define CHECK(ctx, exp, abort)
#endif

typedef enum {
	ZF_MEM_SIZE_VAR = 0,      /* Variable size encoding, 1, 2 or 1+sizeof(zf_cell) bytes */
	ZF_MEM_SIZE_CELL = 1,     /* sizeof(zf_cell) bytes */
	ZF_MEM_SIZE_U8 = 2,
	ZF_MEM_SIZE_U16 = 3,
	ZF_MEM_SIZE_U32 = 4,
	ZF_MEM_SIZE_S8 = 5,
	ZF_MEM_SIZE_S16 = 6,
	ZF_MEM_SIZE_S32 = 7,
	ZF_MEM_SIZE_VAR_MAX = 64, /* Variable size encoding, 1+sizeof(zf_cell) bytes */
} zf_mem_size;

#define _(s) s "\0"

typedef enum {
	PRIM_EXIT,    PRIM_LIT,       PRIM_LTZ,  PRIM_COL,     PRIM_SEMICOL,  PRIM_ADD,
	PRIM_SUB,     PRIM_MUL,       PRIM_DIV,  PRIM_MOD,     PRIM_DROP,     PRIM_DUP,
	PRIM_PICKR,   PRIM_IMMEDIATE, PRIM_PEEK, PRIM_POKE,    PRIM_SWAP,     PRIM_ROT,
	PRIM_JMP,     PRIM_JMP0,      PRIM_TICK, PRIM_COMMENT, PRIM_PUSHR,    PRIM_POPR,
	PRIM_EQUAL,   PRIM_SYS,       PRIM_PICK, PRIM_COMMA,   PRIM_KEY,      PRIM_LITS,
	PRIM_LEN,     PRIM_AND,       PRIM_OR,   PRIM_XOR,     PRIM_SHL,      PRIM_SHR,
	PRIM_LITERAL,
	PRIM_COUNT
} zf_prim;

static const char prim_names[] =
	_("exit")    _("lit")        _("<0")    _(":")     _("_;")        _("+")
	_("-")       _("*")          _("/")     _("%")     _("drop")      _("dup")
	_("pickr")   _("_immediate") _("@@")    _("!!")    _("swap")      _("rot")
	_("jmp")     _("jmp0")       _("'")     _("_(")    _(">r")        _("r>")
	_("=")       _("sys")        _("pick")  _(",,")    _("key")       _("lits")
	_("##")      _("&")          _("|")     _("^")     _("<<")        _(">>")
	_("_literal");


#define HERE(ctx)      ctx->uservar[ZF_USERVAR_HERE]
#define LATEST(ctx)    ctx->uservar[ZF_USERVAR_LATEST]
#define TRACE(ctx)     ctx->uservar[ZF_USERVAR_TRACE]
#define COMPILING(ctx) ctx->uservar[ZF_USERVAR_COMPILING]
#define POSTPONE(ctx)  ctx->uservar[ZF_USERVAR_POSTPONE]
#define DSP(ctx)       ctx->uservar[ZF_USERVAR_DSP]
#define RSP(ctx)       ctx->uservar[ZF_USERVAR_RSP]

static const char uservar_names[] =
	_("h")   _("latest") _("trace")  _("compiling")  _("_postpone")  _("dsp")
	_("rsp");



/* Prototypes */

static void do_prim(zf_ctx *ctx, zf_prim prim, const char *input);
static zf_addr dict_get_cell(zf_ctx *ctx, zf_addr addr, zf_cell *v);
static void dict_get_bytes(zf_ctx *ctx, zf_addr addr, void *buf, size_t len);


#if ZF_ENABLE_TRACE

static void do_trace(zf_ctx *ctx, const char *fmt, ...)
{
	if(TRACE(ctx)) {
		va_list va;
		va_start(va, fmt);
		zf_host_trace(ctx, fmt, va);
		va_end(va);
	}
}

#define trace(ctx, ...) if(TRACE(ctx)) do_trace(ctx, __VA_ARGS__)

static const char *op_name(zf_ctx *ctx, zf_addr addr)
{
	zf_addr w = LATEST(ctx);
	char *name = ctx->name_buf;

	while(TRACE(ctx) && w) {
		zf_addr xt, p = w;
		zf_cell d, link, op2;
		int lenflags;

		p += dict_get_cell(ctx, p, &d);
		lenflags = d;
		p += dict_get_cell(ctx, p, &link);
		xt = p + ZF_FLAG_LEN(lenflags);
		dict_get_cell(ctx, xt, &op2);

		if(((lenflags & ZF_FLAG_PRIM) && addr == (zf_addr)op2) || addr == w || addr == xt) {
			int l = ZF_FLAG_LEN(lenflags);
			dict_get_bytes(ctx, p, name, l);
			name[l] = '\0';
			return name;
		}

		w = link;
	}
	return "?";
}

#else
static void trace(zf_ctx *ctx, const char *fmt, ...) { (void)ctx; (void)fmt; }
static const char *op_name(zf_ctx *ctx, zf_addr addr) { (void)ctx; (void)addr; return NULL; }
#endif


void zf_abort(zf_ctx *ctx, zf_result reason)
{
	longjmp(ctx->jmpbuf, reason);
}



void zf_push(zf_ctx *ctx, zf_cell v)
{
	CHECK(ctx, DSP(ctx) < ZF_DSTACK_SIZE, ZF_ABORT_DSTACK_OVERRUN);
	trace(ctx, ">" ZF_CELL_FMT " ", v);
	ctx->dstack[DSP(ctx)++] = v;
}


zf_cell zf_pop(zf_ctx *ctx)
{
	zf_cell v;
	CHECK(ctx, DSP(ctx) > 0, ZF_ABORT_DSTACK_UNDERRUN);
	CHECK(ctx, DSP(ctx) <= ZF_DSTACK_SIZE, ZF_ABORT_DSTACK_OVERRUN);
	v = ctx->dstack[--DSP(ctx)];
	trace(ctx, "<" ZF_CELL_FMT " ", v);
	return v;
}


zf_cell zf_pick(zf_ctx *ctx, zf_addr n)
{
	CHECK(ctx, n < DSP(ctx), ZF_ABORT_DSTACK_UNDERRUN);
	CHECK(ctx, DSP(ctx) <= ZF_DSTACK_SIZE, ZF_ABORT_DSTACK_OVERRUN);
	return ctx->dstack[DSP(ctx)-n-1];
}


static void zf_pushr(zf_ctx *ctx, zf_cell v)
{
	CHECK(ctx, RSP(ctx) < ZF_RSTACK_SIZE, ZF_ABORT_RSTACK_OVERRUN);
	trace(ctx, "r>" ZF_CELL_FMT " ", v);
	ctx->rstack[RSP(ctx)++] = v;
}


static zf_cell zf_popr(zf_ctx *ctx)
{
	zf_cell v;
	CHECK(ctx, RSP(ctx) > 0, ZF_ABORT_RSTACK_UNDERRUN);
	CHECK(ctx, RSP(ctx) <= ZF_RSTACK_SIZE, ZF_ABORT_RSTACK_OVERRUN);
	v = ctx->rstack[--RSP(ctx)];
	trace(ctx, "r<" ZF_CELL_FMT " ", v);
	return v;
}

zf_cell zf_pickr(zf_ctx *ctx, zf_addr n)
{
	CHECK(ctx, n < RSP(ctx), ZF_ABORT_RSTACK_UNDERRUN);
	CHECK(ctx, RSP(ctx) <= ZF_RSTACK_SIZE, ZF_ABORT_RSTACK_OVERRUN);
	return ctx->rstack[RSP(ctx)-n-1];
}



static zf_addr dict_put_bytes(zf_ctx *ctx, zf_addr addr, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t i = len;
	CHECK(ctx, addr < ZF_DICT_SIZE-len, ZF_ABORT_OUTSIDE_MEM);
	while(i--) ctx->dict[addr++] = *p++;
	return len;
}


static void dict_get_bytes(zf_ctx *ctx, zf_addr addr, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;
	CHECK(ctx, addr < ZF_DICT_SIZE-len, ZF_ABORT_OUTSIDE_MEM);
	while(len--) *p++ = ctx->dict[addr++];
}


#if ZF_ENABLE_TYPED_MEM_ACCESS
#define GET(s, t) if(size == s) { t v ## t; dict_get_bytes(ctx, addr, &v ## t, sizeof(t)); *v = v ## t; return sizeof(t); };
#define PUT(s, t, val) if(size == s) { t v ## t = val; return dict_put_bytes(ctx, addr, &v ## t, sizeof(t)); }
#else
#define GET(s, t)
#define PUT(s, t, val)
#endif

static zf_addr dict_put_cell_typed(zf_ctx *ctx, zf_addr addr, zf_cell v, zf_mem_size size)
{
	unsigned int vi = v;
	uint8_t t[2];

	trace(ctx, "\n+" ZF_ADDR_FMT " " ZF_ADDR_FMT, addr, (zf_addr)v);

	if(size == ZF_MEM_SIZE_VAR) {
		if((v - vi) == 0) {
			if(vi < 128) {
				trace(ctx, " 1");
				t[0] = vi;
				return dict_put_bytes(ctx, addr, t, 1);
			}
			if(vi < 16384) {
				trace(ctx, " 2");
				t[0] = (vi >> 8) | 0x80;
				t[1] = vi;
				return dict_put_bytes(ctx, addr, t, sizeof(t));
			}
		}
	}

	if(size == ZF_MEM_SIZE_VAR || size == ZF_MEM_SIZE_VAR_MAX) {
		trace(ctx, " 5");
		t[0] = 0xff;
		return dict_put_bytes(ctx, addr+0, t, 1) +
		       dict_put_bytes(ctx, addr+1, &v, sizeof(v));
	}

	PUT(ZF_MEM_SIZE_CELL, zf_cell, v);
	PUT(ZF_MEM_SIZE_U8, uint8_t, vi);
	PUT(ZF_MEM_SIZE_U16, uint16_t, vi);
	PUT(ZF_MEM_SIZE_U32, uint32_t, vi);
	PUT(ZF_MEM_SIZE_S8, int8_t, vi);
	PUT(ZF_MEM_SIZE_S16, int16_t, vi);
	PUT(ZF_MEM_SIZE_S32, int32_t, vi);

	zf_abort(ctx, ZF_ABORT_INVALID_SIZE);
	return 0;
}


static zf_addr dict_get_cell_typed(zf_ctx *ctx, zf_addr addr, zf_cell *v, zf_mem_size size)
{
	uint8_t t[2];
	dict_get_bytes(ctx, addr, t, sizeof(t));

	if(size == ZF_MEM_SIZE_VAR) {
		if(t[0] & 0x80) {
			if(t[0] == 0xff) {
				dict_get_bytes(ctx, addr+1, v, sizeof(*v));
				return 1 + sizeof(*v);
			} else {
				*v = ((t[0] & 0x3f) << 8) + t[1];
				return 2;
			}
		} else {
			*v = t[0];
			return 1;
		}
	}

	GET(ZF_MEM_SIZE_CELL, zf_cell);
	GET(ZF_MEM_SIZE_U8, uint8_t);
	GET(ZF_MEM_SIZE_U16, uint16_t);
	GET(ZF_MEM_SIZE_U32, uint32_t);
	GET(ZF_MEM_SIZE_S8, int8_t);
	GET(ZF_MEM_SIZE_S16, int16_t);
	GET(ZF_MEM_SIZE_S32, int32_t);

	zf_abort(ctx, ZF_ABORT_INVALID_SIZE);
	return 0;
}


static zf_addr dict_put_cell(zf_ctx *ctx, zf_addr addr, zf_cell v)
{
	return dict_put_cell_typed(ctx, addr, v, ZF_MEM_SIZE_VAR);
}


static zf_addr dict_get_cell(zf_ctx *ctx, zf_addr addr, zf_cell *v)
{
	return dict_get_cell_typed(ctx, addr, v, ZF_MEM_SIZE_VAR);
}


static void dict_add_cell_typed(zf_ctx *ctx, zf_cell v, zf_mem_size size)
{
	HERE(ctx) += dict_put_cell_typed(ctx, HERE(ctx), v, size);
	trace(ctx, " ");
}


static void dict_add_cell(zf_ctx *ctx, zf_cell v)
{
	dict_add_cell_typed(ctx, v, ZF_MEM_SIZE_VAR);
}


static void dict_add_op(zf_ctx *ctx, zf_addr op)
{
	dict_add_cell(ctx, op);
	trace(ctx, "+%s ", op_name(ctx, op));
}


static void dict_add_lit(zf_ctx *ctx, zf_cell v)
{
	dict_add_op(ctx, PRIM_LIT);
	dict_add_cell(ctx, v);
}


static void dict_add_str(zf_ctx *ctx, const char *s)
{
	size_t l;
	trace(ctx, "\n+" ZF_ADDR_FMT " " ZF_ADDR_FMT " s '%s'", HERE(ctx), (zf_addr)0, s);
	l = strlen(s);
	HERE(ctx) += dict_put_bytes(ctx, HERE(ctx), s, l);
}


static void create(zf_ctx *ctx, const char *name, int flags)
{
	zf_addr here_prev;
	trace(ctx, "\n=== create '%s'", name);
	here_prev = HERE(ctx);
	dict_add_cell(ctx, (strlen(name)) | flags);
	dict_add_cell(ctx, LATEST(ctx));
	dict_add_str(ctx, name);
	LATEST(ctx) = here_prev;
	trace(ctx, "\n===");
}


static int find_word(zf_ctx *ctx, const char *name, zf_addr *word, zf_addr *code)
{
	zf_addr w = LATEST(ctx);
	size_t namelen = strlen(name);

	while(w) {
		zf_cell link, d;
		zf_addr p = w;
		size_t len;
		p += dict_get_cell(ctx, p, &d);
		p += dict_get_cell(ctx, p, &link);
		len = ZF_FLAG_LEN((int)d);
		if(len == namelen) {
			const char *name2 = (const char *)&ctx->dict[p];
			if(memcmp(name, name2, len) == 0) {
				*word = w;
				*code = p + len;
				return 1;
			}
		}
		w = link;
	}

	return 0;
}


static void make_immediate(zf_ctx *ctx)
{
	zf_cell lenflags;
	dict_get_cell(ctx, LATEST(ctx), &lenflags);
	dict_put_cell(ctx, LATEST(ctx), (int)lenflags | ZF_FLAG_IMMEDIATE);
}


static void run(zf_ctx *ctx, const char *input)
{
	while(ctx->ip != 0) {
		zf_cell d;
		zf_addr i, ip_org = ctx->ip;
		zf_addr l = dict_get_cell(ctx, ctx->ip, &d);
		zf_addr code = d;

		trace(ctx, "\n " ZF_ADDR_FMT " " ZF_ADDR_FMT " ", ctx->ip, code);
		for(i=0; i<RSP(ctx); i++) trace(ctx, "  ");

		ctx->ip += l;

		if(code < PRIM_COUNT) {
			do_prim(ctx, (zf_prim)code, input);

			if(ctx->input_state != ZF_INPUT_INTERPRET) {
				ctx->ip = ip_org;
				break;
			}

		} else {
			trace(ctx, "%s/" ZF_ADDR_FMT " ", op_name(ctx, code), code);
			zf_pushr(ctx, ctx->ip);
			ctx->ip = code;
		}

		input = NULL;
	}
}


static void execute(zf_ctx *ctx, zf_addr addr)
{
	ctx->ip = addr;
	RSP(ctx) = 0;
	zf_pushr(ctx, 0);

	trace(ctx, "\n[%s/" ZF_ADDR_FMT "] ", op_name(ctx, ctx->ip), ctx->ip);
	run(ctx, NULL);

}


static zf_addr peek(zf_ctx *ctx, zf_addr addr, zf_cell *val, zf_mem_size size)
{
	if(addr < ZF_USERVAR_COUNT) {
		*val = ctx->uservar[addr];
		return 1;
	} else {
		return dict_get_cell_typed(ctx, addr, val, size);
	}

}


static void do_prim(zf_ctx *ctx, zf_prim op, const char *input)
{
	zf_cell d1, d2, d3;
	zf_addr addr, code;
	zf_mem_size size;

	trace(ctx, "(%s) ", op_name(ctx, op));

	switch(op) {

		case PRIM_COL:
			if(input == NULL) {
				ctx->input_state = ZF_INPUT_PASS_WORD;
			} else {
				create(ctx, input, 0);
				COMPILING(ctx) = 1;
			}
			break;

		case PRIM_LTZ:
			zf_push(ctx, zf_pop(ctx) < 0 ? ZF_TRUE : ZF_FALSE);
			break;

		case PRIM_SEMICOL:
			dict_add_op(ctx, PRIM_EXIT);
			trace(ctx, "\n===");
			COMPILING(ctx) = 0;
			break;

		case PRIM_LITERAL:
			if(COMPILING(ctx)) dict_add_lit(ctx, zf_pop(ctx));
			break;

		case PRIM_LIT:
			ctx->ip += dict_get_cell(ctx, ctx->ip, &d1);
			zf_push(ctx, d1);
			break;

		case PRIM_EXIT:
			ctx->ip = zf_popr(ctx);
			break;

		case PRIM_LEN:
			size = zf_pop(ctx);
			addr = zf_pop(ctx);
			zf_push(ctx, peek(ctx, addr, &d1, size));
			break;

		case PRIM_PEEK:
			size = zf_pop(ctx);
			addr = zf_pop(ctx);
			peek(ctx, addr, &d1, size);
			zf_push(ctx, d1);
			break;

		case PRIM_POKE:
			size = zf_pop(ctx);
			addr = zf_pop(ctx);
			d1 = zf_pop(ctx);
			if(addr < ZF_USERVAR_COUNT) {
				ctx->uservar[addr] = d1;
			} else {
				dict_put_cell_typed(ctx, addr, d1, size);
			}
			break;

		case PRIM_SWAP:
			d1 = zf_pop(ctx); d2 = zf_pop(ctx);
			zf_push(ctx, d1); zf_push(ctx, d2);
			break;

		case PRIM_ROT:
			d1 = zf_pop(ctx); d2 = zf_pop(ctx); d3 = zf_pop(ctx);
			zf_push(ctx, d2); zf_push(ctx, d1); zf_push(ctx, d3);
			break;

		case PRIM_DROP:
			zf_pop(ctx);
			break;

		case PRIM_DUP:
			d1 = zf_pop(ctx);
			zf_push(ctx, d1); zf_push(ctx, d1);
			break;

		case PRIM_ADD:
			d1 = zf_pop(ctx); d2 = zf_pop(ctx);
			zf_push(ctx, d1 + d2);
			break;

		case PRIM_SYS:
			d1 = zf_pop(ctx);
			ctx->input_state = zf_host_sys(ctx, (zf_syscall_id)d1, input);
			if(ctx->input_state != ZF_INPUT_INTERPRET) {
				zf_push(ctx, d1);
			}
			break;

		case PRIM_PICK:
			addr = zf_pop(ctx);
			zf_push(ctx, zf_pick(ctx, addr));
			break;

		case PRIM_PICKR:
			addr = zf_pop(ctx);
			zf_push(ctx, zf_pickr(ctx, addr));
			break;

		case PRIM_SUB:
			d1 = zf_pop(ctx); d2 = zf_pop(ctx);
			zf_push(ctx, d2 - d1);
			break;

		case PRIM_MUL:
			zf_push(ctx, zf_pop(ctx) * zf_pop(ctx));
			break;

		case PRIM_DIV:
			if((d2 = zf_pop(ctx)) == 0) {
				zf_abort(ctx, ZF_ABORT_DIVISION_BY_ZERO);
			}
			d1 = zf_pop(ctx);
			zf_push(ctx, d1 / d2);
			break;

		case PRIM_MOD:
			if((int)(d2 = zf_pop(ctx)) == 0) {
				zf_abort(ctx, ZF_ABORT_DIVISION_BY_ZERO);
			}
			d1 = zf_pop(ctx);
			zf_push(ctx, (int)d1 % (int)d2);
			break;

		case PRIM_IMMEDIATE:
			make_immediate(ctx);
			break;

		case PRIM_JMP:
			ctx->ip += dict_get_cell(ctx, ctx->ip, &d1);
			trace(ctx, "ip " ZF_ADDR_FMT "=>" ZF_ADDR_FMT, ctx->ip, (zf_addr)d1);
			ctx->ip = d1;
			break;

		case PRIM_JMP0:
			ctx->ip += dict_get_cell(ctx, ctx->ip, &d1);
			if(zf_pop(ctx) == 0) {
				trace(ctx, "ip " ZF_ADDR_FMT "=>" ZF_ADDR_FMT, ctx->ip, (zf_addr)d1);
				ctx->ip = d1;
			}
			break;

		case PRIM_TICK:
			if (COMPILING(ctx)) {
				ctx->ip += dict_get_cell(ctx, ctx->ip, &d1);
				trace(ctx, "%s/", op_name(ctx, d1));
				zf_push(ctx, d1);
			}
			else {
				if (input) {
					if (find_word(ctx, input,&addr,&code)) zf_push(ctx, code);
					else zf_abort(ctx, ZF_ABORT_NOT_A_WORD);
				}
				else ctx->input_state = ZF_INPUT_PASS_WORD;
			}

			break;

		case PRIM_COMMA:
			size = zf_pop(ctx);
			d1 = zf_pop(ctx);
			dict_add_cell_typed(ctx, d1, size);
			break;

		case PRIM_COMMENT:
			if(!input || input[0] != ')') {
				ctx->input_state = ZF_INPUT_PASS_CHAR;
			}
			break;

		case PRIM_PUSHR:
			zf_pushr(ctx, zf_pop(ctx));
			break;

		case PRIM_POPR:
			zf_push(ctx, zf_popr(ctx));
			break;

		case PRIM_EQUAL:
			zf_push(ctx, zf_pop(ctx) == zf_pop(ctx) ? ZF_TRUE : ZF_FALSE);
			break;

		case PRIM_KEY:
			if(input == NULL) {
				ctx->input_state = ZF_INPUT_PASS_CHAR;
			} else {
				zf_push(ctx, input[0]);
			}
			break;

		case PRIM_LITS:
			ctx->ip += dict_get_cell(ctx, ctx->ip, &d1);
			zf_push(ctx, ctx->ip);
			zf_push(ctx, d1);
			ctx->ip += d1;
			break;

		case PRIM_AND:
			zf_push(ctx, (zf_int)zf_pop(ctx) & (zf_int)zf_pop(ctx));
			break;

		case PRIM_OR:
			zf_push(ctx, (zf_int)zf_pop(ctx) | (zf_int)zf_pop(ctx));
			break;

		case PRIM_XOR:
			zf_push(ctx, (zf_int)zf_pop(ctx) ^ (zf_int)zf_pop(ctx));
			break;

		case PRIM_SHL:
			d1 = zf_pop(ctx);
			zf_push(ctx, (zf_int)zf_pop(ctx) << (zf_int)d1);
			break;

		case PRIM_SHR:
			d1 = zf_pop(ctx);
			zf_push(ctx, (zf_int)zf_pop(ctx) >> (zf_int)d1);
			break;

		default:
			zf_abort(ctx, ZF_ABORT_INTERNAL_ERROR);
			break;
	}
}


static void handle_word(zf_ctx *ctx, const char *buf)
{
	zf_addr w, c = 0;
	int found;

	if(ctx->input_state == ZF_INPUT_PASS_WORD) {
		ctx->input_state = ZF_INPUT_INTERPRET;
		run(ctx, buf);
		return;
	}

	found = find_word(ctx, buf, &w, &c);

	if(found) {
		zf_cell d;
		int flags;
		dict_get_cell(ctx, w, &d);
		flags = d;

		if(COMPILING(ctx) && (POSTPONE(ctx) || !(flags & ZF_FLAG_IMMEDIATE))) {
			if(flags & ZF_FLAG_PRIM) {
				dict_get_cell(ctx, c, &d);
				dict_add_op(ctx, d);
			} else {
				dict_add_op(ctx, c);
			}
			POSTPONE(ctx) = 0;
		} else {
			execute(ctx, c);
		}
	} else {
		zf_cell v = zf_host_parse_num(ctx, buf);

		if(COMPILING(ctx)) {
			dict_add_lit(ctx, v);
		} else {
			zf_push(ctx, v);
		}
	}
}


static void handle_char(zf_ctx *ctx, char c)
{
	if(ctx->input_state == ZF_INPUT_PASS_CHAR) {

		ctx->input_state = ZF_INPUT_INTERPRET;
		run(ctx, &c);

	} else if(c != '\0' && !isspace(c)) {

		if(ctx->read_len < sizeof(ctx->read_buf)-1) {
			ctx->read_buf[ctx->read_len++] = c;
			ctx->read_buf[ctx->read_len] = '\0';
		}

	} else {

		if(ctx->read_len > 0) {
			ctx->read_len = 0;
			handle_word(ctx, ctx->read_buf);
		}
	}
}


void zf_init(zf_ctx *ctx, int enable_trace)
{
	ctx->uservar = (zf_addr *)ctx->dict;
	ctx->read_len = 0;
	HERE(ctx) = ZF_USERVAR_COUNT * sizeof(zf_addr);
	LATEST(ctx) = 0;
	TRACE(ctx) = enable_trace;
	COMPILING(ctx) = 0;
	POSTPONE(ctx) = 0;
	DSP(ctx) = 0;
	RSP(ctx) = 0;
}


#if ZF_ENABLE_BOOTSTRAP

static void add_prim(zf_ctx *ctx, const char *name, zf_prim op)
{
	int imm = 0;

	if(name[0] == '_') {
		name ++;
		imm = 1;
	}

	create(ctx, name, ZF_FLAG_PRIM);
	dict_add_op(ctx, op);
	dict_add_op(ctx, PRIM_EXIT);
	if(imm) make_immediate(ctx);
}

static void add_uservar(zf_ctx *ctx, const char *name, zf_addr addr)
{
	create(ctx, name, 0);
	dict_add_lit(ctx, addr);
	dict_add_op(ctx, PRIM_EXIT);
}


void zf_bootstrap(zf_ctx *ctx)
{
	zf_addr i = 0;
	const char *p;
	for(p=prim_names; *p; p+=strlen(p)+1) {
		add_prim(ctx, p, (zf_prim)i++);
	}

	i = 0;
	for(p=uservar_names; *p; p+=strlen(p)+1) {
		add_uservar(ctx, p, i++);
	}
}

#else
void zf_bootstrap(zf_ctx *ctx) { (void)ctx; }
#endif


zf_result zf_eval(zf_ctx *ctx, const char *buf)
{
	zf_result r = (zf_result)setjmp(ctx->jmpbuf);

	if(r == ZF_OK) {
		for(;;) {
			handle_char(ctx, *buf);
			if(*buf == '\0') {
				return ZF_OK;
			}
			buf ++;
		}
	} else {
		COMPILING(ctx) = 0;
		RSP(ctx) = 0;
		DSP(ctx) = 0;
		return r;
	}
}


void *zf_dump(zf_ctx *ctx, size_t *len)
{
	if(len) *len = sizeof(ctx->dict);
	return ctx->dict;
}

zf_result zf_uservar_set(zf_ctx *ctx, zf_uservar_id uv, zf_cell v)
{
	zf_result result = ZF_ABORT_INVALID_USERVAR;

	if (uv < ZF_USERVAR_COUNT) {
		ctx->uservar[uv] = v;
		result = ZF_OK;
	}

	return result;
}

zf_result zf_uservar_get(zf_ctx *ctx, zf_uservar_id uv, zf_cell *v)
{
	zf_result result = ZF_ABORT_INVALID_USERVAR;

	if (uv < ZF_USERVAR_COUNT) {
		if (v != NULL) {
			*v = ctx->uservar[uv];
		}
		result = ZF_OK;
	}

	return result;
}
