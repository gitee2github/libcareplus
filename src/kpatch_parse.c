/******************************************************************************
 * 2021.12.16 - kpatch_parse: enhance init_other_block() to extend function cblock to cover .init_array
 * China Telecom, <luoyi2@chinatelecom.cn>
 * 
 * 2021.12.13 - kpatch_parse: adjust the judgment for the end of function cblock in init_func_block()
 * China Telecom, <luoyi2@chinatelecom.cn>
 * 
 * 2021.10.11 - kpatch: fix code checker warning
 * Huawei Technologies Co., Ltd. <zhengchuan@huawei.com>
 *
 * 2021.10.08 - kpatch_parse: fix wrong return in is_data_sec()
 * Huawei Technologies Co., Ltd. <zhengchuan@huawei.com>
 *
 * 2021.10.08 - enhance kpatch_gensrc and kpatch_elf and kpatch_cc code
 * Huawei Technologies Co., Ltd. <zhengchuan@huawei.com>
 *
 * 2021.10.08 - kpatch_parse: fix possible Null pointer dereferences
 * Huawei Technologies Co., Ltd. <jinyan12@huawei.com>
******************************************************************************/

#include <stdlib.h>

#include "include/kpatch_log.h"
#include "include/kpatch_parse.h"
#include "include/kpatch_flags.h"

char *cline(struct kp_file *f, int l)
{
	if (l < 0 || l >= f->nr_lines)
		return NULL;

	return f->lines[l];
}

int clinenum(struct kp_file *f, int l)
{
	if (l < 0 || l >= f->nr_lines)
		return 0;

	return f->lines_num[l];
}

void __get_token(char **str, kpstr_t *x, const char *delim)
{
	char *s = *str, *e;

	kpstrset(x, "", 0);
	if (!s)
		return;

	s = skip_blanks(s);
	if (!*s || *s == '\n') {
		*str = NULL;
		return;
	}

	if (*s == '"') {
		for (e = s + 1; *e && *e != '"'; e++)
			if (e[0] == '\\' && e[1]) e++;	/* skip all escaping sequences, including \" */
		if (*e == '"') e++;
	} else
		for (e = s; *e && !strchr(delim, *e); e++) ;

	if (e == s)
		e++;

	kpstrset(x, s, e - s);

	e = skip_blanks(e);
	if (!*e || *e == '\n')
		e = NULL;
	*str = e;
}

void get_token(char **str, kpstr_t *x)
{
	const char *delim = " \t,;:-+*()[]$\n";
	__get_token(str, x, delim);
}

/* remove .cold in function name */
void remove_cold_suffix(kpstr_t *nm)
{
	if(!nm->s)
		return;

	char *suffix_loc = strstr(nm->s, ".cold");
	if(suffix_loc)
		nm->l = suffix_loc - nm->s;			/* remove .cold */
}

/* ------------------------------  as directives parsing ---------------------------------- */

static struct {
	int type;
	char *s;
} asm_directives[] = {
	{ DIRECTIVE_ALIGN, ".align"},
	{ DIRECTIVE_ALIGN, ".p2align"},

	{ DIRECTIVE_TYPE, ".type"},
	{ DIRECTIVE_COMM, ".comm"},
	{ DIRECTIVE_SIZE, ".size"},
	{ DIRECTIVE_WEAK, ".weak"},

	{ DIRECTIVE_TEXT, ".text"},
	{ DIRECTIVE_DATA, ".data"},
	{ DIRECTIVE_BSS, ".bss"},
	{ DIRECTIVE_SECTION, ".section"},
	{ DIRECTIVE_PUSHSECTION, ".pushsection"},
	{ DIRECTIVE_POPSECTION, ".popsection"},
	{ DIRECTIVE_PREVIOUS, ".previous"},
	{ DIRECTIVE_SUBSECTION, ".subsection"},

	{ DIRECTIVE_GLOBL, ".globl"},
	{ DIRECTIVE_GLOBL, ".global"},

	{ DIRECTIVE_LOCAL, ".local"},
	{ DIRECTIVE_HIDDEN, ".hidden"},
	{ DIRECTIVE_PROTECTED, ".protected"},
	{ DIRECTIVE_INTERNAL, ".internal"},
	{ DIRECTIVE_SET, ".set"},
	{ DIRECTIVE_KPFLAGS, ".kpgensrc_flags" },
};

/* parse arguments of .type command */
void get_type_args(char *s, kpstr_t *nm, kpstr_t *attr)
{
	kpstr_t t, t2;

	get_token(&s, &t);	/* skip command */
	if (kpstrcmpz(&t, ".type"))
		return;

	get_token(&s, nm);	/* get name */
	get_token(&s, &t2);	/* skip ',' */
	get_token(&s, attr);	/* get attr */

	if (kpstrcmpz(&t2, ","))
		kpfatal("can't parse .type command");
}

int find_ctype(kpstr_t *t)
{
	int i;
	for (i = 0; i < (int)(sizeof(asm_directives)/sizeof(asm_directives[0])); i++) {
		if (!kpstrcmpz(t, asm_directives[i].s))
			return asm_directives[i].type;
	}
	return -1;
}

int ctype(struct kp_file *f, int l)
{
	if (l >= f->nr_lines)
		kpfatal("ctype access beyond EOF");

	return f->ctype[l];
}

int is_sect_cmd(struct kp_file *f, int l)
{
	int t = ctype(f, l);

	return t == DIRECTIVE_TEXT || t == DIRECTIVE_DATA || t == DIRECTIVE_BSS || t == DIRECTIVE_SECTION ||
		t == DIRECTIVE_PUSHSECTION || t == DIRECTIVE_POPSECTION ||
		t == DIRECTIVE_PREVIOUS || t == DIRECTIVE_SUBSECTION;
}

void init_ctypes(struct kp_file *f)
{
	int i;

	f->ctype = malloc(f->nr_lines * sizeof(f->ctype[0]));
	if (!f->ctype) {
		kpfatal("Failed to allocate ctype for kp_file\n");
	}

	for (i = 0; i < f->nr_lines; i++) {
		f->ctype[i] = parse_ctype(cline(f, i), true);
	}
}

/* ------------------------------------------- code blocks parsing --------------------------------------------- */

static inline int cblock_name_cmp(struct rb_node *node, unsigned long key)
{
	struct cblock *blk = rb_entry(node, struct cblock, rbnm);
	kpstr_t *nm = (kpstr_t *)key;
	int res;

	res = kpstrcmp(&blk->name, nm);
	return res;
}

static inline int cblock_human_name_cmp(struct rb_node *node, unsigned long key)
{
	struct cblock *blk = rb_entry(node, struct cblock, rb_hnm);
	kpstr_t *nm = (kpstr_t *)key;
	int res;

	res = kpstrcmp(&blk->human_name, nm);
	return res;
}

static inline int cblock_start_cmp(struct rb_node *node, unsigned long key)
{
	struct cblock *blk = rb_entry(node, struct cblock, rbs);

	return blk->start == key ? 0 : (blk->start > key ? -1 : 1);
}

static void cblock_make_human_name(kpstr_t *hnm, kpstr_t *nm)
{
	kpstr_t subnm;
	char *s = nm->s;

	*hnm = *nm;

	/*
	 * Latests gcc versions (RHEL7) generate lots of function clones optimized for some specific cases (e.g. for constanct propogation).
	 * These functions are typically called as func.part.NUM or func.isra.NUM.constprop.NUM, so we cut all the gcc specific suffixes
	 * to get real human name of the function. This is used later for matching functions by name.
	 *
	 * Same applies to static variables. They have a suffix like "var.NUM", so we need to make sure we compare same kind of variables later.
	 */
	__get_token(&s, &subnm, "\t\n.,");
	if (s && isdigit(nm->s[nm->l - 1])) {
		*hnm = subnm;	/* return whatever is before "." */
		return;
	}

	/* __mod_XXX variables are nasty as Linux kernel adds __LINE__ suffix to it in __MODULE_INFO() macro. For proper matching need to handle this as well. */
	if (!kpstrncmpz(nm, "__mod_") && isdigit(nm->s[nm->l - 1]))
		while (isdigit(hnm->s[hnm->l - 1]))
			hnm->l--;
}

struct cblock * cblock_add(struct kp_file *f, int s, int e, kpstr_t *nm, int type, int globl)
{
	struct cblock *blk = malloc(sizeof(*blk));
	if (!blk) {
		kpfatal("Failed to allocate cblock\n");
	}

	if (nm->l && cblock_find_by_name(f, nm))
		kpfatal("duplicate cblock name '%.*s'", nm->l, nm->s);

	blk->start = s;
	blk->end = e;
	blk->f = f;
	blk->name = *nm;
	cblock_make_human_name(&blk->human_name, &blk->name);
	blk->auto_name = !!kpstrcmp(&blk->human_name, &blk->name);
	blk->type = type;
	blk->globl = globl;
	blk->handled = blk->ignore = blk->unlink = 0;
	blk->pair = NULL;
	rb_insert_node(&f->cblocks_by_name, &blk->rbnm, cblock_name_cmp, (unsigned long)&blk->name);
	rb_insert_node(&f->cblocks_by_human_name, &blk->rb_hnm, cblock_human_name_cmp, (unsigned long)&blk->human_name);
	rb_insert_node(&f->cblocks_by_start, &blk->rbs, cblock_start_cmp, s);
	kplog(LOG_DEBUG, "Add cblock %.*s (%d: %d-%d)\n", nm->l, nm->s, f->id, s, e - 1);

	return blk;
}

struct cblock *cblock_find_by_name(struct kp_file *f, kpstr_t *nm)
{
	struct rb_node *rb;
	struct cblock *blk;

	rb = rb_search_node(&f->cblocks_by_name, cblock_name_cmp, (unsigned long)nm);
	if (rb == NULL)
		return NULL;

	blk = rb_entry(rb, struct cblock, rbnm);
	return blk;
}

struct cblock *cblock_find_by_human_name(struct kp_file *f, kpstr_t *nm)
{
	struct rb_node *n, *n2;
	struct cblock *blk;

	n = rb_search_node(&f->cblocks_by_human_name, cblock_human_name_cmp, (unsigned long)nm);
	if (!n)
		return NULL;

	/* find the most-left tree node - we may have collisions, i.e. entries with same name and lookup returns arbitrary one */
	for (n2 = rb_prev(n); n2; n2 = rb_prev(n)) {
		blk = rb_entry(n2, struct cblock, rb_hnm);
		if (kpstrcmp(&blk->human_name, nm))
			break;
		n = n2;
	}

	blk = rb_entry(n, struct cblock, rb_hnm);
	return blk;
}

static int get_kpatch_flags(char *s)
{
	kpstr_t t;
	int flags = 0;

	get_token(&s, &t);

	while(s) {
		get_token(&s, &t);
		if (!kpstrcmpz(&t, "KPGENSRC_ADAPTED"))
			flags |= KPGENSRC_ADAPTED;
	}

	return flags;
}

static void init_func_block(struct kp_file *f, int *i, kpstr_t *nm)
{
	int s = *i, e = *i, globl = 0;
	int flags = 0;
	struct cblock *blk;

	int func_cnt = 0;

	while (e < f->nr_lines - 1) {
		if (ctype(f, e) == DIRECTIVE_GLOBL)
			globl = 1;
		if (ctype(f, e) == DIRECTIVE_KPFLAGS) {
			flags |= get_kpatch_flags(cline(f, e));
			cline(f, e)[0] = 0;
		}

		/* if compiling is optimized by -freorder-functions, e.g funcA, it will contains like ".type funcA.cold.xxx,@function" inside funcA, 
		and the end of funcA is not the first size directive matched with funcA. At present, use count for "@function" to judge*/
		recog_func_attr(f, e, nm, &func_cnt);

		if(is_function_end(f, e, nm)) {
			--func_cnt;
			if(!func_cnt)
				break;
		}

		e++;
	}

	e++;

	blk = cblock_add(f, s, e, nm, CBLOCK_FUNC, globl);
	if (flags & KPGENSRC_ADAPTED)
		blk->adapted = 1;
	*i = e;
}

static void init_var_block(struct kp_file *f, int *i, kpstr_t *nm)
{
	int s = *i, e = *i, e2, globl = 0;
	kpstr_t nm2;

	while (e < f->nr_lines) {
		if (is_variable_start(f, e, &e2, &globl, &nm2)) {
			if (kpstrcmp(nm, &nm2))
				break;
			e = e2;
			continue;
		}
		if (is_data_def(cline(f, e), ctype(f, e))) {
			e++;
			continue;
		}
		break;
	}
#if 0
	/* unfortunately, variables can be constructed manually in a single line, so w/o 2nd line with "body" */
	if (!has_body)
		kpfatal("Failed to find variable '%.*s' body", nm->l, nm->s);
#endif
	cblock_add(f, s, e, nm, CBLOCK_VAR, globl);
	*i = e;
}

static void init_set_block(struct kp_file *f, int *i, kpstr_t *nm)
{
	char *s = cline(f, *i);
	get_token(&s, nm);
	get_token(&s, nm);
	cblock_add(f, *i, *i + 1, nm, CBLOCK_VAR, 0);
	(*i)++;
}

/*if funcA is needed in initialization, e.g constructor in C++, the function pointer will be put into .init_array section. 
the directives will appear right after the function size directive like this,

	.size		funcA, .-funcA
	.section	.init_array,"aw"
	.align 8
	.quad	funcA

since LCOLD* or LHOT* label may appear inside, and the label may change after patched, if classified as OTHER or VAR cblock, 
label change will conflict with the corresponding matching rules. also, we cannot set a proper VAR cblock name with no violation.
it can only be treated as an extension of FUNC cblock. */

#define EXT_INIARR_FLAG 1
#define EXT_UPDATE_FLAG 2

static void init_other_block(struct kp_file *f, int *i)
{
	int s = *i, e = *i;
	int flag = 0;

	kpstr_t nm;
	kpstrset(&nm, "", 0);

	char *line = NULL;
	kpstr_t nm2;
	kpstrset(&nm2, "", 0);

	struct rb_node *node = NULL;
	struct cblock *blk = NULL;

	while (e < f->nr_lines && !(is_function_start(f, e, &nm) || is_variable_start(f, e, NULL, NULL, &nm))) {
		if(ctype(f, e) == DIRECTIVE_SECTION && !strcmp(csect(f, e)->name, ".init_array")) 
			flag = EXT_INIARR_FLAG;

		if(flag && ctype(f, e) == DIRECTIVE_OTHER) {
			line = cline(f, e);

			if (is_data_def(line, DIRECTIVE_OTHER)) {
				get_token(&line, &nm2);
				get_token(&line, &nm2);

				node = rb_last(&f->cblocks_by_start);
				if(!node) {
					++e;
					break;
				}

				blk = rb_entry(node, struct cblock, rbs);
				if(blk->type == CBLOCK_FUNC && !kpstrcmp(&blk->name, &nm2)) {
					kplog(LOG_DEBUG, "Extend cblock %.*s (%d: %d-%d) to (%d: %d-%d)\n", 
						blk->name.l, blk->name.s, f->id, blk->start, blk->end-1, f->id, blk->start, e);
					blk->end = ++e;
					flag = EXT_UPDATE_FLAG;
					break;
				}
			}
		}
		++e;
	}

	if(flag == EXT_INIARR_FLAG) {
		while (e < f->nr_lines && !(is_function_start(f, e, &nm) || is_variable_start(f, e, NULL, NULL, &nm)))
			++e;
	}

	if(flag != EXT_UPDATE_FLAG) {
		kpstrset(&nm, "", 0);
		cblock_add(f, s, e, &nm, CBLOCK_OTHER, 0);
	}

	*i = e;
}

static void init_attr_block(struct kp_file *f, int *i)
{
	kpstr_t nm;
	char *s = cline(f, *i);

	/* use the whole ".weak symbol" or ".globl symbol" as a block name to avoid intersection with real blocks */
	get_token(&s, &nm);
	kpstrset(&nm, nm.s, strlen(nm.s));
	if (!cblock_find_by_name(f, &nm))
		cblock_add(f, *i, *i + 1, &nm, CBLOCK_ATTR, 0);
	(*i)++;
}

void cblock_split(struct cblock *b, int len)
{
	struct cblock *blk = malloc(sizeof(*blk));
	if (!blk) {
		kpfatal("Failed to allocate cblock when splitting it\n");
	}

	memset(blk, 0, sizeof(*blk));
	blk->start = b->start + len;
	blk->end = b->end;
	blk->f = b->f;
	blk->name = b->name;
	blk->type = b->type;
	b->end = b->start + len;
	rb_insert_node(&blk->f->cblocks_by_name, &blk->rbnm, cblock_name_cmp, (unsigned long)&blk->name);
	rb_insert_node(&blk->f->cblocks_by_human_name, &blk->rb_hnm, cblock_human_name_cmp, (unsigned long)&blk->human_name);
	rb_insert_node(&blk->f->cblocks_by_start, &blk->rbs, cblock_start_cmp, blk->start);
	kplog(LOG_DEBUG, "Add split cblock %.*s (%d: %d-%d)\n", blk->name.l, blk->name.s, blk->f->id, blk->start, blk->end - 1);
}

void cblocks_init(struct kp_file *f)
{
	int i;
	kpstr_t nm;

	rb_init(&f->cblocks_by_name);
	rb_init(&f->cblocks_by_human_name);
	rb_init(&f->cblocks_by_start);
	kpstrset(&nm, "", 0);
	for (i = 1; i < f->nr_lines; ) {
		if (cline(f, i)[0] == 0) {
			i++;	/* skip empty lines */
			continue;
		}
		if (is_function_start(f, i, &nm))
			init_func_block(f, &i, &nm);
		else if (is_variable_start(f, i, NULL, NULL, &nm))
			init_var_block(f, &i, &nm);
		else if (ctype(f, i) == DIRECTIVE_SET)
			init_set_block(f, &i, &nm);
		else if (ctype(f, i) == DIRECTIVE_WEAK || ctype(f, i) == DIRECTIVE_GLOBL)
			/* sometimes .globl memcmp can be found in the middle of asm file... */
			init_attr_block(f, &i);
		else
			init_other_block(f, &i);
	}
}

void cblock_free_by_start(struct rb_node *node)
{
	struct cblock *cb = rb_entry(node, struct cblock, rbs);
	free(cb);
}

void cblock_print2(struct cblock *b0, struct cblock *b1)
{
	int i0, i1;

	for (i0 = b0->start, i1 = b1->start; i0 < b0->end || i1 < b1->end; i0++, i1++)
		kplog(LOG_DEBUG, "%-64s            %s\n",
				i0 < b0->end ? cline(b0->f, i0) : "",
				i1 < b1->end ? cline(b1->f, i1) : "");
}

struct cblock *cblock_first(struct kp_file *f)
{
	struct rb_node *n = rb_first(&f->cblocks_by_start);
	if (!n)
		return NULL;
	return rb_entry(n, struct cblock, rbs);
}

struct cblock *cblock_next(struct cblock *blk)
{
	struct rb_node *n = rb_next(&blk->rbs);
	if (!n)
		return NULL;
	return rb_entry(n, struct cblock, rbs);
}

struct cblock *cblock_skip(struct cblock *blk, int type)
{
	while (blk && blk->type != type)
		blk = cblock_next(blk);
	return blk;
}

/* --------------------------------------------- sections handling ----------------------------------------------- */

/* by default outname for all executable sections is .kpatch.text and .kpatch.data otherwise */
static struct section_desc predefined_sections[] = {
	{.name = ".bss"},
	{.name = ".data"},
	{.name = ".init.data"},

	{.name = ".text",		.type = SECTION_EXECUTABLE},
	{.name = ".init.text",		.type = SECTION_EXECUTABLE},
	{.name = ".exit.text",		.type = SECTION_EXECUTABLE},
	{.name = ".text.unlikely",	.type = SECTION_EXECUTABLE},
	{.name = ".text.hot",		.type = SECTION_EXECUTABLE},
	{.name = ".fixup",		.outname = ".kpatch.fixup,\"ax\",@progbits",	.type = SECTION_EXECUTABLE},

	{.name = ".modinfo"},
	{.name = "__ex_table",		.outname = ".kpatch.__ex_table,\"a\",@progbits"},
	{.name = "__bug_table",		.outname = ".kpatch.__bug_table,\"a\""},

	{.name = ".altinstructions",	.outname = ".kpatch.altinstructions,\"a\""},
	{.name = ".altinstr_replacement",.outname = ".kpatch.altinstr_replacement, \"ax\"",	.type = SECTION_EXECUTABLE},
	{.name = ".smp_locks",		.outname = ".kpatch.smp_locks,\"a\""},
	{.name = ".parainstructions",	.outname = ".kpatch.parainstructions,\"a\""},
	{.name = "__jump_table",	.outname = ".kpatch.__jump_table,\"a\""},

	{.name = ".kpatch.text",	.type = SECTION_EXECUTABLE},
	{.name = ".kpatch.init.pre",	.outname = ".kpatch.init.pre,\"aw\",@progbits"},
	{.name = ".kpatch.init",	.outname = ".kpatch.init,\"aw\",@progbits"},
	{.name = ".kpatch.init.post",	.outname = ".kpatch.init.post,\"aw\",@progbits"},
	{.name = ".kpatch.exit.pre",	.outname = ".kpatch.exit.pre,\"aw\",@progbits"},
	{.name = ".kpatch.exit",	.outname = ".kpatch.exit,\"aw\",@progbits"},
	{.name = ".kpatch.exit.post",	.outname = ".kpatch.exit.post,\"aw\",@progbits"},

	{.name = ".discard",		.outname = ".discard,\"awx\",@progbits",	.type = SECTION_EXECUTABLE},

	{.name = "__ksymtab",		.outname = ".kpatch.__ksymtab,\"a\""},
	{.name = "__ksymtab_gpl",	.outname = ".kpatch.__ksymtab,\"a\""},
	{.name = "__kcrctab",		.outname = ".kpatch.__kcrctab,\"a\""},
	{.name = "__kcrctab_gpl",	.outname = ".kpatch.__kcrctab,\"a\""},
	{.name = "__kstrtab",		.outname = ".kpatch.__kstrtab,\"a\""},

	{.name = NULL}
};
static struct rb_root sections_rbroot_byname;

static inline int section_name_cmp(struct rb_node *node, unsigned long key)
{
	struct section_desc *sect = rb_entry(node, struct section_desc, rbnm);
	char *name = (char *)key;

	return strcmp(sect->name, name);
}

struct section_desc *find_section(char *name)
{
	struct rb_node *rb;
	struct section_desc *sect;

	rb = rb_search_node(&sections_rbroot_byname, section_name_cmp, (unsigned long)name);
	if (rb == NULL)
		return NULL;

	sect = rb_entry(rb, struct section_desc, rbnm);
	return sect;
}

static struct section_desc *dup_section(struct section_desc *sect)
{
	struct section_desc *s = malloc(sizeof(*s));
	if (!s || !sect) {
		kpfatal("Failed to allocate duplicate section\n");
	}

	*s = *sect;
	s->name = strdup(sect->name);
	s->prev = NULL;
	memset(&s->rbnm, 0, sizeof(s->rbnm));
	return s;
}

struct section_desc *csect(struct kp_file *f, int l)
{
	struct section_desc *sect;

	if (l < 0 || l >= f->nr_lines)
		return NULL;

	sect = f->section[l];
	return sect;
}

int is_data_sect(struct section_desc *sect)
{
	return (sect == NULL) ? 0 : !(sect->type & SECTION_EXECUTABLE);
}

int is_code_sect(struct section_desc *sect)
{
	return (sect == NULL) ? 0 : (sect->type & SECTION_EXECUTABLE);
}

static struct section_desc *__parse_section(char *s)
{
	struct section_desc *sect;
	char *sname;
	kpstr_t nm;
	const char *delim = " \t,\n;";	/* section names like .note.GNU-stack can have '-' in names, which is not appropriate in normal tokens */

	get_token(&s, &nm);		/* skip section directive */
	__get_token(&s, &nm, delim);	/* section name */

	/* skip quotes around name */
	if (nm.s[0] == '"' && nm.s[nm.l - 1] == '"') {
		nm.s++;
		nm.l -= 2;
	}
	sname = strndup(nm.s, nm.l);
	sect = find_section(sname);
	if (sect) {
		free(sname);
		return dup_section(sect);
	}

	sect = malloc(sizeof(*sect));
	if (!sect) {
		kpfatal("Failed to allocate section\n");
	}

	memset(sect, 0, sizeof(*sect));
	sect->name = sname;

	if (s) {
		get_token(&s, &nm);	/* skip ',' */
		if (kpstrcmpz(&nm, ","))
			goto done;
		get_token(&s, &nm);	/* attr */
		if (nm.s[0] == '"') {
			char *cp = strpbrk(&nm.s[1], "x\"");
			if (cp != NULL && *cp == 'x')
				sect->type |= SECTION_EXECUTABLE;
		}
	}
done:
	rb_insert_node(&sections_rbroot_byname, &sect->rbnm, section_name_cmp, (unsigned long)sname);
	return sect;
}

static struct section_desc *parse_section(struct kp_file *f, int l)
{
	int t;
	struct section_desc *cur = f->section[l - 1], *new;

	t = ctype(f, l);
	switch (t) {
		case DIRECTIVE_TEXT: return dup_section(find_section(".text"));
		case DIRECTIVE_DATA: return dup_section(find_section(".data"));
		case DIRECTIVE_BSS: return dup_section(find_section(".bss"));
		case DIRECTIVE_PUSHSECTION:
		case DIRECTIVE_SECTION:
			return __parse_section(cline(f, l));
		case DIRECTIVE_POPSECTION:
		case DIRECTIVE_PREVIOUS:
			return dup_section(cur->prev);
		case DIRECTIVE_SUBSECTION:
			new = dup_section(cur);
			new->prev = cur;
			return new;
	}
	return NULL;
}

void init_sections(struct kp_file *f)
{
	struct section_desc *sect;
	int i;

	if (rb_empty(&sections_rbroot_byname))
		for (i = 0; predefined_sections[i].name; i++)
			rb_insert_node(&sections_rbroot_byname, &predefined_sections[i].rbnm, section_name_cmp, (unsigned long)predefined_sections[i].name);

	f->section = malloc(f->nr_lines * sizeof(void *));
	if (!f->section) {
		kpfatal("Failed to allocate section for kp_file\n");
	}

	f->section[0] = find_section(".text");		/* code can start w/o sectiong directive */
	for (i = 1; i < f->nr_lines; i++) {
		sect = parse_section(f, i);
		if (sect) {
			sect->prev = f->section[i - 1];
			f->section[i] = sect;
		} else {
			f->section[i] = f->section[i - 1];
		}
	}
}

void free_sections(struct kp_file *f)
{
	struct section_desc **secs = NULL;
	int i;

	if (!f || !f->section) {
		return;
	}
	secs = (struct section_desc **)f->section;
	for (i = 1; i < f->nr_lines; ++i) {
		if (secs[i] != secs[i - 1]) {
			free(secs[i]->name);
			free(secs[i]);
		}
	}
	free(secs);
}

/* ----------------------------------------- code block boundaries detection ---------------------------------------- */
int is_function_end(struct kp_file *f, int l, kpstr_t *nm)
{
	/* Functions should always end by .size directive. Previously used to detect .LFe labels, but they are not generated w/o frame pointers */
	if (ctype(f, l) != DIRECTIVE_SIZE)
		return 0;

	kpstr_t nm2;
	char *s = cline(f, l);
	get_token(&s, &nm2);	/* skip command */
	get_token(&s, &nm2);

	if(nm2.l > nm->l)
		remove_cold_suffix(&nm2);   /* remove .cold */

	if (kpstrcmp(nm, &nm2)) /* verify name matches */
		return 0;

	return 1;
}


